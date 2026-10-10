#include <astra/transport/tcp_server.hpp>

#include <astra/protocol/framing.hpp>
#include <uv.h>

#include <array>
#include <atomic>
#include <deque>
#include <exception>
#include <future>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace astra::transport {
namespace {

void uv_check(int status, std::string_view operation) {
    if (status < 0) {
        throw TransportError(std::string(operation) + ": " + uv_strerror(status));
    }
}

} // namespace

class TcpServer::Impl {
  public:
    Impl(Handler handler,
         Notifications notifications,
         TcpServerOptions options,
         TcpServerHooks hooks)
        : handler_(std::move(handler)), notifications_(std::move(notifications)), options_(options),
          hooks_(std::move(hooks)) {
        if (!handler_ || options_.max_connections == 0 || options_.max_queued_write_bytes == 0 ||
            options_.max_total_queued_write_bytes == 0 || options_.max_body_bytes == 0 ||
            options_.max_header_bytes < 4 || options_.max_pending_tasks == 0 ||
            options_.max_queued_task_bytes == 0 || options_.max_pending_work == 0 ||
            options_.max_local_notifications == 0 || options_.max_local_notification_bytes < 256 ||
            options_.notification_interval_ms == 0 || options_.notification_interval_ms > 60000 ||
            options_.max_body_bytes > std::numeric_limits<unsigned int>::max() - 128U) {
            throw std::invalid_argument("Invalid TCP server handler or resource limits");
        }
    }

    ~Impl() {
        stop();
    }

    void start(std::string_view address, std::uint16_t port) {
        if (address != "127.0.0.1" && address != "::1") {
            throw std::invalid_argument("TCP server only permits literal loopback addresses");
        }
        if (worker_.joinable() || loop_initialized_) {
            throw std::logic_error("TCP server is already started; stop it before restarting");
        }
        stop_requested_.store(false);
        shutting_down_ = false;
        loop_ = {};
        listener_ = {};
        control_ = {};
        notification_timer_ = {};
        uv_check(uv_loop_init(&loop_), "Initialize TCP event loop");
        loop_initialized_ = true;
        try {
            uv_check(uv_tcp_init(&loop_, &listener_), "Initialize TCP listener");
            listener_initialized_ = true;
            listener_.data = this;
            uv_check(uv_async_init(&loop_, &control_, control_callback),
                     "Initialize TCP shutdown signal");
            control_initialized_ = true;
            control_.data = this;
            if (notifications_) {
                uv_check(uv_timer_init(&loop_, &notification_timer_),
                         "Initialize notification timer");
                notification_timer_initialized_ = true;
                notification_timer_.data = this;
            }
            if (address == "::1") {
                sockaddr_in6 endpoint{};
                uv_check(uv_ip6_addr("::1", port, &endpoint), "Parse IPv6 loopback address");
                uv_check(uv_tcp_bind(&listener_,
                                     reinterpret_cast<const sockaddr*>(&endpoint),
                                     UV_TCP_IPV6ONLY),
                         "Bind IPv6 loopback listener");
            } else {
                sockaddr_in endpoint{};
                uv_check(uv_ip4_addr("127.0.0.1", port, &endpoint), "Parse IPv4 loopback address");
                uv_check(uv_tcp_bind(&listener_, reinterpret_cast<const sockaddr*>(&endpoint), 0),
                         "Bind IPv4 loopback listener");
            }
            uv_check(uv_listen(reinterpret_cast<uv_stream_t*>(&listener_), 128, accept_callback),
                     "Listen on loopback socket");
            sockaddr_storage bound{};
            int length = static_cast<int>(sizeof(bound));
            uv_check(uv_tcp_getsockname(&listener_, reinterpret_cast<sockaddr*>(&bound), &length),
                     "Query TCP listener port");
            const auto actual_port =
                bound.ss_family == AF_INET
                    ? ntohs(reinterpret_cast<const sockaddr_in*>(&bound)->sin_port)
                    : ntohs(reinterpret_cast<const sockaddr_in6*>(&bound)->sin6_port);
            port_.store(actual_port);
            running_.store(true);
            const auto startup = std::make_shared<std::promise<void>>();
            auto started = startup->get_future();
            worker_ = std::jthread([this, startup] {
                try {
                    if (hooks_.on_start) {
                        hooks_.on_start();
                    }
                    if (notification_timer_initialized_) {
                        uv_check(uv_timer_start(&notification_timer_,
                                                notification_timer_callback,
                                                options_.notification_interval_ms,
                                                options_.notification_interval_ms),
                                 "Start notification timer");
                    }
                    startup->set_value();
                } catch (...) {
                    startup->set_exception(std::current_exception());
                    stop_requested_.store(true);
                    begin_shutdown();
                }
                uv_run(&loop_, UV_RUN_DEFAULT);
                try {
                    if (hooks_.on_stop) {
                        hooks_.on_stop();
                    }
                } catch (...) {
                    ++handler_errors_;
                }
                const int status = uv_loop_close(&loop_);
                loop_close_error_.store(status);
                running_.store(false);
            });
            started.get();
        } catch (...) {
            running_.store(false);
            if (worker_.joinable()) {
                stop();
            } else {
                close_initialization();
            }
            throw;
        }
    }

