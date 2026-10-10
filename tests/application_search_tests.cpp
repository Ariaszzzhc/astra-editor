#include <astra/service/application_service.hpp>
#include <astra/service/service_host.hpp>
#include <astra/process/process.hpp>
#include "support/temporary_directory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>
#endif

namespace {
namespace fs = std::filesystem;
using astra::protocol::Json;
using namespace std::chrono_literals;

struct SearchFiles {
    fs::path base = fs::canonical(fs::current_path() / "build");
    fs::path root = base / ("search-app-" + std::to_string(std::random_device{}()) + "-" +
                            std::to_string(std::random_device{}()));

    SearchFiles() {
        fs::create_directories(root);
    }

    ~SearchFiles() {
        for (const auto& issue :
             astra::test::cleanup_owned_directory(root, base, "search-app-", 1s)) {
            CHECK_MESSAGE(false, issue.message);
        }
    }

    void write(std::string_view path, std::string_view text) const {
        std::ofstream stream(root / path, std::ios::binary);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream << text;
    }

    std::string read(std::string_view path) const {
        return astra::io::read_file(root / path).text;
    }

    void initialize_git() const {
        astra::process::ProcessOptions options;
        options.argv = {"git", "init", "--initial-branch=main"};
        const auto bytes = root.generic_u8string();
        options.cwd.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        options.shutdown_timeout = 10s;
        options.env = astra::process::current_environment();
        std::erase_if(*options.env, [](const std::string& entry) {
            return entry.starts_with("GIT_");
        });
#if defined(_WIN32)
        constexpr auto null_file = "NUL";
#else
        constexpr auto null_file = "/dev/null";
#endif
        options.env->insert(options.env->end(),
                            {"GIT_CONFIG_NOSYSTEM=1",
                             "GIT_TERMINAL_PROMPT=0",
                             std::string("GIT_CONFIG_GLOBAL=") + null_file,
                             std::string("GIT_CONFIG_SYSTEM=") + null_file});
        astra::process::Process child(std::move(options));
        auto started = child.start();
        REQUIRE(started.wait_for(10s) == std::future_status::ready);
        started.get();
        child.close();
        const auto completed = child.completion();
        REQUIRE(completed.wait_for(15s) == std::future_status::ready);
        const auto result = completed.get();
        REQUIRE(result.error.empty());
        REQUIRE(result.exit_status == 0);
    }
};

Json request(std::string method, Json params) {
    return {{"jsonrpc", "2.0"},
            {"id", "search-test"},
            {"method", std::move(method)},
            {"params", std::move(params)}};
}

Json checked(const Json& response) {
    INFO(response.dump());
    REQUIRE_FALSE(response.contains("error"));
    return response.at("result");
}

class SearchHost {
  public:
    explicit SearchHost(const fs::path& root) : host(make_options(root)) {
    }

    static astra::service::ServiceHostOptions make_options(const fs::path& root) {
        astra::service::ServiceHostOptions options;
        options.workspace = root;
        options.watch_files = false;
        return options;
    }

    Json response(std::string method, Json params = Json::object()) {
        auto future = host.submit(request(std::move(method), std::move(params)).dump());
        REQUIRE(future.wait_for(10s) == std::future_status::ready);
        const auto body = future.get();
        REQUIRE(body);
        return Json::parse(*body);
    }

    Json call(std::string method, Json params = Json::object()) {
        return checked(response(std::move(method), std::move(params)));
    }

    Json wait(const Json& operation) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = call("operation.get", {{"id", operation.at("operationId")}});
            if (result.at("state") != "pending") {
                return result;
            }
            std::this_thread::sleep_for(1ms);
        }
        FAIL("Search operation did not complete");
        return {};
    }

    Json done(std::string method, Json params = Json::object()) {
        const auto completed = wait(call(std::move(method), std::move(params)));
        INFO(completed.dump());
        REQUIRE(completed.at("state") == "completed");
        return completed.at("result");
    }

    astra::service::ServiceHost host;
};

class PausedSearch {
    bool inline_next_{};

  public:
    explicit PausedSearch(const fs::path& root, std::string_view commands = {})
        : app(
              root,
              [this](auto worker, auto completion) {
                  auto task = std::make_unique<Task>();
                  task->worker = std::move(worker);
                  task->completion = std::move(completion);
                  auto future = task->promise.get_future();
                  if (std::exchange(inline_next_, false)) {
                      try {
                          task->worker();
                      } catch (...) {
                          task->error = std::current_exception();
                      }
                      task->completion(task->error);
                      if (task->error) {
                          task->promise.set_exception(task->error);
                      } else {
                          task->promise.set_value();
                      }
                      return future;
                  }
                  tasks.push_back(std::move(task));
                  return future;
              },
              "astra.configure({git:{inheritGlobalConfig:false}});",
              commands) {
    }

