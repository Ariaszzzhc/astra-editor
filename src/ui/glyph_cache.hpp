#pragma once

#include <astra/ui/glyph_source.hpp>
#include <modules/skshaper/include/SkShaper_harfbuzz.h>
#include <memory>

namespace astra::ui {
// The collector has one owner thread and is detached before publication.
class GlyphCache final : public SkShapers::HB::RunCache {
  public:
    explicit GlyphCache(GlyphSource previous);
    ~GlyphCache() override;
    Glyphs shape(const SkShapers::HB::CacheInput&, const Shape&) noexcept override;
    void rethrow_error() const;
    bool failed() const noexcept override;
    [[nodiscard]] GlyphSource snapshot() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Glyphs shape_checked(const SkShapers::HB::CacheInput&, const Shape&);
};
} // namespace astra::ui
