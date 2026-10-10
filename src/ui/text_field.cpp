#include <astra/ui/text_field.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace astra::ui {
namespace {
void validate_bounds(Box box, float dpi) {
    if (!std::isfinite(box.x) || !std::isfinite(box.y) || !std::isfinite(box.width) ||
        !std::isfinite(box.height) || !std::isfinite(box.right()) || !std::isfinite(box.bottom()) ||
        box.width < 0 || box.height < 0 || !std::isfinite(dpi) || dpi <= 0) {
        throw std::invalid_argument("Text field requires finite bounds and positive DPI");
    }
}

Box inset(Box box, float x, float y) {
    x = std::min(x, box.width / 2);
    y = std::min(y, box.height / 2);
    return {box.x + x,
            box.y + y,
            std::max(0.0F, box.width - 2 * x),
            std::max(0.0F, box.height - 2 * y)};
}

void fill(SkCanvas* canvas, Box box, std::uint32_t color) {
    SkPaint paint;
    paint.setColor(color);
    canvas->drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), paint);
}

void border(SkCanvas* canvas, const TextFieldPaint& paint) {
    const auto& colors = paint.colors;
    fill(canvas,
         paint.bounds,
         paint.invalid ? colors.error : (paint.focused ? colors.focus : colors.border));
    fill(canvas,
         inset(paint.bounds, paint.text_style.dpi_scale, paint.text_style.dpi_scale),
         colors.background);
}

void caption(SkCanvas* canvas,
             TextLabels& labels,
             std::string_view text,
             const TextLayoutOptions& style,
             Box content) {
    if (const auto layout = labels.elide(text, style, content.width)) {
        layout->draw(canvas, content.x, content.y);
    }
}

std::optional<Box> visible_area(Box caret, Box clip) {
    // A fully collapsed control must not move the platform candidate window.
    if (clip.width <= 0 || clip.height <= 0) {
        return {};
    }
    caret.width = std::min(caret.width, clip.width);
    caret.height = std::min(caret.height, clip.height);
    caret.x = std::clamp(caret.x, clip.x, clip.right() - caret.width);
    caret.y = std::clamp(caret.y, clip.y, clip.bottom() - caret.height);
    return caret;
}

// Balanced canvas lifetime, including native shaping/drawing failures.
class CanvasScope final {
  public:
    CanvasScope(SkCanvas* canvas, Box clip) : canvas_(canvas) {
        canvas_->save();
        canvas_->clipRect(SkRect::MakeXYWH(clip.x, clip.y, clip.width, clip.height));
    }

    ~CanvasScope() {
        canvas_->restore();
    }

    CanvasScope(const CanvasScope&) = delete;
    CanvasScope& operator=(const CanvasScope&) = delete;

  private:
    SkCanvas* canvas_;
};
} // namespace

TextField::TextField(const TextLayoutEngine& fonts, std::shared_ptr<LayoutJobs> jobs)
    : text_(fonts, jobs), composition_(fonts, std::move(jobs)) {
}

void TextField::poll() {
    text_.poll();
    composition_.poll();
}

bool TextField::pending() const noexcept {
    return text_.pending() || composition_.pending();
}

bool TextField::composition_pending() const noexcept {
    return composition_.pending();
}

TextInputLayout& TextField::text() noexcept {
    return text_;
}

const TextInputLayout& TextField::text() const noexcept {
    return text_;
}

const TextLayout* TextField::composition_layout() const noexcept {
    return composition_.layout();
}

std::pair<float, float> TextField::to_text(Box box, float dpi, float x, float y) const {
    validate_bounds(box, dpi);
    if (!std::isfinite(x) || !std::isfinite(y)) {
        throw std::invalid_argument("Text field pointer coordinates must be finite");
    }
    const auto content = inset(box, 6 * dpi, 5 * dpi);
    return {x - content.x + text_.horizontal, y - content.y};
}

