#include <astra/ui/workbench_chrome.hpp>
#include <astra/ui/icons.hpp>
#include <astra/core/utf8.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace astra::ui {
namespace {
SkRect rect(Box box) {
    return SkRect::MakeXYWH(box.x, box.y, box.width, box.height);
}

void fill(SkCanvas* canvas, Box box, std::uint32_t color) {
    SkPaint paint;
    paint.setColor(color);
    canvas->drawRect(rect(box), paint);
}

void icon(SkCanvas* canvas, Icon value, Box box, float size, std::uint32_t color) {
    if (box.width <= 0 || box.height <= 0)
        return;
    const SkAutoCanvasRestore restore(canvas, true);
    canvas->clipRect(rect(box));
    if (canvas->getDeviceClipBounds().isEmpty())
        return;
    draw_icon(canvas,
              value,
              box.x + (box.width - size) / 2,
              box.y + (box.height - size) / 2,
              size,
              color);
}

std::string diagnostic_caption(std::string_view original) {
    // The full error remains in host state. Bound the displayed caption, keep
    // scalar boundaries, and make malformed host bytes visible as replacements.
    std::string result;
    for (std::size_t offset = 0; offset < original.size() && result.size() < 4096;) {
        std::size_t bytes{};
        try {
            bytes = utf8::decode(original, offset).bytes;
        } catch (const std::invalid_argument&) {
            if (result.size() + 3 > 4096)
                break;
            result += "\xEF\xBF\xBD";
            ++offset;
            continue;
        }
        if (result.size() + bytes > 4096)
            break;
        const auto scalar = original.substr(offset, bytes);
        result += scalar == "\n" || scalar == "\r" || scalar == std::string_view("\0", 1)
                      ? std::string_view(" ")
                      : scalar;
        offset += bytes;
    }
    return result;
}
} // namespace

void WorkbenchChrome::caption(SkCanvas* canvas,
                              Box bounds,
                              std::string_view text,
                              const TextLayoutOptions& style) {
    if (bounds.width <= 0 || bounds.height <= 0 || text.empty())
        return;
    const SkAutoCanvasRestore restore(canvas, true);
    canvas->clipRect(rect(bounds));
    if (canvas->getDeviceClipBounds().isEmpty())
        return;
    if (const auto paragraph = labels_.elide(text, style, bounds.width))
        paragraph->draw(
            canvas, bounds.x, bounds.y + std::max(0.0F, (bounds.height - paragraph->height()) / 2));
}

void WorkbenchChrome::paint_header(SkCanvas* canvas,
                                   const WorkbenchLayout& geometry,
                                   const WorkbenchStyle& theme,
                                   const Header& model) {
    if (!canvas)
        throw std::invalid_argument("Workbench chrome requires a canvas");
    const SkAutoCanvasRestore restore(canvas, true);
    canvas->clipRect(rect(geometry.header));
    if (canvas->getDeviceClipBounds().isEmpty())
        return;
    const auto scale = geometry.scale;
    fill(canvas, geometry.header, theme.colors.activity);
    fill(canvas,
         {geometry.header.x, geometry.header.bottom() - scale, geometry.header.width, scale},
         theme.colors.border);
    const auto search = geometry.command_center;
    if (search.x >= 72 * scale)
        caption(canvas,
                {14 * scale, geometry.header.y, search.x - 28 * scale, geometry.header.height},
                model.brand,
                theme.text(12, scale, theme.colors.muted));
    const SkAutoCanvasRestore search_restore(canvas, true);
    canvas->clipRect(rect(search));
    if (canvas->getDeviceClipBounds().isEmpty())
        return;
    SkPaint paint;
    paint.setColor(theme.colors.input);
    paint.setAntiAlias(true);
    canvas->drawRoundRect(rect(search), 4 * scale, 4 * scale, paint);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(scale);
    paint.setColor(search.contains(model.pointer_x, model.pointer_y) ? theme.colors.accent
                                                                     : theme.colors.border);
    canvas->drawRoundRect(rect(search), 4 * scale, 4 * scale, paint);
    icon(canvas,
         model.command_icon,
         {search.x + 9 * scale, search.y, 15 * scale, search.height},
         15 * scale,
         theme.colors.muted);
    float hint_width{};
    const auto hint_style = theme.text(10, scale, theme.colors.muted);
    if (search.width > 260 * scale && !model.hint.empty()) {
        hint_width = std::min(labels_.prepare(model.hint, hint_style)->width(), search.width / 3);
        caption(canvas,
                {search.right() - 8 * scale - hint_width, search.y, hint_width, search.height},
                model.hint,
                hint_style);
    }
    caption(canvas,
            {search.x + 31 * scale,
             search.y,
             std::max(0.0F, search.width - 39 * scale - (hint_width ? hint_width + 12 * scale : 0)),
             search.height},
            model.workspace,
            theme.text(12, scale, theme.colors.foreground));
}

