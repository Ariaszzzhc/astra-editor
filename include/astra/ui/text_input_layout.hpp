#pragma once

#include <astra/ui/text_layout.hpp>
#include <astra/ui/layout_jobs.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace astra::ui {
// Geometry for a bounded single-line editable control. The owning UI thread
// uses one exact text/style key; failed preparation leaves the old key intact.
// The font engine must outlive this component.
class TextInputLayout final {
  public:
    explicit TextInputLayout(const TextLayoutEngine&, std::shared_ptr<LayoutJobs> = {});
    ~TextInputLayout();
    TextInputLayout(const TextInputLayout&) = delete;
    TextInputLayout& operator=(const TextInputLayout&) = delete;
    [[nodiscard]] const TextLayout& prepare(std::string_view, const TextLayoutOptions&);
    // Cold long inputs return null. Poll on the owner thread after LayoutJobs::poll.
    // Older successful geometry remains cached, but is never returned for new text/style.
    [[nodiscard]] const TextLayout* prepare_async(std::string_view, const TextLayoutOptions&);
    void poll();
    // Retire background work and release the cached paragraph on the owner thread.
    void reset();
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] const TextLayout* layout() const noexcept;
    [[nodiscard]] bool matches(std::string_view) const noexcept;
    float horizontal{};

  private:
    const TextLayoutEngine& fonts_;
    std::shared_ptr<LayoutJobs> jobs_;

    struct Key {
        std::string text;
        TextLayoutOptions options;
    };

    std::optional<Key> key_;
    std::shared_ptr<const TextLayout> layout_;

    struct Pending {
        Key key;
        std::optional<LayoutJobs::Ticket> ticket;
        std::exception_ptr error;
    };

    std::optional<Pending> pending_;
    void cancel();
};
} // namespace astra::ui
