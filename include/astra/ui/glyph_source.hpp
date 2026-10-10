#pragma once

#include <cstddef>
#include <memory>

namespace astra::ui {
class GlyphCache;

struct GlyphSourceStatistics {
    std::size_t runs{}, glyphs{}, bytes{};
    std::size_t shaped_bytes{}, reused_bytes{};
    std::size_t safe_boundaries{}, unsafe_boundaries{};
    std::size_t spliced_runs{}, shape_attempts{}, shared_glyphs{}, equivalent_fonts{};
    std::size_t font_bytes_hashed{};
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
