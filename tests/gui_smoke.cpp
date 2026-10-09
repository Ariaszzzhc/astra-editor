#include "astra/ui/editor_window.hpp"
#include "astra/service/service_host.hpp"
#include "astra/io/file_store.hpp"
#include <astra/transport/editor_channel.hpp>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message) {
    if (!condition) { throw std::runtime_error(message); }
}

void settle(astra::ui::EditorWindow& window) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    do {
        require(window.pump(), "Window closed before acceptance completed");
        require(window.last_error().empty(), "GUI action failed: " + window.last_error());
        if (window.idle()) { return; }
        SDL_Delay(1);
    } while (Clock::now() < deadline);
    throw std::runtime_error("GUI did not become idle within 15 seconds");
}

void key(astra::ui::EditorWindow& window, SDL_Keycode keycode, SDL_Keymod modifiers = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN; event.key.key = keycode; event.key.mod = modifiers;
    event.key.down = true; event.key.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Push production keyboard event failed");
    settle(window);
}

void text(astra::ui::EditorWindow& window, const std::string& input) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT; event.text.text = input.c_str();
    event.text.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Push production text input event failed");
    settle(window);
}

Json read_document(const std::shared_ptr<astra::service::ServiceHost>& host, const std::string& id) {
    const auto response = host->submit(Json{{"jsonrpc", "2.0"}, {"id", "acceptance"}, {"method", "document.get"}, {"params", {{"id", id}}}}.dump()).get();
    require(response.has_value(), "Document verification returned no response");
    const auto parsed = Json::parse(*response);
    require(!parsed.contains("error"), "Document verification returned an error");
    return parsed.at("result");
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        const bool remote = argc == 2 && std::string_view(argv[1]) == "--tcp";
        require(argc == 1 || remote, "Usage: gui-smoke [--tcp]");
        std::cerr << std::unitbuf << "GUI acceptance: starting service\n";
        const auto name = "gui-workspace-" + std::to_string(Clock::now().time_since_epoch().count());
        const auto workspace = std::filesystem::absolute(std::filesystem::path("build/graphics") / name);
        std::filesystem::create_directories(workspace / "src");
        { std::ofstream file(workspace / "src/main.cpp", std::ios::binary); file << "// GUI acceptance\nint main() { return 0; }\n"; }
        { std::ofstream file(workspace / "README.md", std::ios::binary); file << "# Astra GUI acceptance\n"; }
        { std::ofstream file(workspace / "rename.txt", std::ios::binary); file << "needle and needle\n"; }
        const auto long_line = std::string(70'000, 'a') + "\t中文🙂";
        { std::ofstream file(workspace / "long.txt", std::ios::binary); file << long_line; }
        { std::ofstream file(workspace / ".editorconfig", std::ios::binary); file << "root = true\n[*]\nindent_style = space\nindent_size = 4\nend_of_line = lf\n"; }
        { std::ofstream file(workspace / ".clangd", std::ios::binary); file << "CompileFlags:\n  Add: [-nostdinc, -nostdinc++]\n"; }
        astra::service::ServiceHostOptions service_options;
        service_options.workspace = workspace;
        service_options.recovery_directory = workspace.parent_path() / (name + "-recovery");
        service_options.resource_directory = std::filesystem::current_path();
        service_options.defaults_script = astra::io::read_file("data/defaults.js", 1024 * 1024).text;
        auto host = std::make_shared<astra::service::ServiceHost>(std::move(service_options));
        std::cerr << "GUI acceptance: creating window\n";
        astra::ui::EditorWindowOptions options;
        options.title = "Astra — GUI acceptance"; options.workspace_name = "GUI ACCEPTANCE";
        options.open_paths = {"src/main.cpp"}; options.width = 1400; options.height = 860;
        std::shared_ptr<astra::ui::EditorChannel> channel = host;
        if (remote) { channel = std::make_shared<astra::transport::TcpEditorChannel>("127.0.0.1", host->port()); }
        astra::ui::EditorWindow window(channel, std::move(options));
        settle(window);
        std::cerr << "GUI acceptance: initial viewport and syntax ready\n";
        const auto id = window.active_document_id();
        require(!id.empty(), "Workspace file did not open in the editor");
        require(read_document(host, id).at("text") == "// GUI acceptance\nint main() { return 0; }\n", "Opened wrong file");

        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, "alpha\nbeta");
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        key(window, SDLK_DOWN, SDL_KMOD_CTRL | SDL_KMOD_ALT);
        require(read_document(host, id).at("selections").size() == 2, "Add cursor shortcut did not reach the service");
        text(window, "X");
        require(read_document(host, id).at("text") == "Xalpha\nXbeta", "Multicursor text entry was not atomic");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "alpha\nbeta", "One undo did not restore every cursor edit");
        key(window, SDLK_Y, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "Xalpha\nXbeta", "Redo did not restore multicursor text");

        key(window, SDLK_A, SDL_KMOD_CTRL);
        key(window, SDLK_C, SDL_KMOD_CTRL);
        SDL_Event composing{}; composing.type = SDL_EVENT_TEXT_EDITING; composing.edit.text = "中文";
        composing.edit.start = 0; composing.edit.length = 2; composing.edit.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&composing), "IME composition event could not be queued"); settle(window);
        require(read_document(host, id).at("text") == "Xalpha\nXbeta", "IME preedit modified the document before commit");
        text(window, "中文🙂 e\xCC\x81");
        require(read_document(host, id).at("text") == "中文🙂 e\xCC\x81", "Unicode input failed");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        key(window, SDLK_V, SDL_KMOD_CTRL);
        const auto pasted = read_document(host, id).at("text");
        require(pasted == "Xalpha\nXbeta", "Production clipboard paste failed: " + pasted.dump());
        std::cerr << "GUI acceptance: clipboard and multicursor passed\n";

        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        key(window, SDLK_F, SDL_KMOD_CTRL); text(window, "alpha"); key(window, SDLK_RETURN);
        require(read_document(host, id).at("selections").front().at("anchor") == 1, "Find selected the wrong UTF-8 match");
        key(window, SDLK_H, SDL_KMOD_CTRL); key(window, SDLK_RETURN); text(window, "ALPHA"); key(window, SDLK_RETURN);
        require(read_document(host, id).at("text") == "XALPHA\nXbeta", "Replace did not edit the document");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "Xalpha\nXbeta", "Document replace was not one undo transaction");
        std::cerr << "GUI acceptance: document search passed\n";

        window.execute_ui_command("ui.workspaceReplace"); settle(window);
        for (unsigned i = 0; i < 5; ++i) { key(window, SDLK_BACKSPACE); }
        text(window, "needle"); key(window, SDLK_RETURN); text(window, "thread"); key(window, SDLK_RETURN);
        require(astra::io::read_file(workspace / "rename.txt").text == "needle and needle\n", "Workspace replace ran before confirmation");
        window.request_screenshot("build/graphics/gui-replace-confirm.png"); settle(window);
        window.execute_ui_command("ui.confirmReplace"); settle(window);
        require(astra::io::read_file(workspace / "rename.txt").text == "thread and thread\n", "Workspace replace did not update disk");
        std::cerr << "GUI acceptance: workspace replace passed\n";

        window.execute_ui_command("ui.closeTab");
        window.request_screenshot("build/graphics/gui-dirty-close.png");
        settle(window);
        require(window.active_document_id() == id, "Dirty close silently discarded a document");
        window.execute_ui_command("ui.cancel"); settle(window);
        key(window, SDLK_S, SDL_KMOD_CTRL);
        require(!read_document(host, id).at("dirty").get<bool>(), "Save did not mark the persisted revision clean");
        require(astra::io::read_file(workspace / "src/main.cpp").text == "Xalpha\nXbeta", "Save output differs from the service document");

        window.execute_ui_command("ui.closeTab"); settle(window);
        require(window.active_document_id().empty(), "Clean close did not close the tab");
        window.open_path("src/main.cpp"); settle(window);
        require(read_document(host, window.active_document_id()).at("text") == "Xalpha\nXbeta", "Reopen lost saved text");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, "// Astra · real editing, real pixels\n\n#include <iostream>\n\nint main() {\n    // 中文、emoji 🙂 与组合字符 e\xCC\x81\n    std::cout << \"Hello, Astra\" << '\\n';\n    return 0;\n}\n");
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        for (unsigned i = 0; i < 4; ++i) { key(window, SDLK_DOWN); }
        window.execute_ui_command("ui.selectFunction"); settle(window);
        const auto selected_function = read_document(host, window.active_document_id()).at("selections").front();
        require(selected_function.at("head") != selected_function.at("anchor"), "Syntax object selection did not use the parsed function");
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        window.execute_ui_command("ui.explorer"); settle(window);
        window.request_screenshot(remote ? "build/graphics/gui-editor-tcp.png" : "build/graphics/gui-editor.png"); settle(window);
        window.execute_ui_command("ui.commandPalette");
        window.request_screenshot("build/graphics/gui-palette.png"); settle(window);
        key(window, SDLK_ESCAPE);
        window.open_path("long.txt"); settle(window);
        key(window, SDLK_END, SDL_KMOD_CTRL); text(window, "!"); key(window, SDLK_S, SDL_KMOD_CTRL);
        require(astra::io::read_file(workspace / "long.txt").text == long_line + "!", "Long-line continuation or caret clipped text beyond 64 KiB");
        window.execute_ui_command("ui.closeTab"); settle(window);
        std::cout << "GUI " << (remote ? "TCP" : "local") << " real SDL input -> JS policy -> shared dispatcher: open, Unicode, multicursor, undo/redo, clipboard, find/replace, workspace replace, syntax objects, dirty close, save/reopen: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI smoke failed: " << error.what() << '\n';
        return 1;
    }
}