    Json call(std::string method, Json params = Json::object()) {
        const auto result = app.dispatch(request(std::move(method), std::move(params)));
        REQUIRE(result);
        return checked(*result);
    }

    void worker(std::size_t index = 0) {
        REQUIRE(index < tasks.size());
        try {
            tasks[index]->worker();
        } catch (...) {
            tasks[index]->error = std::current_exception();
        }
    }

    void complete(std::size_t index = 0) {
        REQUIRE(index < tasks.size());
        auto task = std::move(tasks[index]);
        tasks.erase(tasks.begin() + static_cast<std::ptrdiff_t>(index));
        task->completion(task->error);
        if (task->error) {
            task->promise.set_exception(task->error);
        } else {
            task->promise.set_value();
        }
    }

    Json finish(const Json& operation) {
        worker();
        complete();
        return call("operation.get", {{"id", operation.at("operationId")}});
    }

    void run_next_inline() {
        inline_next_ = true;
    }

    astra::service::ApplicationService app;

  private:
    struct Task {
        std::function<void()> worker;
        std::function<void(std::exception_ptr)> completion;
        std::promise<void> promise;
        std::exception_ptr error;
    };

    std::vector<std::unique_ptr<Task>> tasks;
};

TEST_CASE("application operation reclamation retains recent results across decimal identities") {
    SearchFiles files;
    PausedSearch service(files.root, "astra.registerCommand('fast',()=>42);");
    const auto doc = service.call("document.create", {{"text", "needle"}});
    const Json parameters{
        {"id", doc.at("id")}, {"baseRevision", "0"}, {"query", "needle"}, {"select", false}};
    const auto held = service.call("document.find", parameters);
    std::vector<Json> completed;
    for (int i = 0; i < 150; ++i) {
        const auto operation = service.call("document.find", parameters);
        service.worker(1);
        service.complete(1);
        completed.push_back(operation.at("operationId"));
        const auto first = completed.size() > 31 ? completed.size() - 31 : 0;
        for (auto index = first; index < completed.size(); ++index) {
            const auto result = service.call("operation.get", {{"id", completed[index]}});
            CHECK(result.at("state") == "completed");
            CHECK(result.at("result").at("match").at("begin") == 0);
        }
        CHECK(service.call("operation.get", {{"id", held.at("operationId")}}).at("state") ==
              "pending");
        if (i >= 31) {
            const auto old =
                service.app.dispatch(request("operation.get", {{"id", completed[first - 1]}}));
            REQUIRE(old);
            CHECK(old->at("error").at("code") == -32002);
        }
        CHECK(service.call("script.command", {{"name", "fast"}}) == 42);
        CHECK(service.call("operation.get", {{"id", completed.back()}}).at("state") == "completed");
    }
    CHECK(service.finish(held).at("state") == "completed");
}

TEST_CASE("document search defers computation and atomically selects Unicode matches") {
    SearchFiles files;
    PausedSearch service(files.root);
    const auto doc = service.call("document.create", {{"text", "中🙂 cat\nCAT"}});
    const auto id = doc.at("id");
    const auto pending = service.call("document.find",
                                      {{"id", id},
                                       {"baseRevision", "0"},
                                       {"query", "cat"},
                                       {"begin", 11},
                                       {"caseSensitive", false},
                                       {"wholeWord", true},
                                       {"select", true}});
    CHECK(service.call("operation.get", {{"id", pending.at("operationId")}}).at("state") ==
          "pending");
    CHECK(service.call("document.get", {{"id", id}}).at("selections") == doc.at("selections"));
    const auto completed = service.finish(pending);
    REQUIRE(completed.at("state") == "completed");
    CHECK(completed["result"]["match"]["begin"] == 12);
    CHECK(completed["result"]["match"]["line"] == 1);
    CHECK(completed["result"]["wrapped"] == false);
    const auto selected = service.call("document.get", {{"id", id}});
    CHECK(selected.at("selections") == Json::array({{{"anchor", 12}, {"head", 15}}}));
    CHECK(selected.at("revision") == "0");
    CHECK_FALSE(selected.at("canUndo").get<bool>());
    const auto wrapped = service.finish(service.call(
        "document.find",
        {{"id", id}, {"baseRevision", "0"}, {"query", "cat"}, {"caseSensitive", false}}));
    REQUIRE(wrapped.at("state") == "completed");
    CHECK(wrapped["result"]["match"]["begin"] == 8);
    CHECK(wrapped["result"]["wrapped"] == true);
    const auto absent = service.finish(
        service.call("document.find",
                     {{"id", id}, {"baseRevision", "0"}, {"query", "missing"}, {"select", true}}));
    REQUIRE(absent.at("state") == "completed");
    CHECK(absent["result"]["match"].is_null());
    CHECK(service.call("document.get", {{"id", id}}).at("selections") == selected.at("selections"));
}

