#include <astra/ui/editor_window.hpp>
#include <astra/ui/text_layout.hpp>
#include <astra/service/service_host.hpp>
#include <astra/io/file_store.hpp>
#include <astra/process/process.hpp>
#include <astra/git/repository.hpp>

#include <SDL3/SDL.h>
#include "include/codec/SkCodec.h"
#include "include/core/SkData.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
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

astra::ui::TextRect symbol_point(astra::ui::EditorWindow& window,
                                 std::string line,
                                 std::size_t number,
                                 std::size_t byte) {
    const auto geometry = window.layout();
    astra::ui::TextLayoutOptions style;
    style.dpi_scale = geometry.scale;
    style.font_families = {"Cascadia Code", "Consolas", "monospace"};
    astra::ui::TextLayoutEngine fonts;
    const auto paragraph = fonts.layout(std::move(line), style);
    const auto hit = paragraph.caret(byte);
    const auto row_height = std::ceil(16 * geometry.scale * 1.4F + 3 * geometry.scale);
    return {geometry.editor.x + 60 * geometry.scale + hit.x,
            geometry.editor.y + static_cast<float>(number) * row_height + hit.height / 2,
            0,
            0};
}

void pointer(
    astra::ui::EditorWindow& window, float x, float y, bool click = false, bool wait = true) {
    int width{}, height{}, pixels_width{}, pixels_height{};
    require(SDL_GetWindowSize(window.window(), &width, &height) &&
                SDL_GetWindowSizeInPixels(window.window(), &pixels_width, &pixels_height),
            "Pointer geometry");
    const auto logical_x = x * static_cast<float>(width) / static_cast<float>(pixels_width);
    const auto logical_y = y * static_cast<float>(height) / static_cast<float>(pixels_height);
    SDL_Event event{};
    event.type = click ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_MOTION;
    if (click) {
        event.button.windowID = SDL_GetWindowID(window.window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = true;
        event.button.x = logical_x;
        event.button.y = logical_y;
    } else {
        event.motion.windowID = SDL_GetWindowID(window.window());
        event.motion.x = logical_x;
        event.motion.y = logical_y;
    }
    require(SDL_PushEvent(&event), "Queue pointer event");
    if (click) {
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.down = false;
        require(SDL_PushEvent(&event), "Queue pointer release");
    }
    if (wait)
        settle(window);
}

void tooltip_wheel(astra::ui::EditorWindow& window, float x, float y) {
    int width{}, height{}, pixels_width{}, pixels_height{};
    require(SDL_GetWindowSize(window.window(), &width, &height) &&
                SDL_GetWindowSizeInPixels(window.window(), &pixels_width, &pixels_height),
            "Wheel geometry");
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.windowID = SDL_GetWindowID(window.window());
    event.wheel.mouse_x = x * static_cast<float>(width) / static_cast<float>(pixels_width);
    event.wheel.mouse_y = y * static_cast<float>(height) / static_cast<float>(pixels_height);
    event.wheel.y = -2;
    require(SDL_PushEvent(&event), "Queue tooltip wheel");
    settle(window);
}

void await_diagnostics(astra::ui::EditorWindow& window, bool errors) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    do {
        settle(window);
        const auto language = window.view_state().document.at("language");
        if (language.value("available", false) && !language.value("pending", true) &&
            (language.value("errors", 0U) > 0) == errors)
            return;
        SDL_Delay(20);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Automatic inline diagnostics did not reach the current revision");
}

void require_error_pixels(const fs::path& path,
                          astra::ui::Box region,
                          std::uint32_t color = 0xFFF48771) {
    auto data = SkData::MakeFromFileName(path.string().c_str());
    require(data != nullptr, "Diagnostic screenshot missing");
    auto codec = SkCodec::MakeFromData(std::move(data));
    require(codec != nullptr, "Diagnostic PNG decoding failed");
    const auto dimensions = codec->dimensions();
    const auto info = SkImageInfo::Make(
        dimensions.width(), dimensions.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    std::vector<std::uint8_t> pixels(info.computeMinByteSize());
    require(codec->getPixels(info, pixels.data(), info.minRowBytes()) == SkCodec::kSuccess,
            "Diagnostic PNG pixels unavailable");
    std::size_t marked{};
    for (int y = std::max(0, static_cast<int>(region.y));
         y < std::min(dimensions.height(), static_cast<int>(region.bottom()));
         ++y) {
        for (int x = std::max(0, static_cast<int>(region.x));
             x < std::min(dimensions.width(), static_cast<int>(region.right()));
             ++x) {
            const auto offset =
                static_cast<std::size_t>(y) * info.minRowBytes() + static_cast<std::size_t>(x) * 4;
            marked += pixels[offset] == ((color >> 16) & 255) &&
                      pixels[offset + 1] == ((color >> 8) & 255) &&
                      pixels[offset + 2] == (color & 255);
        }
    }
    require(marked >= 3,
            "Diagnostic decoration has no error-colored pixels in its expected region");
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
        const std::string invalid_line = "    /*🙂*/ return squ;";
        const std::string original =
            "int square(int value) { return value * value; }\nint main() {\n" + invalid_line +
            "\n}\n";
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
        service.defaults_script = astra::io::read_file("resources/defaults.js").text;
        service.user_script = "astra.configure({git:{inheritGlobalConfig:false,authorName:'Astra "
                              "GUI Tests',authorEmail:'gui@astra.invalid'}});";
        auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
        astra::ui::EditorWindowOptions options;
        options.title = "Astra — real clangd and Git UI acceptance";
        options.workspace_name = "LANGUAGE + GIT";
        options.open_paths = {"main.cpp"};
        options.user_script = R"js(
            astra.registerCommand('ui.acceptanceLongHover', async () => {
                const state = await astra.ui.editorTooltip.read();
                await astra.ui.editorTooltip.publish({version: state.version, text: 'Unicode 🙂 documentation '.repeat(300)});
            });
        )js";
        astra::ui::EditorWindow window(host, std::move(options));
        settle(window);
        const auto id = window.active_document_id();
        await_diagnostics(window, true);
        window.request_screenshot("build/graphics/gui-diagnostics-inline.png");
        settle(window);
        const auto invalid_point =
            symbol_point(window, invalid_line, 2, invalid_line.find("squ") + 1);
        const auto geometry = window.layout();
        const auto row_height = std::ceil(16 * geometry.scale * 1.4F + 3 * geometry.scale);
        require_error_pixels("build/graphics/gui-diagnostics-inline.png",
                             {geometry.editor.x + 7 * geometry.scale,
                              geometry.editor.y + 2 * row_height,
                              8 * geometry.scale,
                              row_height});
        require_error_pixels("build/graphics/gui-diagnostics-inline.png",
                             {invalid_point.x - 12 * geometry.scale,
                              geometry.editor.y + 2 * row_height + 16 * geometry.scale,
                              45 * geometry.scale,
                              10 * geometry.scale});
        const auto before_hover = document(host, id).at("selections");
        pointer(window, invalid_point.x, invalid_point.y);
        require(window.view_state().editor_tooltip.at("text").get<std::string>().find("squ") !=
                    std::string::npos,
                "Mouse hover did not show the real unresolved-identifier diagnostic");
        require(document(host, id).at("selections") == before_hover, "Hover moved the caret");
        window.request_screenshot("build/graphics/gui-diagnostic-tooltip.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        require(window.view_state().editor_tooltip.at("text") == "",
                "Escape did not dismiss hover");
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

        const std::string valid_line = "int main(){/*🙂*/return square(2);}";
        const std::string valid =
            "int square(int value){return value*value;}\n" + valid_line + "\n";
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, valid);
        await_diagnostics(window, false);
        // Observe every publication, including the frames before the background
        // syntax query completes. Editing whitespace must not remove token colors.
        caret(window, host, valid.size() - 1);
        const auto initial_revision = window.view_state().document_viewport.at("viewRevision");
        SDL_Event entered{};
        entered.type = SDL_EVENT_TEXT_INPUT;
        entered.text.windowID = SDL_GetWindowID(window.window());
        entered.text.text = " ";
        require(SDL_PushEvent(&entered), "Queue continuity input");
        bool published{};
        const auto continuity_deadline = Clock::now() + std::chrono::seconds(40);
        do {
            require(window.pump() && window.last_error().empty(), "Syntax continuity pump failed");
            const auto state = window.view_state().document_viewport;
            if (state.at("viewRevision") != initial_revision) {
                published = true;
                require(state.at("syntaxRevision") == state.at("viewRevision") &&
                            state.at("highlightCount").get<std::size_t>() > 0,
                        "New text was published without its carried syntax colors");
            }
            SDL_Delay(2);
        } while ((!published || !window.idle()) && Clock::now() < continuity_deadline);
        require(published && window.idle(), "Syntax continuity input did not complete");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        caret(window, host, 0);
        const auto valid_point = symbol_point(window, valid_line, 1, valid_line.find("square") + 2);
        pointer(window, valid_point.x, valid_point.y);
        const auto mouse_hover = window.view_state().editor_tooltip;
        require(mouse_hover.at("text").get<std::string>().find("square") != std::string::npos,
                "Mouse hover did not show real clangd symbol information: " + mouse_hover.dump() +
                    " language=" + window.view_state().document.at("language").dump());
        require(document(host, id).at("selections").front().at("head") == 0,
                "Nonmodal symbol hover moved the caret");
        const auto box = mouse_hover.at("bounds");
        require(box.at("height").get<float>() > 0, "Hover was not painted");
        pointer(window, box.at("x").get<float>() + 15, box.at("y").get<float>() + 10);
        require(window.view_state().editor_tooltip.at("text") == mouse_hover.at("text"),
                "Moving into the tooltip discarded its content");
        window.request_screenshot("build/graphics/gui-mouse-hover.png");
        settle(window);
        window.execute_ui_command("ui.acceptanceLongHover");
        settle(window);
        const auto long_hover = window.view_state().editor_tooltip;
        const auto long_box = long_hover.at("bounds");
        const auto first_document_line = window.view_state().document_viewport.at("firstLine");
        pointer(window, long_box.at("x").get<float>() + 15, long_box.at("y").get<float>() + 10);
        tooltip_wheel(
            window, long_box.at("x").get<float>() + 15, long_box.at("y").get<float>() + 10);
        require(window.view_state().editor_tooltip.at("offset").get<float>() > 0,
                "Long wrapped hover did not scroll within its paragraph");
        require(window.view_state().editor_tooltip.at("text") == long_hover.at("text"),
                "Tooltip scrolling discarded its contents");
        require(window.view_state().document_viewport.at("firstLine") == first_document_line,
                "Tooltip wheel scrolled the underlying document");
        window.request_screenshot("build/graphics/gui-long-hover-scroll.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        const auto previous_modifiers = SDL_GetModState();
        SDL_SetModState(SDL_KMOD_CTRL);
        pointer(window, valid_point.x, valid_point.y, false, false);
        bool animated{};
        const auto link_deadline = Clock::now() + std::chrono::seconds(10);
        do {
            require(window.pump() && window.last_error().empty(), "Definition link pump failed");
            const auto link = window.view_state().editor_link;
            if (link.at("available").get<bool>() && link.at("progress").get<float>() < 1)
                animated = true;
            SDL_Delay(2);
        } while (!window.idle() && Clock::now() < link_deadline);
        const auto link = window.view_state().editor_link;
        require(link.at("available").get<bool>() && animated && link.at("progress") == 1,
                "Real definition did not animate its confirmed Ctrl-hover affordance");
        require(link.at("target").at("endByteOffset").get<std::size_t>() -
                        link.at("target").at("byteOffset").get<std::size_t>() ==
                    6,
                "Definition underline does not cover the complete symbol");
        window.request_screenshot("build/graphics/gui-definition-link.png");
        settle(window);
        require_error_pixels("build/graphics/gui-definition-link.png",
                             {valid_point.x - 25 * geometry.scale,
                              geometry.editor.y + row_height,
                              90 * geometry.scale,
                              row_height},
                             0xFF0078D4);
        const auto link_tooltip = window.view_state().editor_tooltip.at("bounds");
        pointer(
            window, link_tooltip.at("x").get<float>() + 15, link_tooltip.at("y").get<float>() + 10);
        require(!window.view_state().editor_link.at("available").get<bool>(),
                "Tooltip content advertised a definition for the obscured document");
        SDL_SetModState(previous_modifiers);
        settle(window);
        require(!window.view_state().editor_link.at("available").get<bool>(),
                "Ctrl release retained a definition underline");
        require(SDL_GetCursor() == SDL_GetDefaultCursor(), "Ctrl release retained the link cursor");
        SDL_SetModState(SDL_KMOD_CTRL);
        pointer(window, valid_point.x, valid_point.y);
        pointer(window, valid_point.x, valid_point.y, true);
        SDL_SetModState(previous_modifiers);
        require(document(host, id).at("selections").front().at("anchor") == 4,
                "Ctrl+click did not navigate from the pointed symbol to its declaration");
        require(document(host, id).at("text") == valid, "Ctrl+click edited the document");
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
                     "mouse/keyboard hover, automatic diagnostic pixels, Ctrl+click, references, "
                     "rename confirmation, formatting, diff, stage/unstage, "
                     "commit, guarded discard: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI services failed: " << error.what() << '\n';
        return 1;
    }
}
