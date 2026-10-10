#include <astra/protocol/rpc.hpp>

#include <cmath>
#include <ostream>
#include <streambuf>
#include <utility>

namespace astra::protocol {
namespace {

// A string ID may require six bytes per control character. The remaining 512
// bytes cover the fixed -32005 envelope and its data fields with ample space.
constexpr std::size_t error_reserve_bytes = Dispatcher::max_request_id_bytes * 6 + 512;
static_assert(Dispatcher::max_batch_size * (error_reserve_bytes + 1) + 2 <
              Dispatcher::max_body_bytes);

struct BodyLimit {};

struct DepthLimit {};

// nlohmann's compact ostream serializer counts UTF-8 and JSON escaping exactly.
// This sink never writes beyond its limit; the counting mode allocates no output
// string, while the collecting mode cannot grow beyond the permitted body size.
class BoundedJsonSink final : public std::streambuf {
  public:
    explicit BoundedJsonSink(std::size_t limit, std::string* output = nullptr)
        : remaining_(limit), output_(output) {
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool exceeded() const noexcept {
        return exceeded_;
    }

  private:
    void consume(std::size_t count) {
        if (count > remaining_) {
            exceeded_ = true;
            throw BodyLimit{};
        }
        remaining_ -= count;
        size_ += count;
    }

    std::streamsize xsputn(const char* bytes, std::streamsize count) override {
        if (count <= 0) {
            return count;
        }
        const auto length = static_cast<std::size_t>(count);
        consume(length);
        if (output_) {
            output_->append(bytes, length);
        }
        return count;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) {
            return traits_type::not_eof(value);
        }
        consume(1);
        if (output_) {
            output_->push_back(traits_type::to_char_type(value));
        }
        return value;
    }

    std::size_t remaining_;
    std::size_t size_{};
    std::string* output_;
    bool exceeded_{};
};

std::size_t serialize_limited(const Json& value, std::size_t limit, std::string* output = nullptr) {
    BoundedJsonSink sink(limit, output);
    std::ostream stream(&sink);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    try {
        stream << value;
    } catch (...) {
        if (sink.exceeded()) {
            throw BodyLimit{};
        }
        throw;
    }
    return sink.size();
}

std::string serialize_reply(const Json& value) {
    std::string result;
    (void)serialize_limited(value, Dispatcher::max_body_bytes, &result);
    return result;
}

Json error_response(const Json& id,
                    int code,
                    std::string_view message,
                    const std::optional<Json>& data = std::nullopt) {
    Json error{{"code", code}, {"message", message}};
    if (data) {
        error["data"] = *data;
    }
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", std::move(error)}};
}

Json invalid_request(std::string_view detail = {}) {
    if (detail.empty()) {
        return error_response(nullptr, -32600, "Invalid Request");
    }
    return error_response(nullptr, -32600, "Invalid Request", Json{{"detail", detail}});
}

bool valid_id(const Json& id) {
    return (id.is_string() &&
            id.get_ref<const std::string&>().size() <= Dispatcher::max_request_id_bytes) ||
           id.is_null() || id.is_number_integer() ||
           (id.is_number_float() && std::isfinite(id.get<double>()));
}

// Recursion is bounded to 64 container levels, so wide arrays need no auxiliary
// allocation proportional to their width. Check before invoking the serializer.
bool valid_structure(const Json& value, std::size_t depth = 0) {
    if (value.is_discarded() || value.is_binary() ||
        (value.is_number_float() && !std::isfinite(value.get<double>()))) {
        return false;
    }
    if (!value.is_structured()) {
        return true;
    }
    if (depth >= Dispatcher::max_depth) {
        return false;
    }
    for (const auto& child : value) {
        if (!valid_structure(child, depth + 1)) {
            return false;
        }
    }
    return true;
}

std::pair<Json, std::size_t>
bounded_response(Json response, std::size_t budget, std::size_t nesting_depth) {
    const Json id = response["id"];
    if (!valid_structure(response, nesting_depth)) {
        response = error_response(id, -32603, "Internal error");
    }
    try {
        const auto size = serialize_limited(response, budget);
        return {std::move(response), size};
    } catch (const BodyLimit&) {
        response = error_response(id,
                                  -32005,
                                  "Response exceeds size limit",
                                  Json{{"handlerCompleted", true},
                                       {"resyncRequired", true},
                                       {"maxBodyBytes", Dispatcher::max_body_bytes}});
    } catch (const Json::exception&) {
        response = error_response(id, -32603, "Internal error");
    }
    // The batch scheduler reserves this complete fallback before running each
    // handler. Capped IDs keep even maximally escaped fallback messages bounded.
    const auto size = serialize_limited(response, budget);
    return {std::move(response), size};
}

} // namespace

RpcError::RpcError(int code, std::string message, std::optional<Json> data)
    : std::runtime_error(std::move(message)), code_(code), data_(std::move(data)) {
}

int RpcError::code() const noexcept {
    return code_;
}

const std::optional<Json>& RpcError::data() const noexcept {
    return data_;
}

void Dispatcher::register_method(std::string method, Handler handler) {
    if (method.empty() || method.starts_with("rpc.") || !handler) {
        throw std::invalid_argument("RPC registration requires a non-reserved method and handler");
    }
    if (!handlers_.emplace(std::move(method), std::move(handler)).second) {
        throw std::invalid_argument("RPC method is already registered");
    }
}

std::optional<Dispatcher::Reply> Dispatcher::dispatch_one(const Json& request,
                                                          std::size_t response_budget,
                                                          std::size_t nesting_depth) const {
    if (!request.is_object() || !request.contains("jsonrpc") || request["jsonrpc"] != "2.0" ||
        !request.contains("method") || !request["method"].is_string() ||
        (request.contains("id") && !valid_id(request["id"]))) {
        return bounded_response(invalid_request(), response_budget, nesting_depth);
    }
    const bool notification = !request.contains("id");
    const Json id = notification ? Json(nullptr) : request["id"];
    const auto fail = [&](int code,
                          std::string_view message,
                          const std::optional<Json>& data = std::nullopt) -> std::optional<Reply> {
        if (notification) {
            return std::nullopt;
        }
        return bounded_response(
            error_response(id, code, message, data), response_budget, nesting_depth);
    };
    if (request.contains("params") && !request["params"].is_structured()) {
        return fail(-32602, "Invalid params");
    }
    const auto& method = request["method"].get_ref<const std::string&>();
    const auto found = handlers_.find(method);
    if (found == handlers_.end()) {
        return fail(-32601, "Method not found");
    }
    const Json no_params = Json::object();
    const auto& params = request.contains("params") ? request["params"] : no_params;
    try {
        auto result = found->second(params);
        if (notification) {
            return std::nullopt;
        }
        return bounded_response(Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}},
                                response_budget,
                                nesting_depth);
    } catch (const RpcError& error) {
        return fail(error.code(), error.what(), error.data());
    } catch (...) {
        return fail(-32603, "Internal error");
    }
}

