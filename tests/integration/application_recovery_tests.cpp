#include <astra/service/service_host.hpp>
#include <astra/service/application_service.hpp>
#include <astra/recovery/store.hpp>
#include <astra/protocol/rpc.hpp>
#include "support/temporary_directory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <random>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using astra::protocol::Json;
using astra::service::ServiceHost;
using namespace std::chrono_literals;

struct RecoveryWorkspace {
    fs::path base = fs::canonical(fs::current_path() / "build");
    fs::path directory = base / ("recovery-app-" + std::to_string(std::random_device{}()) + "-" +
                                 std::to_string(std::random_device{}()));

    RecoveryWorkspace() {
        fs::create_directories(workspace());
    }

    ~RecoveryWorkspace() {
        for (const auto& issue :
             astra::test::cleanup_owned_directory(directory, base, "recovery-app-", 1s)) {
            CHECK_MESSAGE(false, issue.message);
        }
    }

    fs::path workspace() const {
        return directory / "workspace";
    }

    fs::path state() const {
        return directory / "state";
    }

    std::unique_ptr<ServiceHost> host() const {
        astra::service::ServiceHostOptions options;
        options.workspace = workspace();
        options.recovery_directory = state();
        return std::make_unique<ServiceHost>(std::move(options));
    }

    void write(std::string_view name, std::string_view value) const {
        std::ofstream output(workspace() / name, std::ios::binary | std::ios::trunc);
        output.exceptions(std::ios::badbit | std::ios::failbit);
        output << value;
    }
};

Json response(ServiceHost& host, std::string method, Json params = Json::object()) {
    const Json request{{"jsonrpc", "2.0"},
                       {"id", "recovery-test"},
                       {"method", std::move(method)},
                       {"params", std::move(params)}};
    auto future = host.submit(request.dump());
    REQUIRE(future.wait_for(10s) == std::future_status::ready);
    const auto body = future.get();
    REQUIRE(body);
    return Json::parse(*body);
}

Json call(ServiceHost& host, std::string method, Json params = Json::object()) {
    const auto result = response(host, std::move(method), std::move(params));
    INFO(result.dump());
    REQUIRE_FALSE(result.contains("error"));
    return result.at("result");
}

Json wait(ServiceHost& host, const Json& started) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = call(host, "operation.get", {{"id", started.at("operationId")}});
        if (result.at("state") != "pending") {
            return result;
        }
        std::this_thread::sleep_for(5ms);
    }
    FAIL("Recovery operation did not settle");
    return {};
}

Json done(ServiceHost& host, std::string method, Json params = Json::object()) {
    const auto result = wait(host, call(host, std::move(method), std::move(params)));
    INFO(result.dump());
    REQUIRE(result.at("state") == "completed");
    return result.at("result");
}

std::string abandoned_session(const RecoveryWorkspace& fixture) {
    for (const auto& session : astra::recovery::RecoveryStore::discover(fixture.state())) {
        if (!session.active && !session.buffers.empty()) {
            return session.id;
        }
    }
    FAIL("No recoverable checkpoint was retained");
    return {};
}

// Completing a scheduled task does not require immediately destroying its
// callbacks. Retain them deliberately to make leaked session ownership a
// deterministic failure, without depending on thread scheduling or sleeps.
class RetainedRecoveryWork {
  public:
    struct Task {
        std::function<void()> worker;
        std::function<void(std::exception_ptr)> complete;
        std::promise<void> promise;
        std::exception_ptr error;
        bool ran{};
        bool completed{};
    };

    explicit RetainedRecoveryWork(const RecoveryWorkspace& fixture)
        : app(
              fixture.workspace(),
              [this](auto worker, auto complete) {
                  auto task = std::make_unique<Task>();
                  task->worker = std::move(worker);
                  task->complete = std::move(complete);
                  auto future = task->promise.get_future();
                  tasks.push_back(std::move(task));
                  return future;
              },
              {},
              {},
              {},
              fixture.state()) {
    }

    ~RetainedRecoveryWork() {
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            if (!tasks[i]->ran) {
                run_worker(i);
            }
            if (!tasks[i]->completed) {
                complete(i);
            }
        }
        tasks.clear();
    }

    Json call(std::string method, Json params = Json::object()) {
        const auto response = app.dispatch({{"jsonrpc", "2.0"},
                                            {"id", "retained-recovery"},
                                            {"method", std::move(method)},
                                            {"params", std::move(params)}});
        REQUIRE(response);
        INFO(response->dump());
        REQUIRE_FALSE(response->contains("error"));
        return response->at("result");
    }

    void run_worker(std::size_t index) {
        auto& task = *tasks.at(index);
        task.ran = true;
        try {
            task.worker();
        } catch (...) {
            task.error = std::current_exception();
        }
    }

    void complete(std::size_t index) {
        auto& task = *tasks.at(index);
        try {
            task.complete(task.error);
        } catch (...) {
            task.error = std::current_exception();
        }
        if (task.error) {
            task.promise.set_exception(task.error);
        } else {
            task.promise.set_value();
        }
        task.completed = true;
    }

    Json finish(const Json& started, std::size_t index) {
        run_worker(index);
        complete(index);
        return call("operation.get", {{"id", started.at("operationId")}});
    }

    std::vector<std::unique_ptr<Task>> tasks;
    astra::service::ApplicationService app;
};
} // namespace

