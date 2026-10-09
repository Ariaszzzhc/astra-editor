#include <astra/config/editor_config.hpp>

#include <astra/core/utf8.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace astra::config {
namespace {

constexpr std::size_t max_config_bytes = 1024U * 1024U;
constexpr std::size_t max_pattern_bytes = 4096;
constexpr std::size_t max_path_bytes = 32768;
constexpr std::size_t max_glob_work = 8U * 1024U * 1024U;

bool whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\r' ||
        character == '\n' || character == '\v' || character == '\f';
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && whitespace(text.front())) { text.remove_prefix(1); }
    while (!text.empty() && whitespace(text.back())) { text.remove_suffix(1); }
    return text;
}

std::string lowercase(std::string_view text) {
    std::string result(text);
    for (auto& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

bool known_property(std::string_view key) {
    return key == "indent_style" || key == "indent_size" || key == "tab_width" ||
        key == "end_of_line" || key == "charset" || key == "trim_trailing_whitespace" ||
        key == "insert_final_newline" || key == "root" || key == "max_line_length";
}

std::u32string scalars(std::string_view text) {
    std::u32string result;
    result.reserve(text.size());
    for (std::size_t offset = 0; offset < text.size();) {
        const auto scalar = utf8::decode(text, offset);
        if (scalar.value == U'\0') { throw std::invalid_argument("EditorConfig text contains NUL"); }
        result.push_back(scalar.value);
        offset += scalar.bytes;
    }
    return result;
}

std::string normalized_relative(std::string_view path, bool allow_empty = false) {
    if (path.size() > max_path_bytes) { throw std::length_error("EditorConfig target path is too long"); }
    utf8::validate(path);
    if (path.find('\0') != std::string_view::npos || path.find('\\') != std::string_view::npos ||
        path.starts_with('/') || (path.size() >= 2 && path[1] == ':')) {
        throw std::invalid_argument("EditorConfig paths must be relative with forward slashes");
    }
    std::string result;
    for (std::size_t begin = 0; begin <= path.size();) {
        const auto slash = path.find('/', begin);
        const auto end = slash == std::string_view::npos ? path.size() : slash;
        const auto part = path.substr(begin, end - begin);
        if (part == "..") { throw std::invalid_argument("EditorConfig paths cannot traverse parents"); }
        if (!part.empty() && part != ".") {
            if (!result.empty()) { result += '/'; }
            result += part;
        }
        if (slash == std::string_view::npos) { break; }
        begin = slash + 1;
    }
    if (!allow_empty && (result.empty() || path.ends_with('/'))) {
        throw std::invalid_argument("EditorConfig target must name a file");
    }
    return result;
}

enum class Kind { literal, one, star, recursive_star, directories, character_set, integer, alternatives };
struct Node;
using Sequence = std::vector<Node>;
struct Node {
    Kind kind{Kind::literal};
    char32_t literal{};
    std::u32string characters;
    bool negated{};
    std::int64_t minimum{};
    std::int64_t maximum{};
    std::vector<Sequence> alternatives;
};

std::optional<std::int64_t> integer(std::u32string_view text) {
    if (text.empty() || text.size() > 21) { return std::nullopt; }
    std::string number;
    number.reserve(text.size());
    for (const auto character : text) {
        if (character > 127) { return std::nullopt; }
        number.push_back(static_cast<char>(character));
    }
    std::string_view view(number);
    if (view.starts_with('+')) { view.remove_prefix(1); }
    if (view.empty()) { return std::nullopt; }
    std::int64_t value{};
    const auto [end, error] = std::from_chars(view.data(), view.data() + view.size(), value);
    if (error != std::errc{} || end != view.data() + view.size()) { return std::nullopt; }
    return value;
}

std::size_t class_end(std::u32string_view pattern, std::size_t begin) {
    auto index = begin + 1;
    if (index < pattern.size() && pattern[index] == U'!') { ++index; }
    if (index < pattern.size() && pattern[index] == U']') { ++index; }
    for (; index < pattern.size(); ++index) {
        if (pattern[index] == U']') { return index; }
    }
    return std::u32string_view::npos;
}

std::size_t brace_end(std::u32string_view pattern, std::size_t begin) {
    std::size_t level = 1;
    for (auto index = begin + 1; index < pattern.size(); ++index) {
        if (pattern[index] == U'\\' && index + 1 < pattern.size()) { ++index; continue; }
        if (pattern[index] == U'[') {
            const auto end = class_end(pattern, index);
            if (end != std::u32string_view::npos) { index = end; continue; }
        }
        if (pattern[index] == U'{') { ++level; }
        if (pattern[index] == U'}' && --level == 0) { return index; }
    }
    return std::u32string_view::npos;
}

Sequence compile(std::u32string_view pattern, std::size_t depth = 0) {
    if (depth > 64) { throw std::length_error("EditorConfig brace nesting exceeds its limit"); }
    Sequence result;
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        const auto character = pattern[index];
        Node node;
        node.literal = character;
        if (character == U'\\' && index + 1 < pattern.size()) {
            node.literal = pattern[++index];
        } else if (character == U'*') {
            node.kind = Kind::star;
            if (index + 1 < pattern.size() && pattern[index + 1] == U'*') {
                node.kind = Kind::recursive_star;
                ++index;
                if (index + 1 < pattern.size() && pattern[index + 1] == U'/' &&
                    (result.empty() || (result.back().kind == Kind::literal && result.back().literal == U'/'))) {
                    node.kind = Kind::directories;
                    ++index;
                }
            }
        } else if (character == U'?') {
            node.kind = Kind::one;
        } else if (character == U'[') {
            const auto end = class_end(pattern, index);
            if (end != std::u32string_view::npos) {
                auto content = pattern.substr(index + 1, end - index - 1);
                if (content.find(U'/') == std::u32string_view::npos) {
                    node.kind = Kind::character_set;
                    node.negated = content.starts_with(U'!');
                    if (node.negated) { content.remove_prefix(1); }
                    node.characters = content;
                    index = end;
                }
            }
        } else if (character == U'{') {
            const auto end = brace_end(pattern, index);
            if (end != std::u32string_view::npos) {
                const auto content = pattern.substr(index + 1, end - index - 1);
                std::vector<std::u32string_view> choices;
                std::size_t begin = 0;
                for (std::size_t cursor = 0; cursor < content.size(); ++cursor) {
                    if (content[cursor] == U'\\' && cursor + 1 < content.size()) { ++cursor; continue; }
                    if (content[cursor] == U'[') {
                        const auto bracket = class_end(content, cursor);
                        if (bracket != std::u32string_view::npos) { cursor = bracket; continue; }
                    }
                    if (content[cursor] == U'{') {
                        const auto brace = brace_end(content, cursor);
                        if (brace != std::u32string_view::npos) { cursor = brace; continue; }
                    }
                    if (content[cursor] == U',') {
                        choices.push_back(content.substr(begin, cursor - begin));
                        begin = cursor + 1;
                    }
                }
                if (!choices.empty()) {
                    choices.push_back(content.substr(begin));
                    node.kind = Kind::alternatives;
                    for (const auto choice : choices) { node.alternatives.push_back(compile(choice, depth + 1)); }
                    index = end;
                } else {
                    const auto dots = content.find(U"..");
                    if (dots != std::u32string_view::npos) {
                        const auto minimum = integer(content.substr(0, dots));
                        const auto maximum = integer(content.substr(dots + 2));
                        if (minimum && maximum && *minimum < *maximum) {
                            node.kind = Kind::integer;
                            node.minimum = *minimum;
                            node.maximum = *maximum;
                            index = end;
                        }
                    }
                }
            }
        }
        result.push_back(std::move(node));
    }
    return result;
}

bool has_path_separator(std::u32string_view pattern) {
    for (std::size_t index = 0; index < pattern.size(); ++index) {
        if (pattern[index] == U'\\' && index + 1 < pattern.size()) {
            if (pattern[index + 1] == U'/') { return true; }
            ++index;
            continue;
        }
        if (pattern[index] == U'[') {
            const auto end = class_end(pattern, index);
            if (end != std::u32string_view::npos) { index = end; continue; }
        }
        if (pattern[index] == U'/') { return true; }
    }
    return false;
}

bool contains(const Node& node, char32_t character) {
    bool found = false;
    for (std::size_t index = 0; index < node.characters.size(); ++index) {
        if (index + 2 < node.characters.size() && node.characters[index + 1] == U'-') {
            found = found || (character >= node.characters[index] && character <= node.characters[index + 2]);
            index += 2;
        } else {
            found = found || character == node.characters[index];
        }
    }
    return node.negated ? !found : found;
}

using Positions = std::vector<unsigned char>;

Positions evaluate(const Sequence& sequence, std::u32string_view path,
                   Positions positions, std::size_t& work) {
    for (const auto& node : sequence) {
        if (path.size() + 1 > work) { throw std::length_error("EditorConfig glob work limit exceeded"); }
        work -= path.size() + 1;
        Positions next(path.size() + 1);
        if (node.kind == Kind::alternatives) {
            for (const auto& branch : node.alternatives) {
                if (path.size() + 1 > work) { throw std::length_error("EditorConfig glob work limit exceeded"); }
                work -= path.size() + 1;
                const auto result = evaluate(branch, path, positions, work);
                for (std::size_t index = 0; index < next.size(); ++index) { next[index] |= result[index]; }
            }
        } else if (node.kind == Kind::star || node.kind == Kind::recursive_star || node.kind == Kind::directories) {
            bool active = false;
            for (std::size_t index = 0; index <= path.size(); ++index) {
                active = active || positions[index] != 0;
                if (node.kind == Kind::directories) {
                    next[index] = static_cast<unsigned char>(positions[index] != 0 ||
                        (active && index > 0 && path[index - 1] == U'/'));
                } else { next[index] = static_cast<unsigned char>(active); }
                if (node.kind == Kind::star && index < path.size() && path[index] == U'/') { active = false; }
            }
        } else {
            for (std::size_t index = 0; index < path.size(); ++index) {
                if (positions[index] == 0) { continue; }
                if (node.kind == Kind::integer) {
                    if (work < 1024) { throw std::length_error("EditorConfig glob work limit exceeded"); }
                    work -= 1024;
                    auto end = index;
                    if (path[end] == U'-') { ++end; }
                    const auto digits = end;
                    while (end < path.size() && path[end] >= U'0' && path[end] <= U'9' && end - index < 20) {
                        ++end;
                        if (end - digits > 1 && path[digits] == U'0') { break; }
                        const auto value = integer(path.substr(index, end - index));
                        if (value && *value >= node.minimum && *value <= node.maximum &&
                            !(index != digits && *value == 0)) { next[end] = 1; }
                    }
                } else {
                    if (node.kind == Kind::character_set) {
                        if (node.characters.size() > work) { throw std::length_error("EditorConfig glob work limit exceeded"); }
                        work -= node.characters.size();
                    }
                    const bool match = node.kind == Kind::literal ? path[index] == node.literal :
                        path[index] != U'/' && (node.kind == Kind::one || contains(node, path[index]));
                    if (match) { next[index + 1] = 1; }
                }
            }
        }
        positions = std::move(next);
    }
    return positions;
}

bool match_pattern(std::string_view pattern, std::u32string_view path, std::size_t& work) {
    if (pattern.size() > max_pattern_bytes) { throw std::length_error("EditorConfig section glob is too long"); }
    auto glob = scalars(pattern);
    if (glob.empty() || glob.back() == U'/') { return false; }
    if (path.size() + 1 > work) { throw std::length_error("EditorConfig glob work limit exceeded"); }
    work -= path.size() + 1;
    const bool anchored = has_path_separator(glob);
    if (glob.front() == U'/') { glob.erase(glob.begin()); }
    const auto sequence = compile(glob);
    Positions positions(path.size() + 1);
    positions[0] = 1;
    if (!anchored) {
        for (std::size_t index = 0; index < path.size(); ++index) {
            if (path[index] == U'/') { positions[index + 1] = 1; }
        }
    }
    return evaluate(sequence, path, std::move(positions), work).back() != 0;
}

std::optional<std::uint32_t> positive_integer(std::string_view text) {
    if (text.empty()) { return std::nullopt; }
    std::uint32_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0) { return std::nullopt; }
    return value;
}

