#include <astra/service/application_service.hpp>
#include <astra/transport/tcp_server.hpp>
#include <astra/transport/tcp_client.hpp>

#include <doctest/doctest.h>

#if defined(_WIN32)
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
using astra::protocol::Json;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

std::string language_path(const fs::path& path) {
    const auto bytes = path.generic_u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
fs::path language_repository() {
    const auto root = fs::absolute(fs::path(__FILE__)).parent_path().parent_path();
    if (!fs::exists(root / "EditorDoc.md")) { throw std::runtime_error("Language integration tests require repository root"); }
    return root;
}
fs::path language_fixture() {
    std::vector<wchar_t> buffer(32768);
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) { throw std::runtime_error("Cannot locate language test fixture"); }
    return fs::path(std::wstring(buffer.data(), size)).parent_path() / "lsp-fixture.exe";
}
class LanguageFiles {
public:
    LanguageFiles() {
        static std::atomic<std::uint64_t> sequence{};
        build_ = fs::weakly_canonical(language_repository() / "build");
        root_ = build_ / ("language-service-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++sequence));
        fs::create_directories(root_);
    }
    ~LanguageFiles() {
        std::error_code error;
        const auto actual = fs::weakly_canonical(root_, error);
        if (error || actual != root_ || actual.parent_path() != build_) { std::fputs("Unsafe language test cleanup refused\n", stderr); return; }
        fs::remove_all(actual, error);
        if (error) { std::fputs("Language test cleanup failed\n", stderr); }
    }
    const fs::path& root() const { return root_; }
    void write(std::string_view file, std::string_view value) const {
        std::ofstream stream(root_ / std::string(file), std::ios::binary | std::ios::trunc);
        stream.exceptions(std::ios::failbit | std::ios::badbit);
        stream.write(value.data(), static_cast<std::streamsize>(value.size()));
    }
private:
    fs::path root_;
    fs::path build_;
};

class LanguageApp {
public:
    explicit LanguageApp(const LanguageFiles& files, std::string fixture = {}) {
        Json config{{"command", "clangd"}, {"arguments", Json::array({"--background-index", "--enable-config=false", "--log=error"})}};
        if (!fixture.empty()) { config = {{"command", language_path(language_fixture())}, {"arguments", Json::array({fixture})}}; }
        const Json typescript{{"command", "typescript-language-server"}, {"arguments", Json::array({"--stdio"})}};
        const Json python{{"command", "basedpyright-langserver"}, {"arguments", Json::array({"--stdio"})}};
        const auto defaults = "astra.configure(" + Json{{"languageServers", {{"cpp", config}, {"c", config},
            {"javascript", typescript}, {"typescript", typescript}, {"python", python}}}}.dump() + ");";
        server_ = std::make_unique<astra::transport::TcpServer>(
            [this](std::string_view body) { return app_->dispatch_text(body); },
            [this] { return app_->take_notifications(); }, astra::transport::TcpServerOptions{},
            astra::transport::TcpServerHooks{
                [this, root = files.root(), defaults] {
                    app_ = std::make_unique<astra::service::ApplicationService>(root,
                        [this](auto work, auto complete) {
                            return server_->queue_work([this, work = std::move(work)] {
                                const auto deadline = std::chrono::steady_clock::now() + 10s;
                                while (workers_paused_.load()) {
                                    if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error("Test worker release deadline expired"); }
                                    std::this_thread::sleep_for(1ms);
                                }
                                work();
                            }, std::move(complete));
                        },
                        defaults, "", language_repository());
                }, [this] { app_.reset(); }});
        server_->start();
    }
    ~LanguageApp() { workers_paused_.store(false); server_->stop(); }
    void pause_workers(bool pause) { workers_paused_.store(pause); }
    Json response(std::string method, Json params = Json::object()) {
        const Json request{{"jsonrpc", "2.0"}, {"id", "test"}, {"method", std::move(method)}, {"params", std::move(params)}};
        auto future = server_->submit(request.dump());
        if (future.wait_for(10s) != std::future_status::ready) { throw std::runtime_error("Service loop blocked by language I/O"); }
        return Json::parse(future.get().value());
    }
    Json call(std::string method, Json params = Json::object()) {
        const auto value = response(std::move(method), std::move(params));
        INFO(value.dump());
        REQUIRE_FALSE(value.contains("error"));
        return value["result"];
    }
    Json batch(const Json& requests) {
        auto response = server_->submit(requests.dump());
        if (response.wait_for(10s) != std::future_status::ready) { throw std::runtime_error("Service batch deadline expired"); }
        return Json::parse(response.get().value());
    }
    Json wait(const Json& started) {
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = call("operation.get", {{"id", started["operationId"]}});
            if (result["state"] != "pending") { return result; }
            std::this_thread::sleep_for(5ms);
        }
        throw std::runtime_error("Language operation deadline expired");
    }
    Json done(std::string method, Json params = Json::object()) {
        const auto result = wait(call(std::move(method), std::move(params)));
        INFO(result.dump());
        REQUIRE(result["state"] == "completed");
        return result["result"];
    }
    Json get(const Json& id) { return call("document.get", {{"id", id}}); }
    std::uint16_t port() const { return server_->port(); }
