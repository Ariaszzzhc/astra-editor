#include <astra/service/application_service.hpp>
#include <astra/transport/tcp_server.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {
using astra::protocol::Json;
using astra::service::ApplicationService;
namespace fs = std::filesystem;

class AppFiles {
public:
    AppFiles() {
        const auto cwd = fs::canonical(fs::current_path());
        build_ = fs::weakly_canonical(cwd / "build");
        if (build_.parent_path() != cwd) { throw std::runtime_error("Unsafe test build directory"); }
        fs::create_directories(build_);
        std::random_device random;
        for (int i = 0; i < 32; ++i) {
            const auto candidate = build_ / ("app-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(candidate)) { root_ = fs::canonical(candidate); return; }
        }
        throw std::runtime_error("Cannot create application test directory");
    }
    ~AppFiles() {
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root_, error);
        if (error || resolved != root_ || resolved.parent_path() != build_) {
            std::fputs("Unsafe application test cleanup refused\n", stderr); return;
        }
        fs::remove_all(resolved, error);
        if (error) { std::fputs("Application test cleanup failed\n", stderr); }
    }
    AppFiles(const AppFiles&) = delete;
    AppFiles& operator=(const AppFiles&) = delete;
    const fs::path& root() const { return root_; }
    void write(const fs::path& relative, std::string_view text) const {
        fs::create_directories((root_ / relative).parent_path());
        std::ofstream stream(root_ / relative, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
    std::string read(const fs::path& relative) const { return astra::io::read_file(root_ / relative).text; }
private:
    fs::path root_;
    fs::path build_;
};

Json envelope(std::string method, Json params) {
    return {{"jsonrpc", "2.0"}, {"id", "test"}, {"method", std::move(method)}, {"params", std::move(params)}};
}

Json checked(const std::optional<Json>& response) {
    REQUIRE(response);
    INFO(response->dump());
    REQUIRE_FALSE(response->contains("error"));
    return response->at("result");
}

class LiveApp {
public:
    explicit LiveApp(const fs::path& root, std::string defaults = {}, std::string user = {}) {
        server_ = std::make_unique<astra::transport::TcpServer>(
            [this](std::string_view body) { return app_->dispatch_text(body); },
            [this] { return app_->take_notifications(); }, astra::transport::TcpServerOptions{},
            astra::transport::TcpServerHooks{
                [this, root, defaults = std::move(defaults), user = std::move(user)] {
                    app_ = std::make_unique<ApplicationService>(root,
                        [this](auto worker, auto complete) { return server_->queue_work(std::move(worker), std::move(complete)); },
                        defaults, user);
                }, [this] { app_.reset(); }});
        server_->start();
    }
    ~LiveApp() { server_->stop(); }
    Json response(std::string method, Json params = Json::object()) {
        auto future = server_->submit(envelope(std::move(method), std::move(params)).dump());
        REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        const auto body = future.get();
        REQUIRE(body);
        return Json::parse(*body);
    }
    Json call(std::string method, Json params = Json::object()) { return checked(response(std::move(method), std::move(params))); }
    Json wait(const Json& started) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            auto result = call("operation.get", {{"id", started["operationId"]}});
            if (result["state"] != "pending") { return result; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        FAIL("Workspace operation did not settle");
        return Json{};
    }
    Json done(std::string method, Json params = Json::object()) {
        const auto completed = wait(call(std::move(method), std::move(params)));
        INFO(completed.dump());
        REQUIRE(completed["state"] == "completed");
        return completed["result"];
    }
    std::vector<std::string> notifications() { return server_->take_local_notifications(); }
private:
    std::unique_ptr<ApplicationService> app_;
    std::unique_ptr<astra::transport::TcpServer> server_;
};

class PausedWork {
public:
    explicit PausedWork(const fs::path& root) : app(root, [this](auto worker, auto complete) {
        auto task = std::make_unique<Task>();
        task->worker = std::move(worker);
        task->complete = std::move(complete);
        auto future = task->promise.get_future();
        tasks.push_back(std::move(task));
        return future;
    }) {}
    Json call(std::string method, Json params = Json::object()) { return checked(app.dispatch(envelope(std::move(method), std::move(params)))); }
    void run_worker() {
        REQUIRE(tasks.size() == 1);
        try { tasks.front()->worker(); } catch (...) { tasks.front()->error = std::current_exception(); }
    }
    void complete() {
        REQUIRE(tasks.size() == 1);
        auto task = std::move(tasks.front());
        tasks.clear();
        task->complete(task->error);
        if (task->error) { task->promise.set_exception(task->error); } else { task->promise.set_value(); }
    }
    Json done(std::string method, Json params = Json::object()) {
        const auto operation = call(std::move(method), std::move(params));
        run_worker(); complete();
        const auto result = call("operation.get", {{"id", operation["operationId"]}});
        REQUIRE(result["state"] == "completed");
        return result["result"];
    }
    ApplicationService app;
private:
    struct Task {
        std::function<void()> worker;
        std::function<void(std::exception_ptr)> complete;
        std::promise<void> promise;
        std::exception_ptr error;
    };
    std::vector<std::unique_ptr<Task>> tasks;
};
} // namespace

TEST_CASE("application real libuv open edit save and reopen uses one service") {
    AppFiles files;
    files.write("source.cpp", "int main() {}\n");
    LiveApp app(files.root());
    auto document = app.done("workspace.open", {{"path", "source.cpp"}});
    const auto id = document["id"];
    CHECK(document["path"] == "source.cpp");
    CHECK_FALSE(document["dirty"].get<bool>());
    CHECK(app.done("workspace.open", {{"path", "source.cpp"}})["id"] == id);
    CHECK(app.call("workspace.documents").size() == 1);
    document = app.call("document.insert", {{"id", id}, {"baseRevision", "0"}, {"text", "// Astra\n"}});
    CHECK(document["dirty"].get<bool>());
    document = app.done("workspace.save", {{"id", id}, {"baseRevision", document["revision"]}});
    CHECK_FALSE(document["dirty"].get<bool>());
    CHECK(files.read("source.cpp") == "// Astra\nint main() {}\n");
    static_cast<void>(app.call("document.close", {{"id", id}, {"baseRevision", document["revision"]}}));
    document = app.done("workspace.open", {{"path", "source.cpp"}});
    CHECK(app.call("document.get", {{"id", document["id"]}})["text"] == files.read("source.cpp"));
    const auto notifications = app.notifications();
    CHECK(std::any_of(notifications.begin(), notifications.end(), [](const auto& text) {
        const auto event = Json::parse(text);
        return event["method"] == "document.created" && event["params"].value("path", "") == "source.cpp";
    }));
}

TEST_CASE("application EditorConfig save preserves selection and marks formatting undo dirty") {
    AppFiles files;
    files.write(".editorconfig", "root=true\n[*]\ntrim_trailing_whitespace=true\ninsert_final_newline=true\nend_of_line=crlf\n");
    files.write("a.txt", "alpha  ");
    LiveApp app(files.root());
    const auto document = app.done("workspace.open", {{"path", "a.txt"}});
    const auto saved = app.done("workspace.save", {{"id", document["id"]}, {"baseRevision", "0"}});
    CHECK(saved["revision"] == "1");
    CHECK(saved["selections"][0]["head"] == 0);
    CHECK_FALSE(saved["dirty"].get<bool>());
    CHECK(files.read("a.txt") == "alpha\r\n");
    const auto undone = app.call("document.undo", {{"id", document["id"]}, {"baseRevision", "1"}});
    CHECK(undone["dirty"].get<bool>());
    CHECK(app.call("document.get", {{"id", document["id"]}})["text"] == "alpha  ");
}

TEST_CASE("application external conflict preserves both disk and unsaved buffer") {
    AppFiles files;
    files.write("a.txt", "old");
    LiveApp app(files.root());
    const auto document = app.done("workspace.open", {{"path", "a.txt"}});
    static_cast<void>(app.call("document.insert", {{"id", document["id"]}, {"baseRevision", "0"}, {"text", "edited "}}));
    files.write("a.txt", "external");
    const auto failed = app.wait(app.call("workspace.save", {{"id", document["id"]}, {"baseRevision", "1"}}));
    REQUIRE(failed["state"] == "failed");
    CHECK(failed["error"]["code"] == -32011);
    CHECK(files.read("a.txt") == "external");
    const auto current = app.call("document.get", {{"id", document["id"]}});
    CHECK(current["text"] == "edited old");
    CHECK(current["dirty"].get<bool>());
}

TEST_CASE("application asynchronous formatting never overwrites newer typing or marks old history clean") {
    AppFiles files;
    files.write(".editorconfig", "root=true\n[*]\ntrim_trailing_whitespace=true\ninsert_final_newline=true\n");
    files.write("a.txt", "old  ");
    PausedWork app(files.root());
    const auto doc = app.done("workspace.open", {{"path", "a.txt"}});
    const auto operation = app.call("workspace.save", {{"id", doc["id"]}, {"baseRevision", "0"}});
    app.run_worker();
    static_cast<void>(app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "new "}}));
    app.complete();
    CHECK(app.call("operation.get", {{"id", operation["operationId"]}})["state"] == "completed");
    CHECK(files.read("a.txt") == "old\n");
    const auto current = app.call("document.get", {{"id", doc["id"]}});
    CHECK(current["text"] == "new old  ");
    CHECK(current["dirty"].get<bool>());
    const auto undone = app.call("document.undo", {{"id", doc["id"]}, {"baseRevision", "1"}});
    CHECK(undone["dirty"].get<bool>());
    CHECK(app.call("document.get", {{"id", doc["id"]}})["text"] == "old  ");
}

