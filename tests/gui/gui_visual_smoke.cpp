#include <astra/io/file_store.hpp>
#include <astra/process/process.hpp>
#include <astra/service/service_host.hpp>
#include <astra/ui/editor_window.hpp>

#include "support/temporary_directory.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
using Window = astra::ui::EditorWindow;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void settle(Window& window) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    do {
        require(window.pump(), "Default workbench closed during acceptance");
        require(window.last_error().empty(), window.last_error());
        if (window.idle()) {
            return;
        }
        SDL_Delay(2);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Default workbench did not settle: " + window.status_text());
}

void key(Window& window, SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = code;
    event.key.mod = modifiers;
    event.key.down = true;
    event.key.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Cannot send default keybinding input");
    settle(window);
}

void text(Window& window, const char* value) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = value;
    event.text.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Cannot send default text input");
    settle(window);
}

void capture(Window& window, const std::string& name) {
    const auto path = fs::path("build/graphics/default-ui") / (name + ".png");
    window.request_screenshot(path);
    settle(window);
    require(fs::is_regular_file(path) && fs::file_size(path) > 1000,
            "Default workbench screenshot is missing: " + path.string());
}

void git(const fs::path& workspace, std::vector<std::string> arguments) {
    astra::process::ProcessOptions options;
    options.argv = {"git",
                    "--no-pager",
                    "-c",
                    "core.hooksPath=NUL",
                    "-c",
                    "commit.gpgsign=false",
                    "-c",
                    "core.autocrlf=false"};
    options.argv.insert(options.argv.end(), arguments.begin(), arguments.end());
    const auto encoded = workspace.u8string();
    options.cwd = std::string(encoded.begin(), encoded.end());
    options.env = astra::process::current_environment();
    std::erase_if(*options.env, [](const auto& item) {
        return item.starts_with("GIT_");
    });
    options.env->insert(options.env->end(),
                        {"GIT_CONFIG_GLOBAL=NUL",
                         "GIT_CONFIG_SYSTEM=NUL",
                         "GIT_CONFIG_NOSYSTEM=1",
                         "GIT_TERMINAL_PROMPT=0"});
    options.max_stdout_bytes = 1024 * 1024;
    astra::process::Process process(std::move(options));
    process.start().get();
    auto done = process.completion();
    require(done.wait_for(std::chrono::seconds(15)) == std::future_status::ready,
            "Visual fixture Git command timed out");
    const auto result = done.get();
    require(result.error.empty() && result.exit_status == 0,
            "Visual fixture Git command failed: " + result.error);
}

