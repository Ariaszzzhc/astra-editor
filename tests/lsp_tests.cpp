#include <astra/language/lsp.hpp>
#include <astra/transport/tcp_server.hpp>

#include <doctest/doctest.h>

#if defined(_WIN32)
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using astra::language::file_uri_to_path;
using astra::language::Json;
using astra::language::LspClient;
using astra::language::LspContentChange;
using astra::language::LspOptions;
using astra::language::LspRange;
using astra::language::path_to_file_uri;
using astra::transport::TransportCancelled;
using astra::transport::TransportError;
using namespace std::chrono_literals;

template <class T> T lsp_ready(std::future<T> future) {
    if (future.wait_for(20s) != std::future_status::ready) {
        throw std::runtime_error("LSP integration test future deadline expired");
    }
    return future.get();
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::filesystem::path executable_directory() {
    std::vector<wchar_t> buffer(32768);
    const auto size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size()) {
        throw std::runtime_error("Locate test executable failed");
    }
    return std::filesystem::path(std::wstring(buffer.data(), size)).parent_path();
}

std::filesystem::path repository() {
    const auto directory =
        std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path().parent_path();
    if (std::filesystem::exists(directory / "EditorDoc.md")) {
        return directory;
    }
    throw std::runtime_error("LSP tests require this repository workspace");
}

class LspWorkspace {
  public:
    LspWorkspace() {
        static std::atomic<std::uint64_t> next{};
        const auto unique = std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++next);
        path =
            repository() / "build" / "lsp-tests" / std::filesystem::path(u8"工作 space#") / unique;
        std::filesystem::create_directories(path);
    }

    ~LspWorkspace() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;

    std::string root_uri() const {
        return path_to_file_uri(path);
    }

    std::string uri(std::string_view file = "main.cpp") const {
        return path_to_file_uri(path / std::string(file));
    }

    void write(std::string_view text, std::string_view file = "main.cpp") const {
        std::ofstream stream(path / std::string(file), std::ios::binary);
        stream.exceptions(std::ios::badbit | std::ios::failbit);
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
};

LspOptions fixture_options(const LspWorkspace& workspace, std::string mode = "normal") {
    LspOptions options;
    options.argv = {utf8_path(executable_directory() / "lsp-fixture.exe"), std::move(mode)};
    options.working_directory = utf8_path(workspace.path);
    options.initialize_timeout = 2s;
    options.request_timeout = 2s;
    options.shutdown_timeout = 300ms;
    return options;
}

Json observe(LspClient& client) {
    return lsp_ready(client.request("fixture/observe").result);
}

void start_fixture(LspClient& client, const LspWorkspace& workspace) {
    const auto initialized = lsp_ready(client.start(workspace.root_uri()));
    REQUIRE(initialized["capabilities"]["positionEncoding"] == "utf-16");
    REQUIRE(client.ready());
}

} // namespace

TEST_CASE("LSP file URI conversion preserves Windows Unicode drive and UNC paths") {
    const std::filesystem::path path(u8"D:/项目/a b#%😀.cpp");
    const auto uri = path_to_file_uri(path);
    CHECK(uri.starts_with("file:///D:/"));
    CHECK(uri.find("%20") != std::string::npos);
    CHECK(uri.find("%23") != std::string::npos);
    CHECK(uri.find("%25") != std::string::npos);
    CHECK(file_uri_to_path(uri).generic_u8string() == path.generic_u8string());
    const std::filesystem::path unc(u8"//server/share/目录/a b.cpp");
    CHECK(file_uri_to_path(path_to_file_uri(unc)).generic_u8string() == unc.generic_u8string());
    const std::filesystem::path unicode_host(u8"//服务器/share/a.cpp");
    CHECK(file_uri_to_path(path_to_file_uri(unicode_host)).generic_u8string() ==
          unicode_host.generic_u8string());
    CHECK(file_uri_to_path("file://localhost/D:/a%20b.cpp").generic_string() == "D:/a b.cpp");
    CHECK_THROWS_AS((void)path_to_file_uri("relative.cpp"), std::invalid_argument);
    CHECK_THROWS_AS((void)path_to_file_uri("D:relative.cpp"), std::invalid_argument);
    CHECK_THROWS_AS((void)file_uri_to_path("https://example.test/file"), std::invalid_argument);
    CHECK_THROWS_AS((void)file_uri_to_path("file:///D:/%xx"), std::invalid_argument);
    CHECK_THROWS_AS((void)file_uri_to_path("file:///D:/%00"), std::invalid_argument);
    CHECK_THROWS_AS((void)file_uri_to_path("file:///D:/%FF"), std::invalid_argument);
    CHECK_THROWS_AS((void)file_uri_to_path("file:///D:/file#fragment"), std::invalid_argument);
    CHECK_THROWS_AS((void)file_uri_to_path("file://user@server/share/file"), std::invalid_argument);
}