TEST_CASE("application unformatted old save retains its precise history identity") {
    AppFiles files;
    files.write("a.txt", "old");
    PausedWork app(files.root());
    const auto doc = app.done("workspace.open", {{"path", "a.txt"}});
    static_cast<void>(app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "saved "}}));
    static_cast<void>(app.call("workspace.save", {{"id", doc["id"]}, {"baseRevision", "1"}}));
    app.run_worker();
    static_cast<void>(app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "1"}, {"text", "new "}}));
    app.complete();
    CHECK(app.call("document.get", {{"id", doc["id"]}})["dirty"].get<bool>());
    const auto undone = app.call("document.undo", {{"id", doc["id"]}, {"baseRevision", "2"}});
    CHECK_FALSE(undone["dirty"].get<bool>());
    CHECK(files.read("a.txt") == "saved old");
}

TEST_CASE("application save as creates new files and refuses implicit overwrite") {
    AppFiles files;
    files.write("existing.txt", "external");
    LiveApp app(files.root());
    const auto doc = app.call("document.create", {{"text", "new file"}});
    CHECK(app.response("workspace.save", {{"id", doc["id"]}, {"baseRevision", "0"}}).contains("error"));
    const auto failed = app.wait(app.call("workspace.save", {{"id", doc["id"]}, {"baseRevision", "0"}, {"path", "existing.txt"}}));
    CHECK(failed["state"] == "failed");
    CHECK(files.read("existing.txt") == "external");
    const auto saved = app.done("workspace.save", {{"id", doc["id"]}, {"baseRevision", "0"}, {"path", "new.txt"}});
    CHECK(saved["path"] == "new.txt");
    CHECK(files.read("new.txt") == "new file");
    CHECK(app.response("workspace.open", {{"path", "../outside"}}).contains("error"));
}

