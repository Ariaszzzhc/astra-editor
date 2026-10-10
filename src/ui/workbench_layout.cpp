#include <astra/ui/workbench_layout.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace astra::ui {
ChoiceLayout ChoiceLayout::measure(float width, float height, float scale, std::size_t choices) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) || width <= 0 ||
        height <= 0 || scale <= 0 || choices < 2 || choices > 4)
        throw std::invalid_argument("Invalid choice dialog dimensions");
    ChoiceLayout result;
    const auto margin = std::min(12 * scale, std::min(width, height) / 6);
    const auto pw = std::min(560 * scale, width - 2 * margin);
    const bool vertical = pw < 420 * scale;
    const auto count = static_cast<float>(choices);
    const auto desired = (vertical ? 136 + 42 * count : 178) * scale;
    const auto ph = std::min(desired, height - 2 * margin);
    const auto x = (width - pw) / 2,
               y = std::min(90 * scale, std::max(margin, height - ph - margin));
    result.panel = {x, y, pw, ph};
    const auto pad = std::min(24 * scale, std::min(pw, ph) / 8);
    const auto gap = std::min(8 * scale, std::min(pw, ph) / (count * 4));
    const auto button_height = std::min(
        34 * scale,
        std::max(0.0F,
                 (ph - 2 * pad - (vertical ? gap * (count - 1) : 0)) / (vertical ? count : 1)));
    const auto area_height = vertical ? count * button_height + (count - 1) * gap : button_height;
    const auto button_y = result.panel.bottom() - pad - area_height;
    const auto button_width = vertical ? pw - 2 * pad : (pw - 2 * pad - (count - 1) * gap) / count;
    for (std::size_t i = 0; i < choices; ++i)
        result.buttons.push_back(
            {x + pad + (vertical ? 0 : static_cast<float>(i) * (button_width + gap)),
             button_y + (vertical ? static_cast<float>(i) * (button_height + gap) : 0),
             button_width,
             button_height});
    const auto title_y = std::min(y + pad, button_y);
    result.title = {
        x + pad, title_y, pw - 2 * pad, std::min(24 * scale, std::max(0.0F, button_y - title_y))};
    const auto detail_y = std::min(result.title.bottom() + 12 * scale, button_y);
    result.detail = {x + pad, detail_y, pw - 2 * pad, std::max(0.0F, button_y - detail_y - gap)};
    return result;
}

ConfirmationLayout
ConfirmationLayout::measure(float width, float height, float scale, std::size_t items) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) || width <= 0 ||
        height <= 0 || scale <= 0)
        throw std::invalid_argument("Invalid confirmation dimensions");
    ConfirmationLayout result;
    const auto pw = std::max(0.0F, std::min(600 * scale, width - 24 * scale));
    const auto desired =
        175.0F +
        (items ? 12.0F + 28.0F * static_cast<float>(std::min(items, std::size_t{9})) : 0.0F);
    const auto ph = std::max(0.0F, std::min(desired * scale, height - 24 * scale));
    const auto x = (width - pw) / 2,
               y = std::min(90 * scale, std::max(0.0F, height - ph - 12 * scale));
    result.panel = {x, y, pw, ph};
    const auto padding = std::min(24 * scale, pw / 6),
               button_y = std::max(y, result.panel.bottom() - 54 * scale);
    const auto bw = std::max(0.0F, (pw - 3 * padding) / 2);
    result.accept = {
        x + padding, button_y, bw, std::min(34 * scale, result.panel.bottom() - button_y)};
    result.cancel = {x + 2 * padding + bw, button_y, bw, result.accept.height};
    const auto title_y = std::min(y + 18 * scale, button_y);
    result.title = {x + padding,
                    title_y,
                    pw - 2 * padding,
                    std::min(24 * scale, std::max(0.0F, button_y - title_y))};
    const auto dy = std::min(y + 55 * scale, button_y);
    result.detail = {x + padding,
                     dy,
                     pw - 2 * padding,
                     std::min(58 * scale, std::max(0.0F, button_y - dy - 8 * scale))};
    const auto iy = std::min(result.detail.bottom() + 8 * scale, button_y);
    result.items = {x + padding,
                    iy,
                    pw - 2 * padding,
                    items ? std::max(0.0F, button_y - iy - 12 * scale) : 0.0F};
    return result;
}

