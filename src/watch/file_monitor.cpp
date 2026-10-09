#include <astra/watch/file_monitor.hpp>

#include <uv.h>

#include <algorithm>
#include <compare>
#include <cstdio>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::watch {
namespace {

namespace fs = std::filesystem;

std::runtime_error uv_error(std::string_view operation, int error) {
    return std::runtime_error(std::string(operation) + ": " + uv_strerror(error));
}

std::string path_utf8(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

bool same_component(const fs::path& a, const fs::path& b) {
#ifdef _WIN32
    // Ordinal Windows comparison avoids locale-dependent byte/case folding.
    // In case-sensitive directories a false-positive notification is harmless:
    // subscriptions stay distinct and the consumer checks each exact disk path.
    const auto& left = a.native();
    const auto& right = b.native();
    if (left.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        right.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) { return false; }
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
        static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
#else
    return a == b;
#endif
}

fs::path inside_relative(const fs::path& root, const fs::path& candidate) {
    auto root_part = root.begin();
    auto part = candidate.begin();
    fs::path prefix;
    for (; root_part != root.end(); ++root_part, ++part) {
        if (part == candidate.end() || !same_component(*root_part, *part)) {
            throw std::invalid_argument("monitored path escapes workspace root");
        }
        prefix /= *part;
    }
    // Do not mistake a differently cased sibling in a Windows case-sensitive
    // directory for the actual workspace, despite conservative name matching.
    if (!fs::equivalent(root, prefix)) { throw std::invalid_argument("monitored path escapes workspace identity"); }
    fs::path relative;
    for (; part != candidate.end(); ++part) { relative /= *part; }
    return relative;
}

fs::path normalize_relative(const fs::path& input) {
    if (input.empty() || input.is_absolute() || input.has_root_name() || input.has_root_directory()) {
        throw std::invalid_argument("monitored file path must be workspace-relative");
    }
    const auto& native = input.native();
    if (native.find(fs::path::value_type{}) != native.npos || native.size() > 32768) {
        throw std::invalid_argument("invalid or excessive monitored path");
    }
    for (const auto& part : input) {
        if (part == "..") { throw std::invalid_argument("parent traversal is not a monitored file path"); }
#ifdef _WIN32
        if (part.native().find(L':') != std::wstring::npos) {
            throw std::invalid_argument("alternate streams are not monitored file paths");
        }
#endif
    }
    auto result = input.lexically_normal();
    if (result.empty() || result == "." || result.filename().empty()) {
        throw std::invalid_argument("monitored path must name a file");
    }
    return result;
}

struct DirectoryIdentity {
    std::uint64_t device{};
    std::uint64_t inode{};
    auto operator<=>(const DirectoryIdentity&) const = default;
};

DirectoryIdentity directory_identity(const fs::path& path) {
    if (!fs::is_directory(path)) { throw std::invalid_argument("monitored directory does not exist"); }
    const auto name = path_utf8(path);
    uv_fs_t request{};
    const int status = uv_fs_stat(nullptr, &request, name.c_str(), nullptr);
    if (status < 0) { uv_fs_req_cleanup(&request); throw uv_error("stat monitored directory", status); }
    const DirectoryIdentity result{request.statbuf.st_dev, request.statbuf.st_ino};
    uv_fs_req_cleanup(&request);
    return result;
}

template<class T>
std::future<T> rejected(std::exception_ptr error) {
    std::promise<T> promise;
    auto result = promise.get_future();
    promise.set_exception(error);
    return result;
}

} // namespace

struct FileMonitor::Impl {
    struct Update {
        std::vector<fs::path> paths;
        std::size_t bytes{};
        std::promise<std::uint64_t> promise;
    };
    struct Match { std::size_t index{}; fs::path name; bool direct{}; };
    struct Plan { fs::path directory; DirectoryIdentity identity; std::vector<Match> matches; };
    struct Watch {
        Impl* owner{};
        uv_fs_event_t handle{};
        Plan plan;
        bool started{};
        bool active{};
        bool closing{};
    };

