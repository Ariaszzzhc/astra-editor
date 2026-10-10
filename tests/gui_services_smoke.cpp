#include <astra/ui/editor_window.hpp>
#include <astra/service/service_host.hpp>
#include <astra/io/file_store.hpp>
#include <astra/process/process.hpp>
#include <astra/git/repository.hpp>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <algorithm>
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
namespace fs = std::filesystem;

void require(bool value, const std::string& message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::string path_text(const fs::path& path) {
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void write(const fs::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void settle(astra::ui::EditorWindow& window) {
    const auto deadline = Clock::now() + std::chrono::seconds(40);
    do {
        require(window.pump(), "GUI closed during service acceptance");
        require(window.last_error().empty(), "GUI service action failed: " + window.last_error());
        if (window.idle()) {
            return;
        }
        SDL_Delay(2);
    } while (Clock::now() < deadline);
    throw std::runtime_error("GUI service action timed out");
}

void key(astra::ui::EditorWindow& window,
         SDL_Keycode keycode,
         SDL_Keymod modifiers = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = keycode;
    event.key.mod = modifiers;
    event.key.down = true;
    event.key.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Queue key event");
    settle(window);
}

void text(astra::ui::EditorWindow& window, const std::string& value) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = value.c_str();
    event.text.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Queue text event");
    settle(window);
}

Json rpc(const std::shared_ptr<astra::service::ServiceHost>& host,
         std::string method,
         Json params) {
    auto pending = host->submit(Json{
        {"jsonrpc", "2.0"},
        {"id", "gui-service-test"},
        {"method", std::move(method)},
        {"params",
         std::move(params)}}.dump());
    require(pending.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "Verification RPC timed out");
    const auto body = pending.get();
    require(body.has_value(), "Verification RPC has no response");
    const auto result = Json::parse(*body);
    require(!result.contains("error"), "Verification RPC failed: " + result.dump());
    return result.at("result");
}

Json document(const std::shared_ptr<astra::service::ServiceHost>& host, const std::string& id) {
    return rpc(host, "document.get", {{"id", id}});
}

void caret(astra::ui::EditorWindow& window,
           const std::shared_ptr<astra::service::ServiceHost>& host,
           std::size_t offset) {
    const auto id = window.active_document_id();
    const auto state = document(host, id);
    (void)rpc(host,
              "document.setSelections",
              {{"id", id},
               {"baseRevision", state.at("revision")},
               {"selections", Json::array({{{"anchor", offset}, {"head", offset}}})}});
    settle(window);
}

void git_command(const fs::path& directory, std::vector<std::string> args) {
    astra::process::ProcessOptions options;
    options.cwd = path_text(directory);
    options.argv = {"git",
                    "--no-pager",
                    "-c",
                    "user.name=Astra GUI Tests",
                    "-c",
                    "user.email=gui@astra.invalid",
                    "-c",
                    "core.hooksPath=NUL",
                    "-c",
                    "commit.gpgsign=false",
                    "-c",
                    "core.autocrlf=false"};
    options.argv.insert(options.argv.end(), args.begin(), args.end());
    options.env = astra::process::current_environment();
    std::erase_if(*options.env, [](const auto& item) {
        return item.starts_with("GIT_");
    });
    options.env->insert(options.env->end(),
                        {"GIT_CONFIG_NOSYSTEM=1",
                         "GIT_CONFIG_SYSTEM=NUL",
                         "GIT_CONFIG_GLOBAL=NUL",
                         "GIT_TERMINAL_PROMPT=0"});
    std::string output;
    astra::process::Process process(std::move(options),
                                    {{},
                                     [&](std::string_view part) {
                                         output += part;
                                     },
                                     [&](std::string_view part) {
                                         output += part;
                                     }});
    process.start().get();
    require(process.completion().wait_for(std::chrono::seconds(15)) == std::future_status::ready,
            "Git fixture command timed out");
    const auto result = process.completion().get();
    require(result.exit_status == 0 && result.error.empty(),
            "Git fixture command failed: " + output + result.error);
}
} // namespace

int main() {
    try {
        std::cerr << std::unitbuf << "GUI services: create isolated Git/C++ workspace\n";
        const auto workspace = fs::absolute(
            fs::path("build/graphics") /
            ("gui-services-" + std::to_string(Clock::now().time_since_epoch().count())));
        fs::create_directories(workspace);
        const std::string original =
            "int square(int value) { return value * value; }\nint main() {\n    return squ;\n}\n";
        write(workspace / "main.cpp", original);
        write(workspace / ".clangd",
              "CompileFlags:\n  Add: [-std=c++23, -nostdinc, -nostdinc++]\n");
        write(workspace / ".gitignore", ".cache/\n");
        git_command(workspace, {"init", "--initial-branch=main"});
        git_command(workspace, {"add", "."});
        git_command(workspace, {"commit", "-m", "test: initialize GUI fixture"});
        astra::service::ServiceHostOptions service;
        service.workspace = workspace;
        service.resource_directory = fs::current_path();
        service.recovery_directory =
            workspace.parent_path() / (workspace.filename().string() + "-recovery");
        service.defaults_script = astra::io::read_file("data/defaults.js").text;
        service.user_script = "astra.configure({git:{inheritGlobalConfig:false,authorName:'Astra "
                              "GUI Tests',authorEmail:'gui@astra.invalid'}});";
        auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
        astra::ui::EditorWindowOptions options;
        options.title = "Astra — real clangd and Git UI acceptance";
        options.workspace_name = "LANGUAGE + GIT";
        options.open_paths = {"main.cpp"};
        astra::ui::EditorWindow window(host, std::move(options));
        settle(window);
        const auto id = window.active_document_id();
        caret(window, host, original.find("squ;") + 3);
        key(window, SDLK_SPACE, SDL_KMOD_CTRL);
        text(window, "square");
        window.request_screenshot("build/graphics/gui-completion.png");
        settle(window);
        key(window, SDLK_RETURN);
        require(document(host, id).at("text").get<std::string>().find("return square") !=
                    std::string::npos,
                "Completion did not insert the clangd item");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(document(host, id).at("text") == original, "Completion undo was not atomic");
        bool diagnostic_received{};
        Json last_diagnostics;
        const auto diagnostic_deadline = Clock::now() + std::chrono::seconds(15);
        while (Clock::now() < diagnostic_deadline) {
            settle(window);
            last_diagnostics = rpc(host, "language.diagnostics", {{"id", id}});
            if (!last_diagnostics.at("diagnostics").empty()) {
                diagnostic_received = true;
                break;
            }
            SDL_Delay(30);
        }
        require(diagnostic_received,
                "Real clangd diagnostic for the unresolved squ identifier was not delivered: " +
                    last_diagnostics.dump());
        window.execute_ui_command("ui.diagnostics");
        settle(window);
        window.request_screenshot("build/graphics/gui-diagnostics.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        std::cerr << "GUI services: real clangd completion passed\n";

        const std::string valid =
            "int square(int value){return value*value;}\nint main(){return square(2);}\n";
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, valid);
        caret(window, host, valid.rfind("square") + 2);
        key(window, SDLK_F12);
        require(document(host, id).at("selections").front().at("anchor") == 4,
                "Definition did not navigate to the declared symbol");
        caret(window, host, 5);
        window.execute_ui_command("ui.hover");
        settle(window);
        window.request_screenshot("build/graphics/gui-hover.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        key(window, SDLK_F12, SDL_KMOD_SHIFT);
        window.request_screenshot("build/graphics/gui-references.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        key(window, SDLK_F2);
        text(window, "cube");
        key(window, SDLK_RETURN);
        require(document(host, id).at("text") == valid,
                "Rename mutated buffers before confirmation");
        window.request_screenshot("build/graphics/gui-rename-confirm.png");
        settle(window);
        key(window, SDLK_RETURN);
        auto renamed = document(host, id).at("text").get<std::string>();
        require(renamed.find("square") == std::string::npos &&
                    renamed.find("cube(2)") != std::string::npos,
                "Rename failed to apply the previewed edits");
        window.execute_ui_command("ui.format");
        settle(window);
        const auto formatted = document(host, id).at("text").get<std::string>();
        require(formatted != renamed &&
                    formatted.find("return value * value;") != std::string::npos,
                "Formatting did not apply clangd edits");
        key(window, SDLK_S, SDL_KMOD_CTRL);
        std::cerr << "GUI services: hover, definition, references, rename, format passed\n";

        key(window, SDLK_G, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
        key(window, SDLK_F6);
        key(window, SDLK_HOME);
        require(window.view_state().sidebar.at("id") == "git" &&
                    window.view_state().sidebar.at("row").at("id") == "main.cpp",
                "Source control keyboard did not select its indexed file row");
        key(window, SDLK_RETURN);
        require(window.view_state().sidebar.at("focused") == false,
                "Git diff activation did not transfer focus from its row widget");
        window.request_screenshot("build/graphics/gui-git-diff.png");
        settle(window);
        key(window, SDLK_S);
        astra::git::Repository repository(workspace, "git", {false});
        auto status = repository.status();
        require(std::ranges::any_of(status.entries,
                                    [](const auto& entry) {
                                        return entry.path == "main.cpp" &&
                                               entry.staged == astra::git::FileState::modified;
                                    }),
                "GUI stage did not update the Git index");
        window.execute_ui_command("ui.gitUnstage");
        settle(window);
        status = repository.status();
        require(std::ranges::none_of(status.entries,
                                     [](const auto& entry) {
                                         return entry.staged != astra::git::FileState::unchanged;
                                     }),
                "GUI unstage did not reset the index");
        window.execute_ui_command("ui.gitStage");
        settle(window);
        window.execute_ui_command("ui.gitCommit");
        settle(window);
        text(window, "test: accept GUI service edits");
        key(window, SDLK_RETURN);
        require(repository.log(2).front().subject == "test: accept GUI service edits",
                "GUI commit did not commit staged files");
        key(window, SDLK_END, SDL_KMOD_CTRL);
        text(window, "// temporary change\n");
        key(window, SDLK_S, SDL_KMOD_CTRL);
        window.execute_ui_command("ui.gitDiff");
        settle(window);
        key(window, SDLK_D);
        require(astra::io::read_file(workspace / "main.cpp").text != formatted,
                "Git discard ran before confirmation");
        window.request_screenshot("build/graphics/gui-git-discard.png");
        settle(window);
        key(window, SDLK_RETURN);
        require(astra::io::read_file(workspace / "main.cpp").text == formatted,
                "Git discard did not restore the previewed baseline");
        require(document(host, id).at("text") == formatted,
                "Git discard left the open clean buffer stale");
        std::cout << "Real Windows GUI -> shared service -> clangd/Git: completion, navigation, "
                     "hover, references, rename confirmation, formatting, diff, stage/unstage, "
                     "commit, guarded discard: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI services failed: " << error.what() << '\n';
        return 1;
    }
}
