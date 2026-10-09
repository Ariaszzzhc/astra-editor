#pragma once

#include <astra/core/document.hpp>
#include <astra/io/file_store.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace astra::recovery {

struct RecoveryRecord {
    DocumentSnapshot document;
    std::optional<std::filesystem::path> path;
    io::TextEncoding encoding{io::TextEncoding::utf8};
    // Persist the exact original encoded bytes, not a timestamp/hash. Restoring
    // does not authorize overwriting a disk file that changed after checkpoint.
    std::optional<io::FileVersion> disk_version;
    // Owner-assigned frame epoch, independent of text revision. Zero preserves
    // legacy revision ordering; explicit epochs must increase within a session.
    std::uint64_t checkpoint_sequence{};
};

struct RecoveryLimits {
    std::size_t max_text_bytes{64 * 1024 * 1024};
    std::size_t max_disk_bytes{64 * 1024 * 1024};
    std::size_t max_record_bytes{256 * 1024 * 1024};
    std::size_t max_records{4096};
    std::size_t max_sessions{256};
};

struct RecoveryIssue {
    std::filesystem::path path;
    std::string message;
};

struct SessionInfo {
    std::string id;
    bool active{};
    std::vector<BufferId> buffers;
    // Corrupt/unfinished records stay on disk and are reported explicitly.
    std::vector<RecoveryIssue> issues;
};

struct PruneResult {
    std::size_t removed{};
    std::vector<RecoveryIssue> issues;
};

class RecoveryLocked final : public std::runtime_error {
public:
    RecoveryLocked();
};

class RecoveryCorrupt final : public std::runtime_error {
public:
    explicit RecoveryCorrupt(const std::string& message);
};

// Worker-only, one owner per instance. The host debounces immutable snapshots;
// checkpoint serializes text once, never on the input/render thread. A restored
// Document must be marked dirty (invalidate_saved_state) by its service owner.
// No method reads or writes the source document's path: only state_directory.
class RecoveryStore {
public:
    // nullopt creates a new isolated session. A supplied ID claims an abandoned
    // session under an exclusive OS lock; a live owner is never overwritten.
    explicit RecoveryStore(std::filesystem::path state_directory,
        std::optional<std::string> claim_session = std::nullopt,
        RecoveryLimits limits = {});
    ~RecoveryStore();
    RecoveryStore(const RecoveryStore&) = delete;
    RecoveryStore& operator=(const RecoveryStore&) = delete;
    RecoveryStore(RecoveryStore&&) noexcept;
    RecoveryStore& operator=(RecoveryStore&&) noexcept;

    [[nodiscard]] const std::string& session_id() const noexcept;
    [[nodiscard]] const std::filesystem::path& session_directory() const noexcept;
    void checkpoint(const RecoveryRecord& record);
    [[nodiscard]] std::optional<RecoveryRecord> load(BufferId id) const;
    [[nodiscard]] SessionInfo inspect() const;
    // Revision-guarded removal: a completed save of an older document must not
    // delete a newer dirty checkpoint. Durable tombstone precedes unlink.
    void erase(BufferId id, Revision expected_revision);

    [[nodiscard]] static std::vector<SessionInfo> discover(
        const std::filesystem::path& state_directory, RecoveryLimits limits = {});
    // Call before creating a session or after destroying a normally closed
    // store. Only abandoned sessions without dirty records qualify; valid erase
    // tombstones can be pruned, but live owners and corrupt evidence remain.
    [[nodiscard]] static PruneResult prune_empty_sessions(
        const std::filesystem::path& state_directory, RecoveryLimits limits = {});

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::recovery