private:
    std::atomic<bool> workers_paused_{};
    std::unique_ptr<astra::service::ApplicationService> app_;
    std::unique_ptr<astra::transport::TcpServer> server_;
};

Json lsp_edit(std::size_t start, std::size_t end, std::string text) {
    return {{"range", {{"start", {{"line", 0}, {"character", start}}},
                       {"end", {{"line", 0}, {"character", end}}}}}, {"newText", std::move(text)}};
}
} // namespace

TEST_CASE("application language text edits and completion are one versioned undo transaction") {
    LanguageFiles files;
    LanguageApp app(files);
    const auto id = app.call("document.create", {{"text", "pri value;"}})["id"];
    const Json item{{"label", "print"}, {"additionalTextEdits", Json::array({lsp_edit(4, 9, "number")})}};
    const auto changed = app.call("language.applyCompletion", {{"id", id}, {"baseRevision", "0"}, {"byteOffset", 3}, {"completionItem", item}});
    CHECK(changed["revision"] == "1");
    CHECK(app.get(id)["text"] == "print number;");
    CHECK(app.response("language.applyCompletion", {{"id", id}, {"baseRevision", "0"}, {"byteOffset", 3}, {"completionItem", item}})["error"]["code"] == -32001);
    (void)app.call("document.undo", {{"id", id}, {"baseRevision", "1"}});
    CHECK(app.get(id)["text"] == "pri value;");
    const auto revision = app.get(id)["revision"];
    const auto invalid = app.response("language.applyTextEdits", {{"id", id}, {"baseRevision", revision},
        {"edits", Json::array({lsp_edit(0, 3, "a"), lsp_edit(2, 5, "b")})}});
    CHECK(invalid.contains("error"));
    CHECK(app.get(id)["revision"] == revision);
    CHECK(app.get(id)["text"] == "pri value;");
    CHECK(app.response("language.applyCompletion", {{"id", id}, {"baseRevision", revision}, {"byteOffset", 3},
        {"completionItem", {{"label", "snippet"}, {"insertTextFormat", 2}, {"insertText", "${1:value}"}}}})["error"]["code"] == -32031);
}

TEST_CASE("application completion fallback respects Unicode words and CRLF boundaries") {
    LanguageFiles files;
    LanguageApp app(files);
    const std::string source = "\xF0\x9F\x98\x80pri\r\n";
    const auto id = app.call("document.create", {{"text", source}})["id"];
    CHECK(app.response("language.applyCompletion", {{"id", id}, {"baseRevision", "0"}, {"byteOffset", 8},
        {"completionItem", {{"label", "print"}}}}).contains("error"));
    CHECK(app.get(id)["text"] == source);
    (void)app.call("language.applyCompletion", {{"id", id}, {"baseRevision", "0"}, {"byteOffset", 7},
        {"completionItem", {{"label", "print"}}}});
    CHECK(app.get(id)["text"] == "\xF0\x9F\x98\x80print\r\n");
}

