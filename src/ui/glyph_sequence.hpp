#pragma once

#include <astra/core/sum_tree.hpp>
#include <modules/skshaper/include/SkShaper_harfbuzz.h>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace astra::ui {
struct GlyphBoundary {
    std::size_t byte{}, glyph{};
    bool unsafe{};
};

struct GlyphSummary {
    std::size_t bytes{}, glyphs{}, payload{}, chunks{};
    friend bool operator==(const GlyphSummary&, const GlyphSummary&) = default;

    friend GlyphSummary operator+(GlyphSummary a, GlyphSummary b) {
        return {a.bytes + b.bytes, a.glyphs + b.glyphs, a.payload + b.payload, a.chunks + b.chunks};
    }
};

// Logical-order native glyphs in bounded immutable blocks. Tree prefixes relocate
// retained clusters after edits without rewriting the old glyph arrays.
class GlyphSequence final {
  public:
    using Glyph = SkShapers::HB::CachedGlyph;
    using Glyphs = SkShapers::HB::RunCache::Glyphs;
    static constexpr std::size_t chunk_limit = 256;

    [[nodiscard]] static GlyphSequence
    capture(std::span<const Glyph>, std::size_t begin, std::size_t end);
    [[nodiscard]] Glyphs flatten(std::size_t begin) const;
    [[nodiscard]] std::vector<GlyphBoundary> boundaries(std::size_t begin) const;
    [[nodiscard]] std::pair<GlyphSequence, GlyphSequence> split(std::size_t glyph,
                                                                std::size_t byte) const;
    [[nodiscard]] GlyphSequence concat(const GlyphSequence&) const;
    [[nodiscard]] const GlyphSummary& summary() const noexcept;
    [[nodiscard]] std::size_t shared_glyphs(const GlyphSequence&) const;
    [[nodiscard]] bool validate() const;

  private:
    struct Piece {
        std::shared_ptr<const Glyphs> source;
        std::size_t begin{}, count{}, bytes{}, origin{};
    };

    struct Measure {
        GlyphSummary operator()(const Piece& piece) const {
            // Sliced leaves retain their entire bounded source allocation.
            return {
                piece.bytes, piece.count, sizeof(Piece) + piece.source->size() * sizeof(Glyph), 1};
        }
    };

    using Tree = SumTree<Piece, GlyphSummary, Measure>;
    Tree tree_;
    [[nodiscard]] std::size_t cluster(std::size_t glyph) const;
};
} // namespace astra::ui
