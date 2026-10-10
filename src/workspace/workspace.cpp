#include <astra/workspace/workspace.hpp>

#include <astra/core/utf8.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace astra::workspace {
namespace {

namespace fs = std::filesystem;

std::string generic_utf8(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool within(const fs::path& path, const fs::path& root) {
    const auto relative = path.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() &&
           std::none_of(relative.begin(), relative.end(), [](const fs::path& part) {
               return part == "..";
           });
}

void require_relative(const fs::path& path) {
    if (path.empty() || path.is_absolute() || path.has_root_name() ||
        path.native().find(fs::path::value_type{}) != fs::path::string_type::npos ||
        std::any_of(path.begin(), path.end(), [](const fs::path& part) {
            return part == "..";
        })) {
        throw std::invalid_argument(
            "workspace path must be relative without parent traversal or NUL");
    }
}

WorkspaceIssue issue(const fs::path& path, const std::exception& error) {
    WorkspaceIssue result{path, error.what(), std::nullopt};
    if (const auto* recovery = dynamic_cast<const io::FileRecoveryRequired*>(&error)) {
        result.recovery_path = recovery->backup_path();
    }
    return result;
}

bool ignored(const fs::path& path, bool include_ignored, bool directory) {
    const auto name = generic_utf8(path.filename());
    if (name == ".git" || (name.starts_with(".astra-") && name.ends_with(".tmp"))) {
        return true;
    }
    if (include_ignored || !directory) {
        return false;
    }
    constexpr std::array names{"build",
                               "node_modules",
                               ".tools",
                               "target",
                               "dist",
                               ".cache",
                               ".venv",
                               "venv",
                               "__pycache__",
                               ".vs",
                               ".xmake"};
    return std::ranges::any_of(names, [&name](std::string_view ignored_name) {
        return name == ignored_name;
    });
}

bool ignored_path(const fs::path& relative, bool include_ignored, bool directory) {
    for (auto part = relative.begin(); part != relative.end(); ++part) {
        auto next = part;
        ++next;
        if (ignored(*part, include_ignored, directory || next != relative.end())) {
            return true;
        }
    }
    return false;
}

io::TextEncoding encoding_from(config::Charset charset) {
    switch (charset) {
    case config::Charset::latin1:
        return io::TextEncoding::latin1;
    case config::Charset::utf8:
        return io::TextEncoding::utf8;
    case config::Charset::utf8_bom:
        return io::TextEncoding::utf8_bom;
    case config::Charset::utf16_le:
        return io::TextEncoding::utf16_le;
    case config::Charset::utf16_be:
        return io::TextEncoding::utf16_be;
    }
    throw std::invalid_argument("unsupported workspace encoding");
}

std::optional<io::TextEncoding> read_override(io::TextEncoding encoding) {
    return encoding == io::TextEncoding::latin1 ? std::optional{encoding} : std::nullopt;
}

bool whitespace(char32_t scalar) {
    return (scalar >= 0x09 && scalar <= 0x0d) || scalar == 0x20 || scalar == 0x85 ||
           scalar == 0xa0 || scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200a) ||
           scalar == 0x2028 || scalar == 0x2029 || scalar == 0x202f || scalar == 0x205f ||
           scalar == 0x3000;
}

std::string_view trimmed_line(std::string_view line) {
    std::size_t last_content = 0;
    for (std::size_t offset = 0; offset < line.size();) {
        const auto scalar = utf8::decode(line, offset);
        offset += scalar.bytes;
        if (!whitespace(scalar.value)) {
            last_content = offset;
        }
    }
    return line.substr(0, last_content);
}

std::string_view newline_for(config::LineEnding ending) {
    switch (ending) {
    case config::LineEnding::lf:
        return "\n";
    case config::LineEnding::crlf:
        return "\r\n";
    case config::LineEnding::cr:
        return "\r";
    }
    throw std::invalid_argument("unsupported workspace line ending");
}

bool cr_only(std::string_view text) {
    return text.find('\r') != std::string_view::npos && text.find('\n') == std::string_view::npos;
}

std::string editor_text(std::string text) {
    // Equal-width replacement keeps all UTF-8 byte offsets stable. CRLF, LF,
    // and existing mixed CRLF/LF files retain their exact buffer representation.
    // Mixed lone-CR/LF files need a separate per-line EOL model, not a lossy
    // blanket normalization hidden in this adapter.
    if (cr_only(text)) {
        std::ranges::replace(text, '\r', '\n');
    }
    return text;
}

std::string format_for_save(std::string_view text, const config::EditorSettings& settings) {
    utf8::validate(text);
    const bool trim = settings.trim_trailing_whitespace.value_or(false);
    std::string result;
    result.reserve(text.size());
    std::string_view first_ending = "\n";
    bool seen_ending = false;
    for (std::size_t begin = 0; begin < text.size();) {
        const auto found = text.find_first_of("\r\n", begin);
        const auto end = found == std::string_view::npos ? text.size() : found;
        const auto line = text.substr(begin, end - begin);
        result += trim ? trimmed_line(line) : line;
        if (found == std::string_view::npos) {
            break;
        }
        auto length = std::size_t{1};
        if (text[found] == '\r' && found + 1 < text.size() && text[found + 1] == '\n') {
            length = 2;
        }
        const auto ending = text.substr(found, length);
        if (!seen_ending) {
            first_ending = ending;
            seen_ending = true;
        }
        result += settings.end_of_line ? newline_for(*settings.end_of_line) : ending;
        begin = found + length;
    }
    if (settings.insert_final_newline == true && !result.empty() && result.back() != '\n' &&
        result.back() != '\r') {
        result += settings.end_of_line ? newline_for(*settings.end_of_line) : first_ending;
    } else if (settings.insert_final_newline == false) {
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
    }
    return result;
}

struct PathLess {
    bool operator()(const fs::path& left, const fs::path& right) const {
#if defined(_WIN32)
        const auto& a = left.native();
        const auto& b = right.native();
        if (a.size() > 32768 || b.size() > 32768) {
            throw std::invalid_argument("Workspace path exceeds supported length");
        }
        const auto compared = CompareStringOrdinal(
            a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE);
        if (compared == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Compare workspace paths");
        }
        return compared == CSTR_LESS_THAN;
#else
        return left < right;
#endif
    }
};

void require_version(const io::FileVersion& expected, const io::FileVersion& current) {
    if (!expected.raw_content || expected.path != current.path ||
        *expected.raw_content != *current.raw_content) {
        throw io::FileConflict("workspace file changed since the operation was planned");
    }
}

} // namespace