void WorkbenchChrome::paint_sidebar(SkCanvas* canvas,
                                    const WorkbenchLayout& geometry,
                                    const WorkbenchStyle& theme,
                                    const Sidebar& model) {
    if (!canvas)
        throw std::invalid_argument("Workbench chrome requires a canvas");
    const SkAutoCanvasRestore restore(canvas, true);
    canvas->clipRect(rect(geometry.sidebar));
    if (canvas->getDeviceClipBounds().isEmpty())
        return;
    const auto scale = geometry.scale;
    const auto heading = geometry.sidebar_heading;
    const auto refresh = geometry.sidebar_refresh;
    fill(canvas, heading, theme.colors.sidebar);
    caption(canvas,
            {heading.x + 16 * scale,
             heading.y,
             std::max(0.0F, refresh.x - heading.x - 24 * scale),
             heading.height},
            model.title,
            theme.text(11, scale, theme.colors.foreground));
    const auto hovered = refresh.contains(model.pointer_x, model.pointer_y);
    if (hovered)
        fill(canvas, refresh, theme.colors.hover);
    icon(canvas,
         model.busy && hovered ? Icon::close : Icon::refresh,
         refresh,
         16 * scale,
         model.busy ? theme.colors.accent : theme.colors.muted);
    const auto section = geometry.sidebar_section;
    fill(canvas,
         section,
         section.contains(model.pointer_x, model.pointer_y) ? theme.colors.hover
                                                            : theme.colors.sidebar);
    icon(canvas,
         model.expanded ? Icon::chevron_down : Icon::chevron_right,
         {section.x + 6 * scale, section.y, 16 * scale, section.height},
         16 * scale,
         theme.colors.muted);
    caption(canvas,
            {section.x + 24 * scale,
             section.y,
             std::max(0.0F, section.width - 30 * scale),
             section.height},
            model.subtitle,
            theme.text(12, scale, theme.colors.foreground));
    const auto outline = geometry.outline_header;
    fill(canvas,
         outline,
         outline.contains(model.pointer_x, model.pointer_y) ? theme.colors.hover
                                                            : theme.colors.sidebar);
    icon(canvas,
         model.outline_expanded ? Icon::chevron_down : Icon::chevron_right,
         {outline.x + 6 * scale, outline.y, 16 * scale, outline.height},
         16 * scale,
         theme.colors.muted);
    caption(canvas,
            {outline.x + 27 * scale,
             outline.y,
             std::max(0.0F, outline.width - 33 * scale),
             outline.height},
            model.outline_title,
            theme.text(11, scale, theme.colors.foreground));
    fill(canvas,
         {geometry.sidebar.right() - scale, geometry.sidebar.y, scale, geometry.sidebar.height},
         theme.colors.border);
}

std::optional<WorkbenchChrome::Item> WorkbenchChrome::OuterLayout::hit(float x, float y) const {
    for (const auto& item : items)
        if (item.bounds.contains(x, y))
            return item;
    return std::nullopt;
}