void run(const fs::path& fixture) {
    const auto workspace = fixture / "workspace";
    fs::create_directories(workspace / "src");
    fs::create_directories(workspace / "scripts");
    const std::string source = "// Astra: a focused place to write code.\n"
                               "namespace editor {\n\n"
                               "struct Position {\n"
                               "    int line{};\n"
                               "    int column{};\n"
                               "};\n\n"
                               "int advance(Position position, int distance) {\n"
                               "    return position.column + distance;\n"
                               "}\n\n"
                               "bool is_valid(Position position) {\n"
                               "    return position.line >= 0 && position.column >= 0;\n"
                               "}\n\n"
                               "} // namespace editor\n";
    const auto source_version = astra::io::save_file(workspace / "src/main.cpp", source);
    (void)astra::io::save_file(
        workspace / "scripts/commands.js",
        "export async function saveDocument(id, signal) {\n"
        "    return await astra.request('document.save', { id }, { signal });\n"
        "}\n");
    (void)astra::io::save_file(workspace / "README.md",
                               "# Astra\n\nA focused place to write code.\n");
    (void)astra::io::save_file(workspace / ".gitignore", ".cache/\nrecovery/\n");
    git(workspace, {"init", "--initial-branch=main"});
    git(workspace, {"config", "--local", "user.name", "Astra Visual Acceptance"});
    git(workspace, {"config", "--local", "user.email", "visual@astra.invalid"});
    git(workspace, {"add", "."});
    git(workspace, {"commit", "-m", "test: initialize visual fixture"});
    (void)astra::io::save_file(workspace / "src/main.cpp",
                               source + "// Windows acceptance\n",
                               astra::io::TextEncoding::utf8,
                               source_version);

    astra::service::ServiceHostOptions service;
    service.workspace = workspace;
    service.resource_directory = fs::current_path();
    service.recovery_directory = fixture / "recovery";
    service.defaults_script = astra::io::read_file("resources/defaults.js").text;
    // Isolate Git configuration only. All shipped UI/theme/keymap providers are unchanged.
    service.user_script = "astra.configure({git:{inheritGlobalConfig:false}});";
    auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
    {
        astra::ui::EditorWindowOptions options;
        options.workspace_name = "astra-workspace";
        options.open_paths = {"README.md", "scripts/commands.js", "src/main.cpp"};
        Window window(host, std::move(options));
        settle(window);
        // Startup opens can complete in any order. Select the C++ tab through
        // the same shipped open workflow before expecting its symbols.
        window.open_path("src/main.cpp");
        settle(window);
        const auto style = window.view_state().style;
        require(style.at("workbench").at("uiFontSize") == 13 &&
                    style.at("theme").at("background") == "#1F1F1F",
                "Visual acceptance did not use the shipped default style");
        // Outline ships collapsed. Exercise its actual command instead of
        // assuming that a hidden provider has already enumerated symbols.
        window.execute_ui_command("ui.toggleOutline");
        settle(window);
        require(window.view_state().tabs.size() == 3 && window.view_state().outline_total > 0,
                "Default editor did not publish tabs and Outline");
        capture(window, "01-editor-explorer-outline");
        key(window, SDLK_P, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
        capture(window, "02-command-palette");
        key(window, SDLK_ESCAPE);
        key(window, SDLK_P, SDL_KMOD_CTRL);
        text(window, "main");
        capture(window, "03-quick-open");
        key(window, SDLK_ESCAPE);
        key(window, SDLK_F, SDL_KMOD_CTRL);
        text(window, "position");
        capture(window, "04-document-find");
        key(window, SDLK_ESCAPE);
        key(window, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
        text(window, "position");
        key(window, SDLK_RETURN);
        require(window.view_state().sidebar.at("id") == "search",
                "Default search keybinding did not activate search");
        capture(window, "05-workspace-search");
        key(window, SDLK_G, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
        require(window.view_state().sidebar.at("id") == "git",
                "Default Git keybinding did not activate source control");
        capture(window, "06-source-control");
        window.execute_ui_command("ui.gitDiff");
        settle(window);
        capture(window, "07-git-diff");
        key(window, SDLK_ESCAPE);
        window.execute_ui_command("ui.focusEditor");
        settle(window);
        key(window, SDLK_END, SDL_KMOD_CTRL);
        text(window, "// \xe4\xb8\xad\xe6\x96\x87 \xf0\x9f\x99\x82\n");
        key(window, SDLK_W, SDL_KMOD_CTRL);
        capture(window, "08-dirty-close-confirmation");
        key(window, SDLK_ESCAPE);
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        window.execute_ui_command("ui.explorer");
        settle(window);
        require(SDL_SetWindowSize(window.window(), 980, 640), "Cannot resize default workbench");
        settle(window);
        capture(window, "09-narrow-editor");
        window.execute_ui_command("ui.closeWindow");
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (window.pump()) {
            require(window.last_error().empty(), window.last_error());
            require(Clock::now() < deadline, "Default workbench shutdown timed out");
            SDL_Delay(2);
        }
    }
    host->shutdown();
}
} // namespace

int main() {
    fs::path workspace;
    try {
        const auto parent = fs::weakly_canonical(fs::absolute("build/graphics"));
        fs::create_directories(parent / "default-ui");
        workspace =
            parent / ("gui-visual-" + std::to_string(Clock::now().time_since_epoch().count()));
        require(fs::create_directory(workspace), "Cannot create visual fixture");
        run(workspace);
        const auto issues = astra::test::cleanup_owned_directory(workspace, parent, "gui-visual-");
        for (const auto& issue : issues) {
            std::cerr << issue.path << ": " << issue.message << '\n';
        }
        require(issues.empty(), "Visual fixture cleanup failed");
        std::cout << "Windows default workbench visual matrix: nine real production states, "
                     "keybindings, Unicode undo and guarded close: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Default visual acceptance failed: " << error.what()
                  << "\nPreserved fixture: " << workspace << '\n';
        return 1;
    }
}