TEST_CASE("LSP UTF16 coordinates reject surrogate splits and exclude CRLF columns") {
    const astra::Rope text("a\xF0\x9F\x98\x80z\r\nlast");
    CHECK(astra::language::lsp_position(text, 5) == Json{{"line", 0}, {"character", 3}});
    CHECK(astra::language::lsp_position(text, 7) == Json{{"line", 0}, {"character", 4}});
    CHECK(astra::language::lsp_byte_offset(text, {0, 3}) == 5);
    CHECK(astra::language::lsp_byte_offset(text, {1, 4}) == text.size_bytes());
    CHECK_THROWS_AS((void)astra::language::lsp_byte_offset(text, {0, 2}), std::invalid_argument);
    CHECK_THROWS_AS((void)astra::language::lsp_byte_offset(text, {0, 5}), std::out_of_range);
    CHECK_THROWS_AS((void)astra::language::lsp_position(text, 2), std::invalid_argument);
}

TEST_CASE("LSP fixture verifies initialize initialized argv cwd and clean shutdown order") {
    LspWorkspace workspace;
    auto options = fixture_options(workspace);
    options.argv.push_back("space and \"quoted\" argument\\");
    options.argv.push_back("& shell|characters; remain literal");
    LspClient client(options);
    CHECK_THROWS_AS((void)lsp_ready(client.request("fixture/echo").result), TransportCancelled);
    start_fixture(client, workspace);
    CHECK_THROWS_AS((void)lsp_ready(client.start(workspace.root_uri())), TransportError);
    CHECK(client.ready());
    const auto seen = observe(client);
    REQUIRE(seen["methods"].size() >= 3);
    CHECK(seen["methods"][0] == "initialize");
    CHECK(seen["methods"][1] == "initialized");
    CHECK(seen["argv"][1] == options.argv[2]);
    CHECK(seen["argv"][2] == options.argv[3]);
    CHECK(seen["cwd"] == utf8_path(workspace.path.generic_u8string()));
    CHECK(client.capabilities()["textDocumentSync"] == 2);
    lsp_ready(client.shutdown());
    CHECK_FALSE(client.ready());
    client.close();
}

