#include <astra/workspace/workspace.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <winioctl.h>
#endif

namespace {

namespace fs = std::filesystem;
using astra::workspace::Workspace;
using astra::workspace::SearchOptions;
using astra::io::TextEncoding;

bool strictly_within(const fs::path& child, const fs::path& parent) {
    const auto relative = child.lexically_relative(parent);
    return !relative.empty() && relative != "." && !relative.is_absolute() &&
        std::none_of(relative.begin(), relative.end(), [](const fs::path& part) { return part == ".."; });
}

class WorkspaceFixture {
public:
    WorkspaceFixture() {
        const auto cwd = fs::canonical(fs::current_path());
        build_ = fs::weakly_canonical(cwd / "build");
        if (!strictly_within(build_, cwd)) { throw std::runtime_error("test build root escaped workspace"); }
        (void)fs::create_directories(build_);
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = build_ / ("workspace-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(candidate)) {
                base_ = fs::canonical(candidate);
                project_ = base_ / "project";
                (void)fs::create_directory(project_);
                return;
            }
        }
        throw std::runtime_error("could not allocate test workspace");
    }
    WorkspaceFixture(const WorkspaceFixture&) = delete;
    WorkspaceFixture& operator=(const WorkspaceFixture&) = delete;
    ~WorkspaceFixture() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(base_, error);
        if (error || resolved != base_ || !strictly_within(resolved, build_)) {
            std::fputs("refusing unsafe workspace fixture cleanup\n", stderr);
            return;
        }
        (void)fs::remove_all(resolved, error);
        if (error) { std::fputs("workspace fixture cleanup failed\n", stderr); }
    }
    [[nodiscard]] const fs::path& root() const noexcept { return project_; }
    [[nodiscard]] const fs::path& base() const noexcept { return base_; }
    void write(const fs::path& relative, std::string_view text) const { write_at(project_ / relative, text); }
    void write_outside(std::string_view text) const { write_at(base_ / "outside" / "secret.txt", text); }
    void parent_config(std::string_view text) const { write_at(base_ / ".editorconfig", text); }
    [[nodiscard]] std::string read(const fs::path& relative) const {
        std::ifstream stream(project_ / relative, std::ios::binary);
        if (!stream) { throw std::runtime_error("cannot read fixture file"); }
        return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}};
    }
private:
    static void write_at(const fs::path& path, std::string_view text) {
        (void)fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.close();
    }
    fs::path build_;
    fs::path base_;
    fs::path project_;
};

} // namespace

TEST_CASE("workspace requires an existing explicit root and confines relative paths") {
    WorkspaceFixture fixture;
    fixture.write("file.txt", "content");
    Workspace workspace(fixture.root());
    CHECK(workspace.root() == fs::canonical(fixture.root()));
    CHECK(workspace.resolve_path("file.txt") == fs::canonical(fixture.root() / "file.txt"));
    CHECK(workspace.resolve_path("new.txt") == fixture.root() / "new.txt");
    CHECK_THROWS_AS((void)workspace.resolve_path("../outside.txt"), std::invalid_argument);
    CHECK_THROWS_AS((void)workspace.resolve_path(fixture.base() / "outside.txt"), std::invalid_argument);
    CHECK_THROWS_AS((void)workspace.resolve_path({}), std::invalid_argument);
    CHECK_THROWS_AS((void)workspace.resolve_path(fs::path(std::string("bad\0path", 8))), std::invalid_argument);
    CHECK_THROWS_AS((void)Workspace(fixture.root() / "file.txt"), std::invalid_argument);
    CHECK_THROWS_AS((void)Workspace(fixture.root() / "missing"), fs::filesystem_error);
}

TEST_CASE("workspace tree sorts siblings with directories first and excludes build dependencies") {
    WorkspaceFixture fixture;
    fixture.write("zDir/file", "z");
    fixture.write("aDir/file", "a");
    fixture.write("z.txt", "z");
    fixture.write("a.txt", "a");
    fixture.write(".git/config", "secret");
    fixture.write("build/generated", "generated");
    fixture.write("node_modules/package/index.js", "dependency");
    fixture.write(".tools/tool", "tool");
    Workspace workspace(fixture.root());
    const auto tree = workspace.tree();
    REQUIRE(tree.entries.size() == 6);
    CHECK(tree.entries[0].relative_path == "aDir");
    CHECK(tree.entries[0].directory);
    CHECK(tree.entries[1].relative_path == fs::path("aDir") / "file");
    CHECK(tree.entries[2].relative_path == "zDir");
    CHECK(tree.entries[4].relative_path == "a.txt");
    CHECK(tree.entries[5].relative_path == "z.txt");
    CHECK(tree.issues.empty());
    CHECK_FALSE(tree.truncated);
    const auto all = workspace.tree({100, 64, true});
    CHECK(all.entries.size() > tree.entries.size());
    CHECK(std::ranges::none_of(all.entries, [](const auto& entry) {
        return entry.relative_path.begin()->string() == ".git";
    }));
}

