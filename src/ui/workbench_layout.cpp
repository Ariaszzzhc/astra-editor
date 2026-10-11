#include <astra/ui/workbench_layout.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace astra::ui {
namespace {
void snap(Box& box) {
    const auto right = std::round(box.right()), bottom = std::round(box.bottom());
    box.x = std::round(box.x);
    box.y = std::round(box.y);
    box.width = std::max(0.0F, right - box.x);
    box.height = std::max(0.0F, bottom - box.y);
}
} // namespace

void DialogMetrics::validate() const {
    for (const auto extent : {title, detail, input, row, button})
        if (!std::isfinite(extent) || extent <= 0 || extent > 256)
            throw std::invalid_argument("Invalid dialog font extents");
}

ChoiceLayout ChoiceLayout::measure(
    float width, float height, float scale, std::size_t choices, DialogMetrics metrics) {
    metrics.validate();
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) || width <= 0 ||
        height <= 0 || scale <= 0 || choices < 2 || choices > 4)
        throw std::invalid_argument("Invalid choice dialog dimensions");
    ChoiceLayout result;
    const auto margin = std::min(12 * scale, std::min(width, height) / 6);
    const auto pw = std::min(560 * scale, width - 2 * margin);
    const bool vertical = pw < 420 * scale;
    const auto count = static_cast<float>(choices);
    const auto desired_button = std::max(34.0F, metrics.button + 16);
    const auto desired =
        ((vertical ? 136 + 42 * count : 178) + std::max(0.0F, metrics.title - 24) +
         3 * std::max(0.0F, metrics.detail - 18) + (desired_button - 34) * (vertical ? count : 1)) *
        scale;
    const auto ph = std::min(desired, height - 2 * margin);
    const auto x = (width - pw) / 2,
               y = std::min(90 * scale, std::max(margin, height - ph - margin));
    result.panel = {x, y, pw, ph};
    const auto pad = std::min(24 * scale, std::min(pw, ph) / 8);
    const auto gap = std::min(8 * scale, std::min(pw, ph) / (count * 4));
    const auto button_height = std::min(
        std::ceil(desired_button * scale),
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
    result.title = {x + pad,
                    title_y,
                    pw - 2 * pad,
                    std::min(metrics.title * scale, std::max(0.0F, button_y - title_y))};
    const auto detail_y = std::min(result.title.bottom() + 12 * scale, button_y);
    result.detail = {x + pad, detail_y, pw - 2 * pad, std::max(0.0F, button_y - detail_y - gap)};
    snap(result.panel);
    snap(result.title);
    snap(result.detail);
    for (auto& button : result.buttons)
        snap(button);
    return result;
}

ConfirmationLayout ConfirmationLayout::measure(
    float width, float height, float scale, std::size_t items, DialogMetrics metrics) {
    metrics.validate();
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) || width <= 0 ||
        height <= 0 || scale <= 0)
        throw std::invalid_argument("Invalid confirmation dimensions");
    ConfirmationLayout result;
    result.row_height = std::ceil(std::max(28.0F, metrics.row + 8) * scale);
    const auto button_height = std::ceil(std::max(34.0F, metrics.button + 16) * scale);
    const auto pw = std::max(0.0F, std::min(600 * scale, width - 24 * scale));
    const auto desired = 175.0F + std::max(0.0F, metrics.title - 24) +
                         3 * std::max(0.0F, metrics.detail - 18) + button_height / scale - 34 +
                         (items ? 12.0F + result.row_height / scale *
                                              static_cast<float>(std::min(items, std::size_t{9}))
                                : 0.0F);
    const auto ph = std::max(0.0F, std::min(desired * scale, height - 24 * scale));
    const auto x = (width - pw) / 2,
               y = std::min(90 * scale, std::max(0.0F, height - ph - 12 * scale));
    result.panel = {x, y, pw, ph};
    const auto padding = std::min(24 * scale, pw / 6),
               button_y = std::max(y, result.panel.bottom() - button_height - 20 * scale);
    const auto bw = std::max(0.0F, (pw - 3 * padding) / 2);
    result.accept = {
        x + padding, button_y, bw, std::min(button_height, result.panel.bottom() - button_y)};
    result.cancel = {x + 2 * padding + bw, button_y, bw, result.accept.height};
    const auto title_y = std::min(y + 18 * scale, button_y);
    result.title = {x + padding,
                    title_y,
                    pw - 2 * padding,
                    std::min(metrics.title * scale, std::max(0.0F, button_y - title_y))};
    const auto dy = std::min(result.title.bottom() + 13 * scale, button_y);
    result.detail = {x + padding,
                     dy,
                     pw - 2 * padding,
                     std::min((58 + 3 * std::max(0.0F, metrics.detail - 18)) * scale,
                              std::max(0.0F, button_y - dy - 8 * scale))};
    const auto iy = std::min(result.detail.bottom() + 8 * scale, button_y);
    result.items = {x + padding,
                    iy,
                    pw - 2 * padding,
                    items ? std::max(0.0F, button_y - iy - 12 * scale) : 0.0F};
    snap(result.panel);
    snap(result.title);
    snap(result.detail);
    snap(result.items);
    snap(result.accept);
    snap(result.cancel);
    return result;
}

