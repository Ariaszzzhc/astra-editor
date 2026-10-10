#include <astra/service/application_service.hpp>
#include <astra/transport/tcp_server.hpp>
#include <astra/process/process.hpp>
#include "support/temporary_directory.hpp"

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
        if (build_.parent_path() != cwd) {
            throw std::runtime_error("Unsafe test build directory");
        }
        fs::create_directories(build_);
        std::random_device random;
        for (int i = 0; i < 32; ++i) {
            const auto candidate =
                build_ / ("app-test-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (fs::create_directory(candidate)) {
                root_ = fs::canonical(candidate);
                return;
            }
        }
        throw std::runtime_error("Cannot create application test directory");
    }

    ~AppFiles() {
        const auto retry_budget =
            git_used_ ? std::chrono::milliseconds(1000) : std::chrono::milliseconds::zero();
        for (const auto& issue :
             astra::test::cleanup_owned_directory(root_, build_, "app-test-", retry_budget)) {
            CHECK_MESSAGE(false, issue.path.string(), ": ", issue.message);
        }
    }

    AppFiles(const AppFiles&) = delete;
    AppFiles& operator=(const AppFiles&) = delete;

    const fs::path& root() const {
        return root_;
    }

    void write(const fs::path& relative, std::string_view text) const {
        fs::create_directories((root_ / relative).parent_path());
        std::ofstream stream(root_ / relative, std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    std::string read(const fs::path& relative) const {
        return astra::io::read_file(root_ / relative).text;
    }

    void initialize_git() {
        git_used_ = true;
        astra::process::ProcessOptions options;
        options.argv = {"git", "init", "--initial-branch=main"};
        const auto bytes = root_.generic_u8string();
        options.cwd = std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        options.env = astra::process::current_environment();
        std::erase_if(*options.env, [](const auto& entry) {
            return entry.starts_with("GIT_");
        });
        options.env->insert(options.env->end(),
                            {"GIT_CONFIG_NOSYSTEM=1",
                             "GIT_CONFIG_GLOBAL=NUL",
                             "GIT_CONFIG_SYSTEM=NUL",
                             "GIT_TERMINAL_PROMPT=0"});
        astra::process::Process child(std::move(options));
        child.start().get();
        child.close();
        const auto completion = child.completion();
        REQUIRE(completion.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        REQUIRE(completion.get().exit_status == 0);
    }

  private:
    fs::path root_;
    fs::path build_;
    bool git_used_{};
};

Json envelope(std::string method, Json params) {
    return {{"jsonrpc", "2.0"},
            {"id", "test"},
            {"method", std::move(method)},
            {"params", std::move(params)}};
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
            [this](std::string_view body) {
                return app_->dispatch_text(body);
            },
            [this] {
                return app_->take_notifications();
            },
            astra::transport::TcpServerOptions{},
            astra::transport::TcpServerHooks{
                [this, root, defaults = std::move(defaults), user = std::move(user)] {
                    app_ = std::make_unique<ApplicationService>(
                        root,
                        [this](auto worker, auto complete) {
                            return server_->queue_work(std::move(worker), std::move(complete));
                        },
                        defaults + "\nastra.configure({git:{inheritGlobalConfig:false}});",
                        user);
                },
                [this] {
                    app_.reset();
                }});
        server_->start();
    }

    ~LiveApp() {
        server_->stop();
    }

    Json response(std::string method, Json params = Json::object()) {
        auto future = server_->submit(envelope(std::move(method), std::move(params)).dump());
        REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
        const auto body = future.get();
        REQUIRE(body);
        return Json::parse(*body);
    }

    Json call(std::string method, Json params = Json::object()) {
        return checked(response(std::move(method), std::move(params)));
    }

    Json wait(const Json& started) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            auto result = call("operation.get", {{"id", started["operationId"]}});
            if (result["state"] != "pending") {
                return result;
            }
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

    std::vector<std::string> notifications() {
        return server_->take_local_notifications();
    }

  private:
    std::unique_ptr<ApplicationService> app_;
    std::unique_ptr<astra::transport::TcpServer> server_;
};

class PausedWork {
  public:
    explicit PausedWork(const fs::path& root)
        : app(root, [this](auto worker, auto complete) {
              auto task = std::make_unique<Task>();
              task->worker = std::move(worker);
              task->complete = std::move(complete);
              auto future = task->promise.get_future();
              tasks.push_back(std::move(task));
              return future;
          }) {
    }

    Json call(std::string method, Json params = Json::object()) {
        return checked(app.dispatch(envelope(std::move(method), std::move(params))));
    }