TEST_CASE("workspace tree enumeration is bounded and cancellable") {
    WorkspaceFixture fixture;
    fixture.write("src/deep/file.txt", "text");
    fixture.write("a.txt", "text");
    fixture.write("b.txt", "text");
    Workspace workspace(fixture.root());
    const auto limited = workspace.tree({2, 64, false});
    CHECK(limited.entries.size() == 2);
    CHECK(limited.truncated);
    const auto shallow = workspace.tree({100, 0, false});
    CHECK(shallow.truncated);
    CHECK(std::ranges::none_of(shallow.entries, [](const auto& entry) {
        return entry.relative_path == fs::path("src") / "deep";
    }));
    std::stop_source source;
    (void)source.request_stop();
    const auto cancelled = workspace.tree({}, source.get_token());
    CHECK(cancelled.cancelled);
    CHECK(cancelled.entries.empty());
    CHECK_THROWS_AS((void)workspace.tree({100, 257, false}), std::invalid_argument);
}

TEST_CASE("workspace EditorConfig discovery stays inside root and layers child settings") {
    WorkspaceFixture fixture;
    fixture.parent_config("[*]\nindent_size=99\ncharset=latin1\n");
    fixture.write(".editorconfig", "[*]\nindent_size=4\nend_of_line=lf\n");
    fixture.write("src/.editorconfig", "[*.cpp]\nindent_size=2\n");
    fixture.write("src/main.cpp", "code\r\n");
    Workspace workspace(fixture.root());
    const auto loaded = workspace.load("src/main.cpp");
    CHECK(loaded.config.settings.indent_size == 2);
    CHECK(loaded.config.settings.end_of_line == astra::config::LineEnding::lf);
    CHECK_FALSE(loaded.config.settings.charset.has_value());
    CHECK(loaded.snapshot.text == "code\r\n");
    CHECK(loaded.issues.empty());
    fixture.write("src/.editorconfig", "root=true\n[*.cpp]\nindent_size=3\n");
    const auto rooted = workspace.editor_config("src/main.cpp");
    CHECK(rooted.config.settings.indent_size == 3);
    CHECK_FALSE(rooted.config.settings.end_of_line.has_value());
}

TEST_CASE("workspace save applies EditorConfig policies to a copy and returns actual saved text") {
    WorkspaceFixture fixture;
    fixture.write(".editorconfig", "[*]\nend_of_line=lf\ntrim_trailing_whitespace=true\ninsert_final_newline=true\ncharset=utf-8-bom\n");
    fixture.write("file.txt", "old");
    Workspace workspace(fixture.root());
    const auto loaded = workspace.load("file.txt");
    const std::string input = "line \t\r\nnext\xc2\xa0";
    const auto saved = workspace.save("file.txt", input, loaded.snapshot.version, loaded.snapshot.encoding);
    CHECK(input == "line \t\r\nnext\xc2\xa0");
    CHECK(saved.text == "line\nnext\n");
    CHECK(saved.encoding == TextEncoding::utf8_bom);
    CHECK(fixture.read("file.txt") == "\xef\xbb\xbfline\nnext\n");
    CHECK(workspace.load("file.txt").snapshot.text == saved.text);
    const auto empty = workspace.save("empty.txt", "");
    CHECK(empty.text.empty());
    CHECK(fixture.read("empty.txt") == "\xef\xbb\xbf");
}

TEST_CASE("workspace final newline false removes endings and unspecified policies preserve them") {
    WorkspaceFixture fixture;
    Workspace workspace(fixture.root());
    const auto saved = workspace.save("file.txt", "a  \r\nb\n");
    CHECK(saved.text == "a  \r\nb\n");
    fixture.write(".editorconfig", "[*]\ninsert_final_newline=false\n");
    const auto changed = workspace.save("file.txt", "a\r\n\r\n", saved.version);
    CHECK(changed.text == "a");
    CHECK(fixture.read("file.txt") == "a");
}

