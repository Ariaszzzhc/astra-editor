#pragma once

#include <astra/core/rope.hpp>

#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace astra {

struct BufferId {
    std::uint64_t value{};
    auto operator<=>(const BufferId&) const = default;
};

struct Revision {
    std::uint64_t value{};
    auto operator<=>(const Revision&) const = default;
};

struct ByteOffset {
    std::size_t value{};
    auto operator<=>(const ByteOffset&) const = default;
};

// Identifies a historical text state, independently of its current revision.
struct ContentId {
    std::uint64_t value{};
    auto operator<=>(const ContentId&) const = default;
};

struct Selection {
    ByteOffset anchor{};
    ByteOffset head{};

    [[nodiscard]] ByteOffset begin() const noexcept;
    [[nodiscard]] ByteOffset end() const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    auto operator<=>(const Selection&) const = default;
};

struct Edit {
    ByteOffset begin{};
    ByteOffset end{};
    std::string text;
};

struct Transaction {
    Revision base_revision{};
    // All coordinates refer to the same unmodified base revision. Order is immaterial.
    std::vector<Edit> edits;
    // Coordinates in the resulting text. If omitted, existing selections are mapped.
    std::optional<std::vector<Selection>> selections_after;
};

struct DocumentSnapshot {
    BufferId id{};
    Revision revision{};
    Rope text;
    std::vector<Selection> selections;
    ContentId content_identity{};
};

class StaleRevision final : public std::runtime_error {
public:
    StaleRevision();
};

enum class AnchorBias { before, after };

// Maps a byte offset through sorted, disjoint edits of the original text.
// Callers must supply validated edits; Document performs this validation itself.
[[nodiscard]] ByteOffset transform_offset(ByteOffset offset,
                                        std::span<const Edit> edits,
                                        AnchorBias bias);

// Single-owner document model. Services serialize access; snapshots can be shared
// across workers because the Rope is persistent and selections are copied values.
class Document {
public:
    explicit Document(BufferId id, std::string_view text = {},
                      std::size_t history_limit = 1000);
    Document& operator=(const Document&) = delete;
    Document(Document&&) = default;
    Document& operator=(Document&&) noexcept = default;

    // Explicit transactional preparation for multi-document operations. Text
    // nodes remain shared; selections and complete undo/redo state are isolated.
    [[nodiscard]] Document clone() const;
    // A new buffer identity/history over immutable snapshot text. Recovery callers
    // explicitly invalidate the saved state after restoring the external version.
    [[nodiscard]] static Document from_snapshot(BufferId id, const DocumentSnapshot& snapshot);

    [[nodiscard]] BufferId id() const noexcept;
    [[nodiscard]] Revision revision() const noexcept;
    [[nodiscard]] ContentId content_identity() const noexcept;
    [[nodiscard]] const Rope& text() const noexcept;
    [[nodiscard]] const std::vector<Selection>& selections() const noexcept;
    [[nodiscard]] DocumentSnapshot snapshot() const;
    [[nodiscard]] bool dirty() const noexcept;
    // Prepared clones must not overwrite a save/invalidation that happened while
    // their worker was running. dirty() alone cannot distinguish two dirty
    // documents with different saved-history baselines.
    [[nodiscard]] bool same_saved_state(const Document& other) const noexcept;
    [[nodiscard]] bool can_undo() const noexcept;
    [[nodiscard]] bool can_redo() const noexcept;

    // Call only after the corresponding content has been durably saved. A worker
    // saving an older snapshot must use the snapshot overload to preserve dirty state.
    void mark_saved() noexcept;
    void mark_saved(const DocumentSnapshot& saved_snapshot);
    // The on-disk content no longer matches any known history identity, for
    // example when a worker formatted an older snapshot before saving it.
    void invalidate_saved_state() noexcept;

    // Empty selection sets and non-scalar boundaries are rejected. Overlapping
    // selections are merged deterministically; adjacent nonempty ranges stay distinct.
    void set_selections(std::vector<Selection> selections);

    // Returns whether text changed. No-op text transactions neither advance the
    // revision nor change history, but may update selections. Every failure leaves
    // text, selections, history, revision and dirty state untouched.
    [[nodiscard]] bool apply(Transaction transaction);
    [[nodiscard]] bool replace_selections(std::string_view text);
    [[nodiscard]] bool undo();
    [[nodiscard]] bool redo();
    void set_history_limit(std::size_t limit) noexcept;

private:
    Document(const Document&) = default;
    struct State {
        Rope text;
        std::vector<Selection> selections;
        ContentId identity{};
    };

    struct HistoryEntry {
        State before;
        State after;
    };

    [[nodiscard]] static std::vector<Selection> normalize_selections(
        const Rope& text, std::vector<Selection> selections);
    void require_revision_space() const;
    void trim_history() noexcept;

    BufferId id_;
    Revision revision_{};
    State state_;
    ContentId saved_identity_{};
    bool saved_identity_valid_{true};
    std::uint64_t next_identity_{1};
    std::size_t history_limit_;
    std::deque<HistoryEntry> undo_;
    std::deque<HistoryEntry> redo_;
};

} // namespace astra
