#include <astra/service/application_service.hpp>
#include <astra/service/service_host.hpp>
#include <astra/io/file_store.hpp>
#include "support/temporary_directory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using astra::protocol::Json;
using astra::service::ApplicationService;
using astra::service::ServiceHost;
using namespace std::chrono_literals;

#ifdef _WIN32
class WatchReadHandle {
public:
    explicit WatchReadHandle(const fs::path& path)
        : handle_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr)) {
        if (handle_ == INVALID_HANDLE_VALUE) { throw std::runtime_error("cannot hold watch test read handle"); }
    }
    ~WatchReadHandle() {
        if (!CloseHandle(handle_)) { std::fputs("failed to close watch test read handle\n", stderr); }
    }
    WatchReadHandle(const WatchReadHandle&) = delete;
    WatchReadHandle& operator=(const WatchReadHandle&) = delete;
    [[nodiscard]] std::string read() const {
        std::array<char, 4096> bytes{};
        DWORD count{};
        if (!ReadFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr)) {
            throw std::runtime_error("cannot read held watch test file");
        }
        return {bytes.data(), count};
    }
private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};
#endif

struct WatchWorkspace {
    fs::path build;
    fs::path root;
    WatchWorkspace() {
        const auto repository = fs::canonical(fs::current_path());
        build = fs::weakly_canonical(repository / "build");
        if (build.parent_path() != repository) { throw std::runtime_error("application watch fixture escaped repository"); }
        (void)fs::create_directories(build);
        for (int attempt = 0; attempt < 32; ++attempt) {
            auto path = build / ("watch-app-" + std::to_string(std::random_device{}()) + "-" + std::to_string(std::random_device{}()));
            if (fs::create_directory(path)) { root = fs::canonical(path); return; }
        }
        throw std::runtime_error("application watch fixture allocation failed");
    }
    ~WatchWorkspace() {
        for (const auto& issue : astra::test::cleanup_owned_directory(root, build, "watch-app-")) {
            CHECK_MESSAGE(false, issue.path.string(), ": ", issue.message);
        }
    }
    void write(const fs::path& path, std::string_view text) const {
        (void)fs::create_directories((root / path).parent_path());
        std::ofstream stream(root / path, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.close();
    }
    std::unique_ptr<ServiceHost> host(bool enabled = true) const {
        astra::service::ServiceHostOptions options;
        options.workspace = root;
        options.watch_files = enabled;
        return std::make_unique<ServiceHost>(std::move(options));
    }
};

Json request(std::string method, Json params = Json::object()) {
    return {{"jsonrpc", "2.0"}, {"id", "watch-test"}, {"method", std::move(method)}, {"params", std::move(params)}};
}
Json result(const Json& response) {
    INFO(response.dump());
    REQUIRE_FALSE(response.contains("error"));
    return response.at("result");
}
Json response(ServiceHost& host, std::string method, Json params = Json::object()) {
    auto future = host.submit(request(std::move(method), std::move(params)).dump());
    REQUIRE(future.wait_for(10s) == std::future_status::ready);
    const auto body = future.get();
    REQUIRE(body);
    return Json::parse(*body);
}
Json call(ServiceHost& host, std::string method, Json params = Json::object()) {
    return result(response(host, std::move(method), std::move(params)));
}
Json operation(ServiceHost& host, const Json& started) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    do {
        const auto value = call(host, "operation.get", {{"id", started.at("operationId")}});
        if (value.at("state") != "pending") { return value; }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("application watch operation did not settle");
    return {};
}
Json done(ServiceHost& host, std::string method, Json params = Json::object()) {
    const auto value = operation(host, call(host, std::move(method), std::move(params)));
    INFO(value.dump());
    REQUIRE(value.at("state") == "completed");
    return value.at("result");
}
void watching(ServiceHost& host) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        const auto state = call(host, "workspace.watchStatus");
        INFO(state.dump());
        REQUIRE_FALSE(state.contains("error"));
        if (state.at("ready").get<bool>() && !state.at("pending").get<bool>()) {
            (void)host.take_notifications();
            return;
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("application watch subscriptions did not become ready");
}
Json notification(ServiceHost& host, std::string_view status) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        // Reading the local queue does not submit any RPC or wake the service;
        // the service's idle notification timer must perform the disk check.
        for (const auto& text : host.take_notifications()) {
            const auto value = Json::parse(text);
            if (value.at("method") == "workspace.fileChanged" && value.at("params").at("status").get<std::string>() == status) {
                return value.at("params");
            }
        }
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
    FAIL("idle file change notification did not arrive");
    return {};
}

