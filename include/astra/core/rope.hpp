#pragma once

#include "astra/core/sum_tree.hpp"

#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace astra {

struct TextPosition {
    std::size_t line{};
    std::size_t utf16_column{};

    friend bool operator==(const TextPosition&, const TextPosition&) = default;
};

struct RopeChangedExtent {
    std::size_t begin{};
    std::size_t old_end{};
    std::size_t new_end{};
};

struct TextSummary {
    std::size_t bytes{};
    std::size_t utf16_units{};
    std::size_t line_breaks{};
    std::size_t trailing_bytes{};
    std::size_t trailing_utf16{};

    friend bool operator==(const TextSummary&, const TextSummary&) = default;

    friend TextSummary operator+(const TextSummary& before, const TextSummary& after) {
        const auto add = [](std::size_t left, std::size_t right) {
            if (right > std::numeric_limits<std::size_t>::max() - left) {
                throw std::length_error("Rope text summary overflow");
            }
            return left + right;
        };
        const auto breaks = add(before.line_breaks, after.line_breaks);
        if (breaks == std::numeric_limits<std::size_t>::max()) {
            throw std::length_error("Rope line count overflow");
        }
        return {
            add(before.bytes, after.bytes),
            add(before.utf16_units, after.utf16_units),
            breaks,
            after.line_breaks != 0 ? after.trailing_bytes : add(before.trailing_bytes, after.bytes),
            after.line_breaks != 0 ? after.trailing_utf16
                                   : add(before.trailing_utf16, after.utf16_units),
        };
    }
};

struct MeasureText {
    [[nodiscard]] TextSummary operator()(std::string_view text) const;
};

// Immutable UTF-8 text backed by a measured persistent tree. Byte offsets are
// Unicode scalar boundaries (not grapheme or display-column boundaries).
// LF starts a new line; CRLF is preserved byte for byte. A standalone CR is an
// ordinary character. Raw UTF-16 columns include CR, which permits exact byte
// boundary round trips. line_end() excludes the complete CRLF/LF sequence;
// callers using LSP or visual columns should clamp there as appropriate.
class Rope {
  public:
    static constexpr std::size_t max_chunk_bytes = 1024;

    Rope() = default;
    explicit Rope(std::string_view text);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size_bytes() const noexcept;
    [[nodiscard]] std::size_t size_utf16() const noexcept;
    [[nodiscard]] std::size_t line_count() const noexcept;
    [[nodiscard]] const TextSummary& summary() const noexcept;

    // Explicit materialization for output; editing never calls this method.
    [[nodiscard]] std::string str() const;
    [[nodiscard]] std::string slice(std::size_t begin, std::size_t end) const;
    [[nodiscard]] Rope
    replace(std::size_t begin, std::size_t end, std::string_view replacement) const;
    [[nodiscard]] std::pair<Rope, Rope> split(std::size_t byte_offset) const;
    [[nodiscard]] Rope concat(const Rope& other) const;
    // The smallest scalar-aligned replacement extent from this snapshot to
    // after. Shared tree interiors are skipped by identity; unshared edge bytes
    // are compared in bounded windows. All three offsets are scalar boundaries.
    [[nodiscard]] RopeChangedExtent changed_extent(const Rope& after) const;

    [[nodiscard]] bool is_boundary(std::size_t byte_offset) const;
    [[nodiscard]] TextPosition byte_to_position(std::size_t byte_offset) const;
    [[nodiscard]] std::size_t position_to_byte(TextPosition position) const;
    [[nodiscard]] std::size_t byte_to_utf16(std::size_t byte_offset) const;
    [[nodiscard]] std::size_t utf16_to_byte(std::size_t utf16_offset) const;
    [[nodiscard]] std::size_t line_start(std::size_t line) const;
    [[nodiscard]] std::size_t line_end(std::size_t line) const;

    [[nodiscard]] std::size_t chunk_count() const noexcept;
    [[nodiscard]] std::size_t tree_height() const noexcept;
    [[nodiscard]] bool validate() const;

  private:
    using Tree = SumTree<std::string, TextSummary, MeasureText>;
    Tree tree_;

    explicit Rope(Tree tree);
    void require_boundary(std::size_t byte_offset) const;
    void require_range(std::size_t begin, std::size_t end) const;
    [[nodiscard]] char byte_at(std::size_t byte_offset) const;
};

} // namespace astra