PickerLayout PickerLayout::measure(
    float width, float height, float scale, std::size_t rows, DialogMetrics metrics) {
    metrics.validate();
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) || width <= 0 ||
        height <= 0 || scale <= 0)
        throw std::invalid_argument("Invalid picker dimensions");
    PickerLayout result;
    const auto title_height = std::ceil(std::max(20.0F, metrics.row) * scale);
    const auto input_height = std::ceil(std::max(30.0F, metrics.input + 10) * scale);
    result.row_height = std::ceil(std::max(34.0F, metrics.row + 16) * scale);
    const auto rows_top = 8 * scale + title_height + 2 * scale + input_height + 6 * scale;
    const auto margin = std::min(12 * scale, width / 4),
               pw = std::min(600 * scale, width - 2 * margin);
    const auto y = std::min(64 * scale, height / 6),
               ph = std::min(rows_top + 8 * scale +
                                 result.row_height * static_cast<float>(std::clamp(
                                                         rows, std::size_t{1}, std::size_t{12})),
                             std::max(0.0F, height - y - 12 * scale));
    const auto x = (width - pw) / 2, pad = std::min(12 * scale, pw / 6);
    result.panel = {x, y, pw, ph};
    result.title = {x + pad,
                    y + std::min(8 * scale, ph),
                    pw - 2 * pad,
                    std::min(title_height, std::max(0.0F, ph - 8 * scale))};
    const auto iy = std::min(result.title.bottom() + 2 * scale, result.panel.bottom());
    result.input = {x + pad,
                    iy,
                    pw - 2 * pad,
                    std::min(input_height, std::max(0.0F, result.panel.bottom() - iy))};
    const auto ry = std::min(y + rows_top, result.panel.bottom());
    result.rows = {
        x + pad, ry, pw - 2 * pad, std::max(0.0F, result.panel.bottom() - ry - 8 * scale)};
    snap(result.panel);
    snap(result.title);
    snap(result.input);
    snap(result.rows);
    result.capacity = static_cast<std::size_t>(std::floor(result.rows.height / result.row_height));
    return result;
}

TextPanelLayout TextPanelLayout::measure(float width,
                                         float height,
                                         float scale,
                                         std::size_t rows,
                                         std::size_t actions,
                                         DialogMetrics metrics) {
    metrics.validate();
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
    const auto toolbar =
        static_cast<float>(action_rows) * std::ceil(std::max(40.0F, metrics.button + 18) * scale);
    const auto y = std::min(64 * scale, height / 6),
               ph = std::min(std::min(620.0F,
                                      56 + std::max(0.0F, metrics.title - 24) + toolbar / scale +
                                          std::max(20.0F, metrics.detail) *
                                              static_cast<float>(std::clamp(
                                                  rows, std::size_t{4}, std::size_t{32}))) *
                                 scale,
                             std::max(0.0F, height - y - 24 * scale));
    const auto x = (width - pw) / 2;
    result.panel = {x, y, pw, ph};
    result.title = {x + pad,
                    y + std::min(10 * scale, ph),
                    pw - 2 * pad,
                    std::min(metrics.title * scale, std::max(0.0F, ph - 10 * scale))};
    const auto cy = std::min(result.title.bottom() + 10 * scale, result.panel.bottom());
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
    snap(result.panel);
    snap(result.title);
    snap(result.content);
    for (auto& action : result.actions)
        snap(action);
    return result;
}

