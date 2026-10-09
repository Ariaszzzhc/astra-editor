#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace astra::transport {

struct TcpServerOptions {
    std::size_t max_connections = 64;
    std::size_t max_queued_write_bytes = 16 * 1024 * 1024;
    std::size_t max_total_queued_write_bytes = 64 * 1024 * 1024;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::size_t max_header_bytes = 8 * 1024;
    std::size_t max_pending_tasks = 256;
    std::size_t max_queued_task_bytes = 16 * 1024 * 1024;
    std::size_t max_pending_work = 64;
    std::size_t max_local_notifications = 1024;
    std::size_t max_local_notification_bytes = 8 * 1024 * 1024;
};

struct TcpServerHooks {
    std::function<void()> on_start;
    std::function<void()> on_stop;
};

struct TcpServerStats {
    std::uint64_t accepted_connections{};
    std::uint64_t rejected_connections{};
    std::uint64_t protocol_errors{};
    std::uint64_t handler_errors{};
    std::uint64_t write_limit_disconnects{};
    std::size_t active_connections{};
    std::size_t pending_write_bytes{};
    std::size_t pending_write_requests{};
    int loop_close_error{};
    std::size_t pending_tasks{};
    std::size_t pending_work{};
    std::uint64_t local_notification_overflows{};
};

class TransportError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class TransportCancelled final : public TransportError {
public:
    using TransportError::TransportError;
};

// Owns a libuv loop and one worker thread. Both callbacks execute only on that
// thread; captured services must not be accessed concurrently elsewhere. Requests
// and notifications use Content-Length framing. Responses go only to their
// origin; notifications are broadcast after each completed request.
//
// Lifecycle calls must be serialized by the owner. start() synchronously binds
// before launching the worker; stop() cancels clients/writes and joins after all
// close callbacks complete. The destructor must run outside a server callback.
// on_start/on_stop hooks execute on the loop thread; start waits for on_start,
// and on_stop runs only after every accepted task/work completion has settled.
// Only literal 127.0.0.1 and ::1 are accepted: use an authenticated SSH tunnel for
// remote access until a native authenticated transport is implemented.
class TcpServer {
public:
    using Handler = std::function<std::optional<std::string>(std::string_view)>;
    using Notifications = std::function<std::vector<std::string>()>;

    explicit TcpServer(Handler handler, Notifications notifications = {},
                       TcpServerOptions options = {}, TcpServerHooks hooks = {});
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;
    TcpServer(TcpServer&&) = delete;
    TcpServer& operator=(TcpServer&&) = delete;

    void start(std::string_view address = "127.0.0.1", std::uint16_t port = 0);
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] TcpServerStats stats() const noexcept;

    // Thread-safe local entry points. Accepted tasks execute FIFO on the same
    // owner loop as TCP handlers. Queue overflow/stopped-server failures arrive
    // through the future. stop() cancels tasks which have not started.
    // Do not block waiting for these futures from within a loop callback.
    [[nodiscard]] std::future<std::optional<std::string>> submit(std::string body);
    [[nodiscard]] std::future<void> post(std::function<void()> task);

    // The worker runs in libuv's thread pool and must not access loop-owned
    // services. Completion runs on the owner loop, even after worker failure or
    // cancellation of accepted work; rejected work does not invoke completion.
    // The future finishes after completion and propagates worker/completion
    // errors. stop() cancels work that has not started and waits for active work.
    [[nodiscard]] std::future<void> queue_work(
        std::function<void()> worker,
        std::function<void(std::exception_ptr)> completion);

    // One local GUI consumer receives the same notifications as TCP clients.
    // Overflow drops queued local events and replaces them with the explicit
    // transport.resyncRequired notification; new events are coalesced until it
    // is consumed. The GUI then reads fresh versioned snapshots.
    [[nodiscard]] std::vector<std::string> take_local_notifications();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::transport