TEST_CASE("document replacement is one Unicode transaction with selection history") {
    SearchFiles files;
    SearchHost service(files.root);
    auto doc = service.call("document.create", {{"text", "中 cat\nCAT 🙂"}});
    const auto id = doc.at("id");
    doc = service.call(
        "document.setSelections",
        {{"id", id},
         {"baseRevision", "0"},
         {"selections",
          Json::array({{{"anchor", 4}, {"head", 7}}, {{"anchor", 11}, {"head", 16}}})}});
    const auto result = service.done("document.replaceAll",
                                     {{"id", id},
                                      {"baseRevision", "0"},
                                      {"query", "(cat)"},
                                      {"replacement", "[$1]"},
                                      {"regex", true},
                                      {"caseSensitive", false}});
    CHECK(result.at("replacements") == 2);
    CHECK(result.at("changed") == true);
    const auto replaced = service.call("document.get", {{"id", id}});
    CHECK(replaced.at("text") == "中 [cat]\n[CAT] 🙂");
    CHECK(replaced.at("revision") == "1");
    CHECK(replaced.at("dirty") == true);
    service.call("document.undo", {{"id", id}, {"baseRevision", "1"}});
    const auto undone = service.call("document.get", {{"id", id}});
    CHECK(undone.at("text") == "中 cat\nCAT 🙂");
    CHECK(undone.at("selections") == doc.at("selections"));
    CHECK(undone.at("dirty") == false);
    CHECK(undone.at("canUndo") == false);
    service.call("document.redo", {{"id", id}, {"baseRevision", "2"}});
    CHECK(service.call("document.get", {{"id", id}}).at("selections") == replaced.at("selections"));
}

TEST_CASE("document search and replacement reject changes before publication") {
    for (const auto replace : {false, true})
        for (const auto change : {"edit", "selections", "close", "cancel"}) {
            CAPTURE(replace);
            CAPTURE(change);
            SearchFiles files;
            PausedSearch service(files.root);
            const auto doc = service.call("document.create", {{"text", "cat cat"}}),
                       id = doc.at("id");
            Json params{{"id", id}, {"baseRevision", "0"}, {"query", "cat"}};
            if (replace)
                params["replacement"] = "dog";
            else
                params["select"] = true;
            const auto operation =
                service.call(replace ? "document.replaceAll" : "document.find", params);
            service.worker();
            if (std::string_view(change) == "edit")
                service.call("document.insert", {{"id", id}, {"baseRevision", "0"}, {"text", "X"}});
            else if (std::string_view(change) == "selections")
                service.call("document.setSelections",
                             {{"id", id},
                              {"baseRevision", "0"},
                              {"selections", Json::array({{{"anchor", 4}, {"head", 4}}})}});
            else if (std::string_view(change) == "close")
                service.call("document.close", {{"id", id}, {"baseRevision", "0"}});
            else
                service.call("operation.cancel", {{"id", operation.at("operationId")}});
            Json before;
            if (std::string_view(change) != "close")
                before = service.call("document.get", {{"id", id}});
            service.complete();
            const auto result =
                service.call("operation.get", {{"id", operation.at("operationId")}});
            CHECK(result.at("state") ==
                  (std::string_view(change) == "cancel" ? "cancelled" : "failed"));
            if (std::string_view(change) != "close")
                CHECK(service.call("document.get", {{"id", id}}) == before);
            else
                CHECK(service.call("document.list").empty());
        }
}

