#pragma once

#include <astra/ui/text_labels.hpp>
#include <astra/ui/workbench_layout.hpp>
#include <astra/ui/workbench_style.hpp>
#include <optional>
#include <string_view>

class SkCanvas;

namespace astra::ui {
// Native presentation and geometry only. JS supplies captions, view state and
// actions; the owner keeps TextLabels alive and dispatches returned targets.
class WorkbenchChrome final {
  public:
    struct Header {
        std::string_view brand, workspace, hint;
        float pointer_x{}, pointer_y{};
    };

    struct Sidebar {
        std::string_view title, subtitle, outline_title;
        bool expanded{true}, busy{}, outline_expanded{};
        float pointer_x{}, pointer_y{};
    };
    enum class Target { heading, refresh, section, outline };

    explicit WorkbenchChrome(TextLabels& labels) : labels_(labels) {
    }

    void paint_header(SkCanvas*, const WorkbenchLayout&, const WorkbenchStyle&, const Header&);
    // Paint after indexed rows; owns the headings and final sidebar divider.
    void paint_sidebar(SkCanvas*, const WorkbenchLayout&, const WorkbenchStyle&, const Sidebar&);

    [[nodiscard]] static std::optional<Target>
    hit_sidebar(const WorkbenchLayout& geometry, float x, float y) {
        if (!geometry.sidebar.contains(x, y))
            return std::nullopt;
        if (geometry.outline_header.contains(x, y))
            return Target::outline;
        if (geometry.sidebar_refresh.contains(x, y))
            return Target::refresh;
        if (geometry.sidebar_section.contains(x, y))
            return Target::section;
        if (geometry.sidebar_heading.contains(x, y))
            return Target::heading;
        return std::nullopt;
    }

  private:
    TextLabels& labels_;
    void caption(SkCanvas*, Box, std::string_view, const TextLayoutOptions&);
};
} // namespace astra::ui
