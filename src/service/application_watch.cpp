#include <astra/service/application_service.hpp>
#include <astra/watch/file_monitor.hpp>

#include <algorithm>
#include <chrono>
#include <set>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::service {
namespace {

namespace fs = std::filesystem;
using protocol::Json;
using protocol::RpcError;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::string path_text(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

bool same_path(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    const auto left = a.lexically_normal().native();
    const auto right = b.lexically_normal().native();
    return CompareStringOrdinal(left.c_str(), -1, right.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return a.lexically_normal() == b.lexically_normal();
#endif
}

void fields(const Json& params, std::initializer_list<std::string_view> allowed) {
    if (!params.is_object()) {
        throw RpcError(-32602, "File monitoring requires named parameters");
    }
    for (auto field = params.begin(); field != params.end(); ++field) {
        if (std::find(allowed.begin(), allowed.end(), field.key()) == allowed.end()) {
            throw RpcError(-32602, "Unrecognized file-monitor parameter: " + field.key());
        }
    }
}

bool missing_error(const std::error_code& error) {
    if (error == std::errc::no_such_file_or_directory) {
        return true;
    }
#ifdef _WIN32
    return error.category() == std::system_category() &&
           (error.value() == ERROR_FILE_NOT_FOUND || error.value() == ERROR_PATH_NOT_FOUND);
#else
    return false;
#endif
}

bool sharing_error(const std::error_code& error) {
#ifdef _WIN32
    return error.category() == std::system_category() &&
           (error.value() == ERROR_SHARING_VIOLATION || error.value() == ERROR_LOCK_VIOLATION);
#else
    return error == std::errc::device_or_resource_busy || error == std::errc::text_file_busy;
#endif
}

std::string exception_text(std::exception_ptr error) {
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& detail) {
        return detail.what();
    } catch (...) {
        return "Unknown file monitor worker failure";
    }
}

} // namespace

struct ApplicationService::WatchState {
    static constexpr std::size_t max_files = 4096;
    static constexpr std::size_t max_observed_bytes = 64 * 1024 * 1024;

    struct Observed {
        std::string status;
        std::shared_ptr<const std::string> bytes;
        std::string error;
    };

    struct Check {
        BufferId id;
        fs::path path;
        io::FileVersion expected;
        DocumentSnapshot snapshot;
        bool initially_dirty{};
        bool explicit_reload{};
        std::optional<Observed> previous;
        std::unique_ptr<Document> prepared;
        std::optional<FileBinding> loaded;
        bool disk_changed{};
        bool missing{};
        bool retryable{};
        bool repeated{};
        bool settled{};
        std::exception_ptr error;
        std::optional<std::future<void>> future;
    };

    std::unique_ptr<watch::FileMonitor> monitor;
    std::optional<std::future<std::uint64_t>> subscription;
    std::vector<fs::path> requested;
    std::vector<fs::path> subscribed;
    std::map<std::uint64_t, fs::path> inventory;
    std::set<std::uint64_t> queued;
    std::map<std::uint64_t, Observed> observed;
    std::map<std::uint64_t, unsigned int> retries;
    std::map<std::uint64_t, std::string> reloads;
    std::shared_ptr<Check> check;
    std::uint64_t generation{};
    bool ready{};
    Clock::time_point next_poll{};
    Clock::time_point retry_subscription{};
    Clock::time_point next_check{};
    std::string warning;

    explicit WatchState(const fs::path& root, bool enabled) {
        if (enabled) {
            monitor = std::make_unique<watch::FileMonitor>(root);
            subscription.emplace(monitor->start());
        }
    }

    static void notify(ApplicationService& app, std::string_view method, Json params) {
        if (app.notifications_.size() >= 1024) {
            app.notifications_.clear();
            app.notifications_.push_back(
                protocol::make_notification("transport.resyncRequired", Json::object()));
        }
        app.notifications_.push_back(
            protocol::make_notification(std::string(method), std::move(params)));
    }

    static void
    changed(ApplicationService& app, const Document& document, std::string_view reason) {
        if (app.editor_.notifications_.size() >= EditorService::max_pending_notifications) {
            app.editor_.notifications_.clear();
            // Retire closed bindings even if their queued close event was lost.
            std::erase_if(app.files_, [&](const auto& entry) {
                return !app.editor_.documents_.contains(entry.first);
            });
            notify(app, "transport.resyncRequired", Json::object());
        }
        app.editor_.changed(document, reason);
    }

    void warn(ApplicationService& app, const std::string& message) {
        if (warning == message) {
            return;
        }
        warning = message;
        notify(app, "workspace.watchError", {{"message", message}, {"rescanRequired", true}});
    }

