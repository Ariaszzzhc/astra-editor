#include <astra/service/application_service.hpp>
#include <astra/language/lsp.hpp>
#include <astra/core/utf8.hpp>
#include <astra/transport/tcp_server.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <type_traits>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace astra::service {
namespace {
using protocol::Json;
using protocol::RpcError;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

[[noreturn]] void invalid(std::string message) {
    throw RpcError(-32602, std::move(message));
}

void fields(const Json& params, std::initializer_list<std::string_view> allowed) {
    if (!params.is_object()) {
        invalid("Named language parameters are required");
    }
    for (auto item = params.begin(); item != params.end(); ++item) {
        if (std::find(allowed.begin(), allowed.end(), item.key()) == allowed.end()) {
            invalid("Unexpected language parameter: " + item.key());
        }
    }
}

const std::string& text(const Json& value, std::string_view name) {
    const auto found = value.find(std::string(name));
    if (found == value.end() || !found->is_string()) {
        invalid("Expected string: " + std::string(name));
    }
    return found->get_ref<const std::string&>();
}

std::size_t integer(const Json& value, std::string_view name) {
    const auto found = value.find(std::string(name));
    if (found == value.end() || !found->is_number_integer() ||
        (!found->is_number_unsigned() && found->get<std::int64_t>() < 0) ||
        found->get<std::uint64_t>() > 9007199254740991ULL) {
        invalid("Expected nonnegative safe integer: " + std::string(name));
    }
    return found->get<std::size_t>();
}

std::string path_text(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

fs::path utf_path(const std::string& value) {
    utf8::validate(value);
    if (value.find('\0') != value.npos) {
        invalid("NUL in language server path");
    }
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

bool equal_path(const fs::path& left, const fs::path& right) {
#if defined(_WIN32)
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

std::string language_name(const fs::path& path) {
    auto extension = path_text(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
        return static_cast<char>(value >= 'A' && value <= 'Z' ? value - 'A' + 'a' : value);
    });
    if (extension == ".c") {
        return "c";
    }
    if (extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".h" ||
        extension == ".hpp" || extension == ".hxx" || extension == ".hh") {
        return "cpp";
    }
    if (extension == ".js" || extension == ".jsx" || extension == ".mjs") {
        return "javascript";
    }
    if (extension == ".ts" || extension == ".tsx") {
        return "typescript";
    }
    if (extension == ".py" || extension == ".pyi") {
        return "python";
    }
    return {};
}

fs::path relative_uri(const fs::path& root, std::string_view uri) {
    const auto absolute = language::file_uri_to_path(uri).lexically_normal();
    const auto normalized_root = root.lexically_normal();
    auto target_component = absolute.begin();
    for (const auto& root_component : normalized_root) {
        if (target_component == absolute.end() || !equal_path(root_component, *target_component)) {
            invalid("Language edit is outside the workspace");
        }
        ++target_component;
    }
    fs::path relative;
    for (; target_component != absolute.end(); ++target_component) {
        relative /= *target_component;
    }
    if (relative.empty() || relative.is_absolute() || relative.has_root_name()) {
        invalid("Language edit is outside the workspace");
    }
    for (const auto& component : relative) {
        if (component == "..") {
            invalid("Language edit is outside the workspace");
        }
    }
    return relative;
}

TextPosition position(const Json& value) {
    if (!value.is_object()) {
        invalid("Expected LSP position object");
    }
    return {integer(value, "line"), integer(value, "character")};
}

std::vector<Edit>
edits_from_lsp(const Rope& rope, const Json& edits, std::string_view mode = "replace") {
    if (!edits.is_array() || edits.size() > EditorService::max_transaction_edits) {
        invalid("Invalid LSP text edits array");
    }
    std::vector<Edit> result;
    result.reserve(edits.size());
    for (const auto& edit : edits) {
        const auto& replacement = text(edit, "newText");
        const auto range = edit.contains("range") ? edit["range"]
                                                  : (mode == "insert" && edit.contains("insert")
                                                         ? edit["insert"]
                                                         : edit.value("replace", Json{}));
        if (!range.is_object() || !range.contains("start") || !range.contains("end")) {
            invalid("LSP text edit has no valid range");
        }
        const auto begin = language::lsp_byte_offset(rope, position(range["start"]));
        const auto end = language::lsp_byte_offset(rope, position(range["end"]));
        if (begin > end) {
            invalid("Reversed LSP text edit range");
        }
        result.push_back({ByteOffset{begin}, ByteOffset{end}, replacement});
    }
    return result;
}

template <class T>
T wait_language(std::future<T> future, std::stop_token stop, std::function<void()> cancel = {}) {
    while (future.wait_for(10ms) != std::future_status::ready) {
        if (stop.stop_requested()) {
            if (cancel) {
                cancel();
            }
            throw transport::TransportCancelled("Language operation cancelled");
        }
    }
    if (stop.stop_requested()) {
        if (cancel) {
            cancel();
        }
        throw transport::TransportCancelled("Language operation cancelled");
    }
    return future.get();
}

struct LanguageInput {
    DocumentSnapshot snapshot;
    fs::path path;
    std::string uri;
    std::string language;
};

struct SyncedDocument {
    DocumentSnapshot snapshot;
    std::string uri;
    std::int32_t version{};
    std::uint64_t highest_content_identity{};
};

struct Session {
    std::string key;
    std::string configured_command;
    std::vector<std::string> arguments;
    fs::path resources;
    fs::path root;
    std::atomic<std::shared_ptr<language::LspClient>> client;
    std::atomic<std::uint64_t> client_epoch{};
    std::mutex worker_mutex;
    std::map<std::uint64_t, SyncedDocument> synced;
    std::mutex published_mutex;
    std::map<std::string, std::tuple<BufferId, Revision, std::int32_t>> published;
    std::atomic<bool> busy{};
    // The service owner alone accesses these scheduling fields.
    std::map<std::uint64_t, std::pair<Revision, std::string>> scheduled;
    bool sync_pending{};
    std::string sync_operation;
    std::string error;
    std::uint64_t generation{};        // Service owner.
    std::uint64_t worker_generation{}; // Protected by worker_mutex.
};

class SessionLock {
  public:
    SessionLock(Session& session, std::stop_token stop)
        : session_(session), lock_(session.worker_mutex, std::defer_lock) {
        while (!lock_.try_lock()) {
            if (stop.stop_requested()) {
                throw transport::TransportCancelled("Language operation cancelled in queue");
            }
            std::this_thread::sleep_for(5ms);
        }
        session_.busy.store(true);
    }

    ~SessionLock() {
        session_.busy.store(false);
    }

  private:
    Session& session_;
    std::unique_lock<std::mutex> lock_;
};

bool superseded(Session& session,
                const std::vector<LanguageInput>& inputs,
                std::uint64_t generation) {
    if (generation >= session.worker_generation) {
        session.worker_generation = generation;
        return false;
    }
    if (inputs.size() != session.synced.size()) {
        return true;
    }
    for (const auto& input : inputs) {
        const auto found = session.synced.find(input.snapshot.id.value);
        if (found == session.synced.end() ||
            found->second.snapshot.revision != input.snapshot.revision ||
            found->second.uri != input.uri) {
            return true;
        }
    }
    return false;
}

std::shared_ptr<language::LspClient>
synchronize(Session& session, const std::vector<LanguageInput>& inputs, std::stop_token stop) {
    auto client = session.client.load();
    if (!client || !client->ready()) {
        language::LspOptions options;
        Json initialization = Json::object();
        auto command = utf_path(session.configured_command);
        if (command == "clangd" || command == "clangd.exe") {
#if defined(_WIN32)
            const fs::path executable = "bin/clangd.exe";
#else
            const fs::path executable = "bin/clangd";
#endif
            const std::vector<fs::path> candidates{
                session.resources / "tools/clangd" / executable,
                session.resources / "tools/clangd_21.1.8" / executable,
                session.resources / ".tools/lsp/clangd_21.1.8" / executable,
                session.resources / "clangd_21.1.8" / executable};
            bool found = false;
            if (!session.resources.empty()) {
                for (const auto& candidate : candidates) {
                    if (fs::is_regular_file(candidate)) {
                        command = candidate;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                throw RpcError(-32030,
                               "Bundled clangd is unavailable in the explicit resource directory");
            }
        } else if (command == "typescript-language-server" ||
                   command == "basedpyright-langserver") {
            const bool typescript = command == "typescript-language-server";
            fs::path bundled;
            if (!session.resources.empty()) {
                for (const auto& candidate :
                     {session.resources / "tools", session.resources / ".tools/lsp/tools"}) {
#if defined(_WIN32)
                    const auto executable = candidate / "node/node.exe";
#else
                    const auto executable = candidate / "node/bin/node";
#endif
                    if (fs::is_regular_file(executable)) {
                        command = executable;
                        bundled = candidate;
                        break;
                    }
                }
            }
            if (bundled.empty()) {
                throw RpcError(-32030,
                               "Bundled Node.js is unavailable in the explicit resource directory");
            }
            const auto script =
                bundled / (typescript ? "node_modules/typescript-language-server/lib/cli.mjs"
                                      : "node_modules/basedpyright/langserver.index.js");
            if (!fs::is_regular_file(script)) {
                throw RpcError(-32030, "Bundled language server package is unavailable");
            }
            options.argv = {path_text(command), path_text(script)};
            if (typescript) {
                const auto server = bundled / "node_modules/typescript/lib/tsserver.js";
                if (!fs::is_regular_file(server)) {
                    throw RpcError(-32030, "Bundled TypeScript tsserver is unavailable");
                }
                initialization = {{"hostInfo", "Astra"},
                                  {"disableAutomaticTypingAcquisition", true},
                                  {"tsserver",
                                   {{"path", path_text(server)},
                                    {"useSyntaxServer", "never"},
                                    {"logVerbosity", "off"}}},
                                  {"preferences",
                                   {{"includeCompletionsWithSnippetText", false},
                                    {"includeCompletionsWithClassMemberSnippets", false},
                                    {"includeCompletionsWithObjectLiteralMethodSnippets", false}}}};
            }
        } else if (!command.is_absolute()) {
            if (!command.has_parent_path()) {
                throw RpcError(
                    -32030,
                    "Language server requires a bundled server or an explicit executable path");
            }
            command = session.root / command;
        }
        if (options.argv.empty()) {
            options.argv.push_back(path_text(command));
        }
        options.argv.insert(options.argv.end(), session.arguments.begin(), session.arguments.end());
        options.working_directory = path_text(session.root);
        client = std::make_shared<language::LspClient>(std::move(options));
        ++session.client_epoch;
        session.client.store(client);
        session.synced.clear();
        (void)wait_language(
            client->start(language::path_to_file_uri(session.root), std::move(initialization)),
            stop,
            [client] {
                client->close();
            });
    }
    std::set<std::uint64_t> alive;
    for (const auto& input : inputs) {
        if (stop.stop_requested()) {
            throw transport::TransportCancelled("Language synchronization cancelled");
        }
        const auto id = input.snapshot.id.value;
        alive.insert(id);
        auto found = session.synced.find(id);
        if (found != session.synced.end() &&
            (found->second.uri != input.uri ||
             found->second.version == std::numeric_limits<std::int32_t>::max())) {
            wait_language(client->did_close(found->second.uri), stop);
            session.synced.erase(found);
            found = session.synced.end();
        }
        if (found == session.synced.end()) {
            wait_language(client->did_open(input.uri, input.language, 0, input.snapshot.text.str()),
                          stop);
            found = session.synced
                        .emplace(id,
                                 SyncedDocument{input.snapshot,
                                                input.uri,
                                                0,
                                                input.snapshot.content_identity.value})
                        .first;
        } else if (found->second.snapshot.revision != input.snapshot.revision) {
            const auto& before = found->second.snapshot.text;
            const auto& after = input.snapshot.text;
            const auto extent = before.changed_extent(after);
            if (extent.begin == extent.old_end && extent.begin == extent.new_end) {
                // Revision changes can cancel each other before reaching LSP
                // (notably edit+undo). The server already has this exact text.
                // Raising its version with an empty change can invalidate the
                // last diagnostics without causing servers to publish again.
                found->second.snapshot = input.snapshot;
                found->second.highest_content_identity = std::max(
                    found->second.highest_content_identity, input.snapshot.content_identity.value);
                continue;
            }
            auto begin = extent.begin;
            auto old_end = extent.old_end;
            auto new_end = extent.new_end;
            const auto inside_crlf = [](const Rope& rope, std::size_t offset) {
                return offset != 0 && offset < rope.size_bytes() && rope.is_boundary(offset - 1) &&
                       rope.is_boundary(offset + 1) && rope.slice(offset - 1, offset + 1) == "\r\n";
            };
            // Only widen an endpoint actually between CR and LF. Expanding to a
            // complete logical line would resend megabytes for a one-byte edit
            // in a long line. Unchanged prefix/suffix bytes are shared by both.
            if (inside_crlf(before, begin) || inside_crlf(after, begin)) {
                --begin;
            }
            if (inside_crlf(before, old_end) || inside_crlf(after, new_end)) {
                ++old_end;
                ++new_end;
            }
            language::LspRange range{found->second.snapshot.text.byte_to_position(begin),
                                     found->second.snapshot.text.byte_to_position(old_end)};
            const auto name = utf_path(session.configured_command).filename();
            const bool clangd = name == "clangd" || name == "clangd.exe";
            const bool historical_content =
                input.snapshot.content_identity.value <= found->second.highest_content_identity;
            // clangd may coalesce an edit followed by undo back into its cached
            // AST and never publish diagnostics for the newer LSP version. Keep
            // strict version matching and rebuild only on history restoration.
            wait_language(client->did_change(input.uri,
                                             found->second.version + 1,
                                             {{range, input.snapshot.text.slice(begin, new_end)}},
                                             {clangd && historical_content}),
                          stop);
            found->second = SyncedDocument{input.snapshot,
                                           input.uri,
                                           found->second.version + 1,
                                           std::max(found->second.highest_content_identity,
                                                    input.snapshot.content_identity.value)};
        }
    }
    for (auto item = session.synced.begin(); item != session.synced.end();) {
        if (!alive.contains(item->first)) {
            wait_language(client->did_close(item->second.uri), stop);
            item = session.synced.erase(item);
        } else {
            ++item;
        }
    }
    {
        const std::lock_guard lock(session.published_mutex);
        session.published.clear();
        for (const auto& [id, document] : session.synced) {
            session.published[document.uri] = {
                BufferId{id}, document.snapshot.revision, document.version};
        }
    }
    return client;
}

struct PlannedFile {
    fs::path path;
    std::string uri;
    std::optional<DocumentSnapshot> open;
    std::optional<workspace::LoadedFile> closed;
    std::vector<Edit> edits;
};

struct EditPlan {
    std::string id;
    BufferId origin;
    Revision revision;
    std::vector<PlannedFile> files;
    std::size_t retained_bytes{};
};

Json plan_description(const EditPlan& plan) {
    Json documents = Json::array();
    for (const auto& file : plan.files) {
        Json item{{"path", path_text(file.path)}, {"editCount", file.edits.size()}};
        if (file.open) {
            item["id"] = std::to_string(file.open->id.value);
            item["baseRevision"] = std::to_string(file.open->revision.value);
        }
        documents.push_back(std::move(item));
    }
    return {{"planId", plan.id}, {"documents", std::move(documents)}};
}

std::shared_ptr<EditPlan> build_plan(const Json& edit,
                                     const std::vector<LanguageInput>& inputs,
                                     const std::map<std::string, workspace::LoadedFile>& disk,
                                     Session& session,
                                     BufferId origin,
                                     Revision revision) {
    if (!edit.is_object()) {
        throw RpcError(-32031, "Language server did not return a WorkspaceEdit");
    }
    std::map<std::string, Json> changes;
    if (edit.contains("changes")) {
        if (!edit["changes"].is_object()) {
            invalid("Invalid WorkspaceEdit changes");
        }
        for (auto entry = edit["changes"].begin(); entry != edit["changes"].end(); ++entry) {
            changes[entry.key()] = entry.value();
        }
    }
    if (edit.contains("documentChanges")) {
        if (!changes.empty() || !edit["documentChanges"].is_array()) {
            invalid("Ambiguous WorkspaceEdit changes");
        }
        for (const auto& document : edit["documentChanges"]) {
            if (document.contains("kind") || !document.contains("textDocument") ||
                !document.contains("edits")) {
                throw RpcError(-32031,
                               "File creation/rename/deletion was not advertised for language "
                               "workspace edits");
            }
            const auto& identity = document["textDocument"];
            const auto uri = text(identity, "uri");
            if (changes.contains(uri)) {
                invalid("Duplicate WorkspaceEdit document");
            }
            if (identity.contains("version") && !identity["version"].is_null()) {
                const auto opened = std::find_if(
                    session.synced.begin(), session.synced.end(), [&](const auto& item) {
                        return language::file_uri_equal(item.second.uri, uri);
                    });
                if (opened == session.synced.end() ||
                    identity["version"] != opened->second.version) {
                    throw RpcError(
                        -32001, "WorkspaceEdit targets an unknown or stale LSP document version");
                }
            }
            changes[uri] = document["edits"];
        }
    }
    if (changes.empty() || changes.size() > 256) {
        throw RpcError(-32031, "WorkspaceEdit must edit between one and 256 files");
    }
    auto plan = std::make_shared<EditPlan>();
    plan->origin = origin;
    plan->revision = revision;
    for (const auto& [uri, edits] : changes) {
        PlannedFile file;
        file.uri = uri;
        file.path = relative_uri(session.root, uri);
        if (std::any_of(plan->files.begin(), plan->files.end(), [&](const auto& previous) {
                return equal_path(previous.path, file.path);
            })) {
            invalid("WorkspaceEdit contains duplicate filesystem targets");
        }
        const auto opened = std::find_if(inputs.begin(), inputs.end(), [&](const auto& input) {
            return equal_path(input.path, file.path);
        });
        if (opened != inputs.end()) {
            file.open = opened->snapshot;
            file.edits = edits_from_lsp(opened->snapshot.text, edits);
        } else {
            const auto baseline =
                std::find_if(disk.begin(), disk.end(), [&](const auto& candidate) {
                    return equal_path(candidate.second.relative_path, file.path);
                });
            if (baseline == disk.end()) {
                throw RpcError(-32031,
                               "Rename target has no verified pre-request workspace snapshot");
            }
            file.closed = baseline->second;
            file.edits = edits_from_lsp(Rope(file.closed->snapshot.text), edits);
        }
        // Validate overlap/UTF-8 without materializing an open document's Rope.
        auto ordered = file.edits;
        std::sort(ordered.begin(), ordered.end(), [](const Edit& left, const Edit& right) {
            return left.begin < right.begin;
        });
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            utf8::validate(ordered[index].text);
            if (index != 0 && (ordered[index].begin < ordered[index - 1].end ||
                               ordered[index].begin == ordered[index - 1].begin)) {
                invalid("Overlapping WorkspaceEdit text edits");
            }
        }
        plan->files.push_back(std::move(file));
    }
    for (const auto& file : plan->files) {
        if (file.closed) {
            plan->retained_bytes += file.closed->snapshot.text.size();
            plan->retained_bytes += file.closed->snapshot.version.raw_content->size();
        }
        for (const auto& change : file.edits) {
            plan->retained_bytes += change.text.size();
        }
    }
    if (plan->retained_bytes > 128 * 1024 * 1024) {
        throw RpcError(-32004, "Rename plan exceeds 128 MiB");
    }
    return plan;
}

Json resolve_locations(const Json& result,
                       const std::vector<LanguageInput>& inputs,
                       const workspace::Workspace& workspace) {
    Json locations = Json::array();
    if (result.is_null()) {
        return locations;
    }
    const auto values = result.is_array() ? result : Json::array({result});
    for (const auto& item : values) {
        if (!item.is_object()) {
            continue;
        }
        const auto uri = item.contains("targetUri") ? text(item, "targetUri") : text(item, "uri");
        const auto range = item.contains("targetSelectionRange") ? item["targetSelectionRange"]
                                                                 : item.value("range", Json{});
        if (!range.is_object()) {
            continue;
        }
        const auto start = position(range.at("start"));
        const auto end = position(range.at("end"));
        Json location{{"uri", uri}, {"line", start.line}, {"character", start.utf16_column}};
        fs::path relative;
        try {
            relative = relative_uri(workspace.root(), uri);
        } catch (const RpcError&) {
            location["outsideWorkspace"] = true;
            locations.push_back(std::move(location));
            continue;
        }
        location["path"] = path_text(relative);
        const auto opened = std::find_if(inputs.begin(), inputs.end(), [&](const auto& input) {
            return equal_path(input.path, relative);
        });
        if (opened != inputs.end()) {
            location["byteOffset"] = language::lsp_byte_offset(opened->snapshot.text, start);
            location["endByteOffset"] = language::lsp_byte_offset(opened->snapshot.text, end);
            location["targetId"] = std::to_string(opened->snapshot.id.value);
            location["targetRevision"] = std::to_string(opened->snapshot.revision.value);
        } else {
            const auto loaded = workspace.load(relative);
            const Rope rope(loaded.snapshot.text);
            location["byteOffset"] = language::lsp_byte_offset(rope, start);
            location["endByteOffset"] = language::lsp_byte_offset(rope, end);
        }
        locations.push_back(std::move(location));
    }
    return locations;
}
} // namespace

struct ApplicationService::LanguageState {
    struct CachedDiagnostics {
        std::string uri;
        std::string session_key;
        std::uint64_t client_epoch{};
        std::int32_t lsp_version{};
        Json result;
    };

    fs::path resources;
    std::map<std::string, std::shared_ptr<Session>> sessions;
    std::map<std::uint64_t, CachedDiagnostics> diagnostics;
    std::map<std::string, std::shared_ptr<EditPlan>> plans;
    std::uint64_t next_plan{1};

    std::shared_ptr<Session> session(ApplicationService& app, const std::string& name) {
        std::string command;
        std::vector<std::string> arguments;
        const auto session_key = key(app, name, &command, &arguments);
        if (const auto found = sessions.find(session_key); found != sessions.end()) {
            return found->second;
        }
        if (sessions.size() >= 16) {
            throw RpcError(-32004, "Language session capacity exceeded");
        }
        auto created = std::make_shared<Session>();
        created->key = session_key;
        created->configured_command = std::move(command);
        created->arguments = std::move(arguments);
        created->root = app.workspace_->root();
        created->resources = resources;
        sessions.emplace(session_key, created);
        return created;
    }

    std::string key(ApplicationService& app,
                    const std::string& language,
                    std::string* command = nullptr,
                    std::vector<std::string>* arguments = nullptr) const {
        const auto settings = app.script_->settings();
        if (!settings.contains("languageServers") || !settings["languageServers"].is_object() ||
            !settings["languageServers"].contains(language)) {
            throw RpcError(-32030, "No language server is configured for this document");
        }
        const auto& config = settings["languageServers"][language];
        const auto& executable = text(config, "command");
        const auto args = config.value("arguments", Json::array());
        if (!args.is_array() || args.size() > 128 || executable.size() > 32768) {
            invalid("Invalid language server arguments");
        }
        for (const auto& arg : args) {
            if (!arg.is_string()) {
                invalid("Language server arguments must be strings");
            }
        }
        if (command) {
            *command = executable;
        }
        if (arguments) {
            *arguments = args.get<std::vector<std::string>>();
        }
        return Json::array({executable, args}).dump();
    }

    std::vector<LanguageInput> inputs(ApplicationService& app,
                                      const std::string& session_key) const {
        std::vector<LanguageInput> result;
        for (const auto& [id, document] : app.editor_.documents_) {
            const auto file = app.files_.find(id);
            if (file == app.files_.end()) {
                continue;
            }
            const auto name = language_name(file->second.path);
            if (name.empty()) {
                continue;
            }
            try {
                if (key(app, name) != session_key) {
                    continue;
                }
            } catch (const RpcError&) {
                continue;
            }
            const auto absolute = app.workspace_->root() / file->second.path;
            result.push_back({document.snapshot(),
                              file->second.path,
                              language::path_to_file_uri(absolute),
                              name});
        }
        return result;
    }

    static void remember(Session& session, const std::vector<LanguageInput>& input) {
        session.scheduled.clear();
        for (const auto& item : input) {
            session.scheduled[item.snapshot.id.value] = {item.snapshot.revision, item.uri};
        }
    }
};

void ApplicationService::register_language_methods(const fs::path& resources) {
    language_ = std::make_shared<LanguageState>();
    language_->resources = resources;
    auto& rpc = editor_.dispatcher();
    rpc.register_method("language.request", [this](const Json& p) {
        return language_request(p);
    });
    rpc.register_method("language.diagnostics", [this](const Json& p) {
        return language_diagnostics(p);
    });
    rpc.register_method("language.applyTextEdits", [this](const Json& p) {
        return language_apply_text_edits(p);
    });
    rpc.register_method("language.applyCompletion", [this](const Json& p) {
        return language_apply_completion(p);
    });
    rpc.register_method("language.prepareWorkspaceEdit", [this](const Json& p) {
        return language_prepare_workspace_edit(p);
    });
    rpc.register_method("language.applyWorkspaceEdit", [this](const Json& p) {
        return language_apply_workspace_edit(p);
    });
}

void ApplicationService::poll_language() {
    if (!language_) {
        return;
    }
    const auto announce_diagnostics = [this](std::uint64_t id, const Json& diagnostics) {
        const auto identity = std::to_string(id);
        auto notification =
            protocol::make_notification("language.diagnosticsChanged",
                                        {{"id", identity},
                                         {"revision", diagnostics["revision"]},
                                         {"count", diagnostics["diagnostics"].size()}});
        const auto queued =
            std::find_if(notifications_.begin(), notifications_.end(), [&](const Json& event) {
                return event.value("method", std::string{}) == "language.diagnosticsChanged" &&
                       event["params"].value("id", std::string{}) == identity;
            });
        if (queued != notifications_.end()) {
            *queued = std::move(notification);
            return;
        }
        if (notifications_.size() >= 1024) {
            notifications_.clear();
            notifications_.push_back(
                protocol::make_notification("transport.resyncRequired", Json::object()));
        }
        notifications_.push_back(std::move(notification));
    };
    std::erase_if(language_->diagnostics, [this](const auto& entry) {
        return !editor_.documents_.contains(entry.first) || !files_.contains(entry.first);
    });
    for (const auto& [key, session] : language_->sessions) {
        if (session->sync_pending && !session->sync_operation.empty()) {
            const auto found = operations_.find(session->sync_operation);
            if (found == operations_.end() || found->second->state != "pending") {
                session->sync_pending = false;
                if (found != operations_.end() && found->second->state == "failed") {
                    session->error = found->second->error.dump();
                }
            }
        }
        std::unique_lock diagnostic_lock(session->worker_mutex, std::try_to_lock);
        if (auto client = session->client.load(); client && diagnostic_lock.owns_lock()) {
            for (const auto& diagnostic : client->take_diagnostics()) {
                const std::lock_guard lock(session->published_mutex);
                const auto stamp = std::find_if(session->published.begin(),
                                                session->published.end(),
                                                [&](const auto& published) {
                                                    return language::file_uri_equal(published.first,
                                                                                    diagnostic.uri);
                                                });
                if (stamp == session->published.end()) {
                    continue;
                }
                const auto [id, revision, version] = stamp->second;
                const auto document = editor_.documents_.find(id.value);
                const auto binding = files_.find(id.value);
                if (document == editor_.documents_.end() || binding == files_.end() ||
                    !language::file_uri_equal(
                        diagnostic.uri,
                        language::path_to_file_uri(workspace_->root() / binding->second.path)) ||
                    (diagnostic.version && *diagnostic.version != version) ||
                    (diagnostic.observed_version && *diagnostic.observed_version != version)) {
                    continue;
                }
                try {
                    if (language_->key(*this, language_name(binding->second.path)) != key) {
                        continue;
                    }
                } catch (const RpcError&) {
                    continue;
                }
                Json result{{"id", std::to_string(id.value)},
                            {"revision", std::to_string(revision.value)},
                            {"diagnostics", diagnostic.diagnostics},
                            {"versioned", diagnostic.version.has_value()},
                            {"available", true},
                            {"pending", false}};
                const auto previous = language_->diagnostics.find(id.value);
                const bool changed =
                    previous == language_->diagnostics.end() || previous->second.result != result ||
                    previous->second.client_epoch != session->client_epoch.load() ||
                    previous->second.session_key != key ||
                    !language::file_uri_equal(previous->second.uri, diagnostic.uri);
                language_->diagnostics[id.value] = {
                    diagnostic.uri, key, session->client_epoch.load(), version, std::move(result)};
                // Retain diagnostics for the precisely published LSP snapshot
                // even while the owner document is ahead. Otherwise edit+undo
                // can drain and discard the sole initial result before a no-op
                // synchronization proves that the original text is current.
                // The getter and rebinding below still require an exact current
                // revision; stale results are neither exposed nor announced.
                if (changed && document->second.revision() == revision) {
                    announce_diagnostics(id.value, language_->diagnostics.at(id.value).result);
                }
            }
            for (const auto& event : client->take_notifications()) {
                if (event.value("method", std::string{}) == "language.resyncRequired") {
                    language_->diagnostics.clear();
                }
            }
            const std::lock_guard published_lock(session->published_mutex);
            for (auto& [id, cached] : language_->diagnostics) {
                if (cached.session_key != key ||
                    cached.client_epoch != session->client_epoch.load()) {
                    continue;
                }
                const auto stamp = session->published.find(cached.uri);
                const auto document = editor_.documents_.find(id);
                const auto binding = files_.find(id);
                if (stamp == session->published.end() || document == editor_.documents_.end() ||
                    binding == files_.end()) {
                    continue;
                }
                const auto [published_id, revision, version] = stamp->second;
                if (published_id.value == id && version == cached.lsp_version &&
                    document->second.revision() == revision &&
                    language::file_uri_equal(
                        cached.uri,
                        language::path_to_file_uri(workspace_->root() / binding->second.path))) {
                    // Within one server instance and URI, an unchanged LSP
                    // version proves no text update occurred. Only then may a
                    // cached diagnostic follow the newer document revision.
                    const auto current_revision = std::to_string(revision.value);
                    if (cached.result["revision"] != current_revision) {
                        cached.result["revision"] = current_revision;
                        announce_diagnostics(id, cached.result);
                    }
                }
            }
        }
        if (diagnostic_lock.owns_lock()) {
            diagnostic_lock.unlock();
        }
        if (session->sync_pending) {
            continue;
        }
        const auto input = language_->inputs(*this, key);
        std::map<std::uint64_t, std::pair<Revision, std::string>> current;
        for (const auto& item : input) {
            current[item.snapshot.id.value] = {item.snapshot.revision, item.uri};
        }
        if (current == session->scheduled) {
            continue;
        }
        try {
            session->sync_pending = true;
            const auto generation = ++session->generation;
            const auto op = start_operation(
                "language.sync",
                [session, input, generation](const auto& operation) {
                    SessionLock lock(*session, operation->cancellation.get_token());
                    if (!superseded(*session, input, generation)) {
                        (void)synchronize(*session, input, operation->cancellation.get_token());
                    }
                },
                [session](const auto&) {
                    session->sync_pending = false;
                    session->error.clear();
                    return Json{{"synchronized", true}};
                });
            session->sync_operation = op["operationId"].get<std::string>();
            LanguageState::remember(*session, input);
        } catch (const RpcError& error) {
            session->sync_pending = false;
            if (error.code() != -32004) {
                session->error = error.what();
            }
            // Capacity exhaustion defers synchronization until the next owner poll.
        } catch (const std::exception& error) {
            session->sync_pending = false;
            session->error = error.what();
        }
    }
}

Json ApplicationService::language_diagnostics(const Json& params) {
    fields(params, {"id"});
    const auto& document = editor_.find_document(params);
    Json status{{"id", std::to_string(document.id().value)},
                {"revision", std::to_string(document.revision().value)},
                {"diagnostics", Json::array()},
                {"available", false},
                {"pending", false}};
    const auto bound = files_.find(document.id().value);
    if (bound == files_.end()) {
        status["reason"] = "Document has no workspace path";
        return status;
    }
    const auto name = language_name(bound->second.path);
    if (name.empty()) {
        status["reason"] = "No language is associated with this extension";
        return status;
    }
    std::shared_ptr<Session> session;
    try {
        session = language_->session(*this, name);
    } catch (const RpcError& error) {
        status["reason"] = error.what();
        return status;
    }
    if (!session->error.empty()) {
        status["reason"] = "Language server is unavailable";
        status["error"] = session->error;
        return status;
    }
    status["available"] = true;
    status["pending"] = true;
    const auto found = language_->diagnostics.find(document.id().value);
    if (found != language_->diagnostics.end() &&
        found->second.result["revision"] == std::to_string(document.revision().value) &&
        found->second.session_key == session->key &&
        language::file_uri_equal(
            found->second.uri,
            language::path_to_file_uri(workspace_->root() / bound->second.path)) &&
        found->second.client_epoch == session->client_epoch.load()) {
        return found->second.result;
    }
    return status;
}

Json ApplicationService::language_apply_text_edits(const Json& params) {
    fields(params, {"id", "baseRevision", "edits", "additionalTextEdits", "editMode"});
    auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    const auto mode = params.value("editMode", std::string("replace"));
    if (mode != "insert" && mode != "replace") {
        invalid("Invalid completion edit mode");
    }
    auto changes = edits_from_lsp(document.text(), params.value("edits", Json::array()), mode);
    auto additional =
        edits_from_lsp(document.text(), params.value("additionalTextEdits", Json::array()));
    changes.insert(changes.end(),
                   std::make_move_iterator(additional.begin()),
                   std::make_move_iterator(additional.end()));
    if (changes.size() > EditorService::max_transaction_edits) {
        invalid("Too many language edits");
    }
    editor_.require_notification_capacity();
    (void)document.apply({document.revision(), std::move(changes), std::nullopt});
    editor_.changed(document, "language");
    return describe(document);
}

Json ApplicationService::language_apply_completion(const Json& params) {
    fields(params, {"id", "baseRevision", "completionItem", "byteOffset", "editMode"});
    auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    const auto offset = integer(params, "byteOffset");
    if (!document.text().is_boundary(offset)) {
        invalid("Completion byte offset is not a UTF-8 boundary");
    }
    const auto item = params.value("completionItem", Json{});
    if (!item.is_object()) {
        invalid("Expected completion item");
    }
    if (item.value("insertTextFormat", 1) != 1) {
        throw RpcError(
            -32031,
            "Snippet completion requires snippet support; this session advertises plain text only");
    }
    Json edit;
    if (item.contains("textEdit")) {
        edit = item["textEdit"];
    } else {
        const auto insertion =
            item.contains("insertText") ? text(item, "insertText") : text(item, "label");
        const auto line = document.text().byte_to_position(offset).line;
        const auto start = document.text().line_start(line);
        const auto content = document.text().slice(start, document.text().line_end(line));
        auto begin = offset - start;
        if (begin > content.size()) {
            invalid("Completion offset is inside a line ending");
        }
        auto end = begin;
        const auto word = [](char32_t scalar) {
            const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(scalar));
            return (category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_PC) ||
                   scalar == 0x200c || scalar == 0x200d;
        };
        while (begin != 0) {
            auto previous = begin - 1;
            while (!utf8::is_boundary(content, previous)) {
                --previous;
            }
            if (!word(utf8::decode(content, previous).value)) {
                break;
            }
            begin = previous;
        }
        while (end < content.size()) {
            const auto scalar = utf8::decode(content, end);
            if (!word(scalar.value)) {
                break;
            }
            end += scalar.bytes;
        }
        edit = {{"range",
                 {{"start", language::lsp_position(document.text(), start + begin)},
                  {"end", language::lsp_position(document.text(), start + end)}}},
                {"newText", insertion}};
    }
    return language_apply_text_edits(
        {{"id", params["id"]},
         {"baseRevision", params["baseRevision"]},
         {"edits", Json::array({edit})},
         {"additionalTextEdits", item.value("additionalTextEdits", Json::array())},
         {"editMode", params.value("editMode", std::string("replace"))}});
}

// Request/rename operations below retain immutable Rope snapshots; every actual
// document mutation remains on the service owner and is revision checked.

Json ApplicationService::language_request(const Json& params) {
    fields(params,
           {"id",
            "baseRevision",
            "method",
            "byteOffset",
            "newName",
            "includeDeclaration",
            "tabSize",
            "insertSpaces"});
    auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    const auto file = files_.find(document.id().value);
    if (file == files_.end()) {
        throw RpcError(
            -32030, "Save this document to a workspace path before requesting language features");
    }
    const auto method = text(params, "method");
    const std::set<std::string> supported{
        "completion", "hover", "definition", "references", "rename", "formatting"};
    if (!supported.contains(method)) {
        invalid("Unsupported language request method");
    }
    const auto name = language_name(file->second.path);
    if (name.empty()) {
        throw RpcError(-32030, "No language is associated with this file extension");
    }
    const auto snapshot = document.snapshot();
    TextPosition point{};
    if (method != "formatting") {
        point = position(language::lsp_position(snapshot.text, integer(params, "byteOffset")));
    }
    const auto new_name = method == "rename" ? text(params, "newName") : std::string{};
    if (method == "rename" && new_name.empty()) {
        invalid("Rename requires a nonempty symbol name");
    }
    const bool declarations = params.value("includeDeclaration", true);
    const auto tab_size = params.contains("tabSize") ? integer(params, "tabSize") : std::size_t{4};
    if (tab_size == 0 || tab_size > 32) {
        invalid("Formatting tabSize must be between 1 and 32");
    }
    const bool spaces = params.value("insertSpaces", true);
    const auto session = language_->session(*this, name);
    const auto input = language_->inputs(*this, session->key);
    const auto target = std::find_if(input.begin(), input.end(), [&](const auto& value) {
        return value.snapshot.id == snapshot.id;
    });
    if (target == input.end()) {
        throw RpcError(-32030, "Document has no matching language session");
    }
    const auto uri = target->uri;

    struct Result {
        Json value;
        Json locations;
        std::shared_ptr<EditPlan> plan;
        bool superseded{};
    };

    auto result = std::make_shared<Result>();
    const auto generation = ++session->generation;
    const auto started = start_operation(
        "language." + method,
        [session,
         input,
         snapshot,
         uri,
         point,
         method,
         new_name,
         declarations,
         tab_size,
         spaces,
         result,
         generation,
         workspace = workspace_](const auto& operation) {
            const auto stop = operation->cancellation.get_token();
            SessionLock lock(*session, stop);
            if (superseded(*session, input, generation)) {
                result->superseded = true;
                return;
            }
            const auto client = synchronize(*session, input, stop);
            std::map<std::string, workspace::LoadedFile> disk;
            if (method == "rename") {
                const auto tree = workspace->tree(workspace::TreeOptions{10000, 64, false}, stop);
                if (tree.truncated || tree.cancelled) {
                    throw RpcError(-32031, "Rename requires a complete bounded workspace scan");
                }
                std::size_t total{};
                for (const auto& entry : tree.entries) {
                    if (entry.directory || entry.symlink ||
                        language_name(entry.relative_path).empty()) {
                        continue;
                    }
                    if (std::any_of(input.begin(), input.end(), [&](const auto& open) {
                            return open.path == entry.relative_path;
                        })) {
                        continue;
                    }
                    auto loaded = workspace->load(entry.relative_path, 16 * 1024 * 1024);
                    if (loaded.snapshot.text.size() > 64 * 1024 * 1024 - total) {
                        throw RpcError(-32031, "Rename workspace snapshot exceeds 64 MiB");
                    }
                    total += loaded.snapshot.text.size();
                    disk.emplace(path_text(entry.relative_path), std::move(loaded));
                }
            }
            language::LspRequest request;
            if (method == "completion") {
                request = client->completion(uri, point);
            } else if (method == "hover") {
                request = client->hover(uri, point);
            } else if (method == "definition") {
                request = client->definition(uri, point);
            } else if (method == "references") {
                request = client->references(uri, point, declarations);
            } else if (method == "rename") {
                request = client->rename(uri, point, new_name);
            } else {
                request = client->formatting(uri, static_cast<std::uint32_t>(tab_size), spaces);
            }
            const auto request_id = request.id;
            result->value = wait_language(std::move(request.result), stop, [client, request_id] {
                client->cancel(request_id);
            });
            if (method == "definition" || method == "references") {
                result->locations = resolve_locations(result->value, input, *workspace);
            }
            if (method == "rename") {
                result->plan = build_plan(
                    result->value, input, disk, *session, snapshot.id, snapshot.revision);
            }
        },
        [this, result, input, snapshot, method](const auto& operation) {
            if (operation->cancellation.stop_requested()) {
                throw transport::TransportCancelled("Language result cancelled");
            }
            bool obsolete = result->superseded;
            for (const auto& captured : input) {
                const auto current = editor_.documents_.find(captured.snapshot.id.value);
                const auto binding = files_.find(captured.snapshot.id.value);
                if (current == editor_.documents_.end() ||
                    current->second.revision() != captured.snapshot.revision ||
                    binding == files_.end() || !equal_path(binding->second.path, captured.path)) {
                    obsolete = true;
                }
            }
            Json response{{"id", std::to_string(snapshot.id.value)},
                          {"revision", std::to_string(snapshot.revision.value)},
                          {"method", method},
                          {"obsolete", obsolete},
                          {"result", result->value}};
            if (!result->locations.is_null()) {
                response["resolvedLocations"] = result->locations;
            }
            if (result->plan && !obsolete) {
                if (language_->next_plan == std::numeric_limits<std::uint64_t>::max()) {
                    throw RpcError(-32004, "Language plan IDs exhausted");
                }
                std::size_t retained{};
                for (const auto& [id, old] : language_->plans) {
                    (void)id;
                    retained += old->retained_bytes;
                }
                while (!language_->plans.empty() &&
                       (language_->plans.size() >= 16 ||
                        retained > 128 * 1024 * 1024 - result->plan->retained_bytes)) {
                    retained -= language_->plans.begin()->second->retained_bytes;
                    language_->plans.erase(language_->plans.begin());
                }
                result->plan->id = std::to_string(language_->next_plan++);
                language_->plans[result->plan->id] = result->plan;
                const auto description = plan_description(*result->plan);
                response["planId"] = description["planId"];
                response["documents"] = description["documents"];
            }
            return response;
        });
    LanguageState::remember(*session, input);
    return started;
}

Json ApplicationService::language_prepare_workspace_edit(const Json& params) {
    fields(params, {"operationId"});
    const auto operation = operations_.find(text(params, "operationId"));
    if (operation == operations_.end() || operation->second->state != "completed" ||
        operation->second->kind != "language.rename" ||
        !operation->second->result.contains("planId")) {
        throw RpcError(-32002, "No current rename plan belongs to this operation");
    }
    const auto found =
        language_->plans.find(operation->second->result["planId"].get<std::string>());
    if (found == language_->plans.end()) {
        throw RpcError(-32002, "Rename plan expired or was consumed");
    }
    return plan_description(*found->second);
}

Json ApplicationService::language_apply_workspace_edit(const Json& params) {
    fields(params, {"planId"});
    const auto found = language_->plans.find(text(params, "planId"));
    if (found == language_->plans.end()) {
        throw RpcError(-32002, "Rename plan expired or was consumed");
    }
    auto plan = found->second;
    const auto started = start_operation(
        "language.applyWorkspaceEdit",
        [plan, workspace = workspace_](const auto& operation) {
            for (const auto& file : plan->files) {
                if (operation->cancellation.stop_requested()) {
                    throw transport::TransportCancelled("Rename apply cancelled");
                }
                if (file.closed) {
                    const auto current = workspace->load(file.path);
                    if (*current.snapshot.version.raw_content !=
                        *file.closed->snapshot.version.raw_content) {
                        throw RpcError(-32011,
                                       "Rename target changed on disk since the request",
                                       Json{{"path", path_text(file.path)}});
                    }
                }
            }
        },
        [this, plan](const auto& operation) {
            if (operation->cancellation.stop_requested()) {
                throw transport::TransportCancelled("Rename apply cancelled");
            }
            if (editor_.notifications_.size() + plan->files.size() >
                EditorService::max_pending_notifications) {
                throw RpcError(-32004, "Rename notifications exceed pending capacity");
            }
            std::map<std::uint64_t, Document> staged;
            std::map<std::uint64_t, FileBinding> bindings;
            auto pending = editor_.notifications_;
            auto next_id = editor_.next_id_;
            Json result = Json::array();
            for (const auto& file : plan->files) {
                std::uint64_t id{};
                if (file.open) {
                    id = file.open->id.value;
                    const auto current = editor_.documents_.find(id);
                    const auto binding = files_.find(id);
                    if (current == editor_.documents_.end() ||
                        current->second.revision() != file.open->revision ||
                        binding == files_.end() || !equal_path(binding->second.path, file.path)) {
                        throw RpcError(
                            -32001,
                            "A rename target buffer changed, closed or was saved to another path");
                    }
                    staged.emplace(id, current->second.clone());
                } else {
                    for (const auto& [bound_id, binding] : files_) {
                        (void)bound_id;
                        if (equal_path(binding.path, file.path)) {
                            throw RpcError(
                                -32001, "A rename target was opened after the plan was prepared");
                        }
                    }
                    if (next_id == std::numeric_limits<std::uint64_t>::max()) {
                        throw RpcError(-32004, "Document IDs exhausted");
                    }
                    id = next_id++;
                    staged.emplace(id, Document(BufferId{id}, file.closed->snapshot.text));
                    const auto& loaded = *file.closed;
                    bindings.emplace(id,
                                     FileBinding{file.path,
                                                 loaded.snapshot.version,
                                                 loaded.snapshot.encoding,
                                                 loaded.config});
                }
                auto& document = staged.at(id);
                (void)document.apply({document.revision(), file.edits, std::nullopt});
                auto metadata = editor_.metadata(document);
                metadata["path"] = path_text(file.path);
                result.push_back(metadata);
                pending.push_back(protocol::make_notification(
                    file.open ? "document.changed" : "document.created", [&] {
                        metadata["reason"] = "rename";
                        return metadata;
                    }()));
            }
            Json response{{"applied", true}, {"documents", std::move(result)}};
            static_assert(std::is_nothrow_move_assignable_v<Document>);
            for (auto item = staged.begin(); item != staged.end();) {
                const auto id = item->first;
                if (const auto current = editor_.documents_.find(item->first);
                    current != editor_.documents_.end()) {
                    current->second = std::move(item->second);
                    item = staged.erase(item);
                } else {
                    auto node = staged.extract(item++);
                    editor_.documents_.insert(std::move(node));
                }
                static_cast<void>(editor_.publish_metadata(BufferId{id}));
            }
            files_.merge(bindings);
            editor_.notifications_.swap(pending);
            editor_.next_id_ = next_id;
            return response;
        });
    language_->plans.erase(found);
    return started;
}

} // namespace astra::service
