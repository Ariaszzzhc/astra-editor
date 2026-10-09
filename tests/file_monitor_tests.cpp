#include <astra/watch/file_monitor.hpp>
#include <astra/io/file_store.hpp>
#include "support/temporary_directory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

namespace fs = std::filesystem;
using astra::watch::EventBatch;
using astra::watch::FileEvent;
using astra::watch::FileMonitor;
using astra::watch::MonitorOptions;
using namespace std::chrono_literals;

class WatchFiles {
public:
    WatchFiles() {
        const auto workspace = fs::canonical(fs::current_path());
        build_ = fs::weakly_canonical(workspace / "build");
        if (build_.parent_path() != workspace) { throw std::runtime_error("watch fixture build path escaped workspace"); }
        (void)fs::create_directories(build_);
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = build_ / ("watch-tests-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(candidate)) {
                base_ = fs::canonical(candidate);
                root_ = base_ / "workspace";
                (void)fs::create_directory(root_);
                return;
            }
        }
        throw std::runtime_error("unable to create unique watch fixture");
    }
    ~WatchFiles() {
        for (const auto& issue : astra::test::cleanup_owned_directory(base_, build_, "watch-tests-")) {
            INFO(issue.path.string());
            INFO(issue.message);
            CHECK(false);
        }
    }
    [[nodiscard]] const fs::path& root() const { return root_; }
    [[nodiscard]] const fs::path& base() const { return base_; }
    void write(const fs::path& relative, std::string_view text = "changed") const {
        const auto target = root_ / relative;
        (void)fs::create_directories(target.parent_path());
        std::ofstream stream(target, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.close();
    }
private:
    fs::path build_, base_, root_;
};

void merge(EventBatch& into, EventBatch batch) {
    into.generation = batch.generation;
    into.rescan_required |= batch.rescan_required;
    if (batch.error) { into.error = std::move(batch.error); }
    for (auto& item : batch.events) {
        const auto found = std::ranges::find(into.events, item.relative_path, &FileEvent::relative_path);
        if (found == into.events.end()) { into.events.push_back(std::move(item)); }
        else { found->content_changed |= item.content_changed; found->renamed |= item.renamed; }
    }
}

template<class Predicate>
EventBatch wait_event(FileMonitor& monitor, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    EventBatch result;
    do {
        merge(result, monitor.take_events());
        if (predicate(result)) { return result; }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("file monitor notification timed out");
    return result;
}

bool contains(const EventBatch& batch, const fs::path& relative) {
    return std::ranges::find(batch.events, relative, &FileEvent::relative_path) != batch.events.end();
}

std::uint64_t started(FileMonitor& monitor, std::vector<fs::path> paths) {
    auto ready = monitor.start(std::move(paths));
    REQUIRE(ready.wait_for(5s) == std::future_status::ready);
    const auto generation = ready.get();
    const auto initial = monitor.take_events();
    CHECK(initial.generation == generation);
    CHECK(initial.rescan_required);
    CHECK_FALSE(initial.error);
    return generation;
}

void stopped(FileMonitor& monitor) {
    auto done = monitor.stop();
    REQUIRE(done.wait_for(5s) == std::future_status::ready);
    CHECK_NOTHROW(done.get());
}

} // namespace

TEST_CASE("file monitor reports real writes only for subscribed workspace files") {
    WatchFiles files;
    files.write("wanted.txt", "original");
    files.write("ignored.txt", "original");
    FileMonitor monitor(files.root());
    const auto generation = started(monitor, {"wanted.txt"});
    files.write("ignored.txt");
    files.write("wanted.txt");
    const auto event = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "wanted.txt"); });
    CHECK(event.generation == generation);
    CHECK(event.events.size() == 1);
    CHECK(event.events.front().content_changed);
    CHECK_FALSE(event.error);
    stopped(monitor);
}

TEST_CASE("file monitor survives durable atomic file replacement and repeated saves") {
    WatchFiles files;
    files.write("source.txt", "original");
    auto version = astra::io::read_file(files.root() / "source.txt").version;
    FileMonitor monitor(files.root());
    (void)started(monitor, {"source.txt"});
    for (int index = 0; index < 3; ++index) {
        version = astra::io::save_file(files.root() / "source.txt", "replacement " + std::to_string(index),
            astra::io::TextEncoding::utf8, version);
        const auto event = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "source.txt"); });
        CHECK(event.events.front().renamed);
        CHECK_FALSE(event.error);
    }
    stopped(monitor);
}

TEST_CASE("file monitor reports rename deletion and recreation of subscribed names") {
    WatchFiles files;
    files.write("before.txt");
    FileMonitor monitor(files.root());
    (void)started(monitor, {"before.txt", "after.txt"});
    fs::rename(files.root() / "before.txt", files.root() / "after.txt");
    const auto renamed = wait_event(monitor, [](const EventBatch& batch) {
        return contains(batch, "before.txt") && contains(batch, "after.txt");
    });
    CHECK(std::ranges::all_of(renamed.events, [](const auto& event) { return event.renamed; }));
    CHECK(fs::remove(files.root() / "after.txt"));
    const auto deleted = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "after.txt"); });
    CHECK(deleted.events.front().renamed);
    files.write("before.txt", "recreated");
    const auto recreated = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "before.txt"); });
    CHECK(recreated.events.front().renamed);
    stopped(monitor);
}