TEST_CASE("LSP document changes validate versions and all sequential UTF16 ranges atomically") {
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace));
    start_fixture(client, workspace);
    const auto uri = workspace.uri();
    lsp_ready(client.did_open(uri, "cpp", 1, "a\xF0\x9F\x98\x80z\r\nnext"));
    CHECK_THROWS_AS(lsp_ready(client.did_open(uri, "cpp", 1, "")), std::invalid_argument);
    CHECK_THROWS_AS(lsp_ready(client.did_change(uri, 1, {{std::nullopt, "bad"}})),
                    std::invalid_argument);
    const std::vector<LspContentChange> invalid{{LspRange{{0, 1}, {0, 3}}, "Q"},
                                                {LspRange{{1, 99}, {1, 99}}, "bad"}};
    CHECK_THROWS(lsp_ready(client.did_change(uri, 2, invalid)));
    CHECK(observe(client)["documents"][uri]["version"] == 1);
    CHECK_THROWS_AS(lsp_ready(client.did_change(uri, 2, {{LspRange{{0, 2}, {0, 3}}, "bad"}})),
                    std::invalid_argument);
    lsp_ready(client.did_change(
        uri, 2, {{LspRange{{0, 1}, {0, 3}}, "Q"}, {LspRange{{0, 2}, {0, 3}}, "!"}}));
    const auto changed = observe(client)["documents"][uri];
    CHECK(changed["version"] == 2);
    REQUIRE(changed["contentChanges"].size() == 2);
    CHECK(changed["contentChanges"][0]["range"]["end"]["character"] == 3);
    CHECK_THROWS_AS((void)lsp_ready(client.hover(uri, {0, 99}).result), std::out_of_range);
    lsp_ready(client.did_close(uri));
    CHECK(observe(client)["documents"].empty());
    CHECK_THROWS_AS(lsp_ready(client.did_close(uri)), std::invalid_argument);
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP full-sync server receives complete resulting text after incremental edits") {
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace, "full-sync"));
    start_fixture(client, workspace);
    const auto uri = workspace.uri();
    lsp_ready(client.did_open(uri, "cpp", 0, "a\xF0\x9F\x98\x80z"));
    lsp_ready(client.did_change(uri, 1, {{LspRange{{0, 1}, {0, 3}}, "middle"}}));
    const auto changes = observe(client)["documents"][uri]["contentChanges"];
    REQUIRE(changes.size() == 1);
    CHECK_FALSE(changes[0].contains("range"));
    CHECK(changes[0]["text"] == "amiddlez");
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP ordinary didChange omits clangd diagnostic rebuild extensions") {
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace));
    start_fixture(client, workspace);
    const auto uri = workspace.uri();
    lsp_ready(client.did_open(uri, "cpp", 1, "int before;\n"));
    lsp_ready(client.did_change(uri, 2, {{std::nullopt, "int after;\n"}}));
    auto wire = observe(client)["documents"][uri]["lastChangeParams"];
    CHECK(wire["textDocument"] == Json{{"uri", uri}, {"version", 2}});
    CHECK(wire["contentChanges"] == Json::array({Json{{"text", "int after;\n"}}}));
    CHECK_FALSE(wire.contains("forceRebuild"));
    CHECK_FALSE(wire.contains("wantDiagnostics"));

    astra::language::LspChangeOptions ordinary;
    ordinary.clangd_force_rebuild = false;
    lsp_ready(client.did_change(uri, 3, {{std::nullopt, "int next;\n"}}, ordinary));
    wire = observe(client)["documents"][uri]["lastChangeParams"];
    CHECK(wire["textDocument"]["version"] == 3);
    CHECK_FALSE(wire.contains("forceRebuild"));
    CHECK_FALSE(wire.contains("wantDiagnostics"));
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP clangd diagnostic rebuild option is explicit and scoped to one change") {
    std::string mode = "normal";
    SUBCASE("incremental document synchronization") {
    }
    SUBCASE("full document synchronization") {
        mode = "full-sync";
    }
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace, mode));
    start_fixture(client, workspace);
    const auto uri = workspace.uri();
    lsp_ready(client.did_open(uri, "cpp", 1, "a\xF0\x9F\x98\x80z\r\nnext"));
    astra::language::LspChangeOptions rebuild;
    rebuild.clangd_force_rebuild = true;
    lsp_ready(client.did_change(uri, 2, {{LspRange{{0, 1}, {0, 3}}, "Q"}}, rebuild));
    const auto forced = observe(client)["documents"][uri]["lastChangeParams"];
    CHECK(forced["textDocument"] == Json{{"uri", uri}, {"version", 2}});
    REQUIRE(forced.at("forceRebuild").is_boolean());
    REQUIRE(forced.at("wantDiagnostics").is_boolean());
    CHECK(forced.at("forceRebuild").get<bool>());
    CHECK(forced.at("wantDiagnostics").get<bool>());
    REQUIRE(forced["contentChanges"].size() == 1);
    const auto& edit = forced["contentChanges"][0];
    CHECK_FALSE(edit.contains("forceRebuild"));
    CHECK_FALSE(edit.contains("wantDiagnostics"));
    if (mode == "full-sync") {
        CHECK_FALSE(edit.contains("range"));
        CHECK(edit["text"] == "aQz\r\nnext");
    } else {
        CHECK(edit["text"] == "Q");
        CHECK(edit["range"]["start"]["character"] == 1);
        CHECK(edit["range"]["end"]["character"] == 3);
    }

    CHECK_THROWS_AS(lsp_ready(client.did_change(uri, 2, {{std::nullopt, "stale"}}, rebuild)),
                    std::invalid_argument);
    CHECK(observe(client)["documents"][uri]["lastChangeParams"] == forced);
    lsp_ready(client.did_change(uri, 3, {{std::nullopt, "ordinary again"}}));
    const auto ordinary = observe(client)["documents"][uri]["lastChangeParams"];
    CHECK(ordinary["textDocument"]["version"] == 3);
    CHECK_FALSE(ordinary.contains("forceRebuild"));
    CHECK_FALSE(ordinary.contains("wantDiagnostics"));
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP cancellation ignores late results and preserves subsequent requests") {
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace));
    start_fixture(client, workspace);
    auto pending = client.request("fixture/hang");
    client.cancel(pending.id);
    CHECK_THROWS_AS((void)lsp_ready(std::move(pending.result)), TransportCancelled);
    CHECK(lsp_ready(client.request("fixture/release").result) == true);
    CHECK(lsp_ready(client.request("fixture/echo", {{"alive", true}}).result)["alive"] == true);
    const auto methods = observe(client)["methods"];
    CHECK(std::find(methods.begin(), methods.end(), "$/cancelRequest") != methods.end());
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP request timeout cancels the request without losing the process") {
    LspWorkspace workspace;
    auto options = fixture_options(workspace);
    options.request_timeout = 50ms;
    LspClient client(options);
    start_fixture(client, workspace);
    CHECK_THROWS_AS((void)lsp_ready(client.request("fixture/hang").result), TransportError);
    CHECK(client.ready());
    CHECK(lsp_ready(client.request("fixture/release").result) == true);
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP RPC errors preserve server code and data") {
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace));
    start_fixture(client, workspace);
    try {
        (void)lsp_ready(client.request("fixture/error").result);
        FAIL("Expected server RPC error");
    } catch (const astra::protocol::RpcError& error) {
        CHECK(error.code() == -1234);
        REQUIRE(error.data());
        CHECK((*error.data())["detail"] == 7);
    }
    CHECK(client.ready());
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP diagnostics discard stale versions and coalesce latest document state") {
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace));
    start_fixture(client, workspace);
    const auto uri = workspace.uri();
    lsp_ready(client.did_open(uri, "cpp", 3, "int value;"));
    const auto diagnostics = [](int number) {
        return Json::array({Json{{"message", "problem " + std::to_string(number)},
                                 {"severity", 1},
                                 {"range",
                                  {{"start", {{"line", 0}, {"character", 0}}},
                                   {"end", {{"line", 0}, {"character", 3}}}}}}});
    };
    (void)lsp_ready(client
                        .request("fixture/diagnostics",
                                 {{"uri", uri}, {"version", 2}, {"diagnostics", diagnostics(2)}})
                        .result);
    CHECK(client.take_diagnostics().empty());
    (void)lsp_ready(client
                        .request("fixture/diagnostics",
                                 {{"uri", uri}, {"version", 3}, {"diagnostics", diagnostics(3)}})
                        .result);
    (void)lsp_ready(client
                        .request("fixture/diagnostics",
                                 {{"uri", uri}, {"version", 3}, {"diagnostics", Json::array()}})
                        .result);
    const auto latest = client.take_diagnostics();
    REQUIRE(latest.size() == 1);
    CHECK(latest[0].version == 3);
    CHECK(latest[0].observed_version == 3);
    CHECK(latest[0].diagnostics.empty());
    auto alias = uri;
    // Windows servers commonly lowercase drive letters and encode ':' while
    // preserving the identity of the originally opened URI.
    REQUIRE(alias.starts_with("file:///"));
    alias[8] = static_cast<char>(std::tolower(static_cast<unsigned char>(alias[8])));
    if (alias[9] == ':') {
        alias.replace(9, 1, "%3a");
    }
    CHECK(astra::language::file_uri_equal(uri, alias));
    (void)lsp_ready(client
                        .request("fixture/diagnostics",
                                 {{"uri", alias}, {"version", 2}, {"diagnostics", diagnostics(2)}})
                        .result);
    CHECK(client.take_diagnostics().empty());
    (void)lsp_ready(
        client.request("fixture/diagnostics", {{"uri", alias}, {"diagnostics", diagnostics(3)}})
            .result);
    lsp_ready(client.did_change(uri, 4, {{std::nullopt, "int newer;"}}));
    const auto unversioned = client.take_diagnostics();
    REQUIRE(unversioned.size() == 1);
    CHECK(unversioned[0].uri == uri);
    CHECK_FALSE(unversioned[0].version.has_value());
    CHECK(unversioned[0].observed_version == 3);
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP notification and stderr buffering stay bounded") {
    LspWorkspace workspace;
    auto options = fixture_options(workspace);
    options.max_notifications = 2;
    options.max_stderr_bytes = 256;
    LspClient client(options);
    start_fixture(client, workspace);
    (void)lsp_ready(client.request("fixture/notifications", {{"count", 100}}).result);
    const auto notifications = client.take_notifications();
    REQUIRE(notifications.size() == 1);
    CHECK(notifications[0]["method"] == "language.resyncRequired");
    (void)lsp_ready(client.request("fixture/stderr", {{"bytes", 65536}}).result);
    lsp_ready(client.shutdown());
    CHECK(client.stderr_text().size() <= 256);
    CHECK((client.stderr_text().ends_with("tail\r\n") || client.stderr_text().ends_with("tail\n")));
    CHECK(client.stderr_truncated());
}