class PausedWatch {
public:
    explicit PausedWatch(const fs::path& root, bool enabled = false)
        : app(root, [this](auto worker, auto complete) {
            auto task = std::make_unique<Task>();
            task->worker = std::move(worker);
            task->complete = std::move(complete);
            auto future = task->promise.get_future();
            tasks.push_back(std::move(task));
            return future;
        }, {}, {}, {}, {}, enabled) {}
    ~PausedWatch() {
        // Honor the production scheduler contract even if a test assertion exits.
        while (!tasks.empty()) { worker(); complete(); }
    }
    Json reply(std::string method, Json params = Json::object()) {
        const auto value = app.dispatch(request(std::move(method), std::move(params)));
        REQUIRE(value);
        return *value;
    }
    Json call(std::string method, Json params = Json::object()) { return result(reply(std::move(method), std::move(params))); }
    void worker() {
        REQUIRE_FALSE(tasks.empty());
        if (tasks.front()->ran) { return; }
        tasks.front()->ran = true;
        try { tasks.front()->worker(); } catch (...) { tasks.front()->error = std::current_exception(); }
    }
    void complete() {
        REQUIRE_FALSE(tasks.empty());
        auto task = std::move(tasks.front());
        tasks.pop_front();
        try { task->complete(task->error); } catch (...) { task->error = std::current_exception(); }
        if (task->error) { task->promise.set_exception(task->error); } else { task->promise.set_value(); }
    }
    Json done(std::string method, Json params = Json::object()) {
        const auto started = call(std::move(method), std::move(params));
        worker(); complete();
        const auto value = call("operation.get", {{"id", started.at("operationId")}});
        INFO(value.dump());
        REQUIRE(value.at("state") == "completed");
        return value.at("result");
    }
    void settled() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            (void)app.take_notifications();
            while (!tasks.empty()) { worker(); complete(); }
            const auto status = call("workspace.watchStatus");
            if (status.at("ready").get<bool>() && !status.at("pending").get<bool>()) { return; }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL("paused watch did not settle");
    }
    void wait_task() {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        do {
            (void)app.take_notifications();
            if (!tasks.empty()) { return; }
            std::this_thread::sleep_for(5ms);
        } while (std::chrono::steady_clock::now() < deadline);
        FAIL("paused watch did not schedule a disk check");
    }
    ApplicationService app;
private:
    struct Task {
        std::function<void()> worker;
        std::function<void(std::exception_ptr)> complete;
        std::promise<void> promise;
        std::exception_ptr error;
        bool ran{};
    };
    std::deque<std::unique_ptr<Task>> tasks;
};

} // namespace

TEST_CASE("application file watch reloads clean documents without client traffic and preserves undo") {
    WatchWorkspace files;
    files.write("source.txt", "before\n");
    auto host = files.host();
    const auto opened = done(*host, "workspace.open", {{"path", "source.txt"}});
    watching(*host);
    files.write("source.txt", "after \xf0\x9f\x98\x80\n");
    const auto event = notification(*host, "reloaded");
    CHECK(event.at("id") == opened.at("id"));
    const auto loaded = call(*host, "document.get", {{"id", opened.at("id")}});
    CHECK(loaded.at("text") == "after \xf0\x9f\x98\x80\n");
    CHECK_FALSE(loaded.at("dirty").get<bool>());
    CHECK(loaded.at("revision") != opened.at("revision"));
    const auto undo = call(*host, "document.undo", {{"id", opened.at("id")}, {"baseRevision", loaded.at("revision")}});
    CHECK(undo.at("dirty").get<bool>());
    CHECK(call(*host, "document.get", {{"id", opened.at("id")}}).at("text") == "before\n");
}