PickerLayout PickerLayout::measure(float width, float height, float scale, std::size_t rows) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) || width <= 0 ||
        height <= 0 || scale <= 0)
        throw std::invalid_argument("Invalid picker dimensions");
    PickerLayout result;
    const auto margin = std::min(12 * scale, width / 4),
               pw = std::min(600 * scale, width - 2 * margin);
    const auto y = std::min(64 * scale, height / 6),
               ph = std::min((74 + 34 * static_cast<float>(
                                            std::clamp(rows, std::size_t{1}, std::size_t{12}))) *
                                 scale,
                             std::max(0.0F, height - y - 12 * scale));
    const auto x = (width - pw) / 2, pad = std::min(12 * scale, pw / 6);
    result.panel = {x, y, pw, ph};
    result.title = {x + pad, y + 8 * scale, pw - 2 * pad, std::min(20 * scale, ph)};
    const auto iy = std::min(y + 30 * scale, result.panel.bottom());
    result.input = {x + pad,
                    iy,
                    pw - 2 * pad,
                    std::min(30 * scale, std::max(0.0F, result.panel.bottom() - iy))};
    const auto ry = std::min(y + 66 * scale, result.panel.bottom());
    result.rows = {
        x + pad, ry, pw - 2 * pad, std::max(0.0F, result.panel.bottom() - ry - 8 * scale)};
    result.row_height = 34 * scale;
    result.capacity = static_cast<std::size_t>(std::floor(result.rows.height / result.row_height));
    return result;
}

TextPanelLayout TextPanelLayout::measure(
    float width, float height, float scale, std::size_t rows, std::size_t actions) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) || width <= 0 ||
        height <= 0 || scale <= 0 || actions > 8)
        throw std::invalid_argument("Invalid text panel dimensions");
    TextPanelLayout result;
    const auto margin = std::min(24 * scale, width / 6),
               pw = std::min(900 * scale, width - 2 * margin);
    const auto pad = std::min(18 * scale, pw / 6);
    const auto columns = std::max(
        std::size_t{1},
        std::min(actions, static_cast<std::size_t>(std::floor((pw - 2 * pad) / (120 * scale)))));
    const auto action_rows = actions ? (actions + columns - 1) / columns : 0;
    const auto toolbar = static_cast<float>(action_rows) * 40 * scale;
    const auto y = std::min(64 * scale, height / 6),
               ph = std::min(std::min(620.0F,
                                      56 + toolbar / scale +
                                          20 * static_cast<float>(std::clamp(
                                                   rows, std::size_t{4}, std::size_t{32}))) *
                                 scale,
                             std::max(0.0F, height - y - 24 * scale));
    const auto x = (width - pw) / 2;
    result.panel = {x, y, pw, ph};
    result.title = {x + pad, y + 10 * scale, pw - 2 * pad, std::min(24 * scale, ph)};
    const auto cy = std::min(y + 44 * scale, result.panel.bottom());
    const auto available = std::max(0.0F, result.panel.bottom() - cy - 12 * scale);
    const auto used_toolbar = std::min(toolbar, available);
    result.content = {x + pad, cy, pw - 2 * pad, available - used_toolbar};
    if (actions) {
        const auto step_y = used_toolbar / static_cast<float>(action_rows);
        const auto step_x = result.content.width / static_cast<float>(columns);
        for (std::size_t index = 0; index < actions; ++index)
            result.actions.push_back(
                {x + pad + static_cast<float>(index % columns) * step_x,
                 result.content.bottom() + static_cast<float>(index / columns) * step_y,
                 std::max(0.0F, step_x - 4 * scale),
                 std::max(0.0F, step_y - 4 * scale)});
    }
    return result;
}