    fs::path requested_root;
    fs::path root;
    DirectoryIdentity root_identity;
    MonitorOptions options;
    // lifecycle serializes start/join; gate protects uv_async_send against close.
    std::mutex lifecycle;
    std::mutex gate;
    std::thread thread;
    bool running{};
    bool stop_requested{};
    bool async_ready{};
    bool finished{true};
    std::deque<std::unique_ptr<Update>> pending;
    std::size_t queued_bytes{};
    std::promise<void> stopped;
    std::shared_future<void> stopped_future;
    uv_loop_t loop{};
    uv_async_t async{};
    uv_timer_t timer{};
    bool loop_initialized{};
    bool async_initialized{};
    bool timer_initialized{};
    bool closing{};
    bool rebuild_pending{};
    std::map<Watch*, std::unique_ptr<Watch>> watches;
    std::vector<Watch*> active;
    std::vector<fs::path> paths;
    std::mutex events_mutex;
    std::uint64_t generation{};
    std::map<std::size_t, FileEvent> events;
    bool rescan_required{};
    std::optional<std::string> event_error;

    Impl(fs::path workspace_root, MonitorOptions settings)
        : requested_root(std::move(workspace_root)), options(settings) {
        if (requested_root.empty() || options.max_paths == 0 || options.max_watched_directories == 0 ||
            options.max_pending_updates == 0 || options.max_queued_path_bytes == 0 || options.max_events == 0 ||
            options.reconcile_interval.count() <= 0) { throw std::invalid_argument("invalid file monitor options"); }
        if (requested_root.native().find(fs::path::value_type{}) != requested_root.native().npos) {
            throw std::invalid_argument("workspace root contains NUL");
        }
        stopped_future = stopped.get_future().share();
        stopped.set_value();
    }

    ~Impl() {
        (void)stop();
        if (thread.joinable()) { thread.join(); }
    }

    std::unique_ptr<Update> update(std::vector<fs::path> input) const {
        if (input.size() > options.max_paths) { throw std::length_error("file monitor subscription limit exceeded"); }
        auto result = std::make_unique<Update>();
        result->paths = std::move(input);
        for (const auto& path : result->paths) {
            if (path.native().size() > options.max_queued_path_bytes / sizeof(fs::path::value_type)) {
                throw std::length_error("file monitor path queue byte limit exceeded");
            }
            const auto bytes = path.native().size() * sizeof(fs::path::value_type);
            if (bytes > options.max_queued_path_bytes - result->bytes) {
                throw std::length_error("file monitor path queue byte limit exceeded");
            }
            result->bytes += bytes;
        }
        return result;
    }

    std::future<std::uint64_t> start(std::vector<fs::path> input) {
        try {
            auto request = update(std::move(input));
            auto result = request->promise.get_future();
            std::lock_guard lifetime(lifecycle);
            {
                std::lock_guard lock(gate);
                if (!finished) { throw std::logic_error("file monitor is already running or stopping"); }
            }
            if (thread.joinable()) { thread.join(); }
            {
                std::lock_guard lock(gate);
                stopped = std::promise<void>();
                stopped_future = stopped.get_future().share();
                running = true;
                stop_requested = false;
                finished = false;
                async_ready = false;
                queued_bytes = 0;
            }
            closing = false;
            rebuild_pending = false;
            loop_initialized = false;
            async_initialized = false;
            timer_initialized = false;
            try { thread = std::thread([this, initial = std::move(request)]() mutable { run(std::move(initial)); }); }
            catch (...) {
                std::lock_guard lock(gate);
                running = false;
                finished = true;
                for (auto& item : pending) { item->promise.set_exception(std::current_exception()); }
                pending.clear();
                queued_bytes = 0;
                stopped.set_value();
                throw;
            }
            return result;
        } catch (...) { return rejected<std::uint64_t>(std::current_exception()); }
    }

    std::future<std::uint64_t> replace(std::vector<fs::path> input) {
        try {
            auto request = update(std::move(input));
            auto result = request->promise.get_future();
            std::lock_guard lock(gate);
            if (!running || stop_requested) { throw std::logic_error("file monitor is not accepting subscriptions"); }
            if (pending.size() >= options.max_pending_updates || request->bytes > options.max_queued_path_bytes - queued_bytes) {
                throw std::length_error("file monitor update queue is full");
            }
            queued_bytes += request->bytes;
            pending.push_back(std::move(request));
            if (async_ready) { (void)uv_async_send(&async); }
            return result;
        } catch (...) { return rejected<std::uint64_t>(std::current_exception()); }
    }

    std::shared_future<void> stop() noexcept {
        std::lock_guard lock(gate);
        stop_requested = true;
        if (async_ready) { (void)uv_async_send(&async); }
        return stopped_future;
    }

