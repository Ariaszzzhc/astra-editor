#include "glyph_sequence.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace astra::ui {
GlyphSequence
GlyphSequence::capture(std::span<const Glyph> glyphs, std::size_t begin, std::size_t end) {
    if (begin > end || end > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("Invalid glyph text extent");
    }
    std::size_t previous = begin;
    for (const auto& glyph : glyphs) {
        if (glyph.cluster < previous || glyph.cluster >= end) {
            throw std::invalid_argument("Glyph clusters must be logical and inside their text");
        }
        previous = glyph.cluster;
    }
    std::vector<Piece> pieces;
    for (std::size_t offset = 0; offset < glyphs.size();) {
        const auto count = std::min(chunk_limit, glyphs.size() - offset);
        const auto origin = offset == 0 ? begin : glyphs[offset].cluster;
        const auto next = offset + count < glyphs.size() ? glyphs[offset + count].cluster : end;
        auto source = std::make_shared<const Glyphs>(glyphs.begin() + offset,
                                                     glyphs.begin() + offset + count);
        pieces.push_back({std::move(source), 0, count, next - origin, origin});
        offset += count;
    }
    if (glyphs.empty() && begin != end) {
        pieces.push_back({std::make_shared<const Glyphs>(), 0, 0, end - begin, begin});
    }
    GlyphSequence result;
    result.tree_ = Tree(std::move(pieces));
    return result;
}

GlyphSequence::Glyphs GlyphSequence::flatten(std::size_t begin) const {
    if (begin > std::numeric_limits<std::uint32_t>::max() ||
        summary().bytes > std::numeric_limits<std::uint32_t>::max() - begin) {
        throw std::length_error("Relocated glyph clusters exceed native coordinates");
    }
    Glyphs result;
    result.reserve(summary().glyphs);
    std::size_t prefix{};
    tree_.for_each([&](const Piece& piece) {
        for (std::size_t i = 0; i < piece.count; ++i) {
            auto glyph = piece.source->at(piece.begin + i);
            glyph.cluster =
                static_cast<std::uint32_t>(begin + prefix + glyph.cluster - piece.origin);
            result.push_back(glyph);
        }
        prefix += piece.bytes;
    });
    return result;
}

std::vector<GlyphBoundary> GlyphSequence::boundaries(std::size_t begin) const {
    std::vector<GlyphBoundary> result{{begin, 0, false}};
    std::size_t prefix{}, index{};
    tree_.for_each([&](const Piece& piece) {
        for (std::size_t i = 0; i < piece.count; ++i, ++index) {
            const auto& glyph = piece.source->at(piece.begin + i);
            const auto byte = begin + prefix + glyph.cluster - piece.origin;
            if (result.back().byte == byte) {
                result.back().unsafe |= glyph.unsafeToConcat;
            } else {
                result.push_back({byte, index, glyph.unsafeToConcat});
            }
        }
        prefix += piece.bytes;
    });
    if (summary().bytes != 0) {
        result.push_back({begin + summary().bytes, index, false});
    }
    return result;
}

std::size_t GlyphSequence::cluster(std::size_t glyph) const {
    const auto found = tree_.seek(glyph, [](GlyphSummary value) {
        return value.glyphs;
    });
    if (!found.item) {
        throw std::out_of_range("Glyph index exceeds sequence");
    }
    const auto& piece = *found.item;
    return found.prefix.bytes +
           piece.source->at(piece.begin + glyph - found.prefix.glyphs).cluster - piece.origin;
}

std::pair<GlyphSequence, GlyphSequence> GlyphSequence::split(std::size_t glyph,
                                                             std::size_t byte) const {
    if (glyph > summary().glyphs || byte > summary().bytes ||
        (glyph != 0 && cluster(glyph - 1) >= byte) ||
        (glyph != summary().glyphs && cluster(glyph) < byte)) {
        throw std::invalid_argument("Glyph split must agree with a text cluster boundary");
    }
    if (glyph == 0 && byte == 0) {
        return {{}, *this};
    }
    if (glyph == summary().glyphs && byte == summary().bytes) {
        return {*this, {}};
    }
    auto found = tree_.seek(glyph, [](GlyphSummary value) {
        return value.glyphs;
    });
    if (!found.item || found.prefix.bytes > byte) {
        found = tree_.seek(byte, [](GlyphSummary value) {
            return value.bytes;
        });
    }
    if (!found.item) {
        throw std::invalid_argument("Glyph split has no containing piece");
    }
    const auto piece = *found.item;
    const auto count = glyph - found.prefix.glyphs;
    const auto width = byte - found.prefix.bytes;
    if (count > piece.count || width > piece.bytes) {
        throw std::invalid_argument("Glyph split exceeds its piece");
    }
    auto [before, remaining] = tree_.split(found.index);
    auto [removed, after] = remaining.split(1);
    if (count != 0 || width != 0) {
        before =
            before.concat(Tree::singleton({piece.source, piece.begin, count, width, piece.origin}));
    }
    if (count != piece.count || width != piece.bytes) {
        after = Tree::singleton({piece.source,
                                 piece.begin + count,
                                 piece.count - count,
                                 piece.bytes - width,
                                 piece.origin + width})
                    .concat(after);
    }
    GlyphSequence left, right;
    left.tree_ = std::move(before);
    right.tree_ = std::move(after);
    return {std::move(left), std::move(right)};
}

GlyphSequence GlyphSequence::concat(const GlyphSequence& other) const {
    GlyphSequence result;
    result.tree_ = tree_.concat(other.tree_);
    return result;
}

const GlyphSummary& GlyphSequence::summary() const noexcept {
    return tree_.summary();
}

std::size_t GlyphSequence::shared_glyphs(const GlyphSequence& other) const {
    return std::min(summary().glyphs,
                    tree_.shared_prefix_summary(other.tree_).glyphs +
                        tree_.shared_suffix_summary(other.tree_).glyphs);
}

bool GlyphSequence::validate() const {
    bool bounded = tree_.validate();
    tree_.for_each([&](const Piece& piece) {
        bounded &= piece.source->size() <= chunk_limit &&
                   piece.begin + piece.count <= piece.source->size();
    });
    return bounded;
}
} // namespace astra::ui
