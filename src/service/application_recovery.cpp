#include <astra/service/application_service.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <set>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::service {
namespace {
using protocol::Json;
using protocol::RpcError;
using recovery::RecoveryRecord;
using recovery::RecoveryStore;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

bool same_path(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    const auto left = a.lexically_normal().native();
    const auto right = b.lexically_normal().native();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a.lexically_normal() == b.lexically_normal();
#endif
}

std::string path_text(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

void fields(const Json& params, std::initializer_list<std::string_view> allowed) {
    if (!params.is_object()) {
        throw RpcError(-32602, "Recovery requires named parameters");
    }
    for (auto i = params.begin(); i != params.end(); ++i) {
        if (std::find(allowed.begin(), allowed.end(), i.key()) == allowed.end()) {
            throw RpcError(-32602, "Unknown recovery parameter");
        }
    }
}

bool same_record(const RecoveryRecord& a, const RecoveryRecord& b) {
    return a.document.id == b.document.id && a.document.revision == b.document.revision &&
           a.document.content_identity == b.document.content_identity &&
           a.document.selections == b.document.selections && a.path == b.path &&
           a.encoding == b.encoding && a.disk_version.has_value() == b.disk_version.has_value() &&
           (!a.disk_version || (a.disk_version->path == b.disk_version->path &&
                                a.disk_version->raw_content == b.disk_version->raw_content));
}

bool same_frame(const std::vector<RecoveryRecord>& a, const std::vector<RecoveryRecord>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), same_record);
}

