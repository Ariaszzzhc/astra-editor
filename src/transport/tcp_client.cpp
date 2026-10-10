#include <astra/transport/tcp_client.hpp>

#include <astra/protocol/framing.hpp>
#include <astra/protocol/rpc.hpp>
#include <uv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace astra::transport {
namespace {

using Json = protocol::Json;
using Clock = std::chrono::steady_clock;
constexpr std::string_view resync_notification =
    R"({"jsonrpc":"2.0","method":"transport.resyncRequired","params":{"reason":"notificationOverflow"}})";

void check_uv(int status, std::string_view operation) {
    if (status < 0) {
        throw TransportError(std::string(operation) + ": " + uv_strerror(status));
    }
}

Json parse_body(std::string_view body) {
    try {
        return Json::parse(body, [](int depth, Json::parse_event_t, Json&) {
            if (depth > static_cast<int>(protocol::Dispatcher::max_depth)) {
                throw TransportError("JSON-RPC nesting exceeds client limit");
            }
            return true;
        });
    } catch (const Json::exception& error) {
        throw TransportError(std::string("Invalid JSON-RPC JSON: ") + error.what());
    }
}

bool valid_id(const Json& id) {
    return id.is_null() || id.is_number() ||
           (id.is_string() &&
            id.get_ref<const std::string&>().size() <= protocol::Dispatcher::max_request_id_bytes);
}

bool valid_version(const Json& value) {
    return value.is_object() && value.contains("jsonrpc") && value["jsonrpc"] == "2.0";
}

bool loopback(const sockaddr* address) {
    if (address->sa_family == AF_INET) {
        return (ntohl(reinterpret_cast<const sockaddr_in*>(address)->sin_addr.s_addr) >> 24U) ==
               127U;
    }
    if (address->sa_family == AF_INET6) {
        const auto* bytes = reinterpret_cast<const unsigned char*>(
            &reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr);
        return std::all_of(bytes,
                           bytes + 15,
                           [](unsigned char value) {
                               return value == 0;
                           }) &&
               bytes[15] == 1;
    }
    return false;
}

} // namespace

class TcpClient::Impl {
    enum class State { idle, connecting, connected, failed, closing, closed };

    struct Group {
        std::promise<std::optional<std::string>> promise;
        std::string body;
        std::vector<std::string> ids;
        std::vector<std::optional<std::string>> responses;
        Clock::time_point deadline;
        std::size_t reserved_bytes{};
        std::size_t received_bytes{};
        std::size_t remaining{};
        bool batch{};
        bool done{};
    };

    struct Connection {
        Impl* owner{};
        uv_tcp_t socket{};
        uv_connect_t request{};
        std::size_t writes{};
        bool connecting{};
        bool closing{};
        bool close_done{};
    };

    struct Write {
        Impl* owner{};
        Connection* connection{}; // Borrowed until its write count reaches zero.
        uv_write_t request{};
        std::shared_ptr<Group> group;
        std::string frame;
        std::size_t reserved_bytes{};
    };

    struct ConnectTask {
        std::string host;
        std::string port;
        std::promise<void> promise;
    };

  public:
    explicit Impl(TcpClientOptions options)
        : options_(options), decoder_(options.max_body_bytes, options.max_header_bytes) {
        if (options_.connect_timeout.count() <= 0 || options_.request_timeout.count() <= 0 ||
            options_.connect_timeout > std::chrono::hours(24) ||
            options_.request_timeout > std::chrono::hours(24) ||
            options_.max_pending_requests == 0 || options_.max_queued_write_bytes == 0 ||
            options_.max_buffered_response_bytes == 0 || options_.max_body_bytes == 0 ||
            options_.max_body_bytes > protocol::Dispatcher::max_body_bytes ||
            options_.max_header_bytes < 4 || options_.max_notifications == 0 ||
            options_.max_notification_bytes < resync_notification.size()) {
            throw std::invalid_argument("Invalid TCP client resource limits");
        }
        check_uv(uv_loop_init(&loop_), "Initialize TCP client loop");
        try {
            check_uv(uv_async_init(&loop_, &control_, control_callback),
                     "Initialize TCP client signal");
            control_initialized_ = true;
            control_.data = this;
            check_uv(uv_timer_init(&loop_, &timer_), "Initialize TCP client timer");
            timer_initialized_ = true;
            timer_.data = this;
            resolver_.data = this;
            worker_ = std::jthread([this] {
                uv_run(&loop_, UV_RUN_DEFAULT);
                loop_close_error_.store(uv_loop_close(&loop_));
                state_.store(State::closed);
            });
        } catch (...) {
            if (timer_initialized_) {
                uv_close(reinterpret_cast<uv_handle_t*>(&timer_), nullptr);
            }
            if (control_initialized_) {
                uv_close(reinterpret_cast<uv_handle_t*>(&control_), nullptr);
            }
            uv_run(&loop_, UV_RUN_DEFAULT);
            (void)uv_loop_close(&loop_);
            throw;
        }
    }

