#include <astra/io/file_store.hpp>
#include <astra/service/service_host.hpp>
#include <astra/ui/editor_window.hpp>
#include "support/temporary_directory.hpp"
#include <SDL3/SDL.h>
#include <windows.h>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>

namespace {
using Window = astra::ui::EditorWindow;
using Json = astra::protocol::Json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;
const std::string seed = "Preserve 中🙂";

struct WindowDeleter {
    void operator()(HWND window) const noexcept {
        if (!DestroyWindow(window))
            std::cerr << "Owned clipboard fixture window cleanup failed: " << GetLastError()
                      << '\n';
    }
};

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

// Lease the real OS clipboard on an owned thread without changing its contents,
// formats or owner. The deadline also releases it if the fixture stops pumping.
class ClipboardLease final {
  public:
    ClipboardLease() {
        std::promise<bool> started;
        auto ready = started.get_future();
        std::promise<std::string> finished;
        finished_ = finished.get_future();
        thread_ = std::jthread([started = std::move(started),
                                finished = std::move(finished),
                                release = release_.get_future()]() mutable {
            // A concrete owned HWND is essential: two NULL openers in the same
            // process need not exercise contention between different windows.
            std::unique_ptr<std::remove_pointer_t<HWND>, WindowDeleter> window(
                CreateWindowExW(0,
                                L"STATIC",
                                L"Astra clipboard lease",
                                0,
                                0,
                                0,
                                0,
                                0,
                                HWND_MESSAGE,
                                nullptr,
                                GetModuleHandleW(nullptr),
                                nullptr));
            if (!window || !OpenClipboard(window.get())) {
                started.set_value(false);
                finished.set_value("Cannot acquire the real Windows clipboard");
                return;
            }
            started.set_value(true);
            const auto signalled =
                release.wait_for(std::chrono::seconds(20)) == std::future_status::ready;
            const auto closed = CloseClipboard() != FALSE;
            const auto destroyed = DestroyWindow(window.get()) != FALSE;
            if (destroyed)
                static_cast<void>(window.release());
            finished.set_value(!closed      ? "Cannot close owned clipboard lease"
                               : !signalled ? "Clipboard fixture lease exceeded its deadline"
                               : !destroyed ? "Cannot destroy owned clipboard fixture window"
                                            : "");
        });
        require(ready.get(), "Cannot acquire the real Windows clipboard");
    }

    ~ClipboardLease() {
        try {
            close();
        } catch (const std::exception& error) {
            std::cerr << "Clipboard lease cleanup failed: " << error.what() << '\n';
        }
    }

    void close() {
        if (closed_)
            return;
        closed_ = true;
        release_.set_value();
        thread_.join();
        const auto error = finished_.get();
        require(error.empty(), error);
    }

