#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace astra::config {

struct ConfigDiagnostic {
    std::size_t line{};
    std::string message;
};

struct ConfigProperty {
    std::string key;
    std::string value;
    std::size_t line{};
};

struct ConfigSection {
    std::string pattern;
    std::vector<ConfigProperty> properties;
    std::size_t line{};
};

struct EditorConfig {
    bool root{};
    std::vector<ConfigSection> sections;
    std::vector<ConfigDiagnostic> diagnostics;
};

struct EditorConfigLayer {
    // Forward-slash relative directory under a common logical root; "" means root.
    std::string directory;
    EditorConfig config;
};

enum class IndentStyle { tabs, spaces };
enum class LineEnding { lf, crlf, cr };
enum class Charset { latin1, utf8, utf8_bom, utf16_be, utf16_le };

struct EditorSettings {
    std::optional<IndentStyle> indent_style;
    std::optional<std::uint32_t> indent_size;
    std::optional<std::uint32_t> tab_width;
    bool indent_size_uses_tab{};
    std::optional<LineEnding> end_of_line;
    std::optional<Charset> charset;
    std::optional<bool> trim_trailing_whitespace;
    std::optional<bool> insert_final_newline;
    std::optional<std::string> spelling_language;
    std::optional<std::uint32_t> max_line_length;
};

struct ResolvedEditorConfig {
    // Unknown keys and values are preserved for scripts; keys are lowercase.
    // Unset entries are absent. Typed settings ignore unsupported property values.
    std::map<std::string, std::string, std::less<>> properties;
    EditorSettings settings;
};

// Pure parsing, no disk access. Invalid lines are diagnosed with 1-based lines;
// syntactically valid sections remain usable. Malformed UTF-8/NUL and size limits throw.
[[nodiscard]] EditorConfig parse_editor_config(std::string_view text);

// Case-sensitive glob matching on UTF-8 paths with forward slashes. Supports
// *, **, ?, character sets, alternatives/nesting and integer ranges. Evaluation
// has explicit input and work limits and never performs regex backtracking.
[[nodiscard]] bool glob_matches(std::string_view pattern, std::string_view relative_path);

// Layers are ordered from outer to inner. Non-ancestor layers do not apply.
// The nearest applicable root=true layer clears inherited outer settings.
// Paths must be relative; parent traversal, NUL and backslash separators are rejected.
[[nodiscard]] ResolvedEditorConfig resolve_editor_config(std::span<const EditorConfigLayer> layers,
                                                         std::string_view relative_path);

} // namespace astra::config