Workspace::Workspace(const fs::path& root) : root_(fs::canonical(root)) {
    if (!fs::is_directory(root_)) {
        throw std::invalid_argument("workspace root must be a directory");
    }
}

const fs::path& Workspace::root() const noexcept {
    return root_;
}

fs::path Workspace::resolve_path(const fs::path& relative) const {
    require_relative(relative);
    const auto resolved = fs::weakly_canonical(root_ / relative);
    if (!within(resolved, root_)) {
        throw std::invalid_argument("resolved workspace path escapes its root");
    }
    return resolved;
}

TreeResult Workspace::tree(TreeOptions options, std::stop_token stop) const {
    if (options.max_entries > 1000000 || options.max_depth > 256) {
        throw std::invalid_argument("workspace tree limits exceed supported bounds");
    }
    TreeResult result;
    std::set<fs::path> visited{root_};
    std::function<void(const fs::path&, std::size_t)> walk;
    walk = [&](const fs::path& directory, std::size_t depth) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            return;
        }
        if (result.entries.size() >= options.max_entries) {
            result.truncated = true;
            return;
        }
        std::vector<TreeEntry> children;
        const auto absolute = directory.empty() ? fs::canonical(root_) : resolve_path(directory);
        if (!within(absolute, root_)) {
            throw std::invalid_argument("workspace root was redirected outside its original path");
        }
        std::error_code error;
        fs::directory_iterator iterator(absolute, error);
        if (error) {
            result.issues.push_back({directory, error.message(), std::nullopt});
            return;
        }
        const fs::directory_iterator end;
        while (iterator != end) {
            if (stop.stop_requested()) {
                result.cancelled = true;
                return;
            }
            const auto relative = directory / iterator->path().filename();
            if (!ignored(relative, true, false)) {
                try {
                    const auto resolved = resolve_path(relative);
                    const auto status = fs::status(resolved);
                    if (fs::is_directory(status) || fs::is_regular_file(status)) {
                        const bool is_directory = fs::is_directory(status);
                        if (ignored(relative, options.include_ignored, is_directory) ||
                            ignored_path(resolved.lexically_relative(root_),
                                         options.include_ignored,
                                         is_directory)) {
                            iterator.increment(error);
                            if (error) {
                                result.issues.push_back({directory, error.message(), std::nullopt});
                                break;
                            }
                            continue;
                        }
                        if (children.size() >= options.max_entries - result.entries.size()) {
                            result.truncated = true;
                            break;
                        }
                        children.push_back({relative,
                                            is_directory,
                                            fs::is_symlink(fs::symlink_status(root_ / relative)),
                                            is_directory ? 0 : fs::file_size(resolved)});
                    }
                } catch (const std::exception& failure) {
                    result.issues.push_back(issue(relative, failure));
                }
            }
            iterator.increment(error);
            if (error) {
                result.issues.push_back({directory, error.message(), std::nullopt});
                break;
            }
        }
        std::ranges::sort(children, [](const TreeEntry& left, const TreeEntry& right) {
            if (left.directory != right.directory) {
                return left.directory;
            }
            return left.relative_path.filename() < right.relative_path.filename();
        });
        for (const auto& child : children) {
            if (stop.stop_requested()) {
                result.cancelled = true;
                return;
            }
            if (result.entries.size() >= options.max_entries) {
                result.truncated = true;
                return;
            }
            result.entries.push_back(child);
            if (child.directory) {
                if (depth >= options.max_depth) {
                    result.truncated = true;
                    continue;
                }
                try {
                    const auto resolved = resolve_path(child.relative_path);
                    if (visited.insert(resolved).second) {
                        walk(child.relative_path, depth + 1);
                    }
                } catch (const std::exception& failure) {
                    result.issues.push_back(issue(child.relative_path, failure));
                }
            }
            if (result.cancelled) {
                return;
            }
        }
    };
    walk({}, 0);
    return result;
}

