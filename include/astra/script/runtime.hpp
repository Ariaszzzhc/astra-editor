#pragma once

#include <astra/protocol/rpc.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <cstdint>
#include <optional>
#include <span>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace astra::script {

class ScriptError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Thread-affine QuickJS-NG host. Scripts call the production RPC dispatcher;
// they never own C++ document pointers. Public values cross the boundary as JSON.
class Runtime {
public:
    using Json = protocol::Json;
    using Request = std::function<Json(std::string_view, const Json&)>;
    // Submit must return promptly. Futures are produced by the owner of the
    // transport, never by blocking std::async work owned by the runtime.
    using AsyncRequest = std::function<std::future<Json>(std::string_view, const Json&)>;
    using TaskId = std::uint64_t;
    struct Outcome {
        Json value;
        std::optional<Json> error;
    };
    struct Limits {
        std::size_t memory_bytes = 64 * 1024 * 1024;
        std::size_t stack_bytes = 512 * 1024;
        std::chrono::milliseconds execution_time{200};
        std::size_t max_pending_jobs = 1024;
        std::size_t max_pending_requests = 256;
        std::size_t max_tasks = 256;
        std::chrono::milliseconds request_timeout{30000};
        std::chrono::milliseconds task_timeout{30000};
    };

    explicit Runtime(Request request);
    Runtime(Request request, Limits limits);
    explicit Runtime(AsyncRequest request);
    Runtime(AsyncRequest request, Limits limits);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] Json evaluate(std::string_view source, std::string_view filename = "user.js");
    [[nodiscard]] Json execute_command(std::string_view name, const Json& params = Json::object());
    [[nodiscard]] TaskId start_command(std::string_view name, const Json& params = Json::object());
    [[nodiscard]] TaskId start_evaluation(std::string_view source, std::string_view filename = "user.js");
    // Nonblocking owner-loop tick. Each tick has a CPU/job budget; awaiting I/O
    // does not consume that budget. Results remain owned until take_result().
    void poll();
    [[nodiscard]] std::optional<Outcome> take_result(TaskId task);
    void cancel(TaskId task);
    // Stop accepting work and reject/cancel outstanding requests. Keep polling
    // until idle before closing the transport, so late operation IDs are cancelled.
    void shutdown();
    [[nodiscard]] std::vector<Json> take_errors();
    [[nodiscard]] bool idle() const;
    // True only when every outstanding task/request awaits this host method.
    [[nodiscard]] bool waiting_only(std::string_view method) const;
    [[nodiscard]] bool waiting_only(std::span<const std::string_view> methods) const;
    void emit(std::string_view event, const Json& payload);
    void define_module(std::string name, std::string source);
    [[nodiscard]] Json evaluate_module(std::string_view source, std::string_view filename);
    [[nodiscard]] std::vector<std::string> commands() const;
    [[nodiscard]] Json settings() const;
    [[nodiscard]] std::map<std::string, std::string> keybindings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::script