TEST_CASE("LSP unsupported server requests receive explicit method-not-found") {
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace));
    start_fixture(client, workspace);
    (void)lsp_ready(client.request("fixture/serverRequest").result);
    const auto observed = observe(client);
    REQUIRE(observed["serverReplies"].size() == 1);
    CHECK(observed["serverReplies"][0]["id"] == "server/1");
    CHECK(observed["serverReplies"][0]["error"]["code"] == -32601);
    lsp_ready(client.shutdown());
}

TEST_CASE("LSP failed spawn initialization timeout and unsupported encoding settle start") {
    LspWorkspace workspace;
    auto options = fixture_options(workspace);
    SUBCASE("Missing executable") {
        options.argv[0] = utf8_path(workspace.path / "missing-server.exe");
    }
    SUBCASE("Early process exit") {
        options.argv[1] = "exit-early";
    }
    SUBCASE("Initialization deadline") {
        options.argv[1] = "initialize-hang";
        options.initialize_timeout = 50ms;
    }
    SUBCASE("Unsupported encoding") {
        options.argv[1] = "bad-encoding";
    }
    SUBCASE("Initialize cannot fit write queue") {
        options.max_queued_write_bytes = 32;
    }
    LspClient client(options);
    CHECK_THROWS((void)lsp_ready(client.start(workspace.root_uri())));
    CHECK_FALSE(client.ready());
    client.close();
}

