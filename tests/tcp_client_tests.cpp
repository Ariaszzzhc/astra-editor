#include <astra/protocol/framing.hpp>
#include <astra/service/editor_service.hpp>
#include <astra/transport/tcp_client.hpp>

#include <doctest/doctest.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace {

using astra::protocol::Json;
using astra::transport::TcpClient;
using astra::transport::TcpClientOptions;
using astra::transport::TcpServer;
using astra::transport::TransportError;
using astra::transport::TransportCancelled;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

template<class T>
T ready(std::future<T> future) {
    if (future.wait_for(5s) != std::future_status::ready) {
        throw std::runtime_error("TCP client test future deadline expired");
    }
    return future.get();
}

template<class Predicate>
void until(Predicate predicate) {
    const auto deadline = Clock::now() + 5s;
    while (!predicate()) {
        if (Clock::now() >= deadline) {
            throw std::runtime_error("TCP client test condition deadline expired");
        }
        std::this_thread::sleep_for(1ms);
    }
}

Json request(Json id, std::string method = "echo", Json params = Json::object()) {
    return Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"method", std::move(method)},
                {"params", std::move(params)}};
}

Json response(Json id, Json result = true) {
    return Json{{"jsonrpc", "2.0"}, {"id", std::move(id)}, {"result", std::move(result)}};
}

Json reply(std::future<std::optional<std::string>> future) {
    const auto body = ready(std::move(future));
    if (!body) {
        throw std::runtime_error("TCP client test expected a response");
    }
    return Json::parse(*body);
}

// A real loopback socket peer allows exact byte fragmentation and adversarial
// protocol responses. Every operation has a deadline; no mock transport runs.
class RawPeer {
public:
    explicit RawPeer(bool listen_now = true) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("Initialize test sockets failed");
        }
        try {
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (listener_ == INVALID_SOCKET) {
                throw std::runtime_error("Create test listener failed");
            }
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), static_cast<int>(sizeof(address))) != 0) {
                throw std::runtime_error("Bind test listener failed");
            }
            int length = static_cast<int>(sizeof(address));
            if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
                throw std::runtime_error("Query test listener failed");
            }
            port_ = ntohs(address.sin_port);
            nonblocking(listener_);
            if (listen_now && listen(listener_, SOMAXCONN) != 0) {
                throw std::runtime_error("Listen on test socket failed");
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~RawPeer() { cleanup(); }
    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;

    std::uint16_t port() const noexcept { return port_; }

    void accept_connection() {
        wait(listener_, true, Clock::now() + 5s);
        socket_ = accept(listener_, nullptr, nullptr);
        if (socket_ == INVALID_SOCKET) {
            throw std::runtime_error("Accept test client failed");
        }
        nonblocking(socket_);
        const BOOL enabled = TRUE;
        if (setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&enabled),
                       static_cast<int>(sizeof(enabled))) != 0) {
            throw std::runtime_error("Configure test socket failed");
        }
    }

    void disconnect() noexcept {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    void send_bytes(std::string_view bytes, std::size_t fragment_size = 65536) const {
        const auto deadline = Clock::now() + 5s;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            wait(socket_, false, deadline);
            const auto count = std::min({bytes.size() - offset, fragment_size, std::size_t{65536}});
            const int sent = send(socket_, bytes.data() + offset, static_cast<int>(count), 0);
            if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                continue;
            }
            if (sent <= 0) {
                throw std::runtime_error("Send test response failed");
            }
            offset += static_cast<std::size_t>(sent);
        }
    }

    void send_json(const Json& value, std::size_t fragment_size = 65536) const {
        send_bytes(astra::protocol::encode_frame(value.dump()), fragment_size);
    }

    std::vector<Json> receive(std::size_t count = 1) {
        const auto deadline = Clock::now() + 5s;
        while (bodies_.size() < count) {
            wait(socket_, true, deadline);
            std::array<char, 65536> buffer{};
            const int length = recv(socket_, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (length == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
                continue;
            }
            if (length <= 0) {
                throw std::runtime_error("Receive test request failed");
            }
            for (auto& body : decoder_.feed(std::string_view(buffer.data(), static_cast<std::size_t>(length)))) {
                bodies_.push_back(std::move(body));
            }
        }
        std::vector<Json> result;
        while (result.size() < count) {
            result.push_back(Json::parse(bodies_.front()));
            bodies_.pop_front();
        }
        return result;
    }

private:
    static void nonblocking(SOCKET socket_value) {
        u_long enabled = 1;
        if (ioctlsocket(socket_value, FIONBIO, &enabled) != 0) {
            throw std::runtime_error("Configure nonblocking test socket failed");
        }
    }

    static void wait(SOCKET socket_value, bool read, Clock::time_point deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
        if (remaining <= 0us) {
            throw std::runtime_error("Test socket deadline expired");
        }
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
        timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
        fd_set selected{};
        FD_ZERO(&selected);
        FD_SET(socket_value, &selected);
        if (select(0, read ? &selected : nullptr, read ? nullptr : &selected, nullptr, &timeout) <= 0) {
            throw std::runtime_error("Test socket select failed or timed out");
        }
    }

    void cleanup() noexcept {
        disconnect();
        if (listener_ != INVALID_SOCKET) {
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        WSACleanup();
    }

    SOCKET listener_{INVALID_SOCKET};
    SOCKET socket_{INVALID_SOCKET};
    std::uint16_t port_{};
    astra::protocol::FrameDecoder decoder_;
    std::deque<std::string> bodies_;
};

} // namespace

