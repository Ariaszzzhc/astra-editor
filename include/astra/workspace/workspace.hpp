#pragma once

#include <astra/config/editor_config.hpp>
#include <astra/core/document.hpp>
#include <astra/io/file_store.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace astra::workspace {

struct WorkspaceIssue {
    std::filesystem::path relative_path;
    std::string message;
    std::optional<std::filesystem::path> recovery_path;
};

struct TreeEntry {
    std::filesystem::path relative_path;
    bool directory{};
    bool symlink{};
    std::uintmax_t size_bytes{};
};

struct TreeOptions {
    std::size_t max_entries{100000};
    std::size_t max_depth{64};
    bool include_ignored{}; // .git is always excluded, even when true.
};

struct TreeResult {
    std::vector<TreeEntry> entries;
    std::vector<WorkspaceIssue> issues;
    bool truncated{};
    bool cancelled{};
};

struct ConfigResult {
    config::ResolvedEditorConfig config;
    std::vector<WorkspaceIssue> issues;
};

struct LoadedFile {
    std::filesystem::path relative_path;
    io::FileSnapshot snapshot;
    config::ResolvedEditorConfig config;
    std::vector<WorkspaceIssue> issues;
};

struct SavedFile {
    std::filesystem::path relative_path;
    io::FileVersion version;
    std::string text; // Exact decoded content saved after EditorConfig formatting.
    io::TextEncoding encoding{io::TextEncoding::utf8};
    config::ResolvedEditorConfig config;
    std::vector<WorkspaceIssue> issues;
};

struct SearchOptions {
    std::string query; // UTF-8 literal, case-sensitive; never interpreted as regex.
    bool whole_word{}; // Unicode letters, marks, numbers and connector punctuation.
    std::size_t max_file_bytes{2U * 1024U * 1024U};
    std::size_t max_total_bytes{64U * 1024U * 1024U};
    std::size_t max_files{10000};
    std::size_t max_matches{10000};
    TreeOptions tree;
};

struct SearchMatch {
    std::filesystem::path relative_path;
    ByteOffset begin;
    ByteOffset end;
    TextPosition start_position; // Zero-based line and UTF-16 column.
    TextPosition end_position;
    io::FileVersion version;
    io::TextEncoding encoding{io::TextEncoding::utf8};
};

struct SearchResult {
    std::string query;
    std::vector<SearchMatch> matches;
    std::vector<WorkspaceIssue> issues;
    bool truncated{};
    bool cancelled{};
};

struct ReplacementFile {
    std::filesystem::path relative_path;
    std::string text;
    io::TextEncoding encoding{io::TextEncoding::utf8};
    io::FileVersion expected;
    std::size_t replacements{};
};

struct ReplacePlan {
    std::vector<ReplacementFile> files;
};

struct ReplaceResult {
    std::vector<SavedFile> applied;
    std::vector<std::filesystem::path> pending;
    std::vector<WorkspaceIssue> issues;
    bool cancelled{};
};

// Pure ownership of an explicit directory root. These synchronous operations are
// intended for libuv workers; callers serialize workspace-wide mutation requests.
// Paths are re-resolved and checked before I/O, including symlinks/junctions.
// External filesystem namespace changes are not an OS sandbox or atomic CAS.
class Workspace {
public:
    explicit Workspace(const std::filesystem::path& root);
    [[nodiscard]] const std::filesystem::path& root() const noexcept;
    [[nodiscard]] std::filesystem::path resolve_path(const std::filesystem::path& relative) const;
    [[nodiscard]] TreeResult tree(TreeOptions options = {}, std::stop_token stop = {}) const;
    [[nodiscard]] ConfigResult editor_config(const std::filesystem::path& relative) const;
    [[nodiscard]] LoadedFile load(const std::filesystem::path& relative,
                                  std::size_t max_bytes = io::default_max_file_bytes) const;
    [[nodiscard]] SavedFile save(const std::filesystem::path& relative, std::string_view text,
                                std::optional<io::FileVersion> expected = std::nullopt,
                                io::TextEncoding original_encoding = io::TextEncoding::utf8) const;
    [[nodiscard]] SearchResult search(const SearchOptions& options, std::stop_token stop = {}) const;
    [[nodiscard]] ReplacePlan plan_replace(const SearchResult& search, std::string_view replacement,
                                          std::stop_token stop = {}) const;
    // Preflight checks every file version before the first write. Each save is
    // atomic, but a multi-file operation is not; partial completion is explicit.
    // The service must handle open dirty documents before invoking this disk API.
    [[nodiscard]] ReplaceResult apply_replace(const ReplacePlan& plan, std::stop_token stop = {}) const;

private:
    [[nodiscard]] SavedFile prepare_save(const std::filesystem::path& relative,
                                        std::string_view text, io::TextEncoding original_encoding) const;
    std::filesystem::path root_;
};

} // namespace astra::workspace
