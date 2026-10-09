#pragma once

#include <astra/protocol/rpc.hpp>

#include <chrono>
#include <cstddef>
#include <functional>
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
    struct Limits {
        std::size_t memory_bytes = 64 * 1024 * 1024;
        std::size_t stack_bytes = 512 * 1024;
        std::chrono::milliseconds execution_time{200};
        std::size_t max_pending_jobs = 1024;
    };

    explicit Runtime(Request request);
    Runtime(Request request, Limits limits);
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&) = delete;
    Runtime& operator=(Runtime&&) = delete;

    [[nodiscard]] Json evaluate(std::string_view source, std::string_view filename = "user.js");
    [[nodiscard]] Json execute_command(std::string_view name, const Json& params = Json::object());
    [[nodiscard]] std::vector<std::string> commands() const;
    [[nodiscard]] Json settings() const;
    [[nodiscard]] std::map<std::string, std::string> keybindings() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::script