TEST_CASE("application diagnostics lazily starts real clangd and unsupported files stay quiet") {
    LanguageFiles files;
    files.write("main.cpp", "int main() { return unknown_identifier; }\n");
    files.write("readme.txt", "plain text\n");
    LanguageApp app(files);
    const auto unsupported = app.done("workspace.open", {{"path", "readme.txt"}})["id"];
    const auto unavailable = app.call("language.diagnostics", {{"id", unsupported}});
    CHECK(unavailable["available"] == false);
    CHECK(unavailable["pending"] == false);
    const auto id = app.done("workspace.open", {{"path", "main.cpp"}})["id"];
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    Json diagnostics;
    do {
        diagnostics = app.call("language.diagnostics", {{"id", id}});
        REQUIRE(diagnostics["available"] == true);
        if (!diagnostics.value("pending", true)) { break; }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    INFO(diagnostics.dump());
    CHECK(diagnostics["pending"] == false);
    CHECK_FALSE(diagnostics["diagnostics"].empty());
    CHECK(diagnostics["revision"] == "0");
}

TEST_CASE("application real clangd broadcasts diagnostics to an otherwise idle TCP client") {
    LanguageFiles files;
    files.write("main.cpp", "int main() { return missing_symbol; }\n");
    LanguageApp app(files);
    const auto id = app.done("workspace.open", {{"path", "main.cpp"}})["id"];
    astra::transport::TcpClient client;
    auto connected = client.connect("127.0.0.1", app.port());
    REQUIRE(connected.wait_for(5s) == std::future_status::ready);
    connected.get();
    const Json request{{"jsonrpc", "2.0"}, {"id", "diagnostics"}, {"method", "language.diagnostics"}, {"params", {{"id", id}}}};
    auto first = client.submit(request.dump());
    REQUIRE(first.wait_for(5s) == std::future_status::ready);
    REQUIRE_FALSE(Json::parse(first.get().value()).contains("error"));
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    Json published;
    // No service request, post or worker completion is driven by this loop.
    // Only the server's owner-loop timer can drain a later LSP diagnostic.
    while (published.is_null() && std::chrono::steady_clock::now() < deadline) {
        for (const auto& body : client.take_notifications()) {
            const auto event = Json::parse(body);
            if (event.value("method", std::string{}) == "language.diagnosticsChanged" && event["params"]["id"] == id) {
                published = event["params"];
            }
        }
        if (published.is_null()) { std::this_thread::sleep_for(10ms); }
    }
    REQUIRE_FALSE(published.is_null());
    CHECK(published["revision"] == "0");
    CHECK(published["count"].get<std::size_t>() > 0);
    auto refreshed = client.submit(request.dump());
    REQUIRE(refreshed.wait_for(5s) == std::future_status::ready);
    const auto diagnostics = Json::parse(refreshed.get().value())["result"];
    CHECK(diagnostics["pending"] == false);
    CHECK_FALSE(diagnostics["diagnostics"].empty());
    const auto quiet_until = std::chrono::steady_clock::now() + 250ms;
    std::size_t duplicate_events{};
    while (std::chrono::steady_clock::now() < quiet_until) {
        for (const auto& body : client.take_notifications()) {
            duplicate_events += Json::parse(body).value("method", std::string{}) == "language.diagnosticsChanged" ? 1U : 0U;
        }
        std::this_thread::sleep_for(10ms);
    }
    CHECK(duplicate_events == 0);
    client.close();
}

TEST_CASE("application language synchronizes changed Rope extent as incremental UTF16 text") {
    LanguageFiles files;
    const std::string source = "int before;\r\n// \xF0\x9F\x98\x80\r\nint after;\r\n" + std::string(30000, ' ');
    files.write("main.cpp", source);
    LanguageApp app(files, "observe-language");
    const auto opened = app.done("workspace.open", {{"path", "main.cpp"}});
    const auto id = opened["id"];
    (void)app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "hover"}, {"byteOffset", 4}});
    const auto location = source.find("after");
    (void)app.call("document.edit", {{"id", id}, {"baseRevision", "0"},
        {"edits", Json::array({Json{{"begin", location}, {"end", location + 5}, {"text", "changed"}}})}});
    const auto result = app.done("language.request", {{"id", id}, {"baseRevision", "1"}, {"method", "hover"}, {"byteOffset", location}});
    CHECK_FALSE(result["obsolete"].get<bool>());
    const auto wire = Json::parse(result["result"]["contents"]["value"].get<std::string>());
    REQUIRE(wire.contains("contentChanges"));
    REQUIRE(wire["contentChanges"].size() == 1);
    CHECK(wire["contentChanges"][0].contains("range"));
    CHECK(wire["contentChanges"][0]["text"].get<std::string>().size() < 100);
    CHECK(wire["contentChanges"][0]["text"].get<std::string>().find("changed") != std::string::npos);
    const auto long_line_offset = source.size() + 2 - 100;
    (void)app.call("document.edit", {{"id", id}, {"baseRevision", "1"},
        {"edits", Json::array({{{"begin", long_line_offset}, {"end", long_line_offset}, {"text", "x"}}})}});
    const auto third = app.done("language.request", {{"id", id}, {"baseRevision", "2"}, {"method", "hover"}, {"byteOffset", long_line_offset}});
    const auto small = Json::parse(third["result"]["contents"]["value"].get<std::string>());
    CHECK(small["contentChanges"][0]["text"].get<std::string>().size() < 10);
}