TEST_CASE("application file watch preserves dirty text old disk version and dirty undo after external change") {
    WatchWorkspace files;
    files.write("dirty.txt", "original");
    auto host = files.host();
    auto document = done(*host, "workspace.open", {{"path", "dirty.txt"}});
    watching(*host);
    document = call(*host, "document.insert", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}, {"text", "local "}});
    files.write("dirty.txt", "external");
    (void)notification(*host, "conflict");
    CHECK(call(*host, "document.get", {{"id", document.at("id")}}).at("text") == "local original");
    const auto failed = operation(*host, call(*host, "workspace.save", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}}));
    CHECK(failed.at("state") == "failed");
    CHECK(failed.at("error").at("code") == -32011);
    CHECK(astra::io::read_file(files.root / "dirty.txt").text == "external");
    document = call(*host, "document.undo", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}});
    CHECK(document.at("dirty").get<bool>());
    CHECK(call(*host, "document.get", {{"id", document.at("id")}}).at("text") == "original");
}

TEST_CASE("application file watch keeps deleted clean content recoverable and rejects unconfirmed close") {
    WatchWorkspace files;
    files.write("deleted.txt", "must survive");
    auto host = files.host();
    const auto opened = done(*host, "workspace.open", {{"path", "deleted.txt"}});
    watching(*host);
    REQUIRE(fs::remove(files.root / "deleted.txt"));
    (void)notification(*host, "deleted");
    const auto document = call(*host, "document.get", {{"id", opened.at("id")}});
    CHECK(document.at("text") == "must survive");
    CHECK(document.at("dirty").get<bool>());
    CHECK(response(*host, "document.close", {{"id", opened.at("id")}, {"baseRevision", document.at("revision")}}).at("error").at("code") == -32003);
    files.write("deleted.txt", "new disk file");
    (void)notification(*host, "conflict");
    const auto reloaded = done(*host, "workspace.reload", {{"id", opened.at("id")}, {"baseRevision", document.at("revision")}, {"discardChanges", true}});
    CHECK_FALSE(reloaded.at("dirty").get<bool>());
    CHECK(call(*host, "document.get", {{"id", opened.at("id")}}).at("text") == "new disk file");
}

TEST_CASE("application file watch follows atomic replacement Unicode paths and CR-only editor text") {
    WatchWorkspace files;
    const fs::path path(u8"\u661f-code.txt");
    files.write(path, "one\rtwo\r");
    auto version = astra::io::read_file(files.root / path).version;
    auto host = files.host();
    const auto opened = done(*host, "workspace.open", {{"path", "\xe6\x98\x9f-code.txt"}});
    watching(*host);
#ifdef _WIN32
    const WatchReadHandle held(files.root / path);
#endif
    version = astra::io::save_file(files.root / path, "changed\rnext\r", astra::io::TextEncoding::utf16_le, version);
#ifdef _WIN32
    CHECK(held.read() == "one\rtwo\r");
#endif
    (void)notification(*host, "reloaded");
    auto document = call(*host, "document.get", {{"id", opened.at("id")}});
    CHECK(document.at("text") == "changed\nnext\n");
    CHECK(document.at("lineCount") == 3);
    document = call(*host, "document.insert", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}, {"text", "edit "}});
    (void)done(*host, "workspace.save", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}});
    const auto saved = astra::io::read_file(files.root / path);
    CHECK(saved.encoding == astra::io::TextEncoding::utf16_le);
    CHECK(saved.text.find('\n') == std::string::npos);
    CHECK(saved.text.find('\r') != std::string::npos);
}

