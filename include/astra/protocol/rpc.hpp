#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace astra::protocol {

using Json = nlohmann::json;

class RpcError final : public std::runtime_error {
public:
    RpcError(int code, std::string message, std::optional<Json> data = std::nullopt);
    [[nodiscard]] int code() const noexcept;
    [[nodiscard]] const std::optional<Json>& data() const noexcept;

private:
    int code_;
    std::optional<Json> data_;
};

// JSON-RPC 2.0 server dispatch, independent of framing and transport. A caller
// serializes access to this object and its handlers. Batch members execute in
// input order; a batch is not an atomic transaction. Missing params becomes an
// empty object. Notifications never produce a response, including handler errors.
// String request IDs are limited to 256 UTF-8 bytes; an oversized ID is an invalid
// request with a null response ID. Both inbound and outbound JSON bodies are
// bounded to 8 MiB, including escaping and the complete batch envelope.
//
// A handler may finish changing state before its result exceeds the reply budget.
// In that case error -32005 carries handlerCompleted=true/resyncRequired=true.
// This does not roll back an edit. Clients must fetch a current (paged) snapshot
// and revision before deciding whether to retry a versioned mutation. Remaining
// batch members still execute in order, and each retains its own response ID.
class Dispatcher {
public:
    using Handler = std::function<Json(const Json&)>;
    static constexpr std::size_t max_batch_size = 128;
    static constexpr std::size_t max_depth = 64;
    static constexpr std::size_t max_body_bytes = 8 * 1024 * 1024;
    static constexpr std::size_t max_request_id_bytes = 256;

    void register_method(std::string method, Handler handler);
    [[nodiscard]] std::optional<Json> dispatch(const Json& request) const;
    [[nodiscard]] std::optional<std::string> dispatch_text(std::string_view body) const;

private:
    using Reply = std::pair<Json, std::size_t>;
    [[nodiscard]] std::optional<Reply> dispatch_one(const Json& request,
                                                  std::size_t response_budget,
                                                  std::size_t nesting_depth = 0) const;
    [[nodiscard]] std::optional<Json> dispatch_validated(const Json& request) const;
    std::unordered_map<std::string, Handler> handlers_;
};

[[nodiscard]] Json make_notification(std::string method, Json params);

} // namespace astra::protocol