WorkbenchChrome::OuterLayout WorkbenchChrome::measure_outer(const WorkbenchLayout& geometry,
                                                            const WorkbenchStyle& theme,
                                                            const ChromeState& source,
                                                            std::size_t activity_first,
                                                            bool diagnostic) {
    OuterLayout result;
    const auto scale = geometry.scale;
    const auto add = [&](ChromeBank bank, std::size_t index, Box box) {
        if (box.width > 0 && box.height > 0)
            result.items.push_back({bank, index, box});
    };
    if (source.rows(ChromeBank::command).size())
        add(ChromeBank::command, 0, geometry.command_center);
    auto activity = geometry.activity;
    const auto& views = source.rows(ChromeBank::activity);
    const auto step = std::round(48 * scale);
    if (source.rows(ChromeBank::manage).size()) {
        const auto height = std::min(activity.height, std::round(40 * scale));
        add(ChromeBank::manage,
            0,
            {activity.x, activity.bottom() - height, activity.width, height});
        activity.height -= height;
    }
    if (static_cast<float>(views.size()) * step > activity.height) {
        const auto height = std::min(std::round(24 * scale), activity.height / 2);
        result.previous = {activity.x, activity.y, activity.width, height};
        result.next = {activity.x, activity.bottom() - height, activity.width, height};
        activity.y += height;
        activity.height -= 2 * height;
    }
    result.activity_page =
        static_cast<std::size_t>(std::max(0.0F, std::floor(activity.height / step)));
    const auto maximum =
        views.size() > result.activity_page ? views.size() - result.activity_page : 0;
    result.activity_first = std::min(activity_first, maximum);
    const auto end = std::min(views.size(), result.activity_first + result.activity_page);
    for (auto index = result.activity_first; index < end; ++index)
        add(ChromeBank::activity,
            index,
            {activity.x,
             activity.y + static_cast<float>(index - result.activity_first) * step,
             activity.width,
             step});
    const auto width = [&](const ViewRow& row, float reference) {
        const auto& value = *row.value;
        return labels_
                   .prepare(value.at("label").get_ref<const std::string&>(),
                            theme.text(reference, scale, theme.colors.foreground))
                   ->width() +
               (value.value("icon", std::string{}).empty() ? 0 : 22 * scale) + 16 * scale;
    };
    // Keep the final path components visible. A single oversized leaf is elided.
    auto breadcrumb = geometry.breadcrumb;
    breadcrumb.x += std::min(8 * scale, breadcrumb.width);
    breadcrumb.width = std::max(0.0F, breadcrumb.width - 16 * scale);
    const auto& paths = source.rows(ChromeBank::breadcrumb);
    float used{};
    std::size_t first = paths.size();
    while (first > 0 && breadcrumb.height > 0 && breadcrumb.width > 0) {
        const auto wanted = width(paths.at(first - 1), 12) + (used > 0 ? 12 * scale : 0);
        if (used > 0 && used + wanted > breadcrumb.width)
            break;
        used = std::min(breadcrumb.width, used + wanted);
        --first;
    }
    auto x = breadcrumb.x;
    for (auto index = first; index < paths.size(); ++index) {
        const auto extent = std::min(width(paths.at(index), 12), breadcrumb.right() - x);
        add(ChromeBank::breadcrumb, index, {x, breadcrumb.y, extent, breadcrumb.height});
        x += extent + 12 * scale;
    }
    const auto status = geometry.status;
    // Earlier right-hand items have higher admission priority, preserving caret
    // coordinates while lower-priority details disappear in narrow windows.
    const auto& right = source.rows(ChromeBank::status_right);
    const auto budget = std::max(0.0F, std::min(status.width * 0.65F, status.width - 44 * scale));
    std::vector<float> widths;
    used = 0;
    for (std::size_t index = 0; index < right.size() && status.height > 0; ++index) {
        const auto wanted = width(right.at(index), 11);
        if (used > 0 && wanted > budget - used)
            break;
        const auto admitted = std::min(wanted, budget - used);
        if (admitted <= 0)
            break;
        widths.push_back(admitted);
        used += admitted;
    }
    const auto right_begin = status.right() - used;
    x = right_begin;
    for (std::size_t index = 0; index < widths.size(); ++index) {
        add(ChromeBank::status_right, index, {x, status.y, widths[index], status.height});
        x += widths[index];
    }
    const auto& left = source.rows(ChromeBank::status_left);
    x = status.x;
    for (std::size_t index = 0; index < left.size() && status.height > 0; ++index) {
        const auto extent =
            std::min(width(left.at(index), 11), std::max(0.0F, right_begin - x - 8 * scale));
        if (extent <= 0)
            break;
        add(ChromeBank::status_left, index, {x, status.y, extent, status.height});
        x += extent;
    }
    const auto editor = geometry.editor;
    const auto pad = std::min(12 * scale, std::min(editor.width, editor.height) / 8);
    const auto available_width = std::max(0.0F, editor.width - 2 * pad);
    auto bottom = editor.bottom() - pad;
    const auto gap = std::min(6 * scale, editor.height / 16);
    auto notice_budget = std::max(0.0F, editor.height * 0.45F - pad);
    const auto banner_height = std::max(
        36 * scale, std::ceil(theme.text(12, scale, 0).font_size * scale * 1.15F + 16 * scale));
    if (diagnostic && available_width > 0 && notice_budget > 0) {
        const auto extent = std::min(banner_height, notice_budget);
        result.diagnostic = {editor.x + pad, bottom - extent, available_width, extent};
        bottom -= extent + gap;
        notice_budget -= extent + gap;
    }
    const auto& notices = source.rows(ChromeBank::notices);
    for (std::size_t index = 0; index < notices.size() && notice_budget >= banner_height; ++index) {
        add(ChromeBank::notices,
            index,
            {editor.x + pad, bottom - banner_height, available_width, banner_height});
        bottom -= banner_height + gap;
        notice_budget -= banner_height + gap;
    }
    const bool empty = !source.context().is_object() || !source.context().contains("document") ||
                       source.context().at("document").is_null();
    if (empty && available_width > 0 && bottom > editor.y + pad) {
        const auto welcome_width = std::min(500 * scale, available_width);
        const auto welcome_x = editor.x + (editor.width - welcome_width) / 2;
        auto y = editor.y + pad + std::min(64 * scale, (bottom - editor.y - pad) / 6);
        for (const auto [bank, reference] : {std::pair{ChromeBank::welcome_title, 32.0F},
                                             std::pair{ChromeBank::welcome_detail, 14.0F},
                                             std::pair{ChromeBank::welcome_actions, 13.0F}}) {
            const auto& rows = source.rows(bank);
            for (std::size_t index = 0; index < rows.size() && y < bottom; ++index) {
                const auto height =
                    labels_
                        .prepare(rows.at(index).value->at("label").get_ref<const std::string&>(),
                                 theme.text(reference, scale, 0))
                        ->height();
                const auto desired =
                    height + (bank == ChromeBank::welcome_actions ? 16 : 8) * scale;
                if (index > 0 && desired > bottom - y)
                    break;
                const auto extent = std::min(desired, bottom - y);
                add(bank, index, {welcome_x, y, welcome_width, extent});
                y += extent + 8 * scale;
            }
        }
    }
    return result;
}

