#include <astra/ui/editor_window.hpp>
#include <astra/service/service_host.hpp>
#include <astra/recovery/store.hpp>
#include <astra/io/file_store.hpp>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Json rpc(const std::shared_ptr<astra::service::ServiceHost>& host,
         std::string method,
         Json params = Json::object()) {
    auto pending = host->submit(Json{
        {"jsonrpc", "2.0"},
        {"id", "recovery-verification"},
        {"method", std::move(method)},
        {"params",
         std::move(params)}}.dump());
    require(pending.wait_for(std::chrono::seconds(15)) == std::future_status::ready,
            "Recovery verification timed out");
    const auto body = pending.get();
    require(body.has_value(), "Recovery verification has no response");
    const auto result = Json::parse(*body);
    require(!result.contains("error"), "Recovery verification failed: " + result.dump());
    return result.at("result");
}

void settle(astra::ui::EditorWindow& window, bool allow_error = false) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    do {
        require(window.pump(), "Recovery window closed without the expected confirmation");
        require(allow_error || window.last_error().empty(),
                "Recovery UI failed: " + window.last_error());
        if (window.idle()) {
            return;
        }
        SDL_Delay(2);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Recovery UI did not settle");
}

void enter(astra::ui::EditorWindow& window) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_RETURN;
    event.key.down = true;
    event.key.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Queue recovery selection key");
    settle(window);
}

// All successful calls go to the real production ServiceHost. Only the explicit
// failure test injects one transport error for the shutdown checkpoint request;
// this does not substitute for durable storage or the renderer under test.
class CheckpointFaultChannel final : public astra::ui::EditorChannel {
  public:
    explicit CheckpointFaultChannel(std::shared_ptr<astra::service::ServiceHost> input)
        : host(std::move(input)) {
    }

    std::future<std::optional<std::string>> submit(std::string body) override {
        const auto request = Json::parse(body);
        if (request.at("method") == "recovery.flush") {
            ++flush_requests;
            if (fail_next_flush) {
                fail_next_flush = false;
                std::promise<std::optional<std::string>> response;
                response.set_value(
                    Json{{"jsonrpc", "2.0"},
                         {"id", request.at("id")},
                         {"error",
                          {{"code", -32010}, {"message", "Injected checkpoint transport failure"}}}}
                        .dump());
                return response.get_future();
            }
            if (hold_next_flush) {
                hold_next_flush = false;
                withheld.emplace(host->submit(std::move(body)));
                reply.emplace();
                return reply->get_future();
            }
        }
        return host->submit(std::move(body));
    }

    std::vector<std::string> take_notifications() override {
        auto notifications = host->take_notifications();
        for (const auto& message : notifications) {
            if (Json::parse(message).value("method", std::string{}) == "recovery.available") {
                received_available = true;
            }
        }
        return notifications;
    }

    void release_checkpoint() {
        require(withheld.has_value() && reply.has_value(),
                "No checkpoint completion is being held");
        require(withheld->wait_for(std::chrono::seconds(15)) == std::future_status::ready,
                "Actual durable checkpoint timed out");
        reply->set_value(withheld->get());
        withheld.reset();
        reply.reset();
    }

    std::shared_ptr<astra::service::ServiceHost> host;
    bool fail_next_flush{};
    bool hold_next_flush{};
    bool received_available{};
    std::optional<std::future<std::optional<std::string>>> withheld;
    std::optional<std::promise<std::optional<std::string>>> reply;
    std::size_t flush_requests{};
};

void wait_closed(astra::ui::EditorWindow& window, bool confirmed_bypass = false) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    do {
        if (!window.pump()) {
            return;
        }
        require(confirmed_bypass || window.last_error().empty(),
                "Checkpoint failed during successful exit: " + window.last_error());
        SDL_Delay(2);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Successful checkpoint did not release window shutdown");
}
} // namespace

