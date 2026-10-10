#pragma once

#include <astra/config/editor_config.hpp>
#include <astra/core/document.hpp>
#include <astra/io/file_store.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
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
    // CR-only files use LF in editor text; version still owns exact disk bytes.
    io::FileSnapshot snapshot;
    config::ResolvedEditorConfig config;
    std::vector<WorkspaceIssue> issues;
};

struct SavedFile {
    std::filesystem::path relative_path;
    io::FileVersion version;
    std::string text; // Editor text after formatting; disk-only CR endings use LF here.
    io::TextEncoding encoding{io::TextEncoding::utf8};
    config::ResolvedEditorConfig config;
    std::vector<WorkspaceIssue> issues;
};

struct SearchOptions {
    std::string query; // UTF-8 literal unless regex is enabled.
    bool whole_word{}; // Unicode letters, marks, numbers and connector punctuation.
    std::size_t max_file_bytes{2U * 1024U * 1024U};
    std::size_t max_total_bytes{64U * 1024U * 1024U};
    std::size_t max_files{10000};
    std::size_t max_matches{10000};
    TreeOptions tree;
    bool case_sensitive{true};
    bool regex{};
    std::size_t max_result_bytes{8U * 1024U * 1024U}; // Match metadata, including capture ranges.
    std::vector<std::filesystem::path>
        excluded_paths; // Internal buffer overlays; skipped before I/O/budgets.
};

struct TextRange {
    ByteOffset begin;
    ByteOffset end;
    auto operator<=>(const TextRange&) const = default;
};

struct TextMatch {
    ByteOffset begin;
    ByteOffset end;
    // Capture zero is the whole match; nullopt denotes an unmatched group.
    // Ranges refer to the original UTF-8 text, never a folded/transformed copy.
    std::vector<std::optional<TextRange>> captures;
};

struct TextSearchResult {
    std::vector<TextMatch> matches;
    bool truncated{};
    bool cancelled{};
};

struct TextFindResult {
    std::optional<TextMatch> match;
    bool wrapped{};
    bool truncated{};
    bool cancelled{};
};

// Reusable pure-text search for disk files and immutable unsaved buffer copies.
// RE2 syntax, Unicode simple case folding (locale independent; no normalization
// or multi-scalar folds such as ss/ssharp), bounded compiled memory, no backtracking.
// Regex \\C is rejected because it can split UTF-8 scalars. \\b remains RE2's ASCII
// boundary; whole_word applies the editor's Unicode word definition instead.
// Empty-width matches advance by one Unicode scalar and include EOF once.
// Overlapping rejected whole-word candidates have a 256 MiB scan-work budget;
// exhaustion returns truncated, so adversarial filtering cannot become quadratic.
class SearchPattern {
  public:
    static constexpr std::size_t max_query_bytes = 64U * 1024U;
    static constexpr std::size_t max_input_bytes = 64U * 1024U * 1024U;
    static constexpr std::size_t max_capture_groups = 64;
    static constexpr std::size_t max_replacement_bytes = 64U * 1024U;
    static constexpr std::size_t max_output_bytes = 128U * 1024U * 1024U;

    explicit SearchPattern(const SearchOptions& options);
    ~SearchPattern();
    SearchPattern(SearchPattern&&) noexcept;
    SearchPattern& operator=(SearchPattern&&) noexcept;
    SearchPattern(const SearchPattern&) = delete;
    SearchPattern& operator=(const SearchPattern&) = delete;
    // Cancellation is observed during validation and between bounded linear RE2
    // calls; RE2 does not expose interruption inside one call.
    [[nodiscard]] TextSearchResult
    find_all(std::string_view text,
             std::stop_token stop = {},
             std::size_t max_matches = std::numeric_limits<std::size_t>::max()) const;
    // Seeks from a UTF-8 scalar boundary, optionally wrapping to the beginning.
    // Regex anchors and word boundaries always use the complete original input.
    // Retains at most one match, independent of the number of earlier matches.
    [[nodiscard]] TextFindResult find_next(std::string_view text,
                                           ByteOffset begin,
                                           bool wrap = true,
                                           std::stop_token stop = {}) const;
    [[nodiscard]] TextFindResult find_previous(std::string_view text,
                                               ByteOffset begin,
                                               bool wrap = true,
                                               std::stop_token stop = {}) const;
    // Literal mode preserves replacement bytes. Regex mode accepts $0..$99,
    // ${name}, and $$; backslashes are literal. Unknown/malformed groups fail.
    // Revalidates the exact match/captures against the original UTF-8 text.
    void validate_replacement(std::string_view replacement) const;
    [[nodiscard]] std::string expand_replacement(std::string_view text,
                                                 const TextMatch& match,
                                                 std::string_view replacement,
                                                 std::size_t max_bytes = max_output_bytes) const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct SearchMatch {
    std::filesystem::path relative_path;
    ByteOffset begin;
    ByteOffset end;
    TextPosition start_position; // Zero-based line and UTF-16 column.
    TextPosition end_position;
    io::FileVersion version;
    io::TextEncoding encoding{io::TextEncoding::utf8};
    std::vector<std::optional<TextRange>> captures;
};

struct SearchResult {
    std::string query;
    std::vector<SearchMatch> matches;
    std::vector<WorkspaceIssue> issues;
    bool truncated{};
    bool cancelled{};
    SearchOptions options; // Exact matching semantics used to validate replacements.
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
    [[nodiscard]] SavedFile save(const std::filesystem::path& relative,
                                 std::string_view text,
                                 std::optional<io::FileVersion> expected = std::nullopt,
                                 io::TextEncoding original_encoding = io::TextEncoding::utf8) const;
    [[nodiscard]] SearchResult search(const SearchOptions& options,
                                      std::stop_token stop = {}) const;
    [[nodiscard]] ReplacePlan plan_replace(const SearchResult& search,
                                           std::string_view replacement,
                                           std::stop_token stop = {}) const;
    // Preflight checks every file version before the first write. Each save is
    // atomic, but a multi-file operation is not; partial completion is explicit.
    // The service must handle open dirty documents before invoking this disk API.
    [[nodiscard]] ReplaceResult apply_replace(const ReplacePlan& plan,
                                              std::stop_token stop = {}) const;

  private:
    struct PreparedSave {
        SavedFile result;
        // Only allocated when disk CR endings differ from editor LF text.
        std::optional<std::string> disk_text;

        [[nodiscard]] std::string_view text_for_disk() const noexcept {
            return disk_text ? std::string_view(*disk_text) : std::string_view(result.text);
        }
    };

    [[nodiscard]] PreparedSave prepare_save(const std::filesystem::path& relative,
                                            std::string_view text,
                                            io::TextEncoding original_encoding,
                                            const std::optional<io::FileVersion>& expected) const;
    std::filesystem::path root_;
};

} // namespace astra::workspace
