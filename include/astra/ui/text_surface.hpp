#pragma once

#include <astra/ui/document_viewport.hpp>
#include <astra/ui/text_line_index.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace astra::ui {
// Read-only text regions share the same measured page and paragraph engine as
// documents. Content is indexed once; only a bounded visible page is prepared.
// The caller owns placement/colors/actions. The font engine must outlive this.
class TextSurface final {
  public:
    explicit TextSurface(const TextLayoutEngine&,
                         std::shared_ptr<LayoutJobs>,
                         ViewportCacheLimits = {});
    void reset(std::string_view);
    void show(std::size_t begin, std::size_t end);
    [[nodiscard]] const TextLineIndex& lines() const noexcept;
    // Borrowed for this frame; reset/show/style replacement may invalidate it.
    [[nodiscard]] const TextLayout* prepare(std::size_t row, const TextLayoutOptions&);
    void poll();
    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] ViewportCacheStatistics statistics() const noexcept;
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> visible() const noexcept;

  private:
    TextLineIndex lines_;
    DocumentViewport viewport_;
    std::optional<std::pair<std::size_t, std::size_t>> visible_;
    std::map<std::size_t, TextLayoutOptions> styles_;
};
} // namespace astra::ui