    void rescan(std::optional<std::string> error = std::nullopt) {
        std::lock_guard lock(events_mutex);
        rescan_required = true;
        if (error && !event_error) { event_error = error->substr(0, 4096); }
    }

    void failure(std::exception_ptr error) noexcept {
        try {
            try { std::rethrow_exception(error); }
            catch (const std::exception& detail) { rescan(detail.what()); }
            catch (...) { rescan("unknown file monitor failure"); }
        } catch (...) {
            // A failure to allocate diagnostic text must still preserve the
            // nonallocating loss signal; there is no user callback to throw into.
            std::lock_guard lock(events_mutex);
            rescan_required = true;
        }
    }

    void check_root() const {
        if (directory_identity(root) != root_identity || fs::canonical(root) != root) {
            throw std::runtime_error("workspace root changed identity; restart its file monitor");
        }
    }

    std::vector<Plan> plan(const std::vector<fs::path>& subscriptions) const {
        check_root();
        std::map<fs::path, Plan> directories;
        const auto add_chain = [&](const fs::path& relative, std::size_t index) {
            auto parent = root;
            for (auto part = relative.begin(); part != relative.end(); ++part) {
                if (!fs::exists(parent)) { break; }
                const auto resolved = fs::canonical(parent);
                (void)inside_relative(root, resolved);
                auto found = directories.find(resolved);
                if (found == directories.end()) {
                    if (directories.size() >= options.max_watched_directories) {
                        throw std::length_error("file monitor directory limit exceeded");
                    }
                    found = directories.emplace(resolved, Plan{resolved, directory_identity(resolved), {}}).first;
                }
                auto next = part;
                ++next;
                const bool direct = next == relative.end();
                auto& matches = found->second.matches;
                if (std::ranges::none_of(matches, [&](const Match& match) {
                    return match.index == index && match.name == *part && match.direct == direct;
                })) { matches.push_back({index, *part, direct}); }
                parent /= *part;
            }
        };
        for (std::size_t index = 0; index < subscriptions.size(); ++index) {
            const auto relative = normalize_relative(subscriptions[index]);
            const auto resolved = fs::weakly_canonical(root / relative);
            const auto resolved_relative = inside_relative(root, resolved);
            if (resolved_relative.empty() || fs::is_directory(resolved)) {
                throw std::invalid_argument("file monitor subscription names a directory");
            }
            add_chain(relative, index);
            // A safe in-workspace symlink needs both namespace and target events.
            if (resolved_relative != relative) { add_chain(resolved_relative, index); }
        }
        std::vector<Plan> result;
        result.reserve(directories.size());
        for (auto& [name, item] : directories) { (void)name; result.push_back(std::move(item)); }
        return result;
    }

    void retire(Watch* watch) noexcept {
        if (watch->closing) { return; }
        watch->active = false;
        watch->closing = true;
        if (watch->started) {
            const auto status = uv_fs_event_stop(&watch->handle);
            if (status < 0) { failure(std::make_exception_ptr(uv_error("stop file watcher", status))); }
        }
        uv_close(reinterpret_cast<uv_handle_t*>(&watch->handle), [](uv_handle_t* handle) {
            auto* item = static_cast<Watch*>(handle->data);
            item->owner->watches.erase(item);
        });
    }

    void install(const std::vector<fs::path>& subscriptions) {
        auto prepared = plan(subscriptions);
        std::vector<Watch*> created;
        created.reserve(prepared.size());
        try {
            for (auto& item : prepared) {
                auto owned = std::make_unique<Watch>();
                owned->owner = this;
                owned->plan = std::move(item);
                auto* watch = owned.get();
                watches.emplace(watch, std::move(owned));
                const auto init = uv_fs_event_init(&loop, &watch->handle);
                if (init < 0) { watches.erase(watch); throw uv_error("initialize file watcher", init); }
                watch->handle.data = watch;
                created.push_back(watch);
                const auto name = path_utf8(watch->plan.directory);
                const auto status = uv_fs_event_start(&watch->handle, [](uv_fs_event_t* handle,
                    const char* filename, int flags, int error) {
                    auto* value = static_cast<Watch*>(handle->data);
                    if (!value->active) { return; }
                    try { value->owner->changed(*value, filename, flags, error); }
                    catch (...) { value->owner->failure(std::current_exception()); }
                }, name.c_str(), 0);
                if (status < 0) { throw uv_error("start file watcher", status); }
                watch->started = true;
            }
        } catch (...) {
            for (auto* item : created) { retire(item); }
            throw;
        }
        for (auto* item : active) { retire(item); }
        active = std::move(created);
        for (auto* item : active) { item->active = true; }
    }

