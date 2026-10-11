#include <astra/io/file_store.hpp>
#include <astra/service/service_host.hpp>
#include <astra/ui/clipboard_client.hpp>
#include <astra/ui/editor_window.hpp>
#include "support/temporary_directory.hpp"
#include <SDL3/SDL.h>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Json = astra::protocol::Json;
using Window = astra::ui::EditorWindow;
using Client = astra::ui::ClipboardClient;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message + " (Win32 " + std::to_string(GetLastError()) + ")");
}

struct GlobalMemoryDeleter {
    void operator()(void* memory) const noexcept {
        if (GlobalFree(memory) != nullptr)
            std::cerr << "Owned delayed clipboard memory cleanup failed: " << GetLastError()
                      << '\n';
    }
};

// A private real Windows station has its own clipboard. Delayed rendering here
// never empties, replaces or enumerates the user's interactive clipboard.
class PrivateDesktop final {
  public:
    PrivateDesktop() {
        original_station_ = GetProcessWindowStation();
        const auto name = L"AstraClipboard-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                          std::to_wstring(GetTickCount64());
        // Requires an administrator token; CWF_CREATE_ONLY prevents accidental
        // reuse of a station whose clipboard would not belong to this fixture.
        station_ = CreateWindowStationW(name.c_str(), CWF_CREATE_ONLY, WINSTA_ALL_ACCESS, nullptr);
        require(station_ != nullptr, "Cannot create isolated clipboard station");
        try {
            require(SetProcessWindowStation(station_), "Cannot select isolated station");
            desktop_ = CreateDesktopW(L"Default", nullptr, nullptr, 0, GENERIC_ALL, nullptr);
            require(desktop_ != nullptr, "Cannot create isolated clipboard desktop");
        } catch (...) {
            restore();
            throw;
        }
    }

    ~PrivateDesktop() {
        restore();
    }

    HDESK get() const noexcept {
        return desktop_;
    }

    void close() {
        require(SetProcessWindowStation(original_station_), "Cannot restore original station");
        require(CloseDesktop(desktop_), "Cannot close isolated desktop");
        desktop_ = nullptr;
        require(CloseWindowStation(station_), "Cannot close isolated station");
        station_ = nullptr;
    }

  private:
    HWINSTA original_station_{}, station_{};
    HDESK desktop_{};

    void restore() noexcept {
        if (!station_)
            return;
        if (!SetProcessWindowStation(original_station_))
            std::cerr << "Clipboard desktop restoration failed: " << GetLastError() << '\n';
        if (desktop_ && !CloseDesktop(desktop_))
            std::cerr << "Clipboard desktop cleanup failed: " << GetLastError() << '\n';
        if (!CloseWindowStation(station_))
            std::cerr << "Clipboard station cleanup failed: " << GetLastError() << '\n';
        station_ = nullptr;
        desktop_ = nullptr;
    }
};

