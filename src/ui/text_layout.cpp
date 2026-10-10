#include "astra/ui/text_layout.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkString.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skparagraph/include/ParagraphStyle.h"
#include "modules/skunicode/include/SkUnicode_icu.h"
#include "src/core/SkUTF.h"
#include <utf8proc.h>

#if defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(__linux__)
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
namespace paragraph = skia::textlayout;

std::vector<std::string> default_families() {
#if defined(_WIN32)
    return {"Cascadia Mono", "Consolas", "Microsoft YaHei UI", "Segoe UI Emoji"};
#elif defined(__APPLE__)
    return {"Menlo", "PingFang SC", "Apple Color Emoji"};
#else
    return {"monospace", "Noto Sans CJK SC", "Noto Color Emoji"};
#endif
}

sk_sp<SkFontMgr> native_font_manager() {
#if defined(_WIN32)
    return SkFontMgr_New_DirectWrite();
#elif defined(__APPLE__)
    return SkFontMgr_New_CoreText(nullptr);
#elif defined(__linux__)
    return SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#else
    throw std::runtime_error("No native font manager is available for this platform");
#endif
}

paragraph::TextDirection paragraph_direction(std::string_view text, TextDirection direction) {
    if (direction == TextDirection::right_to_left) {
        return paragraph::TextDirection::kRtl;
    }
    if (direction == TextDirection::left_to_right) {
        return paragraph::TextDirection::kLtr;
    }
    // Unicode first-strong paragraph direction, ignoring isolated contents.
    unsigned isolates{};
    const char* cursor = text.data();
    const auto* end = cursor + text.size();
    while (cursor != end) {
        const auto scalar = SkUTF::NextUTF8(&cursor, end);
        const auto bidi = utf8proc_get_property(scalar)->bidi_class;
        if (bidi == UTF8PROC_BIDI_CLASS_LRI || bidi == UTF8PROC_BIDI_CLASS_RLI ||
            bidi == UTF8PROC_BIDI_CLASS_FSI) {
            ++isolates;
        } else if (bidi == UTF8PROC_BIDI_CLASS_PDI) {
            if (isolates != 0) {
                --isolates;
            }
        } else if (isolates == 0) {
            if (bidi == UTF8PROC_BIDI_CLASS_R || bidi == UTF8PROC_BIDI_CLASS_AL) {
                return paragraph::TextDirection::kRtl;
            }
            if (bidi == UTF8PROC_BIDI_CLASS_L) {
                return paragraph::TextDirection::kLtr;
            }
        }
    }
    return paragraph::TextDirection::kLtr;
}

float next_tab_width(float advance, float stop) {
    const auto position = std::fmod(std::max(advance, 0.0F), stop);
    const auto remainder = stop - position;
    return remainder < 0.001F ? stop : remainder;
}

// SkParagraph inserts U+FFFC for placeholders. A tab is bidi class S, whereas
// U+FFFC is ON: preserve the original ICU bidi analysis so tab alignment cannot
// change the visual order it is itself measured from.
class TabUnicode final : public SkUnicode {
  public:
    TabUnicode(sk_sp<SkUnicode> delegate, std::string original)
        : delegate_(std::move(delegate)), original_(std::move(original)) {
        for (const auto byte : original_) {
            original_to_expanded_.push_back(expanded_.size());
            if (byte == '\t') {
                expanded_ += "\xEF\xBF\xBC";
            } else {
                expanded_ += byte;
            }
        }
        original_to_expanded_.push_back(expanded_.size());
    }

    SkString toUpper(const SkString& s) override {
        return delegate_->toUpper(s);
    }

    SkString toUpper(const SkString& s, const char* locale) override {
        return delegate_->toUpper(s, locale);
    }

    bool isControl(SkUnichar c) override {
        return delegate_->isControl(c);
    }

    bool isWhitespace(SkUnichar c) override {
        return delegate_->isWhitespace(c);
    }

    bool isSpace(SkUnichar c) override {
        return delegate_->isSpace(c);
    }

    bool isTabulation(SkUnichar c) override {
        return delegate_->isTabulation(c);
    }

    bool isHardBreak(SkUnichar c) override {
        return delegate_->isHardBreak(c);
    }

    bool isEmoji(SkUnichar c) override {
        return delegate_->isEmoji(c);
    }

    bool isEmojiComponent(SkUnichar c) override {
        return delegate_->isEmojiComponent(c);
    }

