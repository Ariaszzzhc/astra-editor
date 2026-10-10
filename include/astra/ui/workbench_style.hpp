#pragma once

#include <astra/protocol/rpc.hpp>
#include <astra/ui/text_layout.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace astra::ui {
// Compiled presentation values. Defaults come from the shipped JS policy;
// compilation validates the whole candidate before the window publishes it.
struct WorkbenchStyle {
    struct Colors {
        std::uint32_t background{}, foreground{}, sidebar{}, activity{}, tabs{}, panel{};
        std::uint32_t active_tab{}, border{}, selection{}, accent{}, status{}, muted{};
        std::uint32_t status_foreground{}, hover{}, current_line{}, input{}, cursor{}, scrollbar{};
        std::uint32_t folder{}, code_icon{}, script_icon{}, added{}, changed{};
        std::uint32_t notice_background{}, recovery_background{}, error_background{}, error{},
            warning{};
    } colors;

    std::string name;
    float font_size{}, sidebar_width{};
    std::vector<std::string> font_families, monospace_families;
    std::map<std::string, std::uint32_t> syntax;

    [[nodiscard]] static WorkbenchStyle compile(const protocol::Json& settings);
    // Reference sizes express hierarchy relative to a 13-unit UI base. Body,
    // title, captions and fields all use the same configured size/families.
    [[nodiscard]] TextLayoutOptions text(float reference_size,
                                         float dpi,
                                         std::uint32_t color,
                                         float line_height = 1.15F,
                                         bool monospace = false,
                                         float wrap_width = 0) const;
};
} // namespace astra::ui