TEST_CASE("LSP malformed stdout and abnormal child exit fail pending requests") {
    LspWorkspace workspace;
    LspClient client(fixture_options(workspace));
    start_fixture(client, workspace);
    auto waiting = client.request("fixture/hang");
    SUBCASE("Crash") {
        CHECK_THROWS((void)lsp_ready(client.request("fixture/crash").result));
    }
    SUBCASE("Malformed frame body") {
        CHECK_THROWS((void)lsp_ready(client.request("fixture/malformed").result));
    }
    CHECK_THROWS((void)lsp_ready(std::move(waiting.result)));
    CHECK_FALSE(client.ready());
    client.close();
}

TEST_CASE("LSP shutdown cancels pending requests and terminates unresponsive child") {
    LspWorkspace workspace;
    auto options = fixture_options(workspace);
    bool expected_clean = false;
    SUBCASE("Clean shutdown") {
        expected_clean = true;
    }
    SUBCASE("Server ignores shutdown") {
        options.argv[1] = "shutdown-hang";
    }
    SUBCASE("Server ignores exit") {
        options.argv[1] = "ignore-exit";
    }
    LspClient client(options);
    start_fixture(client, workspace);
    auto waiting = client.request("fixture/hang");
    auto stopped = client.shutdown();
    CHECK_THROWS_AS((void)lsp_ready(std::move(waiting.result)), TransportCancelled);
    if (expected_clean) {
        lsp_ready(std::move(stopped));
    } else {
        CHECK_THROWS((void)lsp_ready(std::move(stopped)));
    }
    client.close();
}

