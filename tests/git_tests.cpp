#include <astra/git/repository.hpp>
#include <astra/process/process.hpp>
#include "support/temporary_directory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace astra::git;

[[nodiscard]] std::string utf8_path(const fs::path& path) {
    const auto utf8 = path.generic_u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

struct GitFixture {
    fs::path base;
    fs::path directory;

    GitFixture() {
        base = fs::weakly_canonical(fs::current_path() / "build");
        (void)fs::create_directories(base);
        const auto nonce = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(std::random_device{}());
        directory = base / ("git-tests-" + nonce);
        if (!fs::create_directory(directory)) { throw std::runtime_error("Git fixture already exists"); }
        (void)command({"init", "--initial-branch=main"});
        (void)command({"config", "core.autocrlf", "false"});
        (void)command({"config", "commit.gpgsign", "false"});
    }

    ~GitFixture() {
        const auto issues = astra::test::cleanup_owned_directory(directory, base, "git-tests-", std::chrono::seconds(1));
        for (const auto& issue : issues) {
            CHECK_MESSAGE(false, "Git fixture cleanup ", utf8_path(issue.path), ": ", issue.message);
        }
    }
    void write(const fs::path& path, std::string_view text) const {
        (void)fs::create_directories((directory / path).parent_path());
        std::ofstream file(directory / path, std::ios::binary | std::ios::trunc);
        file.exceptions(std::ios::badbit | std::ios::failbit);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    [[nodiscard]] std::string command(std::vector<std::string> arguments, bool allow_failure = false) const {
        astra::process::ProcessOptions options;
        options.argv = {"git", "--no-pager", "-c", "user.name=Astra Tests", "-c", "user.email=tests@astra.invalid",
            "-c", "core.fsmonitor=false", "-c", "core.hooksPath=NUL", "-c", "commit.gpgsign=false"};
        options.argv.insert(options.argv.end(), arguments.begin(), arguments.end());
        options.cwd = utf8_path(directory);
        options.shutdown_timeout = std::chrono::seconds(10);
        options.env = astra::process::current_environment();
        std::erase_if(*options.env, [](const std::string& item) {
            return item.starts_with("GIT_") || item.starts_with("LC_ALL=");
        });
        options.env->insert(options.env->end(), {"GIT_CONFIG_NOSYSTEM=1", "GIT_CONFIG_GLOBAL=NUL",
            "GIT_CONFIG_SYSTEM=NUL", "GIT_ATTR_NOSYSTEM=1", "GIT_TERMINAL_PROMPT=0", "LC_ALL=C"});
        std::string output, errors;
        astra::process::ProcessCallbacks callbacks;
        callbacks.on_stdout = [&](std::string_view bytes) { output += bytes; };
        callbacks.on_stderr = [&](std::string_view bytes) { errors += bytes; };
        astra::process::Process child(std::move(options), std::move(callbacks));
        child.start().get();
        child.close();
        const auto completed = child.completion();
        if (completed.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
            child.terminate();
            throw std::runtime_error("Git fixture command timed out");
        }
        const auto result = completed.get();
        if (!result.error.empty() || (!allow_failure && result.exit_status != 0)) {
            throw std::runtime_error("Git fixture command failed: " + result.error + errors);
        }
        return output;
    }

    void commit(std::string message = "fixture commit") const {
        (void)command({"add", "--all"});
        (void)command({"commit", "--no-gpg-sign", "-m", std::move(message)});
    }
};

[[nodiscard]] const StatusEntry* entry(const RepositoryStatus& status, const fs::path& path) {
    const auto found = std::ranges::find_if(status.entries, [&](const auto& item) { return item.path == path; });
    return found == status.entries.end() ? nullptr : &*found;
}

void stage_one(const Repository& repository, fs::path path) {
    const std::vector<StagePath> paths{{std::move(path), std::nullopt}};
    repository.stage(paths);
}

void unstage_one(const Repository& repository, fs::path path) {
    const std::vector<fs::path> paths{std::move(path)};
    repository.unstage(paths);
}

} // namespace

TEST_CASE("Git status detects unborn branch untracked staged and unstaged files") {
    GitFixture fixture;
    Repository repository(fixture.directory, "git", {false});
    auto status = repository.status();
    CHECK(status.unborn);
    CHECK(status.branch == "main");
    CHECK(status.entries.empty());
    CHECK(repository.log().empty());
    fixture.write("first.txt", "initial\n");
    status = repository.status();
    REQUIRE(entry(status, "first.txt"));
    CHECK(entry(status, "first.txt")->unstaged == FileState::untracked);
    stage_one(repository, "first.txt");
    status = repository.status();
    REQUIRE(entry(status, "first.txt"));
    CHECK(entry(status, "first.txt")->staged == FileState::added);
    fixture.write("first.txt", "modified\n");
    status = repository.status();
    CHECK(entry(status, "first.txt")->unstaged == FileState::modified);
    unstage_one(repository, "first.txt");
    status = repository.status();
    CHECK(entry(status, "first.txt")->unstaged == FileState::untracked);
    CHECK(astra::io::read_file(fixture.directory / "first.txt").text == "modified\n");
}

TEST_CASE("Git stage honors repository attributes and line-ending conversion") {
    GitFixture fixture;
    fixture.write(".gitattributes", "*.txt text eol=lf\n");
    fixture.write("file.txt", "one\r\ntwo\r\n");
    Repository repository(fixture.directory, "git", {false});
    stage_one(repository, "file.txt");
    CHECK(repository.read_blob("file.txt") == "one\ntwo\n");
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "one\r\ntwo\r\n");
}