TEST_CASE("file monitor subscription replacement changes generation and drops removed paths") {
    WatchFiles files;
    files.write("old/a.txt");
    files.write("new/b.txt");
    FileMonitor monitor(files.root());
    const auto first = started(monitor, {"old/a.txt"});
    auto update = monitor.replace_paths({"new/b.txt"});
    REQUIRE(update.wait_for(5s) == std::future_status::ready);
    const auto second = update.get();
    CHECK(second > first);
    const auto handover = monitor.take_events();
    CHECK(handover.generation == second);
    CHECK(handover.rescan_required);
    files.write("old/a.txt");
    files.write("new/b.txt");
    const auto changed = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "new/b.txt"); });
    CHECK(changed.generation == second);
    CHECK_FALSE(contains(changed, "old/a.txt"));
    stopped(monitor);
}

TEST_CASE("file monitor rejected subscription replacement preserves previous watchers") {
    WatchFiles files;
    files.write("kept.txt");
    FileMonitor monitor(files.root());
    const auto generation = started(monitor, {"kept.txt"});
    CHECK_THROWS_AS((void)monitor.replace_paths({"../outside.txt"}).get(), std::invalid_argument);
    CHECK_THROWS_AS((void)monitor.replace_paths({files.root() / "kept.txt"}).get(), std::invalid_argument);
    CHECK_THROWS_AS((void)monitor.replace_paths({"."}).get(), std::invalid_argument);
    files.write("kept.txt");
    const auto event = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "kept.txt"); });
    CHECK(event.generation == generation);
    CHECK_FALSE(event.error);
    stopped(monitor);
}

TEST_CASE("file monitor missing nested parents are discovered by ancestor watches") {
    WatchFiles files;
    FileMonitor monitor(files.root());
    (void)started(monitor, {"future/deep/code.txt"});
    files.write("future/deep/code.txt", "new file");
    const auto creation = wait_event(monitor, [](const EventBatch& batch) { return batch.rescan_required; });
    CHECK_FALSE(creation.error);
    // An explicit barrier makes this check independent of OS event coalescing.
    (void)monitor.replace_paths({"future/deep/code.txt"}).get();
    (void)monitor.take_events();
    files.write("future/deep/code.txt", "later edit");
    const auto write = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "future/deep/code.txt"); });
    CHECK(write.events.front().content_changed);
    stopped(monitor);
}

TEST_CASE("file monitor parent rename causes rescan and watches a recreated original directory") {
    WatchFiles files;
    files.write("dir/code.txt", "original");
    FileMonitor monitor(files.root());
    (void)started(monitor, {"dir/code.txt"});
    fs::rename(files.root() / "dir", files.root() / "moved");
    (void)wait_event(monitor, [](const EventBatch& batch) { return batch.rescan_required; });
    files.write("dir/code.txt", "recreated");
    (void)monitor.replace_paths({"dir/code.txt"}).get();
    (void)monitor.take_events();
    files.write("dir/code.txt", "updated");
    const auto result = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "dir/code.txt"); });
    CHECK(result.events.front().content_changed);
    stopped(monitor);
}

TEST_CASE("file monitor burst overflow remains bounded and explicitly requires rescan") {
    WatchFiles files;
    MonitorOptions options;
    options.max_events = 1;
    FileMonitor monitor(files.root(), options);
    std::vector<fs::path> paths;
    for (int index = 0; index < 20; ++index) {
        paths.emplace_back("file-" + std::to_string(index) + ".txt");
        files.write(paths.back(), "old");
    }
    (void)started(monitor, paths);
    for (const auto& path : paths) { files.write(path, "burst"); }
    const auto result = wait_event(monitor, [](const EventBatch& batch) { return batch.rescan_required; });
    CHECK(result.rescan_required);
    // The current take is bounded; even a slow consumer knows a full scan is due.
    CHECK(monitor.take_events().events.size() <= options.max_events);
    CHECK_FALSE(result.error);
    stopped(monitor);
}

TEST_CASE("file monitor repeated events for one file are coalesced without overflow") {
    WatchFiles files;
    files.write("same.txt");
    MonitorOptions options;
    options.max_events = 1;
    FileMonitor monitor(files.root(), options);
    (void)started(monitor, {"same.txt"});
    for (int index = 0; index < 30; ++index) { files.write("same.txt", std::to_string(index)); }
    const auto result = wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "same.txt"); });
    CHECK(result.events.size() == 1);
    CHECK_FALSE(result.rescan_required);
    stopped(monitor);
}