TEST_CASE("application shutdown captures immediate dirty text and restores selections without "
          "writing source files") {
    RecoveryWorkspace fixture;
    const std::string text = "\xe4\xb8\xad\xf0\x9f\x98\x80\nunsaved";
    {
        auto host = fixture.host();
        const auto fresh = call(*host, "document.create");
        auto document = call(
            *host, "document.insert", {{"id", fresh["id"]}, {"baseRevision", "0"}, {"text", text}});
        static_cast<void>(call(*host,
                               "document.setSelections",
                               {{"id", fresh["id"]},
                                {"baseRevision", document["revision"]},
                                {"selections", Json::array({{{"anchor", 3}, {"head", 7}}})}}));
        host->shutdown(); // No debounce wait and no explicit recovery.flush request.
    }
    const auto session = abandoned_session(fixture);
    auto host = fixture.host();
    const auto restored = done(*host, "recovery.restore", {{"sessionId", session}});
    REQUIRE(restored["documents"].size() == 1);
    const auto id = restored["documents"][0]["id"];
    const auto document = call(*host, "document.get", {{"id", id}});
    CHECK(document["text"] == text);
    CHECK(document["dirty"].get<bool>());
    CHECK(document["selections"][0]["anchor"] == 3);
    CHECK(document["selections"][0]["head"] == 7);
    CHECK(fs::is_empty(fixture.workspace()));
    static_cast<void>(done(*host, "recovery.flush"));
    const auto listed = done(*host, "recovery.list");
    CHECK(
        std::none_of(listed["sessions"].begin(), listed["sessions"].end(), [&](const auto& entry) {
            return entry["id"] == session && !entry["buffers"].empty();
        }));
}

TEST_CASE(
    "application restored disk versions reject external overwrite and open-buffer collisions") {
    RecoveryWorkspace fixture;
    fixture.write("main.txt", "original");
    {
        auto host = fixture.host();
        const auto document = done(*host, "workspace.open", {{"path", "main.txt"}});
        static_cast<void>(
            call(*host,
                 "document.insert",
                 {{"id", document["id"]}, {"baseRevision", "0"}, {"text", "dirty "}}));
        host->shutdown();
    }
    const auto session = abandoned_session(fixture);
    fixture.write("main.txt", "external edit");
    auto host = fixture.host();
    const auto opened = done(*host, "workspace.open", {{"path", "main.txt"}});
    auto refused = wait(*host, call(*host, "recovery.restore", {{"sessionId", session}}));
    CHECK(refused["state"] == "failed");
    CHECK(call(*host, "workspace.documents").size() == 1);
    static_cast<void>(call(*host, "document.close", {{"id", opened["id"]}, {"baseRevision", "0"}}));
    const auto restored = done(*host, "recovery.restore", {{"sessionId", session}});
    const auto document = restored["documents"][0];
    const auto save = wait(*host,
                           call(*host,
                                "workspace.save",
                                {{"id", document["id"]}, {"baseRevision", document["revision"]}}));
    CHECK(save["state"] == "failed");
    CHECK(astra::io::read_file(fixture.workspace() / "main.txt").text == "external edit");
    CHECK(call(*host, "document.get", {{"id", document["id"]}})["text"] == "dirty original");
}

TEST_CASE("application recovery checkpoints idle clients and erases explicitly saved or discarded "
          "buffers") {
    RecoveryWorkspace fixture;
    auto host = fixture.host();
    const auto fresh = call(*host, "document.create");
    const auto document =
        call(*host,
             "document.insert",
             {{"id", fresh["id"]}, {"baseRevision", "0"}, {"text", "idle checkpoint"}});
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    bool persisted = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto sessions = astra::recovery::RecoveryStore::discover(fixture.state());
        persisted = std::any_of(sessions.begin(), sessions.end(), [](const auto& session) {
            return !session.buffers.empty();
        });
        if (persisted) {
            break;
        }
        std::this_thread::sleep_for(10ms); // No RPC wakes the owner loop in this wait.
    }
    REQUIRE(persisted);
    static_cast<void>(done(
        *host,
        "workspace.save",
        {{"id", document["id"]}, {"baseRevision", document["revision"]}, {"path", "saved.txt"}}));
    static_cast<void>(done(*host, "recovery.flush"));
    CHECK(done(*host, "recovery.list")["sessions"].empty());
    const auto dirty = call(
        *host,
        "document.insert",
        {{"id", document["id"]}, {"baseRevision", document["revision"]}, {"text", "discard me"}});
    static_cast<void>(done(*host, "recovery.flush"));
    static_cast<void>(
        call(*host,
             "document.close",
             {{"id", dirty["id"]}, {"baseRevision", dirty["revision"]}, {"discardChanges", true}}));
    static_cast<void>(done(*host, "recovery.flush"));
    CHECK(done(*host, "recovery.list")["sessions"].empty());
}

