#pragma once

#include <astra/ui/workbench_layout.hpp>
#include <cstddef>
#include <optional>

namespace astra::ui {
// Pixel-space viewport shared by native paint, pointer dispatch and paging.
// The half-open row range contains only rows intersecting the clipped bounds.
class ViewRowsLayout final {
  public:
    [[nodiscard]] static ViewRowsLayout
    measure(Box, float scale, float row_height, std::size_t total, std::size_t first);

    [[nodiscard]] const Box& bounds() const noexcept {
        return bounds_;
    }

    [[nodiscard]] float scale() const noexcept {
        return scale_;
    }

    [[nodiscard]] float row_height() const noexcept {
        return row_height_;
    }

    [[nodiscard]] std::size_t first() const noexcept {
        return first_;
    }

    [[nodiscard]] std::size_t end() const noexcept {
        return end_;
    }

    [[nodiscard]] std::size_t page() const noexcept {
        return page_;
    }

    [[nodiscard]] Box row(std::size_t index) const;
    [[nodiscard]] std::optional<std::size_t> hit(float x, float y) const;

  private:
    Box bounds_;
    float scale_{1}, row_height_{1};
    std::size_t first_{}, end_{}, page_{1};
};
} // namespace astra::ui