    void run_worker() {
        REQUIRE(tasks.size() == 1);
        try {
            tasks.front()->worker();
        } catch (...) {
            tasks.front()->error = std::current_exception();
        }
    }

    void complete() {
        REQUIRE(tasks.size() == 1);
        auto task = std::move(tasks.front());
        tasks.clear();
        task->complete(task->error);
        if (task->error) {
            task->promise.set_exception(task->error);
        } else {
            task->promise.set_value();
        }
    }

    Json done(std::string method, Json params = Json::object()) {
        const auto operation = call(std::move(method), std::move(params));
        run_worker();
        complete();
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
    document =
        app.call("document.insert", {{"id", id}, {"baseRevision", "0"}, {"text", "// Astra\n"}});
    CHECK(document["dirty"].get<bool>());
    document = app.done("workspace.save", {{"id", id}, {"baseRevision", document["revision"]}});
    CHECK_FALSE(document["dirty"].get<bool>());
    CHECK(files.read("source.cpp") == "// Astra\nint main() {}\n");
    static_cast<void>(
        app.call("document.close", {{"id", id}, {"baseRevision", document["revision"]}}));
    document = app.done("workspace.open", {{"path", "source.cpp"}});
    CHECK(app.call("document.get", {{"id", document["id"]}})["text"] == files.read("source.cpp"));
    const auto notifications = app.notifications();
    CHECK(std::any_of(notifications.begin(), notifications.end(), [](const auto& text) {
        const auto event = Json::parse(text);
        return event["method"] == "document.created" &&
               event["params"].value("path", "") == "source.cpp";
    }));
}

TEST_CASE("application EditorConfig save preserves selection and marks formatting undo dirty") {
    AppFiles files;
    files.write(".editorconfig",
                "root=true\n[*]\ntrim_trailing_whitespace=true\ninsert_final_newline=true\nend_of_"
                "line=crlf\n");
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
    static_cast<void>(app.call(
        "document.insert", {{"id", document["id"]}, {"baseRevision", "0"}, {"text", "edited "}}));
    files.write("a.txt", "external");
    const auto failed =
        app.wait(app.call("workspace.save", {{"id", document["id"]}, {"baseRevision", "1"}}));
    REQUIRE(failed["state"] == "failed");
    CHECK(failed["error"]["code"] == -32011);
    CHECK(files.read("a.txt") == "external");
    const auto current = app.call("document.get", {{"id", document["id"]}});
    CHECK(current["text"] == "edited old");
    CHECK(current["dirty"].get<bool>());
}

TEST_CASE("application asynchronous formatting never overwrites newer typing or marks old history "
          "clean") {
    AppFiles files;
    files.write(".editorconfig",
                "root=true\n[*]\ntrim_trailing_whitespace=true\ninsert_final_newline=true\n");
    files.write("a.txt", "old  ");
    PausedWork app(files.root());
    const auto doc = app.done("workspace.open", {{"path", "a.txt"}});
    const auto operation = app.call("workspace.save", {{"id", doc["id"]}, {"baseRevision", "0"}});
    app.run_worker();
    static_cast<void>(
        app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "new "}}));
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
    static_cast<void>(app.call("document.insert",
                               {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "saved "}}));
    static_cast<void>(app.call("workspace.save", {{"id", doc["id"]}, {"baseRevision", "1"}}));
    app.run_worker();
    static_cast<void>(
        app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "1"}, {"text", "new "}}));
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
    CHECK(app.response("workspace.save", {{"id", doc["id"]}, {"baseRevision", "0"}})
              .contains("error"));
    const auto failed = app.wait(app.call(
        "workspace.save", {{"id", doc["id"]}, {"baseRevision", "0"}, {"path", "existing.txt"}}));
    CHECK(failed["state"] == "failed");
    CHECK(files.read("existing.txt") == "external");
    const auto saved =
        app.done("workspace.save", {{"id", doc["id"]}, {"baseRevision", "0"}, {"path", "new.txt"}});
    CHECK(saved["path"] == "new.txt");
    CHECK(files.read("new.txt") == "new file");
    CHECK(app.response("workspace.open", {{"path", "../outside"}}).contains("error"));
}

