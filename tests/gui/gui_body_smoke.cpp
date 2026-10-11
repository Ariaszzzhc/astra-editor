#include <astra/io/file_store.hpp>
#include <astra/service/service_host.hpp>
#include <astra/ui/editor_window.hpp>
#include "support/temporary_directory.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace {
using Window = astra::ui::EditorWindow;
using Json = astra::protocol::Json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}

void settle(Window& window, bool allow_error = false) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    do {
        require(window.pump(), "Body window closed unexpectedly");
        require(allow_error || window.last_error().empty(), window.last_error());
        if (window.idle())
            return;
        SDL_Delay(1);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Body workbench did not settle");
}

void click_box(Window& window, const Json& box, bool allow_error = false) {
    int width{}, height{}, pixels_x{}, pixels_y{};
    require(SDL_GetWindowSize(window.window(), &width, &height) &&
                SDL_GetWindowSizeInPixels(window.window(), &pixels_x, &pixels_y),
            "Cannot read body dimensions");
    const auto x = box.at("x").get<float>() + box.at("width").get<float>() / 2;
    const auto y = box.at("y").get<float>() + box.at("height").get<float>() / 2;
    for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
        SDL_Event event{};
        event.type = type;
        event.button.windowID = SDL_GetWindowID(window.window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.x = x * static_cast<float>(width) / static_cast<float>(pixels_x);
        event.button.y = y * static_cast<float>(height) / static_cast<float>(pixels_y);
        require(SDL_PushEvent(&event), "Cannot enqueue body pointer");
        settle(window, allow_error);
    }
}

void click_row(Window& window, std::string_view bank, std::string_view row) {
    const auto targets = window.view_state().chrome.at("targets");
    const auto found = std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
        return target.at("bank").template get_ref<const std::string&>() == bank &&
               target.at("row").template get_ref<const std::string&>() == row;
    });
    require(found != targets.end(), "Body target is not visible: " + std::string(row));
    click_box(window, found->at("bounds"));
}

void escape(Window& window) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = SDL_GetWindowID(window.window());
    event.key.key = SDLK_ESCAPE;
    event.key.down = true;
    require(SDL_PushEvent(&event), "Cannot enqueue body Escape");
    settle(window);
}