    void remember(std::uint64_t id, Observed value) {
        observed.insert_or_assign(id, std::move(value));
        std::size_t bytes = 0;
        for (const auto& [key, previous] : observed) {
            (void)key;
            if (previous.bytes) {
                bytes += previous.bytes->size();
            }
        }
        // Version comparison itself happens on the worker. This cache only
        // suppresses duplicate alerts, so eviction cannot authorize a save.
        while (bytes > max_observed_bytes && !observed.empty()) {
            if (observed.begin()->second.bytes) {
                bytes -= observed.begin()->second.bytes->size();
            }
            observed.erase(observed.begin());
        }
    }

    static std::shared_ptr<Check>
    capture(ApplicationService& app, std::uint64_t id, bool explicit_reload) {
        const auto& document = app.editor_.documents_.at(id);
        const auto& binding = app.files_.at(id);
        auto item = std::make_shared<Check>();
        item->id = document.id();
        item->path = binding.path;
        item->expected = binding.version;
        item->snapshot = document.snapshot();
        item->initially_dirty = document.dirty();
        item->explicit_reload = explicit_reload;
        if (!document.dirty() || explicit_reload) {
            item->prepared = std::make_unique<Document>(document.clone());
        }
        if (const auto previous = app.watch_->observed.find(id);
            previous != app.watch_->observed.end()) {
            item->previous = previous->second;
        }
        return item;
    }

    static void read(const std::shared_ptr<workspace::Workspace>& workspace, Check& item) {
        try {
            auto file = workspace->load(item.path);
            const auto& raw = file.snapshot.version.raw_content;
            item.disk_changed = !item.expected.raw_content || *raw != *item.expected.raw_content;
            item.repeated = item.previous && item.previous->status == "conflict" &&
                            item.previous->bytes && *item.previous->bytes == *raw;
            if (item.prepared && (item.disk_changed || item.explicit_reload)) {
                const Rope after(file.snapshot.text);
                const auto extent = item.prepared->text().changed_extent(after);
                if (extent.begin != extent.old_end || extent.begin != extent.new_end) {
                    (void)item.prepared->apply({item.prepared->revision(),
                                                {{{extent.begin},
                                                  {extent.old_end},
                                                  after.slice(extent.begin, extent.new_end)}},
                                                std::nullopt});
                }
                item.prepared->mark_saved();
            }
            item.loaded = FileBinding{file.relative_path,
                                      std::move(file.snapshot.version),
                                      file.snapshot.encoding,
                                      std::move(file.config)};
        } catch (const fs::filesystem_error& error) {
            item.missing = missing_error(error.code());
            item.retryable = sharing_error(error.code());
            item.error = std::current_exception();
        } catch (const io::FileConflict&) {
            // A writer racing this bounded read can be retried; its bytes never
            // replace the document until a complete strict read succeeds.
            item.retryable = true;
            item.error = std::current_exception();
        } catch (...) {
            item.error = std::current_exception();
        }
        if (item.error && item.previous) {
            item.repeated = item.previous->status == (item.missing ? "deleted" : "error") &&
                            item.previous->error == exception_text(item.error);
        }
    }

    [[nodiscard]] static bool binding_matches(ApplicationService& app, const Check& item) {
        const auto file = app.files_.find(item.id.value);
        return file != app.files_.end() && same_path(file->second.path, item.path) &&
               file->second.version.raw_content == item.expected.raw_content &&
               same_path(file->second.version.path, item.expected.path);
    }

    [[nodiscard]] static bool busy(ApplicationService& app, std::uint64_t id) {
        return app.saving_.contains(id) || app.replacing_ || app.git_mutation_.has_value();
    }

    // Result preparation (including all Rope work and undo history) was on the
    // worker. Publication retains monotonic revision and never overwrites an
    // intervening edit, save, close/reopen, or selection change.
    static Json publish(ApplicationService& app, Check& item) {
        auto found = app.editor_.documents_.find(item.id.value);
        if (found == app.editor_.documents_.end()) {
            throw RpcError(-32002, "Document closed during reload");
        }
        auto& document = found->second;
        if (!binding_matches(app, item) || busy(app, item.id.value) ||
            document.revision() != item.snapshot.revision ||
            document.content_identity() != item.snapshot.content_identity ||
            document.selections() != item.snapshot.selections) {
            throw RpcError(
                -32001,
                "Document or file binding changed during reload; retry with its current revision");
        }
        if (!item.explicit_reload && document.dirty()) {
            throw RpcError(-32003, "Document became dirty during reload");
        }
        if (item.error) {
            std::rethrow_exception(item.error);
        }
        if (!item.prepared || !item.loaded) {
            throw std::logic_error("Missing prepared reload result");
        }
        // Prepare metadata before publishing the nonthrowing value moves.
        auto result = EditorService::metadata(*item.prepared);
        result["path"] = path_text(item.loaded->path);
        result["editorConfig"] = item.loaded->config.properties;
        result["reloaded"] = true;
        document = std::move(*item.prepared);
        app.files_.at(item.id.value) = std::move(*item.loaded);
        app.watch_->observed.erase(item.id.value);
        app.watch_->retries.erase(item.id.value);
        changed(app, document, "externalReload");
        notify(app,
               "workspace.fileChanged",
               {{"id", std::to_string(item.id.value)},
                {"path", path_text(item.path)},
                {"status", "reloaded"},
                {"revision", std::to_string(document.revision().value)},
                {"dirty", false}});
        return result;
    }

