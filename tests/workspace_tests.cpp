#include <astra/workspace/workspace.hpp>
#include <astra/core/utf8.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
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

TEST_CASE("workspace CR-only files use LF editor lines and preserve original CR disk bytes") {
    WorkspaceFixture fixture;
    fixture.write("file.txt", "one\r\xf0\x9f\x98\x80\r");
    Workspace workspace(fixture.root());
    const auto loaded = workspace.load("file.txt");
    CHECK(loaded.snapshot.text == "one\n\xf0\x9f\x98\x80\n");
    CHECK(*loaded.snapshot.version.raw_content == "one\r\xf0\x9f\x98\x80\r");
    CHECK(astra::Rope(loaded.snapshot.text).line_count() == 3);
    CHECK(astra::io::read_file(fixture.root() / "file.txt").text == "one\r\xf0\x9f\x98\x80\r");
    const auto saved = workspace.save("file.txt", "edited\n\xf0\x9f\x98\x80\n", loaded.snapshot.version, loaded.snapshot.encoding);
    CHECK(saved.text == "edited\n\xf0\x9f\x98\x80\n");
    CHECK(fixture.read("file.txt") == "edited\r\xf0\x9f\x98\x80\r");
    CHECK(workspace.load("file.txt").snapshot.text == saved.text);
}

TEST_CASE("workspace EditorConfig CR formatting never collapses returned editor lines") {
    WorkspaceFixture fixture;
    fixture.write(".editorconfig", "[*.txt]\nend_of_line=cr\ntrim_trailing_whitespace=true\ninsert_final_newline=true\n");
    Workspace workspace(fixture.root());
    const auto saved = workspace.save("file.txt", "one  \r\ntwo \t");
    CHECK(saved.text == "one\ntwo\n");
    CHECK(fixture.read("file.txt") == "one\rtwo\r");
    CHECK(astra::Rope(saved.text).line_count() == 3);
    CHECK(workspace.load("file.txt").snapshot.text == saved.text);
    const auto again = workspace.save("file.txt", saved.text + "three\n", saved.version, saved.encoding);
    CHECK(again.text == "one\ntwo\nthree\n");
    CHECK(fixture.read("file.txt") == "one\rtwo\rthree\r");
}

TEST_CASE("workspace CR-only search positions and multiline replacements share editor coordinates") {
    WorkspaceFixture fixture;
    fixture.write("file.txt", "a\rb\rc\r");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "a\nb";
    const auto searched = workspace.search(options);
    REQUIRE(searched.matches.size() == 1);
    CHECK(searched.matches[0].begin.value == 0);
    CHECK(searched.matches[0].end.value == 3);
    CHECK(searched.matches[0].start_position == astra::TextPosition{0, 0});
    CHECK(searched.matches[0].end_position == astra::TextPosition{1, 1});
    const auto plan = workspace.plan_replace(searched, "z\nz");
    REQUIRE(plan.files.size() == 1);
    CHECK(plan.files[0].text == "z\nz\nc\n");
    const auto result = workspace.apply_replace(plan);
    REQUIRE(result.applied.size() == 1);
    CHECK(result.pending.empty());
    CHECK(result.issues.empty());
    CHECK(result.applied[0].text == "z\nz\nc\n");
    CHECK(fixture.read("file.txt") == "z\rz\rc\r");
}

