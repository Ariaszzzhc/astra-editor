#pragma once

#include <astra/ui/icons.hpp>
#include <astra/ui/text_labels.hpp>
#include <astra/ui/workbench_layout.hpp>
#include <astra/ui/workbench_style.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

class SkCanvas;

namespace astra::ui {
// Native presentation only. The caller owns identities, dirty state and actions;
// JS supplies theme/icon policy. TextLabels must outlive this component.
class TabBar final {
  public:
    enum class Target { previous, next, caption, close };

    struct Hit {
        Target target;
        std::size_t index{};
    };

    struct Decoration {
        Icon icon{Icon::document};
        std::uint32_t color{};
        bool dirty{};
    };

    explicit TabBar(TextLabels& labels, IconRenderer& icons) : labels_(labels), icons_(icons) {
    }

    // Pointer dispatch uses prepare_text=false: no new caption measurement.
    // Paint preparation prioritizes visible labels, at most eight per call and
    // checks a four-millisecond dispatch budget between native measurements.
    void measure(
        Box, float scale, TabIndex&, std::size_t active, const WorkbenchStyle&, bool prepare_text);
    void paint(SkCanvas*,
               const TabIndex&,
               std::size_t active,
               const WorkbenchStyle&,
               const std::function<Decoration(std::size_t)>&);
    [[nodiscard]] std::optional<Hit> hit(float x, float y) const;

    void reveal() noexcept {
        reveal_ = true;
    }

    void scroll(int steps, std::size_t count);

    [[nodiscard]] const TabStrip& geometry() const noexcept {
        return strip_;
    }

    [[nodiscard]] std::size_t max_measured_pass() const noexcept {
        return max_measured_pass_;
    }

  private:
    TextLabels& labels_;
    IconRenderer& icons_;
    Box bounds_;
    float scale_{1};
    TabStrip strip_;
    std::optional<TextLayoutOptions> measure_style_;
    std::size_t first_{}, max_measured_pass_{};
    bool reveal_{true};
};
} // namespace astra::ui