TEST_CASE("application tree pagination is complete and search replace refreshes clean buffers") {
    AppFiles files;
    for (int i = 0; i < 12; ++i) { files.write("file" + std::to_string(i) + ".txt", "needle x needle"); }
    LiveApp app(files.root());
    const auto tree_op = app.call("workspace.tree");
    CHECK(app.wait(tree_op)["state"] == "completed");
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < 12; offset += 3) {
        const auto page = app.call("operation.get", {{"id", tree_op["operationId"]}, {"offset", offset}, {"limit", 3}});
        count += page["result"]["entries"].size();
        CHECK(page["page"]["nextOffset"] == offset + 3);
    }
    CHECK(count == 12);
    const auto document = app.done("workspace.open", {{"path", "file0.txt"}});
    const auto search = app.call("workspace.search", {{"query", "needle"}});
    CHECK(app.wait(search)["result"]["matches"].size() == 24);
    const auto replaced = app.done("workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "\xE4\xB8\xAD"}});
    CHECK(replaced["applied"].size() == 12);
    CHECK(replaced["pending"].empty());
    const auto updated = app.call("document.get", {{"id", document["id"]}});
    CHECK(updated["text"] == "\xE4\xB8\xAD x \xE4\xB8\xAD");
    CHECK_FALSE(updated["dirty"].get<bool>());
    CHECK(files.read("file11.txt") == updated["text"].get<std::string>());
}

