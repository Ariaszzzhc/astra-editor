#pragma once

#include <astra/core/document.hpp>
#include <astra/ui/text_input_layout.hpp>
#include <astra/ui/text_labels.hpp>
#include <astra/ui/workbench_layout.hpp>

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace astra::ui {
struct TextFieldColors {
    std::uint32_t background{}, border{}, focus{}, error{}, selection{}, inactive_selection{};
};

// Borrowed text and selections belong to the caller's immutable frame snapshot.
// This control owns only presentation, scrolling and native paragraph lifetime.
struct TextFieldPaint {
    Box bounds;
    TextLayoutOptions text_style, hint_style;
    TextFieldColors colors;
    std::string_view text, placeholder, composition;
    Selection selection;
    bool focused{}, invalid{};
};

class TextField final {
  public:
    explicit TextField(const TextLayoutEngine&, std::shared_ptr<LayoutJobs> = {});
    // Poll after LayoutJobs::poll on the owning UI thread.
    void poll();
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] bool composition_pending() const noexcept;
    [[nodiscard]] TextInputLayout& text() noexcept;
    [[nodiscard]] const TextInputLayout& text() const noexcept;
    [[nodiscard]] const TextLayout* composition_layout() const noexcept;
    // Pointer coordinates use the same padding/scroll transform as painting.
    [[nodiscard]] std::pair<float, float> to_text(Box, float dpi, float x, float y) const;
    // Returned physical-pixel caret area stays within the control; SDL conversion
    // belongs to the window adapter. No model mutation or JS callbacks occur here.
    [[nodiscard]] std::optional<Box> draw(SkCanvas*, TextLabels&, const TextFieldPaint&);
    [[nodiscard]] std::optional<Box>
    draw_unavailable(SkCanvas*, TextLabels&, const TextFieldPaint&, bool failed);

  private:
    TextInputLayout text_, composition_;
};
} // namespace astra::ui