TEST_CASE("document replacement limits and invalid expressions preserve state") {
    SearchFiles files;
    PausedSearch service(files.root);
    for (const auto kind : {"limit", "regex", "capture", "utf8", "noop", "empty"}) {
        CAPTURE(kind);
        const auto text =
            std::string_view(kind) == "limit" ? std::string(4097, 'a') : std::string("a🙂a");
        const auto doc = service.call("document.create", {{"text", text}}), id = doc.at("id");
        const auto before = service.call("document.get", {{"id", id}});
        Json params{{"id", id}, {"baseRevision", "0"}, {"query", "a"}, {"replacement", "b"}};
        if (std::string_view(kind) == "regex") {
            params["regex"] = true;
            params["query"] = "(";
        }
        if (std::string_view(kind) == "capture") {
            params["regex"] = true;
            params["replacement"] = "$1";
        }
        if (std::string_view(kind) == "utf8")
            params["replacement"] = std::string("\xFF");
        if (std::string_view(kind) == "noop")
            params["replacement"] = "a";
        if (std::string_view(kind) == "empty") {
            params["regex"] = true;
            params["query"] = "^|$";
            params["replacement"] = "!";
        }
        if (std::string_view(kind) == "utf8") {
            const auto response = service.app.dispatch(request("document.replaceAll", params));
            REQUIRE(response);
            REQUIRE(response->contains("error"));
            CHECK((*response)["error"]["code"] == -32600);
            CHECK(service.call("document.get", {{"id", id}}) == before);
            service.call("document.close", {{"id", id}, {"baseRevision", "0"}});
            continue;
        }
        const auto result = service.finish(service.call("document.replaceAll", params));
        if (std::string_view(kind) == "empty") {
            REQUIRE(result.at("state") == "completed");
            CHECK(result["result"]["replacements"] == 2);
            CHECK(service.call("document.get", {{"id", id}}).at("text") == "!a🙂a!");
        } else {
            CHECK(result.at("state") ==
                  (std::string_view(kind) == "noop" ? "completed" : "failed"));
            CHECK(service.call("document.get", {{"id", id}}) == before);
        }
        service.call("document.close",
                     {{"id", id},
                      {"baseRevision", std::string_view(kind) == "empty" ? "1" : "0"},
                      {"discardChanges", true}});
    }
}

TEST_CASE("document search validates offsets revisions and runs with inline completion") {
    SearchFiles files;
    PausedSearch service(files.root);
    const auto doc = service.call("document.create", {{"text", "中🙂a"}}), id = doc.at("id");
    for (const auto& invalid : std::vector<Json>{Json{{"begin", 1}},
                                                 Json{{"begin", -1}},
                                                 Json{{"begin", 1.5}},
                                                 Json{{"begin", 9}},
                                                 Json{{"wrap", "yes"}},
                                                 Json{{"select", 1}},
                                                 Json{{"direction", "sideways"}},
                                                 Json{{"direction", 2}},
                                                 Json{{"other", true}}}) {
        Json params{{"id", id}, {"baseRevision", "0"}, {"query", "a"}};
        params.update(invalid);
        const auto response = service.app.dispatch(request("document.find", params));
        REQUIRE(response);
        REQUIRE(response->contains("error"));
        CHECK((*response)["error"]["code"] == -32602);
    }
    const auto stale = service.app.dispatch(
        request("document.replaceAll",
                {{"id", id}, {"baseRevision", "1"}, {"query", "a"}, {"replacement", "b"}}));
    REQUIRE(stale);
    CHECK((*stale)["error"]["code"] == -32001);
    service.run_next_inline();
    const auto result = service.call(
        "operation.get",
        {{"id",
          service.call("document.find", {{"id", id}, {"baseRevision", "0"}, {"query", "a"}})
              .at("operationId")}});
    REQUIRE(result.at("state") == "completed");
    CHECK(result["result"]["match"]["begin"] == 7);
}

TEST_CASE("document previous search starts at selection beginning and wraps in UTF8 coordinates") {
    SearchFiles files;
    SearchHost service(files.root);
    const auto doc = service.call("document.create", {{"text", "中 cat cat 🙂"}}),
               id = doc.at("id");
    service.call("document.setSelections",
                 {{"id", id},
                  {"baseRevision", "0"},
                  {"selections", Json::array({{{"anchor", 8}, {"head", 11}}})}});
    auto found = service.done("document.find",
                              {{"id", id},
                               {"baseRevision", "0"},
                               {"query", "cat"},
                               {"direction", "backward"},
                               {"select", true}});
    REQUIRE(found.at("match").is_object());
    CHECK(found["match"]["begin"] == 4);
    CHECK_FALSE(found["wrapped"].get<bool>());
    found = service.done("document.find",
                         {{"id", id},
                          {"baseRevision", "0"},
                          {"query", "cat"},
                          {"direction", "backward"},
                          {"select", true}});
    CHECK(found["match"]["begin"] == 8);
    CHECK(found["wrapped"].get<bool>());
    const auto after = service.call("document.get", {{"id", id}});
    CHECK(after.at("revision") == "0");
    CHECK_FALSE(after.at("canUndo").get<bool>());
}