TEST_CASE("workspace Latin-1 configuration loads legacy text and rejects unencodable edits") {
    WorkspaceFixture fixture;
    fixture.write(".editorconfig", "[*.txt]\ncharset=latin1\n");
    fixture.write("file.txt", "caf\xe9");
    Workspace workspace(fixture.root());
    const auto loaded = workspace.load("file.txt");
    CHECK(loaded.snapshot.text == "caf\xc3\xa9");
    CHECK(loaded.snapshot.encoding == TextEncoding::latin1);
    const auto saved = workspace.save("file.txt", "caf\xc3\xa9\n", loaded.snapshot.version, loaded.snapshot.encoding);
    CHECK(saved.encoding == TextEncoding::latin1);
    CHECK(fixture.read("file.txt") == "caf\xe9\n");
    CHECK_THROWS_AS((void)workspace.save("file.txt", "\xe4\xb8\xad", saved.version, saved.encoding), std::invalid_argument);
    CHECK(fixture.read("file.txt") == "caf\xe9\n");
}

TEST_CASE("workspace reports invalid config and refuses stale saves") {
    WorkspaceFixture fixture;
    fixture.write(".editorconfig", "[*]\nbad line\nindent_size=2\n");
    fixture.write("file.txt", "initial");
    Workspace workspace(fixture.root());
    const auto loaded = workspace.load("file.txt");
    REQUIRE(loaded.issues.size() == 1);
    CHECK(loaded.config.settings.indent_size == 2);
    fixture.write("file.txt", "outside");
    CHECK_THROWS_AS((void)workspace.save("file.txt", "my edit", loaded.snapshot.version), astra::io::FileConflict);
    CHECK(fixture.read("file.txt") == "outside");
}

TEST_CASE("workspace literal search returns exact UTF8 ranges and UTF16 positions") {
    WorkspaceFixture fixture;
    fixture.write("text.txt", "\xf0\x9f\x98\x80 cat\r\n\xe4\xb8\xad" "cat\ncat");
    fixture.write(".git/hidden", "cat");
    fixture.write("build/generated", "cat");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    const auto result = workspace.search(options);
    REQUIRE(result.matches.size() == 3);
    CHECK(result.matches[0].begin == astra::ByteOffset{5});
    CHECK(result.matches[0].end == astra::ByteOffset{8});
    CHECK(result.matches[0].start_position == astra::TextPosition{0, 3});
    CHECK(result.matches[1].start_position == astra::TextPosition{1, 1});
    CHECK(result.matches[2].start_position == astra::TextPosition{2, 0});
    CHECK(*result.matches[0].version.raw_content == fixture.read("text.txt"));
    CHECK(result.issues.empty());
    CHECK_FALSE(result.truncated);
    options.query = "CAT";
    CHECK(workspace.search(options).matches.empty());
    fixture.write("literal.txt", "a.*b axxxb");
    options.query = "a.*b";
    CHECK(workspace.search(options).matches.size() == 1);
}

TEST_CASE("workspace whole word search uses Unicode letters marks and numbers") {
    WorkspaceFixture fixture;
    fixture.write("text.txt", "cat scatter cat_ cat2 \xc3\xa9" "cat cat\xc3\xa9 \xf0\x9f\x98\x80" "cat\xf0\x9f\x98\x80 cat\n");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    options.whole_word = true;
    const auto result = workspace.search(options);
    CHECK(result.matches.size() == 3);
}

TEST_CASE("workspace search cancellation and limits are explicit and block replace-all plans") {
    WorkspaceFixture fixture;
    fixture.write("a.txt", "cat cat cat");
    fixture.write("b.txt", "cat");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    options.max_matches = 2;
    const auto limited = workspace.search(options);
    CHECK(limited.matches.size() == 2);
    CHECK(limited.truncated);
    CHECK_THROWS_AS((void)workspace.plan_replace(limited, "dog"), std::invalid_argument);
    std::stop_source source;
    (void)source.request_stop();
    const auto cancelled = workspace.search(options, source.get_token());
    CHECK(cancelled.cancelled);
    CHECK(cancelled.matches.empty());
    options.max_matches = 100;
    options.max_total_bytes = 2;
    CHECK(workspace.search(options).truncated);
    options.query.clear();
    CHECK_THROWS_AS((void)workspace.search(options), std::invalid_argument);
}

