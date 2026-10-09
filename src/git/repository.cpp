#include <astra/git/repository.hpp>
#include <astra/core/utf8.hpp>
#include <astra/process/process.hpp>

#include <algorithm>
#include <charconv>
#include <cwctype>
#include <future>
#include <limits>
#include <system_error>
#include <utility>

namespace astra::git {
namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

[[nodiscard]] std::string path_utf8(const fs::path& path) {
    const auto result = path.generic_u8string();
    return {reinterpret_cast<const char*>(result.data()), result.size()};
}

[[nodiscard]] fs::path utf8_path(std::string_view value) {
    utf8::validate(value);
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
}

[[nodiscard]] bool equal_component(const fs::path& left, const fs::path& right) {
#ifdef _WIN32
    auto a = left.native();
    auto b = right.native();
    std::ranges::transform(a, a.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::ranges::transform(b, b.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return a == b;
#else
    return left == right;
#endif
}

[[nodiscard]] bool within(const fs::path& root, const fs::path& path) {
    auto candidate = path.begin();
    for (const auto& component : root) {
        if (candidate == path.end() || !equal_component(component, *candidate++)) { return false; }
    }
    return true;
}

[[nodiscard]] std::string_view trim_newline(std::string_view text) {
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) { text.remove_suffix(1); }
    return text;
}

[[nodiscard]] std::size_t number(std::string_view text) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw GitError("malformed numeric field in Git output", -1);
    }
    return value;
}

[[nodiscard]] std::string_view nul_field(std::string_view input, std::size_t& offset) {
    const auto end = input.find('\0', offset);
    if (end == input.npos) { throw GitError("truncated NUL-delimited Git output", -1); }
    const auto value = input.substr(offset, end - offset);
    offset = end + 1;
    return value;
}

[[nodiscard]] std::vector<std::string_view> fields(std::string_view record, std::size_t count) {
    std::vector<std::string_view> result;
    for (std::size_t index = 0; index < count; ++index) {
        const auto space = record.find(' ');
        if (space == record.npos || space == 0) { throw GitError("malformed Git status record", -1); }
        result.push_back(record.substr(0, space));
        record.remove_prefix(space + 1);
    }
    if (record.empty()) { throw GitError("empty Git status pathname", -1); }
    result.push_back(record);
    return result;
}

[[nodiscard]] FileState file_state(char value) {
    switch (value) {
    case '.': case ' ': return FileState::unchanged;
    case 'M': return FileState::modified;
    case 'A': return FileState::added;
    case 'D': return FileState::deleted;
    case 'R': return FileState::renamed;
    case 'C': return FileState::copied;
    case 'U': return FileState::unmerged;
    case '?': return FileState::untracked;
    case '!': return FileState::ignored;
    case 'T': return FileState::type_changed;
    default: throw GitError("unknown Git status code", -1);
    }
}

[[nodiscard]] std::vector<std::string> child_environment(bool inherit_global_config, const GitOptions& options) {
    auto environment = process::current_environment();
    std::erase_if(environment, [](const std::string& entry) {
        auto key = entry.substr(0, entry.find('='));
        std::ranges::transform(key, key.begin(), [](char c) {
            return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
        });
        return key.starts_with("GIT_") || key == "LC_ALL";
    });
#ifdef _WIN32
    constexpr auto null_file = "NUL";
#else
    constexpr auto null_file = "/dev/null";
#endif
    if (!inherit_global_config) {
        environment.emplace_back("GIT_CONFIG_NOSYSTEM=1");
        environment.emplace_back(std::string("GIT_CONFIG_GLOBAL=") + null_file);
        environment.emplace_back(std::string("GIT_CONFIG_SYSTEM=") + null_file);
        environment.emplace_back("GIT_ATTR_NOSYSTEM=1");
    }
    environment.emplace_back("GIT_TERMINAL_PROMPT=0");
    environment.emplace_back("GIT_OPTIONAL_LOCKS=0");
    environment.emplace_back("GIT_PAGER=");
    environment.emplace_back("LC_ALL=C");
    const auto identity = [&environment](const std::optional<std::string>& value,
                                        std::string_view author, std::string_view committer) {
        if (!value) { return; }
        if (value->empty() || value->find_first_of("\r\n\0", 0, 3) != value->npos || value->size() > 1024) {
            throw std::invalid_argument("Git identity is empty, multiline or too large");
        }
        utf8::validate(*value);
        environment.push_back(std::string(author) + "=" + *value);
        environment.push_back(std::string(committer) + "=" + *value);
    };
    identity(options.author_name, "GIT_AUTHOR_NAME", "GIT_COMMITTER_NAME");
    identity(options.author_email, "GIT_AUTHOR_EMAIL", "GIT_COMMITTER_EMAIL");
    return environment;
}

