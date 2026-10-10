#include <astra/recovery/store.hpp>
#include <astra/process/process.hpp>
#include "support/temporary_directory.hpp"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using namespace astra;
using namespace astra::recovery;
using namespace std::chrono_literals;

struct Fixture {
    fs::path base = fs::weakly_canonical(fs::current_path() / "build");
    fs::path directory =
        base / ("recovery-tests-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(std::random_device{}()));

    Fixture() {
        if (!fs::create_directory(directory)) {
            throw std::runtime_error("recovery fixture collision");
        }
    }

    ~Fixture() {
        for (const auto& issue :
             astra::test::cleanup_owned_directory(directory, base, "recovery-tests-")) {
            CHECK_MESSAGE(false, "Recovery cleanup: ", issue.message);
        }
    }

    [[nodiscard]] fs::path state() const {
        return directory / "state";
    }
};

RecoveryRecord record(const Document& document) {
    return {document.snapshot(), {}, io::TextEncoding::utf8, {}};
}

void overwrite(const fs::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE(
    "recovery checkpoints preserve Unicode text selection identity encoding and raw disk version") {
    Fixture fixture;
    const auto path = fixture.directory / fs::path(u8"星.txt");
    const auto version = io::save_file(path, "original\r\n", io::TextEncoding::utf16_le);
    Document document({7}, "A\xf0\x9f\x98\x80\r\nlast");
    document.set_selections({{{5}, {1}}, {{7}, {7}}});
    REQUIRE(document.apply({document.revision(), {{{7}, {7}, "dirty "}}, std::nullopt}));
    RecoveryStore store(fixture.state());
    auto snapshot = record(document);
    snapshot.path = path;
    snapshot.encoding = io::TextEncoding::utf16_le;
    snapshot.disk_version = version;
    store.checkpoint(snapshot);
    const auto loaded = store.load({7});
    REQUIRE(loaded);
    CHECK(loaded->document.text.str() == document.text().str());
    CHECK(loaded->document.revision == document.revision());
    CHECK(loaded->document.content_identity == document.content_identity());
    CHECK(loaded->document.selections == document.selections());
    CHECK(loaded->path == snapshot.path);
    CHECK(loaded->encoding == io::TextEncoding::utf16_le);
    REQUIRE(loaded->disk_version);
    CHECK(*loaded->disk_version->raw_content == *version.raw_content);
    CHECK(loaded->disk_version->path == path);
    CHECK(store.inspect().buffers == std::vector<BufferId>{{7}});
}

TEST_CASE("recovery sessions isolate instances and reject a second live claimant") {
    Fixture fixture;
    std::string abandoned;
    {
        RecoveryStore first(fixture.state());
        RecoveryStore second(fixture.state());
        CHECK(first.session_id() != second.session_id());
        first.checkpoint(record(Document({1}, "first")));
        second.checkpoint(record(Document({1}, "second")));
        CHECK_THROWS_AS((void)RecoveryStore(fixture.state(), first.session_id()), RecoveryLocked);
        const auto sessions = RecoveryStore::discover(fixture.state());
        REQUIRE(sessions.size() == 2);
        CHECK(sessions[0].active);
        CHECK(sessions[1].active);
        abandoned = first.session_id();
    }
    RecoveryStore claimed(fixture.state(), abandoned);
    CHECK(claimed.load({1})->document.text.str() == "first");
    const auto sessions = RecoveryStore::discover(fixture.state());
    CHECK(std::ranges::count_if(sessions, [](const auto& session) {
              return session.active;
          }) == 1);
}

TEST_CASE("recovery rejects stale worker checkpoints and permits selection changes at the same "
          "revision") {
    Fixture fixture;
    RecoveryStore store(fixture.state());
    Document document({4}, "old");
    const auto old = record(document);
    store.checkpoint(old);
    REQUIRE(document.replace_selections("new"));
    store.checkpoint(record(document));
    CHECK_THROWS_AS(store.checkpoint(old), StaleRevision);
    document.set_selections({{{0}, {2}}});
    store.checkpoint(record(document));
    CHECK(store.load({4})->document.selections == document.selections());
    auto forged = record(document);
    forged.document.text = Rope("different");
    CHECK_THROWS_AS(store.checkpoint(forged), std::invalid_argument);
    CHECK(store.load({4})->document.text.str() == document.text().str());
}

TEST_CASE(
    "recovery erase is revision guarded and late snapshots cannot resurrect discarded content") {
    Fixture fixture;
    RecoveryStore store(fixture.state());
    Document document({3}, "draft");
    const auto old = record(document);
    store.checkpoint(old);
    REQUIRE(document.replace_selections("next"));
    store.checkpoint(record(document));
    CHECK_THROWS_AS(store.erase({3}, old.document.revision), StaleRevision);
    store.erase({3}, document.revision());
    CHECK_FALSE(store.load({3}));
    CHECK(store.inspect().buffers.empty());
    CHECK_FALSE(fs::exists(store.session_directory() / "buffer-3.json"));
    CHECK_THROWS_AS(store.checkpoint(record(document)), StaleRevision);
    REQUIRE(document.replace_selections("new edit"));
    CHECK_NOTHROW(store.checkpoint(record(document)));
}

TEST_CASE(
    "recovery frame epochs allow the same revision to become dirty again and reject old frames") {
    Fixture fixture;
    Document document({3}, "unchanged text");
    auto first = record(document);
    first.checkpoint_sequence = 1;
    std::string session;
    {
        RecoveryStore store(fixture.state());
        session = store.session_id();
        store.checkpoint(first);
        store.erase({3}, document.revision());
        CHECK_FALSE(store.load({3}));
        document.invalidate_saved_state(); // Disk disappeared; no text revision changed.
        auto second = record(document);
        second.checkpoint_sequence = 2;
        CHECK_NOTHROW(store.checkpoint(second));
        CHECK(store.load({3})->checkpoint_sequence == 2);
        CHECK_THROWS_AS(store.checkpoint(first), StaleRevision);
        store.erase({3}, document.revision());
    }
    RecoveryStore claimed(fixture.state(), session);
    CHECK_FALSE(claimed.load({3}));
    CHECK_THROWS_AS(claimed.checkpoint(first), StaleRevision);
    auto newest = record(document);
    newest.checkpoint_sequence = 3;
    claimed.checkpoint(newest);
    CHECK(claimed.load({3})->document.text.str() == "unchanged text");
    CHECK(claimed.load({3})->checkpoint_sequence == 3);
    newest.document.selections = {{{0}, {1}}};
    CHECK_THROWS_AS(claimed.checkpoint(newest), std::invalid_argument);
}

TEST_CASE("recovery pruning removes abandoned empty sessions beyond the discovery limit") {
    Fixture fixture;
    for (std::size_t index = 0; index < 257; ++index) {
        RecoveryStore empty(fixture.state());
    }
    CHECK_THROWS_AS((void)RecoveryStore::discover(fixture.state()), std::length_error);
    const auto pruned = RecoveryStore::prune_empty_sessions(fixture.state());
    CHECK(pruned.removed == 257);
    CHECK(pruned.issues.empty());
    CHECK(RecoveryStore::discover(fixture.state()).empty());
    RecoveryStore next_start(fixture.state());
    CHECK(RecoveryStore::discover(fixture.state()).size() == 1);
}

TEST_CASE("recovery pruning preserves live owners dirty checkpoints and corrupt evidence") {
    Fixture fixture;
    RecoveryStore live(fixture.state());
    fs::path dirty_path, corrupt_path;
    {
        RecoveryStore dirty(fixture.state());
        dirty.checkpoint(record(Document({1}, "precious")));
        dirty_path = dirty.session_directory();
        RecoveryStore corrupt(fixture.state());
        corrupt_path = corrupt.session_directory();
        overwrite(corrupt_path / "buffer-1.json", "damaged evidence");
        RecoveryStore empty(fixture.state());
    }
    const auto pruned = RecoveryStore::prune_empty_sessions(fixture.state());
    CHECK(pruned.removed == 1);
    REQUIRE(pruned.issues.size() == 1);
    CHECK(fs::exists(live.session_directory() / ".lock"));
    CHECK(fs::exists(dirty_path / "buffer-1.json"));
    CHECK(io::read_file(corrupt_path / "buffer-1.json").text == "damaged evidence");
    CHECK_NOTHROW(live.checkpoint(record(Document({5}, "still owned"))));
}

TEST_CASE("recovery pruning removes valid durable epoch tombstones after their owner closes") {
    Fixture fixture;
    fs::path session;
    {
        RecoveryStore store(fixture.state());
        auto checkpoint = record(Document({1}, "discarded"));
        checkpoint.checkpoint_sequence = 42;
        store.checkpoint(checkpoint);
        store.erase({1}, checkpoint.document.revision);
        session = store.session_directory();
        CHECK(store.inspect().buffers.empty());
        CHECK(store.inspect().issues.empty());
        CHECK(RecoveryStore::prune_empty_sessions(fixture.state()).removed == 0);
    }
    const auto pruned = RecoveryStore::prune_empty_sessions(fixture.state());
    CHECK(pruned.removed == 1);
    CHECK(pruned.issues.empty());
    CHECK_FALSE(fs::exists(session));
}

TEST_CASE(
    "recovery persisted disk version refuses overwriting external file changes after reopening") {
    Fixture fixture;
    const auto path = fixture.directory / "source.txt";
    const auto disk = io::save_file(path, "original");
    std::string session;
    {
        RecoveryStore store(fixture.state());
        auto draft = record(Document({1}, "unsaved"));
        draft.path = path;
        draft.disk_version = disk;
        store.checkpoint(draft);
        session = store.session_id();
    }
    (void)io::save_file(path, "external edit", io::TextEncoding::utf8, disk);
    RecoveryStore restored(fixture.state(), session);
    const auto draft = restored.load({1});
    REQUIRE(draft);
    CHECK_THROWS_AS(
        (void)io::save_file(path, draft->document.text.str(), draft->encoding, draft->disk_version),
        io::FileConflict);
    CHECK(io::read_file(path).text == "external edit");
    CHECK(restored.load({1})->document.text.str() == "unsaved");
}

TEST_CASE(
    "recovery checksum and schema corruption are reported while good records remain recoverable") {
    Fixture fixture;
    RecoveryStore store(fixture.state());
    store.checkpoint(record(Document({1}, "keep")));
    store.checkpoint(record(Document({2}, "corrupt")));
    const auto path = store.session_directory() / "buffer-2.json";
    auto data = nlohmann::json::parse(io::read_file(path).text);
    data["payload"]["text"] = "tampered";
    overwrite(path, data.dump());
    const auto inspect = store.inspect();
    CHECK(inspect.buffers == std::vector<BufferId>{{1}});
    REQUIRE(inspect.issues.size() == 1);
    CHECK(inspect.issues[0].message.find("checksum") != std::string::npos);
    CHECK_THROWS_AS((void)store.load({2}), RecoveryCorrupt);
    CHECK_THROWS_AS(store.checkpoint(record(Document({2}, "overwrite forbidden"))),
                    RecoveryCorrupt);
    CHECK(fs::exists(path));
    data["schema"] = 99;
    overwrite(path, data.dump());
    CHECK_THROWS_AS((void)store.load({2}), RecoveryCorrupt);
    CHECK(store.load({1})->document.text.str() == "keep");
}

TEST_CASE(
    "recovery reports incomplete files and truncated or invalid UTF8 without deleting evidence") {
    Fixture fixture;
    RecoveryStore store(fixture.state());
    store.checkpoint(record(Document({1}, "safe")));
    overwrite(store.session_directory() / ".astra-interrupted.tmp", "partial");
    overwrite(store.session_directory() / "buffer-2.json", "{\"format\":");
    overwrite(store.session_directory() / "buffer-3.json", "\xff");
    const auto inspect = store.inspect();
    CHECK(inspect.buffers == std::vector<BufferId>{{1}});
    CHECK(inspect.issues.size() == 3);
    CHECK(fs::exists(store.session_directory() / ".astra-interrupted.tmp"));
    CHECK_THROWS_AS((void)store.load({2}), RecoveryCorrupt);
    CHECK_THROWS_AS((void)store.load({3}), RecoveryCorrupt);
}

TEST_CASE("recovery validation and resource limits leave the previous checkpoint intact") {
    Fixture fixture;
    RecoveryLimits limits;
    limits.max_text_bytes = 16;
    limits.max_records = 1;
    RecoveryStore store(fixture.state(), std::nullopt, limits);
    store.checkpoint(record(Document({1}, "safe")));
    CHECK_THROWS_AS(store.checkpoint(record(Document({2}, "other"))), std::length_error);
    CHECK_THROWS_AS(store.checkpoint(record(Document({1}, std::string(17, 'x')))),
                    std::length_error);
    auto invalid = record(Document({1}, "\xf0\x9f\x98\x80"));
    invalid.document.selections = {{{1}, {1}}};
    CHECK_THROWS_AS(store.checkpoint(invalid), std::invalid_argument);
    invalid = record(Document({1}, "safe"));
    invalid.document.selections.clear();
    CHECK_THROWS_AS(store.checkpoint(invalid), std::invalid_argument);
    CHECK(store.load({1})->document.text.str() == "safe");
    CHECK_THROWS_AS((void)RecoveryStore(fixture.state(), "../escape"), std::invalid_argument);
    CHECK_THROWS_AS((void)RecoveryStore(fs::path{}), std::invalid_argument);
}

TEST_CASE("recovery stores empty untitled text and explicit Latin1 disk bytes") {
    Fixture fixture;
    RecoveryStore store(fixture.state());
    store.checkpoint(record(Document({0}, "")));
    CHECK(store.load({0})->document.text.empty());
    const auto path = fixture.directory / "latin.txt";
    const auto disk = io::save_file(path, "caf\xc3\xa9", io::TextEncoding::latin1);
    auto draft = record(Document({9}, "caf\xc3\xa9 modified"));
    draft.path = path;
    draft.encoding = io::TextEncoding::latin1;
    draft.disk_version = disk;
    store.checkpoint(draft);
    CHECK(*store.load({9})->disk_version->raw_content == "caf\xe9");
    CHECK(store.load({9})->encoding == io::TextEncoding::latin1);
}

#ifdef _WIN32
TEST_CASE("recovery atomic checkpoint failure preserves the prior record") {
    Fixture fixture;
    RecoveryStore store(fixture.state());
    Document document({1}, "old");
    store.checkpoint(record(document));
    const auto path = store.session_directory() / "buffer-1.json";
    REQUIRE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE);
    REQUIRE(document.replace_selections("new"));
    CHECK_THROWS_AS(store.checkpoint(record(document)), fs::filesystem_error);
    CHECK(store.load({1})->document.text.str() == "old");
    CHECK(store.inspect().issues.empty());
    REQUIRE(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != FALSE);
    CHECK_NOTHROW(store.checkpoint(record(document)));
}

TEST_CASE("recovery survives abrupt Windows process termination and releases its session lock") {
    Fixture fixture;
    std::wstring executable(32768, L'\0');
    const auto size =
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    REQUIRE(size > 0);
    REQUIRE(size < executable.size());
    executable.resize(size);
    const auto child_path =
        (fs::path(executable).parent_path() / "astra-recovery-fixture.exe").u8string();
    const auto state_path = fixture.state().u8string();
    process::ProcessOptions options;
    options.argv = {
        std::string(reinterpret_cast<const char*>(child_path.data()), child_path.size()),
        std::string(reinterpret_cast<const char*>(state_path.data()), state_path.size())};
    options.shutdown_timeout = 200ms;
    std::promise<std::string> ready;
    auto confirmed = ready.get_future();
    std::string line;
    bool notified = false;
    process::ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) {
        line += bytes;
        const auto newline = line.find('\n');
        if (!notified && newline != line.npos) {
            notified = true;
            ready.set_value(line.substr(0, newline));
        }
    };
    process::Process child(std::move(options), std::move(callbacks));
    child.start().get();
    REQUIRE(confirmed.wait_for(5s) == std::future_status::ready);
    const auto session = confirmed.get();
    CHECK_THROWS_AS((void)RecoveryStore(fixture.state(), session), RecoveryLocked);
    child.terminate(); // No RecoveryStore destructor or C++ unwinding in child.
    REQUIRE(child.completion().wait_for(5s) == std::future_status::ready);
    CHECK(child.completion().get().terminated);
    const auto abandoned = RecoveryStore::discover(fixture.state());
    REQUIRE(abandoned.size() == 1);
    CHECK_FALSE(abandoned[0].active);
    REQUIRE(abandoned[0].issues.empty());
    CHECK(abandoned[0].buffers == std::vector<BufferId>{{42}});
    RecoveryStore recovered(fixture.state(), session);
    const auto draft = recovered.load({42});
    REQUIRE(draft);
    CHECK(draft->document.text.str() == "unsaved \xf0\x9f\x98\x80\r\n");
    CHECK(draft->document.selections == std::vector<Selection>{{{8}, {12}}});
}
#endif