std::optional<Box> TextField::draw_unavailable(SkCanvas* canvas,
                                               TextLabels& labels,
                                               const TextFieldPaint& paint,
                                               bool failed) {
    const auto dpi = paint.text_style.dpi_scale;
    validate_bounds(paint.bounds, dpi);
    const auto clip = inset(paint.bounds, 2 * dpi, dpi);
    const auto content = inset(paint.bounds, 6 * dpi, 5 * dpi);
    CanvasScope scope(canvas, paint.bounds);
    auto unavailable = paint;
    unavailable.invalid = unavailable.invalid || failed;
    border(canvas, unavailable);
    CanvasScope inner(canvas, clip);
    caption(canvas, labels, failed ? "Text unavailable" : "…", paint.hint_style, content);
    return paint.focused ? visible_area({content.x, content.y, dpi, clip.height}, clip)
                         : std::nullopt;
}

std::optional<Box>
TextField::draw(SkCanvas* canvas, TextLabels& labels, const TextFieldPaint& paint) {
    const auto dpi = paint.text_style.dpi_scale;
    validate_bounds(paint.bounds, dpi);
    if (paint.text_style.wrap_width != 0 || !paint.text_style.color_spans.empty() ||
        paint.selection.begin().value > paint.text.size() ||
        paint.selection.end().value > paint.text.size()) {
        throw std::invalid_argument(
            "Text field requires plain single-line text and valid selection");
    }
    if (!paint.focused || paint.composition.empty()) {
        composition_.reset();
    }
    const auto* input = text_.prepare_async(paint.text, paint.text_style);
    if (!input) {
        return draw_unavailable(canvas, labels, paint, false);
    }
    const auto clip = inset(paint.bounds, 2 * dpi, dpi);
    const auto content = inset(paint.bounds, 6 * dpi, 5 * dpi);
    const auto caret = input->caret(paint.selection.head.value);
    // Reserve the caret stroke itself; the last character's caret must remain
    // inside the visible content after scrolling, including RTL visual positions.
    const auto available = std::max(0.0F, content.width - dpi);
    if (paint.focused) {
        if (caret.x < text_.horizontal) {
            text_.horizontal = caret.x;
        }
        if (caret.x > text_.horizontal + available) {
            text_.horizontal = caret.x - available;
        }
    }
    text_.horizontal =
        std::clamp(text_.horizontal, 0.0F, std::max(0.0F, input->width() - available));
    const auto x = content.x - text_.horizontal, y = content.y;
    const Box cursor{x + caret.x,
                     y + caret.y,
                     dpi,
                     std::min(caret.height, std::max(0.0F, paint.bounds.height - 8 * dpi))};
    CanvasScope scope(canvas, paint.bounds);
    border(canvas, paint);
    CanvasScope inner(canvas, clip);
    if (paint.text.empty() && (!paint.focused || paint.composition.empty())) {
        caption(canvas, labels, paint.placeholder, paint.hint_style, content);
    }
    for (const auto& rect :
         input->selection_rects(paint.selection.begin().value, paint.selection.end().value)) {
        fill(canvas,
             {x + rect.x, y + rect.y, rect.width, rect.height},
             paint.focused ? paint.colors.selection : paint.colors.inactive_selection);
    }
    input->draw(canvas, x, y);
    if (!paint.focused) {
        return {};
    }
    fill(canvas, cursor, paint.text_style.color);
    if (!paint.composition.empty()) {
        if (const auto* composition =
                composition_.prepare_async(paint.composition, paint.text_style)) {
            composition->draw(canvas, x + caret.x, y + caret.y);
            fill(
                canvas,
                {x + caret.x, y + caret.y + composition->height() - dpi, composition->width(), dpi},
                paint.colors.focus);
        }
    }
    return visible_area({cursor.x, cursor.y, dpi, caret.height}, clip);
}
} // namespace astra::ui
