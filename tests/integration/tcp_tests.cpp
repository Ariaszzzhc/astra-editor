#include <astra/protocol/framing.hpp>
#include <astra/service/editor_service.hpp>
#include <astra/transport/tcp_server.hpp>

#include <doctest/doctest.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace {

using astra::protocol::Json;
using astra::transport::TcpServer;
using astra::transport::TcpServerOptions;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class SocketClient {
  public:
    explicit SocketClient(std::uint16_t port, bool ipv6 = false) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        initialized_ = true;
        try {
            socket_ = socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket_ == INVALID_SOCKET) {
                throw std::runtime_error("Create test TCP socket failed");
            }
            u_long nonblocking = 1;
            if (ioctlsocket(socket_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
                throw std::runtime_error("Configure test TCP socket failed");
            }
            int status = SOCKET_ERROR;
            if (ipv6) {
                sockaddr_in6 address{};
                address.sin6_family = AF_INET6;
                address.sin6_port = htons(port);
                if (InetPtonA(AF_INET6, "::1", &address.sin6_addr) != 1) {
                    throw std::runtime_error("Parse test IPv6 address failed");
                }
                status = connect(socket_,
                                 reinterpret_cast<const sockaddr*>(&address),
                                 static_cast<int>(sizeof(address)));
            } else {
                sockaddr_in address{};
                address.sin_family = AF_INET;
                address.sin_port = htons(port);
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                status = connect(socket_,
                                 reinterpret_cast<const sockaddr*>(&address),
                                 static_cast<int>(sizeof(address)));
            }
            if (status == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
                throw std::runtime_error("Connect test TCP socket failed");
            }
            if (!wait_ready(false, Clock::now() + 5s)) {
                throw std::runtime_error("Test TCP connection deadline expired");
            }
            int error = 0;
            int length = static_cast<int>(sizeof(error));
            if (getsockopt(
                    socket_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) != 0 ||
                error != 0) {
                throw std::runtime_error("Test TCP connection was refused");
            }
        } catch (...) {
            close();
            if (initialized_) {
                WSACleanup();
                initialized_ = false;
            }
            throw;
        }
    }

    ~SocketClient() {
        close();
        if (initialized_) {
            WSACleanup();
        }
    }

    SocketClient(const SocketClient&) = delete;
    SocketClient& operator=(const SocketClient&) = delete;

    void close() noexcept {
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    void finish_sending() {
        if (shutdown(socket_, SD_SEND) == SOCKET_ERROR) {
            throw std::runtime_error("Test TCP half-close failed");
        }
    }

    void send_bytes(std::string_view bytes, std::size_t fragment_size = 65536) {
        const auto deadline = Clock::now() + 5s;
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            if (!wait_ready(false, deadline)) {
                throw std::runtime_error("Test TCP send deadline expired");
            }
            const auto length =
                std::min({fragment_size, bytes.size() - offset, static_cast<std::size_t>(65536)});
            const int sent = send(socket_, bytes.data() + offset, static_cast<int>(length), 0);
            if (sent == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    continue;
                }
                throw std::runtime_error("Test TCP send failed");
            }
            if (sent == 0) {
                throw std::runtime_error("Test TCP send made no progress");
            }
            offset += static_cast<std::size_t>(sent);
        }
    }

    void send_body(std::string_view body, std::size_t fragment_size = 65536) {
        send_bytes(astra::protocol::encode_frame(body), fragment_size);
    }

    std::vector<std::string> receive(std::size_t count) {
        const auto deadline = Clock::now() + 5s;
        while (messages_.size() < count) {
            if (!wait_ready(true, deadline)) {
                throw std::runtime_error("Test TCP receive deadline expired");
            }
            std::array<char, 65536> buffer{};
            const int received = recv(socket_, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (received == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) {
                    continue;
                }
                throw std::runtime_error("Test TCP receive failed");
            }
            if (received == 0) {
                throw std::runtime_error("Test TCP peer closed before all frames arrived");
            }
            auto decoded =
                decoder_.feed(std::string_view(buffer.data(), static_cast<std::size_t>(received)));
            for (auto& body : decoded) {
                messages_.push_back(std::move(body));
            }
        }
        std::vector<std::string> result;
        while (result.size() < count) {
            result.push_back(std::move(messages_.front()));
            messages_.pop_front();
        }
        return result;
    }

    bool readable(std::chrono::milliseconds duration = 100ms) const {
        return !messages_.empty() || wait_ready(true, Clock::now() + duration);
    }

    bool await_closed() const {
        const auto deadline = Clock::now() + 5s;
        std::array<char, 65536> buffer{};
        while (wait_ready(true, deadline)) {
            const int count = recv(socket_, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (count == 0) {
                return true;
            }
            if (count == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error == WSAECONNRESET || error == WSAECONNABORTED || error == WSAENOTCONN) {
                    return true;
                }
                if (error != WSAEWOULDBLOCK) {
                    throw std::runtime_error("Unexpected test TCP close error");
                }
            }
        }
        return false;
    }

  private:
    bool wait_ready(bool reading, Clock::time_point deadline) const {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now());
        if (remaining <= 0us) {
            return false;
        }
        timeval timeout{};
        timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
        timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
        fd_set ready{};
        FD_ZERO(&ready);
        FD_SET(socket_, &ready);
        const int status =
            select(0, reading ? &ready : nullptr, reading ? nullptr : &ready, nullptr, &timeout);
        if (status == SOCKET_ERROR) {
            throw std::runtime_error("Test TCP select failed");
        }
        return status > 0;
    }

    SOCKET socket_{INVALID_SOCKET};
    bool initialized_{};
    astra::protocol::FrameDecoder decoder_;
    std::deque<std::string> messages_;
};

