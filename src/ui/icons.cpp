#include <astra/ui/icons.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include <utility>

namespace astra::ui {
Icon view_icon(std::string_view name) {
    const std::pair<std::string_view, Icon> icons[] = {{"files", Icon::files},
                                                       {"search", Icon::search},
                                                       {"branch", Icon::branch},
                                                       {"braces", Icon::braces},
                                                       {"symbol", Icon::symbol},
                                                       {"folder", Icon::folder},
                                                       {"code", Icon::code},
                                                       {"function", Icon::function},
                                                       {"warning", Icon::warning},
                                                       {"settings", Icon::settings}};
    for (const auto& [id, icon] : icons)
        if (id == name)
            return icon;
    return Icon::document;
}

void draw_icon(SkCanvas* canvas, Icon icon, float x, float y, float size, std::uint32_t color) {
    canvas->save();
    canvas->translate(x, y);
    canvas->scale(size / 24, size / 24);
    SkPaint paint;
    paint.setColor(color);
    paint.setAntiAlias(true);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(1.5F);
    paint.setStrokeCap(SkPaint::kRound_Cap);
    paint.setStrokeJoin(SkPaint::kRound_Join);
    const auto line = [&](float x1, float y1, float x2, float y2) {
        canvas->drawLine(x1, y1, x2, y2, paint);
    };
    SkPathBuilder path;
    switch (icon) {
    case Icon::files:
        path.moveTo(5, 16).lineTo(3, 16).lineTo(3, 3).lineTo(14, 3).lineTo(14, 5);
        path.moveTo(7, 7).lineTo(17, 7).lineTo(21, 11).lineTo(21, 21).lineTo(7, 21).close();
        path.moveTo(17, 7).lineTo(17, 11).lineTo(21, 11);
        break;
    case Icon::search:
        canvas->drawCircle(10.5F, 10.5F, 6.5F, paint);
        line(15.5F, 15.5F, 21, 21);
        break;
    case Icon::branch:
        canvas->drawCircle(7, 5, 2, paint);
        canvas->drawCircle(17, 6, 2, paint);
        canvas->drawCircle(7, 20, 2, paint);
        path.moveTo(7, 7).lineTo(7, 18);
        path.moveTo(17, 8).lineTo(17, 10).quadTo(17, 14, 7, 14);
        break;
    case Icon::settings:
        canvas->drawCircle(12, 12, 3.2F, paint);
        path.moveTo(9, 3)
            .lineTo(15, 3)
            .lineTo(16, 6)
            .lineTo(19, 7)
            .lineTo(21, 10)
            .lineTo(19, 12)
            .lineTo(21, 15)
            .lineTo(19, 18)
            .lineTo(16, 18)
            .lineTo(15, 21)
            .lineTo(9, 21)
            .lineTo(8, 18)
            .lineTo(5, 17)
            .lineTo(3, 14)
            .lineTo(5, 12)
            .lineTo(3, 9)
            .lineTo(5, 6)
            .lineTo(8, 6)
            .close();
        break;
    case Icon::close:
        line(6, 6, 18, 18);
        line(18, 6, 6, 18);
        break;
    case Icon::chevron_right:
        path.moveTo(9, 6).lineTo(15, 12).lineTo(9, 18);
        break;
    case Icon::chevron_down:
        path.moveTo(6, 9).lineTo(12, 15).lineTo(18, 9);
        break;
    case Icon::folder:
        path.moveTo(3, 7)
            .lineTo(9, 7)
            .lineTo(11, 9)
            .lineTo(21, 9)
            .lineTo(21, 20)
            .lineTo(3, 20)
            .close();
        path.moveTo(3, 7).lineTo(3, 5).lineTo(10, 5).lineTo(12, 7).lineTo(20, 7).lineTo(20, 9);
        break;
    case Icon::document:
        path.moveTo(6, 3).lineTo(15, 3).lineTo(20, 8).lineTo(20, 21).lineTo(6, 21).close();
        path.moveTo(15, 3).lineTo(15, 8).lineTo(20, 8);
        line(9, 12, 17, 12);
        line(9, 16, 17, 16);
        break;
    case Icon::code:
        path.moveTo(8, 7).lineTo(3, 12).lineTo(8, 17);
        path.moveTo(16, 7).lineTo(21, 12).lineTo(16, 17);
        line(14, 5, 10, 19);
        break;
    case Icon::braces:
        path.moveTo(9, 3)
            .lineTo(7, 3)
            .quadTo(5, 3, 5, 6)
            .lineTo(5, 9)
            .quadTo(5, 12, 3, 12)
            .quadTo(5, 12, 5, 15)
            .lineTo(5, 18)
            .quadTo(5, 21, 7, 21)
            .lineTo(9, 21);
        path.moveTo(15, 3)
            .lineTo(17, 3)
            .quadTo(19, 3, 19, 6)
            .lineTo(19, 9)
            .quadTo(19, 12, 21, 12)
            .quadTo(19, 12, 19, 15)
            .lineTo(19, 18)
            .quadTo(19, 21, 17, 21)
            .lineTo(15, 21);
        break;
    case Icon::symbol:
        path.moveTo(12, 4).lineTo(20, 12).lineTo(12, 20).lineTo(4, 12).close();
        break;
    case Icon::function:
        path.moveTo(16, 4).quadTo(10, 2, 10, 7).lineTo(10, 17).quadTo(10, 22, 5, 20);
        line(6, 10, 16, 10);
        break;
    case Icon::refresh:
        path.moveTo(19, 8)
            .quadTo(17, 3, 11, 4)
            .quadTo(4, 5, 4, 12)
            .quadTo(4, 20, 12, 20)
            .quadTo(18, 20, 20, 15);
        path.moveTo(19, 3).lineTo(19, 8).lineTo(14, 8);
        break;
    case Icon::warning:
        path.moveTo(12, 3).lineTo(22, 21).lineTo(2, 21).close();
        line(12, 8, 12, 14);
        line(12, 17, 12, 17.5F);
        break;
    }
    canvas->drawPath(path.detach(), paint);
    canvas->restore();
}
} // namespace astra::ui
