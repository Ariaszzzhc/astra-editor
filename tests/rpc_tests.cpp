#include <astra/protocol/rpc.hpp>
#include <astra/protocol/framing.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using astra::protocol::Dispatcher;
using astra::protocol::Json;
using astra::protocol::RpcError;

Json rpc_request(std::string method, Json params = Json::object(), Json id = 1) {
    return Json{{"jsonrpc", "2.0"},
                {"method", std::move(method)},
                {"params", std::move(params)},
                {"id", std::move(id)}};
}

void check_error(const std::optional<Json>& response, int code, const Json& id = nullptr) {
    REQUIRE(response.has_value());
    REQUIRE(response->is_object());
    CHECK((*response)["jsonrpc"] == "2.0");
    CHECK((*response)["id"] == id);
    CHECK_FALSE(response->contains("result"));
    REQUIRE(response->contains("error"));
    CHECK((*response)["error"]["code"] == code);
    CHECK((*response)["error"]["message"].is_string());
}

Json nested_value(std::size_t depth) {
    Json value = true;
    for (std::size_t level = 0; level < depth; ++level) {
        value = Json::array({std::move(value)});
    }
    return value;
}

} // namespace

TEST_CASE("RPC registration validates handlers and protects existing methods") {
    Dispatcher dispatcher;
    dispatcher.register_method("echo", [](const Json& params) {
        return params;
    });
    CHECK_THROWS_AS((void)(dispatcher.register_method("echo",
                                                      [](const Json&) {
                                                          return 0;
                                                      })),
                    std::invalid_argument);
    CHECK_THROWS_AS((void)(dispatcher.register_method("",
                                                      [](const Json&) {
                                                          return 0;
                                                      })),
                    std::invalid_argument);
    CHECK_THROWS_AS((void)(dispatcher.register_method("rpc.private",
                                                      [](const Json&) {
                                                          return 0;
                                                      })),
                    std::invalid_argument);
    CHECK_THROWS_AS((void)(dispatcher.register_method("missing", Dispatcher::Handler{})),
                    std::invalid_argument);
    const auto response = dispatcher.dispatch(rpc_request("echo", Json{{"value", 7}}));
    REQUIRE(response);
    CHECK((*response)["result"] == Json{{"value", 7}});
}

TEST_CASE("RPC preserves all permitted ID types and structured parameters") {
    Dispatcher dispatcher;
    dispatcher.register_method("echo", [](const Json& params) {
        return params;
    });
    const std::vector<Json> ids{
        nullptr, "request", "", 0, -1, 1.5, std::numeric_limits<std::uint64_t>::max()};
    for (const auto& id : ids) {
        const auto request = rpc_request("echo", Json::array({1, 2, 3}), id);
        const auto direct = dispatcher.dispatch(request);
        REQUIRE(direct);
        CHECK((*direct)["id"] == id);
        CHECK((*direct)["result"] == request["params"]);
        CHECK_FALSE(direct->contains("error"));
        const auto encoded = dispatcher.dispatch_text(request.dump());
        REQUIRE(encoded);
        CHECK(Json::parse(*encoded) == *direct);
    }
    auto no_params = rpc_request("echo");
    no_params.erase("params");
    const auto response = dispatcher.dispatch(no_params);
    REQUIRE(response);
    CHECK((*response)["result"] == Json::object());
}

TEST_CASE("RPC rejects invalid request envelopes without invoking a handler") {
    Dispatcher dispatcher;
    std::size_t calls = 0;
    dispatcher.register_method("echo", [&calls](const Json&) {
        ++calls;
        return true;
    });
    const std::vector<Json> invalid{
        nullptr,
        1,
        true,
        "request",
        Json::object(),
        Json{{"jsonrpc", "1.0"}, {"method", "echo"}, {"id", 1}},
        Json{{"jsonrpc", 2}, {"method", "echo"}, {"id", 1}},
        Json{{"jsonrpc", "2.0"}, {"method", 1}},
        Json{{"jsonrpc", "2.0"}, {"method", "echo"}, {"id", false}},
        Json{{"jsonrpc", "2.0"}, {"method", "echo"}, {"id", Json::object()}},
        Json{{"jsonrpc", "2.0"}, {"method", "echo"}, {"id", Json::array()}}};
    for (const auto& request : invalid) {
        check_error(dispatcher.dispatch(request), -32600);
    }
    CHECK(calls == 0);
}