namespace {
std::uint32_t tone(std::string_view name, const WorkbenchStyle& theme, std::uint32_t normal) {
    if (name == "muted")
        return theme.colors.muted;
    if (name == "accent")
        return theme.colors.accent;
    if (name == "added")
        return theme.colors.added;
    if (name == "changed")
        return theme.colors.changed;
    if (name == "error")
        return theme.colors.error;
    return normal;
}
} // namespace

void WorkbenchChrome::paint_outer(SkCanvas* canvas,
                                  const WorkbenchLayout& geometry,
                                  const WorkbenchStyle& theme,
                                  const ChromeState& source,
                                  const OuterLayout& layout,
                                  float pointer_x,
                                  float pointer_y) {
    if (!canvas)
        throw std::invalid_argument("Workbench chrome requires a canvas");
    const SkAutoCanvasRestore restore(canvas, true);
    const auto scale = geometry.scale;
    canvas->clipRect(SkRect::MakeLTRB(0, 0, geometry.status.right(), geometry.status.bottom()));
    const auto& command = source.rows(ChromeBank::command);
    paint_header(canvas,
                 geometry,
                 theme,
                 {source.model().at("brand").get_ref<const std::string&>(),
                  command.size() ? command.at(0).value->at("label").get_ref<const std::string&>()
                                 : std::string_view{},
                  source.model().at("hint").get_ref<const std::string&>(),
                  pointer_x,
                  pointer_y,
                  command.size() ? view_icon(command.at(0).value->value("icon", std::string{}))
                                 : Icon::document});
    fill(canvas, geometry.activity, theme.colors.activity);
    fill(canvas, geometry.breadcrumb, theme.colors.background);
    fill(canvas, geometry.status, theme.colors.status);
    fill(canvas,
         {geometry.activity.right() - scale, geometry.activity.y, scale, geometry.activity.height},
         theme.colors.border);
    fill(canvas,
         {geometry.breadcrumb.x,
          geometry.breadcrumb.bottom() - scale,
          geometry.breadcrumb.width,
          scale},
         theme.colors.border);
    fill(canvas,
         {geometry.status.x, geometry.status.y, geometry.status.width, scale},
         theme.colors.border);
    for (const auto& item : layout.items) {
        if (item.bank == ChromeBank::command || item.bank >= ChromeBank::welcome_title)
            continue;
        const auto& value = *source.rows(item.bank).at(item.index).value;
        const SkAutoCanvasRestore row_restore(canvas, true);
        canvas->clipRect(rect(item.bounds));
        if (canvas->getDeviceClipBounds().isEmpty())
            continue;
        const auto hovered = item.bounds.contains(pointer_x, pointer_y) && value.contains("data");
        if (hovered)
            fill(canvas, item.bounds, theme.colors.hover);
        const bool activity = item.bank == ChromeBank::activity || item.bank == ChromeBank::manage;
        const bool status =
            item.bank == ChromeBank::status_left || item.bank == ChromeBank::status_right;
        const auto selected = value.value("selected", false);
        const auto normal = activity ? (selected ? theme.colors.foreground : theme.colors.muted)
                            : status ? theme.colors.status_foreground
                                     : theme.colors.muted;
        const auto color = tone(value.value("tone", std::string{}), theme, normal);
        auto text = item.bounds;
        text.x += 8 * scale;
        text.width = std::max(0.0F, text.width - 16 * scale);
        const auto glyph = value.value("icon", std::string{});
        if (!glyph.empty()) {
            const auto icon_color = tone(value.value("iconTone", std::string{}), theme, color);
            const auto box = activity ? item.bounds : Box{text.x, text.y, 16 * scale, text.height};
            icon(canvas, view_icon(glyph), box, (activity ? 24 : 16) * scale, icon_color);
            text.x += 22 * scale;
            text.width = std::max(0.0F, text.width - 22 * scale);
        }
        if (activity) {
            if (selected)
                fill(canvas,
                     {item.bounds.x, item.bounds.y, 2 * scale, item.bounds.height},
                     theme.colors.accent);
        } else {
            caption(canvas,
                    text,
                    value.at("label").get_ref<const std::string&>(),
                    theme.text(status ? 11.0F : 12.0F, scale, color));
        }
    }
    for (const auto& item : layout.items)
        if (item.bank == ChromeBank::breadcrumb && item.index + 1 < source.rows(item.bank).size())
            icon(canvas,
                 Icon::chevron_right,
                 {item.bounds.right(), item.bounds.y, 12 * scale, item.bounds.height},
                 12 * scale,
                 theme.colors.muted);
    for (const auto [box, previous] :
         {std::pair{layout.previous, true}, std::pair{layout.next, false}}) {
        if (box.contains(pointer_x, pointer_y))
            fill(canvas, box, theme.colors.hover);
        const SkAutoCanvasRestore control_restore(canvas, true);
        canvas->clipRect(rect(box));
        if (previous) {
            canvas->rotate(180, box.x + box.width / 2, box.y + box.height / 2);
        }
        icon(canvas, Icon::chevron_down, box, 16 * scale, theme.colors.muted);
    }
}