Json tcp_request(std::string method, Json params, std::string id) {
    return Json{{"jsonrpc", "2.0"},
                {"method", std::move(method)},
                {"params", std::move(params)},
                {"id", std::move(id)}};
}

TcpServer::Handler echo_handler() {
    return [](std::string_view body) {
        return std::optional<std::string>(std::string(body));
    };
}

template <typename Predicate> bool wait_until(Predicate predicate) {
    const auto deadline = Clock::now() + 5s;
    while (Clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

void check_shutdown(const TcpServer& server) {
    CHECK_FALSE(server.running());
    CHECK(server.port() == 0);
    const auto stats = server.stats();
    CHECK(stats.active_connections == 0);
    CHECK(stats.pending_write_bytes == 0);
    CHECK(stats.pending_write_requests == 0);
    CHECK(stats.loop_close_error == 0);
    CHECK(stats.pending_tasks == 0);
    CHECK(stats.pending_work == 0);
}

template <typename Value> Value await_future(std::future<Value> future) {
    if (future.wait_for(5s) != std::future_status::ready) {
        throw std::runtime_error("Local transport future deadline expired");
    }
    return future.get();
}

void await_release(std::shared_future<void> future) {
    if (future.wait_for(5s) != std::future_status::ready) {
        throw std::runtime_error("Test release deadline expired");
    }
}

} // namespace

TEST_CASE("TCP server uses real sockets for fragmented and coalesced Content-Length frames") {
    TcpServer server(echo_handler());
    server.start();
    REQUIRE(server.running());
    REQUIRE(server.port() != 0);
    SocketClient client(server.port());
    const std::string unicode = "{\"text\":\"\xE4\xB8\xAD\xF0\x9F\x98\x80\"}";
    client.send_body(unicode, 1);
    client.send_bytes(astra::protocol::encode_frame("second") +
                      astra::protocol::encode_frame("third"));
    CHECK(client.receive(3) == std::vector<std::string>{unicode, "second", "third"});
    server.stop();
    REQUIRE(client.await_closed());
    check_shutdown(server);
    CHECK(server.stats().accepted_connections == 1);
}

TEST_CASE(
    "TCP service replies to the origin and broadcasts metadata notifications to both clients") {
    astra::service::EditorService service;
    std::thread::id handler_thread;
    std::thread::id notification_thread;
    TcpServer server(
        [&](std::string_view body) {
            handler_thread = std::this_thread::get_id();
            return service.dispatch_text(body);
        },
        [&] {
            notification_thread = std::this_thread::get_id();
            std::vector<std::string> messages;
            for (const auto& event : service.take_notifications()) {
                messages.push_back(event.dump());
            }
            return messages;
        });
    server.start();
    SocketClient first(server.port());
    SocketClient second(server.port());
    second.send_body(tcp_request("document.get", Json{{"id", "99"}}, "ready").dump());
    CHECK(Json::parse(second.receive(1).front())["error"]["code"] == -32002);
    first.send_body(tcp_request("document.create", Json{{"text", "hello"}}, "create").dump(), 3);
    const auto created = first.receive(2);
    CHECK(Json::parse(created[0])["id"] == "create");
    CHECK(Json::parse(created[0])["result"]["id"] == "1");
    CHECK(Json::parse(created[1])["method"] == "document.created");
    CHECK(Json::parse(second.receive(1).front()) == Json::parse(created[1]));
    second.send_body(tcp_request("document.get", Json{{"id", "1"}}, "read").dump());
    const auto read = Json::parse(second.receive(1).front());
    CHECK(read["id"] == "read");
    CHECK(read["result"]["text"] == "hello");
    CHECK_FALSE(first.readable());
    first.send_body(tcp_request("document.insert",
                                Json{{"id", "1"}, {"baseRevision", "0"}, {"text", "!"}},
                                "edit")
                        .dump());
    const auto edited = first.receive(2);
    CHECK(Json::parse(edited[0])["id"] == "edit");
    CHECK(Json::parse(edited[1])["params"]["revision"] == "1");
    CHECK(Json::parse(second.receive(1).front()) == Json::parse(edited[1]));
    server.stop();
    CHECK(handler_thread == notification_thread);
    CHECK(handler_thread != std::this_thread::get_id());
    check_shutdown(server);
}

TEST_CASE("TCP malformed framing disconnects only the offending peer") {
    TcpServer server(echo_handler());
    server.start();
    SocketClient malformed(server.port());
    SocketClient healthy(server.port());
    malformed.send_bytes("Content-Length: -1\r\n\r\n");
    REQUIRE(malformed.await_closed());
    healthy.send_body("still alive");
    CHECK(healthy.receive(1).front() == "still alive");
    CHECK(server.stats().protocol_errors == 1);
    server.stop();
    check_shutdown(server);
}

TEST_CASE("TCP bounded request body and truncated EOF are rejected") {
    TcpServerOptions options;
    options.max_body_bytes = 64;
    TcpServer server(echo_handler(), {}, options);
    server.start();
    SocketClient oversized(server.port());
    oversized.send_bytes("Content-Length: 65\r\n\r\n");
    REQUIRE(oversized.await_closed());
    SocketClient truncated(server.port());
    truncated.send_bytes("Content-Length: 12\r\n\r\nshort");
    truncated.finish_sending();
    REQUIRE(truncated.await_closed());
    CHECK(server.stats().protocol_errors == 2);
    SocketClient valid(server.port());
    valid.send_body(std::string(64, 'x'));
    CHECK(valid.receive(1).front() == std::string(64, 'x'));
    server.stop();
    check_shutdown(server);
}

TEST_CASE("TCP connection limit rejects excess clients while preserving existing sessions") {
    TcpServerOptions options;
    options.max_connections = 1;
    TcpServer server(echo_handler(), {}, options);
    server.start();
    SocketClient first(server.port());
    first.send_body("ready");
    CHECK(first.receive(1).front() == "ready");
    SocketClient second(server.port());
    REQUIRE(second.await_closed());
    CHECK(server.stats().rejected_connections == 1);
    first.send_body("kept");
    CHECK(first.receive(1).front() == "kept");
    server.stop();
    check_shutdown(server);
}

TEST_CASE(
    "TCP per-client write budget disconnects an oversized response without stopping the server") {
    TcpServerOptions options;
    options.max_queued_write_bytes = 128;
    TcpServer server(
        [](std::string_view body) {
            return std::optional<std::string>(body == "large" ? std::string(256, 'x') : "small");
        },
        {},
        options);
    server.start();
    SocketClient slow(server.port());
    slow.send_body("large");
    REQUIRE(slow.await_closed());
    CHECK(server.stats().write_limit_disconnects == 1);
    CHECK(server.stats().pending_write_bytes == 0);
    SocketClient healthy(server.port());
    healthy.send_body("small");
    CHECK(healthy.receive(1).front() == "small");
    server.stop();
    check_shutdown(server);
}

TEST_CASE("TCP global write budget is checked before queuing a response") {
    TcpServerOptions options;
    options.max_total_queued_write_bytes = 128;
    TcpServer server(
        [](std::string_view) {
            return std::optional<std::string>(std::string(256, 'x'));
        },
        {},
        options);
    server.start();
    SocketClient client(server.port());
    client.send_body("request");
    REQUIRE(client.await_closed());
    CHECK(server.stats().write_limit_disconnects == 1);
    CHECK(server.stats().pending_write_requests == 0);
    server.stop();
    check_shutdown(server);
}

TEST_CASE("TCP half-close finishes complete responses before closing the connection") {
    const std::string response(2 * 1024 * 1024, 'r');
    TcpServer server([&](std::string_view) {
        return std::optional<std::string>(response);
    });
    server.start();
    SocketClient client(server.port());
    client.send_body("request");
    client.finish_sending();
    CHECK(client.receive(1).front() == response);
    REQUIRE(client.await_closed());
    server.stop();
    check_shutdown(server);
}

TEST_CASE(
    "TCP disconnect and shutdown drain outstanding asynchronous writes without stale handles") {
    std::atomic<std::size_t> calls{};
    const std::string response(4 * 1024 * 1024, 'r');
    TcpServer server([&](std::string_view) {
        ++calls;
        return std::optional<std::string>(response);
    });
    server.start();
    for (std::size_t index = 0; index < 8; ++index) {
        SocketClient disconnected(server.port());
        disconnected.send_body("large");
        REQUIRE(wait_until([&] {
            return calls.load() >= index + 1;
        }));
        disconnected.close();
    }
    SocketClient active(server.port());
    active.send_bytes(astra::protocol::encode_frame("a") + astra::protocol::encode_frame("b"));
    REQUIRE(wait_until([&] {
        return calls.load() >= 10;
    }));
    const auto start = Clock::now();
    server.stop();
    CHECK(Clock::now() - start < 5s);
    REQUIRE(active.await_closed());
    check_shutdown(server);
    server.stop();
    check_shutdown(server);
}

TEST_CASE("TCP handler failure is isolated to its connection") {
    TcpServer server([](std::string_view body) -> std::optional<std::string> {
        if (body == "fail") {
            throw std::runtime_error("handler failure");
        }
        return std::string(body);
    });
    server.start();
    SocketClient failed(server.port());
    failed.send_body("fail");
    REQUIRE(failed.await_closed());
    SocketClient healthy(server.port());
    healthy.send_body("healthy");
    CHECK(healthy.receive(1).front() == "healthy");
    CHECK(server.stats().handler_errors == 1);
    server.stop();
    check_shutdown(server);
}

TEST_CASE("TCP lifecycle rejects public binds and supports clean restart") {
    TcpServer server(echo_handler());
    for (const auto* address : {"0.0.0.0", "::", "192.168.1.10", "example.com", "localhost"}) {
        CHECK_THROWS_AS((void)(server.start(address)), std::invalid_argument);
        CHECK_FALSE(server.running());
    }
    server.start();
    CHECK_THROWS_AS((void)(server.start()), std::logic_error);
    {
        SocketClient first(server.port());
        first.send_body("first");
        CHECK(first.receive(1).front() == "first");
    }
    server.stop();
    check_shutdown(server);
    server.start();
    SocketClient second(server.port());
    second.send_body("second");
    CHECK(second.receive(1).front() == "second");
    server.stop();
    check_shutdown(server);
}

TEST_CASE("TCP RAII destructor closes active clients") {
    std::unique_ptr<SocketClient> client;
    {
        TcpServer server(echo_handler());
        server.start();
        client = std::make_unique<SocketClient>(server.port());
        client->send_body("ready");
        CHECK(client->receive(1).front() == "ready");
    }
    REQUIRE(client->await_closed());
}

TEST_CASE("TCP failed bind cleans up its initialized loop and can be retried") {
    TcpServer occupied(echo_handler());
    occupied.start();
    TcpServer retry(echo_handler());
    CHECK_THROWS_AS((void)(retry.start("127.0.0.1", occupied.port())),
                    astra::transport::TransportError);
    CHECK_FALSE(retry.running());
    CHECK(retry.port() == 0);
    CHECK(retry.stats().loop_close_error == 0);
    retry.start();
    SocketClient client(retry.port());
    client.send_body("recovered");
    CHECK(client.receive(1).front() == "recovered");
    retry.stop();
    occupied.stop();
    check_shutdown(retry);
    check_shutdown(occupied);
}

TEST_CASE("TCP supports IPv6 loopback on Windows") {
    TcpServer server(echo_handler());
    server.start("::1");
    SocketClient client(server.port(), true);
    client.send_body("ipv6");
    CHECK(client.receive(1).front() == "ipv6");
    server.stop();
    check_shutdown(server);
}

TEST_CASE("TCP rejects invalid resource configuration before creating a loop") {
    CHECK_THROWS_AS((void)(TcpServer(TcpServer::Handler{})), std::invalid_argument);
    TcpServerOptions options;
    options.max_connections = 0;
    CHECK_THROWS_AS((void)(TcpServer(echo_handler(), {}, options)), std::invalid_argument);
    options = {};
    options.max_queued_write_bytes = 0;
    CHECK_THROWS_AS((void)(TcpServer(echo_handler(), {}, options)), std::invalid_argument);
    options = {};
    options.max_total_queued_write_bytes = 0;
    CHECK_THROWS_AS((void)(TcpServer(echo_handler(), {}, options)), std::invalid_argument);
    options = {};
    options.max_body_bytes = 0;
    CHECK_THROWS_AS((void)(TcpServer(echo_handler(), {}, options)), std::invalid_argument);
    options = {};
    options.max_header_bytes = 3;
    CHECK_THROWS_AS((void)(TcpServer(echo_handler(), {}, options)), std::invalid_argument);
}

TEST_CASE("local submit and TCP share the same handler and service notifications") {
    astra::service::EditorService service;
    TcpServer server(
        [&](std::string_view body) {
            return service.dispatch_text(body);
        },
        [&] {
            std::vector<std::string> result;
            for (const auto& notification : service.take_notifications()) {
                result.push_back(notification.dump());
            }
            return result;
        });
    server.start();
    SocketClient remote(server.port());
    remote.send_body(tcp_request("document.get", Json{{"id", "missing"}}, "ready").dump());
    (void)remote.receive(1);
    const auto create = tcp_request("document.create", Json{{"text", "hello"}}, "create").dump();
    const auto created = await_future(server.submit(create));
    REQUIRE(created);
    CHECK(Json::parse(*created)["result"]["id"] == "1");
    const auto local_events = server.take_local_notifications();
    REQUIRE(local_events.size() == 1);
    CHECK(remote.receive(1).front() == local_events.front());
    const auto read = tcp_request("document.get", Json{{"id", "1"}}, "read").dump();
    remote.send_body(read);
    const auto remote_result = remote.receive(1).front();
    CHECK(await_future(server.submit(read)) == std::optional<std::string>(remote_result));
    CHECK(server.take_local_notifications().empty());
    server.stop();
    check_shutdown(server);
}

TEST_CASE("concurrent local producers preserve FIFO order within each producer") {
    std::size_t serial = 0;
    TcpServer server([&](std::string_view) {
        return std::optional<std::string>(std::to_string(++serial));
    });
    server.start();
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t per_producer = 32;
    std::array<std::vector<std::future<std::optional<std::string>>>, producer_count> futures;
    std::vector<std::jthread> producers;
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
        producers.emplace_back([&, producer] {
            for (std::size_t index = 0; index < per_producer; ++index) {
                futures[producer].push_back(server.submit(std::to_string(producer)));
            }
        });
    }
    producers.clear();
    std::vector<std::size_t> all;
    for (auto& requests : futures) {
        std::size_t previous = 0;
        for (auto& future : requests) {
            const auto response = await_future(std::move(future));
            REQUIRE(response);
            const auto value = static_cast<std::size_t>(std::stoull(*response));
            CHECK(value > previous);
            previous = value;
            all.push_back(value);
        }
    }
    std::ranges::sort(all);
    REQUIRE(all.size() == producer_count * per_producer);
    for (std::size_t index = 0; index < all.size(); ++index) {
        CHECK(all[index] == index + 1);
    }
    server.stop();
    check_shutdown(server);
}