TEST_CASE("LSP bounded pending requests reject overflow and close settles every future") {
    LspWorkspace workspace;
    auto options = fixture_options(workspace);
    options.max_pending_requests = 2;
    LspClient client(options);
    start_fixture(client, workspace);
    auto first = client.request("fixture/hang");
    auto second = client.request("fixture/hang");
    CHECK_THROWS_AS((void)lsp_ready(client.request("fixture/hang").result), TransportError);
    client.close();
    CHECK_THROWS((void)lsp_ready(std::move(first.result)));
    CHECK_THROWS((void)lsp_ready(std::move(second.result)));
}

TEST_CASE("LSP pinned real clangd provides code intelligence and versioned diagnostics") {
    LspWorkspace workspace;
    const auto clangd = repository() / ".tools" / "lsp" / "clangd_21.1.8" / "bin" / "clangd.exe";
    REQUIRE_MESSAGE(std::filesystem::is_regular_file(clangd),
                    "Pinned verified Windows clangd 21.1.8 is required");
    const std::string source = "struct Widget { int count; int get() const { return count; } };\n"
                               "int add(int left,int right){return left+right;}\n"
                               "int main() {\n"
                               "  Widget item;\n"
                               "  item.count = add(1, 2);\n"
                               "  return item.count;\n"
                               "}\n";
    workspace.write(source);
    LspOptions options;
    options.argv = {utf8_path(clangd),
                    "--background-index=false",
                    "--clang-tidy=false",
                    "--enable-config=false",
                    "--log=error",
                    "--pch-storage=memory",
                    "-j=2"};
    options.working_directory = utf8_path(workspace.path);
    options.request_timeout = 15s;
    LspClient client(options);
    const auto initialized = lsp_ready(client.start(
        workspace.root_uri(), {{"fallbackFlags", Json::array({"-xc++", "-std=c++23"})}}));
    INFO(client.stderr_text());
    REQUIRE(initialized["serverInfo"]["name"] == "clangd");
    REQUIRE(client.ready());
    const auto uri = workspace.uri();
    lsp_ready(client.did_open(uri, "cpp", 1, source));
    const auto hover = lsp_ready(client.hover(uri, {1, 5}).result);
    REQUIRE_FALSE(hover.is_null());
    CHECK(hover["contents"].dump().find("add") != std::string::npos);
    const auto definition = lsp_ready(client.definition(uri, {4, 16}).result);
    REQUIRE(definition.is_array());
    REQUIRE_FALSE(definition.empty());
    CHECK((definition[0].contains("uri") || definition[0].contains("targetUri")));
    const auto references = lsp_ready(client.references(uri, {1, 5}).result);
    REQUIRE(references.is_array());
    CHECK(references.size() >= 2);
    const auto completion = lsp_ready(client.completion(uri, {4, 7}).result);
    const auto& items = completion.is_array() ? completion : completion["items"];
    REQUIRE(items.is_array());
    CHECK(std::any_of(items.begin(), items.end(), [](const Json& item) {
        return item.value("label", std::string{}).find("count") != std::string::npos;
    }));
    const auto rename = lsp_ready(client.rename(uri, {1, 5}, "sum").result);
    CHECK((rename.contains("changes") || rename.contains("documentChanges")));
    CHECK(rename.dump().find("sum") != std::string::npos);
    const auto formatting = lsp_ready(client.formatting(uri).result);
    REQUIRE(formatting.is_array());
    CHECK_FALSE(formatting.empty());
    lsp_ready(client.did_change(uri, 2, {{std::nullopt, "int main() { return missing_name; }\n"}}));
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    bool saw_error = false;
    while (!saw_error && std::chrono::steady_clock::now() < deadline) {
        for (const auto& published : client.take_diagnostics()) {
            if (published.uri == uri && (!published.version || published.version == 2)) {
                for (const auto& diagnostic : published.diagnostics) {
                    if (diagnostic.value("severity", 0) == 1 &&
                        diagnostic.value("message", std::string{}).find("missing_name") !=
                            std::string::npos) {
                        saw_error = true;
                    }
                }
            }
        }
        if (!saw_error) {
            std::this_thread::sleep_for(10ms);
        }
    }
    INFO(client.stderr_text());
    CHECK(saw_error);
    lsp_ready(client.did_close(uri));
    lsp_ready(client.shutdown());
    CHECK_FALSE(client.ready());
}

#endif