    void finish(ApplicationService& app,
                const std::shared_ptr<Check>& item,
                std::exception_ptr scheduling_error) {
        if (item->settled) {
            return;
        }
        item->settled = true;
        if (scheduling_error) {
            warn(app, exception_text(scheduling_error));
            return;
        }
        auto found = app.editor_.documents_.find(item->id.value);
        if (found == app.editor_.documents_.end() || !binding_matches(app, *item)) {
            return;
        }
        if (busy(app, item->id.value) || reloads.contains(item->id.value)) {
            queued.insert(item->id.value);
            return;
        }
        if (item->retryable && retries[item->id.value]++ < 3) {
            queued.insert(item->id.value);
            next_check = Clock::now() + 100ms;
            return;
        }
        retries.erase(item->id.value);
        auto& document = found->second;
        if (!item->error && !item->disk_changed) {
            return;
        }
        if (!item->error && !item->initially_dirty && !document.dirty() &&
            document.revision() == item->snapshot.revision &&
            document.content_identity() == item->snapshot.content_identity) {
            if (document.selections() != item->snapshot.selections) {
                queued.insert(item->id.value);
                return;
            }
            (void)publish(app, *item);
            return;
        }
        const auto status = item->error ? (item->missing ? "deleted" : "error") : "conflict";
        const auto detail = item->error ? exception_text(item->error) : std::string{};
        remember(item->id.value,
                 {status, item->loaded ? item->loaded->version.raw_content : nullptr, detail});
        const bool was_dirty = document.dirty();
        document.invalidate_saved_state();
        if (!was_dirty) {
            changed(app, document, "externalConflict");
        }
        if (item->repeated) {
            return;
        }
        Json notification{{"id", std::to_string(item->id.value)},
                          {"path", path_text(item->path)},
                          {"status", status},
                          {"revision", std::to_string(document.revision().value)},
                          {"dirty", true}};
        if (!detail.empty()) {
            notification["message"] = detail;
        }
        notify(app, "workspace.fileChanged", std::move(notification));
    }
};

void ApplicationService::register_watch_methods(bool enabled) {
    watch_ = std::make_shared<WatchState>(workspace_->root(), enabled);
    editor_.dispatcher().register_method("workspace.reload", [this](const Json& params) {
        return reload_file(params);
    });
    editor_.dispatcher().register_method("workspace.watchStatus", [this](const Json& params) {
        fields(params, {});
        Json result{{"enabled", watch_->monitor != nullptr},
                    {"ready", watch_->ready},
                    {"generation", std::to_string(watch_->generation)},
                    {"watchedFiles", watch_->inventory.size()},
                    {"maxFiles", WatchState::max_files},
                    {"pending",
                     watch_->subscription.has_value() || watch_->check != nullptr ||
                         !watch_->queued.empty()}};
        if (!watch_->warning.empty()) {
            result["error"] = watch_->warning;
        }
        return result;
    });
}

void ApplicationService::poll_watch() {
    if (!watch_ || !watch_->monitor) {
        return;
    }
    auto& state = *watch_;
    const auto now = Clock::now();
    if (now < state.next_poll) {
        return;
    }
    state.next_poll = now + 25ms;
    std::erase_if(state.reloads, [&](const auto& entry) {
        const auto operation = operations_.find(entry.second);
        return operation == operations_.end() || operation->second->state != "pending";
    });
    if (state.check && state.check->future &&
        state.check->future->wait_for(0s) == std::future_status::ready) {
        try {
            state.check->future->get();
        } catch (...) {
            state.finish(*this, state.check, std::current_exception());
        }
        state.check.reset();
    }
    std::map<std::uint64_t, fs::path> inventory;
    std::vector<fs::path> paths;
    for (const auto& [id, binding] : files_) {
        if (!editor_.documents_.contains(id)) {
            continue;
        }
        if (inventory.size() == WatchState::max_files) {
            state.warn(*this,
                       "Open-file monitor limit reached; additional files require explicit "
                       "workspace.reload");
            break;
        }
        inventory.emplace(id, binding.path);
        paths.push_back(binding.path);
    }
    if (inventory != state.inventory) {
        for (const auto& [id, path] : inventory) {
            const auto previous = state.inventory.find(id);
            if (previous == state.inventory.end() || previous->second != path) {
                state.queued.insert(id);
            }
        }
        state.inventory = std::move(inventory);
        std::erase_if(state.queued, [&](auto id) {
            return !state.inventory.contains(id);
        });
        std::erase_if(state.observed, [&](const auto& entry) {
            return !state.inventory.contains(entry.first);
        });
        std::erase_if(state.retries, [&](const auto& entry) {
            return !state.inventory.contains(entry.first);
        });
    }
    if (state.subscription && state.subscription->wait_for(0s) == std::future_status::ready) {
        try {
            state.generation = state.subscription->get();
            state.subscribed = state.requested;
            state.ready = true;
            state.warning.clear();
        } catch (...) {
            state.warn(*this, exception_text(std::current_exception()));
            state.retry_subscription = now + 1s;
        }
        state.subscription.reset();
    }
    if (!state.subscription && paths != state.subscribed && now >= state.retry_subscription) {
        state.requested = paths;
        state.subscription.emplace(state.monitor->replace_paths(paths));
    }
    const bool queue_was_empty = state.queued.empty();
    auto batch = state.monitor->take_events();
    if (batch.error) {
        state.warn(*this, *batch.error);
    }
    if (batch.rescan_required) {
        for (const auto& [id, path] : state.inventory) {
            (void)path;
            state.queued.insert(id);
        }
    } else {
        for (const auto& event : batch.events) {
            for (const auto& [id, path] : state.inventory) {
                if (same_path(path, event.relative_path)) {
                    state.queued.insert(id);
                }
            }
        }
    }
    if (queue_was_empty && !state.queued.empty()) {
        state.next_check = std::max(state.next_check, now + 40ms);
    }
    if (state.check || state.queued.empty() || now < state.next_check) {
        return;
    }
    auto next = std::find_if(state.queued.begin(), state.queued.end(), [&](auto id) {
        return !WatchState::busy(*this, id) && !state.reloads.contains(id);
    });
    if (next == state.queued.end()) {
        return;
    }
    const auto id = *next;
    state.queued.erase(next);
    if (!files_.contains(id) || !editor_.documents_.contains(id)) {
        return;
    }
    auto item = WatchState::capture(*this, id, false);
    state.check = item;
    state.next_check = now + 40ms;
    try {
        item->future.emplace(scheduler_(
            [workspace = workspace_, item] {
                WatchState::read(workspace, *item);
            },
            [this, state = watch_, item](std::exception_ptr error) {
                state->finish(*this, item, error);
            }));
    } catch (...) {
        state.finish(*this, item, std::current_exception());
        state.check.reset();
    }
}

Json ApplicationService::reload_file(const Json& params) {
    fields(params, {"id", "baseRevision", "discardChanges"});
    auto& document = editor_.find_document(params);
    EditorService::require_revision(document, params);
    bool discard = false;
    if (params.contains("discardChanges")) {
        if (!params.at("discardChanges").is_boolean()) {
            throw RpcError(-32602, "discardChanges must be boolean");
        }
        discard = params.at("discardChanges").get<bool>();
    }
    if (document.dirty() && !discard) {
        throw RpcError(-32003, "Reload requires explicit discardChanges for a dirty document");
    }
    if (!files_.contains(document.id().value)) {
        throw RpcError(-32602, "Untitled documents cannot reload from disk");
    }
    if (WatchState::busy(*this, document.id().value)) {
        throw RpcError(-32013, "A save or workspace mutation is in progress");
    }
    if (const auto previous = watch_->reloads.find(document.id().value);
        previous != watch_->reloads.end()) {
        const auto operation = operations_.find(previous->second);
        if (operation != operations_.end() && operation->second->state == "pending") {
            throw RpcError(-32013, "A reload is already running");
        }
    }
    const auto item = WatchState::capture(*this, document.id().value, true);
    const auto started = start_operation(
        "reload",
        [workspace = workspace_, item](const auto& operation) {
            if (operation->cancellation.stop_requested()) {
                throw RpcError(-32014, "Reload cancelled before reading");
            }
            WatchState::read(workspace, *item);
            if (item->error) {
                std::rethrow_exception(item->error);
            }
        },
        [this, item](const auto& operation) {
            if (operation->cancellation.stop_requested()) {
                throw RpcError(-32014, "Reload cancelled before publication");
            }
            return WatchState::publish(*this, *item);
        });
    const auto operation = started.at("operationId").get<std::string>();
    if (operations_.at(operation)->state == "pending") {
        watch_->reloads[document.id().value] = operation;
    }
    return started;
}

} // namespace astra::service
