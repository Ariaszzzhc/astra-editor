#pragma once

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

struct TextLayoutOptions {
    float font_size{16.0F};
    float dpi_scale{1.0F};
    float line_height{1.4F};
    unsigned tab_size{4};
    // Physical pixels; zero disables soft wrapping. Wrapping an RTL-base line
    // containing tabs is rejected; unwrapped RTL and wrapped LTR tabs are supported.
    float wrap_width{};
    std::uint32_t color{0xFFE2E5EC};
    TextDirection direction{TextDirection::left_to_right};
    std::vector<std::string> font_families;
    std::string locale{"en"};
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
    [[nodiscard]] float width() const;
    [[nodiscard]] float height() const;
    [[nodiscard]] float baseline() const;
    [[nodiscard]] float tab_stop_width() const noexcept;
    [[nodiscard]] std::size_t line_count() const;
    [[nodiscard]] std::span<const std::size_t> grapheme_boundaries() const noexcept;
    [[nodiscard]] TextHit hit_test(float x, float y) const;
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

// Keep one engine per UI thread. Its native font manager and shaped-run caches
// are shared by its layouts; layouts may safely outlive the engine object.
class TextLayoutEngine final {
public:
    TextLayoutEngine();
    ~TextLayoutEngine();
    TextLayoutEngine(const TextLayoutEngine&) = delete;
    TextLayoutEngine& operator=(const TextLayoutEngine&) = delete;
    [[nodiscard]] TextLayout layout(std::string text, const TextLayoutOptions& options = {}) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace astra::ui
