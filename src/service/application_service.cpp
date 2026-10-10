#include <astra/service/application_service.hpp>
#include <astra/core/utf8.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <limits>
#include <set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::service {
namespace {
using protocol::Json;
using protocol::RpcError;
namespace fs = std::filesystem;

[[noreturn]] void invalid(std::string message) {
    throw RpcError(-32602, std::move(message));
}

void fields(const Json& params, std::initializer_list<std::string_view> allowed) {
    if (!params.is_object()) {
        invalid("Named object parameters are required");
    }
    for (auto i = params.begin(); i != params.end(); ++i) {
        if (std::find(allowed.begin(), allowed.end(), i.key()) == allowed.end()) {
            invalid("Unrecognized parameter: " + i.key());
        }
    }
}

const std::string& string_field(const Json& params, std::string_view key) {
    const auto found = params.find(std::string(key));
    if (found == params.end() || !found->is_string()) {
        invalid("Required string: " + std::string(key));
    }
    return found->get_ref<const std::string&>();
}

std::size_t
number(const Json& params, std::string_view key, std::size_t fallback, std::size_t maximum) {
    const auto found = params.find(std::string(key));
    if (found == params.end()) {
        return fallback;
    }
    if (!found->is_number_integer() ||
        (!found->is_number_unsigned() && found->get<std::int64_t>() < 0) ||
        found->get<std::uint64_t>() > maximum) {
        invalid("Invalid integer: " + std::string(key));
    }
    return found->get<std::size_t>();
}

bool flag(const Json& params, std::string_view key, bool fallback = false) {
    const auto found = params.find(std::string(key));
    if (found == params.end()) {
        return fallback;
    }
    if (!found->is_boolean()) {
        invalid("Invalid boolean: " + std::string(key));
    }
    return found->get<bool>();
}

std::string path_text(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

fs::path parse_path(const std::string& text) {
    if (text.empty() || text.size() > 32768 || text.find('\0') != std::string::npos) {
        invalid("A nonempty workspace-relative path is required");
    }
    // Rope performs strict UTF-8 validation before conversion through char8_t.
    static_cast<void>(Rope(text));
    auto path = fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    if (path.is_absolute() || path.has_root_name()) {
        invalid("Path must be relative to the workspace");
    }
    for (const auto& part : path) {
        if (part == "..") {
            invalid("Parent traversal is not allowed");
        }
    }
    return path.lexically_normal();
}

bool same_path(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    const auto a = left.lexically_normal().native();
    const auto b = right.lexically_normal().native();
    if (a.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        b.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
#else
    return left.lexically_normal() == right.lexically_normal();
#endif
}

Json issues_json(const std::vector<workspace::WorkspaceIssue>& issues) {
    auto result = Json::array();
    for (const auto& issue : issues) {
        Json item{{"path", path_text(issue.relative_path)}, {"message", issue.message}};
        if (issue.recovery_path) {
            item["recoveryPath"] = path_text(*issue.recovery_path);
        }
        result.push_back(std::move(item));
    }
    return result;
}

Json exception_json(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const io::FileRecoveryRequired& failure) {
        return {{"code", -32012},
                {"message", failure.what()},
                {"recoveryPath", path_text(failure.backup_path())}};
    } catch (const io::FileConflict& failure) {
        return {{"code", -32011}, {"message", failure.what()}};
    } catch (const RpcError& failure) {
        Json result{{"code", failure.code()}, {"message", failure.what()}};
        if (failure.data()) {
            result["data"] = *failure.data();
        }
        return result;
    } catch (const std::exception& failure) {
        return {{"code", -32010}, {"message", failure.what()}};
    } catch (...) {
        return {{"code", -32603}, {"message", "Unknown operation failure"}};
    }
}

std::vector<Edit> formatting_edit(const Rope& before, const std::string& after) {
    const auto original = before.str();
    if (original == after) {
        return {};
    }
    std::size_t prefix = 0;
    while (prefix < original.size() && prefix < after.size() && original[prefix] == after[prefix]) {
        ++prefix;
    }
    while (!before.is_boundary(prefix)) {
        --prefix;
    }
    std::size_t suffix = 0;
    while (suffix < original.size() - prefix && suffix < after.size() - prefix &&
           original[original.size() - suffix - 1] == after[after.size() - suffix - 1]) {
        ++suffix;
    }
    while (!before.is_boundary(original.size() - suffix)) {
        --suffix;
    }
    return {{ByteOffset{prefix},
             ByteOffset{original.size() - suffix},
             after.substr(prefix, after.size() - prefix - suffix)}};
}
} // namespace

ApplicationService::ApplicationService(const fs::path& root,
                                       Scheduler scheduler,
                                       std::string_view defaults_script,
                                       std::string_view user_script,
                                       const fs::path& resource_directory,
                                       const fs::path& recovery_directory,
                                       bool watch_files)
    : workspace_(std::make_shared<workspace::Workspace>(root)), scheduler_(std::move(scheduler)) {
    if (!scheduler_) {
        throw std::invalid_argument("ApplicationService requires a worker scheduler");
    }
    auto& rpc = editor_.dispatcher();
    rpc.register_method("workspace.info", [this](const Json& p) {
        fields(p, {});
        return Json{{"root", path_text(workspace_->root())},
                    {"name", path_text(workspace_->root().filename())}};
    });
    rpc.register_method("workspace.open", [this](const Json& p) {
        return open_file(p);
    });
    rpc.register_method("workspace.save", [this](const Json& p) {
        return save_file(p);
    });
    rpc.register_method("workspace.tree", [this](const Json& p) {
        return tree(p);
    });
    rpc.register_method("workspace.search", [this](const Json& p) {
        return search(p);
    });
    rpc.register_method("workspace.replace", [this](const Json& p) {
        return replace(p);
    });
    rpc.register_method("document.find", [this](const Json& p) {
        return find_document_text(p);
    });
    rpc.register_method("document.replaceAll", [this](const Json& p) {
        return replace_document_text(p);
    });
    rpc.register_method("workspace.documents", [this](const Json& p) {
        fields(p, {});
        auto result = Json::array();
        for (const auto& [id, document] : editor_.documents_) {
            static_cast<void>(id);
            result.push_back(describe(document));
        }
        return result;
    });
    rpc.register_method("operation.get", [this](const Json& p) {
        return operation_get(p);
    });
    rpc.register_method("operation.cancel", [this](const Json& p) {
        return operation_cancel(p);
    });
    rpc.register_method("document.syntax", [this](const Json& p) {
        return syntax_view(p);
    });
    rpc.register_method("document.selectObject", [this](const Json& p) {
        return select_object(p);
    });
    script_ = std::make_unique<script::Runtime>([this](std::string_view method, const Json& p) {
        return script_request(method, p);
    });
    editor_.command_defaults_ = [this](const Document& document) {
        return command_defaults(document);
    };
    rpc.register_method("script.command", [this](const Json& p) {
        fields(p, {"name", "params"});
        try {
            return script_command(p);
        } catch (const script::ScriptError& error) {
            throw RpcError(-32020, error.what());
        }
    });
    rpc.register_method("script.settings", [this](const Json& p) {
        fields(p, {});
        return Json{{"settings", script_->settings()},
                    {"keybindings", script_->keybindings()},
                    {"commands", script_->commands()}};
    });
    if (!defaults_script.empty()) {
        static_cast<void>(script_->evaluate(defaults_script, "astra-defaults.js"));
    }
    if (!user_script.empty()) {
        static_cast<void>(script_->evaluate(user_script, "astra-user.js"));
    }
    register_language_methods(resource_directory);
    register_git_methods(resource_directory);
    register_recovery_methods(recovery_directory);
    register_watch_methods(watch_files);
}

ApplicationService::~ApplicationService() = default;

editing::Options ApplicationService::command_defaults(const Document& document) const {
    editing::Options options;
    const auto settings = script_->settings();
    if (settings.contains("editor")) {
        const auto& editor = settings.at("editor");
        if (!editor.is_object()) {
            throw RpcError(-32020, "editor settings must be an object");
        }
        if (editor.contains("tabSize")) {
            const auto& size = editor.at("tabSize");
            if (!size.is_number_integer() ||
                (!size.is_number_unsigned() && size.get<std::int64_t>() <= 0) ||
                size.get<std::uint64_t>() == 0 || size.get<std::uint64_t>() > 256) {
                throw RpcError(-32020, "editor.tabSize must be 1 to 256");
            }
            options.tab_size = size.get<std::size_t>();
        }
        if (editor.contains("insertSpaces")) {
            if (!editor.at("insertSpaces").is_boolean()) {
                throw RpcError(-32020, "editor.insertSpaces must be boolean");
            }
            options.insert_spaces = editor.at("insertSpaces").get<bool>();
        }
    }
    const auto& text = document.text();
    if (text.line_count() > 1) {
        const auto next = text.line_start(1);
        if (next >= 2 && text.is_boundary(next - 2) && text.slice(next - 2, next) == "\r\n") {
            options.newline = "\r\n";
        }
    }
    if (const auto found = files_.find(document.id().value); found != files_.end()) {
        const auto& policy = found->second.config.settings;
        if (policy.indent_style) {
            options.insert_spaces = *policy.indent_style == config::IndentStyle::spaces;
        }
        const auto width = options.insert_spaces
                               ? policy.indent_size.value_or(policy.tab_width.value_or(0))
                               : policy.tab_width.value_or(policy.indent_size.value_or(0));
        if (width != 0) {
            options.tab_size = width;
        }
        if (policy.end_of_line) {
            switch (*policy.end_of_line) {
            case config::LineEnding::lf:
                options.newline = "\n";
                break;
            case config::LineEnding::crlf:
                options.newline = "\r\n";
                break;
            case config::LineEnding::cr:
                options.newline = "\n";
                break;
            }
        }
        const auto extension = path_text(found->second.path.extension());
        if (extension == ".py" || extension == ".pyi" || extension == ".sh" ||
            extension == ".ps1" || extension == ".yaml" || extension == ".yml" ||
            extension == ".toml" || extension == ".rb") {
            options.comment_prefix = "#";
        } else if (extension == ".lua" || extension == ".sql") {
            options.comment_prefix = "--";
        }
    }
    return options;
}

Json ApplicationService::script_request(std::string_view method, const Json& params) {
    if (method.starts_with("script.")) {
        throw RpcError(-32020, "Recursive script entry is not allowed");
    }
    const auto response = editor_.dispatch(
        {{"jsonrpc", "2.0"}, {"id", "script"}, {"method", method}, {"params", params}});
    if (!response) {
        throw RpcError(-32603, "Missing script request reply");
    }
    if (response->contains("error")) {
        const auto& error = (*response)["error"];
        throw RpcError(error["code"].get<int>(),
                       error["message"].get<std::string>(),
                       error.contains("data") ? std::optional<Json>(error["data"]) : std::nullopt);
    }
    return (*response)["result"];
}

Json ApplicationService::script_command(const Json& params) {
    // Fast commands may settle in the same dispatch without waiting. A command
    // awaiting a worker becomes a regular, cancellable service operation.
    prune_operations();
    if (script_tasks_.size() >= 8 || operations_.size() >= 32) {
        throw RpcError(-32004, "Script operation capacity exceeded");
    }
    const auto task = script_->start_command(string_field(params, "name"),
                                             params.value("params", Json::object()));
    for (int tick = 0; tick < 4; ++tick) {
        script_->poll();
        for (const auto& error : script_->take_errors()) {
            notifications_.push_back(protocol::make_notification("script.error", error));
        }
        if (auto outcome = script_->take_result(task)) {
            if (outcome->error) {
                throw RpcError(outcome->error->value("code", -32020),
                               outcome->error->value("message", std::string("Script failed")),
                               *outcome->error);
            }
            return std::move(outcome->value);
        }
    }
    if (next_operation_ == std::numeric_limits<std::uint64_t>::max()) {
        script_->cancel(task);
        static_cast<void>(script_->take_result(task));
        throw RpcError(-32004, "Operation ID capacity exceeded");
    }
    auto operation = std::make_shared<Operation>();
    operation->sequence = next_operation_;
    operation->id = std::to_string(next_operation_++);
    operation->kind = "script";
    operations_.emplace(operation->id, operation);
    script_tasks_.emplace(operation->id, task);
    return {{"operationId", operation->id}};
}

void ApplicationService::poll_scripts() {
    if (polling_scripts_ || !script_) {
        return;
    }
    polling_scripts_ = true;
    try {
        script_->poll();
        for (const auto& error : script_->take_errors()) {
            notifications_.push_back(protocol::make_notification("script.error", error));
        }
        for (auto i = script_tasks_.begin(); i != script_tasks_.end();) {
            auto outcome = script_->take_result(i->second);
            if (!outcome) {
                ++i;
                continue;
            }
            auto operation = operations_.at(i->first);
            i = script_tasks_.erase(i);
            operation->result = std::move(outcome->value);
            if (outcome->error) {
                if (outcome->error->value("name", std::string{}) == "AbortError") {
                    operation->cancellation.request_stop();
                }
                finish_operation(operation,
                                 std::make_exception_ptr(script::ScriptError(outcome->error->value(
                                     "message", std::string("Script failed")))));
                operation->error = std::move(*outcome->error);
            } else {
                finish_operation(operation, nullptr);
            }
        }
    } catch (...) {
        polling_scripts_ = false;
        throw;
    }
    polling_scripts_ = false;
}

std::optional<std::string> ApplicationService::dispatch_text(std::string_view body) {
    poll_operations();
    poll_scripts();
    poll_language();
    auto result = editor_.dispatch_text(body);
    poll_language();
    return result;
}

std::optional<Json> ApplicationService::dispatch(const Json& request) {
    poll_operations();
    poll_scripts();
    poll_language();
    auto result = editor_.dispatch(request);
    poll_language();
    return result;
}

std::vector<std::string> ApplicationService::take_notifications() {
    poll_operations();
    poll_scripts();
    poll_language();
    poll_watch();
    poll_recovery();
    auto events = editor_.take_notifications();
    std::vector<std::string> result;
    result.reserve(events.size() + notifications_.size());
    for (auto& event : events) {
        const auto id = event["params"]["id"].get<std::string>();
        std::uint64_t parsed{};
        const auto converted = std::from_chars(id.data(), id.data() + id.size(), parsed);
        if (converted.ec == std::errc{}) {
            if (const auto found = files_.find(parsed); found != files_.end()) {
                event["params"]["path"] = path_text(found->second.path);
            }
            if (event["method"] == "document.closed") {
                files_.erase(parsed);
                syntax_.erase(parsed);
                if (const auto job = syntax_jobs_.find(parsed); job != syntax_jobs_.end()) {
                    if (const auto op = operations_.find(job->second.operation);
                        op != operations_.end()) {
                        op->second->cancellation.request_stop();
                    }
                    syntax_jobs_.erase(job);
                }
            }
        }
        try {
            script_->emit(event.at("method").get<std::string>(), event.at("params"));
        } catch (const script::ScriptError& error) {
            notifications_.push_back(
                protocol::make_notification("script.error", {{"message", error.what()}}));
        }
        result.push_back(event.dump());
    }
    auto additional = std::exchange(notifications_, {});
    for (const auto& event : additional) {
        try {
            script_->emit(event.at("method").get<std::string>(), event.at("params"));
        } catch (const script::ScriptError& error) {
            result.push_back(
                protocol::make_notification("script.error", {{"message", error.what()}}).dump());
        }
        result.push_back(event.dump());
    }
    return result;
}

Json ApplicationService::describe(const Document& document) const {
    auto result = EditorService::metadata(document);
    if (const auto found = files_.find(document.id().value); found != files_.end()) {
        result["path"] = path_text(found->second.path);
        result["editorConfig"] = found->second.config.properties;
        switch (found->second.encoding) {
        case io::TextEncoding::utf8:
            result["encoding"] = "UTF-8";
            break;
        case io::TextEncoding::utf8_bom:
            result["encoding"] = "UTF-8 BOM";
            break;
        case io::TextEncoding::utf16_le:
            result["encoding"] = "UTF-16 LE";
            break;
        case io::TextEncoding::utf16_be:
            result["encoding"] = "UTF-16 BE";
            break;
        case io::TextEncoding::latin1:
            result["encoding"] = "Latin-1";
            break;
        }
    }
    return result;
}

void ApplicationService::prune_operations() {
    // The bounded result cache retains the newest completed operations, even
    // when their decimal identities cross digit boundaries. Pending work is
    // never a reclamation candidate.
    while (operations_.size() >= 32) {
        auto oldest = operations_.end();
        for (auto candidate = operations_.begin(); candidate != operations_.end(); ++candidate)
            if (candidate->second->state != "pending" &&
                (oldest == operations_.end() ||
                 candidate->second->sequence < oldest->second->sequence))
                oldest = candidate;
        if (oldest == operations_.end())
            return;
        operations_.erase(oldest);
    }
}

Json ApplicationService::start_operation(
    std::string kind,
    std::function<void(const std::shared_ptr<Operation>&)> worker,
    std::function<Json(const std::shared_ptr<Operation>&)> complete,
    bool git_mutation) {
    poll_operations();
    std::size_t active = 0;
    for (const auto& [id, op] : operations_) {
        static_cast<void>(id);
        active += op->state == "pending" ? 1U : 0U;
    }
    if (active >= 8) {
        throw RpcError(-32004, "At most eight workspace operations may run concurrently");
    }
    prune_operations();
    if (operations_.size() >= 32)
        throw RpcError(-32004, "Operation capacity exceeded");
    if (next_operation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw RpcError(-32004, "Operation ID capacity exceeded");
    }
    auto operation = std::make_shared<Operation>();
    operation->sequence = next_operation_;
    operation->id = std::to_string(next_operation_++);
    operation->kind = std::move(kind);
    operation->workspace_mutation = operation->kind == "replace" || git_mutation;
    operations_.emplace(operation->id, operation);
    try {
        if (git_mutation) {
            git_mutation_ = operation->id;
        }
        operation->future.emplace(scheduler_(
            [operation, worker = std::move(worker)] {
                worker(operation);
            },
            [this, operation, complete = std::move(complete)](std::exception_ptr error) {
                if (!error) {
                    try {
                        operation->result = complete(operation);
                    } catch (...) {
                        error = std::current_exception();
                    }
                }
                finish_operation(operation, error);
            }));
    } catch (...) {
        finish_operation(operation, std::current_exception());
    }
    poll_operations();
    return {{"operationId", operation->id}};
}

void ApplicationService::finish_operation(const std::shared_ptr<Operation>& operation,
                                          std::exception_ptr error) {
    if (operation->state != "pending") {
        return;
    }
    finish_recovery_operation(operation, error);
    operation->state =
        error ? (operation->cancellation.stop_requested() ? "cancelled" : "failed") : "completed";
    if (error) {
        operation->error = exception_json(error);
    }
    for (auto i = saving_.begin(); i != saving_.end();) {
        if (i->second == operation->id) {
            i = saving_.erase(i);
        } else {
            ++i;
        }
    }
    if (operation->workspace_mutation) {
        // Reads started before a mutation were allowed onto workers. They may
        // still hold the old bytes even after the busy flag is cleared. Mark
        // them before publishing completion so neither callback order can
        // create a clean buffer from a superseded disk snapshot.
        for (const auto& [id, pending] : operations_) {
            static_cast<void>(id);
            if (pending->kind == "open" && pending->state == "pending") {
                pending->open_invalidated = true;
            }
        }
        if (operation->kind == "replace") {
            replacing_ = false;
        }
    }
    if (git_mutation_ && *git_mutation_ == operation->id) {
        git_mutation_.reset();
    }
    // Results remain queryable after bounded notification overflow.
    if (notifications_.size() >= 1024) {
        notifications_.clear();
        notifications_.push_back(
            protocol::make_notification("transport.resyncRequired", Json::object()));
    }
    notifications_.push_back(protocol::make_notification(
        "operation.completed",
        {{"id", operation->id}, {"kind", operation->kind}, {"state", operation->state}}));
}

void ApplicationService::poll_operations() {
    for (const auto& [id, operation] : operations_) {
        static_cast<void>(id);
        if (operation->future &&
            operation->future->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                operation->future->get();
            } catch (...) {
                finish_operation(operation, std::current_exception());
            }
            operation->future.reset();
        }
    }
}