TEST_CASE("workspace search reports binary and oversized files without reading beyond limits") {
    WorkspaceFixture fixture;
    fixture.write("a.txt", "cat");
    fixture.write("binary.bin", std::string("x\0cat", 5));
    fixture.write("large.txt", std::string(100, 'x'));
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    options.max_file_bytes = 10;
    const auto result = workspace.search(options);
    CHECK(result.matches.size() == 1);
    CHECK(result.issues.size() == 2);
}

TEST_CASE("workspace replacement plans preserve file versions and apply all nonoverlapping matches") {
    WorkspaceFixture fixture;
    fixture.write("a.txt", "cat cat\n");
    fixture.write("b.txt", "one cat");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    const auto search = workspace.search(options);
    const auto plan = workspace.plan_replace(search, "kitten");
    REQUIRE(plan.files.size() == 2);
    CHECK(plan.files[0].replacements == 2);
    CHECK(fixture.read("a.txt") == "cat cat\n");
    const auto result = workspace.apply_replace(plan);
    CHECK(result.applied.size() == 2);
    CHECK(result.pending.empty());
    CHECK(result.issues.empty());
    CHECK(fixture.read("a.txt") == "kitten kitten\n");
    CHECK(fixture.read("b.txt") == "one kitten");
    CHECK(*result.applied[0].version.raw_content == fixture.read("a.txt"));
}

TEST_CASE("workspace preflight conflict causes zero writes across a replacement plan") {
    WorkspaceFixture fixture;
    fixture.write("a.txt", "cat");
    fixture.write("b.txt", "cat");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    const auto plan = workspace.plan_replace(workspace.search(options), "dog");
    fixture.write("b.txt", "external");
    const auto result = workspace.apply_replace(plan);
    CHECK(result.applied.empty());
    CHECK(result.pending.size() == 2);
    REQUIRE(result.issues.size() == 1);
    CHECK(result.issues[0].relative_path == "b.txt");
    CHECK(fixture.read("a.txt") == "cat");
    CHECK(fixture.read("b.txt") == "external");
}

TEST_CASE("workspace rejects stale or forged replacement matches and respects cancellation") {
    WorkspaceFixture fixture;
    fixture.write("a.txt", "cat cat");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    auto search = workspace.search(options);
    const auto plan = workspace.plan_replace(search, "dog");
    std::stop_source source;
    (void)source.request_stop();
    const auto cancelled = workspace.apply_replace(plan, source.get_token());
    CHECK(cancelled.cancelled);
    CHECK(cancelled.applied.empty());
    CHECK(cancelled.pending.size() == 1);
    search.matches.push_back(search.matches.front());
    CHECK_THROWS_AS((void)workspace.plan_replace(search, "dog"), std::invalid_argument);
    fixture.write("a.txt", "changed");
    search.matches.pop_back();
    CHECK_THROWS_AS((void)workspace.plan_replace(search, "dog"), astra::io::FileConflict);
}

#ifdef _WIN32
namespace {

class Junction {
public:
    Junction(fs::path link, const fs::path& target) : link_(std::move(link)) {
        const auto print_name = fs::canonical(target).native();
        const auto substitute = L"\\??\\" + print_name;
        struct MountPoint {
            DWORD tag{};
            WORD data_length{};
            WORD reserved{};
            WORD substitute_offset{};
            WORD substitute_length{};
            WORD print_offset{};
            WORD print_length{};
            std::array<wchar_t, 2048> path{};
        } buffer;
        if (substitute.size() + print_name.size() + 2 > buffer.path.size()) {
            throw std::length_error("junction test path is too long");
        }
        buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
        buffer.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
        buffer.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
        buffer.print_length = static_cast<WORD>(print_name.size() * sizeof(wchar_t));
        buffer.data_length = static_cast<WORD>(8 + buffer.print_offset + buffer.print_length + sizeof(wchar_t));
        std::ranges::copy(substitute, buffer.path.begin());
        std::ranges::copy(print_name, buffer.path.begin() + static_cast<std::ptrdiff_t>(substitute.size() + 1));
        if (!CreateDirectoryW(link_.c_str(), nullptr)) { throw std::runtime_error("cannot create test junction directory"); }
        const auto handle = CreateFileW(link_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            (void)RemoveDirectoryW(link_.c_str());
            throw std::system_error(static_cast<int>(error), std::system_category(), "open test junction");
        }
        DWORD returned = 0;
        const auto succeeded = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer,
            static_cast<DWORD>(buffer.data_length) + 8, nullptr, 0, &returned, nullptr);
        const auto error = GetLastError();
        if (!CloseHandle(handle)) { std::fputs("test junction handle close failed\n", stderr); }
        if (!succeeded) {
            (void)RemoveDirectoryW(link_.c_str());
            throw std::system_error(static_cast<int>(error), std::system_category(), "create test junction");
        }
    }
    Junction(const Junction&) = delete;
    Junction& operator=(const Junction&) = delete;
    ~Junction() {
        if (!RemoveDirectoryW(link_.c_str())) { std::fputs("test junction cleanup failed\n", stderr); }
    }
private:
    fs::path link_;
};

