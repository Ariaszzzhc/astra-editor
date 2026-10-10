#pragma once

#include <astra/ui/text_source.hpp>
#include <astra/ui/glyph_source.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class SkCanvas;

namespace astra::ui {

enum class TextDirection { automatic, left_to_right, right_to_left };
enum class CaretAffinity { upstream, downstream };

struct TextColorSpan {
    std::size_t begin{};
    std::size_t end{};
    std::uint32_t color{};
    friend bool operator==(const TextColorSpan&, const TextColorSpan&) = default;
};

struct TextLayoutOptions {
    float font_size{16.0F};
    float dpi_scale{1.0F};
    float line_height{1.4F};
    unsigned tab_size{4};
    // Physical pixels; zero disables soft wrapping. Wrapping an RTL-base line
    // containing tabs is rejected; unwrapped RTL and wrapped LTR tabs are supported.
    float wrap_width{};
    std::uint32_t color{0xFFE2E5EC};
    // Original UTF-8 ranges. Partial graphemes receive one color; later spans win.
    std::vector<TextColorSpan> color_spans;
    TextDirection direction{TextDirection::left_to_right};
    std::vector<std::string> font_families;
    std::string locale{"en"};
    friend bool operator==(const TextLayoutOptions&, const TextLayoutOptions&) = default;
};

struct TextRect {
    float x{};
    float y{};
    float width{};
    float height{};
};

struct TextHit {
    std::size_t byte_offset{};
    CaretAffinity affinity{CaretAffinity::downstream};
};

struct TextCaret {
    std::size_t byte_offset{};
    float x{};
    float y{};
    float height{};
    CaretAffinity affinity{CaretAffinity::downstream};
};

// One logical UTF-8 line (without CR/LF). Drawing and hit testing share the same
// shaped paragraph. Offsets always address the original, unexpanded UTF-8 text.
class TextLayout final {
  public:
    ~TextLayout();
    TextLayout(TextLayout&&) noexcept;
    TextLayout& operator=(TextLayout&&) noexcept;
    TextLayout(const TextLayout&) = delete;
    TextLayout& operator=(const TextLayout&) = delete;

    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] TextSource source_snapshot() const noexcept;
    [[nodiscard]] GlyphSource glyph_snapshot() const noexcept;
    [[nodiscard]] float width() const;
    [[nodiscard]] float height() const;
    [[nodiscard]] float baseline() const;
    [[nodiscard]] float tab_stop_width() const noexcept;
    [[nodiscard]] std::size_t line_count() const;
    [[nodiscard]] std::span<const std::size_t> grapheme_boundaries() const noexcept;
    [[nodiscard]] TextHit hit_test(float x, float y) const;
    // LSP UTF-16 columns; an interior surrogate offset snaps to its scalar start.
    [[nodiscard]] std::size_t byte_offset_from_utf16(std::size_t column) const;
    // An interior byte offset is snapped back to the start of its grapheme.
    [[nodiscard]] TextCaret caret(std::size_t byte_offset,
                                  CaretAffinity affinity = CaretAffinity::downstream) const;
    // A partial grapheme is selected in full. The resulting boxes can be visually
    // disjoint for RTL runs or wrapped lines.
    [[nodiscard]] std::vector<TextRect> selection_rects(std::size_t begin, std::size_t end) const;
    [[nodiscard]] int unresolved_glyphs() const;
    [[nodiscard]] std::vector<std::string> resolved_font_families() const;
    void draw(SkCanvas* canvas, float x, float y) const;

  private:
    struct Impl;
    explicit TextLayout(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    friend class TextLayoutEngine;
};

// An engine and the layouts sharing its font caches have one owning thread.
// A worker may transfer a paragraph after retiring its private engine; it must
// never subsequently access that paragraph or reuse its fonts. Layouts may
// outlive the engine object. Callers serialize all paragraph access.
class TextLayoutEngine final {
  public:
    TextLayoutEngine();
    ~TextLayoutEngine();
    TextLayoutEngine(const TextLayoutEngine&) = delete;
    TextLayoutEngine& operator=(const TextLayoutEngine&) = delete;
    [[nodiscard]] TextLayout layout(std::string text,
                                    const TextLayoutOptions& options = {},
                                    TextSource previous = {},
                                    GlyphSource glyphs = {}) const;

  private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace astra::ui