TEST_CASE("local stop cancels queued futures and waits for the current loop task") {
    TcpServer server(echo_handler());
    server.start();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    const auto release_future = release.get_future().share();
    auto blocking = server.post([&, release_future] {
        entered.set_value();
        await_release(release_future);
    });
    await_future(std::move(entered_future));
    std::vector<std::future<std::optional<std::string>>> pending;
    for (std::size_t index = 0; index < 16; ++index) {
        pending.push_back(server.submit("never executed"));
    }
    std::jthread stopping([&] {
        server.stop();
    });
    const bool stop_requested = wait_until([&] {
        return !server.running();
    });
    release.set_value();
    stopping.join();
    REQUIRE(stop_requested);
    await_future(std::move(blocking));
    for (auto& future : pending) {
        CHECK_THROWS_AS((void)(await_future(std::move(future))),
                        astra::transport::TransportCancelled);
    }
    CHECK_THROWS_AS((void)(await_future(server.submit("stopped"))),
                    astra::transport::TransportCancelled);
    check_shutdown(server);
}

TEST_CASE("local task count and byte budgets fail through ready futures") {
    TcpServerOptions options;
    options.max_pending_tasks = 2;
    options.max_queued_task_bytes = 5;
    TcpServer server(echo_handler(), {}, options);
    CHECK_THROWS_AS((void)(await_future(server.submit("before start"))),
                    astra::transport::TransportCancelled);
    server.start();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    const auto release_future = release.get_future().share();
    auto blocking = server.post([&, release_future] {
        entered.set_value();
        await_release(release_future);
    });
    await_future(std::move(entered_future));
    auto first = server.submit("abc");
    auto byte_overflow = server.submit("def");
    auto second = server.post([] {
    });
    auto count_overflow = server.post([] {
    });
    release.set_value();
    await_future(std::move(blocking));
    CHECK(await_future(std::move(first)) == std::optional<std::string>("abc"));
    await_future(std::move(second));
    CHECK_THROWS_AS((void)(await_future(std::move(byte_overflow))),
                    astra::transport::TransportError);
    CHECK_THROWS_AS((void)(await_future(std::move(count_overflow))),
                    astra::transport::TransportError);
    server.stop();
    check_shutdown(server);
}

