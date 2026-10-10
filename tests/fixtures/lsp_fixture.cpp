#include <astra/protocol/framing.hpp>
#include <astra/protocol/rpc.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace {
using astra::protocol::Json;

void send(const Json& value) {
    const auto frame = astra::protocol::encode_frame(value.dump());
    std::cout.write(frame.data(), static_cast<std::streamsize>(frame.size()));
    std::cout.flush();
}

void respond(const Json& id, Json result) {
    send(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}});
}
} // namespace

// Test-only executable: adversarial transport/lifecycle and wire observation.
// Actual language features are verified against pinned clangd, never this peer.
int main(int argc, char** argv) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    const std::string mode = argc > 1 ? argv[1] : "normal";
    if (mode == "exit-early") {
        std::cerr << "intentional startup failure\n";
        return 7;
    }
    Json methods = Json::array();
    Json documents = Json::object();
    Json server_replies = Json::array();
    std::vector<Json> hung;
    bool shutdown_received = false;
    bool diagnostic_sent = false;
    astra::protocol::FrameDecoder decoder;
    char byte{};
    while (std::cin.get(byte)) {
        for (const auto& body : decoder.feed(std::string_view(&byte, 1))) {
            const auto message = Json::parse(body);
            if (!message.contains("method")) {
                server_replies.push_back(message);
                continue;
            }
            const auto method = message["method"].get<std::string>();
            methods.push_back(method);
            const auto params = message.value("params", Json::object());
            if (method == "initialize") {
                if (mode == "initialize-hang") {
                    continue;
                }
                const Json capabilities{
                    {"positionEncoding", mode == "bad-encoding" ? "utf-8" : "utf-16"},
                    {"textDocumentSync", mode == "full-sync" ? 1 : 2},
                    {"hoverProvider", true}};
                respond(message["id"],
                        Json{{"capabilities", capabilities},
                             {"serverInfo", {{"name", "Astra fault fixture"}}}});
            } else if (method == "textDocument/didOpen") {
                const auto& document = params["textDocument"];
                documents[document["uri"].get<std::string>()] = document;
            } else if (method == "textDocument/didChange") {
                const auto uri = params["textDocument"]["uri"].get<std::string>();
                documents[uri]["version"] = params["textDocument"]["version"];
                documents[uri]["contentChanges"] = params["contentChanges"];
                // Observe the actual per-notification envelope, including the
                // absence of optional server extensions on subsequent changes.
                documents[uri]["lastChangeParams"] = params;
            } else if (method == "textDocument/didClose") {
                documents.erase(params["textDocument"]["uri"].get<std::string>());
            } else if (method == "fixture/observe") {
                const auto utf = std::filesystem::current_path().generic_u8string();
                Json arguments = Json::array();
                for (int index = 1; index < argc; ++index) {
                    arguments.push_back(argv[index]);
                }
                respond(message["id"],
                        Json{{"methods", methods},
                             {"documents", documents},
                             {"serverReplies", server_replies},
                             {"argv", arguments},
                             {"cwd",
                              std::string(reinterpret_cast<const char*>(utf.data()), utf.size())}});
            } else if (method == "fixture/echo") {
                respond(message["id"], params);
            } else if (method == "textDocument/hover" &&
                       (mode == "observe-language" || mode == "delayed-hover" ||
                        mode == "delayed-diagnostics")) {
                if (mode == "delayed-hover" ||
                    (mode == "delayed-diagnostics" && !diagnostic_sent)) {
                    if (mode == "delayed-diagnostics") {
                        std::ofstream marker(std::filesystem::current_path() /
                                             ".diagnostic-waiting");
                        marker << "ready";
                    }
                    const auto deadline =
                        std::chrono::steady_clock::now() + std::chrono::seconds(10);
                    while (!std::filesystem::exists(std::filesystem::current_path() /
                                                    ".release-hover")) {
                        if (std::chrono::steady_clock::now() >= deadline) {
                            return 8;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
                const auto uri = params["textDocument"]["uri"].get<std::string>();
                if (mode == "delayed-diagnostics" && !diagnostic_sent) {
                    diagnostic_sent = true;
                    send(astra::protocol::make_notification(
                        "textDocument/publishDiagnostics",
                        {{"uri", uri},
                         {"version", documents[uri]["version"]},
                         {"diagnostics",
                          Json::array({{{"range",
                                         {{"start", {{"line", 0}, {"character", 4}}},
                                          {"end", {{"line", 0}, {"character", 9}}}}},
                                        {"message", "Controlled timing diagnostic"},
                                        {"severity", 1}}})}}));
                }
                respond(
                    message["id"],
                    Json{{"contents", {{"kind", "plaintext"}, {"value", documents[uri].dump()}}}});
            } else if (method == "fixture/hang") {
                hung.push_back(message["id"]);
            } else if (method == "fixture/release") {
                for (const auto& id : hung) {
                    respond(id, "late response");
                }
                hung.clear();
                respond(message["id"], true);
            } else if (method == "fixture/error") {
                send(Json{
                    {"jsonrpc", "2.0"},
                    {"id", message["id"]},
                    {"error",
                     {{"code", -1234}, {"message", "fixture error"}, {"data", {{"detail", 7}}}}}});
            } else if (method == "fixture/diagnostics") {
                send(astra::protocol::make_notification("textDocument/publishDiagnostics", params));
                respond(message["id"], true);
            } else if (method == "fixture/notifications") {
                for (int index = 0; index < params.value("count", 10); ++index) {
                    send(astra::protocol::make_notification("fixture/event", {{"index", index}}));
                }
                respond(message["id"], true);
            } else if (method == "fixture/stderr") {
                std::cerr << std::string(params.value("bytes", std::size_t{65536}), 'E')
                          << "tail\n";
                std::cerr.flush();
                respond(message["id"], true);
            } else if (method == "fixture/serverRequest") {
                send(Json{{"jsonrpc", "2.0"},
                          {"id", "server/1"},
                          {"method", "workspace/applyEdit"},
                          {"params", {{"edit", Json::object()}}}});
                respond(message["id"], true);
            } else if (method == "fixture/crash") {
                std::cerr << "intentional crash\n";
                return 7;
            } else if (method == "fixture/malformed") {
                std::cout << astra::protocol::encode_frame("{");
                std::cout.flush();
            } else if (method == "shutdown") {
                if (mode != "shutdown-hang") {
                    shutdown_received = true;
                    respond(message["id"], nullptr);
                }
            } else if (method == "exit") {
                if (mode != "ignore-exit") {
                    return shutdown_received ? 0 : 1;
                }
            }
        }
    }
    if (mode == "ignore-exit") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
    return shutdown_received ? 0 : 1;
}
