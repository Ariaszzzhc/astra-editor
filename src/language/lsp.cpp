#include <astra/language/lsp.hpp>

#include <astra/core/utf8.hpp>
#include <astra/process/process.hpp>
#include <astra/protocol/framing.hpp>
#include <astra/transport/tcp_server.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <functional>
#include <initializer_list>
#include <limits>
#include <mutex>
#include <ostream>
#include <streambuf>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace astra::language {
namespace {

using transport::TransportCancelled;
using transport::TransportError;
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_lsp_integer = 2147483647;

bool letter(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

void require_uri(std::string_view uri) {
    (void)file_uri_to_path(uri);
}

Json position_json(TextPosition position) {
    if (position.line > max_lsp_integer || position.utf16_column > max_lsp_integer) {
        throw std::out_of_range("LSP position exceeds signed 32-bit range");
    }
    return Json{{"line", position.line}, {"character", position.utf16_column}};
}

class LimitedBuffer final : public std::streambuf {
  public:
    explicit LimitedBuffer(std::size_t limit) : limit_(limit) {
    }

    std::string value;

  private:
    std::streamsize xsputn(const char* bytes, std::streamsize count) override {
        if (count <= 0) {
            return count;
        }
        const auto size = static_cast<std::size_t>(count);
        if (size > limit_ - value.size()) {
            throw TransportError("LSP message exceeds body limit");
        }
        value.append(bytes, size);
        return count;
    }

    int_type overflow(int_type character) override {
        if (!traits_type::eq_int_type(character, traits_type::eof())) {
            const auto byte = traits_type::to_char_type(character);
            (void)xsputn(&byte, 1);
        }
        return traits_type::not_eof(character);
    }

    std::size_t limit_;
};

void require_depth(const Json& value, std::size_t depth = 0) {
    if (depth > 64) {
        throw TransportError("LSP JSON exceeds nesting limit");
    }
    if (value.is_structured()) {
        for (const auto& child : value) {
            require_depth(child, depth + 1);
        }
    }
}

std::string serialize(const Json& value, std::size_t limit) {
    require_depth(value);
    LimitedBuffer buffer(limit);
    std::ostream stream(&buffer);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream << value;
    return std::move(buffer.value);
}

Json parse(std::string_view body) {
    return Json::parse(body, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 64) {
            throw TransportError("LSP JSON exceeds nesting limit");
        }
        return true;
    });
}

std::string request_id(std::uint64_t id) {
    return "astra/" + std::to_string(id);
}

std::optional<std::uint64_t> parse_request_id(const Json& value) {
    if (!value.is_string()) {
        return std::nullopt;
    }
    const auto& string = value.get_ref<const std::string&>();
    if (!string.starts_with("astra/") || string.size() <= 6) {
        return std::nullopt;
    }
    std::uint64_t id{};
    const auto [end, error] = std::from_chars(string.data() + 6, string.data() + string.size(), id);
    if (error != std::errc{} || end != string.data() + string.size() || id == 0 ||
        request_id(id) != string) {
        return std::nullopt;
    }
    return id;
}

std::int32_t signed_integer(const Json& value) {
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<std::uint64_t>() >
             static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))) {
        throw TransportError("LSP integer is outside signed 32-bit range");
    }
    const auto number = value.get<std::int64_t>();
    if (number < std::numeric_limits<std::int32_t>::min() ||
        number > std::numeric_limits<std::int32_t>::max()) {
        throw TransportError("LSP integer is outside signed 32-bit range");
    }
    return static_cast<std::int32_t>(number);
}

} // namespace

std::string path_to_file_uri(const std::filesystem::path& path) {
    const auto utf = path.generic_u8string();
    std::string text(reinterpret_cast<const char*>(utf.data()), utf.size());
    utf8::validate(text);
    if (text.empty() || text.find('\0') != std::string::npos) {
        throw std::invalid_argument("Empty or NUL-containing file path");
    }
    std::replace(text.begin(), text.end(), '\\', '/');
    if (text.starts_with("//?/UNC/")) {
        text = "//" + text.substr(8);
    } else if (text.starts_with("//?/")) {
        text.erase(0, 4);
    }
    if (text.starts_with("//./")) {
        throw std::invalid_argument("Device paths are not file URIs");
    }
    std::string prefix;
    if (text.size() >= 3 && letter(text[0]) && text[1] == ':' && text[2] == '/') {
        prefix = "file:///";
    } else if (text.starts_with("//")) {
        const auto slash = text.find('/', 2);
        if (slash == std::string::npos || slash == 2 || slash + 1 == text.size() ||
            text.substr(2, slash - 2).find_first_of(":@?#%") != std::string::npos) {
            throw std::invalid_argument("Invalid UNC file path");
        }
        prefix = "file:";
    } else if (text.starts_with('/')) {
#if defined(_WIN32)
        throw std::invalid_argument("Windows file paths require an absolute drive or UNC share");
#else
        prefix = "file://";
#endif
    } else {
        throw std::invalid_argument("File URI requires an absolute path");
    }
    constexpr char digits[] = "0123456789ABCDEF";
    for (const unsigned char byte : text) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~' || byte == '/' || byte == ':') {
            prefix.push_back(static_cast<char>(byte));
        } else {
            prefix.push_back('%');
            prefix.push_back(digits[byte >> 4U]);
            prefix.push_back(digits[byte & 15U]);
        }
    }
    return prefix;
}