    bool isEmojiModifierBase(SkUnichar c) override {
        return delegate_->isEmojiModifierBase(c);
    }

    bool isEmojiModifier(SkUnichar c) override {
        return delegate_->isEmojiModifier(c);
    }

    bool isRegionalIndicator(SkUnichar c) override {
        return delegate_->isRegionalIndicator(c);
    }

    bool isIdeographic(SkUnichar c) override {
        return delegate_->isIdeographic(c);
    }

    std::unique_ptr<SkBidiIterator>
    makeBidiIterator(const std::uint16_t* s, int n, SkBidiIterator::Direction d) override {
        return delegate_->makeBidiIterator(s, n, d);
    }

    std::unique_ptr<SkBidiIterator>
    makeBidiIterator(const char* s, int n, SkBidiIterator::Direction d) override {
        return delegate_->makeBidiIterator(s, n, d);
    }

    std::unique_ptr<SkBreakIterator> makeBreakIterator(const char* locale,
                                                       BreakType type) override {
        return delegate_->makeBreakIterator(locale, type);
    }

    std::unique_ptr<SkBreakIterator> makeBreakIterator(BreakType type) override {
        return delegate_->makeBreakIterator(type);
    }

    bool getBidiRegions(const char* s,
                        int n,
                        SkUnicode::TextDirection direction,
                        std::vector<BidiRegion>* regions) override {
        if (std::string_view(s, static_cast<std::size_t>(n)) != expanded_) {
            return delegate_->getBidiRegions(s, n, direction, regions);
        }
        if (!delegate_->getBidiRegions(
                original_.data(), static_cast<int>(original_.size()), direction, regions)) {
            return false;
        }
        for (auto& region : *regions) {
            region.start = original_to_expanded_[region.start];
            region.end = original_to_expanded_[region.end];
        }
        return true;
    }

    bool
    getWords(const char* s, int n, const char* locale, std::vector<Position>* result) override {
        return delegate_->getWords(s, n, locale, result);
    }

    bool
    getUtf8Words(const char* s, int n, const char* locale, std::vector<Position>* result) override {
        return delegate_->getUtf8Words(s, n, locale, result);
    }

    bool
    getSentences(const char* s, int n, const char* locale, std::vector<Position>* result) override {
        return delegate_->getSentences(s, n, locale, result);
    }

    bool computeCodeUnitFlags(char* s,
                              int n,
                              bool tabs,
                              skia_private::TArray<CodeUnitFlags, true>* result) override {
        return delegate_->computeCodeUnitFlags(s, n, tabs, result);
    }

    bool computeCodeUnitFlags(char16_t* s,
                              int n,
                              bool tabs,
                              skia_private::TArray<CodeUnitFlags, true>* result) override {
        return delegate_->computeCodeUnitFlags(s, n, tabs, result);
    }

    void reorderVisual(const BidiLevel* levels, int count, std::int32_t* order) override {
        delegate_->reorderVisual(levels, count, order);
    }

  private:
    sk_sp<SkUnicode> delegate_;
    std::string original_;
    std::string expanded_;
    std::vector<std::size_t> original_to_expanded_;
};

} // namespace

struct TextLayoutEngine::Impl {
    sk_sp<paragraph::FontCollection> fonts = sk_make_sp<paragraph::FontCollection>();
    sk_sp<SkUnicode> unicode = SkUnicodes::ICU::Make();

    Impl() {
        if (!unicode) {
            throw std::runtime_error("ICU text layout initialization failed; check icudtl.dat");
        }
        auto manager = native_font_manager();
        if (!manager || manager->countFamilies() == 0) {
            throw std::runtime_error("Native font collection is empty");
        }
        fonts->setDefaultFontManager(std::move(manager));
        fonts->enableFontFallback();
    }
};

struct TextLayout::Impl {
    std::string text;
    std::unique_ptr<paragraph::Paragraph> shaped;
    sk_sp<paragraph::FontCollection> fonts;
    sk_sp<SkUnicode> unicode;
    std::vector<std::size_t> graphemes;
    std::vector<std::size_t> byte_to_utf16;
    std::vector<std::size_t> utf16_to_byte;
    float tab_stop{};
    float measured_width{};

    struct LineGeometry {
        double ascent{}, descent{}, baseline{}, left{}, width{};
    };

    // Compact immutable geometry. Native getLineMetricsAt rebuilds every style
    // run's font metrics, so calling it for each tab/caret is quadratic.
    std::vector<LineGeometry> lines;