TEST_CASE("application language marks results obsolete after concurrent document change") {
    LanguageFiles files;
    files.write("main.cpp", "int value;");
    LanguageApp app(files, "delayed-hover");
    const auto id = app.done("workspace.open", {{"path", "main.cpp"}})["id"];
    const auto operation = app.call("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "hover"}, {"byteOffset", 4}});
    std::string expected;
    SUBCASE("Changed text") {
        (void)app.call("document.edit", {{"id", id}, {"baseRevision", "0"}, {"edits", Json::array({{{"begin", 4}, {"end", 9}, {"text", "other"}}})}});
        expected = "int other;";
    }
    SUBCASE("Saved as another path without changing revision") {
        (void)app.done("workspace.save", {{"id", id}, {"baseRevision", "0"}, {"path", "another.cpp"}});
        REQUIRE(app.get(id)["revision"] == "0");
        expected = "int value;";
    }
    files.write(".release-hover", "ready");
    const auto completed = app.wait(operation);
    REQUIRE(completed["state"] == "completed");
    CHECK(completed["result"]["obsolete"] == true);
    CHECK(app.get(id)["text"] == expected);
}

TEST_CASE("application real clangd formatting applies through the document transaction") {
    LanguageFiles files;
    const std::string source = "int main(){return 0;}\n";
    files.write("main.cpp", source);
    LanguageApp app(files);
    const auto id = app.done("workspace.open", {{"path", "main.cpp"}})["id"];
    const auto formatted = app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "formatting"}});
    REQUIRE_FALSE(formatted["obsolete"].get<bool>());
    REQUIRE(formatted["result"].is_array());
    REQUIRE_FALSE(formatted["result"].empty());
    (void)app.call("language.applyTextEdits", {{"id", id}, {"baseRevision", "0"}, {"edits", formatted["result"]}});
    CHECK(app.get(id)["text"] != source);
    CHECK(app.get(id)["dirty"] == true);
    (void)app.call("document.undo", {{"id", id}, {"baseRevision", "1"}});
    CHECK(app.get(id)["text"] == source);
}

