#include "astra/ui/renderer.hpp"
#include "astra/ui/text_layout.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using astra::ui::CaretAffinity;
using astra::ui::TextDirection;
using astra::ui::TextLayout;
using astra::ui::TextLayoutEngine;
using astra::ui::TextLayoutOptions;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void near(float actual, float expected, const std::string& message, float tolerance = 0.1F) {
    require(std::abs(actual - expected) <= tolerance,
            message + ": " + std::to_string(actual) + " vs " + std::to_string(expected));
}

void roundtrips(const TextLayout& line, bool exact_offsets = true) {
    for (const auto offset : line.grapheme_boundaries()) {
        const auto caret = line.caret(offset);
        require(caret.byte_offset == offset && caret.height > 0,
                "Caret did not preserve a grapheme boundary");
        const auto hit = line.hit_test(caret.x, caret.y + caret.height / 2);
        require(std::ranges::find(line.grapheme_boundaries(), hit.byte_offset) !=
                    line.grapheme_boundaries().end(),
                "Hit test split a grapheme");
        if (exact_offsets) {
            require(hit.byte_offset == offset,
                    "Caret/hit UTF-8 roundtrip failed at byte " + std::to_string(offset) + " in " +
                        std::string(line.text()) + " (got " + std::to_string(hit.byte_offset) +
                        ")");
        } else {
            // At a bidi-run boundary, two logical offsets may share a visual
            // insertion position. Verify the recovered affinity preserves it.
            const auto recovered = line.caret(hit.byte_offset, hit.affinity);
            near(recovered.x, caret.x, "Bidi caret/hit visual roundtrip failed");
        }
    }
    for (float x = -20; x < line.width() + 20; x += 1.75F) {
        const auto hit = line.hit_test(x, line.height() / 2);
        require(std::ranges::find(line.grapheme_boundaries(), hit.byte_offset) !=
                    line.grapheme_boundaries().end(),
                "An arbitrary hit split a grapheme");
    }
}

} // namespace