class DelayedProvider final {
  public:
    explicit DelayedProvider(HDESK desktop) : released_(release_.get_future().share()) {
        std::promise<std::string> initialized;
        auto ready = initialized.get_future();
        finished_ = finished_promise_.get_future();
        thread_ = std::jthread([this, desktop, initialized = std::move(initialized)]() mutable {
            HWND owned{};
            WNDPROC previous{};
            bool announced{};
            std::string error;
            try {
                require(SetThreadDesktop(desktop), "Cannot attach clipboard provider desktop");
                owned = CreateWindowExW(0,
                                        L"STATIC",
                                        L"Astra delayed clipboard provider",
                                        0,
                                        0,
                                        0,
                                        0,
                                        0,
                                        HWND_MESSAGE,
                                        nullptr,
                                        GetModuleHandleW(nullptr),
                                        nullptr);
                require(owned != nullptr, "Cannot create owned clipboard provider");
                SetLastError(ERROR_SUCCESS);
                SetWindowLongPtrW(owned, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                require(GetLastError() == ERROR_SUCCESS, "Cannot retain clipboard provider state");
                previous = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(owned, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&procedure)));
                require(previous != nullptr, "Cannot subclass owned clipboard provider");
                previous_ = previous;
                require(OpenClipboard(owned), "Cannot open isolated clipboard");
                if (!EmptyClipboard()) {
                    const auto failure = GetLastError();
                    require(CloseClipboard(), "Cannot close failed clipboard initialization");
                    throw std::runtime_error("Cannot empty isolated clipboard: " +
                                             std::to_string(failure));
                }
                SetLastError(ERROR_SUCCESS);
                const auto delayed = SetClipboardData(CF_UNICODETEXT, nullptr);
                const auto failure = GetLastError();
                require(CloseClipboard(), "Cannot close initialized clipboard");
                require(delayed == nullptr && failure == ERROR_SUCCESS,
                        "Cannot register real delayed Unicode clipboard format");
                window_.store(owned);
                initialized.set_value("");
                announced = true;
                MSG message{};
                for (;;) {
                    const auto result = GetMessageW(&message, nullptr, 0, 0);
                    require(result != -1, "Cannot pump owned provider messages");
                    if (result == 0 || message.message == WM_APP + 1)
                        break;
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                error = render_error_;
            } catch (const std::exception& failure) {
                error = failure.what();
            }
            window_.store(nullptr);
            if (owned) {
                if (previous)
                    SetWindowLongPtrW(owned, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
                if (!DestroyWindow(owned))
                    error += " Provider window cleanup failed: " + std::to_string(GetLastError());
            }
            if (!announced)
                initialized.set_value(error);
            finished_promise_.set_value(error);
        });
        const auto error = ready.get();
        require(error.empty(), error);
    }

    ~DelayedProvider() {
        try {
            close();
        } catch (const std::exception& error) {
            std::cerr << "Delayed provider cleanup failed: " << error.what() << '\n';
        }
    }

    bool entered() const noexcept {
        return entered_.load();
    }

    void release(bool publish = true) {
        if (!released_flag_) {
            released_flag_ = true;
            publish_.store(publish);
            release_.set_value();
        }
    }

    void close() {
        if (closed_)
            return;
        release(false);
        if (const auto window = window_.load())
            require(PostMessageW(window, WM_APP + 1, 0, 0), "Cannot stop owned clipboard provider");
        thread_.join();
        closed_ = true;
        const auto error = finished_.get();
        require(error.empty(), error);
    }

  private:
    std::atomic<HWND> window_{};
    std::atomic<bool> entered_{};
    std::atomic<bool> publish_{true};
    std::promise<void> release_;
    std::shared_future<void> released_;
    std::promise<std::string> finished_promise_;
    std::future<std::string> finished_;
    std::jthread thread_;
    WNDPROC previous_{};
    std::string render_error_;
    bool released_flag_{}, closed_{};

    void render() {
        entered_.store(true);
        require(released_.wait_for(8s) == std::future_status::ready,
                "Delayed provider exceeded its bounded fixture deadline");
        // A killed requester no longer holds the clipboard open. Finish its
        // pending WM_RENDERFORMAT without publishing into a vanished request.
        if (!publish_.load())
            return;
        constexpr wchar_t text[] = L"Delayed \x4e2d\xd83d\xde42";
        std::unique_ptr<void, GlobalMemoryDeleter> memory(GlobalAlloc(GMEM_MOVEABLE, sizeof(text)));
        require(memory != nullptr, "Cannot allocate delayed Unicode text");
        auto* target = GlobalLock(memory.get());
        require(target != nullptr, "Cannot lock delayed Unicode text");
        std::memcpy(target, text, sizeof(text));
        SetLastError(ERROR_SUCCESS);
        const auto unlocked = GlobalUnlock(memory.get());
        const auto failure = GetLastError();
        if (!unlocked && failure != ERROR_SUCCESS)
            throw std::runtime_error("Cannot unlock delayed Unicode text");
        // Windows requires no OpenClipboard during WM_RENDERFORMAT.
        require(SetClipboardData(CF_UNICODETEXT, memory.get()) != nullptr,
                "Cannot publish delayed Unicode text");
        static_cast<void>(memory.release());
    }

    static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* owner = reinterpret_cast<DelayedProvider*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_RENDERFORMAT && wparam == CF_UNICODETEXT) {
            try {
                owner->render();
            } catch (const std::exception& error) {
                owner->render_error_ = error.what();
            }
            return 0;
        }
        return CallWindowProcW(owner->previous_, window, message, wparam, lparam);
    }
};

