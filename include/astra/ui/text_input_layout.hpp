#pragma once

#include <astra/ui/text_layout.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace astra::ui {
// Geometry for a bounded single-line editable control. The owning UI thread
// uses one exact text/style key; failed preparation leaves the old key intact.
// The font engine must outlive this component.
class TextInputLayout final {
  public:
    explicit TextInputLayout(const TextLayoutEngine&);
    [[nodiscard]] const TextLayout& prepare(std::string_view, const TextLayoutOptions&);
    [[nodiscard]] const TextLayout* layout() const noexcept;
    [[nodiscard]] bool matches(std::string_view) const noexcept;
    float horizontal{};

  private:
    const TextLayoutEngine& fonts_;

    struct Key {
        std::string text;
        TextLayoutOptions options;
    };

    std::optional<Key> key_;
    std::optional<TextLayout> layout_;
};
} // namespace astra::ui