void WorkbenchChrome::paint_body(SkCanvas* canvas,
                                 const WorkbenchLayout& geometry,
                                 const WorkbenchStyle& theme,
                                 const ChromeState& source,
                                 const OuterLayout& layout,
                                 float pointer_x,
                                 float pointer_y,
                                 std::string_view diagnostic) {
    if (!canvas)
        throw std::invalid_argument("Workbench body requires a canvas");
    const SkAutoCanvasRestore restore(canvas, true);
    canvas->clipRect(rect(geometry.editor));
    const auto scale = geometry.scale;
    for (const auto& item : layout.items) {
        if (item.bank < ChromeBank::welcome_title)
            continue;
        const SkAutoCanvasRestore row_restore(canvas, true);
        canvas->clipRect(rect(item.bounds));
        if (canvas->getDeviceClipBounds().isEmpty())
            continue;
        const auto& value = *source.rows(item.bank).at(item.index).value;
        const auto role = value.value("tone", std::string{});
        const auto notice = item.bank == ChromeBank::notices;
        const auto action = item.bank == ChromeBank::welcome_actions;
        const auto title = item.bank == ChromeBank::welcome_title;
        const auto color =
            notice && role == "changed"
                ? theme.colors.warning
                : tone(role,
                       theme,
                       item.bank == ChromeBank::welcome_detail ? theme.colors.muted
                                                               : theme.colors.foreground);
        if (notice) {
            fill(canvas,
                 item.bounds,
                 role == "error"     ? theme.colors.error_background
                 : role == "added"   ? theme.colors.recovery_background
                 : role == "changed" ? theme.colors.notice_background
                                     : theme.colors.panel);
            fill(canvas, {item.bounds.x, item.bounds.y, 2 * scale, item.bounds.height}, color);
        } else if (action) {
            fill(canvas, item.bounds, theme.colors.panel);
        }
        if ((action || notice) && value.contains("data") &&
            item.bounds.contains(pointer_x, pointer_y))
            fill(canvas, item.bounds, theme.colors.hover);
        auto text = item.bounds;
        const auto inset = std::min((notice || action ? 12 : 0) * scale, text.width / 2);
        text.x += inset;
        text.width = std::max(0.0F, text.width - 2 * inset);
        const auto glyph = value.value("icon", std::string{});
        if (!glyph.empty()) {
            icon(canvas,
                 view_icon(glyph),
                 {text.x, text.y, 18 * scale, text.height},
                 18 * scale,
                 tone(value.value("iconTone", std::string{}), theme, color));
            const auto extent = std::min(26 * scale, text.width);
            text.x += extent;
            text.width -= extent;
        }
        const auto badge = value.value("badge", std::string{});
        if (!badge.empty()) {
            const auto style = theme.text(11, scale, theme.colors.muted);
            const auto width = std::min(labels_.prepare(badge, style)->width(), text.width * 0.35F);
            caption(canvas, {text.right() - width, text.y, width, text.height}, badge, style);
            text.width = std::max(0.0F, text.width - width - 12 * scale);
        }
        caption(canvas,
                text,
                value.at("label").get_ref<const std::string&>(),
                theme.text(title                                     ? 32.0F
                           : item.bank == ChromeBank::welcome_detail ? 14.0F
                           : notice                                  ? 12.0F
                                                                     : 13.0F,
                           scale,
                           color));
    }
    if (!diagnostic.empty() && layout.diagnostic.width > 0 && layout.diagnostic.height > 0) {
        const auto box = layout.diagnostic;
        fill(canvas, box, theme.colors.error_background);
        fill(canvas, {box.x, box.y, 2 * scale, box.height}, theme.colors.error);
        const auto inset = std::min(12 * scale, box.width / 2);
        caption(canvas,
                {box.x + inset, box.y, std::max(0.0F, box.width - 2 * inset), box.height},
                diagnostic_caption(diagnostic),
                theme.text(12, scale, theme.colors.foreground));
    }
}

} // namespace astra::ui