void run(const fs::path& workspace) {
    static_cast<void>(astra::io::save_file(
        workspace / "source.txt", "Preserved 中🙂\n", astra::io::TextEncoding::utf8));
    astra::service::ServiceHostOptions service;
    service.workspace = workspace;
    service.resource_directory = fs::current_path();
    service.defaults_script = astra::io::read_file("resources/defaults.js").text;
    service.watch_files = false;
    auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
    astra::ui::EditorWindowOptions options;
    options.title = "Astra — JS body acceptance";
    options.workspace_name = "BODY";
    options.open_paths = {"source.txt"};
    options.resource_directory = fs::current_path();
    options.user_script = R"js(
if (astra.context === 'client') {
    let previous, capturedNotice;
    astra.registerCommand('acceptance.body.firstNotice', async () => {
        const { document } = await astra.ui.read({ immediate: true });
        await astra.ui.update({
            workspaceNotice: { id: document.id, text: 'L'.repeat(5000) + ' First' }
        });
    });
    astra.registerCommand('acceptance.body.captureNotice', async () => {
        capturedNotice = (await astra.ui.chrome.read()).version;
    });
    astra.registerCommand('acceptance.body.secondNotice', async () => {
        const { document } = await astra.ui.read({ immediate: true });
        await astra.ui.update({
            workspaceNotice: { id: document.id, text: 'L'.repeat(5000) + ' Second' }
        });
    });
    astra.registerCommand('acceptance.body.staleNotice', async () => {
        await astra.ui.execute('workbench.chrome.activate', {
            version: capturedNotice,
            bank: 'notices',
            row: 'workspace'
        });
    });
    astra.registerCommand('acceptance.body.delayedDismissal', () => {
        astra.registerCommand('ui.dismissWorkspaceNotice', async (parameters) => {
            const { document } = await astra.ui.read({ immediate: true });
            await astra.ui.update({
                workspaceNotice: {
                    id: document.id,
                    text: 'L'.repeat(5000) + ' Third'
                }
            });
            const result = await astra.ui.native('ui.dismissWorkspaceNotice', parameters);
            if (result !== false) throw Error('Late notice action was accepted');
            await astra.ui.update({ status: 'New warning preserved' });
        });
    });
    astra.registerCommand('acceptance.body.restoreDismissal', () => {
        astra.registerCommand('ui.dismissWorkspaceNotice', (parameters) =>
            astra.ui.native('ui.dismissWorkspaceNotice', parameters)
        );
    });
    astra.registerCommand('acceptance.body.notice', async () => {
        await astra.ui.update({ notice: 'External change 中🙂 — buffer preserved' });
    });
    astra.registerCommand('acceptance.body.custom', async () => {
        previous = astra.ui.registerChrome(
            async (context, options) => {
                await astra.sleep(8, options);
                const model = await previous.provider(context, options);
                model.welcomeTitle = [{ id: 'title', label: 'Write 中🙂' }];
                model.welcomeDetail = [{ id: 'detail', label: 'Original JS captions' }];
                model.welcomeActions = [
                    {
                        id: 'custom',
                        label: 'Custom action 中🙂',
                        badge: 'Async',
                        icon: 'braces',
                        data: { action: 'custom' }
                    }
                ];
                model.notices = [
                    {
                        id: 'custom-notice',
                        label: 'Custom notice 中🙂',
                        badge: 'Review',
                        tone: 'changed',
                        data: { action: 'notice' }
                    }
                ];
                return model;
            },
            {
                activate: async (data) => {
                    await astra.ui.update({ status: 'Custom body ' + data.action });
                }
            }
        );
        await astra.ui.style.update({ workbench: { uiFontSize: 16 } });
        await astra.ui.chrome.refresh();
    });
    astra.registerCommand('acceptance.body.fail', async () => {
        astra.ui.registerChrome(async () => {
            throw Error('Body provider failure 中🙂');
        });
        await astra.ui.chrome.refresh();
    });
    astra.registerCommand('acceptance.body.restore', async () => {
        astra.ui.registerChrome(previous.provider, previous.handlers, {
            timeoutMs: previous.timeoutMs
        });
        await astra.ui.update({ error: '' });
        await astra.ui.style.update({ workbench: { uiFontSize: 13 } });
        await astra.ui.chrome.refresh();
    });
}
)js";
    Window window(host, std::move(options));
    settle(window);
    for (int attempt = 0; !window.active_document_id().empty() && attempt < 8; ++attempt) {
        window.execute_ui_command("ui.closeTab");
        settle(window);
    }
    require(window.active_document_id().empty(), "Clean close did not reveal welcome");
    require(window.view_state().chrome.at("model").at("welcomeTitle")[0].at("label") == "Astra",
            "Default welcome was not supplied by JS");
    window.request_screenshot("build/graphics/gui-body-welcome.png");
    settle(window);
    click_row(window, "welcomeActions", "new");
    require(!window.active_document_id().empty(), "Welcome New did not create a document");
    const auto document_targets = window.view_state().chrome.at("targets");
    for (const auto& target : document_targets)
        require(target.at("bank") != "welcomeActions",
                "Welcome target overlays a created document");
    window.execute_ui_command("ui.closeTab");
    settle(window);
    click_row(window, "welcomeActions", "open");
    require(window.view_state().input_layout.contains("bytes"),
            "Welcome Open did not invoke Quick Open");
    escape(window);
    click_row(window, "welcomeActions", "commands");
    require(window.view_state().input_layout.contains("bytes"),
            "Welcome Commands did not open the palette");
    escape(window);
    window.execute_ui_command("acceptance.body.notice");
    settle(window);
    require(window.workspace_notice().find("buffer preserved") != std::string::npos,
            "Notice is missing");
    click_row(window, "notices", "workspace");
    require(window.workspace_notice().empty(),
            "JS notice action did not dismiss its captured warning");
    window.open_path("source.txt");
    settle(window);
    window.execute_ui_command("acceptance.body.firstNotice");
    settle(window);
    window.execute_ui_command("acceptance.body.captureNotice");
    settle(window);
    const auto first_notice = window.view_state().chrome.at("context").at("notice");
    const auto first_sequence = window.view_state().chrome.at("context").at("noticeSequence");
    window.execute_ui_command("acceptance.body.secondNotice");
    settle(window);
    require(first_notice == window.view_state().chrome.at("context").at("notice"),
            "Long-notice fixture did not preserve the same display prefix");
    require(first_sequence != window.view_state().chrome.at("context").at("noticeSequence"),
            "Original notices share the same action identity");
    window.execute_ui_command("acceptance.body.staleNotice");
    settle(window);
    require(window.workspace_notice().ends_with(" Second"),
            "Stale notice action dismissed a newer original with the same display prefix");
    const auto notice_selections = window.view_state().document.at("selections");
    window.execute_ui_command("acceptance.body.delayedDismissal");
    settle(window);
    click_row(window, "notices", "workspace");
    require(window.workspace_notice().ends_with(" Third") &&
                window.status_text() == "New warning preserved" &&
                window.view_state().document.at("selections") == notice_selections,
            "Asynchronous dismissal cleared a warning arriving inside its handler");
    window.execute_ui_command("acceptance.body.restoreDismissal");
    settle(window);
    window.execute_ui_command("ui.dismissWorkspaceNotice");
    settle(window);
    require(window.workspace_notice().empty(),
            "Explicit dismissal cannot clear the current warning");
    window.execute_ui_command("ui.closeTab");
    settle(window);
    window.execute_ui_command("acceptance.body.notice");
    settle(window);
    window.execute_ui_command("acceptance.body.custom");
    settle(window);
    const auto scale = window.layout().scale;
    require(SDL_SetWindowSize(
                window.window(), static_cast<int>(480 * scale), static_cast<int>(420 * scale)),
            "Cannot resize body fixture");
    settle(window);
    require(window.view_state().chrome.at("model").at("welcomeTitle")[0].at("label") ==
                "Write 中🙂",
            "Custom welcome caption was not retained");
    click_row(window, "welcomeActions", "custom");
    require(window.status_text() == "Custom body custom" && window.active_document_id().empty(),
            "Custom welcome action fell through to native New policy");
    window.request_screenshot("build/graphics/gui-body-custom.png");
    settle(window);
    window.open_path("source.txt");
    settle(window);
    const auto selections = window.view_state().document.at("selections");
    click_row(window, "notices", "custom-notice");
    require(window.status_text() == "Custom body notice" && !window.workspace_notice().empty() &&
                window.view_state().document.at("selections") == selections,
            "Custom notice action fell through to native dismissal or editor selection");
    window.execute_ui_command("acceptance.body.fail");
    settle(window, true);
    require(window.last_error().find("Body provider failure") != std::string::npos,
            "Provider error disappeared from native state");
    const auto diagnostic = window.view_state().chrome.at("diagnostic");
    require(diagnostic.at("height").get<float>() > 0, "Broken provider hid host diagnostics");
    window.request_screenshot("build/graphics/gui-body-diagnostic.png");
    settle(window, true);
    click_box(window, diagnostic, true);
    require(window.view_state().document.at("selections") == selections,
            "Host diagnostic pointer fell through to editor selection");
    window.execute_ui_command("acceptance.body.restore");
    settle(window);
    click_row(window, "notices", "workspace");
    require(window.workspace_notice().empty(),
            "Restored notice handler did not dismiss the warning");
    require(astra::io::read_file(workspace / "source.txt").text == "Preserved 中🙂\n",
            "Body actions changed the source file");
    window.execute_ui_command("ui.closeWindow");
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    while (window.pump()) {
        require(Clock::now() < deadline, "Body workbench did not close");
        require(window.last_error().empty(), window.last_error());
        SDL_Delay(1);
    }
}
} // namespace

int main() {
    fs::path workspace;
    try {
        const auto parent = fs::weakly_canonical(fs::absolute("build/graphics"));
        fs::create_directories(parent);
        workspace =
            parent / ("gui-body-" + std::to_string(Clock::now().time_since_epoch().count()));
        require(fs::create_directory(workspace), "Cannot create body fixture");
        run(workspace);
        const auto issues = astra::test::cleanup_owned_directory(workspace, parent, "gui-body-");
        require(issues.empty(), "Body fixture cleanup failed");
        std::cout << "Windows GUI JS body: default welcome actions, notice dismissal, custom "
                     "Unicode provider, font/resize, diagnostic failure and restoration: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI body failed: " << error.what() << "\nPreserved fixture: " << workspace
                  << '\n';
        return 1;
    }
}
