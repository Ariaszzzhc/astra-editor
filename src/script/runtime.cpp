#include <astra/script/runtime.hpp>

#include <quickjs.h>

#include <chrono>
#include <algorithm>
#include <limits>
#include <deque>
#include <thread>
#include <utility>

namespace astra::script {
namespace {

class Value {
  public:
    Value(JSContext* context, JSValue value) : context_(context), value_(value) {
    }

    ~Value() {
        JS_FreeValue(context_, value_);
    }

    Value(const Value&) = delete;
    Value& operator=(const Value&) = delete;

    [[nodiscard]] JSValue get() const {
        return value_;
    }

  private:
    JSContext* context_;
    JSValue value_;
};

std::string text(JSContext* context, JSValueConst value) {
    std::size_t size{};
    const char* bytes = JS_ToCStringLen(context, &size, value);
    if (!bytes) {
        throw ScriptError("Unable to convert JavaScript value to text");
    }
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
    AsyncRequest request;
    Limits limits;
    std::thread::id owner = std::this_thread::get_id();
    JSRuntime* runtime{};
    JSContext* context{};
    std::chrono::steady_clock::time_point deadline{};
    bool active{};
    bool poisoned{};
    bool stopping{};
    std::map<std::string, JSValue> callbacks;
    Json configuration = Json::object();
    std::map<std::string, std::string> bindings;
    using Clock = std::chrono::steady_clock;

    struct Pending {
        std::string method;
        Json params;
        std::unique_ptr<Value> resolve;
        std::unique_ptr<Value> reject;
        std::unique_ptr<Value> signal;
        TaskId owner{};
        Clock::time_point expires;
        Clock::time_point wake{};
        std::optional<std::future<Json>> future;
        std::string operation;
        std::size_t offset{};
        Json accumulated;
        bool raw{};
    };

    struct Task {
        std::unique_ptr<Value> value;
        std::optional<Outcome> outcome;
        Clock::time_point expires;
        bool cancelled{};
        bool delivered{};
        bool detached{};
    };

    std::map<TaskId, Task> tasks;
    std::map<std::uint64_t, std::unique_ptr<Pending>> pending;
    std::map<std::string, std::map<std::uint64_t, std::unique_ptr<Value>>> listeners;
    std::map<std::string, std::string> modules;
    std::deque<std::future<Json>> cancellations;

    struct Abandoned {
        std::string method;
        std::future<Json> future;
    };

    std::deque<Abandoned> abandoned;
    std::deque<Json> drain_reports;
    std::vector<Json> errors;
    TaskId active_task{};
    std::uint64_t next_id{1};

    Impl(AsyncRequest incoming, Limits budget) : request(std::move(incoming)), limits(budget) {
        if (!request || limits.memory_bytes < 1024 * 1024 || limits.stack_bytes < 64 * 1024 ||
            limits.execution_time.count() <= 0 || limits.max_pending_jobs == 0 ||
            limits.max_pending_requests == 0 || limits.max_tasks == 0 ||
            limits.request_timeout.count() <= 0 || limits.task_timeout.count() <= 0) {
            throw std::invalid_argument("Invalid script runtime configuration");
        }
        runtime = JS_NewRuntime();
        if (!runtime) {
            throw ScriptError("Unable to create JavaScript runtime");
        }
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
        JS_SetModuleLoaderFunc(runtime, nullptr, load_module, this);
        try {
            Value api(context, JS_NewObject(context));
            check(api.get());
            define(api.get(), "request", rpc_request, 3);
            define(api.get(), "registerCommand", register_command, 2);
            define(api.get(), "_invokeCommand", invoke_command, 2);
            define(api.get(), "_commandNames", command_names, 0);
            define(api.get(), "utf8ByteLength", utf8_byte_length, 1);
            define(api.get(), "configure", configure, 1);
            define(api.get(), "settings", current_settings, 0);
            define(api.get(), "bind", bind, 2);
            define(api.get(), "on", subscribe, 2);
            define(api.get(), "off", unsubscribe, 2);
            Value global(context, JS_GetGlobalObject(context));
            if (JS_SetPropertyStr(context, global.get(), "astra", JS_DupValue(context, api.get())) <
                0) {
                throw_error();
            }
            constexpr auto bootstrap = R"JS(
                class AbortSignal {
                    constructor() { this.aborted = false; this.reason = undefined; this._listeners = new Set(); }
                    addEventListener(type, fn) { if (type === 'abort') this._listeners.add(fn); }
                    removeEventListener(type, fn) { if (type === 'abort') this._listeners.delete(fn); }
                    throwIfAborted() { if (this.aborted) throw this.reason; }
                }
                class AbortController {
                    constructor() { this.signal = new AbortSignal(); }
                    abort(reason) {
                        if (this.signal.aborted) return;
                        const error = reason ?? Object.assign(new Error('Operation cancelled'), {name:'AbortError'});
                        this.signal.aborted = true; this.signal.reason = error;
                        for (const fn of this.signal._listeners) fn({type:'abort', target:this.signal});
                    }
                }
                globalThis.AbortController = AbortController;
                globalThis.AbortSignal = AbortSignal;
                astra.context = 'service';
                astra.sleep = (milliseconds, options) => astra.request('_runtime.sleep', {milliseconds}, options);
                astra.operationPages = async function* (method, parameters = {}, options = {}) {
                    const {limit = 256, ...requestOptions} = options;
                    if (!Number.isInteger(limit) || limit < 1 || limit > 1024) throw new RangeError('Invalid page limit');
                    const started = await astra.request(method, parameters, {...requestOptions, raw:true});
                    if (!started?.operationId) { yield started; return; }
                    const id = started.operationId;
                    let offset = 0, completed = false;
                    try {
                        while (true) {
                            const response = await astra.request('operation.get', {id, offset, limit}, requestOptions);
                            if (response.state === 'pending') { await astra.sleep(8, requestOptions); continue; }
                            if (response.state !== 'completed') {
                                throw Object.assign(new Error(response.error?.message ?? 'Operation cancelled'),
                                    response.error, {name:response.state === 'cancelled' ? 'AbortError' : 'ServiceError'});
                            }
                            const next = response.page?.nextOffset;
                            completed = !response.page || next >= response.page.total;
                            yield response.result;
                            if (completed) return;
                            if (!Number.isInteger(next) || next <= offset) throw new Error('Operation page did not advance');
                            offset = next;
                        }
                    } finally {
                        if (!completed) await astra.request('operation.cancel', {id}, {raw:true});
                    }
                };
            )JS";
            Value initialized(context,
                              JS_Eval(context,
                                      bootstrap,
                                      std::char_traits<char>::length(bootstrap),
                                      "<astra-host>",
                                      JS_EVAL_TYPE_GLOBAL));
            check(initialized.get());
        } catch (...) {
            JS_FreeContext(context);
            JS_FreeRuntime(runtime);
            throw;
        }
    }

