#include <astra/ui/workbench_chrome.hpp>
#include <astra/ui/icons.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include <algorithm>
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
         Icon::search,
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

} // namespace astra::ui