TEST_CASE("file monitor stop drains handles and supports restarting the same object") {
    WatchFiles files;
    files.write("child/code.txt");
    FileMonitor monitor(files.root());
    CHECK_THROWS_AS((void)monitor.replace_paths({"child/code.txt"}).get(), std::logic_error);
    const auto first = started(monitor, {"child/code.txt"});
    CHECK_THROWS_AS((void)monitor.start().get(), std::logic_error);
    stopped(monitor);
    CHECK_NOTHROW(monitor.stop().get());
    // Completion promises real handle release, not a delayed teardown retry.
    fs::rename(files.root() / "child", files.root() / "renamed");
    CHECK(fs::remove(files.root() / "renamed/code.txt"));
    CHECK(fs::remove(files.root() / "renamed"));
    const auto second = started(monitor, {"again.txt"});
    CHECK(second > first);
    files.write("again.txt");
    (void)wait_event(monitor, [](const EventBatch& batch) { return contains(batch, "again.txt"); });
    stopped(monitor);
}

TEST_CASE("file monitor pending subscription futures settle during immediate shutdown") {
    WatchFiles files;
    for (int iteration = 0; iteration < 10; ++iteration) {
        FileMonitor monitor(files.root());
        auto initial = monitor.start({"a.txt"});
        std::vector<std::future<std::uint64_t>> updates;
        for (int index = 0; index < 40; ++index) { updates.push_back(monitor.replace_paths({"b.txt"})); }
        stopped(monitor);
        REQUIRE(initial.wait_for(0s) == std::future_status::ready);
        std::size_t settled = 0;
        try { (void)initial.get(); ++settled; } catch (const std::runtime_error&) { ++settled; }
        for (auto& update : updates) {
            REQUIRE(update.wait_for(0s) == std::future_status::ready);
            try { (void)update.get(); ++settled; }
            catch (const std::runtime_error&) { ++settled; }
            catch (const std::logic_error&) { ++settled; }
        }
        CHECK(settled == updates.size() + 1);
    }
}

TEST_CASE("file monitor validates bounds and recovers after failed startup") {
    WatchFiles files;
    MonitorOptions options;
    options.max_paths = 1;
    options.max_queued_path_bytes = 32;
    FileMonitor monitor(files.root(), options);
    CHECK_THROWS_AS((void)monitor.start({"a", "b"}).get(), std::length_error);
    CHECK_THROWS_AS((void)monitor.start({std::string(100, 'a')}).get(), std::length_error);
    CHECK_THROWS_AS((void)monitor.start({"../outside"}).get(), std::invalid_argument);
    stopped(monitor);
    (void)started(monitor, {"valid.txt"});
    CHECK_THROWS_AS((void)monitor.replace_paths({"a", "b"}).get(), std::length_error);
    stopped(monitor);
}

#ifdef _WIN32
TEST_CASE("Windows file monitor matches Unicode names and ordinal case insensitive paths") {
    WatchFiles files;
    const fs::path actual = fs::path(u8"\u661f\u7a7a") / fs::path(u8"\u00c4-code-\U0001f600.TXT");
    const fs::path subscribed = fs::path(u8"\u661f\u7a7a") / fs::path(u8"\u00e4-CODE-\U0001f600.txt");
    files.write(actual, "original");
    FileMonitor monitor(files.root());
    (void)started(monitor, {subscribed});
    files.write(actual, "unicode change");
    const auto result = wait_event(monitor, [&](const EventBatch& batch) { return contains(batch, subscribed); });
    CHECK(result.events.front().relative_path == subscribed);
    CHECK_FALSE(result.error);
    stopped(monitor);
}

TEST_CASE("Windows file monitor rejects a junction escaping its explicit workspace") {
    WatchFiles files;
    const auto outside = files.base() / "outside";
    (void)fs::create_directory(outside);
    const auto link = files.root() / "escape";
    // A relative symlink can be unavailable without developer mode; use a real
    // directory junction via the same native reparse protocol as workspace tests.
    (void)fs::create_directory(link);
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    const auto target = L"\\??\\" + outside.native();
    struct Reparse {
        DWORD tag;
        WORD data_length;
        WORD reserved;
        WORD substitute_offset;
        WORD substitute_length;
        WORD print_offset;
        WORD print_length;
        wchar_t names[32768];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substitute_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.print_offset = static_cast<WORD>(data.substitute_length + sizeof(wchar_t));
    data.print_length = static_cast<WORD>(outside.native().size() * sizeof(wchar_t));
    std::copy(target.begin(), target.end(), data.names);
    std::copy(outside.native().begin(), outside.native().end(), data.names + target.size() + 1);
    data.data_length = static_cast<WORD>(8 + data.substitute_length + sizeof(wchar_t) + data.print_length + sizeof(wchar_t));
    DWORD returned = 0;
    const auto created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        static_cast<DWORD>(data.data_length) + 8, nullptr, 0, &returned, nullptr);
    const auto error = GetLastError();
    CHECK(CloseHandle(handle));
    INFO(error);
    REQUIRE(created);
    {
        FileMonitor monitor(files.root());
        CHECK_THROWS_AS((void)monitor.start({"escape/secret.txt"}).get(), std::invalid_argument);
        stopped(monitor);
    }
    // Remove only the owned link so the fixture helper never traverses a junction.
    CHECK(RemoveDirectoryW(link.c_str()));
    CHECK(fs::exists(outside));
}
#endif