TEST_CASE("application tree pagination is complete and search replace refreshes clean buffers") {
    AppFiles files;
    for (int i = 0; i < 12; ++i) {
        files.write("file" + std::to_string(i) + ".txt", "needle x needle");
    }
    LiveApp app(files.root());
    const auto tree_op = app.call("workspace.tree");
    CHECK(app.wait(tree_op)["state"] == "completed");
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < 12; offset += 3) {
        const auto page = app.call(
            "operation.get", {{"id", tree_op["operationId"]}, {"offset", offset}, {"limit", 3}});
        count += page["result"]["entries"].size();
        CHECK(page["page"]["nextOffset"] == offset + 3);
    }
    CHECK(count == 12);
    const auto document = app.done("workspace.open", {{"path", "file0.txt"}});
    const auto search = app.call("workspace.search", {{"query", "needle"}});
    CHECK(app.wait(search)["result"]["matches"].size() == 24);
    CHECK(app.wait(search)["result"]["fileCount"] == 12);
    CHECK(app.call("operation.get",
                   {{"id", search["operationId"]},
                    {"offset", 5},
                    {"limit", 2}})["result"]["fileCount"] == 12);
    const auto replaced =
        app.done("workspace.replace",
                 {{"searchId", search["operationId"]}, {"replacement", "\xE4\xB8\xAD"}});
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
    static_cast<void>(
        app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "new "}}));
    CHECK(app.response("workspace.replace",
                       {{"searchId", search["operationId"]}, {"replacement", "done"}})
              .contains("error"));
    CHECK(files.read("a.txt") == "target");
    static_cast<void>(app.call("document.undo", {{"id", doc["id"]}, {"baseRevision", "1"}}));
    files.write("a.txt", "external target");
    const auto operation = app.wait(app.call(
        "workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "done"}}));
    CHECK(operation["state"] == "failed");
    CHECK(files.read("a.txt") == "external target");
}

TEST_CASE("application cancellation before a save has no disk effect and future settles") {
    AppFiles files;
    files.write("a.txt", "old");
    PausedWork app(files.root());
    const auto doc = app.done("workspace.open", {{"path", "a.txt"}});
    static_cast<void>(
        app.call("document.insert", {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "new "}}));
    const auto operation = app.call("workspace.save", {{"id", doc["id"]}, {"baseRevision", "1"}});
    static_cast<void>(app.call("operation.cancel", {{"id", operation["operationId"]}}));
    app.run_worker();
    app.complete();
    CHECK(app.call("operation.get", {{"id", operation["operationId"]}})["state"] == "cancelled");
    CHECK(files.read("a.txt") == "old");
    CHECK(app.call("document.get", {{"id", doc["id"]}})["dirty"].get<bool>());
}

