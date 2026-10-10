#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace astra::watch {

struct MonitorOptions {
    std::size_t max_paths{4096};
    std::size_t max_watched_directories{8192};
    std::size_t max_pending_updates{64};
    std::size_t max_queued_path_bytes{4 * 1024 * 1024};
    std::size_t max_events{1024};
    // Directory metadata only: detects a replaced/deleted watched directory even
    // if the native backend could not identify the namespace event.
    std::chrono::milliseconds reconcile_interval{1000};
};

struct FileEvent {
    std::filesystem::path relative_path;
    bool content_changed{};
    // Includes possible creation, deletion and replacement. The consumer must
    // check the disk version; native notifications do not reliably distinguish.
    bool renamed{};
};

struct EventBatch {
    std::uint64_t generation{};
    std::vector<FileEvent> events;
    bool rescan_required{};
    // First backend/reconciliation error since the previous take_events.
    std::optional<std::string> error;
};

// One private libuv thread owns all watchers. Only explicit workspace-relative
// files are reported; their parent/ancestor directories are watched without
// recursive-backend assumptions. No source file content is read here.
//
// start/replace_paths futures acknowledge fully installed subscriptions. Each
// success changes generation, drops old events and requests a complete version
// check, covering the subscription handover. A failed replacement preserves the
// previous subscriptions. Events are bounded and coalesced by subscribed path;
// loss/unknown names explicitly require a rescan rather than pretending exactness.
// Public methods are thread-safe. Destruction stops and joins the private loop.
class FileMonitor {
  public:
    explicit FileMonitor(std::filesystem::path workspace_root, MonitorOptions options = {});
    ~FileMonitor();
    FileMonitor(const FileMonitor&) = delete;
    FileMonitor& operator=(const FileMonitor&) = delete;
    FileMonitor(FileMonitor&&) = delete;
    FileMonitor& operator=(FileMonitor&&) = delete;

    // May restart the same object after stop().get(). Paths must be relative and
    // may name missing files/directories; the nearest existing ancestors remain
    // watched so subsequent creation can establish the more specific watches.
    [[nodiscard]] std::future<std::uint64_t>
    start(std::vector<std::filesystem::path> relative_paths = {});
    [[nodiscard]] std::future<std::uint64_t>
    replace_paths(std::vector<std::filesystem::path> relative_paths);
    [[nodiscard]] EventBatch take_events();
    // Nonblocking and idempotent. Ready only after all uv_close callbacks and the
    // loop have completed. Queued updates are completed with an exception.
    [[nodiscard]] std::shared_future<void> stop() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::watch