TEST_CASE("TCP client validates resource limits and disconnected submissions") {
    TcpClientOptions options;
    options.request_timeout = 0ms;
    CHECK_THROWS_AS((void)TcpClient(options), std::invalid_argument);
    options = {};
    options.max_notification_bytes = 1;
    CHECK_THROWS_AS((void)TcpClient(options), std::invalid_argument);
    TcpClient client;
    CHECK_THROWS_AS((void)ready(client.submit(request(1).dump())), TransportError);
    CHECK_THROWS_AS(ready(client.connect("", 12)), TransportError);
    CHECK_THROWS_AS(ready(client.connect("127.0.0.1", 0)), TransportError);
    client.close();
    client.close();
    CHECK(client.stats().loop_close_error == 0);
    CHECK_FALSE(client.connected());
    CHECK_THROWS_AS(ready(client.connect("127.0.0.1", 123)), TransportError);
}

TEST_CASE("TCP client resolves localhost and IPv6 on real Windows loopback") {
    astra::protocol::Dispatcher dispatcher;
    dispatcher.register_method("echo", [](const Json& params) { return params; });
    TcpServer server([&](std::string_view body) { return dispatcher.dispatch_text(body); });
    std::string address = "127.0.0.1";
    std::string host = "localhost";
    SUBCASE("IPv4 listener with hostname candidates") {}
    SUBCASE("IPv6 literal") { address = host = "::1"; }
    server.start(address);
    TcpClient client;
    ready(client.connect(host, server.port()));
    REQUIRE(client.connected());
    CHECK(reply(client.submit(request("echo", "echo", {{"unicode", "\xF0\x9F\x98\x80"}}).dump()))["result"]["unicode"] ==
          "\xF0\x9F\x98\x80");
    CHECK_THROWS_AS(ready(client.connect(host, server.port())), TransportError);
    client.close();
    CHECK(client.stats().loop_close_error == 0);
}

TEST_CASE("TCP client rejects non-loopback resolved addresses and reports refused connections") {
    SUBCASE("Public numeric IPv4 is rejected before connecting") {
        TcpClient client;
        CHECK_THROWS_AS(ready(client.connect("203.0.113.1", 7777)), TransportError);
        CHECK_FALSE(client.connected());
    }
    SUBCASE("Public numeric IPv6 is rejected before connecting") {
        TcpClient client;
        CHECK_THROWS_AS(ready(client.connect("2001:db8::1", 7777)), TransportError);
    }
    SUBCASE("Reserved loopback port with no listener is refused") {
        RawPeer peer(false);
        TcpClient client;
        CHECK_THROWS_AS(ready(client.connect("127.0.0.1", peer.port())), TransportError);
        client.close();
        CHECK(client.stats().loop_close_error == 0);
    }
}

