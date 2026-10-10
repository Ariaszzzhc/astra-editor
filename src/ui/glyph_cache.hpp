#pragma once

#include <astra/ui/glyph_source.hpp>
#include <astra/core/content_hash.hpp>
#include <modules/skshaper/include/SkShaper_harfbuzz.h>
#include <memory>
#include <string>

namespace astra::ui {
class FontSignatures;

// The collector has one owner thread and is detached before publication.
class GlyphCache final : public SkShapers::HB::RunCache {
  public:
    explicit GlyphCache(GlyphSource previous, std::shared_ptr<FontSignatures> signatures = {});
    ~GlyphCache() override;
    Glyphs shape(const SkShapers::HB::CacheInput&, const Shape&) noexcept override;
    void rethrow_error() const;
    bool failed() const noexcept override;
    [[nodiscard]] GlyphSource snapshot() const;
    // Diagnostic identity of raw native glyph fields, excluding font instance
    // IDs and cache statistics. Tests compare against independent full shaping.
    [[nodiscard]] static ContentDigest glyph_digest(const GlyphSource&);
    [[nodiscard]] static std::string glyph_difference(const GlyphSource&, const GlyphSource&);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Glyphs shape_checked(const SkShapers::HB::CacheInput&, const Shape&);
};
} // namespace astra::ui
