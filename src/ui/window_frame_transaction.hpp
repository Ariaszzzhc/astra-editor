#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <utility>

namespace astra::ui::detail {
struct FrameAttribute {
    std::uint32_t id{}, next{}, previous{};
    bool supported{};
    bool probe{};
};

struct FrameUpdateResult {
    std::uint32_t attribute{};
    std::int32_t error{}, rollback_error{};
    bool finalizing{};
};

// Query/snapshot precedes this operation. A failed write can have mutated the
// backend, so restore it too, and attempt every restore even after a failure.
template <typename Apply, typename Finish>
FrameUpdateResult
update_frame(std::span<const FrameAttribute> attributes, Apply&& apply, Finish&& finish) {
    const auto restore = [&](std::size_t count, FrameUpdateResult failure) {
        for (auto rollback = count; rollback > 0; --rollback) {
            const auto& prior = attributes[rollback - 1];
            if (!prior.supported || (prior.next == prior.previous && !prior.probe))
                continue;
            const auto restored = apply(prior.id, prior.previous);
            if (restored < 0 && failure.rollback_error >= 0)
                failure.rollback_error = restored;
        }
        const auto refreshed = finish();
        if (refreshed < 0 && failure.rollback_error >= 0)
            failure.rollback_error = refreshed;
        return failure;
    };
    for (std::size_t index = 0; index < attributes.size(); ++index) {
        const auto& attribute = attributes[index];
        if (!attribute.supported || (attribute.next == attribute.previous && !attribute.probe))
            continue;
        const auto result = apply(attribute.id, attribute.next);
        if (result >= 0)
            continue;
        return restore(index + 1, {attribute.id, result});
    }
    const auto finalized = finish();
    if (finalized < 0)
        return restore(attributes.size(), {0, finalized, 0, true});
    return {};
}

template <typename Apply>
FrameUpdateResult update_frame(std::span<const FrameAttribute> attributes, Apply&& apply) {
    return update_frame(attributes, std::forward<Apply>(apply), [] {
        return std::int32_t{0};
    });
}
} // namespace astra::ui::detail