TEST_CASE("application clangd diagnostics return after completion and immediate undo") {
    LanguageFiles files;
    const std::string source = "int square(int value) { return value * value; }\nint main() { return squ; }\n";
    files.write("main.cpp", source);
    LanguageApp app(files);
    const auto id = app.done("workspace.open", {{"path", "main.cpp"}})["id"];
    (void)app.call("language.diagnostics", {{"id", id}});
    const auto offset = source.rfind("squ") + 3;
    const auto completed = app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "completion"}, {"byteOffset", offset}});
    const auto items = completed["result"].is_array() ? completed["result"] : completed["result"].value("items", Json::array());
    Json selected;
    for (const auto& item : items) {
        if (item.value("label", std::string{}).find("square") != std::string::npos) { selected = item; break; }
    }
    REQUIRE_FALSE(selected.is_null());
    const auto changed = app.call("language.applyCompletion", {{"id", id}, {"baseRevision", "0"},
        {"byteOffset", offset}, {"completionItem", selected}});
    (void)app.call("document.undo", {{"id", id}, {"baseRevision", changed["revision"]}});
    REQUIRE(app.get(id)["text"] == source);
    const auto revision = app.get(id)["revision"];
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    Json diagnostics;
    do {
        diagnostics = app.call("language.diagnostics", {{"id", id}});
        if (!diagnostics["diagnostics"].empty()) { break; }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    INFO(diagnostics.dump());
    CHECK(diagnostics["revision"] == revision);
    CHECK_FALSE(diagnostics["diagnostics"].empty());
}

TEST_CASE("application coalesced edit and undo preserve the actual LSP document version") {
    LanguageFiles files;
    files.write("main.cpp", "int value;");
    LanguageApp app(files, "observe-language");
    const auto id = app.done("workspace.open", {{"path", "main.cpp"}})["id"];
    (void)app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "hover"}, {"byteOffset", 4}});
    const auto batch = app.batch(Json::array({
        Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "document.edit"}, {"params", {{"id", id}, {"baseRevision", "0"},
            {"edits", Json::array({{{"begin", 4}, {"end", 9}, {"text", "other"}}})}}}},
        Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "document.undo"}, {"params", {{"id", id}, {"baseRevision", "1"}}}}
    }));
    REQUIRE(batch.size() == 2);
    REQUIRE_FALSE(batch[0].contains("error"));
    REQUIRE_FALSE(batch[1].contains("error"));
    REQUIRE(app.get(id)["revision"] == "2");
    const auto observed = app.done("language.request", {{"id", id}, {"baseRevision", "2"}, {"method", "hover"}, {"byteOffset", 4}});
    const auto wire = Json::parse(observed["result"]["contents"]["value"].get<std::string>());
    CHECK(wire["version"] == 0);
    CHECK_FALSE(wire.contains("contentChanges"));
}

TEST_CASE("application retains exact server diagnostics while an edit undo is awaiting synchronization") {
    LanguageFiles files;
    files.write("main.cpp", "int value;");
    LanguageApp app(files, "delayed-diagnostics");
    const auto id = app.done("workspace.open", {{"path", "main.cpp"}})["id"];
    const auto original = app.call("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "hover"}, {"byteOffset", 4}});
    const auto waiting_deadline = std::chrono::steady_clock::now() + 5s;
    while (!fs::exists(files.root() / ".diagnostic-waiting") && std::chrono::steady_clock::now() < waiting_deadline) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(fs::exists(files.root() / ".diagnostic-waiting"));
    // The first hover is already inside its worker. Hold only subsequent
    // synchronization jobs until the owner has drained the original result.
    app.pause_workers(true);
    const auto batch = app.batch(Json::array({
        Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "document.edit"}, {"params", {{"id", id}, {"baseRevision", "0"},
            {"edits", Json::array({{{"begin", 4}, {"end", 9}, {"text", "other"}}})}}}},
        Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "document.undo"}, {"params", {{"id", id}, {"baseRevision", "1"}}}}
    }));
    REQUIRE_FALSE(batch[0].contains("error"));
    REQUIRE_FALSE(batch[1].contains("error"));
    files.write(".release-hover", "ready");
    REQUIRE(app.wait(original)["state"] == "completed");
    const auto pending = app.call("language.diagnostics", {{"id", id}});
    CHECK(pending["revision"] == "2");
    CHECK(pending["pending"] == true);
    CHECK(pending["diagnostics"].empty());
    app.pause_workers(false);
    const auto synchronized = app.done("language.request", {{"id", id}, {"baseRevision", "2"}, {"method", "hover"}, {"byteOffset", 4}});
    const auto wire = Json::parse(synchronized["result"]["contents"]["value"].get<std::string>());
    CHECK(wire["version"] == 0);
    CHECK_FALSE(wire.contains("contentChanges"));
    const auto diagnostics = app.call("language.diagnostics", {{"id", id}});
    CHECK(diagnostics["revision"] == "2");
    CHECK(diagnostics["pending"] == false);
    CHECK(diagnostics["diagnostics"].size() == 1);
}