  private:
    std::promise<void> release_;
    std::future<std::string> finished_;
    std::jthread thread_;
    bool closed_{};
};

template <class Predicate>
void await(Window& window, Predicate complete, bool expect_clipboard_error = false) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    do {
        require(window.pump(), "Clipboard acceptance window closed unexpectedly");
        const auto error = window.last_error();
        require(error.empty() ||
                    (expect_clipboard_error && error.starts_with("Clipboard read failed:")),
                "Unexpected GUI error: " + error);
        if (complete())
            return;
        SDL_Delay(1);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Clipboard acceptance did not settle within its deadline");
}

void settle(Window& window, bool expect_clipboard_error = false) {
    await(
        window,
        [&] {
            return window.idle();
        },
        expect_clipboard_error);
}

void key(Window& window, SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = SDL_GetWindowID(window.window());
    event.key.key = code;
    event.key.mod = modifiers;
    event.key.down = true;
    require(SDL_PushEvent(&event), "Cannot enqueue production field key");
}

Json document(const std::shared_ptr<astra::service::ServiceHost>& host, const std::string& id) {
    const auto response = host->submit(Json{{"jsonrpc", "2.0"},
                                            {"id", "verify"},
                                            {"method", "document.get"},
                                            {"params", {{"id", id}}}}
                                           .dump())
                              .get();
    require(response.has_value(), "Document verification returned no response");
    const auto result = Json::parse(*response);
    require(!result.contains("error"), "Document verification failed");
    return result.at("result");
}

void run(const fs::path& workspace) {
    static_cast<void>(astra::io::save_file(
        workspace / "source.txt", "Preserved document 中🙂\n", astra::io::TextEncoding::utf8));
    astra::service::ServiceHostOptions service;
    service.workspace = workspace;
    service.recovery_directory = workspace / "recovery";
    service.resource_directory = fs::current_path();
    service.defaults_script = astra::io::read_file("data/defaults.js").text;
    service.watch_files = false;
    auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
    {
        astra::ui::EditorWindowOptions options;
        options.title = "Astra — clipboard failure acceptance";
        options.open_paths = {"source.txt"};
        options.resource_directory = fs::current_path();
        options.user_script = R"js(
        for (const view of ['', 'search']) {
            const id = view ? 'clipboard-sidebar' : 'clipboard-editor';
            astra.registerCommand('acceptance.' + id, async () => {
                if (view) await astra.ui.showView(view);
                await astra.ui.forms.open({id, title: 'Clipboard failure', view,
                    fields: [{id:'value', value:'Preserve 中🙂', submit:'accept'}],
                    actions: [{id:'accept', label:'Accept'}]});
                await astra.ui.forms.focus(id, 'value', true);
            });
            astra.registerCommand('acceptance.close-' + id, () => astra.ui.forms.close(id));
        }
        astra.registerCommand('acceptance.clearError', () => astra.ui.update({error:''}));
        astra.registerCommand('acceptance.readClipboard', async () => {
            try {
                await astra.ui.clipboard.read();
                await astra.ui.update({status:'Unexpected clipboard success'});
            } catch (error) {
                await astra.ui.update({status:'Rejected ' + error.message});
            }
        });
        astra.registerCommand('acceptance.clipboard-modal', async () => {
            const result = await astra.ui.input({title:'Clipboard failure', value:'Preserve 中🙂'});
            await astra.ui.update({status:'Modal result ' + result});
        });
        )js";
        Window window(host, std::move(options));
        settle(window);
        const auto id = window.active_document_id();
        require(!id.empty(), "Clipboard fixture document did not open");
        const auto original = document(host, id);
        const auto preserved = [&] {
            const auto current = document(host, id);
            for (const auto* field : {"text", "revision", "selections"})
                require(current.at(field) == original.at(field),
                        "Clipboard workflow changed document " + std::string(field));
        };
        {
            ClipboardLease lease;
            window.execute_ui_command("acceptance.readClipboard");
            settle(window);
            require(window.status_text().starts_with("Rejected Clipboard read failed:"),
                    "Public JS clipboard read did not reject the actual OS failure: " +
                        window.status_text());
            lease.close();
            preserved();
        }
        for (const bool sidebar : {false, true}) {
            const auto role = sidebar ? "clipboard-sidebar" : "clipboard-editor";
            window.execute_ui_command(std::string("acceptance.") + role);
            settle(window);
            const auto state = [&] {
                const auto model = window.view_state();
                return sidebar ? model.sidebar_form : model.inline_form;
            };
            const auto before = state();
            require(before.at("values").at("value") == seed, "Clipboard form seed differs");
            {
                ClipboardLease lease;
                key(window, SDLK_V, SDL_KMOD_CTRL);
                settle(window, true);
                require(window.last_error().starts_with("Clipboard read failed:"),
                        "Actual occupied clipboard was not reported to the field");
                require(state().at("values") == before.at("values") &&
                            state().at("version") == before.at("version"),
                        "Failed paste changed selected content or its transaction version");
                lease.close();
            }
            window.execute_ui_command("acceptance.clearError");
            settle(window);
            const std::string replacement = "Recovered 中🙂";
            SDL_Event text{};
            text.type = SDL_EVENT_TEXT_INPUT;
            text.text.windowID = SDL_GetWindowID(window.window());
            text.text.text = replacement.c_str();
            require(SDL_PushEvent(&text), "Cannot enqueue recovery text");
            settle(window);
            require(state().at("values").at("value") == replacement,
                    "Failed paste changed field selection or prevented continued input");
            key(window, SDLK_Z, SDL_KMOD_CTRL);
            settle(window);
            require(state().at("values").at("value") == seed,
                    "Field undo did not preserve content through the failure");
            preserved();
            window.execute_ui_command(std::string("acceptance.close-") + role);
            settle(window);
            std::cout << role << " actual failure/content/selection/history: PASS\n";
        }
        window.execute_ui_command("acceptance.clipboard-modal");
        await(window, [&] {
            return window.view_state().input_layout.value("bytes", std::size_t{}) == seed.size();
        });
        key(window, SDLK_A, SDL_KMOD_CTRL);
        require(window.pump(), "Modal closed before paste");
        {
            ClipboardLease lease;
            key(window, SDLK_V, SDL_KMOD_CTRL);
            await(
                window,
                [&] {
                    return !window.last_error().empty();
                },
                true);
            require(window.view_state().input_layout.at("bytes") == seed.size(),
                    "Failed clipboard paste cleared selected modal content");
            lease.close();
        }
        window.execute_ui_command("acceptance.clearError");
        await(
            window,
            [&] {
                return window.last_error().empty();
            },
            true);
        key(window, SDLK_RETURN);
        settle(window);
        require(window.status_text() == "Modal result " + seed,
                "Modal did not return its exact preserved Unicode content");
        preserved();
    }
    host->shutdown();
}
} // namespace

int main() {
    fs::path workspace;
    try {
        const auto parent = fs::weakly_canonical(fs::absolute("build/graphics"));
        fs::create_directories(parent);
        workspace =
            parent / ("gui-clipboard-" + std::to_string(Clock::now().time_since_epoch().count()));
        require(fs::create_directory(workspace), "Cannot create owned clipboard fixture");
        run(workspace);
        const auto issues =
            astra::test::cleanup_owned_directory(workspace, parent, "gui-clipboard-");
        require(issues.empty(), "Clipboard fixture cleanup failed");
        std::cout << "Windows real clipboard failure: JS rejection, editor/sidebar/modal "
                     "preservation: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Clipboard acceptance failed: " << error.what()
                  << "\nPreserved fixture: " << workspace << '\n';
        return 1;
    }
}