std::optional<Json> Dispatcher::dispatch_validated(const Json& request) const {
    if (!request.is_array()) {
        auto response = dispatch_one(request, max_body_bytes);
        return response ? std::optional<Json>(std::move(response->first)) : std::nullopt;
    }
    if (request.empty() || request.size() > max_batch_size) {
        return invalid_request("Batch must contain between 1 and 128 requests");
    }
    auto responses = Json::array();
    std::size_t bytes = 2; // Opening and closing array brackets.
    for (std::size_t index = 0; index < request.size(); ++index) {
        const auto remaining = request.size() - index - 1;
        const std::size_t separator = responses.empty() ? 0 : 1;
        const auto reserve = remaining * (error_reserve_bytes + 1);
        const auto budget = max_body_bytes - bytes - separator - reserve;
        if (auto response = dispatch_one(request[index], budget, 1)) {
            bytes += separator + response->second;
            responses.push_back(std::move(response->first));
        }
    }
    if (responses.empty()) {
        return std::nullopt;
    }
    return responses;
}

std::optional<Json> Dispatcher::dispatch(const Json& request) const {
    if (!valid_structure(request)) {
        return invalid_request("Unsupported JSON value or nesting depth exceeds 64");
    }
    try {
        (void)serialize_limited(request, max_body_bytes);
    } catch (const BodyLimit&) {
        return invalid_request("Request body exceeds 8 MiB");
    } catch (const Json::exception&) {
        return invalid_request("Request contains invalid JSON data");
    }
    return dispatch_validated(request);
}

std::optional<std::string> Dispatcher::dispatch_text(std::string_view body) const {
    if (body.size() > max_body_bytes) {
        return serialize_reply(invalid_request("Request body exceeds 8 MiB"));
    }
    Json request;
    try {
        request =
            Json::parse(body.begin(), body.end(), [](int depth, Json::parse_event_t event, Json&) {
                if ((event == Json::parse_event_t::object_start ||
                     event == Json::parse_event_t::array_start) &&
                    depth >= static_cast<int>(max_depth)) {
                    throw DepthLimit{};
                }
                return true;
            });
    } catch (const DepthLimit&) {
        return serialize_reply(invalid_request("JSON nesting depth exceeds 64"));
    } catch (const Json::exception&) {
        return serialize_reply(error_response(nullptr, -32700, "Parse error"));
    }
    auto response = dispatch_validated(request);
    return response ? std::optional<std::string>(serialize_reply(*response)) : std::nullopt;
}

Json make_notification(std::string method, Json params) {
    if (method.empty() || !params.is_structured()) {
        throw std::invalid_argument("RPC notification requires a method and structured params");
    }
    return Json{{"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
}

} // namespace astra::protocol