ConfigResult Workspace::editor_config(const fs::path& relative) const {
    (void)resolve_path(relative);
    std::vector<config::EditorConfigLayer> layers;
    ConfigResult result;
    fs::path directory;
    const auto load_layer = [&] {
        const auto config_relative = directory / ".editorconfig";
        try {
            const auto path = resolve_path(config_relative);
            if (!fs::exists(path)) {
                return;
            }
            const auto file = io::read_file(path, 1024U * 1024U);
            if (file.encoding != io::TextEncoding::utf8 &&
                file.encoding != io::TextEncoding::utf8_bom) {
                throw std::invalid_argument("EditorConfig must be UTF-8 encoded");
            }
            auto parsed = config::parse_editor_config(file.text);
            for (const auto& diagnostic : parsed.diagnostics) {
                result.issues.push_back(
                    {config_relative,
                     "line " + std::to_string(diagnostic.line) + ": " + diagnostic.message,
                     std::nullopt});
            }
            layers.push_back({generic_utf8(directory), std::move(parsed)});
        } catch (const std::exception& failure) {
            result.issues.push_back(issue(config_relative, failure));
        }
    };
    load_layer();
    for (const auto& component : relative.lexically_normal().parent_path()) {
        if (component == ".") {
            continue;
        }
        directory /= component;
        load_layer();
    }
    result.config =
        config::resolve_editor_config(layers, generic_utf8(relative.lexically_normal()));
    return result;
}

LoadedFile Workspace::load(const fs::path& relative, std::size_t max_bytes) const {
    const auto path = resolve_path(relative);
    auto configured = editor_config(relative);
    const auto override = configured.config.settings.charset == config::Charset::latin1
                              ? std::optional{io::TextEncoding::latin1}
                              : std::nullopt;
    auto snapshot = io::read_file(path, max_bytes, override);
    snapshot.text = editor_text(std::move(snapshot.text));
    return {relative.lexically_normal(),
            std::move(snapshot),
            std::move(configured.config),
            std::move(configured.issues)};
}