TEST_CASE("application file watch invalid encoding preserves buffer and explicit reload failure is safe") {
    WatchWorkspace files;
    files.write("invalid.txt", "safe text");
    auto host = files.host();
    const auto opened = done(*host, "workspace.open", {{"path", "invalid.txt"}});
    watching(*host);
    files.write("invalid.txt", std::string("\xff\xfe\0", 3));
    (void)notification(*host, "error");
    const auto document = call(*host, "document.get", {{"id", opened.at("id")}});
    CHECK(document.at("text") == "safe text");
    CHECK(document.at("dirty").get<bool>());
    const auto failed = operation(*host, call(*host, "workspace.reload", {{"id", document.at("id")},
        {"baseRevision", document.at("revision")}, {"discardChanges", true}}));
    CHECK(failed.at("state") == "failed");
    CHECK(call(*host, "document.get", {{"id", document.at("id")}}).at("text") == "safe text");
}

TEST_CASE("application explicit reload requires current revision and dirty discard authorization") {
    WatchWorkspace files;
    files.write("source.txt", "disk");
    auto host = files.host(false);
    auto document = done(*host, "workspace.open", {{"path", "source.txt"}});
    document = call(*host, "document.insert", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}, {"text", "local "}});
    CHECK(response(*host, "workspace.reload", {{"id", document.at("id")}, {"baseRevision", "0"}, {"discardChanges", true}}).at("error").at("code") == -32001);
    CHECK(response(*host, "workspace.reload", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}}).at("error").at("code") == -32003);
    CHECK(response(*host, "workspace.reload", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}, {"discardChanges", "yes"}}).contains("error"));
    const auto reloaded = done(*host, "workspace.reload", {{"id", document.at("id")}, {"baseRevision", document.at("revision")}, {"discardChanges", true}});
    CHECK_FALSE(reloaded.at("dirty").get<bool>());
    CHECK(call(*host, "document.get", {{"id", document.at("id")}}).at("text") == "disk");
    const auto undo = call(*host, "document.undo", {{"id", document.at("id")}, {"baseRevision", reloaded.at("revision")}});
    CHECK(undo.at("dirty").get<bool>());
    CHECK(call(*host, "document.get", {{"id", document.at("id")}}).at("text") == "local disk");
}

TEST_CASE("application explicit reload cannot overwrite edits made while its disk worker was running") {
    WatchWorkspace files;
    files.write("source.txt", "original");
    PausedWatch host(files.root);
    const auto opened = host.done("workspace.open", {{"path", "source.txt"}});
    files.write("source.txt", "external");
    const auto reload = host.call("workspace.reload", {{"id", opened.at("id")}, {"baseRevision", "0"}});
    host.worker();
    const auto changed = host.call("document.insert", {{"id", opened.at("id")}, {"baseRevision", "0"}, {"text", "local "}});
    host.complete();
    const auto completed = host.call("operation.get", {{"id", reload.at("operationId")}});
    CHECK(completed.at("state") == "failed");
    CHECK(completed.at("error").at("code") == -32001);
    const auto current = host.call("document.get", {{"id", opened.at("id")}});
    CHECK(current.at("text") == "local original");
    CHECK(current.at("revision") == changed.at("revision"));
}

TEST_CASE("application explicit reload cannot restore a document closed while reading") {
    WatchWorkspace files;
    files.write("source.txt", "original");
    PausedWatch host(files.root);
    const auto opened = host.done("workspace.open", {{"path", "source.txt"}});
    files.write("source.txt", "external");
    const auto reload = host.call("workspace.reload", {{"id", opened.at("id")}, {"baseRevision", "0"}});
    host.worker();
    (void)host.call("document.close", {{"id", opened.at("id")}, {"baseRevision", "0"}});
    host.complete();
    CHECK(host.call("operation.get", {{"id", reload.at("operationId")}}).at("state") == "failed");
    CHECK(host.call("workspace.documents").empty());
}