TEST_CASE("local handler and posted task exceptions propagate without stopping the loop") {
    TcpServer server([](std::string_view body) -> std::optional<std::string> {
        if (body == "fail") {
            throw std::logic_error("handler failed");
        }
        return std::string(body);
    });
    server.start();
    CHECK_THROWS_AS((void)(await_future(server.submit("fail"))), std::logic_error);
    CHECK_THROWS_AS((void)(await_future(server.post([] {
                        throw std::out_of_range("task failed");
                    }))),
                    std::out_of_range);
    CHECK_THROWS_AS((void)(await_future(server.post({}))), std::invalid_argument);
    CHECK(await_future(server.submit("healthy")) == std::optional<std::string>("healthy"));
    server.stop();
    check_shutdown(server);
}

TEST_CASE(
    "local notification overflow emits one explicit resync marker and leaves TCP delivery intact") {
    std::size_t serial = 0;
    std::vector<std::string> pending;
    TcpServerOptions options;
    options.max_local_notifications = 2;
    options.max_local_notification_bytes = 256;
    TcpServer server(
        [&](std::string_view body) -> std::optional<std::string> {
            if (body == "event") {
                pending.push_back(Json{
                    {"jsonrpc", "2.0"}, {"method", "event"}, {"params", Json{{"serial", ++serial}}}}
                                      .dump());
            }
            return std::string(body);
        },
        [&] {
            return std::exchange(pending, {});
        },
        options);
    server.start();
    SocketClient remote(server.port());
    remote.send_body("ready");
    CHECK(remote.receive(1).front() == "ready");
    for (std::size_t index = 0; index < 4; ++index) {
        CHECK(await_future(server.submit("event")) == std::optional<std::string>("event"));
    }
    const auto remote_events = remote.receive(4);
    for (std::size_t index = 0; index < remote_events.size(); ++index) {
        CHECK(Json::parse(remote_events[index])["params"]["serial"] == index + 1);
    }
    const auto overflow = server.take_local_notifications();
    REQUIRE(overflow.size() == 1);
    CHECK(Json::parse(overflow.front())["method"] == "transport.resyncRequired");
    CHECK(server.stats().local_notification_overflows == 1);
    CHECK(server.take_local_notifications().empty());
    (void)await_future(server.submit("event"));
    const auto fresh = server.take_local_notifications();
    REQUIRE(fresh.size() == 1);
    CHECK(Json::parse(fresh.front())["params"]["serial"] == 5);
    server.stop();
    check_shutdown(server);
}

