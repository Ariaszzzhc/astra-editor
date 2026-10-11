#include <astra/io/file_store.hpp>
#include <astra/service/service_host.hpp>
#include <astra/ui/editor_window.hpp>

#include "support/temporary_directory.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
using Window = astra::ui::EditorWindow;
using Host = std::shared_ptr<astra::service::ServiceHost>;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Predicate>
void pump_until(Window& window,
                Predicate predicate,
                const std::string& description,
                bool allow_error = false) {
    const auto deadline = Clock::now() + std::chrono::seconds(30);
    do {
        require(window.pump(), "GUI closed while " + description);
        if (!allow_error) {
            require(window.last_error().empty(), description + ": " + window.last_error());
        }
        if (predicate()) {
            return;
        }
        SDL_Delay(2); // Poll a condition with a deadline; do not assume a completion delay.
    } while (Clock::now() < deadline);
    throw std::runtime_error(description + " timed out; status=" + window.status_text() +
                             "; error=" + window.last_error());
}

void settle(Window& window) {
    pump_until(
        window,
        [&] {
            return window.idle();
        },
        "waiting for GUI service actions");
}

void queue_key(Window& window, SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = code;
    event.key.mod = modifiers;
    event.key.down = true;
    event.key.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Could not queue a production keyboard event");
}

void key(Window& window, SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
    queue_key(window, code, modifiers);
    settle(window);
}

void text(Window& window, const std::string& value) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = value.c_str();
    event.text.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Could not queue a production text input event");
    settle(window);
}

void prompt_text(Window& window, const std::string& value) {
    key(window, SDLK_A, SDL_KMOD_CTRL);
    text(window, value);
}

Json rpc(const Host& host, std::string method, Json params = Json::object()) {
    auto pending = host->submit(Json{
        {"jsonrpc", "2.0"},
        {"id", "gui-workspace-check"},
        {"method", std::move(method)},
        {"params",
         std::move(params)}}.dump());
    require(pending.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "Verification RPC timed out");
    const auto body = pending.get();
    require(body.has_value(), "Verification RPC returned no response");
    const auto response = Json::parse(*body);
    require(!response.contains("error"), "Verification RPC failed: " + response.dump());
    return response.at("result");
}

Json document(const Host& host, const std::string& id) {
    return rpc(host, "document.get", {{"id", id}});
}

void require_buffer(const Host& host,
                    const std::string& id,
                    const std::string& expected,
                    bool dirty,
                    const std::string& description) {
    const auto state = document(host, id);
    require(state.at("text") == expected,
            description + ": unexpected document text; wanted=" + Json(expected).dump() +
                "; actual=" + state.at("text").dump());
    require(state.at("dirty") == dirty, description + ": unexpected saved state");
}