    [[nodiscard]] std::size_t floor_boundary(std::size_t offset) const {
        if (offset > text.size()) {
            throw std::out_of_range("Text offset exceeds the UTF-8 line");
        }
        const auto after = std::upper_bound(graphemes.begin(), graphemes.end(), offset);
        return *std::prev(after);
    }

    [[nodiscard]] std::size_t ceil_boundary(std::size_t offset) const {
        if (offset > text.size()) {
            throw std::out_of_range("Text offset exceeds the UTF-8 line");
        }
        return *std::lower_bound(graphemes.begin(), graphemes.end(), offset);
    }

    [[nodiscard]] TextCaret caret(std::size_t offset, CaretAffinity affinity) const {
        offset = floor_boundary(offset);
        TextCaret result{offset, 0, 0, shaped->getHeight(), affinity};
        if (text.empty()) {
            return result;
        }
        auto probe = offset;
        bool trailing = affinity == CaretAffinity::upstream && offset != 0;
        if (offset == text.size()) {
            trailing = true;
        }
        if (trailing) {
            probe = *std::prev(std::lower_bound(graphemes.begin(), graphemes.end(), offset));
        }
        paragraph::Paragraph::GlyphInfo glyph{};
        if (!shaped->getGlyphInfoAtUTF16Offset(byte_to_utf16[probe], &glyph)) {
            throw std::runtime_error("Shaped paragraph has no glyph for the requested caret");
        }
        const bool rtl = glyph.fDirection == paragraph::TextDirection::kRtl;
        result.x = (trailing != rtl) ? glyph.fGraphemeLayoutBounds.right()
                                     : glyph.fGraphemeLayoutBounds.left();
        const auto line_index = shaped->getLineNumberAtUTF16Offset(byte_to_utf16[probe]);
        if (line_index >= 0 && static_cast<std::size_t>(line_index) < lines.size()) {
            const auto& metrics = lines[static_cast<std::size_t>(line_index)];
            result.y = static_cast<float>(metrics.baseline - metrics.ascent);
            result.height = static_cast<float>(metrics.ascent + metrics.descent);
        } else {
            result.y = glyph.fGraphemeLayoutBounds.top();
            result.height = glyph.fGraphemeLayoutBounds.height();
        }
        return result;
    }
};

TextLayoutEngine::TextLayoutEngine() : impl_(std::make_shared<Impl>()) {
}

TextLayoutEngine::~TextLayoutEngine() = default;