WorkbenchLayout WorkbenchLayout::measure(float width,
                                         float height,
                                         float scale,
                                         bool sidebar,
                                         float sidebar_width,
                                         bool outline,
                                         bool explorer) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) ||
        !std::isfinite(sidebar_width) || width <= 0 || height <= 0 || scale <= 0) {
        throw std::invalid_argument("Invalid workbench dimensions");
    }
    const auto logical_width = width / scale;
    const auto logical_height = height / scale;
    // Keep the document usable when a narrow window cannot fit the requested sidebar.
    const auto activity_width = std::min(44.0F, logical_width);
    const auto available = std::max(0.0F, logical_width - activity_width - 360.0F);
    const auto side = sidebar && available >= 180.0F
                          ? std::min(std::clamp(sidebar_width, 180.0F, 480.0F), available)
                          : 0;
    const auto top = std::min(32.0F, logical_height);
    const auto status_top = std::max(top, logical_height - 24.0F);
    const auto tab_bottom = std::min(status_top, top + 34.0F);
    const auto text_top = std::min(status_top, tab_bottom + 24.0F);
    const auto left = activity_width + side;
    const auto box = [scale](float x, float y, float w, float h) {
        return Box{x * scale, y * scale, std::max(0.0F, w) * scale, std::max(0.0F, h) * scale};
    };
    WorkbenchLayout result;
    result.scale = scale;
    result.sidebar_row_height = 24 * scale;
    result.header = box(0, 0, logical_width, top);
    const auto command_width = std::min(480.0F, logical_width * 0.44F);
    result.command_center = box((logical_width - command_width) / 2,
                                std::min(5.0F, top),
                                command_width,
                                std::max(0.0F, std::min(22.0F, top - 10.0F)));
    result.activity = box(0, top, activity_width, status_top - top);
    result.sidebar = box(activity_width, top, side, status_top - top);
    result.tabs = box(left, top, logical_width - left, tab_bottom - top);
    result.breadcrumb = box(left, tab_bottom, logical_width - left, text_top - tab_bottom);
    result.editor = box(left, text_top, logical_width - left, status_top - text_top);
    result.status = box(0, status_top, logical_width, logical_height - status_top);
    const auto outline_header =
        explorer && side > 0
            ? std::max(std::min(top + 56.0F, status_top),
                       status_top - (outline ? std::min(220.0F, (status_top - top) * 0.4F) : 26.0F))
            : status_top;
    result.sidebar_rows = box(activity_width,
                              std::min(top + 56.0F, status_top),
                              side,
                              outline_header - std::min(top + 56.0F, status_top));
    result.outline_header = box(activity_width,
                                outline_header,
                                side,
                                explorer ? std::min(26.0F, status_top - outline_header) : 0);
    result.outline_rows =
        box(activity_width,
            result.outline_header.bottom() / scale,
            side,
            outline && explorer ? status_top - result.outline_header.bottom() / scale : 0);
    result.sidebar_handle = box(left - 3, top, side > 0 ? 6.0F : 0.0F, status_top - top);
    return result;
}

TabStrip TabStrip::measure(Box bounds,
                           float scale,
                           const std::vector<float>& labels,
                           std::size_t first,
                           std::size_t active,
                           bool reveal) {
    TabStrip result;
    if (scale <= 0 || !std::isfinite(scale) || !std::isfinite(bounds.x) ||
        !std::isfinite(bounds.y) || !std::isfinite(bounds.width) || !std::isfinite(bounds.height) ||
        bounds.width < 0 || bounds.height < 0) {
        throw std::invalid_argument("Invalid tab scale");
    }
    if (labels.empty()) {
        return result;
    }
    std::vector<float> widths;
    float total{};
    for (const auto width : labels) {
        if (!std::isfinite(width) || width < 0) {
            throw std::invalid_argument("Invalid tab label width");
        }
        widths.push_back(std::clamp(width + 64 * scale, 110 * scale, 260 * scale));
        total += widths.back();
    }
    const bool overflow = total > bounds.width;
    const auto navigation_width = overflow ? std::min(26 * scale, bounds.width / 2) : 0;
    const auto available = std::max(0.0F, bounds.width - 2 * navigation_width);
    result.first = std::min(first, labels.size() - 1);
    active = std::min(active, labels.size() - 1);
    if (reveal) {
        if (active < result.first) {
            result.first = active;
        }
        float length{};
        for (auto i = result.first; i <= active; ++i) {
            length += widths[i];
        }
        while (result.first < active && length > available) {
            length -= widths[result.first++];
        }
    }
    float x = bounds.x;
    for (auto i = result.first; i < labels.size() && x < bounds.x + available; ++i) {
        const auto width = std::min(widths[i], bounds.x + available - x);
        // Every visible tab reserves separate icon, caption and close targets.
        // A sliver of the following tab belongs in overflow navigation instead.
        if (width < 96 * scale) {
            break;
        }
        result.visible.push_back({i,
                                  {x, bounds.y, width, bounds.height},
                                  {x + width - 26 * scale, bounds.y, 26 * scale, bounds.height}});
        x += width;
    }
    if (overflow) {
        result.previous = {
            bounds.right() - 2 * navigation_width, bounds.y, navigation_width, bounds.height};
        result.next = {
            bounds.right() - navigation_width, bounds.y, navigation_width, bounds.height};
    }
    return result;
}
} // namespace astra::ui