TEST_CASE("Git committed staged and unstaged blobs remain distinct") {
    GitFixture fixture;
    fixture.write("file.txt", "head\n");
    fixture.commit("initial state");
    Repository repository(fixture.directory, "git", {false});
    fixture.write("file.txt", "index\n");
    stage_one(repository, "file.txt");
    fixture.write("file.txt", "worktree\n");
    CHECK(repository.read_blob("file.txt", DiffBase::head) == "head\n");
    CHECK(repository.read_blob("file.txt", DiffBase::index) == "index\n");
    auto status = repository.status();
    REQUIRE(entry(status, "file.txt"));
    CHECK(entry(status, "file.txt")->staged == FileState::modified);
    CHECK(entry(status, "file.txt")->unstaged == FileState::modified);
    unstage_one(repository, "file.txt");
    CHECK(repository.read_blob("file.txt") == "head\n");
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "worktree\n");
}

TEST_CASE("Git status recognizes staged rename and deleted path") {
    GitFixture fixture;
    fixture.write("old name.txt", "rename me\n");
    fixture.write("delete.txt", "delete me\n");
    fixture.commit();
    fs::rename(fixture.directory / "old name.txt", fixture.directory / "new name.txt");
    (void)fs::remove(fixture.directory / "delete.txt");
    Repository repository(fixture.directory, "git", {false});
    const std::vector<StagePath> paths{{"old name.txt", {}}, {"new name.txt", {}}, {"delete.txt", {}}};
    repository.stage(paths);
    const auto status = repository.status();
    const auto renamed = entry(status, "new name.txt");
    REQUIRE(renamed);
    CHECK(renamed->staged == FileState::renamed);
    REQUIRE(renamed->original_path);
    CHECK(*renamed->original_path == fs::path("old name.txt"));
    REQUIRE(entry(status, "delete.txt"));
    CHECK(entry(status, "delete.txt")->staged == FileState::deleted);
}

TEST_CASE("Git argv literal pathspecs handle Unicode spaces and glob punctuation") {
    GitFixture fixture;
    const fs::path path = fs::path(u8"星 [one] space.txt");
    fixture.write(path, "unicode\n");
    fixture.write("other.txt", "other\n");
    Repository repository(fixture.directory, "git", {false});
    stage_one(repository, path);
    const auto status = repository.status();
    REQUIRE(entry(status, path));
    CHECK(entry(status, path)->staged == FileState::added);
    CHECK(entry(status, "other.txt")->unstaged == FileState::untracked);
    CHECK(repository.read_blob(path) == "unicode\n");
}