TEST_CASE("local notification byte limit bounds one large event without dropping its TCP copy") {
    std::vector<std::string> pending;
    TcpServerOptions options;
    options.max_local_notification_bytes = 256;
    const auto notification = Json{{"jsonrpc", "2.0"},
                                   {"method", "large"},
                                   {"params", Json{{"text", std::string(300, 'x')}}}}
                                  .dump();
    TcpServer server(
        [&](std::string_view body) -> std::optional<std::string> {
            if (body == "large") {
                pending.push_back(notification);
            }
            return std::string(body);
        },
        [&] {
            return std::exchange(pending, {});
        },
        options);
    server.start();
    SocketClient remote(server.port());
    remote.send_body("ready");
    (void)remote.receive(1);
    (void)await_future(server.submit("large"));
    CHECK(remote.receive(1).front() == notification);
    const auto local = server.take_local_notifications();
    REQUIRE(local.size() == 1);
    CHECK(local.front().size() <= options.max_local_notification_bytes);
    CHECK(Json::parse(local.front())["method"] == "transport.resyncRequired");
    server.stop();
    check_shutdown(server);
}

TEST_CASE("libuv background work leaves the owner loop responsive and completes on that loop") {
    std::thread::id loop_thread;
    std::thread::id work_thread;
    std::thread::id completion_thread;
    astra::transport::TcpServerHooks hooks;
    hooks.on_start = [&] {
        loop_thread = std::this_thread::get_id();
    };
    TcpServer server(echo_handler(), {}, {}, hooks);
    server.start();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    const auto release_future = release.get_future().share();
    auto work = server.queue_work(
        [&, release_future] {
            work_thread = std::this_thread::get_id();
            entered.set_value();
            await_release(release_future);
        },
        [&](std::exception_ptr error) {
            if (error) {
                std::rethrow_exception(error);
            }
            completion_thread = std::this_thread::get_id();
        });
    await_future(std::move(entered_future));
    auto responsive = server.submit("while work is blocked");
    const bool ready = responsive.wait_for(2s) == std::future_status::ready;
    release.set_value();
    REQUIRE(ready);
    CHECK(await_future(std::move(responsive)) ==
          std::optional<std::string>("while work is blocked"));
    await_future(std::move(work));
    server.stop();
    CHECK(work_thread != loop_thread);
    CHECK(completion_thread == loop_thread);
    CHECK(loop_thread != std::this_thread::get_id());
    check_shutdown(server);
}