[[nodiscard]] std::pair<std::size_t, std::size_t> hunk_side(std::string_view text, char prefix) {
    if (text.empty() || text.front() != prefix) { throw GitError("malformed Git diff hunk", -1); }
    text.remove_prefix(1);
    const auto comma = text.find(',');
    return {number(text.substr(0, comma)), comma == text.npos ? 1 : number(text.substr(comma + 1))};
}

void verify_token(const fs::path& path, const io::FileVersion& expected) {
    if (!expected.raw_content || fs::weakly_canonical(path) != fs::weakly_canonical(expected.path)) {
        throw io::FileConflict("Git action expected version belongs to a different file");
    }
    // Latin-1 decoding can represent every non-NUL byte and leaves the retained
    // raw bytes unchanged; the version comparison never depends on decoded text.
    const auto current = io::read_file(path, io::default_max_file_bytes,
        expected.raw_content->find('\0') == std::string::npos ?
            std::optional(io::TextEncoding::latin1) : std::nullopt);
    if (*current.version.raw_content != *expected.raw_content) {
        throw io::FileConflict("file changed before Git staging");
    }
}

} // namespace

GitError::GitError(std::string message, std::int64_t status)
    : std::runtime_error(std::move(message)), exit_status_(status) {}
std::int64_t GitError::exit_status() const noexcept { return exit_status_; }
GitCancelled::GitCancelled() : std::runtime_error("Git operation cancelled or deadline exceeded") {}

Repository::Repository(fs::path root, std::string executable, RepositoryOptions settings)
    : root_(fs::canonical(std::move(root))), executable_(std::move(executable)), settings_(settings) {
    if (!fs::is_directory(root_) || !fs::exists(root_ / ".git")) {
        throw std::invalid_argument("Git requires an explicit working-tree root containing .git");
    }
    if (executable_.empty() || executable_.find('\0') != executable_.npos) {
        throw std::invalid_argument("Git executable is invalid");
    }
    utf8::validate(executable_);
    const auto metadata = fs::weakly_canonical(root_ / ".git");
    if (!within(root_, metadata)) { throw std::invalid_argument("Git metadata symlink escapes repository root"); }
    const auto top = run({"rev-parse", "--show-toplevel"}, {});
    if (fs::canonical(utf8_path(trim_newline(top.out))) != root_) {
        throw std::invalid_argument("Git working-tree root differs from requested root");
    }
}

const fs::path& Repository::root() const noexcept { return root_; }

std::string Repository::relative_path(const fs::path& path) const {
    if (path.empty() || path.is_absolute() || path.has_root_name()) {
        throw std::invalid_argument("Git path must be relative to its explicit repository root");
    }
    for (const auto& component : path) {
        if (component == "..") { throw std::invalid_argument("Git path traverses outside repository"); }
        auto name = path_utf8(component);
        std::ranges::transform(name, name.begin(), [](char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
        });
        if (name == ".git") { throw std::invalid_argument("Git operation cannot target metadata"); }
    }
    const auto relative = path.lexically_normal();
    const auto resolved = fs::weakly_canonical(root_ / relative);
    if (!within(root_, resolved) || resolved == root_) {
        throw std::invalid_argument("Git path resolves outside repository or to its root");
    }
    const auto result = path_utf8(relative);
    if (result.find('\0') != result.npos) { throw std::invalid_argument("Git path contains NUL"); }
    utf8::validate(result);
    return result;
}