TEST_CASE("application automatic reload keeps concurrent input and invalidates the old clean history") {
    WatchWorkspace files;
    files.write("source.txt", "original");
    PausedWatch host(files.root, true);
    const auto opened = host.done("workspace.open", {{"path", "source.txt"}});
    host.settled();
    files.write("source.txt", "external");
    host.wait_task();
    host.worker();
    const auto edit = host.call("document.insert", {{"id", opened.at("id")}, {"baseRevision", "0"}, {"text", "local "}});
    host.complete();
    CHECK(host.call("document.get", {{"id", opened.at("id")}}).at("text") == "local original");
    const auto undone = host.call("document.undo", {{"id", opened.at("id")}, {"baseRevision", edit.at("revision")}});
    CHECK(undone.at("dirty").get<bool>());
    const auto notifications = host.app.take_notifications();
    CHECK(std::ranges::any_of(notifications, [](const auto& text) {
        const auto value = Json::parse(text);
        return value.at("method") == "workspace.fileChanged" && value.at("params").at("status") == "conflict";
    }));
}

TEST_CASE("application file watch defers to in-flight saves and own atomic save stays clean") {
    WatchWorkspace files;
    files.write("source.txt", "original");
    PausedWatch host(files.root, true);
    const auto opened = host.done("workspace.open", {{"path", "source.txt"}});
    host.settled();
    const auto dirty = host.call("document.insert", {{"id", opened.at("id")}, {"baseRevision", "0"}, {"text", "saved "}});
    const auto save = host.call("workspace.save", {{"id", opened.at("id")}, {"baseRevision", dirty.at("revision")}});
    host.worker();
    (void)host.app.take_notifications();
    host.complete();
    CHECK(host.call("operation.get", {{"id", save.at("operationId")}}).at("state") == "completed");
    host.settled();
    const auto current = host.call("document.get", {{"id", opened.at("id")}});
    CHECK(current.at("text") == "saved original");
    CHECK_FALSE(current.at("dirty").get<bool>());
    CHECK(current.at("revision") == dirty.at("revision"));
}

TEST_CASE("application monitor ignores closed paths and restarts with a new service host") {
    WatchWorkspace files;
    files.write("closed.txt", "first");
    {
        auto host = files.host();
        const auto opened = done(*host, "workspace.open", {{"path", "closed.txt"}});
        watching(*host);
        (void)call(*host, "document.close", {{"id", opened.at("id")}, {"baseRevision", opened.at("revision")}});
        watching(*host);
        files.write("closed.txt", "second");
        host->shutdown();
        CHECK(host->stats().handler_errors == 0);
    }
    auto host = files.host();
    const auto reopened = done(*host, "workspace.open", {{"path", "closed.txt"}});
    CHECK(call(*host, "document.get", {{"id", reopened.at("id")}}).at("text") == "second");
    watching(*host);
    files.write("closed.txt", "third");
    (void)notification(*host, "reloaded");
    CHECK(call(*host, "document.get", {{"id", reopened.at("id")}}).at("text") == "third");
}

TEST_CASE("application explicit reload rejects untitled documents and leaves saved selections scalar aligned") {
    WatchWorkspace files;
    files.write("unicode.txt", "a\xf0\x9f\x98\x80z");
    auto host = files.host(false);
    const auto untitled = call(*host, "document.create");
    CHECK(response(*host, "workspace.reload", {{"id", untitled.at("id")}, {"baseRevision", "0"}}).contains("error"));
    const auto opened = done(*host, "workspace.open", {{"path", "unicode.txt"}});
    (void)call(*host, "document.setSelections", {{"id", opened.at("id")}, {"baseRevision", "0"},
        {"selections", Json::array({{{"anchor", 1}, {"head", 5}}})}});
    files.write("unicode.txt", "a\xe4\xb8\xadz");
    const auto reloaded = done(*host, "workspace.reload", {{"id", opened.at("id")}, {"baseRevision", "0"}});
    CHECK(reloaded.at("selections")[0].at("anchor") == 1);
    CHECK(reloaded.at("selections")[0].at("head") == 4);
    CHECK_FALSE(reloaded.at("dirty").get<bool>());
}
