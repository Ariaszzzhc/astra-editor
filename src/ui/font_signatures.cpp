#include "font_signatures.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <vector>

namespace astra::ui {
namespace {
void number(ContentHash& hash, std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = std::byte((value >> (i * 8U)) & 255U);
    }
    hash.update(bytes);
}
} // namespace

std::optional<ContentDigest> FontSignatures::read(const SkTypeface& face) {
    if (const auto found = entries_.find(face.uniqueID()); found != entries_.end()) {
        return found->second;
    }
    if (entries_.size() == 128) {
        return {};
    }
    auto digest = calculate(face);
    entries_.emplace(face.uniqueID(), digest);
    return digest;
}

std::size_t FontSignatures::bytes_hashed() const noexcept {
    return bytes_;
}

std::optional<ContentDigest> FontSignatures::calculate(const SkTypeface& face) {
    const auto count = face.countTables();
    if (count <= 0 || count > 4096) {
        return {};
    }
    std::vector<SkFontTableTag> tables(static_cast<std::size_t>(count));
    if (face.readTableTags({tables.data(), tables.size()}) != count) {
        return {};
    }
    std::ranges::sort(tables);
    if (std::adjacent_find(tables.begin(), tables.end()) != tables.end()) {
        return {};
    }
    ContentHash hash;
    hash.update("Astra SFNT content identity v1");
    const auto style = face.fontStyle();
    number(hash, static_cast<std::uint64_t>(style.weight()));
    number(hash, static_cast<std::uint64_t>(style.width()));
    number(hash, static_cast<std::uint64_t>(style.slant()));
    number(hash, face.isSyntheticBold());
    number(hash, face.isSyntheticOblique());
    number(hash, static_cast<std::uint64_t>(face.countGlyphs()));
    number(hash, static_cast<std::uint64_t>(face.getUnitsPerEm()));
    const auto axes = face.getVariationDesignPosition({});
    if (axes < 0 || axes > 4096) {
        return {};
    }
    std::vector<SkFontArguments::VariationPosition::Coordinate> coordinates(
        static_cast<std::size_t>(axes));
    if (axes != 0 &&
        face.getVariationDesignPosition({coordinates.data(), coordinates.size()}) != axes) {
        return {};
    }
    std::ranges::sort(coordinates, {}, &SkFontArguments::VariationPosition::Coordinate::axis);
    number(hash, coordinates.size());
    for (const auto coordinate : coordinates) {
        number(hash, coordinate.axis);
        number(hash, std::bit_cast<std::uint32_t>(coordinate.value));
    }
    number(hash, tables.size());
    std::array<std::byte, 65536> buffer{};
    constexpr std::size_t maximum = 128 * 1024 * 1024;
    for (const auto tag : tables) {
        const auto size = face.getTableSize(tag);
        if (size == 0 || size > maximum - bytes_) {
            return {};
        }
        number(hash, tag);
        number(hash, size);
        for (std::size_t offset = 0; offset < size;) {
            const auto length = std::min(buffer.size(), size - offset);
            if (face.getTableData(tag, offset, length, buffer.data()) != length) {
                return {};
            }
            hash.update(std::span(buffer).first(length));
            bytes_ += length;
            offset += length;
        }
    }
    return hash.digest();
}
} // namespace astra::ui