Repository::Output Repository::run(std::vector<std::string> arguments, const GitOptions& options,
                                   std::string input, bool allow_failure) const {
    if (options.timeout <= std::chrono::milliseconds::zero() || options.timeout > std::chrono::hours(24) ||
        options.max_output_bytes == 0 || options.max_entries == 0) {
        throw std::invalid_argument("invalid Git resource limits");
    }
    if (options.stop.stop_requested()) { throw GitCancelled(); }
    if (fs::canonical(root_) != root_) { throw std::invalid_argument("Git root has changed"); }
    process::ProcessOptions settings;
    settings.cwd = path_utf8(root_);
    settings.env = child_environment(settings_.inherit_global_config, options);
#ifdef _WIN32
    constexpr auto null_file = "NUL";
#else
    constexpr auto null_file = "/dev/null";
#endif
    settings.argv = {executable_, "--no-pager", "--literal-pathspecs",
        "-c", "core.fsmonitor=false", "-c", std::string("core.hooksPath=") + null_file,
        "-c", "maintenance.auto=false", "-c", "gc.auto=0", "-c", "color.ui=false"};
    if (!settings_.inherit_global_config) {
        settings.argv.insert(settings.argv.end(), {"-c", std::string("core.attributesFile=") + null_file,
            "-c", std::string("core.excludesFile=") + null_file});
    }
    settings.argv.insert(settings.argv.end(), std::make_move_iterator(arguments.begin()), std::make_move_iterator(arguments.end()));
    settings.max_stdout_bytes = options.max_output_bytes;
    settings.max_stderr_bytes = options.max_output_bytes;
    settings.max_queued_stdin_bytes = std::max(settings.max_queued_stdin_bytes, input.size());
    settings.shutdown_timeout = options.timeout;
    Output output;
    process::ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) { output.out.append(bytes); };
    callbacks.on_stderr = [&](std::string_view bytes) { output.error.append(bytes); };
    process::Process child(std::move(settings), std::move(callbacks));
    const auto deadline = Clock::now() + options.timeout;
    auto starting = child.start();
    const auto wait = [&](auto& future) {
        while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            if (options.stop.stop_requested() || Clock::now() >= deadline) {
                child.terminate();
                throw GitCancelled();
            }
        }
    };
    wait(starting);
    starting.get();
    if (!input.empty()) {
        auto write = child.write(std::move(input));
        wait(write);
        write.get();
    }
    child.close();
    auto complete = child.completion();
    wait(complete);
    const auto result = complete.get();
    output.status = result.exit_status;
    if (!result.error.empty()) { throw GitError(result.error, result.exit_status); }
    if (result.terminated || options.stop.stop_requested() || Clock::now() >= deadline) { throw GitCancelled(); }
    if (!allow_failure && result.exit_status != 0) {
        throw GitError(output.error.empty() ? "Git command failed" : std::string(trim_newline(output.error)), result.exit_status);
    }
    return output;
}

RepositoryStatus parse_status(std::string_view porcelain, std::size_t max_entries) {
    RepositoryStatus result;
    std::size_t offset = 0;
    while (offset < porcelain.size()) {
        const auto record = nul_field(porcelain, offset);
        if (record.empty()) { throw GitError("empty Git status record", -1); }
        if (record.starts_with("# ")) {
            if (record.starts_with("# branch.oid ")) {
                result.head = record.substr(13);
                result.unborn = result.head == "(initial)";
            } else if (record.starts_with("# branch.head ")) {
                result.branch = record.substr(14);
                result.detached = result.branch == "(detached)";
            } else if (record.starts_with("# branch.upstream ")) { result.upstream = record.substr(18); }
            else if (record.starts_with("# branch.ab ")) {
                const auto values = fields(record.substr(12), 1);
                if (!values[0].starts_with('+') || !values[1].starts_with('-')) { throw GitError("invalid Git ahead/behind record", -1); }
                result.ahead = number(values[0].substr(1));
                result.behind = number(values[1].substr(1));
            }
            continue;
        }
        if (result.entries.size() >= max_entries) { throw std::length_error("Git status entry limit exceeded"); }
        StatusEntry entry;
        if (record.starts_with("? ") || record.starts_with("! ")) {
            entry.path = utf8_path(record.substr(2));
            entry.unstaged = file_state(record.front());
        } else if (record.front() == '1' || record.front() == '2' || record.front() == 'u') {
            const auto count = record.front() == '1' ? 8U : record.front() == '2' ? 9U : 10U;
            const auto values = fields(record, count);
            if (values[1].size() != 2) { throw GitError("invalid Git XY status", -1); }
            entry.staged = file_state(values[1][0]);
            entry.unstaged = file_state(values[1][1]);
            entry.submodule = values[2];
            entry.path = utf8_path(values.back());
            entry.conflict = record.front() == 'u';
            if (record.front() == '2') { entry.original_path = utf8_path(nul_field(porcelain, offset)); }
        } else { throw GitError("unsupported Git porcelain status record", -1); }
        result.entries.push_back(std::move(entry));
    }
    return result;
}

