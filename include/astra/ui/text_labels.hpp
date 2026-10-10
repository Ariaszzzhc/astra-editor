#pragma once

#include <astra/ui/document_viewport.hpp>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace astra::ui {
// Plain chrome text on the owning UI thread. Display normalization/elision
// never changes its source model. Retained paragraphs survive cache eviction.
class TextLabels final {
  public:
    static constexpr std::size_t max_source_bytes = 4096;
    explicit TextLabels(const TextLayoutEngine&, ViewportCacheLimits = {512, 2 * 1024 * 1024});
    [[nodiscard]] std::shared_ptr<const TextLayout> prepare(std::string_view,
                                                            const TextLayoutOptions&);
    // Null means even the ellipsis cannot fit. Only complete grapheme prefixes
    // are displayed, and the selected paragraph is measured with its final text.
    [[nodiscard]] std::shared_ptr<const TextLayout>
    elide(std::string_view, const TextLayoutOptions&, float width);
    [[nodiscard]] ViewportCacheStatistics statistics() const noexcept;

  private:
    DocumentViewport paragraphs_;
    std::shared_ptr<const TextLayout> shape(std::string, const TextLayoutOptions&);
};
} // namespace astra::ui
