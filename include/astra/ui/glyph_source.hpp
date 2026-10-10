#pragma once

#include <cstddef>
#include <memory>

namespace astra::ui {
class GlyphCache;

struct GlyphSourceStatistics {
    std::size_t runs{}, glyphs{}, bytes{};
    std::size_t shaped_bytes{}, reused_bytes{};
    std::size_t safe_boundaries{}, unsafe_boundaries{};
};

// Immutable logical-order glyph runs, without native paragraphs, font
// collections or typefaces. Retained payload has a separate 4 MiB admission
// bound; uncaptured runs still use the complete native shaper.
class GlyphSource final {
  public:
    GlyphSource() = default;
    [[nodiscard]] GlyphSourceStatistics statistics() const noexcept;

  private:
    struct Data;
    std::shared_ptr<const Data> data_;
    friend class GlyphCache;
};
} // namespace astra::ui