    void stop() noexcept {
        if (!worker_.joinable()) {
            return;
        }
        stop_requested_.store(true);
        if (std::this_thread::get_id() == worker_.get_id()) {
            begin_shutdown();
            return;
        }
        {
            // uv_async_send is thread-safe; sending concurrently with destroying
            // its handle is not. The loop takes this same mutex before closing it.
            const std::lock_guard lock(control_mutex_);
            if (control_initialized_) {
                (void)uv_async_send(&control_);
            }
        }
        worker_.join();
        loop_initialized_ = false;
        listener_initialized_ = false;
        port_.store(0);
    }

    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_.load();
    }

    [[nodiscard]] bool running() const noexcept {
        return running_.load() && !stop_requested_.load();
    }

    [[nodiscard]] TcpServerStats stats() const noexcept {
        return {accepted_.load(),
                rejected_.load(),
                protocol_errors_.load(),
                handler_errors_.load(),
                write_limits_.load(),
                active_.load(),
                queued_bytes_.load(),
                pending_writes_.load(),
                loop_close_error_.load(),
                pending_tasks_.load(),
                pending_work_.load(),
                local_overflows_.load(),
                notification_polls_.load(),
                notification_poll_errors_.load()};
    }

    std::future<std::optional<std::string>> submit(std::string body) {
        const auto promise = std::make_shared<std::promise<std::optional<std::string>>>();
        auto future = promise->get_future();
        if (body.size() > options_.max_body_bytes) {
            promise->set_exception(
                std::make_exception_ptr(TransportError("Local request exceeds body limit")));
            return future;
        }
        const auto bytes = body.size();
        QueuedTask task;
        task.bytes = bytes;
        task.run = [this, promise, body = std::move(body)] {
            try {
                auto response = handler_(body);
                if (response && response->size() > options_.max_body_bytes) {
                    throw TransportError("Local response exceeds body limit");
                }
                broadcast_notifications();
                promise->set_value(std::move(response));
            } catch (...) {
                ++handler_errors_;
                promise->set_exception(std::current_exception());
            }
        };
        task.cancel = [promise](std::exception_ptr error) {
            promise->set_exception(error);
        };
        if (auto error = enqueue(std::move(task))) {
            promise->set_exception(error);
        }
        return future;
    }

    std::future<void> post(std::function<void()> function) {
        const auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        if (!function) {
            promise->set_exception(
                std::make_exception_ptr(std::invalid_argument("Empty loop task")));
            return future;
        }
        QueuedTask task;
        task.run = [this, promise, function = std::move(function)] {
            try {
                function();
                broadcast_notifications();
                promise->set_value();
            } catch (...) {
                ++handler_errors_;
                promise->set_exception(std::current_exception());
            }
        };
        task.cancel = [promise](std::exception_ptr error) {
            promise->set_exception(error);
        };
        if (auto error = enqueue(std::move(task))) {
            promise->set_exception(error);
        }
        return future;
    }

    std::future<void> queue_work(std::function<void()> worker,
                                 std::function<void(std::exception_ptr)> completion) {
        auto work = std::make_shared<WorkItem>();
        work->server = this;
        work->worker = std::move(worker);
        work->completion = std::move(completion);
        auto future = work->promise.get_future();
        if (!work->worker || !work->completion) {
            work->promise.set_exception(std::make_exception_ptr(
                std::invalid_argument("Work and completion callbacks are required")));
            return future;
        }
        QueuedTask task;
        task.is_work = true;
        task.run = [this, work] {
            start_work(work);
        };
        task.cancel = [this, work](std::exception_ptr error) {
            finish_work(work, error);
        };
        if (auto error = enqueue(std::move(task))) {
            work->promise.set_exception(error);
        }
        return future;
    }

    std::vector<std::string> take_local_notifications() {
        const std::lock_guard lock(local_mutex_);
        std::vector<std::string> result;
        result.reserve(local_notifications_.size());
        while (!local_notifications_.empty()) {
            result.push_back(std::move(local_notifications_.front()));
            local_notifications_.pop_front();
        }
        local_notification_bytes_ = 0;
        local_resync_pending_ = false;
        return result;
    }

  private:
    static constexpr std::size_t read_buffer_bytes = 64 * 1024;

    struct Connection {
        Impl* server;
        uv_tcp_t socket{};
        protocol::FrameDecoder decoder;
        std::unique_ptr<std::array<char, read_buffer_bytes>> read_buffer;
        std::size_t queued_bytes{};
        bool closing{};
        bool counted{};
        bool read_ended{};

        explicit Connection(Impl& owner)
            : server(&owner),
              decoder(owner.options_.max_body_bytes, owner.options_.max_header_bytes) {
        }
    };

    struct PendingWrite {
        uv_write_t request{};
        std::shared_ptr<Connection> connection;
        std::shared_ptr<const std::string> frame;
    };

    struct QueuedTask {
        std::function<void()> run;
        std::function<void(std::exception_ptr)> cancel;
        std::size_t bytes{};
        bool is_work{};
    };

    struct WorkItem {
        uv_work_t request{};
        Impl* server{};
        std::function<void()> worker;
        std::function<void(std::exception_ptr)> completion;
        std::exception_ptr error;
        std::promise<void> promise;
    };

    std::exception_ptr enqueue(QueuedTask task) {
        const std::lock_guard lock(control_mutex_);
        if (!running_.load() || stop_requested_.load() || !control_initialized_) {
            return std::make_exception_ptr(TransportCancelled("Server is stopped or stopping"));
        }
        if (tasks_.size() >= options_.max_pending_tasks ||
            task.bytes > options_.max_queued_task_bytes - queued_task_bytes_ ||
            (task.is_work && pending_work_.load() >= options_.max_pending_work)) {
            return std::make_exception_ptr(TransportError("Server task queue capacity exceeded"));
        }
        const auto bytes = task.bytes;
        const bool is_work = task.is_work;
        tasks_.push_back(std::move(task));
        queued_task_bytes_ += bytes;
        ++pending_tasks_;
        if (is_work) {
            ++pending_work_;
        }
        (void)uv_async_send(&control_);
        return {};
    }

    void process_tasks() noexcept {
        // Bound one turn so TCP reads and other loop callbacks remain runnable.
        for (std::size_t count = 0; count < 32; ++count) {
            if (stop_requested_.load()) {
                begin_shutdown();
                return;
            }
            QueuedTask task;
            {
                const std::lock_guard lock(control_mutex_);
                if (tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
                queued_task_bytes_ -= task.bytes;
                --pending_tasks_;
            }
            try {
                task.run();
            } catch (...) {
                ++handler_errors_;
                task.cancel(std::current_exception());
            }
        }
        const std::lock_guard lock(control_mutex_);
        if (!tasks_.empty() && control_initialized_) {
            (void)uv_async_send(&control_);
        }
    }

    void start_work(const std::shared_ptr<WorkItem>& work) {
        work->request.data = work.get();
        try {
            work_items_.emplace(&work->request, work);
            const int status =
                uv_queue_work(&loop_, &work->request, work_callback, after_work_callback);
            if (status < 0) {
                work_items_.erase(&work->request);
                finish_work(work, std::make_exception_ptr(TransportError(uv_strerror(status))));
            }
        } catch (...) {
            work_items_.erase(&work->request);
            finish_work(work, std::current_exception());
        }
    }

    void finish_work(const std::shared_ptr<WorkItem>& work, std::exception_ptr error) noexcept {
        try {
            work->completion(error);
            broadcast_notifications();
        } catch (...) {
            ++handler_errors_;
            error = std::current_exception();
        }
        if (error) {
            work->promise.set_exception(error);
        } else {
            work->promise.set_value();
        }
        --pending_work_;
    }

    void close_initialization() noexcept {
        if (notification_timer_initialized_) {
            uv_close(reinterpret_cast<uv_handle_t*>(&notification_timer_), nullptr);
            notification_timer_initialized_ = false;
        }
        if (listener_initialized_) {
            uv_close(reinterpret_cast<uv_handle_t*>(&listener_), nullptr);
        }
        if (control_initialized_) {
            uv_close(reinterpret_cast<uv_handle_t*>(&control_), nullptr);
        }
        if (loop_initialized_) {
            uv_run(&loop_, UV_RUN_DEFAULT);
            loop_close_error_.store(uv_loop_close(&loop_));
        }
        listener_initialized_ = false;
        control_initialized_ = false;
        loop_initialized_ = false;
        port_.store(0);
    }

    void begin_shutdown() noexcept {
        if (shutting_down_) {
            return;
        }
        shutting_down_ = true;
        if (notification_timer_initialized_) {
            (void)uv_timer_stop(&notification_timer_);
            uv_close(reinterpret_cast<uv_handle_t*>(&notification_timer_), nullptr);
            notification_timer_initialized_ = false;
        }
        if (listener_initialized_ && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&listener_))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&listener_), nullptr);
        }
        for (const auto& [address, connection] : connections_) {
            (void)address;
            close_connection(*connection);
        }
        std::deque<QueuedTask> cancelled;
        {
            const std::lock_guard lock(control_mutex_);
            cancelled.swap(tasks_);
            queued_task_bytes_ = 0;
            pending_tasks_.store(0);
            if (control_initialized_) {
                control_initialized_ = false;
                uv_close(reinterpret_cast<uv_handle_t*>(&control_), nullptr);
            }
        }
        const auto error =
            std::make_exception_ptr(TransportCancelled("Server stopped before task execution"));
        for (auto& task : cancelled) {
            task.cancel(error);
        }
        for (const auto& [request, work] : work_items_) {
            (void)work;
            // Active workers cannot be cancelled and are allowed to finish.
            // Their after-work callbacks keep uv_run alive until completion.
            (void)uv_cancel(reinterpret_cast<uv_req_t*>(request));
        }
        // Do not uv_stop(): every pending write and close callback must drain.
    }

    void close_connection(Connection& connection) noexcept {
        if (connection.closing) {
            return;
        }
        connection.closing = true;
        if (connection.counted) {
            connection.counted = false;
            active_.fetch_sub(1);
        }
        (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&connection.socket));
        uv_close(reinterpret_cast<uv_handle_t*>(&connection.socket), close_callback);
    }

    void accept_connection() {
        auto connection = std::make_shared<Connection>(*this);
        const auto key = connection.get();
        // Register ownership before initializing a libuv handle. Map allocation
        // failure therefore cannot destroy an initialized, unclosed handle.
        connections_.emplace(key, connection);
        const int initialized = uv_tcp_init(&loop_, &connection->socket);
        if (initialized < 0) {
            connections_.erase(key);
            ++handler_errors_;
            return;
        }
        connection->socket.data = key;
        const int accepted = uv_accept(reinterpret_cast<uv_stream_t*>(&listener_),
                                       reinterpret_cast<uv_stream_t*>(&connection->socket));
        if (accepted < 0) {
            close_connection(*connection);
            return;
        }
        if (shutting_down_ || active_.load() >= options_.max_connections) {
            ++rejected_;
            close_connection(*connection);
            return;
        }
        try {
            connection->read_buffer = std::make_unique<std::array<char, read_buffer_bytes>>();
            uv_check(uv_tcp_nodelay(&connection->socket, 1), "Enable TCP_NODELAY");
            uv_check(uv_read_start(reinterpret_cast<uv_stream_t*>(&connection->socket),
                                   allocate_callback,
                                   read_callback),
                     "Start TCP client reads");
            connection->counted = true;
            ++active_;
            ++accepted_;
        } catch (...) {
            close_connection(*connection);
            throw;
        }
    }

    void write_frame(const std::shared_ptr<Connection>& connection,
                     const std::shared_ptr<const std::string>& frame) {
        if (connection->closing) {
            return;
        }
        const auto bytes = frame->size();
        if (bytes > options_.max_queued_write_bytes - connection->queued_bytes ||
            bytes > options_.max_total_queued_write_bytes - queued_bytes_.load()) {
            ++write_limits_;
            close_connection(*connection);
            return;
        }
        auto write = std::make_unique<PendingWrite>();
        write->connection = connection;
        write->frame = frame;
        auto* request = &write->request;
        request->data = write.get();
        writes_.emplace(request, std::move(write));
        connection->queued_bytes += bytes;
        queued_bytes_.fetch_add(bytes);
        ++pending_writes_;
        auto buffer =
            uv_buf_init(const_cast<char*>(frame->data()), static_cast<unsigned int>(frame->size()));
        const int status = uv_write(request,
                                    reinterpret_cast<uv_stream_t*>(&connection->socket),
                                    &buffer,
                                    1,
                                    write_callback);
        if (status < 0) {
            connection->queued_bytes -= bytes;
            queued_bytes_.fetch_sub(bytes);
            --pending_writes_;
            writes_.erase(request);
            close_connection(*connection);
        }
    }

    void send_body(const std::shared_ptr<Connection>& connection, std::string_view body) {
        auto frame = std::make_shared<const std::string>(
            protocol::encode_frame(body, options_.max_body_bytes));
        write_frame(connection, frame);
    }

    void broadcast_notifications() {
        // Providers may schedule background work. Once shutdown begins, drain
        // already accepted workers without starting another recovery/watch cycle.
        if (!notifications_ || shutting_down_ || stop_requested_.load()) {
            return;
        }
        const auto notifications = notifications_();
        for (const auto& body : notifications) {
            auto frame = std::make_shared<const std::string>(
                protocol::encode_frame(body, options_.max_body_bytes));
            record_local_notification(body);
            for (const auto& [address, connection] : connections_) {
                (void)address;
                write_frame(connection, frame);
            }
        }
    }

    void record_local_notification(const std::string& body) {
        const std::lock_guard lock(local_mutex_);
        if (local_resync_pending_) {
            return;
        }
        if (local_notifications_.size() >= options_.max_local_notifications ||
            body.size() > options_.max_local_notification_bytes - local_notification_bytes_) {
            local_notifications_.clear();
            local_notifications_.emplace_back(
                "{\"jsonrpc\":\"2.0\",\"method\":\"transport.resyncRequired\","
                "\"params\":{\"reason\":\"localNotificationOverflow\"}}");
            local_notification_bytes_ = local_notifications_.front().size();
            local_resync_pending_ = true;
            ++local_overflows_;
            return;
        }
        local_notifications_.push_back(body);
        local_notification_bytes_ += body.size();
    }

    void read(Connection& connection, std::string_view bytes) {
        const auto found = connections_.find(&connection);
        if (found == connections_.end() || connection.closing) {
            return;
        }
        const auto owner = found->second;
        std::vector<std::string> messages;
        try {
            messages = connection.decoder.feed(bytes);
        } catch (const protocol::FramingError&) {
            ++protocol_errors_;
            close_connection(connection);
            return;
        }
        for (const auto& message : messages) {
            if (connection.closing || shutting_down_) {
                break;
            }
            try {
                if (const auto response = handler_(message)) {
                    send_body(owner, *response);
                }
                // Drain committed notifications even if this origin disconnected
                // because it exceeded its write budget.
                broadcast_notifications();
            } catch (...) {
                ++handler_errors_;
                close_connection(connection);
                break;
            }
        }
    }

    static void control_callback(uv_async_t* handle) noexcept {
        auto& self = *static_cast<Impl*>(handle->data);
        if (self.stop_requested_.load()) {
            self.begin_shutdown();
        } else {
            self.process_tasks();
        }
    }

    static void notification_timer_callback(uv_timer_t* handle) noexcept {
        auto& self = *static_cast<Impl*>(handle->data);
        if (self.stop_requested_.load()) {
            self.begin_shutdown();
            return;
        }
        ++self.notification_polls_;
        try {
            self.broadcast_notifications();
        } catch (...) {
            ++self.handler_errors_;
            ++self.notification_poll_errors_;
        }
    }

    static void work_callback(uv_work_t* request) noexcept {
        auto& work = *static_cast<WorkItem*>(request->data);
        try {
            work.worker();
        } catch (...) {
            work.error = std::current_exception();
        }
    }

    static void after_work_callback(uv_work_t* request, int status) noexcept {
        const auto* pending = static_cast<WorkItem*>(request->data);
        auto& self = *pending->server;
        const auto found = self.work_items_.find(request);
        if (found == self.work_items_.end()) {
            return;
        }
        auto work = std::move(found->second);
        self.work_items_.erase(found);
        auto error = work->error;
        if (status == UV_ECANCELED) {
            error = std::make_exception_ptr(
                TransportCancelled("Background work cancelled during shutdown"));
        } else if (status < 0) {
            error = std::make_exception_ptr(TransportError(uv_strerror(status)));
        }
        self.finish_work(work, error);
    }

    static void accept_callback(uv_stream_t* listener, int status) noexcept {
        auto& self = *static_cast<Impl*>(listener->data);
        if (status < 0) {
            ++self.handler_errors_;
            return;
        }
        if (self.shutting_down_) {
            return;
        }
        try {
            self.accept_connection();
        } catch (...) {
            ++self.handler_errors_;
        }
    }

    static void
    allocate_callback(uv_handle_t* handle, std::size_t suggested, uv_buf_t* buffer) noexcept {
        (void)suggested;
        const auto& connection = *static_cast<Connection*>(handle->data);
        *buffer =
            uv_buf_init(connection.read_buffer ? connection.read_buffer->data() : nullptr,
                        connection.read_buffer ? static_cast<unsigned int>(read_buffer_bytes) : 0);
    }

    static void read_callback(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) noexcept {
        auto& connection = *static_cast<Connection*>(stream->data);
        auto& self = *connection.server;
        if (count < 0) {
            if (count == UV_EOF) {
                try {
                    connection.decoder.finish();
                    connection.read_ended = true;
                    (void)uv_read_stop(stream);
                    if (connection.queued_bytes != 0) {
                        return;
                    }
                } catch (const protocol::FramingError&) {
                    ++self.protocol_errors_;
                }
            }
            self.close_connection(connection);
            return;
        }
        if (count == 0) {
            return;
        }
        try {
            self.read(connection, std::string_view(buffer->base, static_cast<std::size_t>(count)));
        } catch (...) {
            ++self.handler_errors_;
            self.close_connection(connection);
        }
    }

    static void write_callback(uv_write_t* request, int status) noexcept {
        const auto* pending = static_cast<PendingWrite*>(request->data);
        auto& self = *pending->connection->server;
        const auto found = self.writes_.find(request);
        if (found == self.writes_.end()) {
            return;
        }
        // This shared connection owner survives even if its uv_close callback
        // already removed it from connections_. No callback uses freed storage.
        auto write = std::move(found->second);
        self.writes_.erase(found);
        const auto bytes = write->frame->size();
        write->connection->queued_bytes -= bytes;
        self.queued_bytes_.fetch_sub(bytes);
        --self.pending_writes_;
        if (status < 0 || (write->connection->read_ended && write->connection->queued_bytes == 0)) {
            self.close_connection(*write->connection);
        }
    }

    static void close_callback(uv_handle_t* handle) noexcept {
        auto* connection = static_cast<Connection*>(handle->data);
        connection->server->connections_.erase(connection);
    }

    Handler handler_;
    Notifications notifications_;
    TcpServerOptions options_;
    TcpServerHooks hooks_;
    uv_loop_t loop_{};
    uv_tcp_t listener_{};
    uv_async_t control_{};
    uv_timer_t notification_timer_{};
    bool loop_initialized_{};
    bool listener_initialized_{};
    bool control_initialized_{};
    bool notification_timer_initialized_{};
    bool shutting_down_{};
    std::mutex control_mutex_;
    std::jthread worker_;
    std::unordered_map<Connection*, std::shared_ptr<Connection>> connections_;
    std::unordered_map<uv_write_t*, std::unique_ptr<PendingWrite>> writes_;
    std::unordered_map<uv_work_t*, std::shared_ptr<WorkItem>> work_items_;
    std::deque<QueuedTask> tasks_;
    std::size_t queued_task_bytes_{};
    std::mutex local_mutex_;
    std::deque<std::string> local_notifications_;
    std::size_t local_notification_bytes_{};
    bool local_resync_pending_{};
    std::atomic<bool> stop_requested_{};
    std::atomic<bool> running_{};
    std::atomic<std::uint16_t> port_{};
    std::atomic<std::uint64_t> accepted_{};
    std::atomic<std::uint64_t> rejected_{};
    std::atomic<std::uint64_t> protocol_errors_{};
    std::atomic<std::uint64_t> handler_errors_{};
    std::atomic<std::uint64_t> write_limits_{};
    std::atomic<std::size_t> active_{};
    std::atomic<std::size_t> queued_bytes_{};
    std::atomic<std::size_t> pending_writes_{};
    std::atomic<int> loop_close_error_{};
    std::atomic<std::size_t> pending_tasks_{};
    std::atomic<std::size_t> pending_work_{};
    std::atomic<std::uint64_t> local_overflows_{};
    std::atomic<std::uint64_t> notification_polls_{};
    std::atomic<std::uint64_t> notification_poll_errors_{};
};