TextLayout TextLayoutEngine::layout(std::string text, const TextLayoutOptions& options) const {
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        SkUTF::CountUTF8(text.data(), text.size()) < 0) {
        throw std::invalid_argument("Text layout requires valid, bounded UTF-8");
    }
    if (text.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("TextLayout accepts one logical line at a time");
    }
    if (!std::isfinite(options.font_size) || options.font_size <= 0 ||
        !std::isfinite(options.dpi_scale) || options.dpi_scale <= 0 ||
        !std::isfinite(options.line_height) || options.line_height <= 0 ||
        !std::isfinite(options.font_size * options.dpi_scale * options.line_height) ||
        !std::isfinite(options.wrap_width) || options.wrap_width < 0 || options.tab_size == 0 ||
        options.tab_size > 256) {
        throw std::invalid_argument("Invalid font, DPI, line height, wrapping, or tab options");
    }

    auto result = std::make_unique<TextLayout::Impl>();
    result->text = std::move(text);
    result->fonts = impl_->fonts;
    result->unicode = impl_->unicode;
    result->byte_to_utf16.resize(result->text.size() + 1);
    const char* current = result->text.data();
    const auto* end = current + result->text.size();
    std::size_t index16{};
    while (current != end) {
        const auto begin8 = static_cast<std::size_t>(current - result->text.data());
        const auto scalar = SkUTF::NextUTF8(&current, end);
        const auto end8 = static_cast<std::size_t>(current - result->text.data());
        std::fill(result->byte_to_utf16.begin() + static_cast<std::ptrdiff_t>(begin8),
                  result->byte_to_utf16.begin() + static_cast<std::ptrdiff_t>(end8),
                  index16);
        result->utf16_to_byte.push_back(begin8);
        ++index16;
        if (scalar > 0xFFFF) {
            result->utf16_to_byte.push_back(begin8);
            ++index16;
        }
    }
    result->byte_to_utf16.back() = index16;
    result->utf16_to_byte.push_back(result->text.size());
    auto breaks = impl_->unicode->makeBreakIterator(SkUnicode::BreakType::kGraphemes);
    if (!breaks || !breaks->setText(result->text.data(), static_cast<int>(result->text.size()))) {
        throw std::runtime_error("ICU grapheme segmentation failed");
    }
    for (auto offset = breaks->first(); !breaks->isDone(); offset = breaks->next()) {
        result->graphemes.push_back(static_cast<std::size_t>(offset));
    }
    if (result->graphemes.empty() || result->graphemes.front() != 0) {
        result->graphemes.insert(result->graphemes.begin(), 0);
    }
    if (result->graphemes.back() != result->text.size()) {
        result->graphemes.push_back(result->text.size());
    }

    const auto families =
        options.font_families.empty() ? default_families() : options.font_families;
    std::vector<SkString> sk_families;
    for (const auto& family : families) {
        sk_families.emplace_back(family.c_str());
    }
    paragraph::TextStyle text_style;
    text_style.setFontFamilies(sk_families);
    text_style.setFontSize(options.font_size * options.dpi_scale);
    text_style.setColor(options.color);
    text_style.setLocale(SkString(options.locale.c_str()));
    paragraph::ParagraphStyle style;
    style.setTextStyle(text_style);
    style.setTextDirection(paragraph_direction(result->text, options.direction));
    if (options.wrap_width > 0 && style.getTextDirection() == paragraph::TextDirection::kRtl &&
        result->text.find('\t') != std::string::npos) {
        throw std::invalid_argument("Soft wrapping of RTL-base lines with tabs is not supported");
    }
    style.setTextAlign(style.getTextDirection() == paragraph::TextDirection::kRtl &&
                               options.wrap_width > 0
                           ? paragraph::TextAlign::kRight
                           : paragraph::TextAlign::kLeft);
    style.setReplaceTabCharacters(false);
    style.setApplyRoundingHack(false);
    paragraph::StrutStyle strut;
    strut.setFontFamilies(sk_families);
    strut.setFontSize(options.font_size * options.dpi_scale);
    strut.setHeight(options.line_height);
    strut.setHeightOverride(true);
    strut.setStrutEnabled(true);
    style.setStrutStyle(strut);

    const auto make_text = [&](std::string_view part) {
        auto builder = paragraph::ParagraphBuilder::make(style, impl_->fonts, impl_->unicode);
        builder->addText(part.data(), part.size());
        auto shaped = builder->Build();
        shaped->layout(std::numeric_limits<float>::infinity());
        return shaped;
    };
    auto space = make_text(" ");
    result->tab_stop = space->getMaxIntrinsicWidth() * static_cast<float>(options.tab_size);
    if (!std::isfinite(result->tab_stop) || result->tab_stop <= 0) {
        throw std::runtime_error("The selected font has no usable space advance");
    }

    std::vector<float> tab_widths;
    std::vector<std::size_t> tab_offsets;
    float advance{};
    std::size_t segment_begin{};
    // These are independent width estimates for tab-delimited segments, with
    // one unchanged paragraph style. Repeated segments have identical widths;
    // cache only this estimate, never a slice of the final shaped paragraph.
    constexpr std::size_t max_segment_estimates = 256;
    std::unordered_map<std::string_view, float> segment_estimates;
    for (std::size_t i = 0; i < result->text.size(); ++i) {
        if (result->text[i] != '\t') {
            continue;
        }
        if (i != segment_begin) {
            const auto segment =
                std::string_view(result->text).substr(segment_begin, i - segment_begin);
            const auto found = segment_estimates.find(segment);
            float segment_width{};
            if (found != segment_estimates.end()) {
                segment_width = found->second;
            } else {
                segment_width = make_text(segment)->getMaxIntrinsicWidth();
                if (segment_estimates.size() < max_segment_estimates) {
                    segment_estimates.emplace(segment, segment_width);
                }
            }
            advance += segment_width;
        }
        const auto width = next_tab_width(advance, result->tab_stop);
        tab_widths.push_back(width);
        tab_offsets.push_back(i);
        advance += width;
        segment_begin = i + 1;
    }

    const float layout_width =
        options.wrap_width > 0 ? options.wrap_width : std::numeric_limits<float>::infinity();
    sk_sp<SkUnicode> layout_unicode =
        tab_widths.empty() ? impl_->unicode : sk_make_sp<TabUnicode>(impl_->unicode, result->text);
    std::vector<TextColorSpan> colors;
    std::vector<std::size_t> color_boundaries{0, result->text.size()};
    for (const auto& span : options.color_spans) {
        if (span.begin > span.end || span.end > result->text.size()) {
            throw std::invalid_argument("Text color range exceeds the line");
        }
        if (span.begin == span.end) {
            continue;
        }
        const auto begin = result->floor_boundary(span.begin);
        const auto range_end = result->ceil_boundary(span.end);
        colors.push_back({begin, range_end, span.color});
        color_boundaries.push_back(begin);
        color_boundaries.push_back(range_end);
    }
    std::ranges::sort(color_boundaries);
    color_boundaries.erase(std::unique(color_boundaries.begin(), color_boundaries.end()),
                           color_boundaries.end());
    const auto build_paragraph = [&] {
        auto builder = paragraph::ParagraphBuilder::make(style, impl_->fonts, layout_unicode);
        std::size_t tab{};
        for (std::size_t range = 1; range < color_boundaries.size(); ++range) {
            auto begin = color_boundaries[range - 1];
            const auto end = color_boundaries[range];
            auto run_style = text_style;
            for (const auto& span : colors) {
                if (span.begin <= begin && span.end > begin) {
                    run_style.setColor(span.color);
                }
            }
            builder->pushStyle(run_style);
            for (std::size_t i = begin; i < end; ++i) {
                if (result->text[i] != '\t') {
                    continue;
                }
                builder->addText(result->text.data() + begin, i - begin);
                builder->addPlaceholder({tab_widths[tab++],
                                         space->getHeight(),
                                         paragraph::PlaceholderAlignment::kBaseline,
                                         paragraph::TextBaseline::kAlphabetic,
                                         space->getAlphabeticBaseline()});
                begin = i + 1;
            }
            builder->addText(result->text.data() + begin, end - begin);
            builder->pop();
        }
        result->shaped = builder->Build();
        result->shaped->layout(layout_width);
        std::vector<paragraph::LineMetrics> metrics;
        result->shaped->getLineMetrics(metrics);
        result->lines.clear();
        result->lines.reserve(metrics.size());
        for (const auto& line : metrics) {
            result->lines.push_back(
                {line.fAscent, line.fDescent, line.fBaseline, line.fLeft, line.fWidth});
        }
    };
    build_paragraph();
    // Wrapping and bidi placement can move a tab to another visual position.
    // Recompute from the actual shaped position instead of estimating columns.
    for (unsigned pass = 0; !tab_widths.empty() && pass < 16; ++pass) {
        const auto boxes = result->shaped->getRectsForPlaceholders();
        if (boxes.size() != tab_widths.size()) {
            throw std::runtime_error("Shaped paragraph lost a tab placeholder");
        }
        bool changed{};
        std::vector<std::size_t> visual_order;
        std::vector<int> line_numbers;
        std::vector<float> positions;
        const bool rtl_tabs = style.getTextDirection() == paragraph::TextDirection::kRtl;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const auto number =
                result->shaped->getLineNumberAtUTF16Offset(result->byte_to_utf16[tab_offsets[i]]);
            if (number < 0 || static_cast<std::size_t>(number) >= result->lines.size()) {
                throw std::runtime_error("Tab line metrics are unavailable");
            }
            const auto& metrics = result->lines[static_cast<std::size_t>(number)];
            line_numbers.push_back(number);
            positions.push_back(rtl_tabs
                                    ? (options.wrap_width > 0
                                           ? options.wrap_width
                                           : static_cast<float>(metrics.left + metrics.width)) -
                                          boxes[i].rect.right()
                                    : boxes[i].rect.left() - static_cast<float>(metrics.left));
        }
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            visual_order.push_back(i);
        }
        std::ranges::sort(visual_order, [&](std::size_t lhs, std::size_t rhs) {
            if (line_numbers[lhs] != line_numbers[rhs]) {
                return line_numbers[lhs] < line_numbers[rhs];
            }
            return positions[lhs] < positions[rhs];
        });
        int current_line = -1;
        float shifted{};
        for (const auto i : visual_order) {
            if (line_numbers[i] != current_line) {
                shifted = 0;
                current_line = line_numbers[i];
            }
            const auto desired = next_tab_width(positions[i] + shifted, result->tab_stop);
            if (std::abs(desired - tab_widths[i]) > 0.01F) {
                shifted += desired - tab_widths[i];
                tab_widths[i] = desired;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
        if (pass == 15) {
            throw std::runtime_error("Tab stop layout did not converge for this wrapped bidi line");
        }
        build_paragraph();
    }
    result->measured_width = options.wrap_width > 0 ? result->shaped->getLongestLine()
                                                    : result->shaped->getMaxIntrinsicWidth();
    return TextLayout(std::move(result));
}