std::filesystem::path file_uri_to_path(std::string_view uri) {
    if (!uri.starts_with("file://") || uri.find_first_of("?#") != std::string_view::npos) {
        throw std::invalid_argument("Expected an absolute file URI without query or fragment");
    }
    uri.remove_prefix(7);
    const auto slash = uri.find('/');
    if (slash == std::string_view::npos) {
        throw std::invalid_argument("File URI has no absolute path");
    }
    const auto authority = uri.substr(0, slash);
    if (authority.find_first_of(":@\\") != std::string_view::npos) {
        throw std::invalid_argument("Invalid file URI authority");
    }
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f') {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F') {
            return value - 'A' + 10;
        }
        return -1;
    };
    const auto decode = [&digit](std::string_view encoded) {
        std::string decoded;
        for (std::size_t index = 0; index < encoded.size(); ++index) {
            if (encoded[index] == '%') {
                if (index + 2 >= encoded.size() || digit(encoded[index + 1]) < 0 ||
                    digit(encoded[index + 2]) < 0) {
                    throw std::invalid_argument("Malformed file URI escape");
                }
                decoded.push_back(
                    static_cast<char>(digit(encoded[index + 1]) * 16 + digit(encoded[index + 2])));
                index += 2;
            } else {
                decoded.push_back(encoded[index]);
            }
        }
        utf8::validate(decoded);
        if (decoded.find('\0') != std::string::npos) {
            throw std::invalid_argument("NUL in file URI");
        }
        return decoded;
    };
    const auto host = decode(authority);
    if (host.find_first_of("/:@%\\?#") != std::string::npos) {
        throw std::invalid_argument("Invalid decoded file URI authority");
    }
    auto canonical_host = host;
    std::transform(
        canonical_host.begin(), canonical_host.end(), canonical_host.begin(), [](char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        });
    std::string decoded;
    if (!host.empty() && canonical_host != "localhost") {
        decoded = "//" + host;
    }
    decoded += decode(uri.substr(slash));
    if (decoded.find('\\') != std::string::npos) {
        throw std::invalid_argument("Invalid file URI path bytes");
    }
    if (decoded.size() >= 4 && decoded[0] == '/' && letter(decoded[1]) && decoded[2] == ':' &&
        decoded[3] == '/') {
        decoded.erase(0, 1);
    }
    const std::u8string utf(reinterpret_cast<const char8_t*>(decoded.data()), decoded.size());
    const std::filesystem::path path(utf);
    // Revalidate absolute/UNC/device semantics without accessing the file system.
    (void)path_to_file_uri(path);
    return path;
}

