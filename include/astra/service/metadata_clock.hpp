#pragma once

#include <astra/core/document.hpp>
#include <astra/protocol/rpc.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>

namespace astra::service {
// One owner sequences bounded core metadata tuples, never document text.
// Preparation cannot replace published state. Observing a published state
// retires an abandoned candidate; publication adopts its exact prepared marker.
class DocumentMetadataClock final {
  public:
    using Json = protocol::Json;
    explicit DocumentMetadataClock(std::uint64_t next_sequence = 1);
    DocumentMetadataClock(const DocumentMetadataClock&) = delete;
    DocumentMetadataClock& operator=(const DocumentMetadataClock&) = delete;

    void require_capacity(std::uint64_t count) const;
    [[nodiscard]] Json observe(BufferId, Json state);
    [[nodiscard]] Json prepare(BufferId, Json state, bool new_binding = false);
    [[nodiscard]] bool publish(BufferId) noexcept;
    void forget(BufferId) noexcept;
    [[nodiscard]] std::size_t retained_buffers() const noexcept;

  private:
    struct Snapshot {
        Json state;
        std::uint64_t sequence{};
    };

    struct Record {
        std::optional<Snapshot> published, prepared;
    };

    static void validate(BufferId, const Json&);
    [[nodiscard]] static Json tagged(const Snapshot&);
    [[nodiscard]] Json fresh(BufferId, Json, bool published);
    std::uint64_t next_sequence_;
    std::map<std::uint64_t, Record> records_;
};
} // namespace astra::service