TEST_CASE("TCP client preserves editor service parity with in-process submission") {
    astra::service::EditorService service;
    TcpServer server([&](std::string_view body) { return service.dispatch_text(body); },
                     [&] {
                         std::vector<std::string> events;
                         for (const auto& event : service.take_notifications()) { events.push_back(event.dump()); }
                         return events;
                     });
    server.start();
    TcpClient client;
    ready(client.connect("localhost", server.port()));
    (void)reply(client.submit(request("barrier", "document.list").dump()));
    const auto created = reply(server.submit(request("create", "document.create", {{"text", "abc"}}).dump()));
    const auto id = created["result"]["id"];
    const auto get = request("get", "document.get", {{"id", id}}).dump();
    CHECK(reply(client.submit(get)) == reply(server.submit(get)));
    const auto edit = request("edit", "document.edit", {{"id", id}, {"baseRevision", "0"},
        {"edits", Json::array({Json{{"begin", 1}, {"end", 2}, {"text", "\xE4\xB8\xAD"}}})}}).dump();
    CHECK(reply(client.submit(edit))["result"]["revision"] == "1");
    CHECK(reply(client.submit(get)) == reply(server.submit(get)));
    CHECK(reply(client.submit(edit))["error"]["code"] == -32001);
    CHECK(reply(client.submit(get))["result"]["text"] == "a\xE4\xB8\xAD" "c");
    std::vector<std::string> notifications;
    until([&] {
        auto next = client.take_notifications();
        notifications.insert(notifications.end(), next.begin(), next.end());
        return notifications.size() >= 2;
    });
    CHECK(Json::parse(notifications[0])["method"] == "document.created");
    CHECK(Json::parse(notifications[1])["method"] == "document.changed");
}

TEST_CASE("TCP client rejects uncorrelatable input without sending or poisoning connection") {
    std::atomic<std::size_t> calls{};
    astra::protocol::Dispatcher dispatcher;
    dispatcher.register_method("echo", [](const Json& params) { return params; });
    TcpServer server([&](std::string_view body) { ++calls; return dispatcher.dispatch_text(body); });
    server.start();
    TcpClient client;
    ready(client.connect("127.0.0.1", server.port()));
    std::vector<std::string> invalid{"{", "[]", "null", "42", R"({"id":1,"method":"echo"})",
        R"({"jsonrpc":"1.0","method":"echo","id":1})", R"({"jsonrpc":"2.0","id":1})",
        R"({"jsonrpc":"2.0","method":12,"id":1})", R"({"jsonrpc":"2.0","method":"echo","id":true})"};
    invalid.push_back(request(std::string(257, 'x')).dump());
    invalid.push_back(Json::array({request(1), request(1)}).dump());
    auto too_many = Json::array();
    for (int index = 0; index < 129; ++index) { too_many.push_back(request(index)); }
    invalid.push_back(too_many.dump());
    for (const auto& body : invalid) {
        CHECK_THROWS_AS((void)ready(client.submit(body)), TransportError);
    }
    CHECK(reply(client.submit(request("valid").dump()))["id"] == "valid");
    CHECK(calls.load() == 1);
    CHECK(client.connected());
    CHECK(client.stats().pending_requests == 0);
}

TEST_CASE("TCP client invalid params and unknown methods retain server JSON-RPC errors") {
    astra::protocol::Dispatcher dispatcher;
    dispatcher.register_method("echo", [](const Json& params) { return params; });
    TcpServer server([&](std::string_view body) { return dispatcher.dispatch_text(body); });
    server.start();
    TcpClient client;
    ready(client.connect("127.0.0.1", server.port()));
    const auto body = request(nullptr, "echo", 12).dump();
    CHECK(reply(client.submit(body)) == Json::parse(*dispatcher.dispatch_text(body)));
    CHECK(reply(client.submit(request(7, "unknown").dump()))["error"]["code"] == -32601);
    CHECK(reply(client.submit(request(8, "echo", 12).dump()))["error"]["code"] == -32602);
}

TEST_CASE("TCP client correlates out-of-order replies and preserves distinct ID types") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    std::vector<Json> ids{nullptr, 1, "1", 1.5, -7, std::string(256, '\n')};
    std::vector<std::future<std::optional<std::string>>> futures;
    for (const auto& id : ids) { futures.push_back(client.submit(request(id).dump())); }
    CHECK(peer.receive(ids.size()).size() == ids.size());
    for (std::size_t index = ids.size(); index != 0; --index) {
        peer.send_json(response(ids[index - 1], index - 1));
    }
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const auto received = reply(std::move(futures[index]));
        CHECK(received["id"] == ids[index]);
        CHECK(received["result"] == index);
    }
    CHECK(client.stats().pending_requests == 0);
}