    void apply(Update& request) {
        for (auto& item : request.paths) { item = normalize_relative(item); }
        if (generation == std::numeric_limits<std::uint64_t>::max()) { throw std::overflow_error("file monitor generation exhausted"); }
        install(request.paths);
        rebuild_pending = false;
        paths = std::move(request.paths);
        {
            std::lock_guard lock(events_mutex);
            ++generation;
            events.clear();
            rescan_required = true;
            event_error.reset();
        }
        request.promise.set_value(generation);
    }

    void changed(Watch& watch, const char* filename, int flags, int status) {
        if (status < 0) {
            rescan(uv_error("file watch event", status).what());
            std::erase(active, &watch);
            retire(&watch);
            rebuild_pending = true;
        } else if (filename == nullptr || *filename == '\0') {
            rescan();
            rebuild_pending = true;
        } else {
            const auto length = std::char_traits<char>::length(filename);
            const fs::path name(std::u8string(reinterpret_cast<const char8_t*>(filename), length));
            if (name.is_absolute() || name.has_parent_path()) { rescan(); rebuild_pending = true; }
            else {
                for (const auto& match : watch.plan.matches) {
                    if (!same_component(name, match.name)) { continue; }
                    if (!match.direct) { rescan(); rebuild_pending = true; continue; }
                    std::lock_guard lock(events_mutex);
                    const auto found = events.find(match.index);
                    if (found == events.end()) {
                        if (events.size() >= options.max_events) { rescan_required = true; continue; }
                        events.emplace(match.index, FileEvent{paths.at(match.index),
                            (flags & UV_CHANGE) != 0, (flags & UV_RENAME) != 0});
                    } else {
                        found->second.content_changed |= (flags & UV_CHANGE) != 0;
                        found->second.renamed |= (flags & UV_RENAME) != 0;
                    }
                    if ((flags & (UV_CHANGE | UV_RENAME)) == 0) { rescan_required = true; }
                }
            }
        }
        if (rebuild_pending) { (void)uv_async_send(&async); }
    }

    void reconcile() {
        try {
            check_root();
        } catch (...) {
            failure(std::current_exception());
            for (auto* item : active) { retire(item); }
            active.clear();
            // A different object at the workspace root requires explicit restart;
            // keep no handles watching an old root now moved outside its boundary.
            return;
        }
        for (std::size_t index = 0; index < active.size();) {
            auto* item = active[index];
            bool stale = false;
            try {
                const auto resolved = fs::canonical(item->plan.directory);
                (void)inside_relative(root, resolved);
                if (resolved != item->plan.directory || directory_identity(resolved) != item->plan.identity) {
                    stale = true;
                }
            } catch (...) { failure(std::current_exception()); stale = true; }
            if (stale) {
                active.erase(active.begin() + static_cast<std::ptrdiff_t>(index));
                retire(item);
                rebuild_pending = true;
            } else { ++index; }
        }
        if (rebuild_pending) { rescan(); (void)uv_async_send(&async); }
    }

    void begin_close() noexcept {
        if (closing) { return; }
        closing = true;
        std::deque<std::unique_ptr<Update>> cancelled;
        {
            std::lock_guard lock(gate);
            running = false;
            async_ready = false;
            cancelled.swap(pending);
            queued_bytes = 0;
        }
        const auto error = std::make_exception_ptr(std::runtime_error("file monitor stopped before subscription update"));
        for (auto& request : cancelled) { request->promise.set_exception(error); }
        for (auto* item : active) { retire(item); }
        active.clear();
        if (timer_initialized) {
            (void)uv_timer_stop(&timer);
            uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
        }
        if (async_initialized) { uv_close(reinterpret_cast<uv_handle_t*>(&async), nullptr); }
    }