TEST_CASE("RPC distinguishes method parameter parse and internal errors") {
    Dispatcher dispatcher;
    dispatcher.register_method("echo", [](const Json& params) {
        return params;
    });
    dispatcher.register_method("invalid", [](const Json&) -> Json {
        throw RpcError(-32602, "Invalid params", Json{{"field", "name"}});
    });
    dispatcher.register_method("broken", [](const Json&) -> Json {
        throw std::runtime_error("secret implementation detail");
    });
    check_error(
        dispatcher.dispatch(rpc_request("missing", Json::object(), "missing")), -32601, "missing");
    check_error(dispatcher.dispatch(rpc_request("Echo")), -32601, 1);
    for (const auto& params : std::vector<Json>{nullptr, true, 3, "bad"}) {
        check_error(dispatcher.dispatch(rpc_request("echo", params)), -32602, 1);
    }
    const auto invalid = dispatcher.dispatch(rpc_request("invalid"));
    check_error(invalid, -32602, 1);
    CHECK((*invalid)["error"]["data"] == Json{{"field", "name"}});
    const auto broken = dispatcher.dispatch(rpc_request("broken"));
    check_error(broken, -32603, 1);
    CHECK(broken->dump().find("secret") == std::string::npos);
    const std::vector<std::string> malformed{"",
                                             "{",
                                             "[1,",
                                             "{\"a\":}",
                                             "{} {}",
                                             "[NaN]",
                                             "[1e99999]",
                                             "{\"jsonrpc\":\"2.0\",\"method\":\"\xFF\"}"};
    for (const auto& text : malformed) {
        const auto response = dispatcher.dispatch_text(text);
        REQUIRE(response);
        check_error(Json::parse(*response), -32700);
    }
}

TEST_CASE("RPC notifications execute without replies even when handlers fail") {
    Dispatcher dispatcher;
    std::size_t calls = 0;
    dispatcher.register_method("increment", [&calls](const Json&) {
        return ++calls;
    });
    dispatcher.register_method("fail", [](const Json&) -> Json {
        throw RpcError(-32001, "Failure");
    });
    for (const auto* method : {"increment", "missing", "fail"}) {
        auto request = rpc_request(method);
        request.erase("id");
        CHECK_FALSE(dispatcher.dispatch(request));
        CHECK_FALSE(dispatcher.dispatch_text(request.dump()));
    }
    auto bad_params = rpc_request("increment", 7);
    bad_params.erase("id");
    CHECK_FALSE(dispatcher.dispatch(bad_params));
    CHECK(calls == 2);
    auto explicit_null = rpc_request("increment", Json::object(), nullptr);
    const auto response = dispatcher.dispatch(explicit_null);
    REQUIRE(response);
    CHECK((*response)["id"].is_null());
    CHECK(calls == 3);
}

TEST_CASE("RPC batch responses omit notifications and retain per-item errors") {
    Dispatcher dispatcher;
    std::size_t calls = 0;
    dispatcher.register_method("echo", [&calls](const Json& p) {
        ++calls;
        return p;
    });
    auto notification = rpc_request("echo", Json::array({7}));
    notification.erase("id");
    const Json batch = Json::array({rpc_request("echo", Json{{"a", 1}}, "first"),
                                    notification,
                                    17,
                                    rpc_request("missing", Json::object(), "last"),
                                    Json::array({rpc_request("echo")})});
    const auto response = dispatcher.dispatch(batch);
    REQUIRE(response);
    REQUIRE(response->is_array());
    REQUIRE(response->size() == 4);
    CHECK((*response)[0]["id"] == "first");
    check_error((*response)[1], -32600);
    check_error((*response)[2], -32601, "last");
    check_error((*response)[3], -32600);
    CHECK(calls == 2);
    CHECK_FALSE(dispatcher.dispatch(Json::array({notification, notification})));
    CHECK_FALSE(dispatcher.dispatch_text(Json::array({notification}).dump()));
    check_error(dispatcher.dispatch(Json::array()), -32600);
}

