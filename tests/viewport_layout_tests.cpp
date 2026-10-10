#include <astra/ui/document_viewport.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace astra::ui;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

std::vector<ViewportLine>
rows(std::vector<std::string> text, std::size_t first = 0, std::size_t begin = 0) {
    std::vector<ViewportLine> result;
    for (auto& value : text) {
        const auto end = begin + value.size();
        result.push_back({first++, begin, end, end + 1, true, std::move(value)});
        begin = end + 1;
    }
    return result;
}

std::vector<std::uint32_t> pixels(const TextLayout& layout) {
    const auto info = SkImageInfo::MakeN32Premul(640, 180);
    auto surface = SkSurfaces::Raster(info);
    require(surface != nullptr, "Cannot create a paragraph pixel oracle");
    auto* canvas = surface->getCanvas();
    canvas->clear(0xFF101722);
    layout.draw(canvas, 8, 8);
    std::vector<std::uint32_t> result(640 * 180);
    require(surface->readPixels(info, result.data(), 640 * sizeof(std::uint32_t), 0, 0),
            "Cannot read paragraph pixels");
    return result;
}

void equal(const TextLayout& cached, const TextLayout& fresh) {
    require(cached.text() == fresh.text(), "Cached text differs from the paragraph oracle");
    require(std::abs(cached.width() - fresh.width()) < 0.01F &&
                std::abs(cached.height() - fresh.height()) < 0.01F,
            "Cached geometry differs from the paragraph oracle");
    require(pixels(cached) == pixels(fresh), "Cached colored pixels differ from fresh shaping");
    for (const auto offset : fresh.grapheme_boundaries()) {
        const auto a = cached.caret(offset), b = fresh.caret(offset);
        require(a.byte_offset == b.byte_offset && a.x == b.x && a.y == b.y,
                "Cached grapheme caret differs from fresh shaping");
        require(cached.byte_offset_from_utf16(offset) == fresh.byte_offset_from_utf16(offset),
                "Cached LSP column conversion differs");
        const auto hit = cached.hit_test(a.x, a.y + a.height / 2);
        require(std::ranges::find(cached.grapheme_boundaries(), hit.byte_offset) !=
                    cached.grapheme_boundaries().end(),
                "Cached hit splits a grapheme");
    }
}

void reuse(TextLayoutEngine& engine) {
    DocumentViewport viewport(engine);
    TextLayoutOptions style;
    style.color_spans = {{0, 5, 0xFFE01030}};
    viewport.publish(rows({"const 中🙂é\tvalue", "unchanged line"}));
    const auto& original = viewport.shape(0, style);
    const auto retained = viewport.layout_snapshot(0);
    const auto before = viewport.statistics();
    viewport.publish(rows({"new header", "const 中🙂é\tvalue", "unchanged line"}, 40, 800));
    const auto& shifted = viewport.shape(41, style);
    require(&original == &shifted, "Moving an identical line reshaped its paragraph");
    require(viewport.statistics().shaped == before.shaped &&
                viewport.statistics().reused > before.reused,
            "Moved-line shaping was not reused");
    auto colored = style;
    colored.color_spans.front().color = 0xFF20C030;
    const auto& changed = viewport.shape(41, colored);
    require(&changed != retained.get(), "Changed relative colors reused an old paragraph");
    equal(changed, engine.layout("const 中🙂é\tvalue", colored));
    require(pixels(changed) != pixels(*retained), "Color change did not change painted pixels");
    viewport.invalidate_layouts();
    require(!viewport.layout(41), "Invalidation retained an interactive stale layout");
    require(&viewport.shape(41, style) == retained.get(),
            "Restoring a style did not reuse its paragraph");
    auto invalid = rows({"bad"});
    invalid.front().complete = false;
    bool rejected{};
    try {
        viewport.publish(std::move(invalid));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && viewport.lines().find(41) && viewport.layout(41) == retained.get(),
            "Rejected publication changed interactive content");
    auto invalid_style = style;
    invalid_style.tab_size = 0;
    rejected = false;
    try {
        (void)viewport.shape(41, invalid_style);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && viewport.layout(41) == retained.get(),
            "Failed shaping replaced a current layout");
}

void limits(TextLayoutEngine& engine) {
    DocumentViewport viewport(engine, {2, 700});
    viewport.publish(rows({"one", "two", "three", std::string(2000, 'a')}));
    TextLayoutOptions style;
    (void)viewport.shape(0, style);
    const auto entry_bytes = viewport.statistics().accounted_bytes;
    const auto retained = viewport.layout_snapshot(0);
    (void)viewport.shape(1, style);
    (void)viewport.shape(0, style);
    (void)viewport.shape(2, style);
    const auto stats = viewport.statistics();
    require(stats.entries == 2 && stats.evicted == 1 && stats.accounted_bytes <= 700,
            "LRU entry/byte budget was exceeded");
    viewport.invalidate_layouts();
    require(&viewport.shape(0, style) == retained.get(),
            "Recently used paragraph was evicted first");
    const auto before = viewport.statistics();
    (void)viewport.shape(3, style);
    require(viewport.statistics().entries == before.entries &&
                viewport.statistics().accounted_bytes == before.accounted_bytes,
            "Oversize paragraph bypass did not preserve the bounded cache");
    require(retained->text() == "one" && retained->width() > 0,
            "Cache eviction invalidated an external paragraph snapshot");
    DocumentViewport byte_limited(engine, {8, entry_bytes * 2});
    byte_limited.publish(rows({"one", "two", "six"}));
    for (std::size_t line = 0; line < 3; ++line)
        (void)byte_limited.shape(line, style);
    require(byte_limited.statistics().entries == 2 && byte_limited.statistics().evicted == 1 &&
                byte_limited.statistics().accounted_bytes == entry_bytes * 2,
            "Byte capacity did not evict without exhausting entry capacity");
}