Json ApplicationService::operation_get(const Json& params) {
    fields(params, {"id", "offset", "limit"});
    const auto found = operations_.find(string_field(params, "id"));
    if (found == operations_.end()) {
        throw RpcError(-32002, "Operation expired or does not exist");
    }
    const auto& operation = *found->second;
    Json result{{"id", operation.id}, {"kind", operation.kind}, {"state", operation.state}};
    if (!operation.error.is_null()) {
        result["error"] = operation.error;
    }
    if (operation.state != "completed") {
        return result;
    }
    const auto offset = number(params, "offset", 0, std::numeric_limits<std::size_t>::max());
    const auto limit = number(params, "limit", 256, 1024);
    if (!limit) {
        invalid("Page limit must be positive");
    }
    if (!operation.result.is_object()) {
        result["result"] = operation.result;
        return result;
    }
    result["result"] = Json::object();
    for (auto field = operation.result.begin(); field != operation.result.end(); ++field) {
        if (field.key() != "entries" && field.key() != "matches") {
            result["result"][field.key()] = field.value();
        }
    }
    for (const auto field : {"entries", "matches"}) {
        if (operation.result.contains(field)) {
            const auto& values = operation.result[field];
            if (offset > values.size()) {
                invalid("Page offset is past the result");
            }
            const auto end = offset + std::min(limit, values.size() - offset);
            result["result"][field] = Json::array();
            for (auto i = offset; i < end; ++i) {
                result["result"][field].push_back(values[i]);
            }
            result["page"] = {{"offset", offset}, {"nextOffset", end}, {"total", values.size()}};
        }
    }
    return result;
}