TEST_CASE("RPC batch size limit rejects the whole batch before execution") {
    Dispatcher dispatcher;
    std::size_t calls = 0;
    dispatcher.register_method("count", [&calls](const Json&) {
        return ++calls;
    });
    auto batch = Json::array();
    for (std::size_t index = 0; index < Dispatcher::max_batch_size; ++index) {
        batch.push_back(rpc_request("count", Json::object(), index));
    }
    const auto accepted = dispatcher.dispatch(batch);
    REQUIRE(accepted);
    CHECK(accepted->size() == Dispatcher::max_batch_size);
    CHECK(calls == Dispatcher::max_batch_size);
    batch.push_back(rpc_request("count"));
    check_error(dispatcher.dispatch(batch), -32600);
    const auto encoded = dispatcher.dispatch_text(batch.dump());
    REQUIRE(encoded);
    check_error(Json::parse(*encoded), -32600);
    CHECK(calls == Dispatcher::max_batch_size);
}

TEST_CASE("RPC nesting limit is identical for direct and parsed requests") {
    Dispatcher dispatcher;
    std::size_t calls = 0;
    dispatcher.register_method("count", [&calls](const Json&) {
        return ++calls;
    });
    auto accepted = rpc_request("count", nested_value(Dispatcher::max_depth - 1));
    REQUIRE(dispatcher.dispatch(accepted));
    REQUIRE(dispatcher.dispatch_text(accepted.dump()));
    CHECK(calls == 2);
    const auto too_deep = rpc_request("count", nested_value(Dispatcher::max_depth));
    check_error(dispatcher.dispatch(too_deep), -32600);
    const auto encoded = dispatcher.dispatch_text(too_deep.dump());
    REQUIRE(encoded);
    check_error(Json::parse(*encoded), -32600);
    CHECK(calls == 2);
    // Parsing must stop at the configured depth, not build a hostile deep DOM.
    const auto deep_text = std::string(2000, '[') + "0" + std::string(2000, ']');
    const auto deep_response = dispatcher.dispatch_text(deep_text);
    REQUIRE(deep_response);
    check_error(Json::parse(*deep_response), -32600);
}

TEST_CASE("RPC body limit uses UTF-8 bytes and permits the exact boundary") {
    Dispatcher dispatcher;
    std::size_t calls = 0;
    dispatcher.register_method("count", [&calls](const Json&) {
        return ++calls;
    });
    auto request = rpc_request("count", Json{{"padding", ""}});
    const auto overhead = request.dump().size();
    request["params"]["padding"] = std::string(Dispatcher::max_body_bytes - overhead, 'x');
    const auto body = request.dump();
    REQUIRE(body.size() == Dispatcher::max_body_bytes);
    const auto response = dispatcher.dispatch_text(body);
    REQUIRE(response);
    CHECK(Json::parse(*response)["result"] == 1);
    const auto oversized = dispatcher.dispatch_text(body + " ");
    REQUIRE(oversized);
    check_error(Json::parse(*oversized), -32600);
    request["params"]["padding"].get_ref<std::string&>().push_back('x');
    check_error(dispatcher.dispatch(request), -32600);
    CHECK(calls == 1);
}

TEST_CASE("RPC rejects non-JSON in-process values and isolates invalid handler results") {
    Dispatcher dispatcher;
    dispatcher.register_method("invalidUtf8", [](const Json&) {
        return Json("\xFF");
    });
    dispatcher.register_method("nan", [](const Json&) {
        return Json(std::numeric_limits<double>::quiet_NaN());
    });
    dispatcher.register_method("valid", [](const Json&) {
        return Json(7);
    });
    check_error(dispatcher.dispatch(
                    rpc_request("valid", Json::array({std::numeric_limits<double>::infinity()}))),
                -32600);
    check_error(dispatcher.dispatch(rpc_request("valid", Json::binary({1, 2, 3}))), -32600);
    check_error(dispatcher.dispatch(rpc_request("invalidUtf8")), -32603, 1);
    check_error(dispatcher.dispatch(rpc_request("nan")), -32603, 1);
    const auto batch = Json::array({rpc_request("invalidUtf8", Json::object(), "bad"),
                                    rpc_request("valid", Json::object(), "good")});
    const auto response = dispatcher.dispatch_text(batch.dump());
    REQUIRE(response);
    const auto parsed = Json::parse(*response);
    REQUIRE(parsed.is_array());
    check_error(parsed[0], -32603, "bad");
    CHECK(parsed[1]["result"] == 7);
}

