#pragma once

#include <astra/transport/tcp_server.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace astra::transport {

struct TcpClientOptions {
    std::chrono::milliseconds connect_timeout{5000};
    std::chrono::milliseconds request_timeout{30000};
    std::size_t max_pending_requests = 256;
    std::size_t max_queued_write_bytes = 16 * 1024 * 1024;
    std::size_t max_buffered_response_bytes = 16 * 1024 * 1024;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::size_t max_header_bytes = 8 * 1024;
    std::size_t max_notifications = 1024;
    std::size_t max_notification_bytes = 8 * 1024 * 1024;
};

struct TcpClientStats {
    std::size_t pending_requests{};
    std::size_t queued_write_bytes{};
    std::uint64_t notification_overflows{};
    std::uint64_t protocol_errors{};
    int loop_close_error{};
};

// Owns a libuv loop/thread. connect() resolves a hostname and connects without
// blocking its caller. All resolved addresses must be loopback: use an
// authenticated SSH tunnel for remote services. A client connects once; after a
// disconnect or timeout create a new client, then resynchronize service state.
// close()/destruction join after all handles, writes and DNS callbacks drain.
// An already-running OS DNS lookup cannot be forcibly killed by libuv; the
// connect future still times out promptly, and close waits for resolver cleanup.
// Lifecycle calls must be serialized. submit()/take_notifications() are thread-safe.
class TcpClient {
  public:
    explicit TcpClient(TcpClientOptions options = {});
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;
    TcpClient(TcpClient&&) = delete;
    TcpClient& operator=(TcpClient&&) = delete;

    [[nodiscard]] std::future<void> connect(std::string host, std::uint16_t port);
    void close() noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] TcpClientStats stats() const noexcept;

    // Call after connect succeeds. Caller-supplied string/number/null IDs are
    // preserved and must be unique across outstanding requests (including batch
    // members). Batch replies are returned in request order. Invalid JSON,
    // invalid envelopes, oversized IDs, and ambiguous duplicate IDs fail the
    // future with TransportError BEFORE sending. Invalid params are sent to the
    // service, which returns its ordinary JSON-RPC error. This deliberate client
    // validation boundary prevents uncorrelatable error replies hanging forever.
    // Notifications finish with nullopt after their write completes; this is not
    // an acknowledgement of execution. All requests/writes have a deadline.
    // Timeout/disconnect fails every pending future and closes the connection;
    // an edit may already have executed, so do not blindly retry mutations.
    [[nodiscard]] std::future<std::optional<std::string>> submit(std::string body);

    // One GUI consumer. On overflow the bounded queue is replaced with a single
    // transport.resyncRequired notification; subsequent events coalesce until
    // that marker is consumed. The consumer must fetch fresh service snapshots.
    [[nodiscard]] std::vector<std::string> take_notifications();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::transport