Json ApplicationService::operation_cancel(const Json& params) {
    fields(params, {"id"});
    const auto found = operations_.find(string_field(params, "id"));
    if (found == operations_.end()) {
        throw RpcError(-32002, "Operation expired or does not exist");
    }
    found->second->cancellation.request_stop();
    if (const auto task = script_tasks_.find(found->first); task != script_tasks_.end()) {
        script_->cancel(task->second);
    }
    return {{"id", found->first}, {"state", found->second->state}, {"cancelRequested", true}};
}

Json ApplicationService::open_file(const Json& params) {
    fields(params, {"path"});
    const auto path = parse_path(string_field(params, "path"));
    if (replacing_ || git_mutation_) {
        throw RpcError(-32013, "Wait for workspace mutation to finish before opening files");
    }
    auto loaded = std::make_shared<std::optional<workspace::LoadedFile>>();
    return start_operation(
        "open",
        [workspace = workspace_, path, loaded](const auto& operation) {
            if (operation->cancellation.stop_requested()) {
                throw std::runtime_error("Open cancelled before reading");
            }
            *loaded = workspace->load(path);
        },
        [this, loaded](const auto& operation) {
            if (replacing_ || git_mutation_ || operation->open_invalidated) {
                throw RpcError(-32013,
                               "Workspace mutation crossed this file read; open the file again");
            }
            auto& file = loaded->value();
            for (const auto& [id, binding] : files_) {
                if (same_path(binding.version.path, file.snapshot.version.path)) {
                    const auto existing = editor_.documents_.find(id);
                    if (existing != editor_.documents_.end()) {
                        return describe(existing->second);
                    }
                }
            }
            const auto metadata = editor_.create({{"text", file.snapshot.text}});
            const auto id = std::stoull(metadata["id"].template get<std::string>());
            files_.emplace(id,
                           FileBinding{file.relative_path,
                                       std::move(file.snapshot.version),
                                       file.snapshot.encoding,
                                       std::move(file.config)});
            auto result = describe(editor_.documents_.at(id));
            result["issues"] = issues_json(file.issues);
            return result;
        });
}