TEST_CASE("libuv worker errors reach completion and its future without killing the owner loop") {
    TcpServer server(echo_handler());
    server.start();
    bool completion_saw_error = false;
    auto failed = server.queue_work(
        [] {
            throw std::invalid_argument("worker failed");
        },
        [&](std::exception_ptr error) {
            completion_saw_error = static_cast<bool>(error);
        });
    CHECK_THROWS_AS((void)(await_future(std::move(failed))), std::invalid_argument);
    CHECK(completion_saw_error);
    auto failed_completion = server.queue_work(
        [] {
        },
        [](std::exception_ptr) {
            throw std::out_of_range("completion failed");
        });
    CHECK_THROWS_AS((void)(await_future(std::move(failed_completion))), std::out_of_range);
    CHECK(await_future(server.submit("still alive")) == std::optional<std::string>("still alive"));
    server.stop();
    check_shutdown(server);
}

TEST_CASE("libuv shutdown waits for active workers and calls stop hook after completion") {
    std::vector<std::string> order;
    bool completed = false;
    std::size_t notification_polls_after_completion = 0;
    astra::transport::TcpServerHooks hooks;
    hooks.on_start = [&] {
        order.push_back("start");
    };
    hooks.on_stop = [&] {
        order.push_back("stop");
    };
    TcpServer server(
        echo_handler(),
        [&] {
            if (completed) {
                ++notification_polls_after_completion;
            }
            return std::vector<std::string>{};
        },
        {},
        hooks);
    server.start();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    const auto release_future = release.get_future().share();
    auto work = server.queue_work(
        [&, release_future] {
            entered.set_value();
            await_release(release_future);
        },
        [&](std::exception_ptr error) {
            if (error) {
                std::rethrow_exception(error);
            }
            order.push_back("complete");
            completed = true;
        });
    await_future(std::move(entered_future));
    std::atomic<bool> stopped{};
    std::jthread stopping([&] {
        server.stop();
        stopped.store(true);
    });
    const bool stop_requested = wait_until([&] {
        return !server.running();
    });
    const bool waited = !stopped.load();
    release.set_value();
    stopping.join();
    REQUIRE(stop_requested);
    CHECK(waited);
    await_future(std::move(work));
    CHECK(order == std::vector<std::string>{"start", "complete", "stop"});
    CHECK(notification_polls_after_completion == 0);
    check_shutdown(server);
}