EditorSettings typed_settings(const ResolvedEditorConfig& config, const std::set<std::string, std::less<>>& unset) {
    const auto value = [&config](std::string_view key) -> std::string_view {
        const auto found = config.properties.find(key);
        return found == config.properties.end() ? std::string_view{} : std::string_view(found->second);
    };
    const auto boolean = [&value](std::string_view key) -> std::optional<bool> {
        if (value(key) == "true") { return true; }
        if (value(key) == "false") { return false; }
        return std::nullopt;
    };
    EditorSettings settings;
    if (value("indent_style") == "tab") { settings.indent_style = IndentStyle::tabs; }
    if (value("indent_style") == "space") { settings.indent_style = IndentStyle::spaces; }
    settings.indent_size = positive_integer(value("indent_size"));
    settings.tab_width = positive_integer(value("tab_width"));
    settings.indent_size_uses_tab = value("indent_size") == "tab" ||
        (settings.indent_style == IndentStyle::tabs && !config.properties.contains("indent_size") && !unset.contains("indent_size"));
    if (!settings.tab_width && !unset.contains("tab_width") && settings.indent_size) {
        settings.tab_width = settings.indent_size;
    }
    if (settings.indent_size_uses_tab && settings.tab_width) { settings.indent_size = settings.tab_width; }
    if (value("end_of_line") == "lf") { settings.end_of_line = LineEnding::lf; }
    if (value("end_of_line") == "crlf") { settings.end_of_line = LineEnding::crlf; }
    if (value("end_of_line") == "cr") { settings.end_of_line = LineEnding::cr; }
    if (value("charset") == "latin1") { settings.charset = Charset::latin1; }
    if (value("charset") == "utf-8") { settings.charset = Charset::utf8; }
    if (value("charset") == "utf-8-bom") { settings.charset = Charset::utf8_bom; }
    if (value("charset") == "utf-16be") { settings.charset = Charset::utf16_be; }
    if (value("charset") == "utf-16le") { settings.charset = Charset::utf16_le; }
    settings.trim_trailing_whitespace = boolean("trim_trailing_whitespace");
    settings.insert_final_newline = boolean("insert_final_newline");
    settings.max_line_length = positive_integer(value("max_line_length"));
    const auto language = value("spelling_language");
    const auto letter = [](char character) {
        return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
    };
    if ((language.size() == 2 || (language.size() == 5 && language[2] == '-' && letter(language[3]) && letter(language[4]))) &&
        letter(language[0]) && letter(language[1])) { settings.spelling_language = std::string(language); }
    return settings;
}

} // namespace

