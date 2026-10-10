#include <astra/service/metadata_clock.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace astra::service {
DocumentMetadataClock::DocumentMetadataClock(std::uint64_t next_sequence)
    : next_sequence_(next_sequence) {
    if (!next_sequence)
        throw std::invalid_argument("Document metadata sequence must be positive");
}

void DocumentMetadataClock::require_capacity(std::uint64_t count) const {
    if (count > std::numeric_limits<std::uint64_t>::max() - next_sequence_)
        throw protocol::RpcError(-32004, "Document metadata sequence exhausted");
}

void DocumentMetadataClock::validate(BufferId id, const Json& state) {
    if (!id.value || !state.is_object() || state.contains("metadataSequence"))
        throw std::invalid_argument("Metadata clock requires a buffer and untagged state object");
}

DocumentMetadataClock::Json DocumentMetadataClock::tagged(const Snapshot& snapshot) {
    auto result = snapshot.state;
    result["metadataSequence"] = std::to_string(snapshot.sequence);
    return result;
}

DocumentMetadataClock::Json DocumentMetadataClock::fresh(BufferId id, Json state, bool published) {
    require_capacity(1);
    Snapshot candidate{std::move(state), next_sequence_};
    auto result = tagged(candidate);
    auto& record = records_.try_emplace(id.value).first->second;
    if (published) {
        record.published = std::move(candidate);
        record.prepared.reset();
    } else {
        record.prepared = std::move(candidate);
    }
    ++next_sequence_;
    return result;
}

DocumentMetadataClock::Json DocumentMetadataClock::observe(BufferId id, Json state) {
    validate(id, state);
    if (const auto found = records_.find(id.value); found != records_.end()) {
        auto& record = found->second;
        if (record.published && record.published->state == state) {
            auto result = tagged(*record.published);
            record.prepared.reset();
            return result;
        }
        if (record.prepared && record.prepared->state == state &&
            (!record.published || record.prepared->sequence > record.published->sequence)) {
            auto result = tagged(*record.prepared);
            static_cast<void>(publish(id));
            return result;
        }
    }
    return fresh(id, std::move(state), true);
}

DocumentMetadataClock::Json
DocumentMetadataClock::prepare(BufferId id, Json state, bool new_binding) {
    validate(id, state);
    if (!new_binding) {
        if (const auto found = records_.find(id.value); found != records_.end()) {
            auto& record = found->second;
            if (record.published && record.published->state == state) {
                auto result = tagged(*record.published);
                record.prepared.reset();
                return result;
            }
            if (record.prepared && record.prepared->state == state &&
                (!record.published || record.prepared->sequence > record.published->sequence))
                return tagged(*record.prepared);
        }
    }
    return fresh(id, std::move(state), false);
}

bool DocumentMetadataClock::publish(BufferId id) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<std::optional<Snapshot>>);
    const auto found = records_.find(id.value);
    if (found == records_.end() || !found->second.prepared)
        return false;
    auto& record = found->second;
    if (record.published && record.prepared->sequence <= record.published->sequence)
        return false;
    record.published = std::move(record.prepared);
    record.prepared.reset();
    return true;
}

void DocumentMetadataClock::forget(BufferId id) noexcept {
    records_.erase(id.value);
}

std::size_t DocumentMetadataClock::retained_buffers() const noexcept {
    return records_.size();
}
} // namespace astra::service