TEST_CASE(
    "application recovery rejects two checkpoints for one file before publishing any buffer") {
    RecoveryWorkspace fixture;
    fixture.write("shared.txt", "baseline");
    const auto file = astra::io::read_file(fixture.workspace() / "shared.txt");
    std::string session;
    {
        astra::recovery::RecoveryStore store(fixture.state());
        session = store.session_id();
        astra::Document first(astra::BufferId{1}, "first dirty text");
        astra::Document second(astra::BufferId{2}, "second dirty text");
        store.checkpoint({first.snapshot(), file.version.path, file.encoding, file.version});
        auto second_path = file.version.path;
#ifdef _WIN32
        second_path = second_path.parent_path() / "SHARED.TXT";
#endif
        const astra::io::FileVersion second_version{second_path, file.version.raw_content};
        store.checkpoint({second.snapshot(), second_path, file.encoding, second_version});
    }
    auto host = fixture.host();
    const auto refused = wait(*host, call(*host, "recovery.restore", {{"sessionId", session}}));
    CHECK(refused["state"] == "failed");
    CHECK(call(*host, "workspace.documents").empty());
    CHECK(astra::io::read_file(fixture.workspace() / "shared.txt").text == "baseline");
    const auto restored =
        done(*host, "recovery.restore", {{"sessionId", session}, {"ids", Json::array({"2"})}});
    REQUIRE(restored["documents"].size() == 1);
    CHECK(call(*host, "document.get", {{"id", restored["documents"][0]["id"]}})["text"] ==
          "second dirty text");
}

TEST_CASE("application recovery worker failure releases its session before completion destroys "
          "callbacks") {
    RecoveryWorkspace fixture;
    std::string session;
    {
        astra::recovery::RecoveryStore store(fixture.state());
        session = store.session_id();
        astra::Document document({1}, "recoverable text");
        store.checkpoint({document.snapshot(), {}, astra::io::TextEncoding::utf8, {}});
    }
    RetainedRecoveryWork host(fixture);
    const auto failed =
        host.call("recovery.restore", {{"sessionId", session}, {"ids", Json::array({"2"})}});
    REQUIRE(host.tasks.size() == 1);
    host.run_worker(0);
    REQUIRE(host.tasks[0]->error);
    {
        // The worker has finished with an error, but its completion has not run.
        astra::recovery::RecoveryStore available(fixture.state(), session);
        CHECK(available.load({1})->document.text.str() == "recoverable text");
    }
    host.complete(0);
    CHECK(host.call("operation.get", {{"id", failed.at("operationId")}}).at("state") == "failed");
    const auto retry = host.call("recovery.restore", {{"sessionId", session}});
    const auto restored = host.finish(retry, 1);
    INFO(restored.dump());
    REQUIRE(restored.at("state") == "completed");
    CHECK(restored.at("result").at("documents").size() == 1);
}

TEST_CASE(
    "application recovery publication failure releases its session despite retained callbacks") {
    RecoveryWorkspace fixture;
    fixture.write("shared.txt", "baseline");
    const auto file = astra::io::read_file(fixture.workspace() / "shared.txt");
    std::string session;
    {
        astra::recovery::RecoveryStore store(fixture.state());
        session = store.session_id();
        astra::Document first({1}, "first dirty text");
        astra::Document second({2}, "second dirty text");
        store.checkpoint({first.snapshot(), file.version.path, file.encoding, file.version});
        store.checkpoint({second.snapshot(), file.version.path, file.encoding, file.version});
    }
    RetainedRecoveryWork host(fixture);
    const auto failed = host.call("recovery.restore", {{"sessionId", session}});
    const auto rejected = host.finish(failed, 0);
    REQUIRE(rejected.at("state") == "failed");
    CHECK(rejected.at("error").at("message") ==
          "Restore conflicting checkpoints for one file separately");
    CHECK(host.call("workspace.documents").empty());
    {
        astra::recovery::RecoveryStore available(fixture.state(), session);
        CHECK(available.inspect().buffers.size() == 2);
    }
    const auto retry =
        host.call("recovery.restore", {{"sessionId", session}, {"ids", Json::array({"2"})}});
    const auto restored = host.finish(retry, 1);
    INFO(restored.dump());
    REQUIRE(restored.at("state") == "completed");
    CHECK(restored.at("result").at("documents").size() == 1);
}