    ~Impl() {
        pending.clear();
        tasks.clear();
        listeners.clear();
        for (auto& [name, callback] : callbacks) {
            JS_FreeValue(context, callback);
        }
        JS_FreeContext(context);
        JS_FreeRuntime(runtime);
    }

    static JSModuleDef* load_module(JSContext* context, const char* name, void* opaque) {
        auto& host = *static_cast<Impl*>(opaque);
        const auto found = host.modules.find(name);
        if (found == host.modules.end()) {
            JS_ThrowReferenceError(context, "Module is not registered: %s", name);
            return nullptr;
        }
        const auto compiled = JS_Eval(context,
                                      found->second.c_str(),
                                      found->second.size(),
                                      name,
                                      JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
        if (JS_IsException(compiled)) {
            return nullptr;
        }
        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled));
        JS_FreeValue(context, compiled);
        return module;
    }

    std::uint64_t allocate_id() {
        if (next_id == std::numeric_limits<std::uint64_t>::max()) {
            throw ScriptError("Script identifier capacity exceeded");
        }
        return next_id++;
    }

    void emit_event(std::string_view event, const Json& payload) {
        if (stopping) {
            return;
        }
        const auto found = listeners.find(std::string(event));
        if (found == listeners.end()) {
            return;
        }
        std::vector<std::unique_ptr<Value>> event_callbacks;
        for (const auto& [id, callback] : found->second) {
            static_cast<void>(id);
            event_callbacks.push_back(
                std::make_unique<Value>(context, JS_DupValue(context, callback->get())));
        }
        Value argument(context, from_json(payload));
        check(argument.get());
        for (const auto& callback : event_callbacks) {
            if (tasks.size() >= limits.max_tasks) {
                throw ScriptError("Script event task capacity exceeded");
            }
            const auto id = allocate_id();
            active_task = id;
            auto arg = argument.get();
            try {
                Value result(context, JS_Call(context, callback->get(), JS_UNDEFINED, 1, &arg));
                check(result.get());
                retain(result.get(), id);
                tasks.at(id).detached = true;
                run_jobs();
            } catch (...) {
                active_task = 0;
                abandon_task(id);
                throw;
            }
            active_task = 0;
        }
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
            if (host.poisoned) {
                throw ScriptError(
                    "Script job queue exhausted its budget; reload scripts to recover");
            }
            if (host.active) {
                throw ScriptError("Reentrant JavaScript execution is unsupported");
            }
            host.deadline = std::chrono::steady_clock::now() + host.limits.execution_time;
            host.active = true;
        }