TEST_CASE("application real clangd rename atomically updates open and closed buffers") {
    LanguageFiles files;
    files.write("math.h", "int add(int left, int right);\n");
    const std::string source = "#include \"math.h\"\nint add(int left,int right){return left+right;}\nint main(){return add(1,2);}\n";
    files.write("main.cpp", source);
    files.write("compile_commands.json", Json::array({Json{{"directory", language_path(files.root())},
        {"file", language_path(files.root() / "main.cpp")}, {"arguments", Json::array({"clang++", "-std=c++20", "-c", "main.cpp"})}}}).dump());
    LanguageApp app(files);
    const auto id = app.done("workspace.open", {{"path", "main.cpp"}})["id"];
    // Background indexing is asynchronous. Wait for observable header references
    // before asking clangd for the cross-file rename; never use a fixed sleep.
    const auto index_deadline = std::chrono::steady_clock::now() + 30s;
    bool header_indexed = false;
    do {
        const auto references = app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "references"},
            {"byteOffset", source.find("add") + 1}, {"includeDeclaration", true}});
        for (const auto& location : references["resolvedLocations"]) {
            if (location.value("path", std::string{}) == "math.h") { header_indexed = true; }
        }
        if (!header_indexed) { std::this_thread::sleep_for(20ms); }
    } while (!header_indexed && std::chrono::steady_clock::now() < index_deadline);
    REQUIRE(header_indexed);
    const auto started = app.call("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "rename"},
        {"byteOffset", source.find("add") + 1}, {"newName", "sum"}});
    const auto operation = app.wait(started);
    INFO(operation.dump());
    REQUIRE(operation["state"] == "completed");
    REQUIRE(operation["result"].contains("planId"));
    const auto plan = app.call("language.prepareWorkspaceEdit", {{"operationId", started["operationId"]}});
    REQUIRE(plan["documents"].size() >= 2);
    SUBCASE("All files commit as dirty buffers without disk overwrite") {
        const auto result = app.done("language.applyWorkspaceEdit", {{"planId", plan["planId"]}});
        CHECK(result["applied"] == true);
        CHECK(result["documents"].size() >= 2);
        CHECK(app.get(id)["text"].get<std::string>().find("sum") != std::string::npos);
        for (const auto& document : result["documents"]) {
            CHECK(document["dirty"] == true);
            CHECK(app.get(document["id"])["text"].get<std::string>().find("sum") != std::string::npos);
        }
        CHECK(astra::io::read_file(files.root() / "math.h").text == "int add(int left, int right);\n");
    }
    SUBCASE("A changed closed file rejects the whole plan") {
        files.write("math.h", "int external_change;\n");
        const auto failed = app.wait(app.call("language.applyWorkspaceEdit", {{"planId", plan["planId"]}}));
        CHECK(failed["state"] == "failed");
        CHECK(failed["error"]["code"] == -32011);
        CHECK(app.get(id)["text"] == source);
        CHECK(app.call("workspace.documents").size() == 1);
    }
    SUBCASE("A changed open buffer rejects every target") {
        (void)app.call("document.edit", {{"id", id}, {"baseRevision", "0"}, {"edits", Json::array({{{"begin", 0}, {"end", 0}, {"text", "// edit\n"}}})}});
        const auto failed = app.wait(app.call("language.applyWorkspaceEdit", {{"planId", plan["planId"]}}));
        CHECK(failed["state"] == "failed");
        CHECK(failed["error"]["code"] == -32001);
        CHECK(app.get(id)["text"] == "// edit\n" + source);
        CHECK(app.call("workspace.documents").size() == 1);
    }
    SUBCASE("A saved-as open buffer rejects the old path plan") {
        (void)app.done("workspace.save", {{"id", id}, {"baseRevision", "0"}, {"path", "another.cpp"}});
        REQUIRE(app.get(id)["revision"] == "0");
        const auto failed = app.wait(app.call("language.applyWorkspaceEdit", {{"planId", plan["planId"]}}));
        CHECK(failed["state"] == "failed");
        CHECK(failed["error"]["code"] == -32001);
        CHECK(app.get(id)["text"] == source);
        CHECK(app.call("workspace.documents").size() == 1);
        CHECK(astra::io::read_file(files.root() / "math.h").text == "int add(int left, int right);\n");
    }
}