TEST_CASE("TCP client batches preserve request order with split replies and notifications") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    const Json notification{{"jsonrpc", "2.0"}, {"method", "event"}, {"params", Json::object()}};
    auto future = client.submit(Json::array({request("a"), notification, request("b"), request("c")}).dump());
    CHECK(peer.receive()[0].size() == 4);
    peer.send_json(Json::array({response("c", 3), response("a", 1)}));
    peer.send_json(notification, 1);
    peer.send_json(response("b", 2), 1);
    const auto received = reply(std::move(future));
    REQUIRE(received.size() == 3);
    CHECK(received[0]["id"] == "a");
    CHECK(received[1]["id"] == "b");
    CHECK(received[2]["id"] == "c");
    until([&] { return !client.take_notifications().empty(); });
}

TEST_CASE("TCP client decodes fragmented and coalesced frames") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    auto first = client.submit(request("first").dump());
    auto second = client.submit(request("second").dump());
    auto third = client.submit(request("third").dump());
    CHECK(peer.receive(3).size() == 3);
    peer.send_json(response("first", "\xF0\x9F\x98\x80"), 1);
    peer.send_bytes(astra::protocol::encode_frame(response("third", 3).dump()) +
                    astra::protocol::encode_frame(response("second", 2).dump()));
    CHECK(reply(std::move(first))["result"] == "\xF0\x9F\x98\x80");
    CHECK(reply(std::move(second))["result"] == 2);
    CHECK(reply(std::move(third))["result"] == 3);
}

TEST_CASE("TCP client rejects duplicate pending ID and allows reuse after completion") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    auto first = client.submit(request("same").dump());
    CHECK(peer.receive()[0]["id"] == "same");
    CHECK_THROWS_AS((void)ready(client.submit(request("same").dump())), TransportError);
    peer.send_json(response("same", 1));
    CHECK(reply(std::move(first))["result"] == 1);
    auto next = client.submit(request("same").dump());
    CHECK(peer.receive()[0]["id"] == "same");
    peer.send_json(response("same", 2));
    CHECK(reply(std::move(next))["result"] == 2);
}

TEST_CASE("TCP client notifications complete on write and receive server events") {
    std::vector<std::string> notifications;
    astra::protocol::Dispatcher dispatcher;
    dispatcher.register_method("event", [&](const Json& params) {
        notifications.push_back(astra::protocol::make_notification("seen", params).dump());
        return true;
    });
    TcpServer server([&](std::string_view body) { return dispatcher.dispatch_text(body); }, [&] {
        auto pending = std::move(notifications);
        notifications.clear();
        return pending;
    });
    server.start();
    TcpClient client;
    ready(client.connect("127.0.0.1", server.port()));
    const auto event = astra::protocol::make_notification("event", {{"value", 9}});
    CHECK_FALSE(ready(client.submit(event.dump())));
    CHECK_FALSE(ready(client.submit(Json::array({event, event}).dump())));
    std::vector<std::string> seen;
    until([&] {
        auto pending = client.take_notifications();
        seen.insert(seen.end(), pending.begin(), pending.end());
        return seen.size() == 3;
    });
    for (const auto& body : seen) { CHECK(Json::parse(body)["params"]["value"] == 9); }
}

TEST_CASE("TCP client request deadline closes connection and settles all pending futures") {
    TcpServer server([](std::string_view) { return std::optional<std::string>{}; });
    server.start();
    TcpClientOptions options;
    options.request_timeout = 50ms;
    TcpClient client(options);
    ready(client.connect("127.0.0.1", server.port()));
    auto first = client.submit(request(1).dump());
    auto second = client.submit(request(2).dump());
    CHECK_THROWS_AS((void)ready(std::move(first)), TransportError);
    CHECK_THROWS_AS((void)ready(std::move(second)), TransportError);
    CHECK_FALSE(client.connected());
    CHECK(client.stats().pending_requests == 0);
    client.close();
    CHECK(client.stats().loop_close_error == 0);
}

TEST_CASE("TCP client peer disconnect settles every pending request") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    auto first = client.submit(request(1).dump());
    auto second = client.submit(request(2).dump());
    CHECK(peer.receive(2).size() == 2);
    peer.disconnect();
    CHECK_THROWS_AS((void)ready(std::move(first)), TransportError);
    CHECK_THROWS_AS((void)ready(std::move(second)), TransportError);
    CHECK_FALSE(client.connected());
    CHECK(client.stats().pending_requests == 0);
}