TEST_CASE("libuv queued work cancellation invokes completion on the loop and settles its future") {
    std::thread::id loop_thread;
    std::thread::id completion_thread;
    astra::transport::TcpServerHooks hooks;
    hooks.on_start = [&] {
        loop_thread = std::this_thread::get_id();
    };
    TcpServer server(echo_handler(), {}, {}, hooks);
    server.start();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    const auto release_future = release.get_future().share();
    auto blocking = server.post([&, release_future] {
        entered.set_value();
        await_release(release_future);
    });
    await_future(std::move(entered_future));
    bool worker_ran = false;
    bool cancelled = false;
    auto work = server.queue_work(
        [&] {
            worker_ran = true;
        },
        [&](std::exception_ptr error) {
            completion_thread = std::this_thread::get_id();
            if (error) {
                try {
                    std::rethrow_exception(error);
                } catch (const astra::transport::TransportCancelled&) {
                    cancelled = true;
                }
            }
        });
    std::jthread stopping([&] {
        server.stop();
    });
    const bool stop_requested = wait_until([&] {
        return !server.running();
    });
    release.set_value();
    stopping.join();
    REQUIRE(stop_requested);
    await_future(std::move(blocking));
    CHECK_THROWS_AS((void)(await_future(std::move(work))), astra::transport::TransportCancelled);
    CHECK_FALSE(worker_ran);
    CHECK(cancelled);
    CHECK(completion_thread == loop_thread);
    check_shutdown(server);
}

