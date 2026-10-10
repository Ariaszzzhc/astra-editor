#include <astra/ui/text_input_layout.hpp>
#include <astra/ui/inline_form.hpp>

#include <stdexcept>
#include <utility>

namespace astra::ui {
TextInputLayout::TextInputLayout(const TextLayoutEngine& fonts) : fonts_(fonts) {
}

const TextLayout& TextInputLayout::prepare(std::string_view text,
                                           const TextLayoutOptions& options) {
    if (text.size() > InlineForm::max_input_bytes || text.find_first_of("\r\n") != text.npos ||
        text.find('\0') != text.npos) {
        throw std::invalid_argument("Editable text layout exceeds its single-line bound");
    }
    if (key_ && key_->text == text && key_->options == options) {
        return *layout_;
    }
    Key replacement{std::string(text), options};
    auto paragraph = fonts_.layout(replacement.text, replacement.options);
    key_ = std::move(replacement);
    layout_ = std::move(paragraph);
    return *layout_;
}

const TextLayout* TextInputLayout::layout() const noexcept {
    return layout_ ? &*layout_ : nullptr;
}

bool TextInputLayout::matches(std::string_view text) const noexcept {
    return key_ && key_->text == text;
}
} // namespace astra::ui