void option_keys(TextLayoutEngine& engine) {
    DocumentViewport viewport(engine);
    const std::string text = "abc אבג 中文🙂é";
    viewport.publish(rows({text}));
    TextLayoutOptions base;
    const auto& baseline = viewport.shape(0, base);
    for (int field = 0; field < 10; ++field) {
        auto style = base;
        switch (field) {
        case 0:
            style.font_size = 20;
            break;
        case 1:
            style.dpi_scale = 1.5F;
            break;
        case 2:
            style.line_height = 2;
            break;
        case 3:
            style.tab_size = 2;
            break;
        case 4:
            style.wrap_width = 100;
            break;
        case 5:
            style.color = 0xFFE01030;
            break;
        case 6:
            style.color_spans = {{0, 3, 0xFF20E010}};
            break;
        case 7:
            style.direction = TextDirection::right_to_left;
            break;
        case 8:
            style.font_families = {"Segoe UI", "Segoe UI Emoji"};
            break;
        case 9:
            style.locale = "zh";
            break;
        }
        const auto& variant = viewport.shape(0, style);
        require(&variant != &baseline, "A shaping option was omitted from the cache identity");
        equal(variant, engine.layout(text, style));
    }
    require(&viewport.shape(0, base) == &baseline,
            "Option changes replaced a retained immutable style");
}

void differential(TextLayoutEngine& engine) {
    constexpr unsigned int seed = 0xA57AF07;
    std::mt19937 random(seed);
    DocumentViewport viewport(engine, {8, 8192});
    const std::vector<std::string> text{"中🙂é\tvalue", "abc אבג def", "é 中文 中🙂"};
    for (int iteration = 0; iteration < 80; ++iteration) {
        auto style = TextLayoutOptions{};
        style.font_size = 12 + static_cast<float>(random() % 8);
        style.dpi_scale = iteration % 2 ? 1.25F : 1.0F;
        style.line_height = iteration % 3 ? 1.4F : 1.8F;
        style.tab_size = 2 + random() % 7;
        style.color = iteration % 2 ? 0xFF70A0C0 : 0xFFB0D040;
        style.color_spans = {{0, 2, iteration % 2 ? 0xFFC04070U : 0xFF10E050U}};
        style.font_families = iteration % 2
                                  ? std::vector<std::string>{"Consolas", "Segoe UI Emoji"}
                                  : std::vector<std::string>{"Segoe UI", "Microsoft YaHei"};
        style.locale = iteration % 2 ? "en" : "zh";
        style.direction = iteration % 2 ? TextDirection::automatic : TextDirection::left_to_right;
        const auto& value = text[random() % text.size()];
        style.wrap_width =
            value.find('\t') == std::string::npos && iteration % 3 == 0 ? 80.0F : 0.0F;
        viewport.publish(rows({value}, iteration, iteration * 100));
        const auto& cached = viewport.shape(iteration, style);
        const auto fresh = engine.layout(value, style);
        equal(cached, fresh);
        const auto shaped = viewport.statistics().shaped;
        viewport.invalidate_layouts();
        require(&viewport.shape(iteration, style) == &cached &&
                    viewport.statistics().shaped == shaped,
                "Exact style did not reuse a cached paragraph");
        const auto stats = viewport.statistics();
        require(stats.entries <= 8 && stats.accounted_bytes <= 8192,
                "Random cache publication exceeded budgets");
    }
    std::cout << "Viewport pixel/Unicode/style differential seed=" << seed << ": PASS\n";
}
} // namespace

int main() {
    try {
        TextLayoutEngine engine;
        reuse(engine);
        limits(engine);
        option_keys(engine);
        differential(engine);
        std::shared_ptr<const TextLayout> retained;
        {
            TextLayoutEngine temporary;
            DocumentViewport viewport(temporary);
            viewport.publish(rows({"retained 中🙂"}));
            (void)viewport.shape(0, {});
            retained = viewport.layout_snapshot(0);
        }
        require(retained->width() > 0 && retained->caret(0).byte_offset == 0,
                "Paragraph did not survive viewport/engine destruction");
        std::cout << "Windows document viewport: exact style reuse, relative colors, LRU limits, "
                     "failure preservation and immutable paragraph lifetime: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Document viewport failed: " << error.what() << '\n';
        return 1;
    }
}