TEST_CASE("application replacement rejects dirty matching buffers and stale search content") {
    AppFiles files;
    files.write("a.txt", "target");
    LiveApp app(files.root());
    const auto doc = app.done("workspace.open", {{"path", "a.txt"}});
    const auto search = app.call("workspace.search", {{"query", "target"}});
    CHECK(app.wait(search)["state"] == "completed");
    static_cast<void>(app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "new "}}));
    CHECK(app.response("workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "done"}}).contains("error"));
    CHECK(files.read("a.txt") == "target");
    static_cast<void>(app.call("document.undo", {{"id", doc["id"]}, {"baseRevision", "1"}}));
    files.write("a.txt", "external target");
    const auto operation = app.wait(app.call("workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "done"}}));
    CHECK(operation["state"] == "failed");
    CHECK(files.read("a.txt") == "external target");
}

TEST_CASE("application cancellation before a save has no disk effect and future settles") {
    AppFiles files;
    files.write("a.txt", "old");
    PausedWork app(files.root());
    const auto doc = app.done("workspace.open", {{"path", "a.txt"}});
    static_cast<void>(app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "new "}}));
    const auto operation = app.call("workspace.save", {{"id", doc["id"]}, {"baseRevision", "1"}});
    static_cast<void>(app.call("operation.cancel", {{"id", operation["operationId"]}}));
    app.run_worker(); app.complete();
    CHECK(app.call("operation.get", {{"id", operation["operationId"]}})["state"] == "cancelled");
    CHECK(files.read("a.txt") == "old");
    CHECK(app.call("document.get", {{"id", doc["id"]}})["dirty"].get<bool>());
}

TEST_CASE("application user scripts override defaults through the production dispatcher") {
    AppFiles files;
    LiveApp app(files.root(),
        "astra.configure({editor:{tabSize:4,fontSize:15}}); astra.bind('Ctrl+N','new');"
        "astra.registerCommand('new',()=>astra.request('document.create',{text:'default'}));",
        "astra.configure({editor:{tabSize:2}});"
        "astra.registerCommand('new',()=>astra.request('document.create',{text:'user'}));");
    const auto settings = app.call("script.settings");
    CHECK(settings["settings"]["editor"]["tabSize"] == 2);
    CHECK(settings["settings"]["editor"]["fontSize"] == 15);
    const auto doc = app.call("script.command", {{"name", "new"}});
    CHECK(app.call("document.get", {{"id", doc["id"]}})["text"] == "user");
}

TEST_CASE("application rejected scheduler surfaces failed operations without an eternal pending state") {
    AppFiles files;
    ApplicationService app(files.root(), [](auto, auto) {
        std::promise<void> rejected;
        auto future = rejected.get_future();
        rejected.set_exception(std::make_exception_ptr(std::runtime_error("queue full")));
        return future;
    });
    const auto started = checked(app.dispatch(envelope("workspace.tree", Json::object())));
    const auto result = checked(app.dispatch(envelope("operation.get", {{"id", started["operationId"]}})));
    CHECK(result["state"] == "failed");
    CHECK(result["error"]["message"] == "queue full");
}

TEST_CASE("bundled JavaScript defaults execute editing and respect incremental configuration") {
    AppFiles files;
    const auto defaults = astra::io::read_file(fs::current_path() / "data" / "defaults.js").text;
    LiveApp app(files.root(), defaults, "astra.configure({editor:{tabSize:2}}); astra.bind('Ctrl+S','custom.save');");
    const auto settings = app.call("script.settings");
    CHECK(settings["keybindings"]["Ctrl+S"] == "custom.save");
    CHECK(settings["keybindings"]["Ctrl+Z"] == "editor.undo");
    CHECK(settings["settings"]["editor"]["fontSize"] == 16);
    const auto doc = app.call("document.create");
    auto result = app.call("script.command", {{"name", "editor.indent"}, {"params", {{"id", doc["id"]}, {"baseRevision", "0"}}}});
    CHECK(app.call("document.get", {{"id", doc["id"]}})["text"] == "  ");
    result = app.call("script.command", {{"name", "editor.undo"}, {"params", {{"id", doc["id"]}, {"baseRevision", result["revision"]}}}});
    CHECK(app.call("document.get", {{"id", doc["id"]}})["text"] == "");
}