TEST_CASE("application user scripts override defaults through the production dispatcher") {
    AppFiles files;
    LiveApp app(
        files.root(),
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

TEST_CASE("application async scripts await real open edit and durable save") {
    AppFiles files;
    files.write("async.txt", "before");
    LiveApp app(
        files.root(),
        {},
        "astra.registerCommand('workflow',async()=>{"
        "const doc=await astra.request('workspace.open',{path:'async.txt'});"
        "const edit=await "
        "astra.request('document.insert',{id:doc.id,baseRevision:doc.revision,text:'after '});"
        "await astra.request('workspace.save',{id:edit.id,baseRevision:edit.revision});"
        "return await astra.request('document.get',{id:doc.id});});");
    const auto result = app.done("script.command", {{"name", "workflow"}});
    CHECK(result.at("text") == "after before");
    CHECK_FALSE(result.at("dirty").get<bool>());
    CHECK(files.read("async.txt") == "after before");
}

TEST_CASE("application pending scripts do not block input and cancellation stops later edits") {
    AppFiles files;
    LiveApp app(
        files.root(),
        {},
        "astra.registerCommand('later',async p=>{await astra.sleep(200);"
        "return await "
        "astra.request('document.insert',{id:p.id,baseRevision:p.revision,text:'late'});});");
    const auto document = app.call("document.create", {{"text", "original"}});
    const auto pending = app.call("script.command", {{"name", "later"}, {"params", document}});
    REQUIRE(pending.contains("operationId"));
    const auto changed = app.call(
        "document.insert", {{"id", document.at("id")}, {"baseRevision", "0"}, {"text", "now "}});
    CHECK(changed.at("revision") == "1");
    CHECK(app.call("operation.cancel", {{"id", pending.at("operationId")}})
              .at("cancelRequested")
              .get<bool>());
    CHECK(app.wait(pending).at("state") == "cancelled");
    std::this_thread::sleep_for(std::chrono::milliseconds(220));
    CHECK(app.call("document.get", {{"id", document.at("id")}}).at("text") == "now original");
}

TEST_CASE("application async edits reject revisions made stale while awaiting") {
    AppFiles files;
    LiveApp app(
        files.root(),
        {},
        "astra.registerCommand('stale',async p=>{await astra.sleep(40);"
        "return await "
        "astra.request('document.insert',{id:p.id,baseRevision:p.revision,text:'stale'});});");
    const auto document = app.call("document.create", {{"text", "original"}});
    const auto pending = app.call("script.command", {{"name", "stale"}, {"params", document}});
    static_cast<void>(
        app.call("document.insert",
                 {{"id", document.at("id")}, {"baseRevision", "0"}, {"text", "current "}}));
    const auto failed = app.wait(pending);
    REQUIRE(failed.at("state") == "failed");
    CHECK(failed.at("error").at("name") == "ServiceError");
    CHECK(app.call("document.get", {{"id", document.at("id")}}).at("text") == "current original");
}

TEST_CASE(
    "application rejected scheduler surfaces failed operations without an eternal pending state") {
    AppFiles files;
    ApplicationService app(files.root(), [](auto, auto) {
        std::promise<void> rejected;
        auto future = rejected.get_future();
        rejected.set_exception(std::make_exception_ptr(std::runtime_error("queue full")));
        return future;
    });
    const auto started = checked(app.dispatch(envelope("workspace.tree", Json::object())));
    const auto result =
        checked(app.dispatch(envelope("operation.get", {{"id", started["operationId"]}})));
    CHECK(result["state"] == "failed");
    CHECK(result["error"]["message"] == "queue full");
}

TEST_CASE("bundled JavaScript defaults execute editing and respect incremental configuration") {
    AppFiles files;
    const auto defaults = astra::io::read_file(fs::current_path() / "data" / "defaults.js").text;
    LiveApp app(files.root(),
                defaults,
                "astra.configure({editor:{tabSize:2}}); astra.bind('Ctrl+S','custom.save');");
    const auto settings = app.call("script.settings");
    CHECK(settings["keybindings"]["Ctrl+S"] == "custom.save");
    CHECK(settings["keybindings"]["Ctrl+Z"] == "editor.undo");
    CHECK(settings["settings"]["editor"]["fontSize"] == 16);
    const auto doc = app.call("document.create");
    auto result = app.call(
        "script.command",
        {{"name", "editor.indent"}, {"params", {{"id", doc["id"]}, {"baseRevision", "0"}}}});
    CHECK(app.call("document.get", {{"id", doc["id"]}})["text"] == "  ");
    result = app.call("script.command",
                      {{"name", "editor.undo"},
                       {"params", {{"id", doc["id"]}, {"baseRevision", result["revision"]}}}});
    CHECK(app.call("document.get", {{"id", doc["id"]}})["text"] == "");
}

TEST_CASE("application asynchronous script results preserve scalar and array values") {
    AppFiles files;
    LiveApp app(files.root(),
                {},
                "astra.registerCommand('scalar',async()=>{await astra.sleep(30);return 42}); "
                "astra.registerCommand('array',async()=>{await astra.sleep(30);return [1,2,3]});");
    CHECK(app.done("script.command", {{"name", "scalar"}}) == 42);
    CHECK(app.done("script.command", {{"name", "array"}}) == Json::array({1, 2, 3}));
}

TEST_CASE("application file metadata reports the actual persisted text encoding") {
    AppFiles files;
    static_cast<void>(astra::io::save_file(
        files.root() / "utf16.txt", "中文", astra::io::TextEncoding::utf16_le));
    LiveApp app(files.root());
    const auto document = app.done("workspace.open", {{"path", "utf16.txt"}});
    CHECK(document["encoding"] == "UTF-16 LE");
    const auto edited = app.call("document.insert",
                                 {{"id", document["id"]}, {"baseRevision", "0"}, {"text", "🙂"}});
    const auto saved =
        app.done("workspace.save", {{"id", document["id"]}, {"baseRevision", edited["revision"]}});
    CHECK(saved["encoding"] == "UTF-16 LE");
    CHECK(astra::io::read_file(files.root() / "utf16.txt").encoding ==
          astra::io::TextEncoding::utf16_le);
}

TEST_CASE("application syntax powers real highlight outline and textobject commands") {
    AppFiles files;
    const std::string source = "int answer(int value) { return value + 42; }\n";
    files.write("answer.cpp", source);
    LiveApp app(files.root());
    const auto document = app.done("workspace.open", {{"path", "answer.cpp"}});
    const auto id = document["id"];
    auto syntax = app.call("document.syntax", {{"id", id}, {"baseRevision", "0"}});
    CHECK(syntax["state"] == "pending");
    CHECK(app.wait(syntax)["state"] == "completed");
    syntax = app.call("document.syntax", {{"id", id}, {"baseRevision", "0"}});
    REQUIRE(syntax["state"] == "ready");
    CHECK_FALSE(syntax["incremental"].get<bool>());
    REQUIRE_FALSE(syntax["outline"].empty());
    CHECK(syntax["outline"][0]["name"] == "answer");
    CHECK(
        std::any_of(syntax["highlights"].begin(), syntax["highlights"].end(), [](const auto& span) {
            return span["kind"] == "number";
        }));
    static_cast<void>(app.call(
        "document.setSelections",
        {{"id", id},
         {"baseRevision", "0"},
         {"selections",
          Json::array({{{"anchor", source.find("return")}, {"head", source.find("return")}}})}}));
    const auto selected = app.call("document.selectObject",
                                   {{"id", id}, {"baseRevision", "0"}, {"kind", "function"}});
    CHECK(selected["selections"][0]["anchor"] == 0);
    CHECK(selected["selections"][0]["head"] == source.size() - 1);
    const auto offset = source.find("42");
    static_cast<void>(app.call(
        "document.edit",
        {{"id", id},
         {"baseRevision", "0"},
         {"edits", Json::array({{{"begin", offset}, {"end", offset + 2}, {"text", "99"}}})}}));
    CHECK(
        app.response("document.selectObject",
                     {{"id", id}, {"baseRevision", "1"}, {"kind", "function"}})["error"]["code"] ==
        -32014);
    syntax = app.call("document.syntax", {{"id", id}, {"baseRevision", "1"}});
    CHECK(app.wait(syntax)["state"] == "completed");
    syntax = app.call("document.syntax",
                      {{"id", id}, {"baseRevision", "1"}, {"begin", offset}, {"end", offset + 2}});
    REQUIRE(syntax["state"] == "ready");
    CHECK(syntax["incremental"].get<bool>());
    CHECK(syntax["highlights"].size() == 1);
    CHECK(syntax["highlights"][0]["kind"] == "number");
}

TEST_CASE("application syntax pages indexed outlines beyond the default result limit") {
    AppFiles files;
    std::string source;
    for (int i = 0; i < 1105; ++i)
        source += "int function" + std::to_string(i) + "() { return " + std::to_string(i) + "; }\n";
    files.write("large.cpp", source);
    LiveApp app(files.root());
    const auto document = app.done("workspace.open", {{"path", "large.cpp"}});
    const auto id = document.at("id");
    const auto started =
        app.call("document.syntax", {{"id", id}, {"baseRevision", "0"}, {"begin", 0}, {"end", 0}});
    CHECK(app.wait(started).at("state") == "completed");
    const auto initial =
        app.call("document.syntax", {{"id", id}, {"baseRevision", "0"}, {"begin", 0}, {"end", 0}});
    REQUIRE(initial.at("state") == "ready");
    CHECK(initial.at("outline").size() == 1000);
    CHECK(initial.at("outlinePage").at("total") == 1105);
    const auto page = app.call("document.syntax",
                               {{"id", id},
                                {"baseRevision", "0"},
                                {"begin", 0},
                                {"end", 0},
                                {"outlineBegin", 1099},
                                {"outlineLimit", 6}});
    REQUIRE(page.at("outline").size() == 6);
    CHECK(page.at("outlinePage").at("offset") == 1099);
    CHECK(page.at("outlinePage").at("nextOffset").is_null());
    CHECK(page.at("outline").front().at("name") == "function1099");
    CHECK(page.at("outline").back().at("name") == "function1104");
    CHECK(page.at("highlights").empty());
    const auto hidden = app.call(
        "document.syntax",
        {{"id", id}, {"baseRevision", "0"}, {"includeOutline", false}, {"begin", 0}, {"end", 0}});
    CHECK(hidden.at("outline").empty());
    CHECK(hidden.at("outlinePage").at("total") == 1105);
    const auto end = app.call("document.syntax",
                              {{"id", id},
                               {"baseRevision", "0"},
                               {"outlineBegin", 2000},
                               {"outlineLimit", 10},
                               {"begin", 0},
                               {"end", 0}});
    CHECK(end.at("outline").empty());
    CHECK(end.at("outlinePage").at("offset") == 1105);
    CHECK(
        app.response("document.syntax", {{"id", id}, {"baseRevision", "0"}, {"outlineLimit", 1001}})
            .at("error")
            .at("code") == -32602);
    CHECK(app.response("document.syntax", {{"id", id}, {"baseRevision", "0"}, {"outlineBegin", -1}})
              .at("error")
              .at("code") == -32602);
    CHECK(app.response("document.syntax",
                       {{"id", id}, {"baseRevision", "0"}, {"includeOutline", "no"}})
              .at("error")
              .at("code") == -32602);
}

TEST_CASE("application syntax serves Markdown pages and inline objects after local edits") {
    AppFiles files;
    std::string source;
    for (int i = 0; i < 80; ++i)
        source += "# Heading " + std::to_string(i) + "\n\nParagraph `old` and **bold 中🙂**.\n\n";
    files.write("document.md", source);
    LiveApp app(files.root());
    const auto document = app.done("workspace.open", {{"path", "document.md"}});
    const auto id = document.at("id");
    auto syntax = app.call("document.syntax", {{"id", id}, {"baseRevision", "0"}});
    REQUIRE(app.wait(syntax).at("state") == "completed");
    const auto begin = source.find("old", source.find("Heading 40\n"));
    static_cast<void>(app.call(
        "document.edit",
        {{"id", id},
         {"baseRevision", "0"},
         {"edits", Json::array({{{"begin", begin}, {"end", begin + 3}, {"text", "new"}}})}}));
    syntax = app.call("document.syntax", {{"id", id}, {"baseRevision", "1"}});
    REQUIRE(app.wait(syntax).at("state") == "completed");
    syntax = app.call("document.syntax",
                      {{"id", id},
                       {"baseRevision", "1"},
                       {"begin", begin},
                       {"end", begin + 3},
                       {"outlineBegin", 75},
                       {"outlineLimit", 5}});
    REQUIRE(syntax.at("state") == "ready");
    CHECK(syntax.at("language") == "markdown");
    CHECK(syntax.at("incremental").get<bool>());
    REQUIRE(syntax.at("highlights").size() == 1);
    CHECK(syntax.at("highlights").front().at("kind") == "string");
    REQUIRE(syntax.at("outline").size() == 5);
    CHECK(syntax.at("outline").front().at("name") == "Heading 75");
    CHECK(syntax.at("outline").back().at("name") == "Heading 79");
    CHECK(syntax.at("outlinePage").at("total") == 80);
    static_cast<void>(
        app.call("document.setSelections",
                 {{"id", id},
                  {"baseRevision", "1"},
                  {"selections", Json::array({{{"anchor", begin}, {"head", begin}}})}}));
    const auto selection =
        app.call("document.selectObject", {{"id", id}, {"baseRevision", "1"}, {"kind", "string"}});
    CHECK(selection.at("selections").front().at("anchor") == begin - 1);
    CHECK(selection.at("selections").front().at("head") == begin + 4);
}

TEST_CASE("application editing policy is shared by direct and scripted commands") {
    AppFiles files;
    files.write(".editorconfig",
                "root=true\n[*.py]\nindent_style=space\nindent_size=2\nend_of_line=crlf\n");
    files.write("main.py", "pass\r\n");
    const auto defaults = astra::io::read_file(fs::current_path() / "data" / "defaults.js").text;
    LiveApp app(
        files.root(), defaults, "astra.configure({editor:{tabSize:8,insertSpaces:false}});");
    const auto doc = app.done("workspace.open", {{"path", "main.py"}});
    const auto id = doc["id"];
    auto result =
        app.call("document.command", {{"id", id}, {"baseRevision", "0"}, {"command", "indent"}});
    CHECK(app.call("document.get", {{"id", id}})["text"] == "  pass\r\n");
    result = app.call("script.command",
                      {{"name", "editor.indent"},
                       {"params", {{"id", id}, {"baseRevision", result["revision"]}}}});
    CHECK(app.call("document.get", {{"id", id}})["text"] == "    pass\r\n");
    result = app.call(
        "document.command",
        {{"id", id}, {"baseRevision", result["revision"]}, {"command", "toggle_line_comment"}});
    CHECK(app.call("document.get", {{"id", id}})["text"] == "    # pass\r\n");
    result = app.call("document.command",
                      {{"id", id},
                       {"baseRevision", result["revision"]},
                       {"command", "indent"},
                       {"insertSpaces", false}});
    CHECK(app.call("document.get", {{"id", id}})["text"] == "\t    # pass\r\n");
    const auto plain = app.call("document.create", {{"text", "\xE4\xB8\xAD\n"}});
    static_cast<void>(app.call(
        "document.command", {{"id", plain["id"]}, {"baseRevision", "0"}, {"command", "indent"}}));
    CHECK(app.call("document.get", {{"id", plain["id"]}})["text"] == "\t\xE4\xB8\xAD\n");
}

TEST_CASE("application newline follows file policy and invalid settings cannot mutate text") {
    AppFiles files;
    files.write(".editorconfig", "root=true\n[*.txt]\nend_of_line=crlf\n");
    files.write("new.txt", "text");
    LiveApp app(files.root());
    const auto doc = app.done("workspace.open", {{"path", "new.txt"}});
    static_cast<void>(app.call("document.command",
                               {{"id", doc["id"]}, {"baseRevision", "0"}, {"command", "newline"}}));
    CHECK(app.call("document.get", {{"id", doc["id"]}})["text"] == "\r\ntext");
    LiveApp invalid(files.root(), "astra.configure({editor:{tabSize:-1}});");
    const auto fresh = invalid.call("document.create");
    CHECK(invalid.response("document.command",
                           {{"id", fresh["id"]},
                            {"baseRevision", "0"},
                            {"command", "indent"}})["error"]["code"] == -32020);
    CHECK(invalid.call("document.get", {{"id", fresh["id"]}})["text"] == "");
}

TEST_CASE("application syntax discards stale worker results and coalesces identical requests") {
    AppFiles files;
    PausedWork app(files.root());
    const auto document = app.call("document.create", {{"text", "{\"number\": 1}"}});
    const Json params{{"id", document["id"]}, {"baseRevision", "0"}, {"language", "json"}};
    const auto syntax = app.call("document.syntax", params);
    CHECK(app.call("document.syntax", params)["operationId"] == syntax["operationId"]);
    app.run_worker();
    static_cast<void>(app.call("document.insert",
                               {{"id", document["id"]}, {"baseRevision", "0"}, {"text", " "}}));
    app.complete();
    const auto completed = app.call("operation.get", {{"id", syntax["operationId"]}});
    CHECK(completed["result"]["state"] == "obsolete");
    const auto new_syntax = app.call(
        "document.syntax", {{"id", document["id"]}, {"baseRevision", "1"}, {"language", "json"}});
    CHECK(new_syntax["operationId"] != syntax["operationId"]);
    app.run_worker();
    app.complete();
    CHECK(
        app.call("document.syntax",
                 {{"id", document["id"]}, {"baseRevision", "1"}, {"language", "json"}})["state"] ==
        "ready");
}

TEST_CASE("application Git stages commits and guards discarded previews against external writes") {
    AppFiles files;
    files.initialize_git();
    files.write("main.cpp", "int answer = 42;\n");
    LiveApp app(
        files.root(),
        {},
        "astra.configure({git:{authorName:'Astra Tests',authorEmail:'tests@astra.invalid'}});");
    auto status = app.done("git.status");
    CHECK(status["unborn"].get<bool>());
    static_cast<void>(app.done("git.stage", {{"paths", Json::array({"main.cpp"})}}));
    status = app.done("git.commit", {{"message", "initial fixture"}});
    CHECK_FALSE(status["unborn"].get<bool>());
    CHECK(status["entries"].empty());
    CHECK(app.done("git.log")["commits"][0]["subject"] == "initial fixture");
    const auto document = app.done("workspace.open", {{"path", "main.cpp"}});
    files.write("main.cpp", "int answer = 99;\n");
    auto preview = app.done("git.diff", {{"path", "main.cpp"}});
    REQUIRE(preview.contains("previewId"));
    CHECK_FALSE(preview["gutter"].empty());
    files.write("main.cpp", "int answer = 100;\n");
    const auto failed = app.wait(app.call("git.discard", {{"previewId", preview["previewId"]}}));
    CHECK(failed["state"] == "failed");
    CHECK(files.read("main.cpp") == "int answer = 100;\n");
    preview = app.done("git.diff", {{"path", "main.cpp"}});
    static_cast<void>(app.done("git.discard", {{"previewId", preview["previewId"]}}));
    CHECK(files.read("main.cpp") == "int answer = 42;\n");
    CHECK_FALSE(app.call("document.get", {{"id", document["id"]}})["dirty"].get<bool>());
    CHECK(app.response("git.discard", {{"previewId", "expired"}})["error"]["code"] == -32002);
}

TEST_CASE("application Git branch switching and staging preserve unsaved documents") {
    AppFiles files;
    files.initialize_git();
    files.write("main.txt", "baseline\n");
    LiveApp app(
        files.root(),
        {},
        "astra.configure({git:{authorName:'Astra Tests',authorEmail:'tests@astra.invalid'}});");
    static_cast<void>(app.done("git.stage", {{"paths", Json::array({"main.txt"})}}));
    static_cast<void>(app.done("git.commit", {{"message", "baseline"}}));
    static_cast<void>(app.done("git.createBranch", {{"name", "feature"}}));
    const auto doc = app.done("workspace.open", {{"path", "main.txt"}});
    static_cast<void>(app.call("document.insert",
                               {{"id", doc["id"]}, {"baseRevision", "0"}, {"text", "unsaved "}}));
    CHECK(app.response("git.switchBranch", {{"name", "feature"}})["error"]["code"] == -32013);
    CHECK(app.response("git.stage", {{"paths", Json::array({"main.txt"})}})["error"]["code"] ==
          -32013);
    CHECK(app.call("document.get", {{"id", doc["id"]}})["text"] == "unsaved baseline\n");
    CHECK(files.read("main.txt") == "baseline\n");
    static_cast<void>(app.call("document.undo", {{"id", doc["id"]}, {"baseRevision", "1"}}));
    CHECK(app.done("git.switchBranch", {{"name", "feature"}})["branch"] == "feature");
    CHECK_FALSE(app.call("document.get", {{"id", doc["id"]}})["dirty"].get<bool>());
}

TEST_CASE("application textobject guards reject stale selections without any mutation") {
    AppFiles files;
    const std::string source = "int one() { return 1; }\nint two() { return 2; }\n";
    files.write("objects.cpp", source);
    LiveApp app(files.root());
    const auto document = app.done("workspace.open", {{"path", "objects.cpp"}});
    const auto id = document.at("id");
    const auto syntax = app.call("document.syntax", {{"id", id}, {"baseRevision", "0"}});
    CHECK(syntax.at("shared") == true);
    const auto second_waiter = app.call("document.syntax", {{"id", id}, {"baseRevision", "0"}});
    if (second_waiter.contains("operationId")) {
        CHECK(second_waiter.at("operationId") == syntax.at("operationId"));
        CHECK(second_waiter.at("shared") == true);
    } else {
        CHECK(second_waiter.at("state") == "ready");
    }
    CHECK(app.wait(syntax).at("state") == "completed");
    const auto first = source.find("return"), second = source.rfind("return");
    const auto captured = app.call("document.setSelections",
                                   {{"id", id},
                                    {"baseRevision", "0"},
                                    {"selections",
                                     Json::array({{{"anchor", first}, {"head", first}},
                                                  {{"anchor", second}, {"head", second}}})}});
    const auto changed = app.call("document.setSelections",
                                  {{"id", id},
                                   {"baseRevision", "0"},
                                   {"selections", Json::array({{{"anchor", 0}, {"head", 0}}})}});
    CHECK(changed.at("revision") == captured.at("revision"));
    const auto rejected = app.response("document.selectObject",
                                       {{"id", id},
                                        {"baseRevision", "0"},
                                        {"kind", "function"},
                                        {"baseSelections", captured.at("selections")}});
    CHECK(rejected.at("error").at("code") == -32001);
    const auto retained = app.call("document.get", {{"id", id}});
    CHECK(retained.at("text") == source);
    CHECK(retained.at("selections") == changed.at("selections"));
    CHECK(retained.at("revision") == changed.at("revision"));
    CHECK(retained.at("dirty") == changed.at("dirty"));
    for (const auto& invalid :
         std::vector<Json>{Json::array(),
                           Json::array({{{"anchor", -1}, {"head", 0}}}),
                           Json::array({{{"anchor", 0.5}, {"head", 0}}}),
                           Json::array({{{"anchor", 9007199254740992ULL}, {"head", 0}}}),
                           Json::array({{{"head", 0}}}),
                           Json::array({{{"anchor", 0}, {"head", 0}, {"extra", true}}})}) {
        CHECK(app.response("document.selectObject",
                           {{"id", id},
                            {"baseRevision", "0"},
                            {"kind", "function"},
                            {"baseSelections", invalid}})
                  .at("error")
                  .at("code") == -32602);
        CHECK(app.call("document.get", {{"id", id}}).at("selections") == changed.at("selections"));
    }
    static_cast<void>(
        app.call("document.setSelections",
                 {{"id", id}, {"baseRevision", "0"}, {"selections", captured.at("selections")}}));
    const auto selected = app.call("document.selectObject",
                                   {{"id", id},
                                    {"baseRevision", "0"},
                                    {"kind", "function"},
                                    {"baseSelections", captured.at("selections")}});
    REQUIRE(selected.at("selections").size() == 2);
    CHECK(selected.at("selections").at(0).at("anchor") == 0);
    CHECK(selected.at("selections").at(0).at("head") == source.find('\n'));
    CHECK(selected.at("selections").at(1).at("anchor") == source.find('\n') + 1);
    CHECK(selected.at("selections").at(1).at("head") == source.size() - 1);
    CHECK(app.call("document.get", {{"id", id}}).at("text") == source);
}