RepositoryStatus Repository::status(const GitOptions& options) const {
    return parse_status(run({"status", "--porcelain=v2", "-z", "--branch", "--untracked-files=all", "--ignore-submodules=none"}, options).out,
                        options.max_entries);
}

DiffResult parse_diff(std::string patch, std::size_t max_hunks) {
    DiffResult result;
    result.patch = std::move(patch);
    std::string_view remaining(result.patch);
    while (!remaining.empty()) {
        const auto end = remaining.find('\n');
        const auto line = remaining.substr(0, end);
        remaining = end == remaining.npos ? std::string_view{} : remaining.substr(end + 1);
        if (line.starts_with("Binary files ") || line == "GIT binary patch") { result.binary = true; }
        if (!line.starts_with("@@ ")) { continue; }
        if (result.hunks.size() >= max_hunks) { throw std::length_error("Git diff hunk limit exceeded"); }
        const auto header_end = line.find(" @@", 3);
        if (header_end == line.npos) { throw GitError("invalid Git diff hunk header", -1); }
        const auto values = fields(line.substr(3, header_end - 3), 1);
        const auto [old_start, old_count] = hunk_side(values[0], '-');
        const auto [new_start, new_count] = hunk_side(values[1], '+');
        result.hunks.push_back({old_start, old_count, new_start, new_count});
        result.gutter.push_back({new_count == 0 ? new_start : (new_start == 0 ? 0 : new_start - 1),
            new_count, old_count, old_count == 0 ? GutterKind::added : new_count == 0 ? GutterKind::deleted : GutterKind::modified});
    }
    return result;
}

DiffResult Repository::diff(const fs::path& path, DiffBase base, bool staged, const GitOptions& options) const {
    const auto relative = relative_path(path);
    if (!staged && fs::is_regular_file(root_ / path) &&
        run({"ls-files", "--error-unmatch", "--", relative}, options, {}, true).status != 0) {
#ifdef _WIN32
        constexpr auto null_file = "NUL";
#else
        constexpr auto null_file = "/dev/null";
#endif
        const auto output = run({"diff", "--no-index", "--no-ext-diff", "--no-textconv", "--no-color",
            "--unified=0", "--", null_file, relative}, options, {}, true);
        if (output.status > 1) { throw GitError(output.error, output.status); }
        return parse_diff(output.out, options.max_entries);
    }
    std::vector<std::string> arguments{"diff", "--no-ext-diff", "--no-textconv", "--no-color", "--find-renames", "--unified=0"};
    if (staged) { arguments.push_back("--cached"); }
    if (base == DiffBase::head && !staged) { arguments.push_back("HEAD"); }
    arguments.insert(arguments.end(), {"--", relative});
    return parse_diff(run(std::move(arguments), options).out, options.max_entries);
}

std::vector<CommitInfo> Repository::log(std::size_t limit, const GitOptions& options) const {
    if (limit == 0 || limit > options.max_entries) { throw std::invalid_argument("Git log count exceeds its entry limit"); }
    if (status(options).unborn) { return {}; }
    const auto output = run({"log", "-z", "--no-show-signature", "--max-count=" + std::to_string(limit),
        "--format=%H%x00%P%x00%an%x00%ae%x00%at%x00%s"}, options).out;
    std::vector<CommitInfo> result;
    std::size_t offset = 0;
    while (offset < output.size()) {
        CommitInfo commit;
        commit.id = nul_field(output, offset);
        const auto parents = nul_field(output, offset);
        std::size_t parent_offset = 0;
        while (parent_offset < parents.size()) {
            const auto end = parents.find(' ', parent_offset);
            commit.parents.emplace_back(parents.substr(parent_offset, end == parents.npos ? parents.size() - parent_offset : end - parent_offset));
            parent_offset = end == parents.npos ? parents.size() : end + 1;
        }
        commit.author = nul_field(output, offset);
        commit.email = nul_field(output, offset);
        const auto timestamp = nul_field(output, offset);
        const auto parsed = std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), commit.timestamp);
        if (parsed.ec != std::errc{} || parsed.ptr != timestamp.data() + timestamp.size()) { throw GitError("invalid Git commit timestamp", -1); }
        commit.subject = nul_field(output, offset);
        result.push_back(std::move(commit));
    }
    return result;
}

