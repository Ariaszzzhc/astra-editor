#pragma once

#include <astra/protocol/rpc.hpp>
#include <astra/core/sum_tree.hpp>
#include <algorithm>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace astra::ui {
[[nodiscard]] std::uint64_t document_id(std::string_view);

// Text revisions validate edits. Metadata sequences order observed state snapshots
// within one service connection, including selection/save changes without edits.
struct DocumentMetadataStamp {
    std::uint64_t revision{}, sequence{};

    [[nodiscard]] static DocumentMetadataStamp read(const protocol::Json&);
    [[nodiscard]] bool follows(const DocumentMetadataStamp& current) const noexcept;
    auto operator<=>(const DocumentMetadataStamp&) const = default;
};

// Service buffer IDs are never reused within a connection. Keep closed IDs in
// disjoint, non-adjacent inclusive ranges, including closes seen before creation.
// Persistent measured seeks/updates are O(log ranges); copies retain old state.
class ClosedDocumentIds final {
  public:
    [[nodiscard]] bool contains(std::uint64_t) const;
    [[nodiscard]] bool retire(std::uint64_t);
    [[nodiscard]] std::size_t range_count() const noexcept;
    [[nodiscard]] bool validate() const;

  private:
    struct Range {
        std::uint64_t first{}, last{};
    };

    struct Summary {
        std::uint64_t last{};

        friend Summary operator+(Summary before, Summary after) noexcept {
            return {std::max(before.last, after.last)};
        }

        bool operator==(const Summary&) const = default;
    };

    struct Measure {
        Summary operator()(const Range& range) const noexcept {
            return {range.last};
        }
    };

    SumTree<Range, Summary, Measure> ranges_;
};
} // namespace astra::ui