bool file_uri_equal(std::string_view left, std::string_view right) {
    const auto a = file_uri_to_path(left).lexically_normal().native();
    const auto b = file_uri_to_path(right).lexically_normal().native();
#if defined(_WIN32)
    if (a.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        b.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    return CompareStringOrdinal(
               a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
#else
    return a == b;
#endif
}

Json lsp_position(const Rope& text, std::size_t byte_offset) {
    auto position = text.byte_to_position(byte_offset);
    if (byte_offset > text.line_end(position.line)) {
        position = text.byte_to_position(text.line_end(position.line));
    }
    return position_json(position);
}

std::size_t lsp_byte_offset(const Rope& text, TextPosition position) {
    (void)position_json(position);
    const auto offset = text.position_to_byte(position);
    if (offset > text.line_end(position.line)) {
        throw std::out_of_range("LSP position extends beyond line content");
    }
    return offset;
}

class LspClient::Impl {
    enum class State { idle, initializing, running, stopping, exited, failed };
    enum class Purpose { request, initialize, shutdown };

    struct Pending {
        std::uint64_t id{};
        Purpose purpose{};
        Clock::time_point deadline;
        std::shared_ptr<std::promise<Json>> promise;
    };

    struct Document {
        std::int32_t version{};
        Rope text;
    };

  public:
    explicit Impl(LspOptions options)
        : options_(std::move(options)), decoder_(options_.max_body_bytes) {
        if (options_.argv.empty() || options_.argv[0].empty() ||
            options_.max_pending_requests == 0 || options_.max_body_bytes == 0 ||
            options_.max_body_bytes > protocol::Dispatcher::max_body_bytes ||
            options_.max_queued_write_bytes == 0 || options_.max_stderr_bytes == 0 ||
            options_.max_notifications == 0 || options_.max_notification_bytes < 256 ||
            options_.max_open_documents == 0 || options_.initialize_timeout.count() <= 0 ||
            options_.max_pending_requests > std::numeric_limits<std::size_t>::max() / 4 ||
            options_.request_timeout.count() <= 0 || options_.shutdown_timeout.count() <= 0 ||
            options_.initialize_timeout > std::chrono::hours(24) ||
            options_.request_timeout > std::chrono::hours(24) ||
            options_.shutdown_timeout > std::chrono::hours(24)) {
            throw std::invalid_argument("Invalid LSP process options or resource limits");
        }
        process::ProcessOptions process_options;
        process_options.argv = options_.argv;
        process_options.cwd = options_.working_directory;
        process_options.env = options_.environment;
        process_options.max_queued_stdin_bytes = options_.max_queued_write_bytes;
        process_options.max_queued_tasks = options_.max_pending_requests * 4;
        process_options.max_stdout_bytes = 0;
        process_options.max_stderr_bytes = 0;
        process_options.shutdown_timeout = options_.shutdown_timeout;
        process::ProcessCallbacks callbacks;
        callbacks.on_started = [this] {
            on_started();
        };
        callbacks.on_stdout = [this](std::string_view bytes) {
            on_stdout(bytes);
        };
        callbacks.on_stderr = [this](std::string_view bytes) {
            on_stderr(bytes);
        };
        callbacks.on_error = [this](std::exception_ptr error) {
            fail(error);
        };
        callbacks.on_exit = [this](const process::ProcessExit& result) {
            on_exit(result);
        };
        callbacks.on_tick = [this] {
            on_tick();
        };
        process_ =
            std::make_unique<process::Process>(std::move(process_options), std::move(callbacks));
    }

    ~Impl() {
        close();
        process_.reset();
    }

    std::future<Json> start(std::string root_uri, Json initialization_options) {
        auto promise = std::make_shared<std::promise<Json>>();
        auto future = promise->get_future();
        const std::lock_guard lock(lifecycle_mutex_);
        bool starting = false;
        try {
            if (state_.load() != State::idle || closed_) {
                throw TransportError("LSP client starts once");
            }
            if (!root_uri.empty()) {
                require_uri(root_uri);
            }
            root_uri_ = std::move(root_uri);
            initialization_options_ = std::move(initialization_options);
            initialize_promise_ = promise;
            state_.store(State::initializing);
            starting = true;
            (void)process_->start();
            started_ = true;
        } catch (...) {
            if (starting) {
                initialize_promise_.reset();
                state_.store(State::failed);
            }
            promise->set_exception(std::current_exception());
        }
        return future;
    }

    bool ready() const noexcept {
        return state_.load() == State::running;
    }

    Json capabilities() const {
        const std::lock_guard lock(output_mutex_);
        return capabilities_;
    }

    LspRequest request(std::string method,
                       Json params,
                       std::optional<std::pair<std::string, TextPosition>> position = {}) {
        auto promise = std::make_shared<std::promise<Json>>();
        LspRequest result{next_id_.fetch_add(1), promise->get_future()};
        std::size_t queued_bytes{};
        try {
            if (method.size() > options_.max_body_bytes) {
                throw TransportError("LSP method exceeds body limit");
            }
            queued_bytes = serialize(params, options_.max_body_bytes).size() + method.size() + 128;
        } catch (...) {
            promise->set_exception(std::current_exception());
            return result;
        }
        const auto deadline = Clock::now() + options_.request_timeout;
        dispatch(
            [this,
             promise,
             id = result.id,
             method = std::move(method),
             params = std::move(params),
             position = std::move(position),
             deadline]() mutable {
                try {
                    require_running();
                    if (method.empty() || method == "initialize" || method == "shutdown" ||
                        method == "exit" || (!params.is_object() && !params.is_array())) {
                        throw std::invalid_argument(
                            "Invalid or reserved LSP request method/params");
                    }
                    if (position) {
                        const auto& document = open_document(position->first);
                        (void)lsp_byte_offset(document.text, position->second);
                    }
                    send_request(id,
                                 std::move(method),
                                 std::move(params),
                                 promise,
                                 Purpose::request,
                                 deadline);
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            },
            [promise](std::exception_ptr error) {
                promise->set_exception(error);
            },
            queued_bytes);
        return result;
    }

    void cancel(std::uint64_t id) {
        dispatch(
            [this, id] {
                const auto found = pending_.find(id);
                if (found == pending_.end() || found->second.purpose != Purpose::request) {
                    return;
                }
                auto promise = found->second.promise;
                pending_.erase(found);
                promise->set_exception(
                    std::make_exception_ptr(TransportCancelled("LSP request cancelled")));
                send_notification("$/cancelRequest", {{"id", request_id(id)}});
            },
            [](std::exception_ptr) {
            });
    }

    std::future<void>
    did_open(std::string uri, std::string language, std::int32_t version, std::string text) {
        const auto bytes = input_budget({uri.size(), language.size(), text.size(), 128});
        return task(
            [this,
             uri = std::move(uri),
             language = std::move(language),
             version,
             text = std::move(text)] {
                require_running();
                require_uri(uri);
                if (!open_close_) {
                    throw TransportError("Language server did not enable document open/close");
                }
                if (language.empty() || documents_.contains(uri) ||
                    documents_.size() >= options_.max_open_documents) {
                    throw std::invalid_argument(
                        "LSP document is already open or document limit/language is invalid");
                }
                Document document{version, Rope(text)};
                send_notification("textDocument/didOpen",
                                  {{"textDocument",
                                    {{"uri", uri},
                                     {"languageId", language},
                                     {"version", version},
                                     {"text", text}}}});
                documents_.emplace(uri, std::move(document));
            },
            bytes);
    }

    std::future<void> did_change(std::string uri,
                                 std::int32_t version,
                                 std::vector<LspContentChange> changes,
                                 LspChangeOptions options) {
        auto bytes = input_budget({uri.size(), 128});
        for (const auto& change : changes) {
            bytes = input_budget({bytes, change.text.size(), 128});
        }
        return task(
            [this, uri = std::move(uri), version, changes = std::move(changes), options] {
                require_running();
                auto& document = open_document(uri);
                if (version <= document.version || changes.empty()) {
                    throw std::invalid_argument(
                        "LSP changes require a newer version and at least one change");
                }
                auto updated = document.text;
                Json content = Json::array();
                for (const auto& change : changes) {
                    Json item{{"text", change.text}};
                    if (change.range) {
                        const auto begin = lsp_byte_offset(updated, change.range->start);
                        const auto end = lsp_byte_offset(updated, change.range->end);
                        if (begin > end) {
                            throw std::invalid_argument("LSP edit range is reversed");
                        }
                        updated = updated.replace(begin, end, change.text);
                        item["range"] = {{"start", position_json(change.range->start)},
                                         {"end", position_json(change.range->end)}};
                    } else {
                        updated = Rope(change.text);
                    }
                    content.push_back(std::move(item));
                }
                if (sync_kind_ == 0) {
                    throw TransportError("Language server did not enable document changes");
                }
                if (sync_kind_ == 1) {
                    content = Json::array({Json{{"text", updated.str()}}});
                }
                Json params{{"textDocument", {{"uri", uri}, {"version", version}}},
                            {"contentChanges", std::move(content)}};
                if (options.clangd_force_rebuild) {
                    params["forceRebuild"] = true;
                    params["wantDiagnostics"] = true;
                }
                send_notification("textDocument/didChange", std::move(params));
                document = Document{version, std::move(updated)};
            },
            bytes);
    }

    std::future<void> did_close(std::string uri) {
        const auto bytes = input_budget({uri.size(), 128});
        return task(
            [this, uri = std::move(uri)] {
                require_running();
                (void)open_document(uri);
                send_notification("textDocument/didClose", {{"textDocument", {{"uri", uri}}}});
                documents_.erase(uri);
                const std::lock_guard lock(output_mutex_);
                diagnostics_.erase(uri);
                const auto size = diagnostic_sizes_.find(uri);
                if (size != diagnostic_sizes_.end()) {
                    diagnostic_bytes_ -= size->second;
                    diagnostic_sizes_.erase(size);
                }
            },
            bytes);
    }

    LspRequest at_position(std::string method,
                           std::string uri,
                           TextPosition position,
                           Json extras = Json::object()) {
        extras["textDocument"] = {{"uri", uri}};
        extras["position"] = position_json(position);
        return request(std::move(method), std::move(extras), std::pair{std::move(uri), position});
    }

    std::vector<Json> take_notifications() {
        const std::lock_guard lock(output_mutex_);
        std::vector<Json> result;
        result.swap(notifications_);
        notification_bytes_ = 0;
        notification_resync_ = false;
        return result;
    }

    std::vector<LspDiagnostics> take_diagnostics() {
        const std::lock_guard lock(output_mutex_);
        std::vector<LspDiagnostics> result;
        result.reserve(diagnostics_.size());
        for (auto& [uri, diagnostic] : diagnostics_) {
            (void)uri;
            result.push_back(std::move(diagnostic));
        }
        diagnostics_.clear();
        diagnostic_sizes_.clear();
        diagnostic_bytes_ = 0;
        return result;
    }

    std::string stderr_text() const {
        const std::lock_guard lock(output_mutex_);
        return stderr_;
    }

    bool stderr_truncated() const noexcept {
        return stderr_truncated_.load();
    }

    std::future<void> shutdown() {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        dispatch(
            [this, promise] {
                try {
                    require_running();
                    if (!pending_.empty()) {
                        const auto error = std::make_exception_ptr(
                            TransportCancelled("LSP shutdown cancels pending requests"));
                        auto cancelled = std::exchange(pending_, {});
                        for (const auto& [id, pending] : cancelled) {
                            (void)id;
                            pending.promise->set_exception(error);
                        }
                        for (const auto& [id, pending] : cancelled) {
                            (void)pending;
                            send_notification("$/cancelRequest", {{"id", request_id(id)}});
                        }
                    }
                    auto internal = std::make_shared<std::promise<Json>>();
                    send_request(next_id_.fetch_add(1),
                                 "shutdown",
                                 nullptr,
                                 internal,
                                 Purpose::shutdown,
                                 Clock::now() + options_.shutdown_timeout);
                    shutdown_promise_ = promise;
                    state_.store(State::stopping);
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            },
            [promise](std::exception_ptr error) {
                promise->set_exception(error);
            });
        return future;
    }

    void close() noexcept {
        const std::lock_guard lock(lifecycle_mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
        if (!started_) {
            return;
        }
        process_->close();
        process_->completion().wait();
    }

  private:
    std::size_t input_budget(std::initializer_list<std::size_t> sizes) const {
        std::size_t bytes{};
        for (const auto size : sizes) {
            if (size > options_.max_queued_write_bytes - bytes) {
                throw TransportError("LSP queued command exceeds memory limit");
            }
            bytes += size;
        }
        return bytes;
    }

    void dispatch(std::function<void()> function,
                  std::function<void(std::exception_ptr)> reject,
                  std::size_t bytes = 128) {
        const std::lock_guard lock(lifecycle_mutex_);
        if (closed_ || !started_) {
            reject(std::make_exception_ptr(TransportCancelled("LSP process is not running")));
            return;
        }
        if (bytes > options_.max_queued_write_bytes - queued_task_bytes_.load()) {
            reject(
                std::make_exception_ptr(TransportError("LSP queued commands exceed memory limit")));
            return;
        }
        auto released = std::make_shared<std::atomic<bool>>(false);
        queued_task_bytes_.fetch_add(bytes);
        const auto release = [this, released, bytes] {
            if (!released->exchange(true)) {
                queued_task_bytes_.fetch_sub(bytes);
            }
        };
        try {
            (void)process_->post(
                [function = std::move(function), release] {
                    release();
                    function();
                },
                [reject, release](std::exception_ptr error) {
                    release();
                    reject(error);
                });
        } catch (...) {
            release();
            reject(std::current_exception());
        }
    }

    std::future<void> task(std::function<void()> function, std::size_t bytes = 128) {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        dispatch(
            [promise, function = std::move(function)] {
                try {
                    function();
                    promise->set_value();
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
            },
            [promise](std::exception_ptr error) {
                promise->set_exception(error);
            },
            bytes);
        return future;
    }

    void require_running() const {
        if (!ready()) {
            throw TransportError("Language server is not initialized or is shutting down");
        }
    }

    Document& open_document(const std::string& uri) {
        const auto found = documents_.find(uri);
        if (found == documents_.end()) {
            throw std::invalid_argument("LSP document is not open");
        }
        return found->second;
    }

    void send(Json message) {
        process_->write_now(protocol::encode_frame(serialize(message, options_.max_body_bytes),
                                                   options_.max_body_bytes));
    }

    void send_notification(std::string method, Json params) {
        send(protocol::make_notification(std::move(method), std::move(params)));
    }

    void send_request(std::uint64_t id,
                      std::string method,
                      Json params,
                      const std::shared_ptr<std::promise<Json>>& promise,
                      Purpose purpose,
                      Clock::time_point deadline) {
        if (pending_.size() >= options_.max_pending_requests) {
            throw TransportError("LSP pending request limit reached");
        }
        if (Clock::now() >= deadline) {
            throw TransportError("LSP request expired before writing");
        }
        Pending pending{id, purpose, deadline, promise};
        pending_.emplace(id, std::move(pending));
        try {
            Json message{{"jsonrpc", "2.0"}, {"id", request_id(id)}, {"method", std::move(method)}};
            if (!params.is_null()) {
                message["params"] = std::move(params);
            }
            send(std::move(message));
        } catch (...) {
            pending_.erase(id);
            throw;
        }
    }

    void on_started() {
        const Json client_capabilities{
            {"general", {{"positionEncodings", Json::array({"utf-16"})}}},
            {"workspace",
             {{"applyEdit", false}, {"configuration", false}, {"workspaceFolders", false}}},
            {"textDocument",
             {{"synchronization", {{"dynamicRegistration", false}}},
              {"completion", {{"completionItem", {{"snippetSupport", false}}}}},
              {"hover", {{"contentFormat", Json::array({"plaintext", "markdown"})}}},
              {"definition", {{"linkSupport", true}}},
              {"references", Json::object()},
              {"rename", {{"prepareSupport", false}}},
              {"formatting", Json::object()},
              {"publishDiagnostics", {{"relatedInformation", true}, {"versionSupport", true}}}}}};
        Json params{{"processId", nullptr},
                    {"rootUri", root_uri_.empty() ? Json(nullptr) : Json(root_uri_)},
                    {"clientInfo", {{"name", "Astra"}, {"version", "0.1.0"}}},
                    {"capabilities", client_capabilities},
                    {"initializationOptions", initialization_options_}};
        send_request(next_id_.fetch_add(1),
                     "initialize",
                     std::move(params),
                     initialize_promise_,
                     Purpose::initialize,
                     Clock::now() + options_.initialize_timeout);
    }

    void on_stdout(std::string_view bytes) {
        for (const auto& body : decoder_.feed(bytes)) {
            const auto message = parse(body);
            if (!message.is_object() || !message.contains("jsonrpc") ||
                message["jsonrpc"] != "2.0") {
                throw TransportError("Invalid LSP JSON-RPC envelope");
            }
            if (message.contains("method")) {
                if (!message["method"].is_string() || message.contains("result") ||
                    message.contains("error") ||
                    (message.contains("params") && !message["params"].is_object() &&
                     !message["params"].is_array())) {
                    throw TransportError("Invalid LSP method or params");
                }
                if (message.contains("id")) {
                    const auto& id = message["id"];
                    if ((!id.is_number_integer() && !id.is_string()) ||
                        (id.is_string() && id.get_ref<const std::string&>().size() > 256)) {
                        throw TransportError("Invalid LSP server request ID");
                    }
                    send(Json{{"jsonrpc", "2.0"},
                              {"id", message["id"]},
                              {"error",
                               {{"code", -32601}, {"message", "Client method is not supported"}}}});
                } else {
                    on_notification(message);
                }
            } else {
                on_response(message);
            }
        }
    }

    void on_response(const Json& message) {
        if (!message.contains("id") || message.contains("error") == message.contains("result")) {
            throw TransportError("Invalid LSP response fields");
        }
        const auto id = parse_request_id(message["id"]);
        if (!id || *id >= next_id_.load()) {
            throw TransportError("Unknown LSP response ID");
        }
        const auto found = pending_.find(*id);
        if (found == pending_.end()) {
            return;
        } // Late result after explicit cancellation/deadline.
        const auto pending = found->second;
        pending_.erase(found);
        try {
            if (message.contains("error")) {
                const auto& error = message["error"];
                if (!error.is_object() || !error.contains("code") ||
                    !error["code"].is_number_integer() || !error.contains("message") ||
                    !error["message"].is_string()) {
                    throw TransportError("Malformed LSP error response");
                }
                throw protocol::RpcError(signed_integer(error["code"]),
                                         error["message"].get<std::string>(),
                                         error.contains("data") ? std::optional<Json>(error["data"])
                                                                : std::nullopt);
            }
            const auto& result = message["result"];
            if (pending.purpose == Purpose::initialize) {
                if (!result.is_object() || !result.contains("capabilities") ||
                    !result["capabilities"].is_object()) {
                    throw TransportError(
                        "Language server returned invalid initialize capabilities");
                }
                const auto& capabilities = result["capabilities"];
                if (capabilities.value("positionEncoding", std::string("utf-16")) != "utf-16") {
                    throw TransportError(
                        "Language server selected an unsupported position encoding");
                }
                if (capabilities.contains("textDocumentSync")) {
                    const auto& sync = capabilities["textDocumentSync"];
                    sync_kind_ = sync.is_number_integer()
                                     ? signed_integer(sync)
                                     : signed_integer(sync.value("change", Json(0)));
                    open_close_ =
                        sync.is_number_integer() ? sync_kind_ != 0 : sync.value("openClose", false);
                    if (sync_kind_ < 0 || sync_kind_ > 2) {
                        throw TransportError("Invalid LSP document sync kind");
                    }
                }
                {
                    const std::lock_guard lock(output_mutex_);
                    capabilities_ = capabilities;
                }
                send_notification("initialized", Json::object());
                state_.store(State::running);
                initialize_promise_.reset();
            } else if (pending.purpose == Purpose::shutdown) {
                send(Json{{"jsonrpc", "2.0"}, {"method", "exit"}});
                shutdown_acknowledged_ = true;
                process_->close();
            }
            pending.promise->set_value(result);
        } catch (...) {
            pending.promise->set_exception(std::current_exception());
            if (pending.purpose != Purpose::request) {
                if (pending.purpose == Purpose::initialize) {
                    initialize_promise_.reset();
                }
                fail(std::current_exception());
                process_->terminate();
            }
        }
    }

    void on_notification(const Json& notification) {
        const auto size = serialize(notification, options_.max_body_bytes).size();
        if (notification["method"] == "textDocument/publishDiagnostics") {
            if (!notification.contains("params")) {
                throw TransportError("LSP diagnostics has no params");
            }
            const auto& params = notification["params"];
            if (!params.is_object() || !params.contains("uri") || !params["uri"].is_string() ||
                !params.contains("diagnostics") || !params["diagnostics"].is_array()) {
                throw TransportError("Invalid LSP diagnostics");
            }
            LspDiagnostics diagnostic{params["uri"].get<std::string>(),
                                      std::nullopt,
                                      params["diagnostics"],
                                      std::nullopt};
            require_uri(diagnostic.uri);
            const auto opened =
                std::find_if(documents_.begin(), documents_.end(), [&](const auto& document) {
                    return file_uri_equal(document.first, diagnostic.uri);
                });
            if (opened != documents_.end()) {
                diagnostic.uri = opened->first;
                diagnostic.observed_version = opened->second.version;
            }
            if (params.contains("version") && !params["version"].is_null()) {
                if (!params["version"].is_number_integer()) {
                    throw TransportError("Invalid diagnostic version");
                }
                const auto version = signed_integer(params["version"]);
                diagnostic.version = version;
                if (opened != documents_.end() && version < opened->second.version) {
                    return;
                }
            }
            const std::lock_guard lock(output_mutex_);
            const auto previous = diagnostic_sizes_.find(diagnostic.uri);
            if (previous != diagnostic_sizes_.end()) {
                diagnostic_bytes_ -= previous->second;
                diagnostic_sizes_.erase(previous);
                diagnostics_.erase(diagnostic.uri);
            }
            if (diagnostics_.size() >= options_.max_open_documents ||
                size > options_.max_notification_bytes - diagnostic_bytes_) {
                diagnostics_.clear();
                diagnostic_sizes_.clear();
                diagnostic_bytes_ = 0;
                resync_locked();
            }
            if (size <= options_.max_notification_bytes) {
                diagnostic_bytes_ += size;
                diagnostic_sizes_[diagnostic.uri] = size;
                diagnostics_[diagnostic.uri] = std::move(diagnostic);
            }
        }
        const std::lock_guard lock(output_mutex_);
        if (notification_resync_) {
            return;
        }
        if (notifications_.size() >= options_.max_notifications ||
            size > options_.max_notification_bytes - notification_bytes_) {
            resync_locked();
        } else {
            notification_bytes_ += size;
            notifications_.push_back(notification);
        }
    }

    void resync_locked() {
        notifications_.clear();
        notifications_.push_back(protocol::make_notification("language.resyncRequired",
                                                             {{"reason", "notificationOverflow"}}));
        notification_bytes_ = notifications_.front().dump().size();
        notification_resync_ = true;
    }

    void on_stderr(std::string_view bytes) {
        const std::lock_guard lock(output_mutex_);
        if (bytes.size() >= options_.max_stderr_bytes) {
            stderr_.assign(bytes.substr(bytes.size() - options_.max_stderr_bytes));
            stderr_truncated_.store(true);
        } else {
            if (bytes.size() > options_.max_stderr_bytes - stderr_.size()) {
                stderr_.erase(0, bytes.size() - (options_.max_stderr_bytes - stderr_.size()));
                stderr_truncated_.store(true);
            }
            stderr_.append(bytes);
        }
    }

    void on_tick() {
        const auto now = Clock::now();
        std::vector<std::uint64_t> expired;
        for (const auto& [id, pending] : pending_) {
            if (now >= pending.deadline) {
                expired.push_back(id);
            }
        }
        for (const auto id : expired) {
            const auto found = pending_.find(id);
            if (found == pending_.end()) {
                continue;
            }
            const auto pending = found->second;
            pending_.erase(found);
            const auto error =
                std::make_exception_ptr(TransportError("LSP request deadline expired"));
            pending.promise->set_exception(error);
            if (pending.purpose == Purpose::request) {
                send_notification("$/cancelRequest", {{"id", request_id(id)}});
            } else {
                if (pending.purpose == Purpose::initialize) {
                    initialize_promise_.reset();
                }
                fail(error);
                process_->terminate();
                break;
            }
        }
    }

    void fail(std::exception_ptr error) {
        state_.store(State::failed);
        bool initialized_pending = false;
        for (const auto& [id, pending] : pending_) {
            (void)id;
            initialized_pending = initialized_pending || pending.purpose == Purpose::initialize;
            pending.promise->set_exception(error);
        }
        pending_.clear();
        if (initialize_promise_ && !initialized_pending) {
            initialize_promise_->set_exception(error);
        }
        initialize_promise_.reset();
        if (shutdown_promise_) {
            shutdown_promise_->set_exception(error);
            shutdown_promise_.reset();
        }
    }

    void on_exit(const process::ProcessExit& result) {
        std::exception_ptr error;
        try {
            decoder_.finish();
            if (!shutdown_acknowledged_ || result.exit_status != 0 || result.term_signal != 0 ||
                result.terminated || !result.error.empty()) {
                throw TransportError("Language server exited before clean shutdown: status=" +
                                     std::to_string(result.exit_status) +
                                     " signal=" + std::to_string(result.term_signal) +
                                     (result.error.empty() ? std::string{} : " " + result.error));
            }
        } catch (...) {
            error = std::current_exception();
        }
        if (error) {
            fail(error);
        } else {
            if (!pending_.empty()) {
                fail(std::make_exception_ptr(TransportCancelled("Language server exited")));
            }
            if (shutdown_promise_) {
                shutdown_promise_->set_value();
                shutdown_promise_.reset();
            }
            state_.store(State::exited);
        }
    }

    LspOptions options_;
    protocol::FrameDecoder decoder_;
    std::mutex lifecycle_mutex_;
    mutable std::mutex output_mutex_;
    std::atomic<State> state_{State::idle};
    std::atomic<std::uint64_t> next_id_{1};
    std::atomic<std::size_t> queued_task_bytes_{};
    bool started_{};
    bool closed_{};
    std::string root_uri_;
    Json initialization_options_;
    std::shared_ptr<std::promise<Json>> initialize_promise_;
    std::shared_ptr<std::promise<void>> shutdown_promise_;
    bool shutdown_acknowledged_{};
    int sync_kind_{};
    bool open_close_{};
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::unordered_map<std::string, Document> documents_;
    Json capabilities_ = Json::object();
    std::vector<Json> notifications_;
    std::size_t notification_bytes_{};
    bool notification_resync_{};
    std::unordered_map<std::string, LspDiagnostics> diagnostics_;
    std::unordered_map<std::string, std::size_t> diagnostic_sizes_;
    std::size_t diagnostic_bytes_{};
    std::string stderr_;
    std::atomic<bool> stderr_truncated_{};
    std::unique_ptr<process::Process> process_;
};

LspClient::LspClient(LspOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {
}

LspClient::~LspClient() = default;

std::future<Json> LspClient::start(std::string root_uri, Json initialization_options) {
    return impl_->start(std::move(root_uri), std::move(initialization_options));
}

bool LspClient::ready() const noexcept {
    return impl_->ready();
}

Json LspClient::capabilities() const {
    return impl_->capabilities();
}

LspRequest LspClient::request(std::string method, Json params) {
    return impl_->request(std::move(method), std::move(params));
}

void LspClient::cancel(std::uint64_t id) {
    impl_->cancel(id);
}

std::future<void>
LspClient::did_open(std::string uri, std::string language, std::int32_t version, std::string text) {
    return impl_->did_open(std::move(uri), std::move(language), version, std::move(text));
}

std::future<void> LspClient::did_change(std::string uri,
                                        std::int32_t version,
                                        std::vector<LspContentChange> changes,
                                        LspChangeOptions options) {
    return impl_->did_change(std::move(uri), version, std::move(changes), options);
}

std::future<void> LspClient::did_close(std::string uri) {
    return impl_->did_close(std::move(uri));
}

LspRequest LspClient::completion(std::string uri, TextPosition position) {
    return impl_->at_position("textDocument/completion", std::move(uri), position);
}

LspRequest LspClient::hover(std::string uri, TextPosition position) {
    return impl_->at_position("textDocument/hover", std::move(uri), position);
}

LspRequest LspClient::definition(std::string uri, TextPosition position) {
    return impl_->at_position("textDocument/definition", std::move(uri), position);
}

LspRequest LspClient::references(std::string uri, TextPosition position, bool include_declaration) {
    return impl_->at_position("textDocument/references",
                              std::move(uri),
                              position,
                              {{"context", {{"includeDeclaration", include_declaration}}}});
}

LspRequest LspClient::rename(std::string uri, TextPosition position, std::string new_name) {
    return impl_->at_position(
        "textDocument/rename", std::move(uri), position, {{"newName", std::move(new_name)}});
}

LspRequest LspClient::formatting(std::string uri, std::uint32_t tab_size, bool insert_spaces) {
    return impl_->request("textDocument/formatting",
                          {{"textDocument", {{"uri", std::move(uri)}}},
                           {"options", {{"tabSize", tab_size}, {"insertSpaces", insert_spaces}}}});
}

std::vector<Json> LspClient::take_notifications() {
    return impl_->take_notifications();
}

std::vector<LspDiagnostics> LspClient::take_diagnostics() {
    return impl_->take_diagnostics();
}

std::string LspClient::stderr_text() const {
    return impl_->stderr_text();
}

bool LspClient::stderr_truncated() const noexcept {
    return impl_->stderr_truncated();
}

std::future<void> LspClient::shutdown() {
    return impl_->shutdown();
}

void LspClient::close() noexcept {
    impl_->close();
}

} // namespace astra::language
