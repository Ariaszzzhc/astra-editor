#include <astra/ui/view_rows_layout.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace astra::ui {
ViewRowsLayout ViewRowsLayout::measure(
    Box bounds, float scale, float row_height, std::size_t total, std::size_t first) {
    if (!std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || !std::isfinite(bounds.right()) ||
        !std::isfinite(bounds.bottom()) || bounds.width < 0 || bounds.height < 0 ||
        (bounds.width > 0 && bounds.right() <= bounds.x) ||
        (bounds.height > 0 && bounds.bottom() <= bounds.y) || !std::isfinite(scale) || scale <= 0 ||
        !std::isfinite(row_height) || row_height < 1)
        throw std::invalid_argument("Invalid view row geometry");
    ViewRowsLayout result;
    result.bounds_ = bounds;
    result.scale_ = scale;
    result.row_height_ = row_height;
    const auto edge_magnitude = std::max(std::abs(bounds.y), std::abs(bounds.bottom()));
    if (bounds.height > 0 &&
        std::nextafter(edge_magnitude, std::numeric_limits<float>::infinity()) - edge_magnitude >
            row_height)
        throw std::invalid_argument("View row height is below pixel coordinate precision");
    result.first_ = total ? std::min(first, total - 1) : 0;
    result.end_ = result.first_;
    const auto rows = static_cast<double>(bounds.height) / row_height;
    result.page_ = static_cast<std::size_t>(std::clamp(std::floor(rows), 1.0, 256.0));
    if (bounds.width > 0 && bounds.height > 0) {
        const auto remaining = total - result.first_;
        const auto intersecting = std::ceil(rows);
        auto count = intersecting >= static_cast<double>(remaining)
                         ? remaining
                         : static_cast<std::size_t>(intersecting);
        const auto start = [&](std::size_t offset) {
            return static_cast<float>(
                std::min(static_cast<double>(bounds.bottom()),
                         static_cast<double>(bounds.y) + static_cast<double>(offset) * row_height));
        };
        // Source extents and painted clip edges round differently in floats.
        // Pixel precision validation above bounds the correction to a neighbor.
        if (count < remaining && start(count) < bounds.bottom())
            ++count;
        if (count && start(count - 1) >= bounds.bottom())
            --count;
        result.end_ += count;
    }
    return result;
}

Box ViewRowsLayout::row(std::size_t index) const {
    if (index < first_ || index >= end_)
        throw std::out_of_range("View row lies outside the measured viewport");
    const auto start = [&](std::size_t offset) {
        return static_cast<float>(
            std::min(static_cast<double>(bounds_.bottom()),
                     static_cast<double>(bounds_.y) + static_cast<double>(offset) * row_height_));
    };
    const auto y = start(index - first_);
    const auto bottom = start(index - first_ + 1);
    return {bounds_.x, y, bounds_.width, std::max(0.0F, bottom - y)};
}

std::optional<std::size_t> ViewRowsLayout::hit(float x, float y) const {
    if (!bounds_.contains(x, y) || end_ == first_)
        return std::nullopt;
    const auto offset = std::floor((static_cast<double>(y) - bounds_.y) / row_height_);
    if (offset < 0)
        return std::nullopt;
    const auto index = offset >= static_cast<double>(end_ - first_)
                           ? end_ - 1
                           : first_ + static_cast<std::size_t>(offset);
    // Paint uses float pixel edges. Correct a one-row rounding difference at
    // a fractional boundary instead of independently rounding the hit point.
    if (row(index).contains(x, y))
        return index;
    if (index + 1 < end_ && row(index + 1).contains(x, y))
        return index + 1;
    if (index > first_ && row(index - 1).contains(x, y))
        return index - 1;
    return std::nullopt;
}
} // namespace astra::ui