TEST_CASE("document replacement cannot overwrite a concurrent save baseline") {
    SearchFiles files;
    PausedSearch service(files.root);
    auto doc = service.call("document.create", {{"text", "cat"}}), id = doc.at("id");
    doc = service.call("document.insert", {{"id", id}, {"baseRevision", "0"}, {"text", "cat "}});
    REQUIRE(doc.at("dirty").get<bool>());
    const auto replace =
        service.call("document.replaceAll",
                     {{"id", id}, {"baseRevision", "1"}, {"query", "cat"}, {"replacement", "dog"}});
    service.worker();
    const auto save =
        service.call("workspace.save", {{"id", id}, {"baseRevision", "1"}, {"path", "saved.txt"}});
    service.worker(1);
    service.complete(1);
    REQUIRE(service.call("operation.get", {{"id", save.at("operationId")}}).at("state") ==
            "completed");
    const auto saved = service.call("document.get", {{"id", id}});
    REQUIRE(saved.at("dirty") == false);
    service.complete();
    const auto result = service.call("operation.get", {{"id", replace.at("operationId")}});
    CHECK(result.at("state") == "failed");
    CHECK(result["error"]["code"] == -32001);
    CHECK(service.call("document.get", {{"id", id}}) == saved);
    CHECK(files.read("saved.txt") == "cat cat");
}

TEST_CASE("document search cancellation before worker and pure queries preserve selections") {
    SearchFiles files;
    PausedSearch service(files.root);
    const auto doc = service.call("document.create", {{"text", "cat"}}), id = doc.at("id");
    const auto cancelled = service.call(
        "document.find", {{"id", id}, {"baseRevision", "0"}, {"query", "cat"}, {"select", true}});
    service.call("operation.cancel", {{"id", cancelled.at("operationId")}});
    CHECK(service.finish(cancelled).at("state") == "cancelled");
    const auto query =
        service.call("document.find", {{"id", id}, {"baseRevision", "0"}, {"query", "cat"}});
    service.worker();
    const auto selected =
        service.call("document.setSelections",
                     {{"id", id},
                      {"baseRevision", "0"},
                      {"selections", Json::array({{{"anchor", 3}, {"head", 3}}})}});
    service.complete();
    const auto result = service.call("operation.get", {{"id", query.at("operationId")}});
    CHECK(result.at("state") == "completed");
    CHECK(result["result"]["match"]["begin"] == 0);
    CHECK(service.call("document.get", {{"id", id}}).at("selections") == selected.at("selections"));
}

#if defined(_WIN32)
class SearchJunction {
  public:
    SearchJunction(fs::path link, const fs::path& target) : link_(std::move(link)) {
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
            throw std::length_error("Search junction test path is too long");
        }
        buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
        buffer.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
        buffer.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
        buffer.print_length = static_cast<WORD>(print_name.size() * sizeof(wchar_t));
        buffer.data_length =
            static_cast<WORD>(8 + buffer.print_offset + buffer.print_length + sizeof(wchar_t));
        std::ranges::copy(substitute, buffer.path.begin());
        std::ranges::copy(print_name,
                          buffer.path.begin() + static_cast<std::ptrdiff_t>(substitute.size() + 1));
        if (!CreateDirectoryW(link_.c_str(), nullptr)) {
            throw std::runtime_error("Cannot create search junction directory");
        }
        const auto handle = CreateFileW(link_.c_str(),
                                        GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (!RemoveDirectoryW(link_.c_str())) {
                std::fputs("Search junction constructor cleanup failed\n", stderr);
            }
            throw std::system_error(
                static_cast<int>(error), std::system_category(), "Open search junction");
        }
        DWORD returned{};
        const auto succeeded = DeviceIoControl(handle,
                                               FSCTL_SET_REPARSE_POINT,
                                               &buffer,
                                               static_cast<DWORD>(buffer.data_length) + 8,
                                               nullptr,
                                               0,
                                               &returned,
                                               nullptr);
        const auto error = GetLastError();
        if (!CloseHandle(handle)) {
            std::fputs("Search junction handle close failed\n", stderr);
        }
        if (!succeeded) {
            if (!RemoveDirectoryW(link_.c_str())) {
                std::fputs("Search junction constructor cleanup failed\n", stderr);
            }
            throw std::system_error(
                static_cast<int>(error), std::system_category(), "Create search junction");
        }
    }

    SearchJunction(const SearchJunction&) = delete;
    SearchJunction& operator=(const SearchJunction&) = delete;

    ~SearchJunction() {
        if (!RemoveDirectoryW(link_.c_str())) {
            std::fputs("Search junction cleanup failed\n", stderr);
        }
    }

  private:
    fs::path link_;
};
#endif
} // namespace

