#pragma once

#include <astra/core/rope.hpp>
#include <astra/protocol/rpc.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace astra::language {

using Json = protocol::Json;

struct LspOptions {
    // UTF-8 executable and individual arguments, never a shell command string.
    std::vector<std::string> argv;
    std::string working_directory;
    std::optional<std::vector<std::string>> environment;
    std::chrono::milliseconds initialize_timeout{30000};
    std::chrono::milliseconds request_timeout{10000};
    std::chrono::milliseconds shutdown_timeout{3000};
    std::size_t max_pending_requests = 128;
    std::size_t max_body_bytes = 8 * 1024 * 1024;
    std::size_t max_queued_write_bytes = 16 * 1024 * 1024;
    std::size_t max_stderr_bytes = 64 * 1024;
    std::size_t max_notifications = 256;
    std::size_t max_notification_bytes = 8 * 1024 * 1024;
    std::size_t max_open_documents = 1024;
};

struct LspRequest {
    std::uint64_t id{};
    std::future<Json> result;
};

struct LspRange {
    TextPosition start;
    TextPosition end;
};

struct LspContentChange {
    // Missing range replaces the entire document. Changes in one notification
    // apply sequentially; every range is measured in UTF-16 code units.
    std::optional<LspRange> range;
    std::string text;
};

struct LspChangeOptions {
    // clangd's explicit extension: request fresh diagnostics when restoring an
    // earlier content identity that its AST cache can otherwise suppress.
    // The application enables this only for a known clangd process.
    bool clangd_force_rebuild{};
};

struct LspDiagnostics {
    std::string uri;
    std::optional<std::int32_t> version;
    Json diagnostics;
    // Open-document version on the client loop when this notification arrived.
    // This does not claim that an unversioned server result is versioned.
    std::optional<std::int32_t> observed_version;
};

// Percent-encoded UTF-8 file URIs; drive paths, UNC paths, spaces, # and Unicode
// round-trip without accessing the file system. Reject relative/device paths,
// malformed escapes, embedded NUL, and non-file schemes.
[[nodiscard]] std::string path_to_file_uri(const std::filesystem::path& path);
[[nodiscard]] std::filesystem::path file_uri_to_path(std::string_view uri);
[[nodiscard]] bool file_uri_equal(std::string_view left, std::string_view right);
[[nodiscard]] Json lsp_position(const Rope& text, std::size_t byte_offset);
[[nodiscard]] std::size_t lsp_byte_offset(const Rope& text, TextPosition position);

// Asynchronous stdio JSON-RPC over a real libuv child process. The adapter owns
// all server/document state on its process loop; public calls may be concurrent.
// Start once, initialize/initialized, run, shutdown/exit. close() settles pending
// futures and drains the child/pipe handles, terminating an unresponsive child.
// Successful document notification futures mean accepted writes, not server ACKs.
// RPC errors retain their server code/data through protocol::RpcError.
class LspClient {
public:
    explicit LspClient(LspOptions options);
    ~LspClient();
    LspClient(const LspClient&) = delete;
    LspClient& operator=(const LspClient&) = delete;
    LspClient(LspClient&&) = delete;
    LspClient& operator=(LspClient&&) = delete;

    [[nodiscard]] std::future<Json> start(std::string root_uri,
                                         Json initialization_options = Json::object());
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] Json capabilities() const;
    [[nodiscard]] LspRequest request(std::string method, Json params = Json::object());
    // Idempotent: completed/unknown IDs need no action. Late server responses to
    // a cancelled or expired request cannot satisfy a newer request.
    void cancel(std::uint64_t id);

    [[nodiscard]] std::future<void> did_open(std::string uri, std::string language_id,
                                           std::int32_t version, std::string text);
    [[nodiscard]] std::future<void> did_change(std::string uri, std::int32_t version,
                                              std::vector<LspContentChange> changes, LspChangeOptions options = {});
    [[nodiscard]] std::future<void> did_close(std::string uri);

    [[nodiscard]] LspRequest completion(std::string uri, TextPosition position);
    [[nodiscard]] LspRequest hover(std::string uri, TextPosition position);
    [[nodiscard]] LspRequest definition(std::string uri, TextPosition position);
    [[nodiscard]] LspRequest references(std::string uri, TextPosition position,
                                      bool include_declaration = true);
    [[nodiscard]] LspRequest rename(std::string uri, TextPosition position, std::string new_name);
    [[nodiscard]] LspRequest formatting(std::string uri, std::uint32_t tab_size = 4,
                                       bool insert_spaces = true);

    [[nodiscard]] std::vector<Json> take_notifications();
    [[nodiscard]] std::vector<LspDiagnostics> take_diagnostics();
    [[nodiscard]] std::string stderr_text() const;
    [[nodiscard]] bool stderr_truncated() const noexcept;
    [[nodiscard]] std::future<void> shutdown();
    void close() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::language