TEST_CASE("TCP client close cancels queued requests and drains outstanding writes") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    std::vector<std::future<std::optional<std::string>>> pending;
    const auto large = std::string(1024 * 1024, 'x');
    for (int index = 0; index < 8; ++index) {
        pending.push_back(client.submit(request(index, "echo", {{"text", large}}).dump()));
    }
    client.close();
    for (auto& future : pending) {
        CHECK_THROWS_AS((void)ready(std::move(future)), TransportCancelled);
    }
    CHECK(client.stats().pending_requests == 0);
    CHECK(client.stats().queued_write_bytes == 0);
    CHECK(client.stats().loop_close_error == 0);
}

TEST_CASE("TCP client close during asynchronous connect always settles connection future") {
    RawPeer peer;
    for (int iteration = 0; iteration < 10; ++iteration) {
        TcpClient client;
        auto connecting = client.connect("localhost", peer.port());
        client.close();
        REQUIRE(connecting.wait_for(0ms) == std::future_status::ready);
        try { connecting.get(); }
        catch (const TransportCancelled&) { /* Expected if connect did not finish first. */ }
        CHECK(client.stats().loop_close_error == 0);
    }
}

TEST_CASE("TCP client enforces pending-count body and write-byte bounds") {
    TcpServer server([](std::string_view) { return std::optional<std::string>{}; });
    server.start();
    TcpClientOptions options;
    options.max_pending_requests = 1;
    options.max_queued_write_bytes = 512;
    options.max_body_bytes = 1024;
    TcpClient client(options);
    ready(client.connect("127.0.0.1", server.port()));
    CHECK_THROWS_AS((void)ready(client.submit(request(1, "echo", {{"text", std::string(1024, 'a')}}).dump())), TransportError);
    CHECK_THROWS_AS((void)ready(client.submit(request(1, "echo", {{"text", std::string(512, 'a')}}).dump())), TransportError);
    auto first = client.submit(request(1).dump());
    CHECK_THROWS_AS((void)ready(client.submit(request(2).dump())), TransportError);
    CHECK(client.stats().pending_requests == 1);
    CHECK(client.stats().queued_write_bytes <= 512);
    client.close();
    CHECK_THROWS_AS((void)ready(std::move(first)), TransportCancelled);
}

TEST_CASE("TCP client concurrent submissions retain each caller response") {
    astra::protocol::Dispatcher dispatcher;
    dispatcher.register_method("echo", [](const Json& params) { return params; });
    TcpServer server([&](std::string_view body) { return dispatcher.dispatch_text(body); });
    server.start();
    TcpClient client;
    ready(client.connect("127.0.0.1", server.port()));
    std::vector<std::future<std::optional<std::string>>> pending(64);
    std::vector<std::jthread> threads;
    for (std::size_t thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&, thread] {
            for (std::size_t index = thread; index < pending.size(); index += 4) {
                pending[index] = client.submit(request(index, "echo", {{"value", index}}).dump());
            }
        });
    }
    threads.clear();
    for (std::size_t index = 0; index < pending.size(); ++index) {
        const auto result = reply(std::move(pending[index]));
        CHECK(result["id"] == index);
        CHECK(result["result"]["value"] == index);
    }
}

TEST_CASE("TCP client malformed frames fail pending calls without hanging") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    auto pending = client.submit(request(1).dump());
    CHECK(peer.receive().size() == 1);
    SUBCASE("Bad header") { peer.send_bytes("Content-Length: -1\r\n\r\n"); }
    SUBCASE("Oversized body") { peer.send_bytes("Content-Length: 8388609\r\n\r\n"); }
    SUBCASE("Truncated body") { peer.send_bytes("Content-Length: 80\r\n\r\n{"); peer.disconnect(); }
    CHECK_THROWS((void)ready(std::move(pending)));
    CHECK_FALSE(client.connected());
    CHECK(client.stats().protocol_errors == 1);
    client.close();
    CHECK(client.stats().loop_close_error == 0);
}