template <class Pump, class Predicate>
void await(Pump pump, Predicate done, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = Clock::now() + timeout;
    do {
        const auto before = Clock::now();
        pump();
        require(Clock::now() - before < 250ms, "Clipboard blocked the owner loop for 250ms");
        if (done())
            return;
        std::this_thread::sleep_for(1ms);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Async clipboard acceptance deadline expired");
}

void client_acceptance(HDESK desktop, const fs::path& executable) {
    Client client({executable, 5000ms});
    const auto rejects = [](auto action) {
        bool rejected{};
        try {
            action();
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "Clipboard request did not enforce its declared input/queue bound");
    };
    rejects([&] {
        static_cast<void>(client.read(std::string("invalid\0id", 10)));
    });
    rejects([&] {
        static_cast<void>(client.read(std::string(1, static_cast<char>(0xff))));
    });
    rejects([&] {
        static_cast<void>(client.read("zero-limit", 0));
    });
    rejects([&] {
        static_cast<void>(client.write("nul-text", std::string("a\0b", 3)));
    });
    rejects([&] {
        static_cast<void>(client.write("oversize", std::string(Client::max_text_bytes + 1, 'x')));
    });
    std::vector<std::future<Json>> queued;
    for (std::size_t index = 0; index < Client::max_jobs; ++index)
        queued.push_back(client.read("queued-" + std::to_string(index)));
    rejects([&] {
        static_cast<void>(client.read("queued-overflow"));
    });
    rejects([&] {
        static_cast<void>(client.read("queued-0"));
    });
    client.cancel_pending();
    await(
        [&] {
            client.poll();
        },
        [&] {
            return client.drained();
        });
    for (auto& result : queued) {
        bool cancelled{};
        try {
            static_cast<void>(result.get());
        } catch (const astra::protocol::RpcError& error) {
            cancelled = error.code() == -32004;
        }
        require(cancelled, "Queued cancellation did not reject the original request");
    }
    require(client.take_errors().empty(), "Pre-dispatch cancellation leaked cleanup errors");
    {
        Client short_deadline({executable, 1ms});
        auto queued_timeout = short_deadline.read("queued-timeout");
        std::this_thread::sleep_for(20ms);
        short_deadline.poll();
        bool expired{};
        try {
            static_cast<void>(queued_timeout.get());
        } catch (const astra::protocol::RpcError& error) {
            expired = std::string(error.what()).find("before dispatch") != std::string::npos;
        }
        require(expired && short_deadline.drained(),
                "Expired queued read launched a native helper");
    }
    for (int mode = 0; mode < 3; ++mode) {
        std::cout << "Delayed provider mode " << mode << ": begin\n";
        Client delayed({executable, mode == 0 ? 5000ms : 2000ms});
        DelayedProvider provider(desktop);
        const auto submitted = Clock::now();
        auto result = delayed.read("delayed-" + std::to_string(mode));
        await(
            [&] {
                delayed.poll();
            },
            [&] {
                return provider.entered();
            });
        std::cout << "Native provider entered after "
                  << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - submitted)
                         .count()
                  << "ms\n";
        require(result.wait_for(0ms) != std::future_status::ready,
                "Read did not wait for the actual native provider");
        if (mode == 0) {
            const auto until = Clock::now() + 300ms;
            std::size_t ticks{};
            await(
                [&] {
                    delayed.poll();
                    ++ticks;
                },
                [&] {
                    return Clock::now() >= until;
                });
            // A 1ms sleep can use Windows' coarser default scheduler granularity.
            // Per-poll latency is measured independently by await().
            require(ticks >= 8, "Insufficient owner-loop progress while provider was held");
            provider.release();
            await(
                [&] {
                    delayed.poll();
                },
                [&] {
                    return result.wait_for(0ms) == std::future_status::ready;
                });
            require(result.get() == "Delayed 中🙂", "Delayed read lost exact Unicode contents");
        } else {
            if (mode == 1)
                delayed.cancel("delayed-1");
            await(
                [&] {
                    delayed.poll();
                },
                [&] {
                    return result.wait_for(0ms) == std::future_status::ready;
                });
            bool rejected{};
            try {
                static_cast<void>(result.get());
            } catch (const astra::protocol::RpcError& error) {
                rejected = std::string(error.what()).find(mode == 1 ? "cancelled" : "timed out") !=
                           std::string::npos;
            }
            require(rejected, "Held clipboard read did not reject cancellation/deadline");
            await(
                [&] {
                    delayed.poll();
                },
                [&] {
                    return delayed.drained();
                },
                2s);
            // The real provider is still blocked when process cleanup finishes.
            provider.release(false);
        }
        provider.close();
        await(
            [&] {
                delayed.poll();
            },
            [&] {
                return delayed.drained();
            });
        require(delayed.take_errors().empty(), "Owned helper cleanup reported an error");
    }
    auto written = client.write("unicode-owner", "Owned 中🙂");
    await(
        [&] {
            client.poll();
        },
        [&] {
            return written.wait_for(0ms) == std::future_status::ready;
        });
    require(written.get() == true, "Native asynchronous clipboard write failed");
    auto read = client.read("roundtrip");
    await(
        [&] {
            client.poll();
        },
        [&] {
            return read.wait_for(0ms) == std::future_status::ready;
        });
    require(read.get() == "Owned 中🙂", "Persistent clipboard owner lost Unicode text");
    await(
        [&] {
            client.poll();
        },
        [&] {
            return client.idle();
        });
    client.close();
    await(
        [&] {
            client.poll();
        },
        [&] {
            return client.drained();
        });
    require(client.take_errors().empty(), "Clipboard writer shutdown failed");
    std::cout << "Real delayed clipboard: loop progress, Unicode, cancellation, timeout, owner "
                 "shutdown: PASS\n";
}