TEST_CASE(
    "application search overlays dirty and untitled buffers and replaces with independent undo") {
    SearchFiles files;
    files.write("open.txt", "needle old disk");
    files.write("closed.txt", "needle on disk");
    SearchHost host(files.root);
    const auto opened = host.done("workspace.open", {{"path", "open.txt"}});
    const auto dirty = host.call(
        "document.edit",
        {{"id", opened["id"]},
         {"baseRevision", "0"},
         {"edits", Json::array({{{"begin", 0}, {"end", 15}, {"text", "NEEDLE unsaved"}}})}});
    const auto untitled = host.call("document.create", {{"text", "needle untitled"}});
    const auto search =
        host.call("workspace.search", {{"query", "needle"}, {"caseSensitive", false}});
    const auto found = host.wait(search);
    REQUIRE(found["state"] == "completed");
    const auto& matches = found["result"]["matches"];
    CHECK(matches.size() == 3);
    CHECK(std::count_if(matches.begin(), matches.end(), [](const auto& match) {
              return match.value("source", "") == "buffer";
          }) == 2);
    const auto replaced = host.done(
        "workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "replacement"}});
    CHECK(replaced["applied"].size() == 1);
    CHECK(replaced["modifiedBuffers"].size() == 2);
    CHECK(replaced["pendingBuffers"].empty());
    CHECK(files.read("open.txt") == "needle old disk");
    CHECK(files.read("closed.txt") == "replacement on disk");
    const auto current = host.call("document.get", {{"id", dirty["id"]}});
    CHECK(current["text"] == "replacement unsaved");
    CHECK(current["dirty"] == true);
    CHECK(host.call("document.get", {{"id", untitled["id"]}})["text"] == "replacement untitled");
    static_cast<void>(
        host.call("document.undo", {{"id", current["id"]}, {"baseRevision", current["revision"]}}));
    CHECK(host.call("document.get", {{"id", dirty["id"]}})["text"] == "NEEDLE unsaved");
    const auto disk = host.done("workspace.search", {{"query", "needle"}, {"scope", "disk"}});
    CHECK(disk["scope"] == "disk");
    CHECK(disk["matches"].size() == 1);
}

TEST_CASE("application buffer regex replacement supports captures and adjacent empty matches") {
    SearchFiles files;
    SearchHost host(files.root);
    const auto document = host.call("document.create", {{"text", "aa"}});
    const auto search = host.call("workspace.search", {{"query", "(a*)"}, {"regex", true}});
    REQUIRE(host.wait(search)["state"] == "completed");
    const auto replaced = host.done("workspace.replace",
                                    {{"searchId", search["operationId"]}, {"replacement", "<$1>"}});
    REQUIRE(replaced["modifiedBuffers"].size() == 1);
    CHECK(host.call("document.get", {{"id", document["id"]}})["text"] == "<aa><>");
    CHECK(fs::is_empty(files.root));
}

TEST_CASE("application rejects stale buffer search before any disk replacement") {
    SearchFiles files;
    files.write("closed.txt", "target");
    SearchHost host(files.root);
    const auto document = host.call("document.create", {{"text", "target"}});
    const auto search = host.call("workspace.search", {{"query", "target"}});
    REQUIRE(host.wait(search)["state"] == "completed");
    static_cast<void>(host.call("document.insert",
                                {{"id", document["id"]}, {"baseRevision", "0"}, {"text", "new "}}));
    CHECK(host.response("workspace.replace",
                        {{"searchId", search["operationId"]}, {"replacement", "done"}})
              .contains("error"));
    CHECK(files.read("closed.txt") == "target");
}

TEST_CASE("application replacement reports buffers edited while worker finished without "
          "overwriting them") {
    SearchFiles files;
    files.write("closed.txt", "target");
    PausedSearch host(files.root);
    const auto document = host.call("document.create", {{"text", "target"}});
    const auto search = host.call("workspace.search", {{"query", "target"}});
    REQUIRE(host.finish(search)["state"] == "completed");
    const auto replacement = host.call(
        "workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "done"}});
    host.worker();
    static_cast<void>(host.call(
        "document.insert", {{"id", document["id"]}, {"baseRevision", "0"}, {"text", "keep "}}));
    host.complete();
    const auto result = host.call("operation.get", {{"id", replacement["operationId"]}});
    REQUIRE(result["state"] == "completed");
    CHECK(result["result"]["pendingBuffers"] == Json::array({document["id"]}));
    CHECK(result["result"]["applied"].size() == 1);
    CHECK(files.read("closed.txt") == "done");
    CHECK(host.call("document.get", {{"id", document["id"]}})["text"] == "keep target");
}

TEST_CASE("application replacement preserves a moved selection during asynchronous preparation") {
    SearchFiles files;
    PausedSearch host(files.root);
    const auto document = host.call("document.create", {{"text", "target"}});
    const auto search = host.call("workspace.search", {{"query", "target"}});
    REQUIRE(host.finish(search)["state"] == "completed");
    const auto replacement = host.call(
        "workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "done"}});
    host.worker();
    static_cast<void>(host.call("document.setSelections",
                                {{"id", document["id"]},
                                 {"baseRevision", "0"},
                                 {"selections", Json::array({{{"anchor", 6}, {"head", 6}}})}}));
    host.complete();
    const auto result = host.call("operation.get", {{"id", replacement["operationId"]}});
    CHECK(result["result"]["pendingBuffers"].size() == 1);
    const auto current = host.call("document.get", {{"id", document["id"]}});
    CHECK(current["text"] == "target");
    CHECK(current["selections"][0]["head"] == 6);
}

TEST_CASE("application open rejects old disk reads spanning a workspace replacement") {
    SearchFiles files;
    files.write("closed.txt", "target disk");
    PausedSearch host(files.root);
    const auto search = host.call("workspace.search", {{"query", "target"}});
    REQUIRE(host.finish(search)["state"] == "completed");
    const auto opening = host.call("workspace.open", {{"path", "closed.txt"}});
    host.worker(); // Holds the original read, but has not created a buffer.
    const auto replacement = host.call(
        "workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "done"}});
    host.worker(1); // Disk is changed while the old open result is still pending.
    REQUIRE(files.read("closed.txt") == "done disk");
    SUBCASE("The old open completes while replacement is still busy") {
        host.complete(0);
        host.complete(0);
    }
    SUBCASE("Replacement completes and clears busy before the old open returns") {
        host.complete(1);
        host.complete(0);
    }
    const auto rejected = host.call("operation.get", {{"id", opening["operationId"]}});
    REQUIRE(rejected["state"] == "failed");
    CHECK(rejected["error"]["code"] == -32013);
    CHECK(host.call("workspace.documents").empty());
    const auto replaced = host.call("operation.get", {{"id", replacement["operationId"]}});
    REQUIRE(replaced["state"] == "completed");
    CHECK(replaced["result"]["applied"] == Json::array({"closed.txt"}));
    const auto fresh = host.finish(host.call("workspace.open", {{"path", "closed.txt"}}));
    REQUIRE(fresh["state"] == "completed");
    const auto current = host.call("document.get", {{"id", fresh["result"]["id"]}});
    CHECK(current["text"] == "done disk");
    CHECK(current["dirty"] == false);
}

TEST_CASE("application open rejects old disk reads spanning Git discard") {
    SearchFiles files;
    files.initialize_git();
    files.write("tracked.txt", "index content");
    PausedSearch host(files.root);
    REQUIRE(host.finish(host.call(
                "git.stage", {{"paths", Json::array({"tracked.txt"})}}))["state"] == "completed");
    files.write("tracked.txt", "modified worktree");
    const auto diff = host.finish(host.call("git.diff", {{"path", "tracked.txt"}}));
    REQUIRE(diff["state"] == "completed");
    REQUIRE(diff["result"].contains("previewId"));
    const auto opening = host.call("workspace.open", {{"path", "tracked.txt"}});
    host.worker();
    const auto discard = host.call("git.discard", {{"previewId", diff["result"]["previewId"]}});
    host.worker(1);
    REQUIRE(files.read("tracked.txt") == "index content");
    SUBCASE("Old open completes while discard is still busy") {
        host.complete(0);
        host.complete(0);
    }
    SUBCASE("Discard clears its mutation flag before old open completes") {
        host.complete(1);
        host.complete(0);
    }
    const auto rejected = host.call("operation.get", {{"id", opening["operationId"]}});
    REQUIRE(rejected["state"] == "failed");
    CHECK(rejected["error"]["code"] == -32013);
    CHECK(host.call("workspace.documents").empty());
    CHECK(host.call("operation.get", {{"id", discard["operationId"]}})["state"] == "completed");
    const auto fresh = host.finish(host.call("workspace.open", {{"path", "tracked.txt"}}));
    REQUIRE(fresh["state"] == "completed");
    const auto current = host.call("document.get", {{"id", fresh["result"]["id"]}});
    CHECK(current["text"] == "index content");
    CHECK(current["dirty"] == false);
}

TEST_CASE("application inline Git scheduling invalidates stale opens only for mutations") {
    SearchFiles files;
    files.initialize_git();
    files.write("tracked.txt", "index content");
    PausedSearch host(files.root);
    REQUIRE(host.finish(host.call(
                "git.stage", {{"paths", Json::array({"tracked.txt"})}}))["state"] == "completed");
    files.write("tracked.txt", "modified worktree");
    const auto preview = host.finish(host.call("git.diff", {{"path", "tracked.txt"}}));
    REQUIRE(preview["state"] == "completed");
    REQUIRE(preview["result"].contains("previewId"));

    const auto opening = host.call("workspace.open", {{"path", "tracked.txt"}});
    host.worker(); // Retain bytes read before the selectively inline Git task.
    std::string method;
    Json params = Json::object();
    bool mutation = false;
    SUBCASE("Discard completes inside start_operation before the request returns") {
        method = "git.discard";
        params = {{"previewId", preview["result"]["previewId"]}};
        mutation = true;
    }
    SUBCASE("Inline status does not invalidate a pending open") {
        method = "git.status";
    }
    SUBCASE("Inline diff does not invalidate a pending open") {
        method = "git.diff";
        params = {{"path", "tracked.txt"}};
    }
    host.run_next_inline();
    const auto operation = host.call(method, params);
    REQUIRE(host.call("operation.get", {{"id", operation["operationId"]}})["state"] == "completed");
    CHECK(files.read("tracked.txt") == (mutation ? "index content" : "modified worktree"));
    host.complete(); // Git has already completed; only the old read is queued.
    const auto opened = host.call("operation.get", {{"id", opening["operationId"]}});
    if (mutation) {
        REQUIRE(opened["state"] == "failed");
        CHECK(opened["error"]["code"] == -32013);
        CHECK(host.call("workspace.documents").empty());
        const auto fresh = host.finish(host.call("workspace.open", {{"path", "tracked.txt"}}));
        REQUIRE(fresh["state"] == "completed");
        const auto current = host.call("document.get", {{"id", fresh["result"]["id"]}});
        CHECK(current["text"] == "index content");
        CHECK(current["dirty"] == false);
    } else {
        REQUIRE(opened["state"] == "completed");
        const auto current = host.call("document.get", {{"id", opened["result"]["id"]}});
        CHECK(current["text"] == "modified worktree");
        CHECK(current["dirty"] == false);
    }
}

#if defined(_WIN32)
TEST_CASE(
    "Windows application search reconciles and protects buffers opened through a junction alias") {
    SearchFiles files;
    fs::create_directory(files.root / "a-real");
    files.write("a-real/file.txt", "target disk");
    // Enumeration visits a-real before z-alias, while the editor binds the
    // alias. Destroy the host first, then the junction, then the owned tree.
    const SearchJunction alias(files.root / "z-alias", files.root / "a-real");
    SearchHost host(files.root);
    const auto opened = host.done("workspace.open", {{"path", "z-alias/file.txt"}});
    REQUIRE(opened["path"] == "z-alias/file.txt");
    REQUIRE(opened["dirty"] == false);
    bool dirty = false;
    bool mixed = false;
    SUBCASE("Default workspace search updates a clean alias buffer") {
    }
    SUBCASE("Mixed unsaved buffer search also updates a clean alias buffer") {
        mixed = true;
        static_cast<void>(host.call("document.create", {{"text", "target unsaved"}}));
    }
    SUBCASE("Explicit disk search refuses a dirty alias buffer") {
        dirty = true;
        static_cast<void>(host.call(
            "document.edit",
            {{"id", opened["id"]},
             {"baseRevision", "0"},
             {"edits", Json::array({{{"begin", 0}, {"end", 0}, {"text", "unsaved "}}})}}));
    }
    Json parameters{{"query", "target"}};
    if (dirty) {
        parameters["scope"] = "disk";
    }
    const auto search = host.call("workspace.search", std::move(parameters));
    const auto found = host.wait(search);
    REQUIRE(found["state"] == "completed");
    const auto& matches = found["result"]["matches"];
    REQUIRE(matches.size() == (mixed ? 2U : 1U));
    CHECK(std::any_of(matches.begin(), matches.end(), [](const Json& match) {
        return match.value("path", std::string{}) == "a-real/file.txt";
    }));
    if (dirty) {
        const auto refused = host.response(
            "workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "done"}});
        REQUIRE(refused.contains("error"));
        CHECK(refused["error"]["code"] == -32013);
        CHECK(files.read("a-real/file.txt") == "target disk");
        const auto current = host.call("document.get", {{"id", opened["id"]}});
        CHECK(current["text"] == "unsaved target disk");
        CHECK(current["dirty"] == true);
    } else {
        const auto replaced = host.done(
            "workspace.replace", {{"searchId", search["operationId"]}, {"replacement", "done"}});
        CHECK(replaced["applied"] == Json::array({"a-real/file.txt"}));
        CHECK(replaced["pending"].empty());
        CHECK(files.read("a-real/file.txt") == "done disk");
        const auto current = host.call("document.get", {{"id", opened["id"]}});
        CHECK(current["text"] == "done disk");
        CHECK(current["dirty"] == false);
        if (mixed) {
            CHECK(replaced["modifiedBuffers"].size() == 1);
        }
    }
}
#endif