int main() {
    try {
        const auto fixture = fs::absolute(
            fs::path("build/graphics") /
            ("gui-recovery-" + std::to_string(Clock::now().time_since_epoch().count())));
        const auto workspace = fixture / "workspace";
        const auto recovery = fixture / "state";
        fs::create_directories(workspace);
        const auto original = astra::io::save_file(
            workspace / "recovered.txt", "saved baseline\n", astra::io::TextEncoding::utf8);
        const std::string unsaved = "Recovered 中文🙂 e\xCC\x81\nunsaved edits\n";
        std::string abandoned_session;
        {
            astra::Document document({97}, unsaved);
            document.invalidate_saved_state();
            document.set_selections({{{0}, {9}}});
            astra::recovery::RecoveryStore store(recovery);
            abandoned_session = store.session_id();
            store.checkpoint({document.snapshot(),
                              workspace / "recovered.txt",
                              astra::io::TextEncoding::utf8,
                              original});
        }
        astra::service::ServiceHostOptions service;
        service.workspace = workspace;
        service.recovery_directory = recovery;
        service.resource_directory = fs::current_path();
        service.defaults_script = astra::io::read_file("data/defaults.js").text;
        auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
        auto channel = std::make_shared<CheckpointFaultChannel>(host);
        astra::ui::EditorWindowOptions options;
        options.title = "Astra — recovery and durable-exit acceptance";
        options.workspace_name = "RECOVERY";
        astra::ui::EditorWindow window(channel, std::move(options));
        settle(window);
        const auto initial = window.active_document_id();
        require(rpc(host, "document.get", {{"id", initial}}).at("text") == "",
                "Startup silently restored unsaved content");
        const auto discovery_deadline = Clock::now() + std::chrono::seconds(15);
        while (!channel->received_available && Clock::now() < discovery_deadline) {
            require(window.pump(), "Window closed during recovery discovery");
            require(window.last_error().empty(),
                    "Recovery discovery failed: " + window.last_error());
            SDL_Delay(2);
        }
        require(channel->received_available, "Lazy recovery discovery did not notify the GUI");
        window.request_screenshot("build/graphics/gui-recovery-available.png");
        settle(window);
        const auto notice_targets = window.view_state().chrome.at("targets");
        const auto recovery_target =
            std::find_if(notice_targets.begin(), notice_targets.end(), [](const auto& target) {
                return target.at("bank") == "notices" && target.at("row") == "recovery";
            });
        require(recovery_target != notice_targets.end(), "JS recovery notice is not visible");
        const auto& recovery_box = recovery_target->at("bounds");
        int window_width{}, window_height{}, pixels_width{}, pixels_height{};
        require(SDL_GetWindowSize(window.window(), &window_width, &window_height) &&
                    SDL_GetWindowSizeInPixels(window.window(), &pixels_width, &pixels_height),
                "Cannot read recovery notice dimensions");
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event pointer{};
            pointer.type = type;
            pointer.button.windowID = SDL_GetWindowID(window.window());
            pointer.button.button = SDL_BUTTON_LEFT;
            pointer.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            pointer.button.x =
                (recovery_box.at("x").get<float>() + recovery_box.at("width").get<float>() / 2) *
                static_cast<float>(window_width) / static_cast<float>(pixels_width);
            pointer.button.y =
                (recovery_box.at("y").get<float>() + recovery_box.at("height").get<float>() / 2) *
                static_cast<float>(window_height) / static_cast<float>(pixels_height);
            require(SDL_PushEvent(&pointer), "Cannot click real recovery notice");
        }
        settle(window);
        window.request_screenshot("build/graphics/gui-recovery-sessions.png");
        settle(window);
        enter(window);
        window.request_screenshot("build/graphics/gui-recovery-confirm.png");
        settle(window);
        require(window.active_document_id() == initial,
                "Recovery ran before explicit confirmation");
        require(astra::io::read_file(workspace / "recovered.txt").text == "saved baseline\n",
                "Recovery preview modified the saved file");
        window.execute_ui_command("ui.confirmRecovery");
        settle(window);
        const auto restored = window.active_document_id();
        require(restored != initial, "Confirmed recovery did not open its document tab");
        const auto state = rpc(host, "document.get", {{"id", restored}});
        require(state.at("text") == unsaved && state.at("dirty") == true,
                "Recovered Unicode content was lost or marked clean");
        require(state.at("selections").front().at("head") == 9,
                "Recovered selection was not preserved");
        require(astra::io::read_file(workspace / "recovered.txt").text == "saved baseline\n",
                "Restore wrote the original file without Save");
        window.execute_ui_command("ui.checkpoint");
        settle(window);
        window.execute_ui_command("ui.save");
        settle(window);
        require(astra::io::read_file(workspace / "recovered.txt").text == unsaved,
                "Explicit save did not persist recovered text");
        require(rpc(host, "document.get", {{"id", restored}}).at("dirty") == false,
                "Recovered buffer stayed dirty after confirmed save");

        const auto before_exit = channel->flush_requests;
        channel->fail_next_flush = true;
        window.execute_ui_command("ui.closeWindow");
        settle(window, true);
        require(channel->flush_requests == before_exit + 1,
                "Window exit skipped the recovery flush");
        require(window.last_error().find("Injected checkpoint") != std::string::npos,
                "Checkpoint failure was not shown to the user");
        window.request_screenshot("build/graphics/gui-recovery-exit-failed.png");
        settle(window, true);
        window.execute_ui_command("ui.cancel");
        settle(window);
        require(window.pump(), "Keep open dismissed the window after checkpoint failure");
        // Cancellation must sever the close continuation while the real service
        // reply is withheld; its later operation ID is drained without exiting.
        channel->hold_next_flush = true;
        window.execute_ui_command("ui.closeWindow");
        const auto cancelled_deadline = Clock::now() + std::chrono::seconds(15);
        while (!channel->withheld && Clock::now() < cancelled_deadline) {
            require(window.pump(), "Exit preceded the held checkpoint");
            SDL_Delay(2);
        }
        require(channel->withheld.has_value(), "Cancellation test did not dispatch a checkpoint");
        window.execute_ui_command("ui.cancel");
        for (int tick = 0; tick < 8; ++tick)
            require(window.pump(), "Cancelled checkpoint closed the window");
        window.execute_ui_command("ui.new");
        const auto create_deadline = Clock::now() + std::chrono::seconds(15);
        while (window.active_document_id().empty() && Clock::now() < create_deadline) {
            require(window.pump(), "Cancelled exit rejected new editing");
            SDL_Delay(2);
        }
        const auto after_cancel = window.active_document_id();
        require(!after_cancel.empty(), "Editing did not resume after cancelling exit");
        SDL_Event continued{};
        continued.type = SDL_EVENT_TEXT_INPUT;
        continued.text.windowID = SDL_GetWindowID(window.window());
        continued.text.text = "continued 中🙂 edits";
        require(SDL_PushEvent(&continued), "Cannot enqueue editing after exit cancellation");
        const auto edit_deadline = Clock::now() + std::chrono::seconds(15);
        while (Clock::now() < edit_deadline) {
            require(window.pump(), "Editing after cancellation closed the window");
            if (rpc(host, "document.get", {{"id", after_cancel}}).at("text") ==
                "continued 中🙂 edits")
                break;
            SDL_Delay(2);
        }
        channel->release_checkpoint();
        settle(window);
        require(window.pump() && window.active_document_id() == after_cancel,
                "A late cancelled checkpoint completed window exit");
        require(rpc(host, "document.get", {{"id", after_cancel}}).at("text") ==
                    "continued 中🙂 edits",
                "Cancellation lost accepted Unicode editing");
        window.execute_ui_command("ui.closeTab");
        settle(window);
        window.execute_ui_command("ui.discard");
        settle(window);
        require(channel->flush_requests == before_exit + 2,
                "Cancelled exit performed an unexpected checkpoint retry");
        channel->hold_next_flush = true;
        window.execute_ui_command("ui.closeWindow");
        const auto checkpoint_deadline = Clock::now() + std::chrono::seconds(15);
        while (!channel->withheld && Clock::now() < checkpoint_deadline) {
            require(window.pump(), "Window closed before receiving checkpoint completion");
            SDL_Delay(2);
        }
        require(channel->withheld.has_value(), "Exit did not dispatch a checkpoint");
        window.request_screenshot("build/graphics/gui-recovery-flushing.png");
        require(window.pump(), "Window closed while checkpoint completion was pending");
        // Another real client can create a dirty document during durable I/O.
        // Its arrival must cause a fresh user close decision and checkpoint.
        const auto late = rpc(host, "document.create");
        const auto late_id = late.at("id").get<std::string>();
        const auto late_edit = rpc(host,
                                   "document.insert",
                                   {{"id", late_id},
                                    {"baseRevision", late.at("revision")},
                                    {"text", "late 中🙂 edits\n"}});
        require(late_edit.at("dirty") == true,
                "Concurrent-exit fixture must contain actual unsaved editing");
        channel->release_checkpoint();
        settle(window);
        require(window.active_document_id() == late_id,
                "New dirty document was lost during checkpoint exit");
        require(rpc(host, "document.get", {{"id", late_id}}).at("text") == "late 中🙂 edits\n",
                "Checkpoint exit changed newly accepted content");
        require(channel->flush_requests == before_exit + 3,
                "Exit bypassed the new document close decision");
        window.execute_ui_command("ui.discard");
        wait_closed(window);
        require(channel->flush_requests == before_exit + 4,
                "Newly closed document did not receive a fresh exit checkpoint");

        // The explicit bypass is available only after a failed checkpoint. Calling
        // it during ordinary editing must not close the window.
        astra::ui::EditorWindowOptions second_options;
        second_options.title = "Astra — explicit recovery exit confirmation";
        astra::ui::EditorWindow second(channel, std::move(second_options));
        settle(second);
        second.execute_ui_command("ui.exitWithoutCheckpoint");
        settle(second);
        channel->fail_next_flush = true;
        second.execute_ui_command("ui.closeWindow");
        settle(second, true);
        const auto before_bypass = channel->flush_requests;
        second.execute_ui_command("ui.exitWithoutCheckpoint");
        wait_closed(second, true);
        require(channel->flush_requests == before_bypass,
                "Confirmed bypass dispatched an unexpected successful-checkpoint requirement");
        astra::ui::EditorWindowOptions retry_options;
        retry_options.title = "Astra — retry failed checkpoint";
        astra::ui::EditorWindow retry_window(channel, std::move(retry_options));
        settle(retry_window);
        const auto before_retry = channel->flush_requests;
        channel->fail_next_flush = true;
        retry_window.execute_ui_command("ui.closeWindow");
        settle(retry_window, true);
        channel->hold_next_flush = true;
        retry_window.execute_ui_command("ui.retryExit");
        const auto retry_deadline = Clock::now() + std::chrono::seconds(15);
        while (!channel->withheld && Clock::now() < retry_deadline) {
            require(retry_window.pump(), "Retry closed before durable completion");
            SDL_Delay(2);
        }
        require(channel->withheld.has_value(), "Retry did not dispatch another real checkpoint");
        require(channel->flush_requests == before_retry + 2,
                "Retry did not follow the failed checkpoint with exactly one new attempt");
        channel->release_checkpoint();
        wait_closed(retry_window);
        std::cout
            << "Real Windows GPU GUI + RecoveryStore/ServiceHost: explicit Unicode recovery, dirty "
               "selection, save, durable exit, failure retention/retry/confirmation: PASS (session "
            << abandoned_session << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI recovery acceptance failed: " << error.what() << '\n';
        return 1;
    }
}