    ~Impl() {
        close();
    }

    std::future<void> connect(std::string host, std::uint16_t port) {
        auto task = std::make_unique<ConnectTask>();
        task->host = std::move(host);
        task->port = std::to_string(port);
        auto future = task->promise.get_future();
        const std::lock_guard lock(mutex_);
        if (state_.load() != State::idle || stop_requested_.load()) {
            task->promise.set_exception(
                std::make_exception_ptr(TransportError("TCP client connects only once")));
        } else if (task->host.empty() || task->host.size() > 1024 ||
                   task->host.find('\0') != std::string::npos || port == 0) {
            task->promise.set_exception(
                std::make_exception_ptr(TransportError("Invalid TCP endpoint")));
        } else {
            state_.store(State::connecting);
            connect_deadline_ = Clock::now() + options_.connect_timeout;
            queued_connect_ = std::move(task);
            (void)uv_async_send(&control_);
        }
        return future;
    }

    void close() noexcept {
        if (!worker_.joinable()) {
            return;
        }
        stop_requested_.store(true);
        {
            const std::lock_guard lock(mutex_);
            if (control_initialized_) {
                (void)uv_async_send(&control_);
            }
        }
        worker_.join();
    }

    bool connected() const noexcept {
        return state_.load() == State::connected && !stop_requested_.load();
    }

    TcpClientStats stats() const noexcept {
        const std::lock_guard lock(mutex_);
        return {pending_count_,
                queued_bytes_,
                notification_overflows_.load(),
                protocol_errors_.load(),
                loop_close_error_.load()};
    }

    std::future<std::optional<std::string>> submit(std::string body) {
        auto group = std::make_shared<Group>();
        auto future = group->promise.get_future();
        group->body = std::move(body);
        group->deadline = Clock::now() + options_.request_timeout;
        const std::lock_guard lock(mutex_);
        if (!connected()) {
            group->promise.set_exception(
                std::make_exception_ptr(TransportError("TCP client is not connected")));
        } else if (group->body.size() > options_.max_body_bytes) {
            group->promise.set_exception(
                std::make_exception_ptr(TransportError("TCP request exceeds body limit")));
        } else if (pending_count_ >= options_.max_pending_requests ||
                   group->body.size() + 64 > options_.max_queued_write_bytes - queued_bytes_) {
            group->promise.set_exception(
                std::make_exception_ptr(TransportError("TCP client request queue is full")));
        } else {
            group->reserved_bytes = group->body.size() + 64;
            queued_.push_back(group);
            ++pending_count_;
            queued_bytes_ += group->reserved_bytes;
            (void)uv_async_send(&control_);
        }
        return future;
    }

    std::vector<std::string> take_notifications() {
        const std::lock_guard lock(notification_mutex_);
        std::vector<std::string> result;
        result.swap(notifications_);
        notification_bytes_ = 0;
        notification_resync_ = false;
        return result;
    }

  private:
    static void control_callback(uv_async_t* handle) noexcept {
        auto& self = *static_cast<Impl*>(handle->data);
        try {
            self.on_control();
        } catch (...) {
            self.fail(std::current_exception());
        }
    }

    void on_control() {
        if (stop_requested_.load()) {
            shutdown();
            return;
        }
        std::unique_ptr<ConnectTask> connect_task;
        {
            const std::lock_guard lock(mutex_);
            connect_task = std::move(queued_connect_);
        }
        if (connect_task) {
            connect_task_ = std::move(connect_task);
            check_uv(uv_timer_start(&timer_, timer_callback, 1, 10), "Start TCP client deadlines");
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            hints.ai_protocol = IPPROTO_TCP;
            const int status = uv_getaddrinfo(&loop_,
                                              &resolver_,
                                              resolve_callback,
                                              connect_task_->host.c_str(),
                                              connect_task_->port.c_str(),
                                              &hints);
            check_uv(status, "Resolve TCP hostname");
            resolving_ = true;
        }
        for (std::size_t processed = 0; processed < 32 && !stop_requested_.load(); ++processed) {
            std::shared_ptr<Group> group;
            {
                const std::lock_guard lock(mutex_);
                if (queued_.empty()) {
                    break;
                }
                group = std::move(queued_.front());
                queued_.pop_front();
            }
            try {
                send_group(group);
            } catch (...) {
                finish(group, std::current_exception());
            }
        }
        const std::lock_guard lock(mutex_);
        if (!queued_.empty() || stop_requested_.load()) {
            (void)uv_async_send(&control_);
        }
    }