TEST_CASE("TCP client invalid server envelopes fail pending calls") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    auto pending = client.submit(request(1).dump());
    CHECK(peer.receive().size() == 1);
    SUBCASE("Invalid JSON") { peer.send_bytes(astra::protocol::encode_frame("{")); }
    SUBCASE("Unknown ID") { peer.send_json(response(77)); }
    SUBCASE("Wrong version") { peer.send_json(Json{{"jsonrpc", "1.0"}, {"id", 1}, {"result", true}}); }
    SUBCASE("Result and error") {
        auto bad = response(1); bad["error"] = Json{{"code", -1}, {"message", "bad"}}; peer.send_json(bad);
    }
    SUBCASE("Malformed error") { peer.send_json(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"error", "bad"}}); }
    SUBCASE("Empty batch") { peer.send_json(Json::array()); }
    SUBCASE("Server request is unsupported") { peer.send_json(request("server-request")); }
    CHECK_THROWS_AS((void)ready(std::move(pending)), TransportError);
    CHECK_FALSE(client.connected());
    CHECK(client.stats().protocol_errors == 1);
}

TEST_CASE("TCP client bounds aggregated batch replies across multiple frames") {
    RawPeer peer;
    TcpClientOptions options;
    options.max_body_bytes = 256;
    TcpClient client(options);
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    auto pending = client.submit(Json::array({request(1), request(2)}).dump());
    CHECK(peer.receive().size() == 1);
    peer.send_json(response(1, std::string(120, 'a')));
    peer.send_json(response(2, std::string(120, 'a')));
    CHECK_THROWS_AS((void)ready(std::move(pending)), TransportError);
    CHECK(client.stats().pending_requests == 0);
}

TEST_CASE("TCP client duplicate response ID cannot complete the wrong batch member") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    auto pending = client.submit(Json::array({request(1), request(2)}).dump());
    CHECK(peer.receive().size() == 1);
    peer.send_json(Json::array({response(1), response(1)}));
    CHECK_THROWS_AS((void)ready(std::move(pending)), TransportError);
    CHECK(client.stats().protocol_errors == 1);
}

TEST_CASE("TCP client bounds partial responses across outstanding batches") {
    RawPeer peer;
    TcpClientOptions options;
    options.max_buffered_response_bytes = 256;
    TcpClient client(options);
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    auto first = client.submit(Json::array({request(1), request(2)}).dump());
    auto second = client.submit(Json::array({request(3), request(4)}).dump());
    CHECK(peer.receive(2).size() == 2);
    peer.send_json(response(1, std::string(120, 'a')));
    peer.send_json(response(3, std::string(120, 'a')));
    CHECK_THROWS_AS((void)ready(std::move(first)), TransportError);
    CHECK_THROWS_AS((void)ready(std::move(second)), TransportError);
}

TEST_CASE("TCP client notification overflow emits bounded resync marker and recovers") {
    RawPeer peer;
    TcpClientOptions options;
    options.max_notifications = 2;
    options.max_notification_bytes = 256;
    TcpClient client(options);
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    const auto event = astra::protocol::make_notification("changed", {{"revision", "1"}});
    for (int index = 0; index < 10; ++index) { peer.send_json(event); }
    auto barrier = client.submit(request("barrier").dump());
    CHECK(peer.receive().size() == 1);
    peer.send_json(response("barrier"));
    (void)reply(std::move(barrier));
    const auto notifications = client.take_notifications();
    REQUIRE(notifications.size() == 1);
    CHECK(Json::parse(notifications[0])["method"] == "transport.resyncRequired");
    CHECK(client.stats().notification_overflows == 1);
    peer.send_json(event);
    std::vector<std::string> next;
    until([&] { next = client.take_notifications(); return !next.empty(); });
    REQUIRE(next.size() == 1);
    CHECK(Json::parse(next[0])["method"] == "changed");
}

TEST_CASE("TCP client rejects deeply nested input and server responses") {
    RawPeer peer;
    TcpClient client;
    ready(client.connect("127.0.0.1", peer.port()));
    peer.accept_connection();
    Json nested = true;
    for (int depth = 0; depth < 70; ++depth) { nested = Json::array({std::move(nested)}); }
    CHECK_THROWS_AS((void)ready(client.submit(request(1, "echo", nested).dump())), TransportError);
    auto pending = client.submit(request(2).dump());
    CHECK(peer.receive()[0]["id"] == 2);
    peer.send_json(response(2, nested));
    CHECK_THROWS_AS((void)ready(std::move(pending)), TransportError);
    CHECK(client.stats().protocol_errors == 1);
}

#endif