int main() {
    try {
        TextLayoutEngine engine;
        auto empty = engine.layout("");
        require(empty.width() == 0 && empty.height() > 0, "Empty line has invalid metrics");
        require(empty.hit_test(500, 0).byte_offset == 0, "Empty line hit is not byte zero");
        require(empty.selection_rects(0, 0).empty(), "Empty selection has boxes");
        roundtrips(empty);

        auto ascii = engine.layout("const value = 42;");
        roundtrips(ascii);
        auto cjk = engine.layout("中文编辑器");
        require(cjk.unresolved_glyphs() == 0, "Native CJK fallback is missing");
        roundtrips(cjk);

        auto combining = engine.layout("e\xCC\x81");
        require(combining.grapheme_boundaries().size() == 2 &&
                    combining.grapheme_boundaries()[1] == 3,
                "Combining sequence was split by ICU");
        require(combining.caret(2).byte_offset == 0, "Interior UTF-8 caret was not snapped");
        const auto partial = combining.selection_rects(1, 2);
        const auto whole = combining.selection_rects(0, 3);
        require(partial.size() == whole.size() && !partial.empty(),
                "Partial grapheme selection was not expanded");
        near(partial.front().width, whole.front().width, "Combining selection width differs");
        roundtrips(combining);

        auto emoji = engine.layout("😀👩‍💻🇨🇳");
        require(emoji.grapheme_boundaries().size() == 4,
                "Emoji, ZWJ, or regional-indicator grapheme split");
        require(emoji.unresolved_glyphs() == 0, "Native emoji fallback is missing");
        roundtrips(emoji);

        TextLayoutOptions rtl_options;
        rtl_options.direction = TextDirection::right_to_left;
        auto rtl = engine.layout("שלום", rtl_options);
        require(rtl.caret(0).x > rtl.caret(rtl.text().size()).x,
                "RTL caret direction is incorrect");
        roundtrips(rtl);
        auto mixed = engine.layout("abc אבג def");
        roundtrips(mixed, false);
        const auto mixed_boxes = mixed.selection_rects(0, mixed.text().size());
        require(mixed_boxes.size() >= 3,
                "Mixed-direction selection did not preserve separate shaped runs");

        auto tabs = engine.layout("a\tb\t中文\tz");
        roundtrips(tabs);
        for (std::size_t i = 0; i < tabs.text().size(); ++i) {
            if (tabs.text()[i] != '\t') {
                continue;
            }
            const auto caret = tabs.caret(i + 1, CaretAffinity::upstream);
            const auto stops = caret.x / tabs.tab_stop_width();
            near(stops, std::round(stops), "Tab did not reach a physical tab stop", 0.005F);
            const auto rectangles = tabs.selection_rects(i, i + 1);
            require(!rectangles.empty() && rectangles.front().width > 0,
                    "Tab selection has no physical width");
        }
        auto trailing_spaces = engine.layout("a   \tz");
        near(trailing_spaces.caret(5).x,
             2 * trailing_spaces.tab_stop_width(),
             "Spaces before tab were discarded");
        TextLayoutOptions proportional_options;
        proportional_options.font_families = {"Segoe UI"};
        auto proportional_tabs = engine.layout("iiii\tW\t中文\tx", proportional_options);
        for (std::size_t i = 0; i < proportional_tabs.text().size(); ++i) {
            if (proportional_tabs.text()[i] != '\t') {
                continue;
            }
            const auto stops = proportional_tabs.caret(i + 1, CaretAffinity::upstream).x /
                               proportional_tabs.tab_stop_width();
            near(stops, std::round(stops), "Proportional font tab did not align", 0.005F);
        }

        TextLayoutOptions dpi_options;
        dpi_options.dpi_scale = 2;
        auto scaled = engine.layout("const value = 42;", dpi_options);
        near(scaled.width(), ascii.width() * 2, "DPI scaling did not scale shaped width", 1.0F);
        require(scaled.height() > ascii.height() * 1.8F, "DPI scaling did not scale line metrics");

        const std::string colored_text = "const\t中文 e\xCC\x81 אבג = 42;";
        auto uncolored = engine.layout(colored_text);
        TextLayoutOptions colored_options;
        colored_options.color_spans = {{0, 5, 0xFFC586C0},
                                       {6, 12, 0xFF4EC9B0},
                                       {13, 15, 0xFFCE9178},
                                       {16, colored_text.size(), 0xFFDCDCAA}};
        auto colored = engine.layout(colored_text, colored_options);
        const auto columns = engine.layout("A🙂中文");
        require(
            columns.byte_offset_from_utf16(0) == 0 && columns.byte_offset_from_utf16(1) == 1 &&
                columns.byte_offset_from_utf16(2) == 1 && columns.byte_offset_from_utf16(3) == 5 &&
                columns.byte_offset_from_utf16(4) == 8 && columns.byte_offset_from_utf16(100) == 11,
            "LSP UTF-16 columns were confused with UTF-8 byte offsets");
        near(colored.width(), uncolored.width(), "Syntax colors changed shaping width");
        for (const auto offset : uncolored.grapheme_boundaries()) {
            near(colored.caret(offset).x,
                 uncolored.caret(offset).x,
                 "Syntax colors changed caret geometry");
            near(colored.caret(offset).y,
                 uncolored.caret(offset).y,
                 "Syntax colors changed line geometry");
        }
        for (float x = 0; x < colored.width(); x += 2.5F) {
            require(colored.hit_test(x, colored.height() / 2).byte_offset ==
                        uncolored.hit_test(x, uncolored.height() / 2).byte_offset,
                    "Syntax colors changed bidi/grapheme hit testing");
        }

        TextLayoutOptions wrapped_options;
        wrapped_options.wrap_width = 110;
        auto wrapped = engine.layout("one two three four five six", wrapped_options);
        require(wrapped.line_count() > 1, "Soft wrapping did not create visual lines");
        roundtrips(wrapped);
        auto wrapped_tabs = engine.layout("one\ttwo three\tfour", wrapped_options);
        require(wrapped_tabs.line_count() > 1, "Wrapped tabs did not create visual lines");
        roundtrips(wrapped_tabs);
        std::mt19937 random(0xA57A);
        const std::vector<std::string> fragments{
            "abc", "中文", "😀", "e\xCC\x81", " ", "\t", "אבג"};
        for (unsigned iteration = 0; iteration < 60; ++iteration) {
            std::string text;
            for (unsigned part = 0; part < 10; ++part) {
                text += fragments[random() % fragments.size()];
            }
            TextLayoutOptions options;
            if (iteration % 2 == 0) {
                options.wrap_width = 150;
            }
            options.direction =
                iteration % 3 == 0 ? TextDirection::right_to_left : TextDirection::left_to_right;
            if (options.direction == TextDirection::right_to_left &&
                text.find('\t') != std::string::npos) {
                options.wrap_width = 0;
            }
            auto line = [&] {
                try {
                    return engine.layout(text, options);
                } catch (const std::exception& error) {
                    throw std::runtime_error("Random case " + std::to_string(iteration) + " [" +
                                             text + "]: " + error.what());
                }
            }();
            for (float y = 0; y < line.height(); y += 5.5F) {
                for (float x = 0; x < std::min(200.0F, line.width()); x += 3.25F) {
                    const auto hit = line.hit_test(x, y);
                    require(std::ranges::find(line.grapheme_boundaries(), hit.byte_offset) !=
                                line.grapheme_boundaries().end(),
                            "Randomized bidi/tab hit split a grapheme");
                    const auto caret = line.caret(hit.byte_offset, hit.affinity);
                    require(std::isfinite(caret.x) && std::isfinite(caret.y) && caret.height > 0,
                            "Randomized bidi/tab caret has invalid metrics");
                }
            }
        }
        bool rejected_rtl_tab_wrap{};
        try {
            TextLayoutOptions unsupported;
            unsupported.wrap_width = 150;
            unsupported.direction = TextDirection::right_to_left;
            (void)engine.layout("éאבג\t😀😀é 中文אבגé", unsupported);
        } catch (const std::invalid_argument&) {
            rejected_rtl_tab_wrap = true;
        }
        require(rejected_rtl_tab_wrap,
                "Unsupported RTL/tab wrapping silently produced incorrect geometry");

        bool rejected_invalid{};
        try {
            (void)engine.layout(std::string("\xF0\x28\x8C\x28", 4));
        } catch (const std::invalid_argument&) {
            rejected_invalid = true;
        }
        require(rejected_invalid, "Malformed UTF-8 was accepted");

        astra::ui::Renderer renderer("Astra shaped Unicode text verification", 1040, 500);
        renderer.poll_events([](const SDL_Event&) {
        });
        auto* canvas = renderer.begin_frame();
        require(canvas != nullptr, "Native canvas was unavailable for text drawing");
        canvas->clear(SkColorSetRGB(24, 28, 37));
        std::vector<TextLayout> examples;
        for (const auto& text : {"const answer = 42;",
                                 "中文编辑器 — 文本塑形",
                                 "😀 👩‍💻 🇨🇳",
                                 "e\xCC\x81 a\xCC\x88 o\xCC\x82",
                                 "abc אבג def",
                                 "a\tb\t中文\tz"}) {
            TextLayoutOptions options;
            options.font_size = 24;
            examples.push_back(engine.layout(text, options));
        }
        float y = 24;
        colored.draw(canvas, 430, 30);
        for (const auto& line : examples) {
            SkPaint selection;
            selection.setColor(SkColorSetARGB(120, 60, 95, 160));
            for (const auto& box : line.selection_rects(0, line.text().size())) {
                canvas->drawRect(SkRect::MakeXYWH(24 + box.x, y + box.y, box.width, box.height),
                                 selection);
            }
            line.draw(canvas, 24, y);
            const auto caret = line.caret(line.text().size());
            SkPaint caret_paint;
            caret_paint.setColor(SK_ColorWHITE);
            canvas->drawRect(SkRect::MakeXYWH(24 + caret.x, y + caret.y, 2, caret.height),
                             caret_paint);
            y += line.height() + 15;
        }
        renderer.capture_png("build/graphics/text-layout-smoke.png");
        renderer.end_frame();
        std::cout << "SkParagraph/HarfBuzz/ICU native fonts, Unicode hit/caret/ranges, bidi, tabs, "
                     "wrap, DPI, GPU text: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Text layout test failed: " << error.what() << '\n';
        return 1;
    }
}