    static void resolve_callback(uv_getaddrinfo_t* request, int status, addrinfo* result) noexcept {
        auto& self = *static_cast<Impl*>(request->data);
        self.resolving_ = false;
        const std::unique_ptr<addrinfo, decltype(&uv_freeaddrinfo)> resolved(result,
                                                                             uv_freeaddrinfo);
        if (self.state_.load() != State::connecting || self.stop_requested_.load()) {
            return;
        }
        try {
            check_uv(status, "Resolve TCP hostname");
            for (const auto* entry = result; entry != nullptr; entry = entry->ai_next) {
                if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6) {
                    continue;
                }
                if (!loopback(entry->ai_addr)) {
                    throw TransportError(
                        "TCP client only permits resolved loopback addresses; use an SSH tunnel");
                }
                sockaddr_storage address{};
                const auto size =
                    entry->ai_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
                std::memcpy(&address, entry->ai_addr, size);
                self.addresses_.push_back(address);
            }
            self.connect_next();
        } catch (...) {
            self.fail(std::current_exception());
        }
    }

    void connect_next() {
        while (next_address_ < addresses_.size()) {
            auto connection = std::make_unique<Connection>();
            connection->owner = this;
            auto* raw = connection.get();
            connections_.emplace(raw, std::move(connection));
            const int initialized = uv_tcp_init(&loop_, &raw->socket);
            if (initialized < 0) {
                connections_.erase(raw);
                check_uv(initialized, "Initialize TCP socket");
            }
            raw->socket.data = raw;
            raw->request.data = raw;
            raw->connecting = true;
            current_ = raw;
            const auto& address = addresses_[next_address_++];
            const int status = uv_tcp_connect(&raw->request,
                                              &raw->socket,
                                              reinterpret_cast<const sockaddr*>(&address),
                                              connect_callback);
            if (status == 0) {
                return;
            }
            raw->connecting = false;
            last_connect_error_ = status;
            close_connection(raw);
        }
        throw TransportError(std::string("Connect TCP endpoint: ") +
                             uv_strerror(last_connect_error_));
    }

    static void connect_callback(uv_connect_t* request, int status) noexcept {
        auto* connection = static_cast<Connection*>(request->data);
        auto& self = *connection->owner;
        connection->connecting = false;
        try {
            if (self.state_.load() != State::connecting || self.stop_requested_.load()) {
                self.close_connection(connection);
            } else if (status < 0) {
                self.last_connect_error_ = status;
                self.close_connection(connection);
                self.connect_next();
            } else {
                check_uv(uv_tcp_nodelay(&connection->socket, 1), "Configure TCP client latency");
                check_uv(uv_read_start(reinterpret_cast<uv_stream_t*>(&connection->socket),
                                       allocate_callback,
                                       read_callback),
                         "Read TCP responses");
                self.state_.store(State::connected);
                self.connect_task_->promise.set_value();
                self.connect_task_.reset();
            }
        } catch (...) {
            self.fail(std::current_exception());
        }
        self.release_connection(connection);
    }

    void close_connection(Connection* connection) noexcept {
        if (current_ == connection) {
            current_ = nullptr;
        }
        if (!connection->closing) {
            connection->closing = true;
            (void)uv_read_stop(reinterpret_cast<uv_stream_t*>(&connection->socket));
            uv_close(reinterpret_cast<uv_handle_t*>(&connection->socket), connection_closed);
        }
    }

    static void connection_closed(uv_handle_t* handle) noexcept {
        auto* connection = static_cast<Connection*>(handle->data);
        connection->close_done = true;
        connection->owner->release_connection(connection);
    }

    void release_connection(Connection* connection) noexcept {
        if (connection->close_done && !connection->connecting && connection->writes == 0) {
            connections_.erase(connection);
        }
    }

    void validate_group(Group& group) {
        const auto request = parse_body(group.body);
        group.batch = request.is_array();
        if (group.batch &&
            (request.empty() || request.size() > protocol::Dispatcher::max_batch_size)) {
            throw TransportError("Invalid JSON-RPC request batch");
        }
        std::unordered_set<std::string> ids;
        const auto validate = [&](const Json& member) {
            if (!valid_version(member) || !member.contains("method") ||
                !member["method"].is_string()) {
                throw TransportError("Invalid JSON-RPC request envelope");
            }
            if (member.contains("id")) {
                if (!valid_id(member["id"])) {
                    throw TransportError("Invalid JSON-RPC request ID");
                }
                auto id = member["id"].dump();
                if (!ids.insert(id).second || correlations_.contains(id)) {
                    throw TransportError("Duplicate outstanding JSON-RPC request ID");
                }
                group.ids.push_back(std::move(id));
            }
        };
        if (group.batch) {
            for (const auto& member : request) {
                validate(member);
            }
        } else {
            validate(request);
        }
        group.remaining = group.ids.size();
        group.responses.resize(group.remaining);
    }

    void send_group(const std::shared_ptr<Group>& group) {
        if (stop_requested_.load()) {
            throw TransportCancelled("TCP client closed before request was written");
        }
        if (!connected() || current_ == nullptr) {
            throw TransportError("TCP client disconnected before request was written");
        }
        if (Clock::now() >= group->deadline) {
            throw TransportError("TCP request expired in the client queue before sending");
        }
        validate_group(*group);
        if (stop_requested_.load()) {
            throw TransportCancelled("TCP client closed before request was written");
        }
        groups_.emplace(group.get(), group);
        for (std::size_t index = 0; index < group->ids.size(); ++index) {
            correlations_.emplace(group->ids[index], std::pair{group.get(), index});
        }
        auto write = std::make_unique<Write>();
        write->owner = this;
        write->connection = current_;
        write->group = group;
        write->frame = protocol::encode_frame(group->body, options_.max_body_bytes);
        group->body.clear();
        group->body.shrink_to_fit();
        auto* raw = write.get();
        writes_.emplace(raw, std::move(write));
        raw->reserved_bytes = std::exchange(group->reserved_bytes, 0);
        raw->request.data = raw;
        ++raw->connection->writes;
        auto buffer = uv_buf_init(raw->frame.data(), static_cast<unsigned int>(raw->frame.size()));
        const int status = uv_write(&raw->request,
                                    reinterpret_cast<uv_stream_t*>(&current_->socket),
                                    &buffer,
                                    1,
                                    write_callback);
        if (status < 0) {
            release_write(raw);
            // Socket errors make every pending transaction outcome uncertain.
            fail(std::make_exception_ptr(
                TransportError(std::string("Write TCP request: ") + uv_strerror(status))));
        }
    }

    static void write_callback(uv_write_t* request, int status) noexcept {
        auto* write = static_cast<Write*>(request->data);
        auto& self = *write->owner;
        const auto group = write->group;
        self.release_write(write);
        if (status < 0) {
            self.fail(std::make_exception_ptr(
                TransportError(std::string("Write TCP request: ") + uv_strerror(status))));
        } else if (!group->done && group->ids.empty()) {
            self.finish(group, nullptr, std::nullopt);
        }
    }

    void release_write(Write* write) noexcept {
        auto* connection = write->connection;
        {
            const std::lock_guard lock(mutex_);
            queued_bytes_ -= write->reserved_bytes;
        }
        --connection->writes;
        writes_.erase(write);
        release_connection(connection);
    }

    static void allocate_callback(uv_handle_t* handle, std::size_t, uv_buf_t* buffer) noexcept {
        auto& self = *static_cast<Connection*>(handle->data)->owner;
        *buffer = uv_buf_init(self.read_buffer_.data(),
                              static_cast<unsigned int>(self.read_buffer_.size()));
    }

    static void read_callback(uv_stream_t* stream, ssize_t count, const uv_buf_t* buffer) noexcept {
        auto& self = *static_cast<Connection*>(stream->data)->owner;
        if (self.state_.load() != State::connected || count == 0) {
            return;
        }
        try {
            if (count < 0) {
                self.decoder_.finish();
                throw TransportError(std::string("TCP peer disconnected: ") +
                                     uv_strerror(static_cast<int>(count)));
            }
            const auto frames =
                self.decoder_.feed(std::string_view(buffer->base, static_cast<std::size_t>(count)));
            for (const auto& frame : frames) {
                self.receive_body(frame);
            }
        } catch (...) {
            if (count > 0 || self.decoder_.failed() || !self.decoder_.idle()) {
                ++self.protocol_errors_;
            }
            self.fail(std::current_exception());
        }
    }

    void receive_body(std::string_view body) {
        const auto message = parse_body(body);
        if (message.is_array()) {
            if (message.empty() || message.size() > protocol::Dispatcher::max_batch_size) {
                throw TransportError("Invalid JSON-RPC response batch");
            }
            for (const auto& member : message) {
                receive_message(member);
            }
        } else {
            receive_message(message);
        }
    }

    void receive_message(const Json& message) {
        if (!valid_version(message)) {
            throw TransportError("Invalid JSON-RPC response envelope");
        }
        if (message.contains("method")) {
            if (!message["method"].is_string() || message.contains("id") ||
                message.contains("result") || message.contains("error") ||
                (message.contains("params") && !message["params"].is_object() &&
                 !message["params"].is_array())) {
                throw TransportError("Invalid JSON-RPC server notification");
            }
            push_notification(message.dump());
            return;
        }
        if (!message.contains("id") || !valid_id(message["id"]) ||
            message.contains("result") == message.contains("error")) {
            throw TransportError("Invalid JSON-RPC response fields");
        }
        if (message.contains("error")) {
            const auto& error = message["error"];
            if (!error.is_object() || !error.contains("code") ||
                !error["code"].is_number_integer() || !error.contains("message") ||
                !error["message"].is_string()) {
                throw TransportError("Invalid JSON-RPC error response");
            }
        }
        const auto found = correlations_.find(message["id"].dump());
        if (found == correlations_.end()) {
            throw TransportError("Unrecognized or duplicate JSON-RPC response ID");
        }
        const auto group = groups_.at(found->second.first);
        const auto index = found->second.second;
        if (group->responses[index]) {
            throw TransportError("Duplicate JSON-RPC response ID");
        }
        auto response = message.dump();
        const auto envelope_bytes = group->batch ? group->ids.size() + 1 : 0;
        if (response.size() + envelope_bytes > options_.max_body_bytes - group->received_bytes ||
            response.size() > options_.max_buffered_response_bytes - buffered_response_bytes_) {
            throw TransportError("TCP response aggregation exceeds client limit");
        }
        group->received_bytes += response.size();
        buffered_response_bytes_ += response.size();
        group->responses[index] = std::move(response);
        if (--group->remaining != 0) {
            return;
        }
        std::string result;
        result.reserve(group->received_bytes + envelope_bytes);
        if (group->batch) {
            result.push_back('[');
        }
        for (std::size_t item = 0; item < group->responses.size(); ++item) {
            if (item != 0) {
                result.push_back(',');
            }
            result += *group->responses[item];
        }
        if (group->batch) {
            result.push_back(']');
        }
        finish(group, nullptr, std::move(result));
    }

    void push_notification(std::string notification) {
        const std::lock_guard lock(notification_mutex_);
        if (notification_resync_) {
            return;
        }
        if (notifications_.size() >= options_.max_notifications ||
            notification.size() > options_.max_notification_bytes - notification_bytes_) {
            notifications_.clear();
            notifications_.emplace_back(resync_notification);
            notification_bytes_ = resync_notification.size();
            notification_resync_ = true;
            ++notification_overflows_;
        } else {
            notification_bytes_ += notification.size();
            notifications_.push_back(std::move(notification));
        }
    }

    void finish(const std::shared_ptr<Group>& group,
                std::exception_ptr error,
                std::optional<std::string> result = std::nullopt) noexcept {
        if (group->done) {
            return;
        }
        group->done = true;
        for (const auto& id : group->ids) {
            const auto found = correlations_.find(id);
            if (found != correlations_.end() && found->second.first == group.get()) {
                correlations_.erase(found);
            }
        }
        buffered_response_bytes_ -= group->received_bytes;
        groups_.erase(group.get());
        {
            const std::lock_guard lock(mutex_);
            --pending_count_;
            queued_bytes_ -= std::exchange(group->reserved_bytes, 0);
        }
        if (error) {
            group->promise.set_exception(error);
        } else {
            group->promise.set_value(std::move(result));
        }
    }

    static void timer_callback(uv_timer_t* timer) noexcept {
        auto& self = *static_cast<Impl*>(timer->data);
        const auto now = Clock::now();
        if (self.state_.load() == State::connecting && now >= self.connect_deadline_) {
            self.fail(std::make_exception_ptr(TransportError("TCP connect deadline expired")));
            return;
        }
        for (const auto& [pointer, group] : self.groups_) {
            (void)pointer;
            if (now >= group->deadline) {
                self.fail(std::make_exception_ptr(
                    TransportError("TCP request deadline expired; resynchronize before retry")));
                return;
            }
        }
    }

    void fail(std::exception_ptr error) noexcept {
        if (state_.load() == State::closed) {
            return;
        }
        state_.store(stop_requested_.load() ? State::closing : State::failed);
        std::deque<std::shared_ptr<Group>> queued;
        std::unique_ptr<ConnectTask> queued_connect;
        {
            const std::lock_guard lock(mutex_);
            queued.swap(queued_);
            queued_connect = std::move(queued_connect_);
        }
        for (const auto& group : queued) {
            finish(group, error);
        }
        while (!groups_.empty()) {
            const auto group = groups_.begin()->second;
            finish(group, error);
        }
        if (queued_connect) {
            queued_connect->promise.set_exception(error);
        }
        if (connect_task_) {
            connect_task_->promise.set_exception(error);
            connect_task_.reset();
        }
        if (resolving_) {
            (void)uv_cancel(reinterpret_cast<uv_req_t*>(&resolver_));
        }
        for (const auto& [pointer, connection] : connections_) {
            (void)pointer;
            close_connection(connection.get());
        }
        if (timer_initialized_) {
            (void)uv_timer_stop(&timer_);
        }
    }

    void shutdown() noexcept {
        fail(std::make_exception_ptr(TransportCancelled("TCP client closed")));
        if (timer_initialized_) {
            uv_close(reinterpret_cast<uv_handle_t*>(&timer_), nullptr);
            timer_initialized_ = false;
        }
        const std::lock_guard lock(mutex_);
        if (control_initialized_) {
            uv_close(reinterpret_cast<uv_handle_t*>(&control_), nullptr);
            control_initialized_ = false;
        }
    }

    TcpClientOptions options_;
    protocol::FrameDecoder decoder_;
    uv_loop_t loop_{};
    uv_async_t control_{};
    uv_timer_t timer_{};
    uv_getaddrinfo_t resolver_{};
    std::jthread worker_;
    std::atomic<State> state_{State::idle};
    std::atomic<bool> stop_requested_{};
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<Group>> queued_;
    std::unique_ptr<ConnectTask> queued_connect_;
    std::size_t pending_count_{};
    std::size_t queued_bytes_{};
    bool control_initialized_{};
    bool timer_initialized_{};
    bool resolving_{};
    std::unique_ptr<ConnectTask> connect_task_;
    Clock::time_point connect_deadline_;
    std::vector<sockaddr_storage> addresses_;
    std::size_t next_address_{};
    int last_connect_error_{UV_ECONNREFUSED};
    Connection* current_{}; // Borrowed from connections_.
    std::unordered_map<Connection*, std::unique_ptr<Connection>> connections_;
    std::unordered_map<Write*, std::unique_ptr<Write>> writes_;
    std::unordered_map<Group*, std::shared_ptr<Group>> groups_;
    std::unordered_map<std::string, std::pair<Group*, std::size_t>> correlations_;
    std::size_t buffered_response_bytes_{};
    std::array<char, 65536> read_buffer_{};
    std::mutex notification_mutex_;
    std::vector<std::string> notifications_;
    std::size_t notification_bytes_{};
    bool notification_resync_{};
    std::atomic<std::uint64_t> notification_overflows_{};
    std::atomic<std::uint64_t> protocol_errors_{};
    std::atomic<int> loop_close_error_{};
};

TcpClient::TcpClient(TcpClientOptions options) : impl_(std::make_unique<Impl>(options)) {
}

TcpClient::~TcpClient() = default;

std::future<void> TcpClient::connect(std::string host, std::uint16_t port) {
    return impl_->connect(std::move(host), port);
}

void TcpClient::close() noexcept {
    impl_->close();
}

bool TcpClient::connected() const noexcept {
    return impl_->connected();
}

TcpClientStats TcpClient::stats() const noexcept {
    return impl_->stats();
}

std::future<std::optional<std::string>> TcpClient::submit(std::string body) {
    return impl_->submit(std::move(body));
}

std::vector<std::string> TcpClient::take_notifications() {
    return impl_->take_notifications();
}

} // namespace astra::transport