Json list_sessions(const fs::path& directory) {
    auto result = Json::array();
    for (const auto& session : RecoveryStore::discover(directory)) {
        if (session.buffers.empty() && session.issues.empty()) {
            continue;
        }
        Json item{{"id", session.id},
                  {"active", session.active},
                  {"buffers", Json::array()},
                  {"issues", Json::array()}};
        for (const auto& issue : session.issues) {
            item["issues"].push_back({{"path", path_text(issue.path)}, {"message", issue.message}});
        }
        if (!session.active) {
            try {
                RecoveryStore store(directory, session.id);
                for (const auto id : session.buffers) {
                    const auto record = store.load(id);
                    if (!record) {
                        continue;
                    }
                    Json buffer{{"id", std::to_string(id.value)},
                                {"revision", std::to_string(record->document.revision.value)},
                                {"bytes", record->document.text.size_bytes()}};
                    if (record->path) {
                        buffer["path"] = path_text(*record->path);
                    }
                    item["buffers"].push_back(std::move(buffer));
                }
            } catch (const std::exception& error) {
                item["issues"].push_back(
                    {{"path", path_text(directory)}, {"message", error.what()}});
            }
        } else {
            for (const auto id : session.buffers) {
                item["buffers"].push_back({{"id", std::to_string(id.value)}});
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}
} // namespace

struct ApplicationService::RecoveryState {
    explicit RecoveryState(fs::path path) : directory(std::move(path)) {
    }

    fs::path directory;
    // Only workers touch the store/cache/claims, under this mutex. Completion
    // callbacks touch owner fields below after the worker has finished.
    std::mutex mutex;
    std::unique_ptr<RecoveryStore> store;
    Json maintenance_issues = Json::array();
    std::map<std::uint64_t, RecoveryRecord> persisted;
    std::uint64_t worker_epoch{};

    struct Transfer {
        std::shared_ptr<RecoveryStore> source;
        BufferId old_id;
        Revision old_revision;
        BufferId new_id;
    };

    bool ready{};
    std::size_t pending{};
    std::string session;
    std::string error;
    std::uint64_t next_epoch{1};
    std::vector<RecoveryRecord> observed;
    std::vector<Transfer> transfers;
    Clock::time_point next_poll{};
    Clock::time_point last_change{};
    std::optional<Clock::time_point> changed_since;

    void initialize() {
        if (!store) {
            const auto pruned = RecoveryStore::prune_empty_sessions(directory);
            for (const auto& issue : pruned.issues) {
                maintenance_issues.push_back(
                    {{"path", path_text(issue.path)}, {"message", issue.message}});
            }
            store = std::make_unique<RecoveryStore>(directory);
        }
    }

    Json checkpoint(std::vector<RecoveryRecord> frame,
                    std::uint64_t epoch,
                    const std::vector<Transfer>& pending_transfers) {
        const std::lock_guard lock(mutex);
        initialize();
        if (epoch < worker_epoch) {
            return {{"sessionId", store->session_id()}, {"obsolete", true}};
        }
        worker_epoch = epoch;
        std::set<std::uint64_t> current;
        for (auto& record : frame) {
            const auto id = record.document.id.value;
            current.insert(id);
            if (const auto old = persisted.find(id);
                old != persisted.end() && same_record(old->second, record)) {
                continue;
            }
            record.checkpoint_sequence = epoch;
            store->checkpoint(record);
            persisted.insert_or_assign(id, std::move(record));
        }
        for (auto i = persisted.begin(); i != persisted.end();) {
            if (current.contains(i->first)) {
                ++i;
                continue;
            }
            store->erase(BufferId{i->first}, i->second.document.revision);
            i = persisted.erase(i);
        }
        // Preserve the old session until the restored text is durable in this
        // session, or the user has explicitly saved/discarded the new buffer.
        auto transferred = Json::array();
        for (const auto& transfer : pending_transfers) {
            if (!current.contains(transfer.new_id.value) ||
                persisted.contains(transfer.new_id.value)) {
                transfer.source->erase(transfer.old_id, transfer.old_revision);
                transferred.push_back(transfer.new_id.value);
            }
        }
        return {{"sessionId", store->session_id()},
                {"checkpointed", persisted.size()},
                {"obsolete", false},
                {"transferred", transferred},
                {"maintenanceIssues", maintenance_issues}};
    }
};

std::vector<RecoveryRecord> ApplicationService::capture_recovery() const {
    std::vector<RecoveryRecord> frame;
    for (const auto& [id, document] : editor_.documents_) {
        if (!document.dirty()) {
            continue;
        }
        RecoveryRecord record{
            document.snapshot(), std::nullopt, io::TextEncoding::utf8, std::nullopt};
        if (const auto file = files_.find(id); file != files_.end()) {
            record.path = file->second.version.path;
            record.encoding = file->second.encoding;
            if (file->second.version.raw_content) {
                record.disk_version = file->second.version;
            }
        }
        frame.push_back(std::move(record));
    }
    return frame;
}

void ApplicationService::checkpoint_for_shutdown() {
    std::exception_ptr script_failure;
    try {
        script_->shutdown();
        // The in-process dispatcher returns ready futures; workers have already
        // drained before this final hook. No transport wait is needed here.
        for (int tick = 0; tick < 256 && !script_->idle(); ++tick) {
            script_->poll();
        }
        if (!script_->idle()) {
            throw script::ScriptError("Script shutdown did not drain within its job budget");
        }
    } catch (...) {
        script_failure = std::current_exception();
    }
    if (!recovery_) {
        if (script_failure) {
            std::rethrow_exception(script_failure);
        }
        return;
    }
    if (recovery_->next_epoch == std::numeric_limits<std::uint64_t>::max()) {
        throw RpcError(-32040, "Recovery sequence exhausted");
    }
    static_cast<void>(
        recovery_->checkpoint(capture_recovery(), recovery_->next_epoch++, recovery_->transfers));
    recovery_->transfers.clear();
    if (script_failure) {
        std::rethrow_exception(script_failure);
    }
}

void ApplicationService::register_recovery_methods(const fs::path& directory) {
    if (!directory.empty()) {
        recovery_ = std::make_shared<RecoveryState>(directory);
    }
    for (const auto method :
         {"recovery.status", "recovery.list", "recovery.flush", "recovery.restore"}) {
        editor_.dispatcher().register_method(method, [this, method](const Json& params) {
            return recovery_request(method, params);
        });
    }
}

void ApplicationService::poll_recovery() {
    if (!recovery_) {
        return;
    }
    auto& state = *recovery_;
    const auto now = Clock::now();
    if (now < state.next_poll) {
        return;
    }
    state.next_poll = now + 100ms;
    auto frame = capture_recovery();
    if (!same_frame(frame, state.observed)) {
        state.observed = std::move(frame);
        state.last_change = now;
        if (!state.changed_since) {
            state.changed_since = now;
        }
    }
    if (state.pending != 0 || (!state.error.empty() && now < state.last_change + 5s)) {
        return;
    }
    const bool due = !state.ready || (state.changed_since && (now - state.last_change >= 500ms ||
                                                              now - *state.changed_since >= 2s));
    if (!due) {
        return;
    }
    try {
        static_cast<void>(recovery_request("recovery.flush", Json::object()));
    } catch (const std::exception& error) {
        state.error = error.what();
        state.last_change = now;
        notifications_.push_back(
            protocol::make_notification("recovery.error", {{"message", state.error}}));
    }
}

void ApplicationService::finish_recovery_operation(const std::shared_ptr<Operation>& operation,
                                                   std::exception_ptr error) {
    if (!recovery_ || operation->kind != "recovery.flush") {
        return;
    }
    auto& state = *recovery_;
    if (state.pending != 0) {
        --state.pending;
    }
    if (!error) {
        return;
    }
    std::string message;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& failure) {
        message = failure.what();
    } catch (...) {
        message = "Unknown checkpoint failure";
    }
    if (state.error != message) {
        notifications_.push_back(
            protocol::make_notification("recovery.error", {{"message", message}}));
    }
    state.error = std::move(message);
    state.last_change = Clock::now();
}

Json ApplicationService::recovery_request(std::string_view method, const Json& params) {
    fields(params,
           method == "recovery.restore"
               ? std::initializer_list<std::string_view>{"sessionId", "ids"}
               : std::initializer_list<std::string_view>{});
    if (method == "recovery.status") {
        if (!recovery_) {
            return {{"enabled", false}, {"ready", false}, {"pending", false}};
        }
        Json status{
            {"enabled", true}, {"ready", recovery_->ready}, {"pending", recovery_->pending != 0}};
        if (!recovery_->session.empty()) {
            status["sessionId"] = recovery_->session;
        }
        if (!recovery_->error.empty()) {
            status["error"] = recovery_->error;
        }
        return status;
    }
    if (!recovery_) {
        if (method == "recovery.flush") {
            return start_operation(
                "recovery.flush",
                [](const auto&) {
                },
                [](const auto&) {
                    return Json{{"enabled", false}, {"checkpointed", 0}};
                });
        }
        throw RpcError(-32040, "Recovery is disabled for this service host");
    }
    const auto state = recovery_;
    if (method == "recovery.flush") {
        if (state->next_epoch == std::numeric_limits<std::uint64_t>::max()) {
            throw RpcError(-32040, "Recovery sequence exhausted");
        }
        auto frame = capture_recovery();
        const auto epoch = state->next_epoch++;
        const bool announce = !state->ready;
        auto result = std::make_shared<Json>();
        ++state->pending;
        try {
            const auto started = start_operation(
                "recovery.flush",
                [state, frame, epoch, announce, result, transfers = state->transfers](
                    const auto&) mutable {
                    // A scheduler may retain completed callbacks. Their capture must
                    // not extend source-session ownership beyond the durable transfer.
                    const auto pending_transfers = std::move(transfers);
                    *result = state->checkpoint(std::move(frame), epoch, pending_transfers);
                    if (announce) {
                        // Listing briefly claims abandoned sessions to read their
                        // metadata. Serialize these claims with restore/list/prune,
                        // not just checkpoint writes, or this host can reject its own
                        // concurrent restore as if another application owned it.
                        const std::lock_guard lock(state->mutex);
                        (*result)["sessions"] = list_sessions(state->directory);
                    }
                },
                [this, state, result, frame, announce](const auto&) {
                    state->ready = true;
                    state->error.clear();
                    state->session = result->at("sessionId").get<std::string>();
                    if (result->contains("transferred")) {
                        for (const auto& id : result->at("transferred")) {
                            std::erase_if(state->transfers, [&](const auto& transfer) {
                                return transfer.new_id.value == id.get<std::uint64_t>();
                            });
                        }
                    }
                    if (same_frame(state->observed, frame)) {
                        state->changed_since.reset();
                    }
                    if (result->contains("sessions") && !result->at("sessions").empty()) {
                        notifications_.push_back(protocol::make_notification(
                            "recovery.available", {{"sessions", result->at("sessions")}}));
                    }
                    if (announce && result->contains("maintenanceIssues") &&
                        !result->at("maintenanceIssues").empty()) {
                        notifications_.push_back(protocol::make_notification(
                            "recovery.warning", {{"issues", result->at("maintenanceIssues")}}));
                    }
                    return std::move(*result);
                });
            return started;
        } catch (...) {
            --state->pending;
            throw;
        }
    }
    if (method == "recovery.list") {
        const auto result = std::make_shared<Json>();
        return start_operation(
            "recovery.list",
            [state, result](const auto&) {
                const std::lock_guard lock(state->mutex);
                state->initialize();
                *result = {{"sessions", list_sessions(state->directory)}};
            },
            [result](const auto&) {
                return std::move(*result);
            });
    }
    if (!params.contains("sessionId") || !params["sessionId"].is_string()) {
        throw RpcError(-32602, "Recovery sessionId is required");
    }
    const auto session_id = params["sessionId"].get<std::string>();
    std::set<std::string> selected;
    if (params.contains("ids")) {
        if (!params["ids"].is_array() || params["ids"].empty() || params["ids"].size() > 128) {
            throw RpcError(-32602, "Restore 1 to 128 buffer IDs");
        }
        for (const auto& id : params["ids"]) {
            if (!id.is_string() || !selected.insert(id.get<std::string>()).second) {
                throw RpcError(-32602, "Recovery IDs must be unique strings");
            }
        }
    }

    struct Restored {
        std::shared_ptr<RecoveryStore> claim;
        std::vector<RecoveryRecord> records;
        std::vector<std::optional<FileBinding>> bindings;
    };

    const auto restored = std::make_shared<Restored>();
    return start_operation(
        "recovery.restore",
        [state, session_id, selected, restored, workspace = workspace_](const auto& operation) {
            const std::lock_guard lock(state->mutex);
            // Keep ownership local until every selected record is validated. A
            // failed worker must release the OS lock immediately, independently of
            // when the scheduler destroys its worker/completion closures.
            auto claim = std::make_shared<RecoveryStore>(state->directory, session_id);
            const auto listed = claim->inspect();
            std::size_t bytes = 0;
            for (const auto id : listed.buffers) {
                if (!selected.empty() && !selected.contains(std::to_string(id.value))) {
                    continue;
                }
                if (operation->cancellation.stop_requested()) {
                    throw RpcError(-32040, "Recovery restore cancelled");
                }
                auto record = claim->load(id);
                if (!record) {
                    continue;
                }
                bytes += record->document.text.size_bytes();
                if (record->disk_version) {
                    bytes += record->disk_version->raw_content->size();
                }
                if (restored->records.size() >= 128 || bytes > 128 * 1024 * 1024) {
                    throw RpcError(-32040, "Restore at most 128 buffers and 128 MiB per request");
                }
                std::optional<FileBinding> binding;
                if (record->path) {
                    const auto relative = record->path->lexically_relative(workspace->root());
                    const auto absolute = workspace->resolve_path(relative);
                    if (!same_path(absolute, *record->path)) {
                        throw RpcError(-32040, "Recovery document is outside this workspace");
                    }
                    const auto configured = workspace->editor_config(relative);
                    binding =
                        FileBinding{relative,
                                    record->disk_version.value_or(io::FileVersion{absolute, {}}),
                                    record->encoding,
                                    configured.config};
                }
                restored->records.push_back(std::move(*record));
                restored->bindings.push_back(std::move(binding));
            }
            if ((!selected.empty() && restored->records.size() != selected.size()) ||
                restored->records.empty()) {
                throw RpcError(
                    -32040,
                    "Selected recovery buffers are missing or corrupt; inspect recovery.list");
            }
            restored->claim = std::move(claim);
        },
        [this, state, restored, session_id](const auto& operation) {
            // On failure release the claim as this callback unwinds. On success
            // only pending durable transfers retain it; completed task captures
            // must not keep the old session locked after transfer finishes.
            auto claim = std::move(restored->claim);
            if (operation->cancellation.stop_requested()) {
                throw RpcError(-32040, "Recovery restore cancelled");
            }
            const auto count = restored->records.size();
            if (count > EditorService::max_pending_notifications - editor_.notifications_.size() ||
                count >= std::numeric_limits<std::uint64_t>::max() - editor_.next_id_) {
                throw RpcError(-32004, "Recovery document or notification capacity exceeded");
            }
            std::map<std::uint64_t, Document> prepared;
            std::map<std::uint64_t, FileBinding> bindings;
            auto documents = Json::array();
            auto events = std::deque<Json>();
            std::vector<RecoveryState::Transfer> transfers;
            const auto first_id = editor_.next_id_;
            auto next_id = first_id;
            for (std::size_t i = 0; i < count; ++i) {
                const auto& binding = restored->bindings[i];
                if (binding) {
                    for (const auto& [id, existing] : files_) {
                        if (editor_.documents_.contains(id) &&
                            same_path(existing.version.path, binding->version.path)) {
                            throw RpcError(
                                -32040,
                                "Close the already-open document before restoring its checkpoint");
                        }
                    }
                    for (const auto& [id, existing] : bindings) {
                        static_cast<void>(id);
                        if (same_path(existing.version.path, binding->version.path)) {
                            throw RpcError(
                                -32040, "Restore conflicting checkpoints for one file separately");
                        }
                    }
                }
                const auto& record = restored->records[i];
                auto document = Document::from_snapshot(BufferId{next_id}, record.document);
                document.invalidate_saved_state();
                auto metadata = editor_.metadata(document);
                if (binding) {
                    metadata["path"] = path_text(binding->path);
                    metadata["editorConfig"] = binding->config.properties;
                    bindings.emplace(next_id, *binding);
                }
                documents.push_back(metadata);
                events.push_back(protocol::make_notification("document.created", metadata));
                transfers.push_back(
                    {claim, record.document.id, record.document.revision, BufferId{next_id}});
                prepared.emplace(next_id++, std::move(document));
            }
            // Allocate all transfer/event storage before publishing any document.
            auto new_events = editor_.notifications_;
            for (auto& event : events) {
                new_events.push_back(std::move(event));
            }
            state->transfers.reserve(state->transfers.size() + transfers.size());
            for (auto& transfer : transfers) {
                state->transfers.push_back(std::move(transfer));
            }
            editor_.documents_.merge(prepared);
            for (auto id = first_id; id < next_id; ++id)
                static_cast<void>(editor_.publish_metadata(BufferId{id}));
            files_.merge(bindings);
            editor_.next_id_ = next_id;
            editor_.notifications_.swap(new_events);
            return Json{{"documents", std::move(documents)}, {"sessionId", session_id}};
        });
}
} // namespace astra::service