TEST_CASE("RPC notifications have a canonical envelope") {
    const auto message = astra::protocol::make_notification("document.changed", Json{{"id", "1"}});
    CHECK(message["jsonrpc"] == "2.0");
    CHECK(message["method"] == "document.changed");
    CHECK(message["params"]["id"] == "1");
    CHECK_FALSE(message.contains("id"));
    CHECK_THROWS_AS((void)(astra::protocol::make_notification("", Json::object())),
                    std::invalid_argument);
    CHECK_THROWS_AS((void)(astra::protocol::make_notification("event", 1)), std::invalid_argument);
}

TEST_CASE("RPC string ID byte cap bounds worst-case escaped error envelopes") {
    Dispatcher dispatcher;
    std::size_t calls = 0;
    dispatcher.register_method("count", [&calls](const Json&) {
        return ++calls;
    });
    const std::string capped(Dispatcher::max_request_id_bytes, '\0');
    const auto response = dispatcher.dispatch(rpc_request("count", Json::object(), capped));
    REQUIRE(response);
    CHECK((*response)["id"] == capped);
    CHECK((*response)["result"] == 1);
    check_error(dispatcher.dispatch(rpc_request("count", Json::object(), capped + "x")), -32600);
    const auto wire =
        dispatcher.dispatch_text(rpc_request("count", Json::object(), capped + "x").dump());
    REQUIRE(wire);
    check_error(Json::parse(*wire), -32600);
    CHECK(wire->size() < 256);
    std::string unicode;
    for (std::size_t index = 0; index < Dispatcher::max_request_id_bytes / 4; ++index) {
        unicode += "\xF0\x9F\x98\x80";
    }
    const auto unicode_response =
        dispatcher.dispatch(rpc_request("count", Json::object(), unicode));
    REQUIRE(unicode_response);
    CHECK((*unicode_response)["id"] == unicode);
    check_error(dispatcher.dispatch(rpc_request("count", Json::object(), unicode + "x")), -32600);
    CHECK(calls == 2);
}

TEST_CASE(
    "RPC outbound exact body boundary is accepted and one extra byte becomes a resync error") {
    Dispatcher dispatcher;
    const auto overhead = Json{{"jsonrpc", "2.0"}, {"id", 1}, {"result", ""}}.dump().size();
    std::size_t length = Dispatcher::max_body_bytes - overhead;
    std::size_t calls = 0;
    dispatcher.register_method("large", [&](const Json&) {
        ++calls;
        return Json(std::string(length, 'x'));
    });
    const auto exact = dispatcher.dispatch_text(rpc_request("large").dump());
    REQUIRE(exact);
    CHECK(exact->size() == Dispatcher::max_body_bytes);
    CHECK_NOTHROW((void)(astra::protocol::encode_frame(*exact)));
    ++length;
    const auto oversized = dispatcher.dispatch_text(rpc_request("large").dump());
    REQUIRE(oversized);
    const auto parsed = Json::parse(*oversized);
    check_error(parsed, -32005, 1);
    CHECK(parsed["error"]["data"]["handlerCompleted"] == true);
    CHECK(parsed["error"]["data"]["resyncRequired"] == true);
    CHECK(parsed["error"]["data"]["maxBodyBytes"] == Dispatcher::max_body_bytes);
    CHECK(oversized->size() < 512);
    CHECK(calls == 2);
    check_error(dispatcher.dispatch(rpc_request("large")), -32005, 1);
    CHECK(calls == 3);
}