class ReadOnlyGuard {
public:
    explicit ReadOnlyGuard(fs::path path) : path_(std::move(path)), old_(GetFileAttributesW(path_.c_str())) {
        if (old_ == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(path_.c_str(), old_ | FILE_ATTRIBUTE_READONLY)) {
            throw std::runtime_error("cannot set read-only test file");
        }
    }
    ReadOnlyGuard(const ReadOnlyGuard&) = delete;
    ReadOnlyGuard& operator=(const ReadOnlyGuard&) = delete;
    ~ReadOnlyGuard() {
        if (!SetFileAttributesW(path_.c_str(), old_)) { std::fputs("read-only test cleanup failed\n", stderr); }
    }
private:
    fs::path path_;
    DWORD old_{};
};

} // namespace

TEST_CASE("Windows workspace prevents junction escapes for load save enumeration and config") {
    WorkspaceFixture fixture;
    fixture.write("inside.txt", "inside");
    fixture.write_outside("secret");
    const Junction escape(fixture.root() / "escape", fixture.base() / "outside");
    Workspace workspace(fixture.root());
    CHECK_THROWS_AS((void)workspace.resolve_path("escape/secret.txt"), std::invalid_argument);
    CHECK_THROWS_AS((void)workspace.load("escape/secret.txt"), std::invalid_argument);
    CHECK_THROWS_AS((void)workspace.save("escape/new.txt", "bad"), std::invalid_argument);
    CHECK_THROWS_AS((void)workspace.editor_config("escape/secret.txt"), std::invalid_argument);
    const auto tree = workspace.tree();
    CHECK(tree.entries.size() == 1);
    CHECK(tree.issues.size() == 1);
    CHECK_FALSE(fs::exists(fixture.base() / "outside" / "new.txt"));
}

TEST_CASE("Windows workspace internal junction cycles are bounded") {
    WorkspaceFixture fixture;
    fixture.write("src/file.txt", "inside");
    const Junction loop(fixture.root() / "src" / "loop", fixture.root());
    Workspace workspace(fixture.root());
    const auto tree = workspace.tree();
    CHECK(tree.entries.size() == 3);
    CHECK_FALSE(tree.truncated);
    CHECK(tree.issues.empty());
}

TEST_CASE("Windows workspace excludes git metadata reached through an internal junction alias") {
    WorkspaceFixture fixture;
    fixture.write("inside.txt", "inside");
    fixture.write(".git/config", "hidden");
    const Junction alias(fixture.root() / "git-alias", fixture.root() / ".git");
    Workspace workspace(fixture.root());
    const auto tree = workspace.tree({100, 64, true});
    REQUIRE(tree.entries.size() == 1);
    CHECK(tree.entries[0].relative_path == "inside.txt");
    CHECK(tree.issues.empty());
}

TEST_CASE("Windows workspace replacement reports partial completion on later save failure") {
    WorkspaceFixture fixture;
    fixture.write("a.txt", "cat");
    fixture.write("b.txt", "cat");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    const auto plan = workspace.plan_replace(workspace.search(options), "dog");
    const ReadOnlyGuard read_only(fixture.root() / "b.txt");
    const auto result = workspace.apply_replace(plan);
    REQUIRE(result.applied.size() == 1);
    CHECK(result.applied[0].relative_path == "a.txt");
    REQUIRE(result.pending.size() == 1);
    CHECK(result.pending[0] == "b.txt");
    CHECK(result.issues.size() == 1);
    CHECK(fixture.read("a.txt") == "dog");
    CHECK(fixture.read("b.txt") == "cat");
}
#endif
