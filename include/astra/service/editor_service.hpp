#pragma once

#include <astra/core/document.hpp>
#include <astra/editing/commands.hpp>
#include <astra/protocol/rpc.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace astra::service {

// One owner serializes calls from GUI, TCP, JS and Agent transports. All callers
// use this same dispatcher and Document transaction implementation. Buffer IDs
// and revisions are canonical decimal strings on the wire, never rounded JS
// numbers. Offsets are nonnegative, JavaScript-safe UTF-8 byte integers.
class EditorService {
  public:
    using Json = protocol::Json;
    static constexpr std::size_t max_read_bytes = 64 * 1024;
    static constexpr std::size_t max_pending_notifications = 1024;
    static constexpr std::size_t max_transaction_edits = 4096;
    static constexpr std::size_t max_selections = 4096;

    EditorService();
    EditorService(const EditorService&) = delete;
    EditorService& operator=(const EditorService&) = delete;
    EditorService(EditorService&&) = delete;
    EditorService& operator=(EditorService&&) = delete;

    [[nodiscard]] protocol::Dispatcher& dispatcher() noexcept;
    [[nodiscard]] const protocol::Dispatcher& dispatcher() const noexcept;
    [[nodiscard]] std::optional<Json> dispatch(const Json& request);
    [[nodiscard]] std::optional<std::string> dispatch_text(std::string_view body);

    // Ordered, metadata-only notifications. Mutations reserve queue capacity
    // before touching a document and fail explicitly when the queue is full.
    [[nodiscard]] std::vector<Json> take_notifications();
    [[nodiscard]] std::size_t document_count() const noexcept;

  private:
    friend class ApplicationService;
    [[nodiscard]] Json create(const Json& params);
    [[nodiscard]] Json get(const Json& params);
    [[nodiscard]] Json list(const Json& params);
    [[nodiscard]] Json lines(const Json& params);
    [[nodiscard]] Json command(const Json& params);
    [[nodiscard]] Json edit(const Json& params);
    [[nodiscard]] Json insert(const Json& params);
    [[nodiscard]] Json set_selections(const Json& params);
    [[nodiscard]] Json undo(const Json& params);
    [[nodiscard]] Json redo(const Json& params);
    [[nodiscard]] Json close(const Json& params);
    [[nodiscard]] Document& find_document(const Json& params);
    static void require_revision(const Document& document, const Json& params);
    [[nodiscard]] static Json metadata(const Document& document);
    void require_notification_capacity() const;
    void changed(const Document& document, std::string_view reason);

    protocol::Dispatcher dispatcher_;
    std::map<std::uint64_t, Document> documents_;
    std::deque<Json> notifications_;
    std::function<editing::Options(const Document&)> command_defaults_;
    std::uint64_t next_id_{1};
};

} // namespace astra::service