WorkbenchLayout WorkbenchLayout::measure(float width,
                                         float height,
                                         float scale,
                                         bool sidebar,
                                         float sidebar_width,
                                         bool outline,
                                         bool explorer,
                                         float ui_font_size) {
    if (!std::isfinite(width) || !std::isfinite(height) || !std::isfinite(scale) ||
        !std::isfinite(sidebar_width) || !std::isfinite(ui_font_size) || width <= 0 ||
        height <= 0 || scale <= 0 || ui_font_size < 9 || ui_font_size > 16) {
        throw std::invalid_argument("Invalid workbench dimensions");
    }
    const auto logical_width = width / scale;
    const auto logical_height = height / scale;
    if (!std::isfinite(logical_width) || !std::isfinite(logical_height))
        throw std::invalid_argument("Workbench logical dimensions exceed finite range");
    // Keep the document usable when a narrow window cannot fit the requested sidebar.
    const auto activity_width = std::min(44.0F, logical_width);
    const auto available = std::max(0.0F, logical_width - activity_width - 360.0F);
    const auto side = sidebar && available >= 180.0F
                          ? std::min(std::clamp(sidebar_width, 180.0F, 480.0F), available)
                          : 0;
    const auto line_height = ui_font_size * 1.15F;
    const auto physical_row_height =
        std::max(24 * scale, std::ceil(line_height * scale + 8 * scale));
    if (!std::isfinite(physical_row_height))
        throw std::invalid_argument("Workbench font geometry exceeds finite range");
    const auto heading_height = std::max(32.0F, std::ceil(line_height * 12 / 13 + 16));
    const auto section_height = std::max(24.0F, std::ceil(line_height * 12 / 13 + 8));
    const auto outline_height = std::max(26.0F, std::ceil(line_height * 11 / 13 + 8));
    const auto top = std::min(heading_height, logical_height);
    const auto status_top =
        std::max(top, logical_height - std::max(24.0F, std::ceil(line_height * 11 / 13 + 10)));
    const auto tab_bottom =
        std::min(status_top, top + std::max(34.0F, std::ceil(line_height + 16)));
    const auto text_top = std::min(status_top, tab_bottom + section_height);
    const auto left = activity_width + side;
    const auto box = [scale, width, height](float x, float y, float w, float h) {
        // Allocate shared edges on the actual pixel grid. Paint clips and pointer
        // hit tests must agree even at fractional DPI and partial final regions.
        const auto left = std::clamp(std::round(x * scale), 0.0F, width);
        const auto top = std::clamp(std::round(y * scale), 0.0F, height);
        const auto right = std::clamp(std::round((x + std::max(0.0F, w)) * scale), left, width);
        const auto bottom = std::clamp(std::round((y + std::max(0.0F, h)) * scale), top, height);
        return Box{left, top, right - left, bottom - top};
    };
    WorkbenchLayout result;
    result.scale = scale;
    result.sidebar_row_height = physical_row_height;
    result.header = box(0, 0, logical_width, top);
    const auto command_width = std::min(480.0F, logical_width * 0.44F);
    result.command_center = box((logical_width - command_width) / 2,
                                std::min(5.0F, top),
                                command_width,
                                std::max(0.0F, top - 10.0F));
    result.activity = box(0, top, activity_width, status_top - top);
    result.sidebar = box(activity_width, top, side, status_top - top);
    const auto heading_bottom = std::min(status_top, top + heading_height);
    const auto section_bottom = std::min(status_top, heading_bottom + section_height);
    result.sidebar_heading = box(activity_width, top, side, heading_bottom - top);
    const auto refresh_height = std::min(26.0F, heading_bottom - top);
    result.sidebar_refresh = box(side > 0 ? left - 34 : left,
                                 top + (heading_bottom - top - refresh_height) / 2,
                                 side > 0 ? 28.0F : 0,
                                 side > 0 ? refresh_height : 0);
    result.sidebar_section =
        box(activity_width, heading_bottom, side, section_bottom - heading_bottom);
    result.tabs = box(left, top, logical_width - left, tab_bottom - top);
    result.breadcrumb = box(left, tab_bottom, logical_width - left, text_top - tab_bottom);
    result.editor = box(left, text_top, logical_width - left, status_top - text_top);
    result.status = box(0, status_top, logical_width, logical_height - status_top);
    const auto outline_header =
        explorer && side > 0
            ? std::max(section_bottom,
                       status_top -
                           (outline ? std::min(220.0F, (status_top - top) * 0.4F) : outline_height))
            : status_top;
    result.sidebar_rows =
        box(activity_width, section_bottom, side, outline_header - section_bottom);
    result.outline_header =
        box(activity_width,
            outline_header,
            side,
            explorer ? std::min(outline_height, status_top - outline_header) : 0);
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
                           const TabIndex::Snapshot& tabs,
                           std::size_t first,
                           std::size_t active,
                           bool reveal) {
    TabStrip result;
    if (scale <= 0 || !std::isfinite(scale) || !std::isfinite(bounds.x) ||
        !std::isfinite(bounds.y) || !std::isfinite(bounds.width) || !std::isfinite(bounds.height) ||
        bounds.width < 0 || bounds.height < 0 || !std::isfinite(bounds.right())) {
        throw std::invalid_argument("Invalid tab scale");
    }
    if (tabs.empty()) {
        return result;
    }
    const bool overflow = tabs.summary().width * scale > bounds.width;
    const auto navigation_width = overflow ? std::min(26 * scale, bounds.width / 2) : 0;
    const auto available = std::max(0.0F, bounds.width - 2 * navigation_width);
    result.first = std::min(first, tabs.size() - 1);
    active = std::min(active, tabs.size() - 1);
    if (reveal) {
        if (active < result.first) {
            result.first = active;
        }
        const auto end = tabs.prefix_summary(active + 1).width;
        const auto threshold = end - static_cast<double>(available) / scale;
        if (tabs.prefix_summary(result.first).width < threshold) {
            const auto cut = tabs.seek_prefix([&](const TabIndex::Summary& summary) {
                return summary.width >= threshold;
            });
            result.first = std::min(active, cut.index + 1);
        }
    }
    float x = bounds.x;
    const auto visited = static_cast<std::size_t>(
        std::min(static_cast<double>(tabs.size() - result.first),
                 1 + std::floor(static_cast<double>(available) / (110.0 * scale))));
    auto index = result.first;
    bool exhausted{};
    tabs.for_each_range(result.first, result.first + visited, [&](const TabIndex::Entry& tab) {
        const auto i = index++;
        if (exhausted) {
            return;
        }
        const auto width = static_cast<float>(std::min(
            static_cast<double>(tab.width) * scale, static_cast<double>(bounds.x + available - x)));
        // Every visible tab reserves separate icon, caption and close targets.
        // A sliver of the following tab belongs in overflow navigation instead.
        if (width < 96 * scale) {
            exhausted = true;
            return;
        }
        result.visible.push_back({i,
                                  {x, bounds.y, width, bounds.height},
                                  {x + width - 26 * scale, bounds.y, 26 * scale, bounds.height}});
        x += width;
    });
    if (overflow) {
        result.previous = {
            bounds.right() - 2 * navigation_width, bounds.y, navigation_width, bounds.height};
        result.next = {
            bounds.right() - navigation_width, bounds.y, navigation_width, bounds.height};
    }
    return result;
}
} // namespace astra::ui