    void drain() {
        std::unique_ptr<Update> request;
        bool stop_now = false;
        {
            std::lock_guard lock(gate);
            stop_now = stop_requested;
            if (!stop_now && !pending.empty()) {
                request = std::move(pending.front());
                pending.pop_front();
                queued_bytes -= request->bytes;
            }
        }
        if (stop_now) { begin_close(); return; }
        if (request) {
            try { apply(*request); }
            catch (...) { request->promise.set_exception(std::current_exception()); }
        }
        if (rebuild_pending) {
            rebuild_pending = false;
            try { install(paths); }
            catch (...) { failure(std::current_exception()); }
        }
        std::lock_guard lock(gate);
        if (stop_requested || !pending.empty()) { (void)uv_async_send(&async); }
    }

    void run(std::unique_ptr<Update> initial) noexcept {
        try {
            int status = uv_loop_init(&loop);
            if (status < 0) { throw uv_error("initialize monitor loop", status); }
            loop_initialized = true;
            status = uv_async_init(&loop, &async, [](uv_async_t* handle) {
                auto* self = static_cast<Impl*>(handle->data);
                try { self->drain(); }
                catch (...) { self->failure(std::current_exception()); self->begin_close(); }
            });
            if (status < 0) { throw uv_error("initialize monitor wakeup", status); }
            async_initialized = true;
            async.data = this;
            status = uv_timer_init(&loop, &timer);
            if (status < 0) { throw uv_error("initialize monitor timer", status); }
            timer_initialized = true;
            timer.data = this;
            const auto interval = static_cast<std::uint64_t>(options.reconcile_interval.count());
            status = uv_timer_start(&timer, [](uv_timer_t* handle) {
                auto* self = static_cast<Impl*>(handle->data);
                try { self->reconcile(); }
                catch (...) { self->failure(std::current_exception()); }
            }, interval, interval);
            if (status < 0) { throw uv_error("start monitor timer", status); }
            {
                std::lock_guard lock(gate);
                async_ready = true;
                if (stop_requested) { throw std::runtime_error("file monitor stopped during startup"); }
            }
            root = fs::canonical(requested_root);
            root_identity = directory_identity(root);
            apply(*initial);
            initial.reset();
            // Commands may have arrived before uv_async_init became visible.
            (void)uv_async_send(&async);
        } catch (...) {
            const auto error = std::current_exception();
            if (initial) { initial->promise.set_exception(error); }
            failure(error);
            begin_close();
        }
        if (loop_initialized) {
            (void)uv_run(&loop, UV_RUN_DEFAULT);
            int result = uv_loop_close(&loop);
            if (result < 0) {
                failure(std::make_exception_ptr(uv_error("close monitor loop", result)));
                // Defensive recovery retains ownership until even an unexpected
                // initialized handle has drained; never announce stale handles.
                uv_walk(&loop, [](uv_handle_t* handle, void*) {
                    if (uv_is_closing(handle)) { return; }
                    uv_close(handle, [](uv_handle_t* closed) {
                        if (closed->type == UV_FS_EVENT) {
                            auto* item = static_cast<Watch*>(closed->data);
                            item->owner->watches.erase(item);
                        }
                    });
                }, nullptr);
                (void)uv_run(&loop, UV_RUN_DEFAULT);
                result = uv_loop_close(&loop);
                if (result < 0) { std::fputs("file monitor loop remained busy after handle drain\n", stderr); std::terminate(); }
            }
        }
        paths.clear();
        {
            std::lock_guard lock(gate);
            running = false;
            async_ready = false;
            finished = true;
            stopped.set_value();
        }
    }

    EventBatch take() {
        EventBatch result;
        std::lock_guard lock(events_mutex);
        result.generation = generation;
        result.rescan_required = rescan_required;
        result.error = std::move(event_error);
        result.events.reserve(events.size());
        for (auto& [index, item] : events) { (void)index; result.events.push_back(std::move(item)); }
        events.clear();
        event_error.reset();
        rescan_required = false;
        return result;
    }
};

FileMonitor::FileMonitor(fs::path root, MonitorOptions options)
    : impl_(std::make_unique<Impl>(std::move(root), options)) {}
FileMonitor::~FileMonitor() = default;
std::future<std::uint64_t> FileMonitor::start(std::vector<fs::path> paths) { return impl_->start(std::move(paths)); }
std::future<std::uint64_t> FileMonitor::replace_paths(std::vector<fs::path> paths) { return impl_->replace(std::move(paths)); }
EventBatch FileMonitor::take_events() { return impl_->take(); }
std::shared_future<void> FileMonitor::stop() noexcept { return impl_->stop(); }

} // namespace astra::watch
