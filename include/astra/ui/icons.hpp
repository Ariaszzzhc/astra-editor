#pragma once

#include <cstdint>
#include <string_view>
class SkCanvas;

namespace astra::ui {
enum class Icon {
    files,
    search,
    branch,
    settings,
    close,
    chevron_right,
    chevron_down,
    folder,
    document,
    code,
    braces,
    symbol,
    function,
    refresh,
    warning
};
void draw_icon(SkCanvas* canvas, Icon icon, float x, float y, float size, std::uint32_t color);
[[nodiscard]] Icon view_icon(std::string_view name);
} // namespace astra::ui