        ~Scope() {
            host.active = false;
        }
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
            if (JS_IsString(stack.get())) {
                message += "\n" + text(context, stack.get());
            }
        }
        throw ScriptError(message);
    }

    void check(JSValueConst value) {
        if (JS_IsException(value)) {
            throw_error();
        }
    }

    void define(JSValueConst object, const char* name, JSCFunction* function, int arity) {
        const auto value = JS_NewCFunction(context, function, name, arity);
        check(value);
        if (JS_SetPropertyStr(context, object, name, value) < 0) {
            throw_error();
        }
    }

    Json to_json(JSValueConst value) {
        if (JS_IsUndefined(value)) {
            return nullptr;
        }
        Value serialized(context, JS_JSONStringify(context, value, JS_UNDEFINED, JS_UNDEFINED));
        check(serialized.get());
        if (JS_IsUndefined(serialized.get())) {
            throw ScriptError("Value is not JSON serializable");
        }
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
            if (JS_ExecutePendingJob(runtime, &job_context) < 0) {
                throw_error();
            }
        }
    }

    template <typename Function>
    static JSValue guarded(JSContext* context, Function&& function) noexcept {
        try {
            return function(*static_cast<Impl*>(JS_GetContextOpaque(context)));
        } catch (const std::exception& error) {
            return JS_ThrowInternalError(context, "%s", error.what());
        } catch (...) {
            return JS_ThrowInternalError(context, "Native script callback failed");
        }
    }

    static JSValue
    utf8_byte_length(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl&) {
            if (argc != 1 || !JS_IsString(argv[0]))
                return JS_ThrowTypeError(context, "utf8ByteLength requires a string");
            std::size_t size{};
            const auto* bytes = JS_ToCStringLen(context, &size, argv[0]);
            if (!bytes)
                return JS_EXCEPTION;
            JS_FreeCString(context, bytes);
            if (size > 9'007'199'254'740'991ULL)
                return JS_ThrowRangeError(context,
                                          "UTF-8 length exceeds a safe JavaScript integer");
            return JS_NewFloat64(context, static_cast<double>(size));
        });
    }

    static JSValue rpc_request(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc < 1 || !JS_IsString(argv[0])) {
                return JS_ThrowTypeError(context, "request requires a method string");
            }
            const auto method = text(context, argv[0]);
            const auto params = argc > 1 ? host.to_json(argv[1]) : Json::object();
            if (method.empty() || method.size() > 256 || !params.is_object()) {
                return JS_ThrowTypeError(context, "request requires a method and parameter object");
            }
            if (host.pending.size() + host.abandoned.size() + host.cancellations.size() >=
                host.limits.max_pending_requests) {
                return JS_ThrowRangeError(context, "Script request capacity exceeded");
            }
            auto duration = host.limits.request_timeout;
            auto signal = std::make_unique<Value>(context, JS_UNDEFINED);
            bool raw{};
            if (argc > 2 && !JS_IsUndefined(argv[2])) {
                if (!JS_IsObject(argv[2])) {
                    return JS_ThrowTypeError(context, "request options must be an object");
                }
                Value timeout(context, JS_GetPropertyStr(context, argv[2], "timeoutMs"));
                host.check(timeout.get());
                if (!JS_IsUndefined(timeout.get())) {
                    std::int64_t milliseconds{};
                    if (JS_ToInt64(context, &milliseconds, timeout.get()) < 0) {
                        host.throw_error();
                    }
                    if (milliseconds <= 0 || milliseconds > 300000) {
                        return JS_ThrowRangeError(context, "timeoutMs must be 1 to 300000");
                    }
                    duration = std::chrono::milliseconds(milliseconds);
                }
                signal =
                    std::make_unique<Value>(context, JS_GetPropertyStr(context, argv[2], "signal"));
                host.check(signal->get());
                Value raw_option(context, JS_GetPropertyStr(context, argv[2], "raw"));
                host.check(raw_option.get());
                if (!JS_IsUndefined(raw_option.get()) && !JS_IsBool(raw_option.get())) {
                    return JS_ThrowTypeError(context, "raw must be boolean");
                }
                raw = JS_ToBool(context, raw_option.get()) > 0;
            }
            JSValue functions[2];
            Value promise(context, JS_NewPromiseCapability(context, functions));
            host.check(promise.get());
            auto request = std::make_unique<Pending>();
            request->method = method;
            request->params = params;
            request->resolve = std::make_unique<Value>(context, functions[0]);
            request->reject = std::make_unique<Value>(context, functions[1]);
            request->signal = std::move(signal);
            request->owner = host.active_task;
            request->raw = raw;
            request->expires = Clock::now() + duration;
            if (method == "_runtime.sleep") {
                const auto milliseconds = params.value("milliseconds", std::int64_t{-1});
                if (milliseconds < 0 || milliseconds > 300000) {
                    return JS_ThrowRangeError(context, "sleep must be 0 to 300000 milliseconds");
                }
                request->wake = Clock::now() + std::chrono::milliseconds(milliseconds);
            }
            host.pending.emplace(host.allocate_id(), std::move(request));
            return JS_DupValue(context, promise.get());
        });
    }

    static JSValue subscribe(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsFunction(context, argv[1])) {
                return JS_ThrowTypeError(context, "on requires an event name and callback");
            }
            const auto event = text(context, argv[0]);
            if (event.empty() || event.size() > 256) {
                return JS_ThrowRangeError(context, "Invalid event name");
            }
            std::size_t total{};
            for (const auto& [name, values] : host.listeners) {
                static_cast<void>(name);
                total += values.size();
            }
            if (total >= host.limits.max_pending_requests) {
                return JS_ThrowRangeError(context, "Event listener capacity exceeded");
            }
            const auto id = host.allocate_id();
            host.listeners[event].emplace(
                id, std::make_unique<Value>(context, JS_DupValue(context, argv[1])));
            const auto token = std::to_string(id);
            return JS_NewStringLen(context, token.data(), token.size());
        });
    }

    static JSValue unsubscribe(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
                return JS_ThrowTypeError(context, "off requires an event and subscription token");
            }
            const auto event = host.listeners.find(text(context, argv[0]));
            if (event == host.listeners.end()) {
                return JS_FALSE;
            }
            const auto id = std::stoull(text(context, argv[1]));
            const auto erased = event->second.erase(id);
            if (event->second.empty()) {
                host.listeners.erase(event);
            }
            return JS_NewBool(context, erased != 0);
        });
    }

    Json error_json(JSValueConst value) {
        if (!JS_IsObject(value)) {
            return {{"name", "Error"}, {"message", text(context, value)}};
        }
        Json error;
        for (const auto* name : {"name", "message", "stack", "code", "data"}) {
            Value property(context, JS_GetPropertyStr(context, value, name));
            check(property.get());
            if (!JS_IsUndefined(property.get())) {
                error[name] = to_json(property.get());
            }
        }
        if (!error.contains("message")) {
            error["message"] = text(context, value);
        }
        return error;
    }

    void settle(Pending& incoming, const Json& value, bool failure) {
        const auto saved_owner = active_task;
        active_task = incoming.owner;
        try {
            Value argument(context, failure ? JS_NewError(context) : from_json(value));
            check(argument.get());
            if (failure) {
                for (const auto& [name, field] : value.items()) {
                    if (JS_SetPropertyStr(context, argument.get(), name.c_str(), from_json(field)) <
                        0) {
                        throw_error();
                    }
                }
            }
            auto arg = argument.get();
            Value result(context,
                         JS_Call(context,
                                 failure ? incoming.reject->get() : incoming.resolve->get(),
                                 JS_UNDEFINED,
                                 1,
                                 &arg));
            check(result.get());
            run_jobs();
        } catch (...) {
            active_task = saved_owner;
            throw;
        }
        active_task = saved_owner;
    }

    void request_cancel(Pending& incoming) {
        if (!incoming.operation.empty()) {
            cancellations.push_back(request("operation.cancel", {{"id", incoming.operation}}));
        } else if (incoming.future) {
            // An outstanding transport response may reveal an operation ID only
            // after cancellation. Retain it and cancel that operation on receipt.
            abandoned.push_back({incoming.method, std::move(*incoming.future)});
            incoming.future.reset();
        }
    }

    void tick() {
        for (auto& [id, task] : tasks) {
            static_cast<void>(id);
            if (!task.outcome && Clock::now() >= task.expires) {
                task.cancelled = true;
                task.outcome = Outcome{
                    nullptr, Json{{"name", "TimeoutError"}, {"message", "Script task timed out"}}};
            }
        }
        // Resolving one request drains its microtasks with the originating task
        // context. Newly issued requests are deferred to the next owner-loop tick.
        std::vector<std::uint64_t> ids;
        for (const auto& [id, value] : pending) {
            static_cast<void>(value);
            ids.push_back(id);
        }
        for (const auto id : ids) {
            const auto found = pending.find(id);
            if (found == pending.end()) {
                continue;
            }
            auto& incoming = *found->second;
            std::optional<Json> result;
            std::optional<Json> failure;
            try {
                bool aborted{};
                if (!JS_IsUndefined(incoming.signal->get())) {
                    Value value(context,
                                JS_GetPropertyStr(context, incoming.signal->get(), "aborted"));
                    check(value.get());
                    aborted = JS_ToBool(context, value.get()) != 0;
                }
                const auto task = tasks.find(incoming.owner);
                if (stopping || aborted || (task != tasks.end() && task->second.cancelled)) {
                    failure = {{"name", "AbortError"}, {"message", "Operation cancelled"}};
                } else if (Clock::now() >= incoming.expires) {
                    failure = {{"name", "TimeoutError"}, {"message", "Operation timed out"}};
                }
                if (failure) {
                    request_cancel(incoming);
                } else if (incoming.method == "_runtime.sleep") {
                    if (Clock::now() >= incoming.wake) {
                        result = nullptr;
                    }
                } else {
                    if (!incoming.future) {
                        incoming.future.emplace(incoming.operation.empty()
                                                    ? request(incoming.method, incoming.params)
                                                    : request("operation.get",
                                                              {{"id", incoming.operation},
                                                               {"offset", incoming.offset},
                                                               {"limit", 256}}));
                    }
                    if (incoming.future->wait_for(std::chrono::seconds(0)) ==
                        std::future_status::ready) {
                        auto value = incoming.future->get();
                        incoming.future.reset();
                        if (!incoming.raw && incoming.operation.empty() && value.is_object() &&
                            value.contains("operationId")) {
                            incoming.operation = value.at("operationId").get<std::string>();
                        } else if (!incoming.operation.empty()) {
                            const auto state = value.at("state").get<std::string>();
                            if (state == "failed" || state == "cancelled") {
                                failure =
                                    value.value("error", Json{{"message", "Operation cancelled"}});
                                (*failure)["name"] =
                                    state == "cancelled" ? "AbortError" : "ServiceError";
                            } else if (state == "completed") {
                                auto page = value.at("result");
                                if (value.contains("page")) {
                                    for (const auto* field : {"entries", "matches"}) {
                                        if (!page.contains(field)) {
                                            continue;
                                        }
                                        if (incoming.accumulated.is_null()) {
                                            incoming.accumulated = page;
                                        } else {
                                            for (const auto& entry : page[field]) {
                                                incoming.accumulated[field].push_back(entry);
                                            }
                                        }
                                    }
                                    if (incoming.accumulated.dump().size() >
                                        protocol::Dispatcher::max_body_bytes) {
                                        throw ScriptError("Script operation result exceeds the "
                                                          "byte budget; use paged requests");
                                    }
                                    incoming.offset =
                                        value.at("page").at("nextOffset").get<std::size_t>();
                                    if (incoming.offset >=
                                        value.at("page").at("total").get<std::size_t>()) {
                                        result = incoming.accumulated.is_null()
                                                     ? page
                                                     : std::move(incoming.accumulated);
                                    }
                                } else {
                                    result = std::move(page);
                                }
                                if (result && result->is_object()) {
                                    (*result)["operationId"] = incoming.operation;
                                }
                            }
                        } else {
                            result = std::move(value);
                        }
                    }
                }
            } catch (const protocol::RpcError& error) {
                failure = {
                    {"name", "ServiceError"}, {"message", error.what()}, {"code", error.code()}};
                if (error.data()) {
                    (*failure)["data"] = *error.data();
                }
            } catch (const std::exception& error) {
                failure = {{"name", "Error"}, {"message", error.what()}};
            }
            if (failure || result) {
                auto completed = std::move(found->second);
                pending.erase(found);
                settle(*completed, failure ? *failure : *result, failure.has_value());
            }
        }
        for (auto i = cancellations.begin(); i != cancellations.end();) {
            if (i->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++i;
                continue;
            }
            // Cancellation transport errors must be observable, never swallowed.
            auto completed = std::move(*i);
            i = cancellations.erase(i);
            static_cast<void>(completed.get());
        }
        for (auto i = abandoned.begin(); i != abandoned.end();) {
            if (i->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++i;
                continue;
            }
            auto completed = std::move(*i);
            i = abandoned.erase(i);
            try {
                const auto reply = completed.future.get();
                if (reply.is_object() && reply.contains("operationId")) {
                    cancellations.push_back(
                        request("operation.cancel", {{"id", reply.at("operationId")}}));
                }
            } catch (const protocol::RpcError& error) {
                // A valid service rejection settles the already-cancelled
                // request without creating an operation to cancel. Report it
                // separately from the caller's settled AbortError. Transport,
                // malformed-reply and cancellation-RPC failures still surface.
                const auto code = error.code();
                if (!((code <= -32000 && code >= -32099) || code == -32600 || code == -32601 ||
                      code == -32602)) {
                    throw;
                }
                Json failure{{"code", code}, {"message", error.what()}};
                if (error.data()) {
                    failure["data"] = *error.data();
                }
                if (drain_reports.size() >= limits.max_pending_requests) {
                    throw ScriptError("Cancelled request report capacity exceeded");
                }
                drain_reports.push_back({{"method", completed.method},
                                         {"cancelled", true},
                                         {"error", std::move(failure)}});
            }
        }
        for (auto& [id, task] : tasks) {
            static_cast<void>(id);
            if (task.outcome) {
                continue;
            }
            if (Clock::now() >= task.expires) {
                task.cancelled = true;
                task.outcome = Outcome{
                    nullptr, Json{{"name", "TimeoutError"}, {"message", "Script task timed out"}}};
                continue;
            }
            const auto state = JS_PromiseState(context, task.value->get());
            if (state == JS_PROMISE_PENDING) {
                continue;
            }
            Value value(context,
                        state == JS_PROMISE_NOT_A_PROMISE
                            ? JS_DupValue(context, task.value->get())
                            : JS_PromiseResult(context, task.value->get()));
            check(value.get());
            try {
                task.outcome = state == JS_PROMISE_REJECTED
                                   ? Outcome{nullptr, error_json(value.get())}
                                   : Outcome{to_json(value.get()), std::nullopt};
            } catch (const ScriptError& error) {
                task.outcome = Outcome{
                    nullptr, Json{{"name", "SerializationError"}, {"message", error.what()}}};
            }
        }
        for (auto& [id, task] : tasks) {
            static_cast<void>(id);
            if (!task.detached || !task.outcome || task.delivered) {
                continue;
            }
            if (task.outcome->error) {
                if (errors.size() >= limits.max_tasks) {
                    throw ScriptError("Unhandled script error capacity exceeded");
                }
                errors.push_back(*task.outcome->error);
            }
            task.delivered = true;
        }
        std::erase_if(tasks, [this](const auto& entry) {
            return entry.second.delivered &&
                   std::ranges::none_of(pending, [&](const auto& incoming) {
                       return incoming.second->owner == entry.first;
                   });
        });
    }

    TaskId retain(JSValueConst value, TaskId id) {
        if (tasks.size() >= limits.max_tasks) {
            throw ScriptError("Script task capacity exceeded");
        }
        tasks.emplace(id,
                      Task{std::make_unique<Value>(context, JS_DupValue(context, value)),
                           std::nullopt,
                           Clock::now() + limits.task_timeout,
                           false,
                           false});
        return id;
    }

    void abandon_task(TaskId id) {
        tasks.erase(id);
        if (std::ranges::any_of(pending, [id](const auto& entry) {
                return entry.second->owner == id;
            })) {
            Task cancelled;
            cancelled.cancelled = true;
            cancelled.delivered = true;
            cancelled.outcome = Outcome{
                nullptr,
                Json{{"name", "AbortError"}, {"message", "Script failed before returning"}}};
            tasks.emplace(id, std::move(cancelled));
        }
    }

    static JSValue
    register_command(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc != 2 || !JS_IsString(argv[0]) || !JS_IsFunction(context, argv[1])) {
                return JS_ThrowTypeError(context, "registerCommand requires a name and function");
            }
            const auto name = text(context, argv[0]);
            if (name.empty() || name.size() > 128) {
                return JS_ThrowRangeError(context, "Invalid command name");
            }
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

    static JSValue invoke_command(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc < 1 || argc > 2 || !JS_IsString(argv[0]))
                return JS_ThrowTypeError(
                    context, "Command invocation requires a name and optional parameters");
            const auto name = text(context, argv[0]);
            const auto found = host.callbacks.find(name);
            if (found == host.callbacks.end())
                return JS_ThrowReferenceError(context, "Unknown command: %s", name.c_str());
            // Keep the callback alive if it replaces its own registration.
            Value callback(context, JS_DupValue(context, found->second));
            Value parameters(context,
                             argc == 2 ? JS_DupValue(context, argv[1]) : JS_NewObject(context));
            host.check(parameters.get());
            auto argument = parameters.get();
            return JS_Call(context, callback.get(), JS_UNDEFINED, 1, &argument);
        });
    }

    static JSValue command_names(JSContext* context, JSValueConst, int, JSValueConst*) {
        return guarded(context, [](Impl& host) {
            auto names = Json::array();
            for (const auto& [name, callback] : host.callbacks) {
                static_cast<void>(callback);
                names.push_back(name);
            }
            return host.from_json(names);
        });
    }

    static JSValue current_settings(JSContext* context, JSValueConst, int, JSValueConst*) {
        return guarded(context, [](Impl& host) {
            return host.from_json(host.configuration);
        });
    }

    static JSValue configure(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
        return guarded(context, [&](Impl& host) {
            if (argc != 1) {
                return JS_ThrowTypeError(context, "configure requires one object");
            }
            auto configuration = host.to_json(argv[0]);
            if (!configuration.is_object()) {
                return JS_ThrowTypeError(context, "Configuration must be an object");
            }
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

Runtime::Runtime(Request request) : Runtime(std::move(request), Limits{}) {
}

Runtime::Runtime(Request request, Limits limits)
    : Runtime(
          AsyncRequest{[request = std::move(request)](std::string_view method, const Json& params) {
              std::promise<Json> promise;
              try {
                  if (!request) {
                      throw std::invalid_argument("Missing script request dispatcher");
                  }
                  promise.set_value(request(method, params));
              } catch (...) {
                  promise.set_exception(std::current_exception());
              }
              return promise.get_future();
          }},
          limits) {
}

Runtime::Runtime(AsyncRequest request) : Runtime(std::move(request), Limits{}) {
}

Runtime::Runtime(AsyncRequest request, Limits limits)
    : impl_(std::make_unique<Impl>(std::move(request), limits)) {
}

Runtime::~Runtime() = default;

Runtime::TaskId Runtime::start_evaluation(std::string_view source, std::string_view filename) {
    Impl::Scope scope(*impl_);
    if (impl_->stopping) {
        throw ScriptError("Script runtime is shutting down");
    }
    if (source.size() > protocol::Dispatcher::max_body_bytes ||
        filename.find('\0') != std::string_view::npos) {
        throw ScriptError("Invalid script source size or filename");
    }
    const std::string owned_source(source);
    const std::string owned_filename(filename);
    if (impl_->tasks.size() >= impl_->limits.max_tasks) {
        throw ScriptError("Script task capacity exceeded");
    }
    const auto id = impl_->allocate_id();
    impl_->active_task = id;
    try {
        Value result(impl_->context,
                     JS_Eval(impl_->context,
                             owned_source.c_str(),
                             owned_source.size(),
                             owned_filename.c_str(),
                             JS_EVAL_TYPE_GLOBAL));
        impl_->check(result.get());
        impl_->retain(result.get(), id);
        impl_->run_jobs();
    } catch (...) {
        impl_->active_task = 0;
        impl_->abandon_task(id);
        throw;
    }
    impl_->active_task = 0;
    return id;
}

Runtime::TaskId Runtime::start_command(std::string_view name, const Json& params) {
    Impl::Scope scope(*impl_);
    if (impl_->stopping) {
        throw ScriptError("Script runtime is shutting down");
    }
    const auto found = impl_->callbacks.find(std::string(name));
    if (found == impl_->callbacks.end()) {
        throw ScriptError("Unknown JavaScript command: " + std::string(name));
    }
    if (impl_->tasks.size() >= impl_->limits.max_tasks) {
        throw ScriptError("Script task capacity exceeded");
    }
    const auto id = impl_->allocate_id();
    Value callback(impl_->context, JS_DupValue(impl_->context, found->second));
    Value argument(impl_->context, impl_->from_json(params));
    impl_->check(argument.get());
    auto arg = argument.get();
    impl_->active_task = id;
    try {
        Value result(impl_->context,
                     JS_Call(impl_->context, callback.get(), JS_UNDEFINED, 1, &arg));
        impl_->check(result.get());
        impl_->retain(result.get(), id);
        impl_->run_jobs();
    } catch (...) {
        impl_->active_task = 0;
        impl_->abandon_task(id);
        throw;
    }
    impl_->active_task = 0;
    return id;
}

void Runtime::poll() {
    Impl::Scope scope(*impl_);
    impl_->tick();
    while (!impl_->drain_reports.empty() && Impl::Clock::now() < impl_->deadline) {
        auto report = std::move(impl_->drain_reports.front());
        impl_->drain_reports.pop_front();
        impl_->emit_event("runtime.requestDrained", report);
    }
}

std::optional<Runtime::Outcome> Runtime::take_result(TaskId id) {
    impl_->require_owner();
    const auto found = impl_->tasks.find(id);
    if (found == impl_->tasks.end()) {
        throw ScriptError("Script task does not exist");
    }
    if (!found->second.outcome || found->second.delivered) {
        return std::nullopt;
    }
    auto result = found->second.outcome;
    found->second.delivered = true;
    if (std::ranges::none_of(impl_->pending, [&](const auto& incoming) {
            return incoming.second->owner == id;
        })) {
        impl_->tasks.erase(found);
    }
    return result;
}

void Runtime::cancel(TaskId id) {
    impl_->require_owner();
    const auto found = impl_->tasks.find(id);
    if (found == impl_->tasks.end()) {
        throw ScriptError("Script task does not exist");
    }
    if (found->second.outcome) {
        return;
    }
    found->second.cancelled = true;
    found->second.outcome =
        Outcome{nullptr, Json{{"name", "AbortError"}, {"message", "Script task cancelled"}}};
}

bool Runtime::idle() const {
    impl_->require_owner();
    return impl_->pending.empty() && impl_->tasks.empty() && impl_->cancellations.empty() &&
           impl_->abandoned.empty() && impl_->drain_reports.empty();
}

bool Runtime::waiting_only(std::string_view method) const {
    return waiting_only(std::span<const std::string_view>(&method, 1));
}

bool Runtime::waiting_only(std::span<const std::string_view> methods) const {
    impl_->require_owner();
    if (impl_->pending.empty() || !impl_->cancellations.empty() || !impl_->abandoned.empty() ||
        !impl_->drain_reports.empty() || JS_IsJobPending(impl_->runtime)) {
        return false;
    }
    if (!std::ranges::all_of(impl_->pending, [methods](const auto& entry) {
            return std::ranges::find(methods, entry.second->method) != methods.end() &&
                   entry.second->future.has_value();
        })) {
        return false;
    }
    return std::ranges::all_of(impl_->tasks, [this](const auto& entry) {
        return !entry.second.cancelled && !entry.second.outcome &&
               std::ranges::any_of(impl_->pending, [id = entry.first](const auto& request) {
                   return request.second->owner == id;
               });
    });
}

void Runtime::shutdown() {
    impl_->require_owner();
    impl_->stopping = true;
    for (auto& [id, task] : impl_->tasks) {
        static_cast<void>(id);
        task.cancelled = true;
        task.delivered = true;
        task.outcome = Outcome{
            nullptr, Json{{"name", "AbortError"}, {"message", "Script runtime is shutting down"}}};
    }
    poll();
}

std::vector<Runtime::Json> Runtime::take_errors() {
    impl_->require_owner();
    return std::exchange(impl_->errors, {});
}

Runtime::Json Runtime::evaluate(std::string_view source, std::string_view filename) {
    const auto id = start_evaluation(source, filename);
    for (int tick = 0; tick < 16; ++tick) {
        poll();
        if (auto result = take_result(id)) {
            if (result->error) {
                throw ScriptError(result->error->value("message", std::string("Script failed")));
            }
            return std::move(result->value);
        }
    }
    cancel(id);
    static_cast<void>(take_result(id));
    throw ScriptError("Evaluation is asynchronous; use start_evaluation and poll");
}

Runtime::Json Runtime::execute_command(std::string_view name, const Json& params) {
    const auto id = start_command(name, params);
    for (int tick = 0; tick < 16; ++tick) {
        poll();
        if (auto result = take_result(id)) {
            if (result->error) {
                throw ScriptError(result->error->value("message", std::string("Script failed")));
            }
            return std::move(result->value);
        }
    }
    cancel(id);
    static_cast<void>(take_result(id));
    throw ScriptError("Command is asynchronous; use start_command and poll");
}

void Runtime::emit(std::string_view event, const Json& payload) {
    Impl::Scope scope(*impl_);
    impl_->emit_event(event, payload);
}

void Runtime::define_module(std::string name, std::string source) {
    impl_->require_owner();
    if (name.empty() || name.find('\0') != std::string::npos || name.size() > 256 ||
        source.size() > protocol::Dispatcher::max_body_bytes || impl_->modules.size() >= 128) {
        throw ScriptError("Invalid module name, source or module capacity");
    }
    if (!impl_->modules.emplace(std::move(name), std::move(source)).second) {
        throw ScriptError("Module is already registered");
    }
}

Runtime::Json Runtime::evaluate_module(std::string_view source, std::string_view filename) {
    Impl::Scope scope(*impl_);
    if (source.size() > protocol::Dispatcher::max_body_bytes ||
        filename.find('\0') != std::string_view::npos) {
        throw ScriptError("Invalid script module source or filename");
    }
    const std::string owned(source);
    const std::string name(filename);
    Value value(
        impl_->context,
        JS_Eval(impl_->context, owned.c_str(), owned.size(), name.c_str(), JS_EVAL_TYPE_MODULE));
    impl_->check(value.get());
    impl_->run_jobs();
    if (JS_PromiseState(impl_->context, value.get()) == JS_PROMISE_REJECTED) {
        Value error(impl_->context, JS_PromiseResult(impl_->context, value.get()));
        throw ScriptError(impl_->error_json(error.get())
                              .value("message", std::string("Module initialization failed")));
    }
    if (JS_PromiseState(impl_->context, value.get()) == JS_PROMISE_PENDING) {
        throw ScriptError("Module initialization must register policy synchronously; requests "
                          "belong in async commands");
    }
    return nullptr;
}

std::vector<std::string> Runtime::commands() const {
    impl_->require_owner();
    std::vector<std::string> result;
    for (const auto& [name, callback] : impl_->callbacks) {
        result.push_back(name);
    }
    return result;
}

Runtime::Json Runtime::settings() const {
    impl_->require_owner();
    return impl_->configuration;
}

std::map<std::string, std::string> Runtime::keybindings() const {
    impl_->require_owner();
    return impl_->bindings;
}

} // namespace astra::script
