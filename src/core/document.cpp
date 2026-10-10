#include <astra/core/document.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace astra {

Document Document::clone() const {
    return Document(*this);
}

namespace {

void validate_offset(const Rope& text, ByteOffset offset) {
    if (offset.value > text.size_bytes()) {
        throw std::out_of_range("byte offset exceeds document length");
    }
    if (!text.is_boundary(offset.value)) {
        throw std::invalid_argument("byte offset is inside a UTF-8 scalar");
    }
}

void validate_edits(const Rope& text, std::vector<Edit>& edits) {
    std::ranges::sort(edits, [](const Edit& left, const Edit& right) {
        if (left.begin != right.begin) {
            return left.begin < right.begin;
        }
        return left.end < right.end;
    });
    const Edit* previous = nullptr;
    for (const auto& edit : edits) {
        if (edit.begin > edit.end) {
            throw std::invalid_argument("edit range is reversed");
        }
        validate_offset(text, edit.begin);
        validate_offset(text, edit.end);
        // Constructing a replacement Rope validates the entire UTF-8 string,
        // including edits which would otherwise be discarded as no-ops.
        [[maybe_unused]] const Rope replacement(edit.text);
        if (previous != nullptr) {
            const bool touching_insertion =
                previous->end == edit.begin &&
                (previous->begin == previous->end || edit.begin == edit.end);
            if (previous->end > edit.begin || touching_insertion) {
                throw std::invalid_argument(
                    "transaction edits overlap or have ambiguous insertion order");
            }
        }
        previous = &edit;
    }
}

std::size_t add_checked(std::size_t left, std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        throw std::length_error("document byte length overflow");
    }
    return left + right;
}

class OffsetMap {
  public:
    explicit OffsetMap(std::span<const Edit> edits) : edits_(edits) {
        mapped_begins_.reserve(edits.size());
        std::size_t source = 0;
        std::size_t destination = 0;
        for (const auto& edit : edits) {
            destination = add_checked(destination, edit.begin.value - source);
            mapped_begins_.push_back(destination);
            destination = add_checked(destination, edit.text.size());
            source = edit.end.value;
        }
    }

    [[nodiscard]] ByteOffset map(ByteOffset offset, AnchorBias bias) const {
        auto found = std::upper_bound(
            edits_.begin(), edits_.end(), offset, [](ByteOffset position, const Edit& edit) {
                return position < edit.begin;
            });
        if (found == edits_.begin()) {
            return offset;
        }
        --found;
        const auto index = static_cast<std::size_t>(found - edits_.begin());
        const auto begin = mapped_begins_[index];
        if (offset == found->begin || offset < found->end) {
            return ByteOffset{bias == AnchorBias::before ? begin
                                                         : add_checked(begin, found->text.size())};
        }
        return ByteOffset{
            add_checked(add_checked(begin, found->text.size()), offset.value - found->end.value)};
    }

  private:
    std::span<const Edit> edits_;
    std::vector<std::size_t> mapped_begins_;
};

} // namespace

ByteOffset Selection::begin() const noexcept {
    return std::min(anchor, head);
}

ByteOffset Selection::end() const noexcept {
    return std::max(anchor, head);
}

bool Selection::empty() const noexcept {
    return anchor == head;
}

StaleRevision::StaleRevision() : std::runtime_error("transaction base revision is stale") {
}

ByteOffset transform_offset(ByteOffset offset, std::span<const Edit> edits, AnchorBias bias) {
    std::size_t source = 0;
    std::size_t destination = 0;
    for (const auto& edit : edits) {
        if (offset < edit.begin) {
            break;
        }
        destination = add_checked(destination, edit.begin.value - source);
        if (offset == edit.begin || offset < edit.end) {
            return ByteOffset{bias == AnchorBias::before
                                  ? destination
                                  : add_checked(destination, edit.text.size())};
        }
        destination = add_checked(destination, edit.text.size());
        source = edit.end.value;
    }
    return ByteOffset{add_checked(destination, offset.value - source)};
}

Document::Document(BufferId id, std::string_view text, std::size_t history_limit)
    : id_(id), state_{Rope(text), {Selection{}}, ContentId{}}, history_limit_(history_limit) {
}

Document Document::from_snapshot(BufferId id, const DocumentSnapshot& snapshot) {
    Document result(id);
    result.state_.text = snapshot.text;
    result.set_selections(snapshot.selections);
    return result;
}

BufferId Document::id() const noexcept {
    return id_;
}

Revision Document::revision() const noexcept {
    return revision_;
}

ContentId Document::content_identity() const noexcept {
    return state_.identity;
}

const Rope& Document::text() const noexcept {
    return state_.text;
}

const std::vector<Selection>& Document::selections() const noexcept {
    return state_.selections;
}

DocumentSnapshot Document::snapshot() const {
    return {id_, revision_, state_.text, state_.selections, state_.identity};
}

bool Document::dirty() const noexcept {
    return !saved_identity_valid_ || state_.identity != saved_identity_;
}

bool Document::same_saved_state(const Document& other) const noexcept {
    return id_ == other.id_ && saved_identity_ == other.saved_identity_ &&
           saved_identity_valid_ == other.saved_identity_valid_;
}

bool Document::can_undo() const noexcept {
    return !undo_.empty();
}

bool Document::can_redo() const noexcept {
    return !redo_.empty();
}

void Document::mark_saved() noexcept {
    saved_identity_ = state_.identity;
    saved_identity_valid_ = true;
}

