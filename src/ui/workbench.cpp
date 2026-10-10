#include <astra/ui/workbench.hpp>

#include <chrono>
#include <algorithm>
#include <deque>
#include <limits>
#include <utility>

namespace astra::ui {
struct Workbench::Impl {
    struct Request {
        std::string id;
        std::future<std::optional<std::string>> response;
        std::promise<Json> completion;
    };

    std::shared_ptr<EditorChannel> channel;
    NativeRequest native;
    std::deque<Request> requests;
    std::map<script::Runtime::TaskId, Completion> commands;
    std::unique_ptr<script::Runtime> runtime;
    std::map<std::string, std::string> default_bindings;
    std::uint64_t next_id{1};

    Impl(std::shared_ptr<EditorChannel> transport,
         NativeRequest adapter,
         const std::map<std::string, std::string>& modules,
         std::string_view user)
        : channel(std::move(transport)), native(std::move(adapter)) {
        if (!channel || !native) {
            throw std::invalid_argument("Workbench requires transport and native UI primitives");
        }
        script::Runtime::Limits budget;
        budget.execution_time = std::chrono::milliseconds(8);
        budget.task_timeout = std::chrono::milliseconds(310000);
        runtime = std::make_unique<script::Runtime>(
            script::Runtime::AsyncRequest{[this](std::string_view method, const Json& params) {
                if (method.starts_with("_ui.")) {
                    return native(method, params);
                }
                if (requests.size() >= 256) {
                    throw script::ScriptError("Workbench transport queue is full");
                }
                if (next_id == std::numeric_limits<std::uint64_t>::max()) {
                    throw script::ScriptError("Workbench request ID capacity exceeded");
                }
                auto id = "workbench-" + std::to_string(next_id++);
                std::promise<Json> completion;
                auto future = completion.get_future();
                auto response = channel->submit(
                    Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}
                        .dump());
                requests.push_back({std::move(id), std::move(response), std::move(completion)});
                return future;
            }},
            budget);
        static_cast<void>(runtime->evaluate("astra.context='client';"));
        for (const auto& [name, source] : modules) {
            runtime->define_module(name, source);
        }
        static_cast<void>(runtime->evaluate_module(
            "import {initialize} from 'astra/workbench'; initialize();", "astra-client.js"));
        default_bindings = runtime->keybindings();
        if (!user.empty()) {
            static_cast<void>(runtime->evaluate(user, "astra-client-user.js"));
        }
    }

    void poll() {
        for (auto it = requests.begin(); it != requests.end();) {
            if (it->response.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            auto request = std::move(*it);
            it = requests.erase(it);
            try {
                const auto body = request.response.get();
                if (!body) {
                    throw protocol::RpcError(-32603, "Workbench request returned no response");
                }
                const auto response = Json::parse(*body);
                if (response.at("id") != request.id) {
                    throw protocol::RpcError(-32603, "Workbench response ID mismatch");
                }
                if (response.contains("error")) {
                    const auto& error = response.at("error");
                    throw protocol::RpcError(error.at("code").get<int>(),
                                             error.at("message").get<std::string>(),
                                             error.contains("data")
                                                 ? std::optional<Json>(error.at("data"))
                                                 : std::nullopt);
                }
                request.completion.set_value(response.at("result"));
            } catch (...) {
                request.completion.set_exception(std::current_exception());
            }
        }
        runtime->poll();
        for (auto it = commands.begin(); it != commands.end();) {
            const auto result = runtime->take_result(it->first);
            if (!result) {
                ++it;
                continue;
            }
            auto complete = std::move(it->second);
            it = commands.erase(it);
            if (complete) {
                complete(*result);
            }
        }
    }
};

Workbench::Workbench(std::shared_ptr<EditorChannel> channel,
                     NativeRequest native,
                     const std::map<std::string, std::string>& modules,
                     std::string_view user)
    : impl_(std::make_unique<Impl>(std::move(channel), std::move(native), modules, user)) {
}

Workbench::~Workbench() = default;

bool Workbench::contains(std::string_view command) const {
    const auto names = impl_->runtime->commands();
    return std::ranges::find(names, command) != names.end();
}

void Workbench::execute(std::string_view name, const Json& params, Completion complete) {
    const auto task = impl_->runtime->start_command(name, params);
    impl_->commands.emplace(task, std::move(complete));
}

void Workbench::poll() {
    impl_->poll();
}

void Workbench::emit(std::string_view event, const Json& params) {
    impl_->runtime->emit(event, params);
}

bool Workbench::idle() const {
    return impl_->commands.empty() && impl_->requests.empty() && impl_->runtime->idle();
}

bool Workbench::awaiting_input() const {
    return awaiting("_ui.dialog");
}

bool Workbench::awaiting(std::string_view method) const {
    return impl_->requests.empty() && impl_->runtime->waiting_only(method);
}

bool Workbench::awaiting(std::span<const std::string_view> methods) const {
    return impl_->requests.empty() && impl_->runtime->waiting_only(methods);
}

Workbench::Json Workbench::settings() const {
    return impl_->runtime->settings();
}

std::map<std::string, std::string> Workbench::keybindings() const {
    auto result = impl_->runtime->keybindings();
    std::erase_if(result, [this](const auto& entry) {
        const auto found = impl_->default_bindings.find(entry.first);
        return found != impl_->default_bindings.end() && found->second == entry.second;
    });
    return result;
}

std::vector<Workbench::Json> Workbench::take_errors() {
    return impl_->runtime->take_errors();
}

void Workbench::shutdown() {
    impl_->runtime->shutdown();
    impl_->commands.clear();
}
} // namespace astra::ui