void ApplicationService::reconcile_saved(BufferId id,
                                         const DocumentSnapshot& snapshot,
                                         const workspace::SavedFile& saved) {
    const auto found = editor_.documents_.find(id.value);
    if (found == editor_.documents_.end()) {
        return;
    }
    auto& document = found->second;
    if (document.content_identity() == snapshot.content_identity) {
        auto edits = formatting_edit(document.text(), saved.text);
        if (!edits.empty()) {
            static_cast<void>(
                document.apply({document.revision(), std::move(edits), std::nullopt}));
        }
        document.mark_saved();
    } else if (snapshot.text.str() == saved.text) {
        document.mark_saved(snapshot);
    } else {
        document.invalidate_saved_state();
    }
    files_.insert_or_assign(
        id.value, FileBinding{saved.relative_path, saved.version, saved.encoding, saved.config});
    if (editor_.notifications_.size() >= EditorService::max_pending_notifications) {
        editor_.notifications_.clear();
        notifications_.push_back(
            protocol::make_notification("transport.resyncRequired", Json::object()));
        for (auto binding = files_.begin(); binding != files_.end();) {
            if (!editor_.documents_.contains(binding->first)) {
                binding = files_.erase(binding);
            } else {
                ++binding;
            }
        }
    }
    editor_.changed(document, "saved");
}