TEST_CASE("workspace preserves CR-only UTF16 encoding while explicit LF policy overrides disk endings") {
    WorkspaceFixture fixture;
    (void)astra::io::save_file(fixture.root() / "file.txt", "one\rtwo\r", TextEncoding::utf16_be);
    Workspace workspace(fixture.root());
    const auto loaded = workspace.load("file.txt");
    CHECK(loaded.snapshot.encoding == TextEncoding::utf16_be);
    CHECK(loaded.snapshot.text == "one\ntwo\n");
    const auto saved = workspace.save("file.txt", "edited\ntwo\n", loaded.snapshot.version, loaded.snapshot.encoding);
    CHECK(astra::io::read_file(fixture.root() / "file.txt").text == "edited\rtwo\r");
    CHECK(saved.encoding == TextEncoding::utf16_be);
    fixture.write(".editorconfig", "[*.txt]\nend_of_line=lf\n");
    const auto changed = workspace.save("file.txt", saved.text, saved.version, saved.encoding);
    CHECK(astra::io::read_file(fixture.root() / "file.txt").text == "edited\ntwo\n");
    CHECK(changed.text == "edited\ntwo\n");
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

TEST_CASE("workspace Unicode case insensitive matches preserve original bytes and exact disk versions") {
    WorkspaceFixture fixture;
    const std::string source = "\xF0\x9F\x98\x80 \xC3\x89" "cole \xC3\xA9" "COLE\r\n\xCE\xA3 \xCF\x83 \xCF\x82 K \xE2\x84\xAA k Stra\xC3\x9F" "e STRASSE";
    fixture.write("text.txt", source);
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "\xC3\xA9" "cole";
    options.case_sensitive = false;
    const auto found = workspace.search(options);
    REQUIRE(found.matches.size() == 2);
    CHECK(found.matches[0].start_position == astra::TextPosition{0, 3});
    CHECK(found.matches[0].end.value - found.matches[0].begin.value == 6);
    CHECK(*found.matches[0].version.raw_content == source);
    CHECK(found.options.case_sensitive == false);
    const auto plan = workspace.plan_replace(found, "$0"); // Literal mode never expands captures.
    REQUIRE(plan.files.size() == 1);
    CHECK(plan.files[0].text.starts_with("\xF0\x9F\x98\x80 $0 $0\r\n"));
    options.query = "\xCE\xA3";
    CHECK(workspace.search(options).matches.size() == 3);
    options.query = "k";
    CHECK(workspace.search(options).matches.size() == 3);
    options.query = "strasse";
    CHECK(workspace.search(options).matches.size() == 1); // Simple folding, not full ss/ssharp folding.
    options.query = "\xC3\xA9";
    const astra::workspace::SearchPattern pattern(options);
    CHECK(pattern.find_all("e\xCC\x81").matches.empty()); // Normalization is not implicit.
}

TEST_CASE("workspace regex captures named groups and replacements use documented dollar syntax") {
    WorkspaceFixture fixture;
    fixture.write("text.txt", "\xF0\x9F\x98\x80 CAF\xC3\x89=42 \xCE\xB1=7/x");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.regex = true;
    options.query = R"((?P<name>\p{L}+)=(\d+)(?:/(x))?)";
    const auto found = workspace.search(options);
    REQUIRE(found.matches.size() == 2);
    REQUIRE(found.matches[0].captures.size() == 4);
    CHECK_FALSE(found.matches[0].captures[3].has_value());
    CHECK(found.matches[1].captures[3].has_value());
    const auto plan = workspace.plan_replace(found, R"(${name}:$2:$3:$$:$0\n)");
    REQUIRE(plan.files.size() == 1);
    CHECK(plan.files[0].text == "\xF0\x9F\x98\x80 CAF\xC3\x89:42::$:CAF\xC3\x89=42\\n \xCE\xB1:7:x:$:\xCE\xB1=7/x\\n");
    const auto applied = workspace.apply_replace(plan);
    CHECK(applied.applied.size() == 1);
    CHECK(fixture.read("text.txt") == plan.files[0].text);
}

TEST_CASE("workspace regex empty matches advance by scalar and replace without duplicate offsets") {
    WorkspaceFixture fixture;
    fixture.write("text.txt", "\xF0\x9F\x98\x80" "a");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.regex = true;
    options.query = "(?:)";
    auto found = workspace.search(options);
    REQUIRE(found.matches.size() == 3);
    CHECK(found.matches[0].begin.value == 0);
    CHECK(found.matches[1].begin.value == 4);
    CHECK(found.matches[2].begin.value == 5);
    CHECK(found.matches[1].start_position == astra::TextPosition{0, 2});
    const auto plan = workspace.plan_replace(found, "-");
    REQUIRE(plan.files.size() == 1);
    CHECK(plan.files[0].text == "-\xF0\x9F\x98\x80-a-");
    found.matches.insert(found.matches.begin(), found.matches.front());
    CHECK_THROWS_AS((void)workspace.plan_replace(found, "-"), std::invalid_argument);
    const astra::workspace::SearchPattern pattern(options);
    REQUIRE(pattern.find_all("").matches.size() == 1);
    CHECK(pattern.expand_replacement("", pattern.find_all("").matches.front(), "$0!") == "!");
}

TEST_CASE("workspace regex multiline positions preserve CR only disk identity") {
    WorkspaceFixture fixture;
    fixture.write("text.txt", "one\rtwo\rthree\r");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.regex = true;
    options.query = R"((?m)^(\p{L}+)$)";
    const auto found = workspace.search(options);
    REQUIRE(found.matches.size() == 3);
    CHECK(found.matches[1].start_position == astra::TextPosition{1, 0});
    CHECK(found.matches[1].begin.value == 4);
    CHECK(*found.matches[1].version.raw_content == "one\rtwo\rthree\r");
    const auto plan = workspace.plan_replace(found, "[$1]");
    REQUIRE(plan.files.size() == 1);
    CHECK(plan.files[0].text == "[one]\n[two]\n[three]\n");
    REQUIRE(workspace.apply_replace(plan).applied.size() == 1);
    CHECK(fixture.read("text.txt") == "[one]\r[two]\r[three]\r");
}

TEST_CASE("workspace search rejects unsafe regex and malformed replacement references before mutation") {
    SearchOptions options;
    options.regex = true;
    for (const auto* query : {"[", "(?=a)", "(a)\\1", "\\C"}) {
        options.query = query;
        CHECK_THROWS_AS((void)astra::workspace::SearchPattern(options), std::invalid_argument);
    }
    options.query = std::string(65, '(') + "a" + std::string(65, ')');
    CHECK_THROWS_AS((void)astra::workspace::SearchPattern(options), std::invalid_argument);
    options.query = R"((?P<letter>a)(b)?)";
    const astra::workspace::SearchPattern pattern(options);
    for (const auto* rewrite : {"$", "$x", "$3", "$100", "${missing}", "${letter"}) {
        CHECK_THROWS_AS(pattern.validate_replacement(rewrite), std::invalid_argument);
    }
    auto match = pattern.find_all("a").matches.front();
    CHECK(pattern.expand_replacement("a", match, "$2-${letter}-$0-$$") == "-a-a-$");
    CHECK_THROWS_AS((void)pattern.expand_replacement("a", match, "$0$0$0", 2), std::length_error);
    match.captures[1] = astra::workspace::TextRange{astra::ByteOffset{0}, astra::ByteOffset{0}};
    CHECK_THROWS_AS((void)pattern.expand_replacement("a", match, "$1"), std::invalid_argument);
    options.query = R"(\Q\C\E)";
    CHECK(astra::workspace::SearchPattern(options).find_all("\\C").matches.size() == 1);
    options.query = R"(\\C)";
    CHECK(astra::workspace::SearchPattern(options).find_all("\\C").matches.size() == 1);
}

TEST_CASE("workspace regex and literal whole word filtering use Unicode boundaries") {
    SearchOptions options;
    options.query = "cat|dog";
    options.regex = true;
    options.case_sensitive = false;
    options.whole_word = true;
    const astra::workspace::SearchPattern pattern(options);
    const auto result = pattern.find_all("CAT dog \xC3\xA9" "cat cat\xCC\x81 dog_ \xF0\x9F\x98\x80" "Dog\xF0\x9F\x98\x80");
    CHECK(result.matches.size() == 3);
}

TEST_CASE("workspace search limits cancellation and hostile regex are bounded") {
    SearchOptions options;
    options.query = "a";
    options.max_matches = 2;
    auto result = astra::workspace::SearchPattern(options).find_all("aaa");
    CHECK(result.matches.size() == 2);
    CHECK(result.truncated);
    CHECK_FALSE(astra::workspace::SearchPattern(options).find_all("aa").truncated);
    options.max_result_bytes = 0;
    CHECK(astra::workspace::SearchPattern(options).find_all("a").truncated);
    options.max_result_bytes = 1024;
    std::stop_source source;
    (void)source.request_stop();
    const auto cancelled = astra::workspace::SearchPattern(options).find_all("aaa", source.get_token());
    CHECK(cancelled.cancelled);
    CHECK(cancelled.matches.empty());
    CHECK_THROWS_AS((void)astra::workspace::SearchPattern(options).find_all("\xFF"), std::invalid_argument);
    options.max_file_bytes = 2;
    CHECK_THROWS_AS((void)astra::workspace::SearchPattern(options).find_all("aaa"), std::length_error);
    options.query = std::string(astra::workspace::SearchPattern::max_query_bytes + 1, 'a');
    CHECK_THROWS_AS((void)astra::workspace::SearchPattern(options), std::invalid_argument);
    options = {};
    options.regex = true;
    options.query = "(a+)+$";
    const auto started = std::chrono::steady_clock::now();
    const auto hostile = astra::workspace::SearchPattern(options).find_all(std::string(200000, 'a') + "!");
    CHECK(hostile.matches.empty());
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
}

TEST_CASE("workspace find next preserves full regex context and Unicode boundaries") {
    using astra::ByteOffset;
    using astra::workspace::SearchPattern;
    SearchOptions options; options.regex = true; options.query = "^cat";
    auto found = SearchPattern(options).find_next("cat cat", ByteOffset{4}, false);
    CHECK_FALSE(found.match);
    found = SearchPattern(options).find_next("cat cat", ByteOffset{4});
    REQUIRE(found.match); CHECK(found.match->begin.value == 0); CHECK(found.wrapped);
    options.query = "\\bcat\\b";
    CHECK_FALSE(SearchPattern(options).find_next("xcat", ByteOffset{1}).match);
    options.query = "cat|dog"; options.case_sensitive = false; options.whole_word = true;
    found = SearchPattern(options).find_next("CAT \xC3\xA9" "cat dog_ Dog", ByteOffset{4});
    REQUIRE(found.match); CHECK(found.match->begin.value == 15); CHECK_FALSE(found.wrapped);
    options = {}; options.query = "🙂";
    found = SearchPattern(options).find_next("中🙂中🙂", ByteOffset{7}, false);
    REQUIRE(found.match); CHECK(found.match->begin.value == 10); CHECK(found.match->end.value == 14);
    CHECK_THROWS_AS((void)SearchPattern(options).find_next("中🙂", ByteOffset{1}), std::invalid_argument);
    CHECK_THROWS_AS((void)SearchPattern(options).find_next("中🙂", ByteOffset{8}), std::invalid_argument);
    CHECK_THROWS_AS((void)SearchPattern(options).find_next("\xFF", ByteOffset{0}), std::invalid_argument);
    std::stop_source source; (void)source.request_stop();
    found = SearchPattern(options).find_next("中🙂", ByteOffset{0}, true, source.get_token());
    CHECK(found.cancelled); CHECK_FALSE(found.match);
}

TEST_CASE("workspace find next retains one match beyond the all match budget") {
    using astra::ByteOffset;
    using astra::workspace::SearchPattern;
    SearchOptions options; options.query = "a"; options.max_matches = 0; options.max_result_bytes = 0;
    const auto found = SearchPattern(options).find_next(std::string(100000, 'a'), ByteOffset{99999});
    REQUIRE(found.match); CHECK(found.match->begin.value == 99999); CHECK_FALSE(found.truncated);
    options.query = "$"; options.regex = true;
    const auto eof = SearchPattern(options).find_next("中🙂", ByteOffset{7});
    REQUIRE(eof.match); CHECK(eof.match->begin.value == 7); CHECK(eof.match->end.value == 7);
    const auto empty = SearchPattern(options).find_next("", ByteOffset{0});
    REQUIRE(empty.match); CHECK(empty.match->begin.value == 0); CHECK_FALSE(empty.wrapped);
}

TEST_CASE("workspace find next agrees with a flat literal oracle at random scalar boundaries") {
    constexpr auto seed = 0xA57FA11U;
    std::mt19937 random(seed); CAPTURE(seed);
    const std::array<std::string_view,6> parts{"a","b"," ","中","🙂","e\xCC\x81"};
    for (unsigned sample = 0; sample < 300; ++sample) {
        std::string input;
        for (unsigned i=0;i<80;++i) input += parts[random()%parts.size()];
        std::vector<std::size_t> boundaries{0};
        for (std::size_t index=0;index<input.size();) { index += astra::utf8::decode(input,index).bytes;boundaries.push_back(index); }
        const auto begin = boundaries[random()%boundaries.size()];
        SearchOptions options; options.query = std::string(parts[random()%parts.size()]);
        for (const auto wrap : {false,true}) {
            auto expected = input.find(options.query,begin);
            const auto wrapped = wrap && expected == std::string::npos && begin != 0;
            if (wrapped) expected = input.find(options.query);
            const auto actual = astra::workspace::SearchPattern(options).find_next(input,astra::ByteOffset{begin},wrap);
            CHECK(actual.match.has_value() == (expected != std::string::npos));
            if (actual.match) {
                CHECK(actual.match->begin.value == expected);
                CHECK(actual.match->end.value == expected+options.query.size());
                CHECK(actual.wrapped == wrapped);
            }
            CHECK_FALSE(actual.cancelled); CHECK_FALSE(actual.truncated);
        }
    }
}

TEST_CASE("workspace previous search agrees with a flat Unicode oracle and retains one result") {
    constexpr auto seed=0xA57BA11U;std::mt19937 random(seed);CAPTURE(seed);
    const std::array<std::string_view,6> parts{"a","b"," ","中","🙂","e\xCC\x81"};
    for(unsigned sample=0;sample<300;++sample) {
        std::string input;for(unsigned i=0;i<80;++i)input+=parts[random()%parts.size()];
        std::vector<std::size_t> boundaries{0};
        for(std::size_t i=0;i<input.size();){i+=astra::utf8::decode(input,i).bytes;boundaries.push_back(i);}
        const auto begin=boundaries[random()%boundaries.size()];
        SearchOptions options;options.query=parts[random()%parts.size()];
        for(const auto wrap:{false,true}) {
            auto expected=begin ? input.rfind(options.query,begin-1) : std::string::npos;
            const bool wrapped=expected==std::string::npos && wrap;
            if(wrapped)expected=input.rfind(options.query);
            const auto actual=astra::workspace::SearchPattern(options).find_previous(input,astra::ByteOffset{begin},wrap);
            CHECK(actual.match.has_value()==(expected!=std::string::npos));
            if(actual.match){CHECK(actual.match->begin.value==expected);CHECK(actual.match->end.value==expected+options.query.size());CHECK(actual.wrapped==wrapped);}
            CHECK_FALSE(actual.cancelled);CHECK_FALSE(actual.truncated);
        }
    }
    SearchOptions options;options.query="a";
    const auto many=astra::workspace::SearchPattern(options).find_previous(std::string(10000,'a'),astra::ByteOffset{9999});
    REQUIRE(many.match);CHECK(many.match->begin.value==9998);
    options.regex=true;options.query="^|$";
    auto found=astra::workspace::SearchPattern(options).find_previous("中🙂",astra::ByteOffset{7});
    REQUIRE(found.match);CHECK(found.match->begin.value==0);CHECK_FALSE(found.wrapped);
    found=astra::workspace::SearchPattern(options).find_previous("中🙂",astra::ByteOffset{0});
    REQUIRE(found.match);CHECK(found.match->begin.value==7);CHECK(found.wrapped);
    std::stop_source cancelled;cancelled.request_stop();
    found=astra::workspace::SearchPattern(options).find_previous("中🙂",astra::ByteOffset{0},true,cancelled.get_token());
    CHECK(found.cancelled);CHECK_FALSE(found.match);
    CHECK_THROWS_AS((void)astra::workspace::SearchPattern(options).find_previous("中🙂",astra::ByteOffset{1}),std::invalid_argument);
    CHECK_THROWS_AS((void)astra::workspace::SearchPattern(options).find_previous("\xFF",astra::ByteOffset{0}),std::invalid_argument);
}

TEST_CASE("workspace excludes buffer overlay paths before file and byte budgets") {
    WorkspaceFixture fixture;
    fixture.write("a.txt", std::string(2000, 'x'));
    fixture.write("b.txt", "cat");
    Workspace workspace(fixture.root());
    SearchOptions options;
    options.query = "cat";
    options.max_files = 1;
    options.max_total_bytes = 3;
    options.max_file_bytes = 3;
#if defined(_WIN32)
    options.excluded_paths = {"A.TXT"};
#else
    options.excluded_paths = {"a.txt"};
#endif
    const auto found = workspace.search(options);
    REQUIRE(found.matches.size() == 1);
    CHECK(found.matches[0].relative_path == "b.txt");
    CHECK_FALSE(found.truncated);
    CHECK(found.issues.empty());
    CHECK(found.options.excluded_paths.empty());
    options.excluded_paths = {"../outside"};
    CHECK_THROWS_AS((void)workspace.search(options), std::invalid_argument);
}

TEST_CASE("workspace literal Unicode folding agrees with deterministic scalar reference") {
    const std::array<std::string, 12> alphabet{"A", "a", "K", "k", "\xE2\x84\xAA", "\xCE\xA3", "\xCF\x83", "\xCF\x82", "_", "\xF0\x9F\x98\x80", "\xC3\x89", "\xC3\xA9"};
    const std::array<int, 12> folded{0, 0, 1, 1, 1, 2, 2, 2, 3, 4, 5, 5};
    std::mt19937 random(0x5EAC1234U);
    for (std::size_t trial = 0; trial < 300; ++trial) {
        std::vector<std::size_t> scalars;
        std::vector<std::size_t> boundaries{0};
        std::string input;
        for (std::size_t index = 0; index < 60; ++index) {
            const auto scalar = static_cast<std::size_t>(random() % alphabet.size());
            scalars.push_back(scalar);
            input += alphabet[scalar];
            boundaries.push_back(input.size());
        }
        SearchOptions options;
        options.case_sensitive = false;
        const auto count = static_cast<std::size_t>(random() % 4 + 1);
        std::vector<int> query;
        for (std::size_t index = 0; index < count; ++index) {
            const auto scalar = static_cast<std::size_t>(random() % alphabet.size());
            options.query += alphabet[scalar];
            query.push_back(folded[scalar]);
        }
        std::vector<astra::workspace::TextRange> expected;
        for (std::size_t index = 0; index + count <= scalars.size();) {
            bool matches = true;
            for (std::size_t part = 0; part < count; ++part) { matches = matches && folded[scalars[index + part]] == query[part]; }
            if (matches) {
                expected.push_back({astra::ByteOffset{boundaries[index]}, astra::ByteOffset{boundaries[index + count]}});
                index += count;
            } else { ++index; }
        }
        const auto actual = astra::workspace::SearchPattern(options).find_all(input);
        REQUIRE(actual.matches.size() == expected.size());
        for (std::size_t index = 0; index < expected.size(); ++index) {
            CHECK(actual.matches[index].begin == expected[index].begin);
            CHECK(actual.matches[index].end == expected[index].end);
            CHECK(actual.matches[index].captures[0] == expected[index]);
        }
    }
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