void gui_acceptance(HDESK desktop, const fs::path& workspace) {
    static_cast<void>(astra::io::save_file(
        workspace / "source.txt", "Seed 中🙂\n", astra::io::TextEncoding::utf8));
    astra::service::ServiceHostOptions service;
    service.workspace = workspace;
    service.recovery_directory = workspace / "recovery";
    service.resource_directory = fs::current_path();
    service.defaults_script = astra::io::read_file("data/defaults.js").text;
    service.watch_files = false;
    auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
    {
        astra::ui::EditorWindowOptions options;
        options.title = "Astra — asynchronous clipboard acceptance";
        options.open_paths = {"source.txt"};
        options.resource_directory = fs::current_path();
        options.user_script = R"js(
            let controller;
            astra.registerCommand('acceptance.delayed', async () => {
                controller = new AbortController();
                try {
                    const text = await astra.ui.clipboard.read({signal:controller.signal});
                    await astra.ui.update({status:'Read ' + text});
                } catch (error) {
                    await astra.ui.update({status:'Rejected ' + error.name});
                }
            });
            astra.registerCommand('acceptance.abort', () => controller.abort());
            for (const view of ['', 'search']) {
                const id = view ? 'async-sidebar' : 'async-editor';
                astra.registerCommand('acceptance.' + id, async () => {
                    if (view) await astra.ui.showView(view);
                    await astra.ui.forms.open({id, title:'Asynchronous clipboard', view,
                        fields:[{id:'value', value:'Keep 中🙂'}],
                        actions:[{id:'accept', label:'Accept'}]});
                    await astra.ui.forms.focus(id, 'value', true);
                });
                astra.registerCommand('acceptance.close-' + id, () => astra.ui.forms.close(id));
            }
        )js";
        Window window(host, std::move(options));
        const auto pump = [&] {
            require(window.pump(), "Real GUI closed unexpectedly");
            require(window.last_error().empty(), "Real GUI error: " + window.last_error());
        };
        // Startup is deliberately warmed before measuring input/rendering latency.
        const auto startup_deadline = Clock::now() + 10s;
        while (!window.idle()) {
            pump();
            require(Clock::now() < startup_deadline, "Native GUI startup timed out");
            SDL_Delay(1);
        }
        for (const bool cancelled : {false, true}) {
            DelayedProvider provider(desktop);
            const auto original_revision = window.view_state().document.at("revision");
            window.execute_ui_command("acceptance.delayed");
            await(pump, [&] {
                return provider.entered();
            });
            SDL_Event text{};
            text.type = SDL_EVENT_TEXT_INPUT;
            text.text.windowID = SDL_GetWindowID(window.window());
            text.text.text = "Responsive ";
            require(SDL_PushEvent(&text), "Cannot enqueue actual editor input");
            await(pump, [&] {
                return window.view_state().document.at("revision") != original_revision;
            });
            require(!window.idle(), "GUI became idle while clipboard provider was blocked");
            const auto until = Clock::now() + 300ms;
            await(pump, [&] {
                return Clock::now() >= until;
            });
            if (cancelled) {
                window.execute_ui_command("acceptance.abort");
                await(pump, [&] {
                    return window.status_text() == "Rejected AbortError";
                });
                await(
                    pump,
                    [&] {
                        return window.idle();
                    },
                    2s);
            }
            provider.release(!cancelled);
            provider.close();
            await(pump, [&] {
                return window.idle();
            });
            require(window.status_text() ==
                        (cancelled ? "Rejected AbortError" : "Read Delayed 中🙂"),
                    "Public async API returned an unexpected outcome: " + window.status_text());
        }
        for (const bool sidebar : {false, true}) {
            const auto id = sidebar ? "async-sidebar" : "async-editor";
            const auto state = [&] {
                const auto model = window.view_state();
                return sidebar ? model.sidebar_form : model.inline_form;
            };
            window.execute_ui_command(std::string("acceptance.") + id);
            await(pump, [&] {
                return window.idle();
            });
            DelayedProvider provider(desktop);
            SDL_Event paste{};
            paste.type = SDL_EVENT_KEY_DOWN;
            paste.key.windowID = SDL_GetWindowID(window.window());
            paste.key.key = SDLK_V;
            paste.key.mod = SDL_KMOD_CTRL;
            paste.key.down = true;
            require(SDL_PushEvent(&paste), "Cannot enqueue delayed field paste");
            await(pump, [&] {
                return provider.entered();
            });
            require(window.view_state().input_layout.at("clipboardPending") == true,
                    "Field did not retain its asynchronous clipboard operation");
            SDL_Event text{};
            text.type = SDL_EVENT_TEXT_INPUT;
            text.text.windowID = SDL_GetWindowID(window.window());
            text.text.text = "!";
            require(SDL_PushEvent(&text), "Cannot enqueue text behind delayed field paste");
            pump();
            require(state().at("values").at("value") == "Keep 中🙂",
                    "Following text bypassed the pending paste or replaced its selection");
            provider.release();
            provider.close();
            await(pump, [&] {
                return window.idle();
            });
            require(state().at("values").at("value") == "Delayed 中🙂!",
                    "Field paste and queued typing did not retain FIFO Unicode behavior");
            window.execute_ui_command(std::string("acceptance.close-") + id);
            await(pump, [&] {
                return window.idle();
            });
        }
        window.execute_ui_command("ui.save");
        await(pump, [&] {
            return window.idle();
        });
        const auto saved = astra::io::read_file(workspace / "source.txt").text;
        require(saved.find("Responsive Responsive ") != saved.npos,
                "Input during delayed clipboard work was lost before durable save");
        DelayedProvider provider(desktop);
        window.execute_ui_command("acceptance.delayed");
        await(pump, [&] {
            return provider.entered();
        });
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        require(SDL_PushEvent(&quit), "Cannot enqueue owned GUI shutdown");
        bool closed{};
        await(
            [&] {
                closed = !window.pump();
            },
            [&] {
                return closed;
            },
            3s);
        provider.release(false);
        provider.close();
        require(astra::io::read_file(workspace / "source.txt").text == saved,
                "Shutdown during delayed clipboard work changed saved content");
    }
    host->shutdown();
    std::cout << "Native SDL/Graphite/Dawn GUI: input/rendering, AbortSignal, save and bounded "
                 "shutdown: PASS\n";
}
} // namespace