EditorConfig parse_editor_config(std::string_view text) {
    if (text.size() > max_config_bytes) { throw std::length_error("EditorConfig exceeds its byte limit"); }
    utf8::validate(text);
    if (text.find('\0') != std::string_view::npos) { throw std::invalid_argument("EditorConfig contains NUL"); }
    if (text.starts_with("\xef\xbb\xbf")) { text.remove_prefix(3); }
    EditorConfig result;
    for (std::size_t begin = 0, line_number = 1; begin <= text.size(); ++line_number) {
        const auto newline = text.find('\n', begin);
        const auto end = newline == std::string_view::npos ? text.size() : newline;
        const auto line = trim(text.substr(begin, end - begin));
        if (!line.empty() && line.front() != '#' && line.front() != ';') {
            if (line.front() == '[' && line.back() == ']') {
                const auto pattern = line.substr(1, line.size() - 2);
                if (pattern.size() > max_pattern_bytes) { throw std::length_error("EditorConfig section glob is too long"); }
                result.sections.push_back({std::string(pattern), {}, line_number});
            } else if (const auto separator = line.find('='); separator != std::string_view::npos) {
                const auto key = lowercase(trim(line.substr(0, separator)));
                auto property_value = std::string(trim(line.substr(separator + 1)));
                if (key.size() > 4096 || property_value.size() > 16384) { throw std::length_error("EditorConfig key or value is too long"); }
                if (key.empty()) { result.diagnostics.push_back({line_number, "property key is empty"}); }
                else if (result.sections.empty()) {
                    if (key == "root") { result.root = lowercase(property_value) == "true"; }
                } else {
                    if (known_property(key) || lowercase(property_value) == "unset") { property_value = lowercase(property_value); }
                    result.sections.back().properties.push_back({key, std::move(property_value), line_number});
                }
            } else { result.diagnostics.push_back({line_number, "expected a section header or key=value pair"}); }
        }
        if (newline == std::string_view::npos) { break; }
        begin = newline + 1;
    }
    return result;
}

