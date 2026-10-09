#include <astra/ui/editor_window.hpp>
#include <astra/service/service_host.hpp>
#include <astra/recovery/store.hpp>
#include <astra/io/file_store.hpp>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
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
    if (!condition) { throw std::runtime_error(message); }
}

Json rpc(const std::shared_ptr<astra::service::ServiceHost>& host, std::string method, Json params = Json::object()) {
    auto pending = host->submit(Json{{"jsonrpc", "2.0"}, {"id", "recovery-verification"},
        {"method", std::move(method)}, {"params", std::move(params)}}.dump());
    require(pending.wait_for(std::chrono::seconds(15)) == std::future_status::ready, "Recovery verification timed out");
    const auto body = pending.get(); require(body.has_value(), "Recovery verification has no response");
    const auto result = Json::parse(*body);
    require(!result.contains("error"), "Recovery verification failed: " + result.dump());
    return result.at("result");
}

void settle(astra::ui::EditorWindow& window, bool allow_error = false) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    do {
        require(window.pump(), "Recovery window closed without the expected confirmation");
        require(allow_error || window.last_error().empty(), "Recovery UI failed: " + window.last_error());
        if (window.idle()) { return; }
        SDL_Delay(2);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Recovery UI did not settle");
}

void enter(astra::ui::EditorWindow& window) {
    SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN; event.key.key = SDLK_RETURN;
    event.key.down = true; event.key.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Queue recovery selection key"); settle(window);
}

// All successful calls go to the real production ServiceHost. Only the explicit
// failure test injects one transport error for the shutdown checkpoint request;
// this does not substitute for durable storage or the renderer under test.
class CheckpointFaultChannel final : public astra::ui::EditorChannel {
public:
    explicit CheckpointFaultChannel(std::shared_ptr<astra::service::ServiceHost> input) : host(std::move(input)) {}
    std::future<std::optional<std::string>> submit(std::string body) override {
        const auto request = Json::parse(body);
        if (request.at("method") == "recovery.flush") {
            ++flush_requests;
            if (fail_next_flush) {
                fail_next_flush = false;
                std::promise<std::optional<std::string>> response;
                response.set_value(Json{{"jsonrpc", "2.0"}, {"id", request.at("id")},
                    {"error", {{"code", -32010}, {"message", "Injected checkpoint transport failure"}}}}.dump());
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
            if (Json::parse(message).value("method", std::string{}) == "recovery.available") { received_available = true; }
        }
        return notifications;
    }
    void release_checkpoint() {
        require(withheld.has_value() && reply.has_value(), "No checkpoint completion is being held");
        require(withheld->wait_for(std::chrono::seconds(15)) == std::future_status::ready, "Actual durable checkpoint timed out");
        reply->set_value(withheld->get()); withheld.reset(); reply.reset();
    }
    std::shared_ptr<astra::service::ServiceHost> host;
    bool fail_next_flush{};
    bool hold_next_flush{};
    bool received_available{};
    std::optional<std::future<std::optional<std::string>>> withheld;
    std::optional<std::promise<std::optional<std::string>>> reply;
    std::size_t flush_requests{};
};

void wait_closed(astra::ui::EditorWindow& window) {
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    do {
        if (!window.pump()) { return; }
        require(window.last_error().empty(), "Checkpoint failed during successful exit: " + window.last_error());
        SDL_Delay(2);
    } while (Clock::now() < deadline);
    throw std::runtime_error("Successful checkpoint did not release window shutdown");
}
} // namespace