void Document::mark_saved(const DocumentSnapshot& saved_snapshot) {
    if (saved_snapshot.id != id_) {
        throw std::invalid_argument("saved snapshot belongs to a different document");
    }
    saved_identity_ = saved_snapshot.content_identity;
    saved_identity_valid_ = true;
}

void Document::invalidate_saved_state() noexcept {
    saved_identity_valid_ = false;
}

std::vector<Selection> Document::normalize_selections(const Rope& text,
                                                      std::vector<Selection> selections) {
    if (selections.empty()) {
        throw std::invalid_argument("a document requires at least one selection");
    }
    for (const auto& selection : selections) {
        validate_offset(text, selection.anchor);
        validate_offset(text, selection.head);
    }
    std::ranges::sort(selections, [](const Selection& left, const Selection& right) {
        if (left.begin() != right.begin()) {
            return left.begin() < right.begin();
        }
        if (left.end() != right.end()) {
            return left.end() < right.end();
        }
        return left.anchor < right.anchor;
    });
    std::vector<Selection> result;
    result.reserve(selections.size());
    for (const auto& selection : selections) {
        if (!result.empty()) {
            auto& previous = result.back();
            const bool touching_cursor =
                selection.begin() == previous.end() && (selection.empty() || previous.empty());
            if (selection.begin() < previous.end() || touching_cursor) {
                const bool reversed = previous.anchor > previous.head;
                const auto begin = previous.begin();
                const auto end = std::max(previous.end(), selection.end());
                previous = reversed ? Selection{end, begin} : Selection{begin, end};
                continue;
            }
        }
        result.push_back(selection);
    }
    return result;
}

void Document::set_selections(std::vector<Selection> selections) {
    state_.selections = normalize_selections(state_.text, std::move(selections));
}

void Document::require_revision_space() const {
    if (revision_.value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("document revision exhausted");
    }
}

bool Document::apply(Transaction transaction) {
    if (transaction.base_revision != revision_) {
        throw StaleRevision();
    }
    validate_edits(state_.text, transaction.edits);
    // Remove semantic no-ops only after every original edit has been validated.
    std::erase_if(transaction.edits, [this](const Edit& edit) {
        return edit.end.value - edit.begin.value == edit.text.size() &&
               state_.text.slice(edit.begin.value, edit.end.value) == edit.text;
    });
    State next = state_;
    for (auto edit = transaction.edits.rbegin(); edit != transaction.edits.rend(); ++edit) {
        next.text = next.text.replace(edit->begin.value, edit->end.value, edit->text);
    }
    if (transaction.selections_after) {
        next.selections = normalize_selections(next.text, std::move(*transaction.selections_after));
    } else {
        const OffsetMap offset_map(transaction.edits);
        for (auto& selection : next.selections) {
            // Collapsed cursors track inserted text; nonempty selections keep their
            // direction and include replacements at their outer boundaries.
            const bool reversed = selection.anchor > selection.head;
            const auto begin = offset_map.map(
                selection.begin(), selection.empty() ? AnchorBias::after : AnchorBias::before);
            const auto end = offset_map.map(selection.end(), AnchorBias::after);
            selection = reversed ? Selection{end, begin} : Selection{begin, end};
        }
        next.selections = normalize_selections(next.text, std::move(next.selections));
    }
    if (transaction.edits.empty()) {
        state_.selections = std::move(next.selections);
        return false;
    }
    require_revision_space();
    if (next_identity_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("document content identity exhausted");
    }
    next.identity = ContentId{next_identity_};
    if (history_limit_ != 0) {
        // All potentially throwing allocations happen before any document mutation.
        undo_.push_back(HistoryEntry{state_, next});
    }
    state_ = std::move(next);
    ++next_identity_;
    ++revision_.value;
    redo_.clear();
    trim_history();
    return true;
}

bool Document::replace_selections(std::string_view text) {
    Transaction transaction{revision_, {}, std::vector<Selection>{}};
    transaction.edits.reserve(state_.selections.size());
    transaction.selections_after->reserve(state_.selections.size());
    std::size_t source = 0;
    std::size_t destination = 0;
    for (const auto& selection : state_.selections) {
        destination = add_checked(destination, selection.begin().value - source);
        destination = add_checked(destination, text.size());
        source = selection.end().value;
        transaction.edits.push_back(Edit{selection.begin(), selection.end(), std::string(text)});
        const ByteOffset caret{destination};
        transaction.selections_after->push_back(Selection{caret, caret});
    }
    return apply(std::move(transaction));
}

bool Document::undo() {
    if (undo_.empty()) {
        return false;
    }
    require_revision_space();
    State previous = undo_.back().before;
    redo_.push_back(undo_.back());
    state_ = std::move(previous);
    undo_.pop_back();
    ++revision_.value;
    trim_history();
    return true;
}

bool Document::redo() {
    if (redo_.empty()) {
        return false;
    }
    require_revision_space();
    State next = redo_.back().after;
    undo_.push_back(redo_.back());
    state_ = std::move(next);
    redo_.pop_back();
    ++revision_.value;
    trim_history();
    return true;
}

void Document::trim_history() noexcept {
    while (undo_.size() > history_limit_) {
        undo_.pop_front();
    }
    while (redo_.size() > history_limit_) {
        redo_.pop_front();
    }
}

void Document::set_history_limit(std::size_t limit) noexcept {
    history_limit_ = limit;
    trim_history();
}

} // namespace astra