TEST_CASE("Git diff supplies actual patches and added modified deleted gutter ranges") {
    GitFixture fixture;
    fixture.write("file.txt", "one\ntwo\nthree\nfour\n");
    fixture.commit();
    fixture.write("file.txt", "one\nchanged\nthree\nfour\nnew\n");
    Repository repository(fixture.directory, "git", {false});
    auto diff = repository.diff("file.txt");
    CHECK(diff.patch.find("-two") != std::string::npos);
    CHECK(diff.patch.find("+changed") != std::string::npos);
    REQUIRE(diff.hunks.size() == 2);
    CHECK(diff.gutter[0].kind == GutterKind::modified);
    CHECK(diff.gutter[0].line == 1);
    CHECK(diff.gutter[1].kind == GutterKind::added);
    CHECK(diff.gutter[1].line == 4);
    stage_one(repository, "file.txt");
    CHECK(repository.diff("file.txt").patch.empty());
    CHECK_FALSE(repository.diff("file.txt", DiffBase::index, true).patch.empty());
    fixture.write("file.txt", "one\nthree\nfour\nnew\n");
    diff = repository.diff("file.txt");
    REQUIRE(diff.gutter.size() == 1);
    CHECK(diff.gutter[0].kind == GutterKind::deleted);
    CHECK(diff.gutter[0].count == 0);
    CHECK(diff.gutter[0].removed_lines == 1);
}

TEST_CASE("Git untracked diff displays additions without staging the file") {
    GitFixture fixture;
    fixture.write("new.txt", "first\nsecond\n");
    Repository repository(fixture.directory, "git", {false});
    const auto diff = repository.diff("new.txt");
    REQUIRE(diff.hunks.size() == 1);
    CHECK(diff.hunks[0].old_count == 0);
    CHECK(diff.hunks[0].new_count == 2);
    CHECK(diff.gutter[0].kind == GutterKind::added);
    const auto status = repository.status();
    CHECK(entry(status, "new.txt")->unstaged == FileState::untracked);
}

TEST_CASE("Git history and branch actions preserve normal working-tree safety") {
    GitFixture fixture;
    fixture.write("file.txt", "main\n");
    fixture.commit("first commit");
    Repository repository(fixture.directory, "git", {false});
    auto history = repository.log(10);
    REQUIRE(history.size() == 1);
    CHECK(history[0].subject == "first commit");
    CHECK(history[0].author == "Astra Tests");
    CHECK(history[0].parents.empty());
    repository.create_branch("feature/test", true);
    fixture.write("file.txt", "feature\n");
    fixture.commit("second commit");
    history = repository.log(1);
    REQUIRE(history.size() == 1);
    CHECK(history[0].subject == "second commit");
    CHECK(history[0].parents.size() == 1);
    const auto branches = repository.branches();
    REQUIRE(branches.size() == 2);
    CHECK(std::ranges::any_of(branches, [](const auto& branch) {
        return branch.ref == "refs/heads/feature/test" && branch.current;
    }));
    repository.switch_branch("main");
    CHECK(repository.status().branch == "main");
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "main\n");
    fixture.write("file.txt", "unsaved worktree\n");
    CHECK_THROWS_AS(repository.switch_branch("feature/test"), GitError);
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "unsaved worktree\n");
}

TEST_CASE("Git commit records only the existing index with explicit isolated identity") {
    GitFixture fixture;
    fixture.write("file.txt", "original\n");
    fixture.commit("base");
    Repository repository(fixture.directory, "git", {false});
    fixture.write("file.txt", "staged\n");
    stage_one(repository, "file.txt");
    fixture.write("file.txt", "still unstaged\n");
    fixture.write(".git/hooks/pre-commit", "#!/bin/sh\nexit 77\n");
    GitOptions identity;
    identity.author_name = "Astra Isolated Commit";
    identity.author_email = "isolated@astra.invalid";
    repository.commit("record staged changes\n\nLiteral $(text) stays literal.\n", identity);
    const auto history = repository.log(1);
    REQUIRE(history.size() == 1);
    CHECK(history[0].subject == "record staged changes");
    CHECK(history[0].author == "Astra Isolated Commit");
    CHECK(history[0].email == "isolated@astra.invalid");
    CHECK(repository.read_blob("file.txt", DiffBase::head) == "staged\n");
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "still unstaged\n");
    const auto status = repository.status();
    REQUIRE(entry(status, "file.txt"));
    CHECK(entry(status, "file.txt")->staged == FileState::unchanged);
    CHECK(entry(status, "file.txt")->unstaged == FileState::modified);
    CHECK_THROWS_AS(repository.commit("nothing staged", identity), GitError);
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "still unstaged\n");
}