int main() {
    try {
        const auto fixture = fs::absolute(fs::path("build/graphics") /
            ("gui-recovery-" + std::to_string(Clock::now().time_since_epoch().count())));
        const auto workspace = fixture / "workspace";
        const auto recovery = fixture / "state";
        fs::create_directories(workspace);
        const auto original = astra::io::save_file(workspace / "recovered.txt", "saved baseline\n", astra::io::TextEncoding::utf8);
        const std::string unsaved = "Recovered 中文🙂 e\xCC\x81\nunsaved edits\n";
        std::string abandoned_session;
        {
            astra::Document document({97}, unsaved);
            document.invalidate_saved_state();
            document.set_selections({{{0}, {9}}});
            astra::recovery::RecoveryStore store(recovery);
            abandoned_session = store.session_id();
            store.checkpoint({document.snapshot(), workspace / "recovered.txt", astra::io::TextEncoding::utf8, original});
        }
        astra::service::ServiceHostOptions service;
        service.workspace = workspace; service.recovery_directory = recovery;
        service.resource_directory = fs::current_path();
        service.defaults_script = astra::io::read_file("data/defaults.js").text;
        auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
        auto channel = std::make_shared<CheckpointFaultChannel>(host);
        astra::ui::EditorWindowOptions options;
        options.title = "Astra — recovery and durable-exit acceptance"; options.workspace_name = "RECOVERY";
        astra::ui::EditorWindow window(channel, std::move(options)); settle(window);
        const auto initial = window.active_document_id();
        require(rpc(host, "document.get", {{"id", initial}}).at("text") == "", "Startup silently restored unsaved content");
        const auto discovery_deadline = Clock::now() + std::chrono::seconds(15);
        while (!channel->received_available && Clock::now() < discovery_deadline) {
            require(window.pump(), "Window closed during recovery discovery");
            require(window.last_error().empty(), "Recovery discovery failed: " + window.last_error());
            SDL_Delay(2);
        }
        require(channel->received_available, "Lazy recovery discovery did not notify the GUI");
        window.request_screenshot("build/graphics/gui-recovery-available.png"); settle(window);
        window.execute_ui_command("ui.recovery"); settle(window);
        window.request_screenshot("build/graphics/gui-recovery-sessions.png"); settle(window);
        enter(window);
        window.request_screenshot("build/graphics/gui-recovery-confirm.png"); settle(window);
        require(window.active_document_id() == initial, "Recovery ran before explicit confirmation");
        require(astra::io::read_file(workspace / "recovered.txt").text == "saved baseline\n", "Recovery preview modified the saved file");
        window.execute_ui_command("ui.confirmRecovery"); settle(window);
        const auto restored = window.active_document_id();
        require(restored != initial, "Confirmed recovery did not open its document tab");
        const auto state = rpc(host, "document.get", {{"id", restored}});
        require(state.at("text") == unsaved && state.at("dirty") == true, "Recovered Unicode content was lost or marked clean");
        require(state.at("selections").front().at("head") == 9, "Recovered selection was not preserved");
        require(astra::io::read_file(workspace / "recovered.txt").text == "saved baseline\n", "Restore wrote the original file without Save");
        window.execute_ui_command("ui.checkpoint"); settle(window);
        window.execute_ui_command("ui.save"); settle(window);
        require(astra::io::read_file(workspace / "recovered.txt").text == unsaved, "Explicit save did not persist recovered text");
        require(rpc(host, "document.get", {{"id", restored}}).at("dirty") == false, "Recovered buffer stayed dirty after confirmed save");

        const auto before_exit = channel->flush_requests;
        channel->fail_next_flush = true;
        window.execute_ui_command("ui.closeWindow"); settle(window, true);
        require(channel->flush_requests == before_exit + 1, "Window exit skipped the recovery flush");
        require(window.last_error().find("Injected checkpoint") != std::string::npos, "Checkpoint failure was not shown to the user");
        window.request_screenshot("build/graphics/gui-recovery-exit-failed.png"); settle(window, true);
        window.execute_ui_command("ui.cancel"); settle(window);
        require(window.pump(), "Keep open dismissed the window after checkpoint failure");
        channel->hold_next_flush = true;
        window.execute_ui_command("ui.closeWindow");
        const auto checkpoint_deadline = Clock::now() + std::chrono::seconds(15);
        while (!channel->withheld && Clock::now() < checkpoint_deadline) {
            require(window.pump(), "Window closed before receiving checkpoint completion"); SDL_Delay(2);
        }
        require(channel->withheld.has_value(), "Exit did not dispatch a checkpoint");
        window.request_screenshot("build/graphics/gui-recovery-flushing.png");
        require(window.pump(), "Window closed while checkpoint completion was pending");
        channel->release_checkpoint(); wait_closed(window);
        require(channel->flush_requests == before_exit + 2, "Retry did not perform a real checkpoint");

        // The explicit bypass is available only after a failed checkpoint. Calling
        // it during ordinary editing must not close the window.
        astra::ui::EditorWindowOptions second_options; second_options.title = "Astra — explicit recovery exit confirmation";
        astra::ui::EditorWindow second(channel, std::move(second_options)); settle(second);
        second.execute_ui_command("ui.exitWithoutCheckpoint"); settle(second);
        channel->fail_next_flush = true;
        second.execute_ui_command("ui.closeWindow"); settle(second, true);
        second.execute_ui_command("ui.exitWithoutCheckpoint");
        require(!second.pump(), "Explicit exit confirmation did not close the failed-checkpoint window");
        std::cout << "Real Windows GPU GUI + RecoveryStore/ServiceHost: explicit Unicode recovery, dirty selection, save, durable exit, failure retention/retry/confirmation: PASS (session " << abandoned_session << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI recovery acceptance failed: " << error.what() << '\n'; return 1;
    }
}
