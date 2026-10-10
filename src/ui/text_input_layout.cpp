#include <astra/ui/text_input_layout.hpp>
#include <astra/ui/inline_form.hpp>
#include <astra/core/utf8.hpp>

#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
void validate_input(std::string_view text) {
    if (text.size() > InlineForm::max_input_bytes || text.find_first_of("\r\n") != text.npos ||
        text.find('\0') != text.npos) {
        throw std::invalid_argument("Editable text layout requires bounded single-line UTF-8");
    }
    utf8::validate(text);
}
} // namespace

TextInputLayout::TextInputLayout(const TextLayoutEngine& fonts, std::shared_ptr<LayoutJobs> jobs)
    : fonts_(fonts), jobs_(std::move(jobs)) {
}

TextInputLayout::~TextInputLayout() {
    cancel();
}

void TextInputLayout::cancel() {
    if (pending_ && pending_->ticket) {
        jobs_->cancel(*pending_->ticket);
    }
    pending_.reset();
}

const TextLayout& TextInputLayout::prepare(std::string_view text,
                                           const TextLayoutOptions& options) {
    validate_input(text);
    if (key_ && key_->text == text && key_->options == options) {
        cancel();
        return *layout_;
    }
    Key replacement{std::string(text), options};
    auto paragraph =
        std::make_shared<const TextLayout>(fonts_.layout(replacement.text, replacement.options));
    cancel();
    key_ = std::move(replacement);
    layout_ = std::move(paragraph);
    return *layout_;
}

const TextLayout* TextInputLayout::prepare_async(std::string_view text,
                                                 const TextLayoutOptions& options) {
    validate_input(text);
    if (!jobs_ || text.size() < 4096 || (key_ && key_->text == text && key_->options == options)) {
        return &prepare(text, options);
    }
    if (!pending_ || pending_->key.text != text || pending_->key.options != options) {
        Pending replacement{{std::string(text), options}, {}, {}};
        cancel();
        pending_ = std::move(replacement);
    }
    if (pending_->error) {
        std::rethrow_exception(pending_->error);
    }
    if (!pending_->ticket) {
        pending_->ticket = jobs_->submit(pending_->key.text, pending_->key.options);
    }
    return nullptr;
}

void TextInputLayout::poll() {
    if (!pending_ || pending_->error) {
        return;
    }
    if (!pending_->ticket) {
        pending_->ticket = jobs_->submit(pending_->key.text, pending_->key.options);
    }
    auto result = pending_->ticket ? jobs_->take(*pending_->ticket) : std::nullopt;
    if (!result) {
        return;
    }
    pending_->ticket.reset();
    if (result->error) {
        pending_->error = result->error;
        return;
    }
    if (!result->paragraph) {
        throw std::logic_error("Editable layout completed without a paragraph");
    }
    key_ = std::move(pending_->key);
    layout_ = std::move(result->paragraph);
    pending_.reset();
}

bool TextInputLayout::pending() const noexcept {
    return pending_.has_value() && !pending_->error;
}

const TextLayout* TextInputLayout::layout() const noexcept {
    return layout_.get();
}

bool TextInputLayout::matches(std::string_view text) const noexcept {
    return key_ && key_->text == text;
}
} // namespace astra::ui