Workspace::PreparedSave
Workspace::prepare_save(const fs::path& relative,
                        std::string_view text,
                        io::TextEncoding original_encoding,
                        const std::optional<io::FileVersion>& expected) const {
    (void)resolve_path(relative);
    auto configured = editor_config(relative);
    const auto encoding = configured.config.settings.charset
                              ? encoding_from(*configured.config.settings.charset)
                              : original_encoding;
    switch (encoding) {
    case io::TextEncoding::utf8:
    case io::TextEncoding::utf8_bom:
    case io::TextEncoding::utf16_le:
    case io::TextEncoding::utf16_be:
    case io::TextEncoding::latin1:
        break;
    default:
        throw std::invalid_argument("unsupported workspace encoding");
    }
    auto settings = configured.config.settings;
    if (!settings.end_of_line && expected && expected->raw_content) {
        const auto original =
            io::decode_bytes(*expected->raw_content, read_override(original_encoding));
        if (cr_only(original.text)) {
            settings.end_of_line = config::LineEnding::cr;
        }
    }
    auto formatted = format_for_save(text, settings);
    for (std::size_t offset = 0; offset < formatted.size();) {
        const auto scalar = utf8::decode(formatted, offset);
        if (scalar.value == 0 || (encoding == io::TextEncoding::latin1 && scalar.value > 0xff)) {
            throw std::invalid_argument(
                "text cannot be represented in the requested file encoding");
        }
        offset += scalar.bytes;
    }
    PreparedSave prepared;
    if (cr_only(formatted)) {
        prepared.disk_text = std::move(formatted);
        formatted = editor_text(*prepared.disk_text);
    }
    prepared.result = {relative.lexically_normal(),
                       {},
                       std::move(formatted),
                       encoding,
                       std::move(configured.config),
                       std::move(configured.issues)};
    return prepared;
}

SavedFile Workspace::save(const fs::path& relative,
                          std::string_view text,
                          std::optional<io::FileVersion> expected,
                          io::TextEncoding original_encoding) const {
    auto prepared = prepare_save(relative, text, original_encoding, expected);
    prepared.result.version = io::save_file(resolve_path(relative),
                                            prepared.text_for_disk(),
                                            prepared.result.encoding,
                                            std::move(expected));
    return std::move(prepared.result);
}

SearchResult Workspace::search(const SearchOptions& options, std::stop_token stop) const {
    const SearchPattern pattern(options);
    SearchResult result;
    result.query = options.query;
    result.options = options;
    result.options.excluded_paths.clear();
    std::set<fs::path, PathLess> excluded;
    for (const auto& path : options.excluded_paths) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        excluded.insert(resolve_path(path));
    }
    const auto listed = tree(options.tree, stop);
    result.issues = listed.issues;
    result.truncated = listed.truncated;
    result.cancelled = listed.cancelled;
    if (result.cancelled) {
        return result;
    }
    std::size_t total = 0;
    std::size_t result_bytes = 0;
    std::size_t files = 0;
    std::set<fs::path, PathLess> searched;
    for (const auto& entry : listed.entries) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            break;
        }
        if (entry.directory) {
            continue;
        }
        try {
            const auto resolved = resolve_path(entry.relative_path);
            if (excluded.contains(resolved) || !searched.insert(resolved).second) {
                continue;
            }
            if (files >= options.max_files) {
                result.truncated = true;
                break;
            }
            ++files;
            auto file = load(entry.relative_path, options.max_file_bytes);
            result.issues.insert(result.issues.end(), file.issues.begin(), file.issues.end());
            if (file.snapshot.text.size() > options.max_total_bytes - total) {
                result.truncated = true;
                break;
            }
            total += file.snapshot.text.size();
            const Rope rope(file.snapshot.text);
            auto matches = pattern.find_all(
                file.snapshot.text, stop, options.max_matches - result.matches.size());
            result.cancelled = matches.cancelled;
            result.truncated = result.truncated || matches.truncated;
            bool metadata_full = false;
            for (auto& match : matches.matches) {
                if (stop.stop_requested()) {
                    result.cancelled = true;
                    break;
                }
                const auto bytes =
                    sizeof(SearchMatch) +
                    entry.relative_path.native().size() * sizeof(fs::path::value_type) +
                    match.captures.size() * sizeof(std::optional<TextRange>);
                if (bytes > options.max_result_bytes - result_bytes) {
                    result.truncated = true;
                    metadata_full = true;
                    break;
                }
                result_bytes += bytes;
                result.matches.push_back({entry.relative_path,
                                          match.begin,
                                          match.end,
                                          rope.byte_to_position(match.begin.value),
                                          rope.byte_to_position(match.end.value),
                                          file.snapshot.version,
                                          file.snapshot.encoding,
                                          std::move(match.captures)});
            }
            if (result.cancelled || matches.truncated || metadata_full) {
                break;
            }
            // Reaching a limit exactly is not itself truncation; later files
            // are still scanned with the remaining match/metadata capacity.
        } catch (const std::exception& failure) {
            result.issues.push_back(issue(entry.relative_path, failure));
        }
    }
    return result;
}