Json ApplicationService::save_file(const Json& params) {
    fields(params, {"id", "baseRevision", "path"});
    auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    if (replacing_ || git_mutation_ || saving_.contains(document.id().value)) {
        throw RpcError(-32013, "A save or workspace mutation is already running");
    }
    const auto binding = files_.find(document.id().value);
    const auto path = params.contains("path")   ? parse_path(string_field(params, "path"))
                      : binding != files_.end() ? binding->second.path
                                                : fs::path{};
    if (path.empty()) {
        invalid("Untitled documents require a save path");
    }
    for (const auto& [id, bound] : files_) {
        if (id != document.id().value && editor_.documents_.contains(id) &&
            same_path(bound.path, path)) {
            throw RpcError(-32013, "That path is already open in another document");
        }
    }
    const auto snapshot = document.snapshot();
    const auto expected = binding != files_.end() && binding->second.version.raw_content &&
                                  same_path(binding->second.path, path)
                              ? std::optional<io::FileVersion>(binding->second.version)
                              : std::nullopt;
    const auto encoding =
        binding != files_.end() ? binding->second.encoding : io::TextEncoding::utf8;
    auto saved = std::make_shared<std::optional<workspace::SavedFile>>();
    const auto result = start_operation(
        "save",
        [workspace = workspace_, path, snapshot, expected, encoding, saved](const auto& operation) {
            if (operation->cancellation.stop_requested()) {
                throw std::runtime_error("Save cancelled before writing");
            }
            *saved = workspace->save(path, snapshot.text.str(), expected, encoding);
        },
        [this, snapshot, saved](const auto&) {
            reconcile_saved(snapshot.id, snapshot, saved->value());
            const auto found = editor_.documents_.find(snapshot.id.value);
            auto result = found != editor_.documents_.end()
                              ? describe(found->second)
                              : Json{{"id", std::to_string(snapshot.id.value)},
                                     {"closed", true},
                                     {"path", path_text(saved->value().relative_path)}};
            result["issues"] = issues_json(saved->value().issues);
            return result;
        });
    const auto operation_id = result["operationId"].get<std::string>();
    if (operations_.at(operation_id)->state == "pending") {
        saving_[snapshot.id.value] = operation_id;
    }
    return result;
}