bool glob_matches(std::string_view pattern, std::string_view relative_path) {
    const auto path = scalars(normalized_relative(relative_path));
    auto work = max_glob_work;
    return match_pattern(pattern, path, work);
}

ResolvedEditorConfig resolve_editor_config(std::span<const EditorConfigLayer> layers,
                                          std::string_view relative_path) {
    const auto target = normalized_relative(relative_path);
    ResolvedEditorConfig result;
    std::set<std::string, std::less<>> unset;
    auto work = max_glob_work;
    for (const auto& layer : layers) {
        const auto directory = normalized_relative(layer.directory, true);
        std::string_view relative(target);
        if (!directory.empty()) {
            if (!target.starts_with(directory) || target.size() <= directory.size() || target[directory.size()] != '/') { continue; }
            relative.remove_prefix(directory.size() + 1);
        }
        if (layer.config.root) { result.properties.clear(); unset.clear(); }
        const auto path_scalars = scalars(relative);
        for (const auto& section : layer.config.sections) {
            if (!match_pattern(section.pattern, path_scalars, work)) { continue; }
            for (const auto& property : section.properties) {
                if (property.value == "unset") {
                    result.properties.erase(property.key);
                    unset.insert(property.key);
                } else {
                    result.properties[property.key] = property.value;
                    unset.erase(property.key);
                }
            }
        }
    }
    result.settings = typed_settings(result, unset);
    return result;
}

} // namespace astra::config