TextLayout::TextLayout(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
}

TextLayout::~TextLayout() = default;
TextLayout::TextLayout(TextLayout&&) noexcept = default;
TextLayout& TextLayout::operator=(TextLayout&&) noexcept = default;

std::string_view TextLayout::text() const noexcept {
    return impl_->text;
}

float TextLayout::width() const {
    return impl_->measured_width;
}

float TextLayout::height() const {
    return impl_->shaped->getHeight();
}

std::size_t TextLayout::byte_offset_from_utf16(std::size_t column) const {
    return impl_->utf16_to_byte[std::min(column, impl_->utf16_to_byte.size() - 1)];
}

float TextLayout::baseline() const {
    return impl_->shaped->getAlphabeticBaseline();
}

float TextLayout::tab_stop_width() const noexcept {
    return impl_->tab_stop;
}

std::size_t TextLayout::line_count() const {
    return impl_->shaped->lineNumber();
}

std::span<const std::size_t> TextLayout::grapheme_boundaries() const noexcept {
    return impl_->graphemes;
}

TextCaret TextLayout::caret(std::size_t offset, CaretAffinity affinity) const {
    return impl_->caret(offset, affinity);
}

TextHit TextLayout::hit_test(float x, float y) const {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        throw std::invalid_argument("Hit-test coordinates must be finite");
    }
    const auto raw = impl_->shaped->getGlyphPositionAtCoordinate(x, y);
    const auto index = std::min(static_cast<std::size_t>(std::max(raw.position, 0)),
                                impl_->utf16_to_byte.size() - 1);
    const auto offset = impl_->utf16_to_byte[index];
    const auto affinity = raw.affinity == paragraph::Affinity::kUpstream
                              ? CaretAffinity::upstream
                              : CaretAffinity::downstream;
    const auto low = impl_->floor_boundary(offset);
    const auto high = impl_->ceil_boundary(offset);
    if (low == high) {
        return {offset, affinity};
    }
    const auto before = impl_->caret(low, CaretAffinity::downstream);
    const auto after = impl_->caret(high, CaretAffinity::upstream);
    const auto distance = [&](const TextCaret& caret) {
        return std::abs(caret.x - x) + std::abs(caret.y + caret.height / 2 - y);
    };
    return distance(before) <= distance(after) ? TextHit{low, CaretAffinity::downstream}
                                               : TextHit{high, CaretAffinity::upstream};
}