int main(int argc, char** argv) {
    fs::path workspace;
    std::ofstream log;

    struct Streams {
        std::streambuf* output = std::cout.rdbuf();
        std::streambuf* error = std::cerr.rdbuf();

        ~Streams() {
            std::cout.rdbuf(output);
            std::cerr.rdbuf(error);
        }
    } streams;

    try {
        wchar_t executable[32768]{};
        const auto count = GetModuleFileNameW(nullptr, executable, 32768);
        require(count > 0 && count < 32768, "Cannot locate owned clipboard helper");
        if (argc > 1) {
            require(argc == 2 && std::string(argv[1]) == "--log",
                    "Unknown clipboard fixture argument");
            log.open(fs::path(executable).parent_path() /
                     ("clipboard-isolated-" + std::to_string(GetTickCount64()) + ".log"));
            require(log.is_open(), "Cannot open isolated clipboard acceptance log");
            std::cout.rdbuf(log.rdbuf());
            std::cerr.rdbuf(log.rdbuf());
            std::cout << std::unitbuf;
        }
        PrivateDesktop desktop;
        const auto parent = fs::weakly_canonical(fs::absolute("build/graphics"));
        fs::create_directories(parent);
        workspace = parent / ("gui-clipboard-async-" + std::to_string(GetTickCount64()));
        require(fs::create_directory(workspace), "Cannot create owned async clipboard workspace");
        int outcome = 1;
        // Startup libraries may already own windows/hooks on the entry thread.
        // This fresh thread attaches before creating any window. On Windows,
        // SDL's video-init thread owns all its native GUI input and rendering.
        std::jthread acceptance([&] {
            try {
                require(SetThreadDesktop(desktop.get()),
                        "Cannot select isolated clipboard desktop");
                client_acceptance(desktop.get(),
                                  fs::path(executable).parent_path() / "astra-clipboard-host.exe");
                gui_acceptance(desktop.get(), workspace);
                outcome = 0;
            } catch (const std::exception& error) {
                std::cerr << "Isolated clipboard worker failed: " << error.what() << '\n';
            }
        });
        acceptance.join();
        // No thread remains attached when the station's owned handles close.
        desktop.close();
        require(outcome == 0, "Isolated clipboard acceptance failed");
        const auto issues =
            astra::test::cleanup_owned_directory(workspace, parent, "gui-clipboard-async-");
        require(issues.empty(), "Async clipboard workspace cleanup failed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Async clipboard acceptance failed: " << error.what()
                  << "\nPreserved fixture: " << workspace << '\n';
        return 1;
    }
}
