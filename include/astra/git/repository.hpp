#pragma once

#include <astra/io/file_store.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace astra::git {

struct GitOptions {
    std::stop_token stop;
    std::chrono::milliseconds timeout{10'000};
    std::size_t max_output_bytes{16 * 1024 * 1024};
    std::size_t max_entries{100'000};
    // Per-operation identity override; affects this child process only.
    std::optional<std::string> author_name;
    std::optional<std::string> author_email;
};

struct RepositoryOptions {
    // Normal product behavior uses the user's Git configuration. Isolated
    // automated tests must explicitly set false to avoid external config reads.
    bool inherit_global_config{true};
};

class GitError : public std::runtime_error {
public:
    GitError(std::string message, std::int64_t exit_status);
    [[nodiscard]] std::int64_t exit_status() const noexcept;
private:
    std::int64_t exit_status_;
};

class GitCancelled final : public std::runtime_error {
public:
    GitCancelled();
};

enum class FileState { unchanged, modified, added, deleted, renamed, copied, unmerged, untracked, ignored, type_changed };
struct StatusEntry {
    std::filesystem::path path;
    std::optional<std::filesystem::path> original_path;
    FileState staged{FileState::unchanged};
    FileState unstaged{FileState::unchanged};
    bool conflict{};
    std::string submodule;
};

struct RepositoryStatus {
    std::string branch;
    std::string head;
    std::string upstream;
    bool unborn{};
    bool detached{};
    std::size_t ahead{};
    std::size_t behind{};
    std::vector<StatusEntry> entries;
};

enum class DiffBase { index, head };
struct DiffHunk {
    // Git unified-diff coordinates: one-based starts; zero is valid for an
    // empty side. Counts are explicit even when Git omits ",1" in its output.
    std::size_t old_start{};
    std::size_t old_count{};
    std::size_t new_start{};
    std::size_t new_count{};
};
enum class GutterKind { added, modified, deleted };
struct GutterMark {
    std::size_t line{}; // Zero-based new-text line, or deletion gap after N lines.
    std::size_t count{};
    std::size_t removed_lines{};
    GutterKind kind;
};
struct DiffResult {
    std::string patch;
    std::vector<DiffHunk> hunks;
    std::vector<GutterMark> gutter;
    bool binary{};
};

struct CommitInfo {
    std::string id;
    std::vector<std::string> parents;
    std::string author;
    std::string email;
    std::int64_t timestamp{};
    std::string subject;
};

struct BranchInfo {
    std::string ref;
    std::string commit;
    std::string upstream;
    bool current{};
    bool remote{};
};

struct StagePath {
    std::filesystem::path path;
    // Optional guard against staging a different on-disk text snapshot. A
    // missing token permits Git's ordinary current-worktree staging semantics.
    std::optional<io::FileVersion> expected;
};

// Synchronous worker service over the shared asynchronous libuv process adapter.
// An explicit canonical working-tree root is required: no parent-repo discovery.
// Git receives an argv array and sanitized child-only environment. System/global
// Git config may be explicitly isolated; hooks and fsmonitor are disabled. Repository config/attributes and
// explicitly requested add/restore filters retain standard Git behavior.
class Repository {
public:
    explicit Repository(std::filesystem::path root, std::string executable = "git",
                        RepositoryOptions settings = {});
    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] RepositoryStatus status(const GitOptions& options = {}) const;
    [[nodiscard]] DiffResult diff(const std::filesystem::path& path,
        DiffBase base = DiffBase::index, bool staged = false,
        const GitOptions& options = {}) const;
    [[nodiscard]] std::vector<CommitInfo> log(std::size_t limit = 100,
        const GitOptions& options = {}) const;
    [[nodiscard]] std::vector<BranchInfo> branches(const GitOptions& options = {}) const;
    [[nodiscard]] std::string read_blob(const std::filesystem::path& path,
        DiffBase source = DiffBase::index, const GitOptions& options = {}) const;
    void stage(std::span<const StagePath> paths, const GitOptions& options = {}) const;
    void unstage(std::span<const std::filesystem::path> paths, const GitOptions& options = {}) const;
    void create_branch(std::string name, bool checkout = false, const GitOptions& options = {}) const;
    void switch_branch(std::string name, const GitOptions& options = {}) const;
    // Commits only the existing index. Does not stage files, run hooks, or run
    // network operations. User signing config remains effective; failures retain
    // Git's stderr and leave dirty worktree content alone.
    void commit(std::string message, const GitOptions& options = {}) const;
    // Explicit destructive user action only. Restore the index version through
    // Git's working-tree filters/EOL rules, then atomically save iff expected
    // still matches. nullopt explicitly means the path must remain absent.
    // Binary content, unmerged index entries and index deletions fail intact.
    [[nodiscard]] io::FileSnapshot discard(const std::filesystem::path& path,
        std::optional<io::FileVersion> expected,
        std::optional<io::TextEncoding> encoding_override = std::nullopt,
        const GitOptions& options = {}) const;

private:
    struct Output { std::string out; std::string error; std::int64_t status; };
    std::filesystem::path root_;
    std::string executable_;
    RepositoryOptions settings_;
    [[nodiscard]] Output run(std::vector<std::string> arguments, const GitOptions& options,
                            std::string input = {}, bool allow_failure = false) const;
    [[nodiscard]] std::string relative_path(const std::filesystem::path& path) const;
    void verify_branch(const std::string& name, const GitOptions& options) const;
};

// Pure parsers are public for protocol/failure-path tests and fuzzing.
[[nodiscard]] RepositoryStatus parse_status(std::string_view porcelain,
                                           std::size_t max_entries = 100'000);
[[nodiscard]] DiffResult parse_diff(std::string patch, std::size_t max_hunks = 100'000);

} // namespace astra::git