TcpServer::TcpServer(Handler handler,
                     Notifications notifications,
                     TcpServerOptions options,
                     TcpServerHooks hooks)
    : impl_(std::make_unique<Impl>(
          std::move(handler), std::move(notifications), options, std::move(hooks))) {
}

TcpServer::~TcpServer() = default;

void TcpServer::start(std::string_view address, std::uint16_t port) {
    impl_->start(address, port);
}

void TcpServer::stop() noexcept {
    impl_->stop();
}

std::uint16_t TcpServer::port() const noexcept {
    return impl_->port();
}

bool TcpServer::running() const noexcept {
    return impl_->running();
}

TcpServerStats TcpServer::stats() const noexcept {
    return impl_->stats();
}

std::future<std::optional<std::string>> TcpServer::submit(std::string body) {
    return impl_->submit(std::move(body));
}

std::future<void> TcpServer::post(std::function<void()> task) {
    return impl_->post(std::move(task));
}

std::future<void> TcpServer::queue_work(std::function<void()> worker,
                                        std::function<void(std::exception_ptr)> completion) {
    return impl_->queue_work(std::move(worker), std::move(completion));
}

std::vector<std::string> TcpServer::take_local_notifications() {
    return impl_->take_local_notifications();
}

} // namespace astra::transport