std::vector<TextRect> TextLayout::selection_rects(std::size_t begin, std::size_t end) const {
    if (begin > end) {
        std::swap(begin, end);
    }
    if (begin == end) {
        (void)impl_->floor_boundary(begin);
        return {};
    }
    begin = impl_->floor_boundary(begin);
    end = impl_->ceil_boundary(end);
    const auto boxes =
        impl_->shaped->getRectsForRange(static_cast<unsigned>(impl_->byte_to_utf16[begin]),
                                        static_cast<unsigned>(impl_->byte_to_utf16[end]),
                                        paragraph::RectHeightStyle::kMax,
                                        paragraph::RectWidthStyle::kTight);
    std::vector<TextRect> rectangles;
    rectangles.reserve(boxes.size());
    for (const auto& box : boxes) {
        rectangles.push_back({box.rect.x(), box.rect.y(), box.rect.width(), box.rect.height()});
    }
    return rectangles;
}

int TextLayout::unresolved_glyphs() const {
    return impl_->shaped->unresolvedGlyphs();
}

std::vector<std::string> TextLayout::resolved_font_families() const {
    std::vector<std::string> families;
    for (const auto& font : impl_->shaped->getFonts()) {
        if (!font.fFont.getTypeface()) {
            continue;
        }
        SkString family;
        font.fFont.getTypeface()->getFamilyName(&family);
        if (std::ranges::find(families, family.c_str()) == families.end()) {
            families.emplace_back(family.c_str());
        }
    }
    return families;
}

void TextLayout::draw(SkCanvas* canvas, float x, float y) const {
    if (!canvas || !std::isfinite(x) || !std::isfinite(y)) {
        throw std::invalid_argument("Invalid text drawing destination");
    }
    impl_->shaped->paint(canvas, x, y);
}

} // namespace astra::ui
