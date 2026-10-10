#pragma once

#include <astra/protocol/rpc.hpp>
#include <compare>
#include <cstdint>

namespace astra::ui {
// Text revisions validate edits. Metadata sequences order observed state snapshots
// within one service connection, including selection/save changes without edits.
struct DocumentMetadataStamp {
    std::uint64_t revision{}, sequence{};

    [[nodiscard]] static DocumentMetadataStamp read(const protocol::Json&);
    [[nodiscard]] bool follows(const DocumentMetadataStamp& current) const noexcept;
    auto operator<=>(const DocumentMetadataStamp&) const = default;
};
} // namespace astra::ui
