#pragma once

#include <astra/ui/tab_index.hpp>

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
    // Physical pixel dimensions/edges; UI font size is in logical units.
    // Shared snapped edges keep caption clipping and pointer dispatch aligned.
    Box header, command_center, activity, sidebar, sidebar_rows, outline_header, outline_rows;
    Box sidebar_heading, sidebar_refresh, sidebar_section;
    Box tabs, breadcrumb, editor, status, sidebar_handle;
    float scale{1};
    float sidebar_row_height{24};
    static WorkbenchLayout measure(float width,
                                   float height,
                                   float scale,
                                   bool sidebar,
                                   float sidebar_width,
                                   bool outline,
                                   bool explorer,
                                   float ui_font_size = 13);
};

// Logical line extents measured with the configured native UI fonts. Defaults
// preserve the baseline geometry for callers without a native font engine.
struct DialogMetrics {
    float title{24}, detail{18}, input{20}, row{18}, button{18};
    void validate() const;
};

struct ConfirmationLayout {
    Box panel, title, detail, items, accept, cancel;
    float row_height{};
    static ConfirmationLayout
    measure(float width, float height, float scale, std::size_t items, DialogMetrics = {});
};

struct ChoiceLayout {
    Box panel, title, detail;
    std::vector<Box> buttons;
    static ChoiceLayout
    measure(float width, float height, float scale, std::size_t choices, DialogMetrics = {});
};

struct PickerLayout {
    Box panel, title, input, rows;
    float row_height{};
    std::size_t capacity{};
    static PickerLayout
    measure(float width, float height, float scale, std::size_t rows = 12, DialogMetrics = {});
};

struct TextPanelLayout {
    Box panel, title, content;
    std::vector<Box> actions;
    static TextPanelLayout measure(float width,
                                   float height,
                                   float scale,
                                   std::size_t rows = 32,
                                   std::size_t actions = 0,
                                   DialogMetrics = {});
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
                            const TabIndex::Snapshot& tabs,
                            std::size_t first,
                            std::size_t active,
                            bool reveal);
};
} // namespace astra::ui