TEST_CASE("application recovery durable transfer releases source claims despite retained completed "
          "tasks") {
    RecoveryWorkspace fixture;
    std::string session;
    {
        astra::recovery::RecoveryStore store(fixture.state());
        session = store.session_id();
        astra::Document document({1}, "durable transferred text");
        store.checkpoint({document.snapshot(), {}, astra::io::TextEncoding::utf8, {}});
    }
    RetainedRecoveryWork host(fixture);
    const auto restore = host.call("recovery.restore", {{"sessionId", session}});
    const auto restored = host.finish(restore, 0);
    REQUIRE(restored.at("state") == "completed");
    CHECK_THROWS_AS((void)astra::recovery::RecoveryStore(fixture.state(), session),
                    astra::recovery::RecoveryLocked);
    const auto flush = host.call("recovery.flush");
    const auto flushed = host.finish(flush, 1);
    INFO(flushed.dump());
    REQUIRE(flushed.at("state") == "completed");
    REQUIRE(host.tasks.size() == 2); // Both completed tasks still retain closures.
    astra::recovery::RecoveryStore source(fixture.state(), session);
    CHECK_FALSE(source.load({1}));
    CHECK(host.call("document.get", {{"id", restored.at("result").at("documents")[0].at("id")}})
              .at("text") == "durable transferred text");
    const auto sessions = astra::recovery::RecoveryStore::discover(fixture.state());
    CHECK(std::any_of(sessions.begin(), sessions.end(), [&](const auto& entry) {
        return entry.id != session && entry.active && entry.buffers.size() == 1;
    }));
}

TEST_CASE("application recovery startup announcements and restoration serialize temporary claims") {
    RecoveryWorkspace fixture;
    std::string session;
    {
        astra::recovery::RecoveryStore store(fixture.state());
        session = store.session_id();
        // Metadata inspection reads these checkpoints; this also exercises the
        // real scan while another worker is ready to restore a selected record.
        for (std::uint64_t id = 1; id <= 12; ++id) {
            astra::Document document({id}, std::string(16 * 1024, 'x'));
            store.checkpoint({document.snapshot(), {}, astra::io::TextEncoding::utf8, {}});
        }
    }
    RetainedRecoveryWork host(fixture);
    const auto flush = host.call("recovery.flush");
    const auto restore =
        host.call("recovery.restore", {{"sessionId", session}, {"ids", Json::array({"1"})}});
    REQUIRE(host.tasks.size() == 2);
    std::latch start(3);
    auto first = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        host.run_worker(0);
    });
    auto second = std::async(std::launch::async, [&] {
        start.arrive_and_wait();
        host.run_worker(1);
    });
    start.arrive_and_wait();
    REQUIRE(first.wait_for(10s) == std::future_status::ready);
    REQUIRE(second.wait_for(10s) == std::future_status::ready);
    first.get();
    second.get();
    host.complete(0);
    host.complete(1);
    const auto checkpointed = host.call("operation.get", {{"id", flush.at("operationId")}});
    const auto restored = host.call("operation.get", {{"id", restore.at("operationId")}});
    INFO(checkpointed.dump());
    INFO(restored.dump());
    CHECK(checkpointed.at("state") == "completed");
    REQUIRE(restored.at("state") == "completed");
    CHECK(restored.at("result").at("documents").size() == 1);
}

TEST_CASE("application recovery shutdown releases session ownership before host destruction") {
    RecoveryWorkspace fixture;
    auto host = fixture.host();
    const auto fresh = call(*host, "document.create");
    static_cast<void>(
        call(*host,
             "document.insert",
             {{"id", fresh.at("id")}, {"baseRevision", "0"}, {"text", "immediate recovery"}}));
    const auto flushed = done(*host, "recovery.flush");
    const auto session = flushed.at("sessionId").get<std::string>();
    CHECK_THROWS_AS((void)astra::recovery::RecoveryStore(fixture.state(), session),
                    astra::recovery::RecoveryLocked);
    host->shutdown();
    // The host object and its loop facade still exist. OS ownership must already
    // be released at the explicit shutdown boundary, without a polling delay.
    astra::recovery::RecoveryStore claimed(fixture.state(), session);
    CHECK(claimed.load({std::stoull(fresh.at("id").get<std::string>())})->document.text.str() ==
          "immediate recovery");
}