TEST_CASE("RPC oversized handler error data and messages are also bounded") {
    Dispatcher dispatcher;
    dispatcher.register_method("data", [](const Json&) -> Json {
        throw RpcError(-32006, "Failure", Json(std::string(Dispatcher::max_body_bytes, 'x')));
    });
    dispatcher.register_method("message", [](const Json&) -> Json {
        throw RpcError(-32006, std::string(Dispatcher::max_body_bytes, 'x'));
    });
    dispatcher.register_method("invalidMessage", [](const Json&) -> Json {
        throw RpcError(-32006, "\xFF");
    });
    for (const auto* method : {"data", "message"}) {
        const auto response = dispatcher.dispatch_text(rpc_request(method).dump());
        REQUIRE(response);
        const auto parsed = Json::parse(*response);
        check_error(parsed, -32005, 1);
        CHECK(parsed["error"]["data"]["handlerCompleted"] == true);
        CHECK(parsed["error"]["data"]["resyncRequired"] == true);
    }
    check_error(dispatcher.dispatch(rpc_request("invalidMessage")), -32603, 1);
}

TEST_CASE("RPC batch budget reserves every remaining response including maximally escaped IDs") {
    Dispatcher dispatcher;
    std::size_t calls = 0;
    dispatcher.register_method("large", [&](const Json&) {
        ++calls;
        return Json(std::string(128 * 1024, 'x'));
    });
    auto batch = Json::array();
    for (std::size_t index = 0; index < Dispatcher::max_batch_size; ++index) {
        const auto id = std::string(250, '\0') + std::to_string(index);
        batch.push_back(rpc_request("large", Json::object(), id));
    }
    const auto response = dispatcher.dispatch_text(batch.dump());
    REQUIRE(response);
    CHECK(response->size() <= Dispatcher::max_body_bytes);
    CHECK_NOTHROW((void)(astra::protocol::encode_frame(*response)));
    const auto parsed = Json::parse(*response);
    REQUIRE(parsed.size() == batch.size());
    std::size_t successes = 0;
    std::size_t resyncs = 0;
    for (std::size_t index = 0; index < parsed.size(); ++index) {
        CHECK(parsed[index]["id"] == batch[index]["id"]);
        if (parsed[index].contains("result")) {
            ++successes;
        } else {
            ++resyncs;
            check_error(parsed[index], -32005, batch[index]["id"]);
            CHECK(parsed[index]["error"]["data"]["resyncRequired"] == true);
        }
    }
    CHECK(successes > 0);
    CHECK(resyncs > 0);
    CHECK(calls == Dispatcher::max_batch_size);
    const auto direct = dispatcher.dispatch(batch);
    REQUIRE(direct);
    CHECK(*direct == parsed);
    CHECK(calls == 2 * Dispatcher::max_batch_size);
}

TEST_CASE("RPC oversized result preserves completed side effects and later batch execution") {
    Dispatcher dispatcher;
    std::size_t changes = 0;
    dispatcher.register_method("mutate", [&](const Json&) {
        ++changes;
        return Json(std::string(Dispatcher::max_body_bytes, 'x'));
    });
    dispatcher.register_method("snapshot", [&](const Json&) {
        return Json(changes);
    });
    auto notification = rpc_request("mutate");
    notification.erase("id");
    const auto batch = Json::array({rpc_request("mutate", Json::object(), "edit"),
                                    rpc_request("snapshot", Json::object(), "afterEdit"),
                                    notification,
                                    rpc_request("snapshot", Json::object(), "afterNotification")});
    const auto response = dispatcher.dispatch(batch);
    REQUIRE(response);
    REQUIRE(response->size() == 3);
    check_error((*response)[0], -32005, "edit");
    CHECK((*response)[0]["error"]["data"]["handlerCompleted"] == true);
    CHECK((*response)[1]["result"] == 1);
    CHECK((*response)[2]["result"] == 2);
    CHECK(changes == 2);
    CHECK_FALSE(dispatcher.dispatch(notification));
    CHECK_FALSE(dispatcher.dispatch_text(Json::array({notification, notification}).dump()));
    CHECK(changes == 5);
}