TEST_CASE("Git commit rejects invalid messages and identity without writing a commit") {
    GitFixture fixture;
    fixture.write("file.txt", "staged\n");
    Repository repository(fixture.directory, "git", {false});
    stage_one(repository, "file.txt");
    GitOptions identity;
    identity.author_name = "Astra Tests";
    identity.author_email = "tests@astra.invalid";
    CHECK_THROWS_AS(repository.commit(" \r\n\t", identity), std::invalid_argument);
    CHECK_THROWS_AS(repository.commit("\xc0\xaf", identity), std::invalid_argument);
    CHECK_THROWS_AS(repository.commit(std::string("a\0b", 3), identity), std::invalid_argument);
    CHECK_THROWS_AS(repository.commit(std::string(1024 * 1024 + 1, 'x'), identity), std::invalid_argument);
    identity.author_name = "line\nbreak";
    CHECK_THROWS_AS(repository.commit("valid message", identity), std::invalid_argument);
    CHECK(repository.status().unborn);
}

TEST_CASE("Git status reports actual merge conflicts without losing their worktree") {
    GitFixture fixture;
    fixture.write("file.txt", "base\n");
    fixture.commit();
    Repository repository(fixture.directory, "git", {false});
    repository.create_branch("feature", true);
    fixture.write("file.txt", "feature\n");
    fixture.commit();
    repository.switch_branch("main");
    fixture.write("file.txt", "main\n");
    fixture.commit();
    (void)fixture.command({"merge", "--no-edit", "feature"}, true);
    const auto status = repository.status();
    const auto conflict = entry(status, "file.txt");
    REQUIRE(conflict);
    CHECK(conflict->conflict);
    CHECK(conflict->staged == FileState::unmerged);
    CHECK(conflict->unstaged == FileState::unmerged);
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text.find("<<<<<<<") != std::string::npos);
}

TEST_CASE("Git stage expected versions reject external changes before index mutation") {
    GitFixture fixture;
    fixture.write("file.txt", "first\n");
    Repository repository(fixture.directory, "git", {false});
    const auto snapshot = astra::io::read_file(fixture.directory / "file.txt");
    fixture.write("file.txt", "other\n");
    const std::vector<StagePath> paths{{"file.txt", snapshot.version}};
    CHECK_THROWS_AS(repository.stage(paths), astra::io::FileConflict);
    const auto status = repository.status();
    CHECK(entry(status, "file.txt")->unstaged == FileState::untracked);
}

TEST_CASE("Git path branch cancellation and output limits are enforced") {
    GitFixture fixture;
    fixture.write("file.txt", "data\n");
    Repository repository(fixture.directory, "git", {false});
    CHECK_THROWS_AS((void)repository.diff("../outside.txt"), std::invalid_argument);
    CHECK_THROWS_AS((void)repository.diff(fixture.directory / "file.txt"), std::invalid_argument);
    CHECK_THROWS_AS((void)repository.diff(".git/config"), std::invalid_argument);
    CHECK_THROWS_AS(repository.create_branch("--help"), std::invalid_argument);
    CHECK_THROWS_AS(repository.create_branch("bad name"), GitError);
    std::stop_source cancellation;
    (void)cancellation.request_stop();
    GitOptions cancelled;
    cancelled.stop = cancellation.get_token();
    CHECK_THROWS_AS((void)repository.status(cancelled), GitCancelled);
    GitOptions bounded;
    bounded.max_output_bytes = 1;
    CHECK_THROWS_AS((void)repository.status(bounded), GitError);
    CHECK_FALSE(repository.status().entries.empty());
    (void)fs::create_directories(fixture.directory / "nested");
    CHECK_THROWS_AS((void)Repository(fixture.directory / "nested", "git", {false}), std::invalid_argument);
}

TEST_CASE("Git discard restores filtered CRLF bytes atomically with a version guard") {
    GitFixture fixture;
    fixture.write(".gitattributes", "*.txt text eol=crlf\n");
    fixture.write("file.txt", "original\r\n");
    fixture.commit();
    fixture.write("file.txt", "edited\r\n");
    Repository repository(fixture.directory, "git", {false});
    const auto edited = astra::io::read_file(fixture.directory / "file.txt");
    const auto restored = repository.discard("file.txt", edited.version);
    CHECK(restored.text == "original\r\n");
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "original\r\n");
    fixture.write("file.txt", "new external\r\n");
    CHECK_THROWS_AS((void)repository.discard("file.txt", restored.version), astra::io::FileConflict);
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "new external\r\n");
}

