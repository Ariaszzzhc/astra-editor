#include <astra/script/runtime.hpp>

#include <quickjs.h>

#include <chrono>
#include <thread>
#include <utility>

namespace astra::script {
namespace {

class Value {
public:
    Value(JSContext* context, JSValue value) : context_(context), value_(value) {}
    ~Value() { JS_FreeValue(context_, value_); }
    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;
    [[nodiscard]] JSValue get() const { return value_; }
private:
    JSContext* context_;
    JSValue value_;
};

std::string text(JSContext* context, JSValueConst value) {
    std::size_t size{};
    const char* bytes = JS_ToCStringLen(context, &size, value);
    if (!bytes) { throw ScriptError("Unable to convert JavaScript value to text"); }
    try {
        std::string result(bytes, size);
        JS_FreeCString(context, bytes);
        return result;
    } catch (...) {
        JS_FreeCString(context, bytes);
        throw;
    }
}

} // namespace

struct Runtime::Impl {
    Request request;
    Limits limits;
    std::thread::id owner = std::this_thread::get_id();
    JSRuntime* runtime{};
    JSContext* context{};
    std::chrono::steady_clock::time_point deadline{};
    bool active{};
    bool poisoned{};
    std::map<std::string, JSValue> callbacks;
    Json configuration = Json::object();
    std::map<std::string, std::string> bindings;

    Impl(Request incoming, Limits budget) : request(std::move(incoming)), limits(budget) {
        if (!request || limits.memory_bytes < 1024 * 1024 || limits.stack_bytes < 64 * 1024 ||
            limits.execution_time.count() <= 0 || limits.max_pending_jobs == 0) {
            throw std::invalid_argument("Invalid script runtime configuration");
        }
        runtime = JS_NewRuntime();
        if (!runtime) { throw ScriptError("Unable to create JavaScript runtime"); }
        JS_SetMemoryLimit(runtime, limits.memory_bytes);
        JS_SetMaxStackSize(runtime, limits.stack_bytes);
        JS_SetInterruptHandler(runtime, interrupt, this);
        context = JS_NewContext(runtime);
        if (!context) {
            JS_FreeRuntime(runtime);
            runtime = nullptr;
            throw ScriptError("Unable to create JavaScript context");
        }
        JS_SetContextOpaque(context, this);
        try {
            Value api(context, JS_NewObject(context));
            check(api.get());
            define(api.get(), "request", rpc_request, 2);
            define(api.get(), "registerCommand", register_command, 2);
            define(api.get(), "configure", configure, 1);
            define(api.get(), "settings", current_settings, 0);
            define(api.get(), "bind", bind, 2);
            Value global(context, JS_GetGlobalObject(context));
            if (JS_SetPropertyStr(context, global.get(), "astra", JS_DupValue(context, api.get())) < 0) {
                throw_error();
            }
        } catch (...) {
            JS_FreeContext(context);
            JS_FreeRuntime(runtime);
            throw;
        }
    }

    ~Impl() {
        for (auto& [name, callback] : callbacks) { JS_FreeValue(context, callback); }
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
    }

    void require_owner() const {
        if (owner != std::this_thread::get_id()) {
            throw std::logic_error("JavaScript runtime accessed from a different thread");
        }
    }

    struct Scope {
        Impl& host;
        explicit Scope(Impl& host_in) : host(host_in) {
            host.require_owner();
            if (host.poisoned) { throw ScriptError("Script job queue exhausted its budget; reload scripts to recover"); }
            if (host.active) { throw ScriptError("Reentrant JavaScript execution is unsupported"); }
            host.deadline = std::chrono::steady_clock::now() + host.limits.execution_time;
            host.active = true;
        }
        ~Scope() { host.active = false; }
    };

    static int interrupt(JSRuntime*, void* opaque) {
        const auto& host = *static_cast<Impl*>(opaque);
        return host.active && std::chrono::steady_clock::now() >= host.deadline ? 1 : 0;
    }

    [[noreturn]] void throw_error() {
        Value exception(context, JS_GetException(context));
        auto message = text(context, exception.get());
        if (JS_IsError(exception.get())) {
            Value stack(context, JS_GetPropertyStr(context, exception.get(), "stack"));
            if (JS_IsString(stack.get())) { message += "\n" + text(context, stack.get()); }
        }
        throw ScriptError(message);
    }

    void check(JSValueConst value) {
        if (JS_IsException(value)) { throw_error(); }
    }

    void define(JSValueConst object, const char* name, JSCFunction* function, int arity) {
        const auto value = JS_NewCFunction(context, function, name, arity);
        check(value);
        if (JS_SetPropertyStr(context, object, name, value) < 0) { throw_error(); }
    }