TEST_CASE("application bundled JavaScript TypeScript and Python servers provide real intelligence") {
    LanguageFiles files;
    std::string name;
    std::string source;
    std::string member;
    SUBCASE("TypeScript") {
        name = "main.ts";
        source = "const answer = 42;\nconst doubled = answer * 2;\nanswer.toString();\nconst wrong: number = 'bad';\n";
        member = "toFixed";
        files.write("tsconfig.json", R"({"compilerOptions":{"strict":true,"noEmit":true},"files":["main.ts"]})");
    }
    SUBCASE("JavaScript") {
        name = "main.js";
        source = "const answer = 42;\nconst doubled = answer * 2;\nanswer.toString();\nmissing_symbol();\n";
        member = "toFixed";
        files.write("jsconfig.json", R"({"compilerOptions":{"checkJs":true,"noEmit":true},"files":["main.js"]})");
    }
    SUBCASE("Python") {
        name = "main.py";
        source = "answer: int = 42\nprint(answer)\nanswer.bit_length()\nwrong: int = 'bad'\n";
        member = "bit_length";
        files.write("pyrightconfig.json", R"({"pythonVersion":"3.13","typeCheckingMode":"basic","include":["main.py"]})");
    }
    files.write(name, source);
    LanguageApp app(files);
    const auto id = app.done("workspace.open", {{"path", name}})["id"];
    const auto point = source.find("answer", source.find('\n')) + 1;
    const auto hover = app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "hover"}, {"byteOffset", point}});
    INFO(hover.dump());
    REQUIRE_FALSE(hover["obsolete"].get<bool>());
    CHECK_FALSE(hover["result"].is_null());
    const auto completion = app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "completion"},
        {"byteOffset", source.rfind("answer.") + 7}});
    const auto items = completion["result"].is_array() ? completion["result"] : completion["result"].value("items", Json::array());
    bool member_found = false;
    for (const auto& item : items) {
        if (item.value("label", std::string{}).starts_with(member)) { member_found = true; }
    }
    INFO("completion item count: ", items.size());
    CHECK(member_found);
    const auto definition = app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "definition"}, {"byteOffset", point}});
    REQUIRE_FALSE(definition["resolvedLocations"].empty());
    CHECK(definition["resolvedLocations"][0]["path"] == name);
    CHECK(definition["resolvedLocations"][0]["targetId"] == id);
    const auto references = app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "references"}, {"byteOffset", point}});
    CHECK(references["resolvedLocations"].size() >= 2);
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    Json diagnostics;
    do {
        diagnostics = app.call("language.diagnostics", {{"id", id}});
        if (!diagnostics["diagnostics"].empty()) { break; }
        std::this_thread::sleep_for(10ms);
    } while (std::chrono::steady_clock::now() < deadline);
    INFO(diagnostics.dump());
    CHECK_FALSE(diagnostics["diagnostics"].empty());
    const auto renamed = app.done("language.request", {{"id", id}, {"baseRevision", "0"}, {"method", "rename"},
        {"byteOffset", point}, {"newName", "renamed_answer"}});
    REQUIRE(renamed.contains("planId"));
    (void)app.done("language.applyWorkspaceEdit", {{"planId", renamed["planId"]}});
    CHECK(app.get(id)["text"].get<std::string>().find("renamed_answer") != std::string::npos);
    CHECK(app.get(id)["dirty"] == true);
}

#endif
