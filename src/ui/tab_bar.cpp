#include <astra/ui/tab_bar.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
void fill(SkCanvas* canvas, Box box, std::uint32_t color) {
    SkPaint paint;
    paint.setColor(color);
    canvas->drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), paint);
}
} // namespace

void TabBar::measure(Box bounds,
                     float scale,
                     TabIndex& tabs,
                     std::size_t active,
                     const WorkbenchStyle& theme,
                     bool prepare_text) {
    // Validate geometry before changing the measured model or presentation.
    auto strip = TabStrip::measure(bounds, scale, tabs.snapshot(), first_, active, reveal_);
    active = tabs.empty() ? 0 : std::min(active, tabs.size() - 1);
    const auto style = theme.text(13, scale, theme.colors.foreground);
    auto metric_style = style;
    metric_style.color = 0;
    if (!measure_style_ || *measure_style_ != metric_style) {
        tabs.invalidate();
        measure_style_ = std::move(metric_style);
    }
    if (prepare_text) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
        std::size_t measured{};
        const auto prepare = [&](std::size_t position) {
            if (!tabs.prepared(position) && measured < 8 &&
                std::chrono::steady_clock::now() < until) {
                const auto width = labels_.prepare(tabs.at(position).label, style)->width();
                tabs.measured(position, width, scale);
                ++measured;
            }
        };
        for (const auto& tab : strip.visible) {
            prepare(tab.index);
        }
        while (measured < 8 && std::chrono::steady_clock::now() < until) {
            const auto pending = tabs.next_pending();
            if (!pending) {
                break;
            }
            prepare(*pending);
        }
        const auto keep_active =
            reveal_ || std::ranges::any_of(strip.visible, [&](const TabBox& tab) {
                return tab.index == active;
            });
        if (measured) {
            strip = TabStrip::measure(bounds, scale, tabs.snapshot(), first_, active, keep_active);
        }
        first_ = strip.first;
        max_measured_pass_ = std::max(max_measured_pass_, measured);
        reveal_ = false;
    }
    bounds_ = bounds;
    scale_ = scale;
    strip_ = std::move(strip);
}

void TabBar::paint(SkCanvas* canvas,
                   const TabIndex& tabs,
                   std::size_t active,
                   const WorkbenchStyle& theme,
                   const std::function<Decoration(std::size_t)>& decoration) {
    if (!canvas) {
        throw std::invalid_argument("Tab bar requires a canvas");
    }
    const SkAutoCanvasRestore restore(canvas, true);
    canvas->clipRect(SkRect::MakeXYWH(bounds_.x, bounds_.y, bounds_.width, bounds_.height));
    fill(canvas, bounds_, theme.colors.tabs);
    for (const auto& tab : strip_.visible) {
        const auto item = decoration(tab.index);
        const auto selected = tab.index == active;
        if (selected) {
            fill(canvas, tab.bounds, theme.colors.active_tab);
            fill(canvas,
                 {tab.bounds.x, tab.bounds.y, tab.bounds.width, 2 * scale_},
                 theme.colors.accent);
        }
        fill(canvas,
             {tab.bounds.right() - scale_, tab.bounds.y, scale_, tab.bounds.height},
             theme.colors.border);
        draw_icon(canvas,
                  item.icon,
                  tab.bounds.x + 12 * scale_,
                  tab.bounds.y + 9 * scale_,
                  16 * scale_,
                  item.color);
        {
            const SkAutoCanvasRestore caption_restore(canvas, true);
            const auto caption_x = tab.bounds.x + 34 * scale_;
            const auto available = tab.bounds.width - 62 * scale_;
            canvas->clipRect(
                SkRect::MakeXYWH(caption_x, tab.bounds.y, available, tab.bounds.height));
            const auto style =
                theme.text(13, scale_, selected ? theme.colors.foreground : theme.colors.muted);
            if (const auto label = labels_.elide(tabs.at(tab.index).label, style, available)) {
                label->draw(canvas, caption_x, tab.bounds.y + 10 * scale_);
            }
        }
        if (item.dirty) {
            SkPaint dirty;
            dirty.setColor(selected ? theme.colors.foreground : theme.colors.muted);
            dirty.setAntiAlias(true);
            canvas->drawCircle(
                tab.close.x + 13 * scale_, tab.close.y + 17 * scale_, 3 * scale_, dirty);
        } else {
            draw_icon(canvas,
                      Icon::close,
                      tab.close.x + 6 * scale_,
                      tab.close.y + 10 * scale_,
                      14 * scale_,
                      theme.colors.muted);
        }
    }
    if (strip_.previous.width > 0) {
        fill(canvas, strip_.previous, theme.colors.tabs);
        fill(canvas, strip_.next, theme.colors.tabs);
        {
            const SkAutoCanvasRestore previous_restore(canvas, true);
            canvas->translate(strip_.previous.x + 18 * scale_, strip_.previous.y + 10 * scale_);
            canvas->scale(-1, 1);
            draw_icon(canvas, Icon::chevron_right, 0, 0, 14 * scale_, theme.colors.muted);
        }
        draw_icon(canvas,
                  Icon::chevron_right,
                  strip_.next.x + 6 * scale_,
                  strip_.next.y + 10 * scale_,
                  14 * scale_,
                  theme.colors.muted);
    }
}

std::optional<TabBar::Hit> TabBar::hit(float x, float y) const {
    if (!bounds_.contains(x, y)) {
        return {};
    }
    if (strip_.previous.contains(x, y)) {
        return Hit{Target::previous};
    }
    if (strip_.next.contains(x, y)) {
        return Hit{Target::next};
    }
    for (const auto& tab : strip_.visible) {
        if (tab.bounds.contains(x, y)) {
            return Hit{tab.close.contains(x, y) ? Target::close : Target::caption, tab.index};
        }
    }
    return {};
}

void TabBar::scroll(int steps, std::size_t count) {
    if (count > TabIndex::max_tabs) {
        throw std::length_error("Tab scroll exceeds the model's count bound");
    }
    first_ = static_cast<std::size_t>(std::clamp(static_cast<long long>(first_) + steps,
                                                 0LL,
                                                 static_cast<long long>(count ? count - 1 : 0)));
    reveal_ = false;
}
} // namespace astra::ui
