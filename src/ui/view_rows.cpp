#include <astra/ui/view_rows.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace astra::ui {
namespace {
void fill(SkCanvas* canvas, Box box, std::uint32_t color) {
    SkPaint paint;
    paint.setColor(color);
    canvas->drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), paint);
}
} // namespace

std::size_t ViewRows::paint(SkCanvas* canvas,
                            const ViewIndex& source,
                            const ViewRowsLayout& layout,
                            const WorkbenchStyle& theme,
                            const Model& model,
                            const ResolveIcon& resolve_icon) {
    if (!canvas || !source.ready() || layout.end() > source.size())
        throw std::invalid_argument("View rows require a canvas and matching ready source");
    if (!model.expanded || layout.bounds().width <= 0 || layout.bounds().height <= 0)
        return 0;
    const SkAutoCanvasRestore restore(canvas, true);
    const auto bounds = layout.bounds();
    canvas->clipRect(SkRect::MakeXYWH(bounds.x, bounds.y, bounds.width, bounds.height));
    if (canvas->isClipEmpty())
        return 0;
    const auto scale = layout.scale();
    const auto label = [&](std::string_view text,
                           float x,
                           float y,
                           float reference_size,
                           float available,
                           std::uint32_t color) {
        if (available <= 0)
            return;
        const auto style = theme.text(reference_size, scale, color);
        if (const auto paragraph = labels_.elide(text, style, available)) {
            const SkAutoCanvasRestore caption_restore(canvas, true);
            canvas->clipRect(SkRect::MakeXYWH(x, y, available, paragraph->height()));
            paragraph->draw(canvas, x, y);
        }
    };
    std::size_t painted{};
    source.for_each_range(layout.first(), layout.end(), [&](std::size_t index, const ViewRow& row) {
        const auto& item = *row.value;
        const auto& id = item.at("id").get_ref<const std::string&>();
        const auto box = layout.row(index);
        const auto x = bounds.x + (8 + 12 * static_cast<float>(row.depth)) * scale;
        const auto selected = model.cursor == id;
        if (selected || item.value("selected", false))
            fill(canvas,
                 box,
                 model.focused ? theme.colors.selection : theme.colors.inactive_selection);
        else if (box.contains(model.pointer_x, model.pointer_y))
            fill(canvas, box, theme.colors.hover);
        if (model.focused && selected)
            fill(canvas, {box.x, box.y, 2 * scale, box.height}, theme.colors.accent);
        if (row.group)
            icons_.draw(canvas,
                        source.collapsed(id) ? Icon::chevron_right : Icon::chevron_down,
                        x,
                        box.y + 4 * scale,
                        16 * scale,
                        theme.colors.muted);
        const auto projected = resolve_icon ? resolve_icon(row) : std::nullopt;
        const auto icon =
            projected ? projected->icon : view_icon(item.value("icon", std::string{}));
        const auto icon_tone = item.value("iconTone", std::string{});
        const auto icon_color = icon_tone == "normal"    ? theme.colors.foreground
                                : icon_tone == "muted"   ? theme.colors.muted
                                : icon_tone == "accent"  ? theme.colors.accent
                                : icon_tone == "added"   ? theme.colors.added
                                : icon_tone == "changed" ? theme.colors.changed
                                : icon_tone == "error"   ? theme.colors.error
                                : icon == Icon::folder   ? theme.colors.folder
                                : icon == Icon::code     ? theme.colors.code_icon
                                : icon == Icon::braces   ? theme.colors.script_icon
                                                         : theme.colors.muted;
        icons_.draw(canvas,
                    icon,
                    x + 18 * scale,
                    box.y + 4 * scale,
                    16 * scale,
                    projected ? projected->color : icon_color);
        const auto tone = item.value("tone", std::string{});
        const auto color = tone == "error"     ? theme.colors.error
                           : tone == "added"   ? theme.colors.added
                           : tone == "changed" ? theme.colors.changed
                           : tone == "muted"   ? theme.colors.muted
                                               : theme.colors.foreground;
        const auto badge = item.value("badge", std::string{});
        const auto badge_width = badge.empty() ? 0.0F : 48 * scale;
        label(item.at("label").get_ref<const std::string&>(),
              x + 42 * scale,
              box.y + 4 * scale,
              13,
              std::max(0.0F, bounds.right() - x - 48 * scale - badge_width),
              color);
        if (!badge.empty())
            label(badge,
                  bounds.right() - badge_width,
                  box.y + 4 * scale,
                  12,
                  badge_width - 8 * scale,
                  color);
        ++painted;
    });
    if (!source.size() && model.expanded)
        label(model.empty,
              bounds.x + 16 * scale,
              bounds.y + 12 * scale,
              12,
              bounds.width - 32 * scale,
              theme.colors.muted);
    return painted;
}
} // namespace astra::ui