Json ApplicationService::tree(const Json& params) {
    fields(params, {"includeIgnored", "maxEntries"});
    workspace::TreeOptions options;
    options.include_ignored = flag(params, "includeIgnored");
    options.max_entries = number(params, "maxEntries", 10000, 100000);
    auto result = std::make_shared<Json>();
    return start_operation(
        "tree",
        [workspace = workspace_, options, result](const auto& operation) {
            const auto tree_result = workspace->tree(options, operation->cancellation.get_token());
            auto entries = Json::array();
            for (const auto& entry : tree_result.entries) {
                entries.push_back({{"path", path_text(entry.relative_path)},
                                   {"directory", entry.directory},
                                   {"symlink", entry.symlink},
                                   {"sizeBytes", entry.size_bytes}});
            }
            *result = {{"entries", std::move(entries)},
                       {"issues", issues_json(tree_result.issues)},
                       {"truncated", tree_result.truncated},
                       {"cancelled", tree_result.cancelled}};
        },
        [result](const auto&) {
            return std::move(*result);
        });
}

Json ApplicationService::search_disk(const Json& params) {
    fields(params,
           {"query", "wholeWord", "includeIgnored", "maxMatches", "caseSensitive", "regex"});
    workspace::SearchOptions options;
    options.query = string_field(params, "query");
    options.whole_word = flag(params, "wholeWord");
    options.case_sensitive = flag(params, "caseSensitive", true);
    options.regex = flag(params, "regex");
    options.tree.include_ignored = flag(params, "includeIgnored");
    options.max_matches = number(params, "maxMatches", 10000, 100000);
    return start_operation(
        "search",
        [workspace = workspace_, options](const auto& operation) {
            operation->search = std::make_shared<workspace::SearchResult>(
                workspace->search(options, operation->cancellation.get_token()));
        },
        [](const auto& operation) {
            const auto& search_result = *operation->search;
            auto matches = Json::array();
            for (const auto& match : search_result.matches) {
                matches.push_back({{"path", path_text(match.relative_path)},
                                   {"begin", match.begin.value},
                                   {"end", match.end.value},
                                   {"line", match.start_position.line},
                                   {"utf16Column", match.start_position.utf16_column}});
            }
            return Json{{"query", search_result.query},
                        {"matches", std::move(matches)},
                        {"issues", issues_json(search_result.issues)},
                        {"truncated", search_result.truncated},
                        {"cancelled", search_result.cancelled},
                        {"scope", "disk"}};
        });
}