void search(Window& window,
            const std::string& query,
            std::size_t expected_count,
            std::initializer_list<SDL_Keycode> toggles = {}) {
    key(window, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    for (const auto toggle : toggles) {
        key(window, toggle, SDL_KMOD_ALT);
    }
    prompt_text(window, query);
    key(window, SDLK_RETURN);
    const auto expected = std::to_string(expected_count) + " workspace match(es)";
    require(window.status_text() == expected,
            "Search options did not reach the service: wanted " + expected + ", got " +
                window.status_text());
}

void click_search_row(Window& window, std::size_t row) {
    int logical_width{}, logical_height{}, pixel_width{}, pixel_height{};
    require(SDL_GetWindowSize(window.window(), &logical_width, &logical_height) &&
                SDL_GetWindowSizeInPixels(window.window(), &pixel_width, &pixel_height),
            "Could not read the real SDL window dimensions");
    require(logical_width > 0 && logical_height > 0 && pixel_width > 0 && pixel_height > 0,
            "SDL returned an empty window");
    const auto layout = window.layout();
    // Search rows use UI units. SDL input coordinates and renderer pixels can
    // differ on high-DPI Windows displays; convert through the actual window.
    const auto x = (layout.sidebar_rows.x + layout.sidebar_rows.width / 2) *
                   static_cast<float>(logical_width) / static_cast<float>(pixel_width);
    const auto y =
        (layout.sidebar_rows.y + layout.sidebar_row_height * (static_cast<float>(row) + 0.5F)) *
        static_cast<float>(logical_height) / static_cast<float>(pixel_height);
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    event.button.windowID = SDL_GetWindowID(window.window());
    event.button.button = SDL_BUTTON_LEFT;
    event.button.down = true;
    event.button.x = x;
    event.button.y = y;
    require(SDL_PushEvent(&event), "Could not queue a search result click");
    event.type = SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.down = false;
    require(SDL_PushEvent(&event), "Could not queue a search result mouse release");
    settle(window);
}

void prepare_replace(Window& window) {
    key(window, SDLK_F, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    prompt_text(window, "needle");
    key(window, SDLK_RETURN);
    key(window, SDLK_H, SDL_KMOD_CTRL | SDL_KMOD_SHIFT);
    prompt_text(window, "thread");
    const auto values = window.view_state().sidebar_form.at("values");
    require(values.at("query") == "needle" && values.at("replacement") == "thread",
            "Replacement controls did not retain independent search/replacement input");
    key(window, SDLK_RETURN);
}

void run_acceptance(const fs::path& workspace) {
    const std::string baseline = "persisted baseline\n";
    const std::string dirty_text =
        "needle DIRTY\nCaseToken caseToken casetokenX\nrx12 rxAB\nwhole wholeX\n";
    const std::string untitled_text = "needle UNTITLED\n";
    (void)astra::io::save_file(workspace / "dirty.txt", baseline);
    (void)astra::io::save_file(workspace / "disk.txt", "needle DISK\n");

    astra::service::ServiceHostOptions service;
    service.workspace = workspace;
    service.resource_directory = fs::current_path();
    service.defaults_script = astra::io::read_file("resources/defaults.js").text;
    service.user_script = "astra.configure({git:{inheritGlobalConfig:false}});";
    service.watch_files = true;
    auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
    {
        astra::ui::EditorWindowOptions options;
        options.title = "Astra - workspace and external-change acceptance";
        options.workspace_name = "WORKSPACE ACCEPTANCE";
        options.open_paths = {"dirty.txt"};
        Window window(host, std::move(options));
        settle(window);
        const auto dirty_id = window.active_document_id();
        require(!dirty_id.empty(), "Initial workspace file did not open");
        pump_until(
            window,
            [&] {
                const auto watch = rpc(host, "workspace.watchStatus");
                return watch.value("ready", false) &&
                       watch.value("watchedFiles", std::size_t{}) == 1;
            },
            "waiting for the real workspace file subscription");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, dirty_text);
        key(window, SDLK_N, SDL_KMOD_CTRL);
        const auto untitled_id = window.active_document_id();
        require(!untitled_id.empty() && untitled_id != dirty_id,
                "New file did not create an independent buffer");
        text(window, untitled_text);
        require_buffer(host, dirty_id, dirty_text, true, "Initial unsaved file");
        require_buffer(host, untitled_id, untitled_text, true, "Initial untitled file");

        search(window, "needle", 3);
        click_search_row(window, 0);
        require(window.active_document_id() == dirty_id,
                "Unsaved search result did not activate the existing file buffer");
        require(document(host, dirty_id).at("selections") ==
                    Json::array({{{"anchor", 0}, {"head", 6}}}),
                "Unsaved search result did not select its exact range");
        click_search_row(window, 1);
        require(window.active_document_id() == untitled_id,
                "Untitled search result did not activate the existing untitled buffer");
        require(document(host, untitled_id).at("selections") ==
                    Json::array({{{"anchor", 0}, {"head", 6}}}),
                "Untitled search result did not select its exact range");
        require(rpc(host, "document.list").size() == 2,
                "Search result click created a duplicate or pseudo-path document");
        require(window.view_state().sidebar.at("focused") == false,
                "Search result activation did not return editor focus");
        key(window, SDLK_F6);
        key(window, SDLK_HOME);
        key(window, SDLK_RETURN);
        require(window.active_document_id() == dirty_id &&
                    window.view_state().sidebar.at("focused") == false,
                "Search keyboard did not activate the first result with editor focus");
        key(window, SDLK_F6);
        key(window, SDLK_DOWN);
        key(window, SDLK_RETURN);
        require(window.active_document_id() == untitled_id &&
                    document(host, untitled_id).at("selections") ==
                        Json::array({{{"anchor", 0}, {"head", 6}}}),
                "Search keyboard did not activate the indexed untitled result");
        window.request_screenshot("build/graphics/gui-workspace-search.png");
        settle(window);

        // Defaults: case-sensitive literal substring search. Toggle only via
        // actual Alt+C/R/W while the production search prompt owns input.
        search(window, "casetoken", 1);
        search(window, "casetoken", 3, {SDLK_C});
        search(window, "casetoken", 2, {SDLK_W});
        search(window, "rx[0-9]+", 0, {SDLK_C, SDLK_W});
        search(window, "rx[0-9]+", 1, {SDLK_R});
        search(window, "needle", 3, {SDLK_R});
        std::cerr << "GUI workspace: dirty/untitled result clicks and Alt+C/R/W passed\n";

        prepare_replace(window);
        require_buffer(
            host, dirty_id, dirty_text, true, "Replace preview must preserve dirty content");
        require_buffer(host,
                       untitled_id,
                       untitled_text,
                       true,
                       "Replace preview must preserve untitled content");
        require(astra::io::read_file(workspace / "disk.txt").text == "needle DISK\n",
                "Workspace replacement wrote disk before confirmation");
        key(window, SDLK_ESCAPE);
        require_buffer(host, dirty_id, dirty_text, true, "Cancelled replacement");
        require_buffer(host, untitled_id, untitled_text, true, "Cancelled untitled replacement");
        prepare_replace(window);
        window.request_screenshot("build/graphics/gui-workspace-replace-confirm.png");
        settle(window);
        key(window, SDLK_RETURN);
        const auto replaced_dirty = std::string("thread") + dirty_text.substr(6);
        const auto replaced_untitled = std::string("thread") + untitled_text.substr(6);
        require_buffer(host, dirty_id, replaced_dirty, true, "Confirmed unsaved replacement");
        require_buffer(
            host, untitled_id, replaced_untitled, true, "Confirmed untitled replacement");
        require(astra::io::read_file(workspace / "dirty.txt").text == baseline,
                "Workspace replacement implicitly saved the dirty buffer");
        require(astra::io::read_file(workspace / "disk.txt").text == "thread DISK\n",
                "Confirmed workspace replacement did not update the closed disk file");
        require(window.workspace_notice().find("1 disk file(s) and 2 unsaved buffer(s)") !=
                    std::string::npos,
                "Replacement completion did not report the service's modifiedBuffers");
        require(window.active_document_id() == untitled_id,
                "Replacement unexpectedly activated a different buffer");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require_buffer(host,
                       untitled_id,
                       untitled_text,
                       true,
                       "One undo must restore the untitled replacement");
        key(window, SDLK_TAB, SDL_KMOD_CTRL);
        require(window.active_document_id() == dirty_id,
                "Next-tab shortcut did not activate the dirty file");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require_buffer(host,
                       dirty_id,
                       dirty_text,
                       true,
                       "One undo must restore the file's unsaved replacement");
        require(astra::io::read_file(workspace / "disk.txt").text == "thread DISK\n",
                "Undo of a buffer replacement changed a separate disk file");
        std::cerr
            << "GUI workspace: replacement confirmation, modified buffers and atomic undo passed\n";

        const auto before_external = document(host, dirty_id);
        const auto disk_version = astra::io::read_file(workspace / "dirty.txt").version;
        const std::string external = "external replacement\n";
        (void)astra::io::save_file(
            workspace / "dirty.txt", external, astra::io::TextEncoding::utf8, disk_version);
        pump_until(
            window,
            [&] {
                const auto notice = window.workspace_notice();
                return notice.find("File changed outside Astra") != std::string::npos &&
                       notice.find("buffer preserved") != std::string::npos;
            },
            "waiting for the real atomic-replacement conflict notification");
        settle(window);
        require_buffer(
            host, dirty_id, dirty_text, true, "External conflict must preserve unsaved content");
        require(document(host, dirty_id).at("revision") == before_external.at("revision"),
                "External conflict unexpectedly edited the document");
        const auto conflict_notice = window.workspace_notice();
        search(window, "external replacement", 0);
        window.execute_ui_command("ui.focusEditor");
        settle(window);
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        require(document(host, dirty_id).at("selections") ==
                    Json::array({{{"anchor", 0}, {"head", 0}}}),
                "Conflict verification did not move the actual document cursor to its start");
        require(window.workspace_notice() == conflict_notice,
                "An unrelated search or cursor command erased the persistent conflict notice");
        window.request_screenshot("build/graphics/gui-workspace-conflict.png");
        settle(window);

        window.execute_ui_command("ui.reload");
        settle(window);
        require_buffer(
            host, dirty_id, dirty_text, true, "Reload must await an explicit confirmation");
        window.request_screenshot("build/graphics/gui-workspace-reload-confirm.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        require_buffer(
            host, dirty_id, dirty_text, true, "Cancelled reload must preserve dirty content");
        require(window.workspace_notice() == conflict_notice,
                "Cancelling reload dismissed the unresolved conflict");

        window.execute_ui_command("ui.reload");
        settle(window);
        const auto prompt_state = document(host, dirty_id);
        // A separate client can edit while the GUI's confirmation is visible.
        // The confirmation must retain its original revision rather than using
        // the latest notification's revision to silently approve newer content.
        (void)rpc(host,
                  "document.insert",
                  {{"id", dirty_id},
                   {"baseRevision", prompt_state.at("revision")},
                   {"text", "concurrent "}});
        settle(window);
        const auto concurrent = document(host, dirty_id);
        require(concurrent.at("text") == "concurrent " + dirty_text,
                "Second-client edit did not reach the selected buffer: " +
                    concurrent.at("text").dump());
        queue_key(window, SDLK_RETURN);
        pump_until(
            window,
            [&] {
                return window.idle() && !window.last_error().empty();
            },
            "rejecting a reload confirmation after a concurrent edit",
            true);
        require(window.last_error().find("Stale document revision") != std::string::npos,
                "Concurrent reload did not report its stale confirmation: " + window.last_error());
        require(document(host, dirty_id).at("text") == concurrent.at("text") &&
                    document(host, dirty_id).at("revision") == concurrent.at("revision"),
                "Stale reload confirmation discarded the concurrent edit");
        require(astra::io::read_file(workspace / "dirty.txt").text == external,
                "Rejected reload unexpectedly changed disk content");
        key(window, SDLK_ESCAPE);
        require(window.workspace_notice() == conflict_notice,
                "Rejected reload cleared the unresolved external conflict");

        window.execute_ui_command("ui.reload");
        settle(window);
        key(window, SDLK_RETURN);
        require_buffer(host,
                       dirty_id,
                       external,
                       false,
                       "Fresh confirmed reload must publish the disk baseline");
        require(window.workspace_notice().find("Reloaded from disk") != std::string::npos,
                "Confirmed reload did not replace the conflict notice with a reload result");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require_buffer(host,
                       dirty_id,
                       concurrent.at("text").get<std::string>(),
                       true,
                       "Confirmed reload must preserve one-step undo of the discarded buffer");
        std::cerr << "GUI workspace: persistent conflict and reload cancel/confirm/concurrent edit "
                     "passed\n";
    }
    host->shutdown();
}
} // namespace

int main() {
    fs::path workspace;
    try {
        std::cerr << std::unitbuf;
        const auto parent = fs::weakly_canonical(fs::absolute("build/graphics"));
        fs::create_directories(parent);
        workspace = parent / ("gui-workspace-services-" +
                              std::to_string(Clock::now().time_since_epoch().count()));
        require(fs::create_directory(workspace), "Could not create a unique GUI fixture directory");
        run_acceptance(workspace);
        const auto issues =
            astra::test::cleanup_owned_directory(workspace, parent, "gui-workspace-services-");
        for (const auto& issue : issues) {
            std::cerr << "Fixture cleanup: " << issue.path << ": " << issue.message << '\n';
        }
        require(issues.empty(), "GUI fixture handles were not released at shutdown");
        std::cout << "Windows GUI -> shared service: unsaved/untitled search, Alt+C/R/W, confirmed "
                     "workspace replacement, dirty undo, persistent external conflict and guarded "
                     "reload: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI workspace failed: " << error.what()
                  << "\nPreserved fixture: " << workspace << '\n';
        return 1;
    }
}
