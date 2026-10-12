#pragma once

#include <astra/ui/text_labels.hpp>
#include <astra/ui/workbench_layout.hpp>
#include <astra/ui/workbench_style.hpp>
#include <astra/ui/chrome_state.hpp>
#include <astra/ui/icons.hpp>
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
        Icon command_icon{Icon::search};
    };

    struct Sidebar {
        std::string_view title, subtitle, outline_title;
        bool expanded{true}, busy{}, outline_expanded{};
        float pointer_x{}, pointer_y{};
    };
    enum class Target { heading, refresh, section, outline };

    struct Item {
        ChromeBank bank;
        std::size_t index{};
        Box bounds;
    };

    struct OuterLayout {
        std::vector<Item> items;
        Box previous, next, diagnostic;
        std::size_t activity_first{}, activity_page{};
        [[nodiscard]] std::optional<Item> hit(float x, float y) const;
    };

    explicit WorkbenchChrome(TextLabels& labels, IconRenderer& icons)
        : labels_(labels), icons_(icons) {
    }

    void paint_header(SkCanvas*, const WorkbenchLayout&, const WorkbenchStyle&, const Header&);
    // Paint after indexed rows; owns the headings and final sidebar divider.
    void paint_sidebar(SkCanvas*, const WorkbenchLayout&, const WorkbenchStyle&, const Sidebar&);
    [[nodiscard]] OuterLayout measure_outer(const WorkbenchLayout&,
                                            const WorkbenchStyle&,
                                            const ChromeState&,
                                            std::size_t activity_first,
                                            bool diagnostic = false);
    void paint_outer(SkCanvas*,
                     const WorkbenchLayout&,
                     const WorkbenchStyle&,
                     const ChromeState&,
                     const OuterLayout&,
                     float pointer_x,
                     float pointer_y);
    // Paint after document content; these targets share the outer publication
    // and hit geometry. Host diagnostics remain visible if a JS provider fails.
    void paint_body(SkCanvas*,
                    const WorkbenchLayout&,
                    const WorkbenchStyle&,
                    const ChromeState&,
                    const OuterLayout&,
                    float pointer_x,
                    float pointer_y,
                    std::string_view diagnostic = {});

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
    IconRenderer& icons_;
    void caption(SkCanvas*, Box, std::string_view, const TextLayoutOptions&);
};
} // namespace astra::ui