    Json to_json(JSValueConst value) {
        if (JS_IsUndefined(value)) { return nullptr; }
        Value serialized(context, JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED));
        check(serialized.get());
        if (JS_IsUndefined(serialized.get())) { throw ScriptError("Value is not JSON serializable"); }
        const auto bytes = text(context, serialized.get());
        if (bytes.size() > protocol::Dispatcher::max_body_bytes) {
            throw ScriptError("JavaScript result exceeds the JSON byte limit");
        }
        return Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > static_cast<int>(protocol::Dispatcher::max_depth)) {
                throw ScriptError("JavaScript result exceeds the JSON nesting limit");
            }
            return true;
        });
    }

    JSValue from_json(const Json& value) {
        const auto serialized = value.dump();
        return JS_ParseJSON(context, serialized.data(), serialized.size(), "<native-result>");
    }

    void run_jobs() {
        std::size_t count{};
        while (JS_IsJobPending(runtime)) {
            if (++count > limits.max_pending_jobs || interrupt(runtime, this)) {
                // QuickJS has no API to discard pending jobs individually. Leave
                // this runtime disabled until its owner rebuilds it on reload.
                poisoned = true;
                throw ScriptError("JavaScript pending job budget exceeded");
            }
            JSContext* job_context{};
            if (JS_ExecutePendingJob(runtime, &job_context) < 0) { throw_error(); }
        }
    }

    template <typename Function>
    static JSValue guarded(JSContext* context, Function&& function) noexcept {
        try { return function(*static_cast<Impl*>(JS_GetContextOpaque(context))); }
        catch (const std::exception& error) { return JS_ThrowInternalError(context, "%s", error.what()); }
        catch (...) { return JS_ThrowInternalError(context, "Native script callback failed"); }
    }

    static JSValue rpc_request(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(context, "request requires a method string");
            }
            const auto method = text(context, argv[0]);
            const auto params = argc > 1 ? host.to_json(argv[1]) : Json::object();
            return host.from_json(host.request(method, params));
        });
    }

    static JSValue register_command(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsFunction(context, argv[1])) {
                return JS_ThrowTypeError(context, "registerCommand requires a name and function");
            }
            const auto name = text(context, argv[0]);
            if (name.empty() || name.size() > 128) { return JS_ThrowRangeError(context, "Invalid command name"); }
            const auto existing = host.callbacks.find(name);
            if (existing != host.callbacks.end()) {
                JS_FreeValue(context, existing->second);
                existing->second = JS_DupValue(context, argv[1]);
            } else {
                const auto [entry, inserted] = host.callbacks.emplace(name, JS_UNDEFINED);
                (void)inserted;
                entry->second = JS_DupValue(context, argv[1]);
            }
            return JS_UNDEFINED;
        });
    }

    static JSValue current_settings(JSContext* context, JSValueConst, int, JSValueConst*) {
        return guarded(context, [](Impl& host) { return host.from_json(host.configuration); });
    }

    static JSValue configure(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc != 1) { return JS_ThrowTypeError(context, "configure requires one object"); }
            auto configuration = host.to_json(argv[0]);
            if (!configuration.is_object()) { return JS_ThrowTypeError(context, "Configuration must be an object"); }
            host.configuration.merge_patch(configuration);
            return JS_UNDEFINED;
        });
    }

    static JSValue bind(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
                return JS_ThrowTypeError(context, "bind requires a key and command name");
            }
            const auto key = text(context, argv[0]);
            const auto command = text(context, argv[1]);
            if (key.empty() || key.size() > 128 || command.empty() || command.size() > 128) {
                return JS_ThrowRangeError(context, "Invalid key binding");
            }
            host.bindings.insert_or_assign(key, command);
            return JS_UNDEFINED;
        });
    }
};

Runtime::Runtime(Request request) : Runtime(std::move(request), Limits{}) {}
Runtime::Runtime(Request request, Limits limits) : impl_(std::make_unique<Impl>(std::move(request), limits)) {}
Runtime::~Runtime() = default;

Runtime::Json Runtime::evaluate(std::string_view source, std::string_view filename) {
    Impl::Scope scope(*impl_);
    if (source.size() > protocol::Dispatcher::max_body_bytes || filename.find('\0') != std::string_view::npos) {
        throw ScriptError("Invalid script source size or filename");
    }
    const std::string owned_source(source);
    const std::string owned_filename(filename);
    Value result(impl_->context, JS_Eval(impl_->context, owned_source.c_str(), owned_source.size(),
                                      owned_filename.c_str(), JS_EVAL_TYPE_GLOBAL));
    impl_->check(result.get());
    impl_->run_jobs();
    return impl_->to_json(result.get());
}

Runtime::Json Runtime::execute_command(std::string_view name, const Json& params) {
    Impl::Scope scope(*impl_);
    const auto found = impl_->callbacks.find(std::string(name));
    if (found == impl_->callbacks.end()) { throw ScriptError("Unknown JavaScript command: " + std::string(name)); }
    // Retain the function because it may replace its own registration while running.
    Value callback(impl_->context, JS_DupValue(impl_->context, found->second));
    Value argument(impl_->context, impl_->from_json(params));
    impl_->check(argument.get());
    auto arg = argument.get();
    Value result(impl_->context, JS_Call(impl_->context, callback.get(), JS_UNDEFINED, 1, &arg));
    impl_->check(result.get());
    impl_->run_jobs();
    return impl_->to_json(result.get());
}

std::vector<std::string> Runtime::commands() const {
    impl_->require_owner();
    std::vector<std::string> result;
    for (const auto& [name, callback] : impl_->callbacks) { result.push_back(name); }
    return result;
}

Runtime::Json Runtime::settings() const { impl_->require_owner(); return impl_->configuration; }
std::map<std::string, std::string> Runtime::keybindings() const { impl_->require_owner(); return impl_->bindings; }

} // namespace astra::script
