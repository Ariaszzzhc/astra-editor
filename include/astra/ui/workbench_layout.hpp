#pragma once

#include <cstddef>
#include <vector>

namespace astra::ui {
struct Box {
    float x{}, y{}, width{}, height{};

    [[nodiscard]] float right() const {
        return x + width;
    }

    [[nodiscard]] float bottom() const {
        return y + height;
    }

    [[nodiscard]] bool contains(float px, float py) const {
        return px >= x && px < right() && py >= y && py < bottom();
    }
};

struct WorkbenchLayout {
    Box header, activity, sidebar, sidebar_rows, outline_header, outline_rows;
    Box tabs, breadcrumb, editor, status, sidebar_handle;
    float scale{1};
    float sidebar_row_height{24};
    static WorkbenchLayout measure(float width,
                                   float height,
                                   float scale,
                                   bool sidebar,
                                   float sidebar_width,
                                   bool outline,
                                   bool explorer);
};

struct ConfirmationLayout {
    Box panel, title, detail, items, accept, cancel;
    static ConfirmationLayout measure(float width, float height, float scale, std::size_t items);
};

struct ChoiceLayout {
    Box panel, title, detail;
    std::vector<Box> buttons;
    static ChoiceLayout measure(float width, float height, float scale, std::size_t choices);
};

struct PickerLayout {
    Box panel, title, input, rows;
    float row_height{};
    std::size_t capacity{};
    static PickerLayout measure(float width, float height, float scale, std::size_t rows = 12);
};

struct TextPanelLayout {
    Box panel, title, content;
    static TextPanelLayout measure(float width, float height, float scale, std::size_t rows = 32);
};

struct TabBox {
    std::size_t index{};
    Box bounds;
    Box close;
};

struct TabStrip {
    std::vector<TabBox> visible;
    std::size_t first{};
    Box previous, next;
    static TabStrip measure(Box bounds,
                            float scale,
                            const std::vector<float>& label_widths,
                            std::size_t first,
                            std::size_t active,
                            bool reveal);
};
} // namespace astra::ui