TEST_CASE("Git discard can restore an absent path but refuses newly created content") {
    GitFixture fixture;
    fixture.write("file.txt", "original\n");
    fixture.commit();
    (void)fs::remove(fixture.directory / "file.txt");
    Repository repository(fixture.directory, "git", {false});
    CHECK(repository.discard("file.txt", std::nullopt).text == "original\n");
    fixture.write("file.txt", "precious\n");
    CHECK_THROWS_AS((void)repository.discard("file.txt", std::nullopt), astra::io::FileConflict);
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "precious\n");
}

TEST_CASE("Git discard rejects missing index or binary conversion without changing the file") {
    GitFixture fixture;
    fixture.write("binary.bin", std::string("a\0b", 3));
    fixture.commit();
    fixture.write("binary.bin", "edited text\n");
    fixture.write("untracked.txt", "precious\n");
    Repository repository(fixture.directory, "git", {false});
    const auto edited = astra::io::read_file(fixture.directory / "binary.bin");
    CHECK_THROWS_AS((void)repository.discard("binary.bin", edited.version), std::invalid_argument);
    CHECK(astra::io::read_file(fixture.directory / "binary.bin").text == "edited text\n");
    const auto untracked = astra::io::read_file(fixture.directory / "untracked.txt");
    CHECK_THROWS_AS((void)repository.discard("untracked.txt", untracked.version), GitError);
    CHECK(astra::io::read_file(fixture.directory / "untracked.txt").text == "precious\n");
}

TEST_CASE("Git discard filter failure leaves the original worktree version unchanged") {
    GitFixture fixture;
    fixture.write("file.txt", "committed\n");
    fixture.commit();
    fixture.write(".gitattributes", "*.txt filter=astra-failing-filter\n");
    (void)fixture.command({"config", "filter.astra-failing-filter.smudge", "astra-nonexistent-filter-test-command"});
    (void)fixture.command({"config", "filter.astra-failing-filter.required", "true"});
    fixture.write("file.txt", "preserve me\n");
    Repository repository(fixture.directory, "git", {false});
    const auto before = astra::io::read_file(fixture.directory / "file.txt");
    CHECK_THROWS_AS((void)repository.discard("file.txt", before.version), GitError);
    CHECK(astra::io::read_file(fixture.directory / "file.txt").text == "preserve me\n");
}

TEST_CASE("Git porcelain parser preserves rename order names and branch metadata") {
    std::string input;
    for (const auto line : {"# branch.oid abc123", "# branch.head main", "# branch.upstream origin/main",
        "# branch.ab +3 -2", "2 R. N... 100644 100644 100644 aaaa bbbb R100 new name.txt",
        "old name.txt", "? odd\nname.txt"}) {
        input += line;
        input.push_back('\0');
    }
    const auto status = parse_status(input);
    CHECK(status.branch == "main");
    CHECK(status.ahead == 3);
    CHECK(status.behind == 2);
    REQUIRE(status.entries.size() == 2);
    CHECK(status.entries[0].path == fs::path("new name.txt"));
    CHECK(status.entries[0].original_path == fs::path("old name.txt"));
    CHECK(status.entries[1].path == fs::path("odd\nname.txt"));
    CHECK_THROWS_AS((void)parse_status(input, 1), std::length_error);
    CHECK_THROWS_AS((void)parse_status("? missing-terminator"), GitError);
    CHECK_THROWS_AS((void)parse_status(std::string("1 malformed\0", 12)), GitError);
}

TEST_CASE("Git diff parser handles omitted counts zero-side insertions and binary patches") {
    const auto result = parse_diff("@@ -2 +2,3 @@ function\n-old\n+new\n@@ -5,2 +7,0 @@\n-a\n-b\n");
    REQUIRE(result.hunks.size() == 2);
    CHECK(result.hunks[0].old_count == 1);
    CHECK(result.hunks[0].new_count == 3);
    CHECK(result.gutter[1].kind == GutterKind::deleted);
    CHECK(result.gutter[1].line == 7);
    CHECK(parse_diff("Binary files a/a and b/a differ\n").binary);
    CHECK_THROWS_AS((void)parse_diff("@@ invalid @@\n"), GitError);
    CHECK_THROWS_AS((void)parse_diff("@@ -1 +1 @@\n@@ -3 +3 @@\n", 1), std::length_error);
}