Json ApplicationService::replace_disk(const Json& params) {
    fields(params, {"searchId", "replacement"});
    if (replacing_ || git_mutation_ || !saving_.empty()) {
        throw RpcError(-32013, "Wait for active file mutations to finish");
    }
    const auto found = operations_.find(string_field(params, "searchId"));
    if (found == operations_.end() || found->second->state != "completed" ||
        !found->second->search) {
        throw RpcError(-32002, "A completed search operation is required");
    }
    const auto search_result = found->second->search;
    if (search_result->cancelled || search_result->truncated) {
        throw RpcError(-32013, "Refine an incomplete search before replacing");
    }
    const auto replacement = string_field(params, "replacement");
    std::map<std::uint64_t, DocumentSnapshot> snapshots;
    for (const auto& match : search_result->matches) {
        for (const auto& [id, binding] : files_) {
            const auto document = editor_.documents_.find(id);
            if (document != editor_.documents_.end() &&
                (same_path(binding.path, match.relative_path) ||
                 same_path(binding.version.path, match.version.path))) {
                if (document->second.dirty()) {
                    throw RpcError(-32013,
                                   "Save modified matching documents before disk replacement");
                }
                snapshots.try_emplace(id, document->second.snapshot());
            }
        }
    }
    auto applied = std::make_shared<workspace::ReplaceResult>();
    const auto result = start_operation(
        "replace",
        [workspace = workspace_, search_result, replacement, applied](const auto& operation) {
            const auto plan = workspace->plan_replace(
                *search_result, replacement, operation->cancellation.get_token());
            *applied = workspace->apply_replace(plan, operation->cancellation.get_token());
        },
        [this, snapshots = std::move(snapshots), applied](const auto&) {
            auto paths = Json::array();
            for (const auto& file : applied->applied) {
                paths.push_back(path_text(file.relative_path));
                for (const auto& [id, snapshot] : snapshots) {
                    const auto binding = files_.find(id);
                    if (binding != files_.end() &&
                        (same_path(binding->second.path, file.relative_path) ||
                         same_path(binding->second.version.path, file.version.path))) {
                        reconcile_saved(BufferId{id}, snapshot, file);
                    }
                }
            }
            auto pending = Json::array();
            for (const auto& path : applied->pending) {
                pending.push_back(path_text(path));
            }
            return Json{{"applied", std::move(paths)},
                        {"pending", std::move(pending)},
                        {"issues", issues_json(applied->issues)},
                        {"cancelled", applied->cancelled}};
        });
    if (operations_.at(result["operationId"].get<std::string>())->state == "pending") {
        replacing_ = true;
    }
    return result;
}
} // namespace astra::service