std::vector<BranchInfo> Repository::branches(const GitOptions& options) const {
    const auto output = run({"for-each-ref", "--format=%(refname)%00%(objectname)%00%(HEAD)%00%(upstream)%00",
        "refs/heads/", "refs/remotes/"}, options).out;
    std::vector<BranchInfo> result;
    std::size_t offset = 0;
    while (offset < output.size()) {
        if (result.size() >= options.max_entries) { throw std::length_error("Git branch entry limit exceeded"); }
        BranchInfo branch;
        branch.ref = nul_field(output, offset);
        branch.commit = nul_field(output, offset);
        branch.current = nul_field(output, offset) == "*";
        branch.upstream = nul_field(output, offset);
        branch.remote = branch.ref.starts_with("refs/remotes/");
        if (offset < output.size() && output[offset] == '\r') { ++offset; }
        if (offset < output.size() && output[offset] == '\n') { ++offset; }
        result.push_back(std::move(branch));
    }
    return result;
}

std::string Repository::read_blob(const fs::path& path, DiffBase source, const GitOptions& options) const {
    return run({"cat-file", "blob", std::string(source == DiffBase::index ? ":" : "HEAD:") + relative_path(path)}, options).out;
}

void Repository::stage(std::span<const StagePath> paths, const GitOptions& options) const {
    if (paths.empty()) { return; }
    std::vector<std::string> arguments{"add", "--"};
    for (const auto& item : paths) {
        const auto relative = relative_path(item.path);
        if (item.expected) { verify_token(root_ / item.path, *item.expected); }
        arguments.push_back(relative);
    }
    (void)run(std::move(arguments), options);
}

void Repository::unstage(std::span<const fs::path> paths, const GitOptions& options) const {
    if (paths.empty()) { return; }
    std::vector<std::string> arguments;
    const auto current = status(options);
    if (current.unborn) { arguments = {"rm", "--cached", "-r", "-f", "--ignore-unmatch", "--"}; }
    else { arguments = {"restore", "--staged", "--source=HEAD", "--"}; }
    for (const auto& path : paths) { arguments.push_back(relative_path(path)); }
    (void)run(std::move(arguments), options);
}

void Repository::verify_branch(const std::string& name, const GitOptions& options) const {
    if (name.empty() || name.front() == '-' || name.find('\0') != name.npos || name.starts_with("refs/")) {
        throw std::invalid_argument("Git branch name is invalid");
    }
    utf8::validate(name);
    (void)run({"check-ref-format", "refs/heads/" + name}, options);
}

void Repository::create_branch(std::string name, bool checkout, const GitOptions& options) const {
    verify_branch(name, options);
    if (checkout) { (void)run({"switch", "--no-guess", "-c", std::move(name)}, options); }
    else { (void)run({"branch", "--", std::move(name)}, options); }
}

void Repository::switch_branch(std::string name, const GitOptions& options) const {
    verify_branch(name, options);
    (void)run({"switch", "--no-guess", "--", std::move(name)}, options);
}

void Repository::commit(std::string message, const GitOptions& options) const {
    utf8::validate(message);
    if (message.empty() || message.size() > 1024 * 1024 || message.find('\0') != message.npos ||
        message.find_first_not_of(" \t\r\n") == message.npos) {
        throw std::invalid_argument("Git commit message must be nonempty UTF-8 text of at most 1 MiB");
    }
    // -F - avoids platform command-line length limits and preserves literal
    // multiline messages without any shell interpretation or temporary file.
    (void)run({"commit", "--file=-"}, options, std::move(message));
}

io::FileSnapshot Repository::discard(const fs::path& path, std::optional<io::FileVersion> expected,
    std::optional<io::TextEncoding> encoding_override, const GitOptions& options) const {
    const auto relative = relative_path(path);
    const auto absolute = root_ / path;
    if (expected) { verify_token(absolute, *expected); }
    else if (fs::exists(absolute)) { throw io::FileConflict("Git discard expected an absent working-tree path"); }
    // cat-file performs the same attributes/EOL/smudge conversion as checkout,
    // but only returns bytes. It cannot truncate the user's working-tree file.
    const auto restored = run({"cat-file", "--filters", ":" + relative}, options).out;
    auto decoded = io::decode_bytes(restored, encoding_override);
    if (options.stop.stop_requested()) { throw GitCancelled(); }
    // Resolve again after an external filter ran: a renamed symlink/junction
    // must not redirect the final write outside the explicit repository root.
    (void)relative_path(path);
    auto version = io::save_file(absolute, decoded.text, decoded.encoding, std::move(expected));
    return {std::move(decoded.text), decoded.encoding, std::move(version)};
}

} // namespace astra::git