TEST_CASE("libuv work admission is bounded and startup failure still releases the loop on its "
          "owner thread") {
    TcpServerOptions options;
    options.max_pending_work = 1;
    TcpServer server(echo_handler(), {}, options);
    server.start();
    std::promise<void> entered;
    auto entered_future = entered.get_future();
    std::promise<void> release;
    const auto release_future = release.get_future().share();
    auto first = server.queue_work(
        [&, release_future] {
            entered.set_value();
            await_release(release_future);
        },
        [](std::exception_ptr) {
        });
    await_future(std::move(entered_future));
    bool rejected_completion = false;
    auto rejected = server.queue_work(
        [] {
        },
        [&](std::exception_ptr) {
            rejected_completion = true;
        });
    release.set_value();
    await_future(std::move(first));
    CHECK_THROWS_AS((void)(await_future(std::move(rejected))), astra::transport::TransportError);
    CHECK_FALSE(rejected_completion);
    server.stop();
    check_shutdown(server);
    std::thread::id startup_thread;
    std::thread::id shutdown_thread;
    astra::transport::TcpServerHooks hooks;
    hooks.on_start = [&] {
        startup_thread = std::this_thread::get_id();
        throw std::logic_error("startup failed");
    };
    hooks.on_stop = [&] {
        shutdown_thread = std::this_thread::get_id();
    };
    TcpServer failed(echo_handler(), {}, {}, hooks);
    CHECK_THROWS_AS((void)(failed.start()), std::logic_error);
    CHECK(startup_thread == shutdown_thread);
    CHECK(startup_thread != std::this_thread::get_id());
    check_shutdown(failed);
}

TEST_CASE("libuv idle notification timer delivers to TCP and local consumers without requests") {
    std::atomic<bool> publish{};
    std::thread::id owner;
    std::thread::id provider_thread;
    const std::string event = R"({"jsonrpc":"2.0","method":"idle.ready","params":{"value":7}})";
    TcpServerOptions options;
    options.notification_interval_ms = 10;
    astra::transport::TcpServerHooks hooks;
    hooks.on_start = [&] {
        owner = std::this_thread::get_id();
    };
    TcpServer server(
        echo_handler(),
        [&] {
            provider_thread = std::this_thread::get_id();
            return publish.exchange(false) ? std::vector<std::string>{event}
                                           : std::vector<std::string>{};
        },
        options,
        hooks);
    server.start();
    SocketClient client(server.port());
    REQUIRE(wait_until([&] {
        return server.stats().active_connections == 1;
    }));
    publish.store(true);
    CHECK(client.receive(1).front() == event);
    std::vector<std::string> local;
    REQUIRE(wait_until([&] {
        local = server.take_local_notifications();
        return !local.empty();
    }));
    CHECK(local == std::vector<std::string>{event});
    const auto stopped = Clock::now();
    server.stop();
    CHECK(Clock::now() - stopped < 1s);
    CHECK(provider_thread == owner);
    CHECK(server.stats().notification_polls > 0);
    CHECK(server.stats().notification_poll_errors == 0);
    check_shutdown(server);
}

TEST_CASE("libuv notification timer records provider failures and keeps servicing idle clients") {
    std::atomic<bool> armed{};
    int attempts{};
    TcpServerOptions options;
    options.notification_interval_ms = 10;
    TcpServer server(
        echo_handler(),
        [&]() -> std::vector<std::string> {
            if (!armed.load()) {
                return {};
            }
            ++attempts;
            if (attempts == 1) {
                throw std::runtime_error("intentional idle provider failure");
            }
            return attempts == 2 ? std::vector<std::string>{"recovered"}
                                 : std::vector<std::string>{};
        },
        options);
    server.start();
    SocketClient client(server.port());
    REQUIRE(wait_until([&] {
        return server.stats().active_connections == 1;
    }));
    armed.store(true);
    CHECK(client.receive(1).front() == "recovered");
    CHECK(server.stats().notification_poll_errors == 1);
    CHECK(server.stats().handler_errors == 1);
    client.send_bytes(astra::protocol::encode_frame("request remains live"));
    CHECK(client.receive(1).front() == "request remains live");
    server.stop();
    check_shutdown(server);
}

TEST_CASE("libuv long notification timer closes promptly and restarts without leaked handles") {
    TcpServerOptions options;
    options.notification_interval_ms = 60000;
    TcpServer server(
        echo_handler(),
        [] {
            return std::vector<std::string>{};
        },
        options);
    for (int iteration = 0; iteration < 2; ++iteration) {
        server.start();
        const auto stopped = Clock::now();
        server.stop();
        CHECK(Clock::now() - stopped < 1s);
        check_shutdown(server);
    }
    CHECK(server.stats().notification_polls == 0);
}

#endif // defined(_WIN32): native Windows acceptance only in this environment.