ReplacePlan Workspace::plan_replace(const SearchResult& search_result,
                                    std::string_view replacement,
                                    std::stop_token stop) const {
    if (search_result.query.empty() || search_result.options.query != search_result.query ||
        search_result.cancelled || search_result.truncated) {
        throw std::invalid_argument(
            "cannot plan replacement from an incomplete search or invalid replacement");
    }
    const SearchPattern pattern(search_result.options);
    pattern.validate_replacement(replacement);
    std::map<fs::path, std::vector<const SearchMatch*>> grouped;
    for (const auto& match : search_result.matches) {
        grouped[match.relative_path].push_back(&match);
    }
    ReplacePlan plan;
    std::size_t total_output = 0;
    for (auto& [relative, matches] : grouped) {
        if (stop.stop_requested()) {
            throw std::runtime_error("workspace replacement planning was cancelled");
        }
        const auto path = resolve_path(relative);
        auto original = io::read_file(
            path, search_result.options.max_file_bytes, read_override(matches.front()->encoding));
        original.text = editor_text(std::move(original.text));
        std::ranges::sort(matches, {}, [](const SearchMatch* match) {
            return match->begin;
        });
        std::string text;
        std::size_t source = 0;
        std::optional<std::size_t> previous_begin;
        const auto append = [&](std::string_view value) {
            if (value.size() > SearchPattern::max_output_bytes - total_output - text.size()) {
                throw std::length_error("Workspace replacement plan exceeds 128 MiB output limit");
            }
            text.append(value);
        };
        for (const auto* match : matches) {
            if (stop.stop_requested()) {
                throw std::runtime_error("workspace replacement planning was cancelled");
            }
            require_version(match->version, original.version);
            if (match->begin.value < source ||
                (previous_begin && *previous_begin == match->begin.value) ||
                match->begin > match->end || match->end.value > original.text.size() ||
                !utf8::is_boundary(original.text, match->begin.value) ||
                !utf8::is_boundary(original.text, match->end.value) ||
                match->encoding != original.encoding) {
                throw std::invalid_argument(
                    "workspace search contains invalid or overlapping matches");
            }
            append(std::string_view(original.text).substr(source, match->begin.value - source));
            append(pattern.expand_replacement(original.text,
                                              {match->begin, match->end, match->captures},
                                              replacement,
                                              SearchPattern::max_output_bytes - total_output -
                                                  text.size()));
            source = match->end.value;
            previous_begin = match->begin.value;
        }
        append(std::string_view(original.text).substr(source));
        total_output += text.size();
        plan.files.push_back(
            {relative, std::move(text), original.encoding, original.version, matches.size()});
    }
    return plan;
}

ReplaceResult Workspace::apply_replace(const ReplacePlan& plan, std::stop_token stop) const {
    ReplaceResult result;
    result.applied.reserve(plan.files.size());
    result.pending.reserve(plan.files.size());
    result.issues.reserve(1);
    for (const auto& file : plan.files) {
        result.pending.push_back(file.relative_path);
    }
    std::vector<PreparedSave> prepared;
    prepared.reserve(plan.files.size());
    std::set<fs::path> paths;
    for (const auto& file : plan.files) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            return result;
        }
        try {
            const auto path = resolve_path(file.relative_path);
            if (!paths.insert(path).second) {
                throw std::invalid_argument("replacement plan contains a duplicate file");
            }
            const auto current =
                io::read_file(path, io::default_max_file_bytes, read_override(file.encoding));
            require_version(file.expected, current.version);
            prepared.push_back(
                prepare_save(file.relative_path, file.text, file.encoding, file.expected));
        } catch (const std::exception& failure) {
            result.issues.push_back(issue(file.relative_path, failure));
            return result;
        }
    }
    for (std::size_t index = 0; index < plan.files.size(); ++index) {
        if (stop.stop_requested()) {
            result.cancelled = true;
            result.pending.erase(result.pending.begin(),
                                 result.pending.begin() +
                                     static_cast<std::ptrdiff_t>(result.applied.size()));
            return result;
        }
        const auto& file = plan.files[index];
        try {
            auto& saving = prepared[index];
            saving.result.version = io::save_file(resolve_path(file.relative_path),
                                                  saving.text_for_disk(),
                                                  saving.result.encoding,
                                                  file.expected);
            result.applied.push_back(std::move(saving.result));
        } catch (const std::exception& failure) {
            result.issues.push_back(issue(file.relative_path, failure));
            result.pending.erase(result.pending.begin(),
                                 result.pending.begin() +
                                     static_cast<std::ptrdiff_t>(result.applied.size()));
            return result;
        }
    }
    result.pending.clear();
    return result;
}

} // namespace astra::workspace
