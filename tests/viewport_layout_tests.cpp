#include <astra/ui/document_viewport.hpp>
#include <astra/ui/text_surface.hpp>
#include <astra/ui/text_input_layout.hpp>
#include <astra/ui/input_events.hpp>
#include <astra/ui/text_labels.hpp>
#include <astra/ui/text_field.hpp>
#include <astra/ui/tab_bar.hpp>
#include "../src/ui/glyph_cache.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
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

void text_fields(TextLayoutEngine& engine) {
    constexpr std::uint32_t untouched = 0xFF102030;
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(640, 180));
    require(surface != nullptr, "Cannot create text field pixel surface");
    auto* canvas = surface->getCanvas();
    TextLabels labels(engine);
    TextField field(engine);
    TextFieldPaint paint;
    paint.bounds = {20, 20, 180, 32};
    paint.text_style.font_size = 13;
    paint.text_style.line_height = 1;
    paint.text_style.direction = TextDirection::automatic;
    paint.text_style.font_families = {"Segoe UI", "Microsoft YaHei UI", "Segoe UI Emoji"};
    paint.hint_style = paint.text_style;
    paint.hint_style.color = 0xFF999999;
    paint.colors = {0xFF181818, 0xFF454545, 0xFF007ACC, 0xFFCC3333, 0xFF264F78, 0xFF303030};
    paint.placeholder = "Type 中🙂";
    paint.focused = true;
    const auto capture = [&] {
        std::vector<std::uint32_t> result(640 * 180);
        require(surface->readPixels(SkImageInfo::MakeN32Premul(640, 180),
                                    result.data(),
                                    640 * sizeof(std::uint32_t),
                                    0,
                                    0),
                "Cannot read text field pixels");
        return result;
    };
    const auto render = [&] {
        canvas->clear(untouched);
        const auto saved = canvas->getSaveCount();
        const auto caret = field.draw(canvas, labels, paint);
        require(canvas->getSaveCount() == saved, "Text field leaked a canvas clip/save");
        if (caret) {
            require(caret->x >= paint.bounds.x && caret->y >= paint.bounds.y &&
                        caret->right() <= paint.bounds.right() &&
                        caret->bottom() <= paint.bounds.bottom(),
                    "Text field IME candidate area escaped its bounds");
        }
        const auto result = capture();
        for (int y = 0; y < 180; ++y) {
            for (int x = 0; x < 640; ++x) {
                if (x + 0.5F < paint.bounds.x || x + 0.5F >= paint.bounds.right() ||
                    y + 0.5F < paint.bounds.y || y + 0.5F >= paint.bounds.bottom()) {
                    require(result[static_cast<std::size_t>(y * 640 + x)] == untouched,
                            "Text field painting escaped its control");
                }
            }
        }
        return result;
    };
    for (const auto dpi : {1.0F, 1.25F, 2.0F}) {
        paint.text_style.dpi_scale = paint.hint_style.dpi_scale = dpi;
        for (const std::string text :
             {"", "ASCII", "中🙂é👨‍👩‍👧‍👦", "אבג xyz"}) {
            paint.text = text;
            paint.selection = {{0}, {text.size()}};
            const auto original = render();
            const auto* paragraph = field.text().layout();
            require(paragraph != nullptr, "Text field did not prepare its native paragraph");
            equal(*paragraph, engine.layout(text, paint.text_style));
            require(render() == original && field.text().layout() == paragraph,
                    "Repeated text field paint changed stable pixels or paragraph identity");
        }
    }
    const std::string long_text(80, 'W');
    paint.text_style.dpi_scale = paint.hint_style.dpi_scale = 1;
    paint.text = long_text;
    paint.selection = {{long_text.size()}, {long_text.size()}};
    static_cast<void>(render());
    require(field.text().horizontal > 0, "Text field did not reveal a scrolled caret");
    const auto caret = field.draw(canvas, labels, paint);
    require(caret && caret->width > 0 && caret->right() <= paint.bounds.right() - 6,
            "End-of-input caret stroke was clipped instead of reserved");
    const auto [x, y] = field.to_text(paint.bounds, 1, caret->x, caret->y + caret->height / 2);
    require(field.text().layout()->hit_test(x, y).byte_offset == long_text.size(),
            "Text field pointer and painted caret used different transforms");
    paint.selection = {{0}, {0}};
    static_cast<void>(render());
    require(field.text().horizontal == 0, "Text field did not reveal the first caret");
    paint.composition = "中🙂é";
    const auto composition_pixels = render();
    const auto* composition = field.composition_layout();
    require(composition != nullptr, "Text field lost native IME composition");
    equal(*composition, engine.layout(std::string(paint.composition), paint.text_style));
    require(render() == composition_pixels && field.composition_layout() == composition,
            "Unchanged IME composition was reshaped or painted differently");
    const auto* accepted = field.text().layout();
    paint.composition = "\xFF";
    bool rejected{};
    const auto saved = canvas->getSaveCount();
    try {
        static_cast<void>(field.draw(canvas, labels, paint));
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected && canvas->getSaveCount() == saved && field.text().layout() == accepted,
            "IME failure damaged accepted input or leaked a canvas clip");
    paint.composition = "中🙂é";
    paint.focused = false;
    static_cast<void>(render());
    require(field.composition_layout() == nullptr && !field.composition_pending(),
            "Blur retained obsolete IME composition");
    paint.focused = true;
    paint.composition = {};
    for (const auto size : {0.0F, 1.0F, 2.0F, 8.0F, 12.0F}) {
        paint.bounds = {20, 20, size, size};
        static_cast<void>(render());
    }
    paint.bounds.width = -1;
    rejected = false;
    try {
        static_cast<void>(field.draw(canvas, labels, paint));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && canvas->getSaveCount() == saved,
            "Invalid text field bounds reached native painting");

    auto jobs = std::make_shared<LayoutJobs>();
    TextField background(engine, jobs);
    paint.bounds = {20, 20, 180, 32};
    paint.text = "Input";
    paint.selection = {{5}, {5}};
    const std::string long_composition(5600, 'a');
    paint.composition = long_composition;
    static_cast<void>(background.draw(canvas, labels, paint));
    require(background.composition_pending() && background.text().layout() != nullptr &&
                jobs->statistics().submitted == 1,
            "Cold long IME composition blocked or replaced accepted input");
    paint.composition = "短";
    static_cast<void>(background.draw(canvas, labels, paint));
    require(!background.composition_pending() && background.composition_layout()->text() == "短",
            "Changed IME composition did not cancel obsolete shaping");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (jobs->statistics().jobs != 0 && std::chrono::steady_clock::now() < deadline) {
        jobs->poll();
        background.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(jobs->statistics().jobs == 0 && background.composition_layout()->text() == "短",
            "Retired IME worker published into a newer control frame");
    paint.composition = long_composition;
    static_cast<void>(background.draw(canvas, labels, paint));
    require(background.composition_pending(), "Cold composition cache unexpectedly matched");
    const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (background.composition_pending() &&
           std::chrono::steady_clock::now() < completion_deadline) {
        jobs->poll();
        background.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(!background.composition_pending() && background.composition_layout() &&
                background.composition_layout()->text() == long_composition,
            "Successful background composition was not published");
    equal(*background.composition_layout(), engine.layout(long_composition, paint.text_style));
    const auto* composed = background.composition_layout();
    static_cast<void>(background.draw(canvas, labels, paint));
    require(background.composition_layout() == composed && jobs->statistics().jobs == 0,
            "Completed IME composition did not reuse its exact paragraph");
    paint.focused = false;
    static_cast<void>(background.draw(canvas, labels, paint));
    require(background.composition_layout() == nullptr,
            "Blur did not release the composition paragraph");
    std::cout
        << "Shared native text fields: Unicode pixels, exact reuse, bounded clips, "
           "caret reveal/hit transforms, IME caching/cancellation and failure preservation: PASS\n";
}

void text_labels(TextLayoutEngine& engine) {
    TextLayoutOptions style;
    style.font_size = 13;
    style.line_height = 1.15F;
    style.direction = TextDirection::automatic;
    style.font_families = {"Segoe UI", "Microsoft YaHei UI", "Segoe UI Emoji"};
    TextLabels labels(engine);
    const std::string source = "מסמך 中🙂é👨‍👩‍👧‍👦.cpp";
    const auto original = labels.prepare(source, style);
    equal(*original, engine.layout(source, style));
    require(labels.prepare(source, style) == original, "Chrome text cache did not reuse its key");
    auto changed = style;
    changed.dpi_scale = std::nextafter(style.dpi_scale, 2.0F);
    const auto tiny_dpi_change = labels.prepare(source, changed);
    require(tiny_dpi_change != original,
            "A rounded string cache key hid an exact chrome DPI change");
    equal(*tiny_dpi_change, engine.layout(source, changed));
    changed.color = 0xFF66AA11;
    const auto colored = labels.prepare(source, changed);
    equal(*colored, engine.layout(source, changed));
    require(pixels(*colored) != pixels(*original), "Chrome color change reused stale pixels");
    const auto normalized = labels.prepare("A\r\nB\tC\u2028中", style);
    require(normalized->text() == "A  B C 中" && normalized->line_count() == 1,
            "Chrome separators escaped the single-line display");
    equal(*normalized, engine.layout("A  B C 中", style));
    for (float width = 1; width < original->width(); width += 7) {
        const auto elided = labels.elide(source, style, width);
        if (!elided) {
            require(engine.layout("…", style).width() > width,
                    "A fitting chrome ellipsis was unnecessarily hidden");
            continue;
        }
        require(elided->width() <= width && elided->text().ends_with("…"),
                "Elided chrome text escaped its measured width");
        const auto prefix = elided->text().substr(0, elided->text().size() - 3);
        require(source.starts_with(prefix) &&
                    std::ranges::find(original->grapheme_boundaries(), prefix.size()) !=
                        original->grapheme_boundaries().end(),
                "Chrome elision split a combining or emoji sequence");
        equal(*elided, engine.layout(std::string(elided->text()), style));
    }
    require(!labels.elide(source, style, 0), "Zero-width chrome text produced a paragraph");
    std::string crossing(4090, 'a');
    crossing += "👨‍👩‍👧‍👦";
    const auto bounded = labels.prepare(crossing, style);
    require(bounded->text() == std::string(4090, 'a') + "…",
            "A byte-limited chrome prefix displayed half an emoji family");
    std::string combining = "e";
    for (int count = 0; count < 2500; ++count) {
        combining += "́";
    }
    require(labels.prepare(combining, style)->text() == "…",
            "A bounded chrome caption displayed an incomplete combining cluster");
    const auto prepared_before_failure = labels.statistics().shaped;
    bool invalid{};
    try {
        static_cast<void>(labels.prepare(std::string("bad\xFF"), style));
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    require(invalid && labels.statistics().shaped == prepared_before_failure,
            "Invalid chrome UTF-8 mutated the paragraph cache");
    TextLabels small(engine, {3, 2048});
    const auto retained = small.prepare(source, style);
    for (int count = 0; count < 60; ++count) {
        static_cast<void>(small.prepare("caption " + std::to_string(count), style));
        require(small.statistics().entries <= 3 && small.statistics().accounted_bytes <= 2048,
                "Chrome LRU exceeded its entry or byte bounds");
    }
    require(small.statistics().evicted > 0 && retained->text() == source,
            "Chrome eviction invalidated a retained paragraph");
    equal(*retained, engine.layout(source, style));
    std::cout << "Chrome text: exact DPI/color keys, native Unicode pixels, grapheme elision, "
                 "bounded display prefixes and retained LRU paragraphs: PASS\n";
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
    // Three short captions now include genuine retained raw glyph snapshots.
    // Keep entry-capacity and payload-capacity eviction as separate checks.
    DocumentViewport viewport(engine, {2, 4096});
    viewport.publish(rows({"one", "two", "three", std::string(2000, 'a')}));
    TextLayoutOptions style;
    (void)viewport.shape(0, style);
    const auto entry_bytes = viewport.statistics().accounted_bytes;
    const auto retained = viewport.layout_snapshot(0);
    (void)viewport.shape(1, style);
    (void)viewport.shape(0, style);
    (void)viewport.shape(2, style);
    const auto stats = viewport.statistics();
    require(stats.entries == 2 && stats.evicted == 1 && stats.accounted_bytes <= 4096,
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

template <typename Predicate> void await(LayoutJobs& jobs, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!predicate()) {
        const auto stats = jobs.statistics();
        require(std::chrono::steady_clock::now() < deadline,
                "Layout worker did not drain: jobs=" + std::to_string(stats.jobs) +
                    " running=" + std::to_string(stats.running) +
                    " submitted=" + std::to_string(stats.submitted) +
                    " completed=" + std::to_string(stats.completed));
        jobs.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

LayoutJobResult result(LayoutJobs& jobs, LayoutJobs::Ticket ticket) {
    std::optional<LayoutJobResult> completed;
    await(jobs, [&] {
        completed = jobs.take(ticket);
        return completed.has_value();
    });
    return std::move(*completed);
}

void background_layout(TextLayoutEngine& engine) {
    LayoutJobs jobs;
    const std::vector<std::string> text{"中🙂é\tvalue", "abc אבג def", "é 中文 中🙂"};
    for (unsigned int index = 0; index < 12; ++index) {
        TextLayoutOptions style;
        style.font_size = 12.0F + index;
        style.dpi_scale = index % 2 ? 1.25F : 1.0F;
        style.line_height = index % 3 ? 1.4F : 1.8F;
        style.tab_size = 2 + index % 7;
        style.color = index % 2 ? 0xFF70A0C0 : 0xFFB0D040;
        style.color_spans = {{0, 2, 0xFFC04070U}};
        style.font_families = {"Consolas", "Microsoft YaHei", "Segoe UI Emoji"};
        style.locale = index % 2 ? "en" : "zh";
        style.direction = index % 2 ? TextDirection::automatic : TextDirection::left_to_right;
        style.wrap_width = index % 3 == 0 && index % text.size() != 0 ? 80.0F : 0.0F;
        const auto ticket = jobs.submit(text[index % text.size()], style);
        require(ticket.has_value(), "Empty queue rejected a paragraph");
        const auto completed = result(jobs, *ticket);
        if (completed.error) {
            std::rethrow_exception(completed.error);
        }
        require(completed.paragraph != nullptr, "Worker returned no paragraph");
        equal(*completed.paragraph, engine.layout(text[index % text.size()], style));
    }
    const auto invalid = jobs.submit(std::string("\xFF", 1), {});
    require(result(jobs, *invalid).error != nullptr, "Worker Unicode failure was lost");
    LayoutJobLimits limits;
    limits.jobs = 1;
    LayoutJobs bounded(limits);
    const auto first = bounded.submit("中🙂", {});
    require(!bounded.submit("second", {}), "Queue exceeded its job bound");
    bounded.cancel(*first);
    await(bounded, [&] {
        return bounded.idle();
    });
    for (int index = 0; index < 1000; ++index) {
        const auto ticket = jobs.submit("cancelled", {});
        if (ticket) {
            jobs.cancel(*ticket);
        }
        jobs.poll();
        require(jobs.statistics().jobs <= 16, "Cancellation accumulated unbounded jobs");
    }
    jobs.close();
    await(jobs, [&] {
        return jobs.idle();
    });
    require(jobs.statistics().bytes == 0, "Drained queue retained accounted bytes");
    bool closed = false;
    try {
        (void)jobs.submit("closed", {});
    } catch (const std::logic_error&) {
        closed = true;
    }
    require(closed, "Shutdown admitted more work");
    limits.timeout = std::chrono::milliseconds(1);
    limits.jobs = 2;
    LayoutJobs expiring(limits);
    const auto timed = expiring.submit("deadline", {});
    const auto queued_timeout = expiring.submit("queued deadline", {});
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    expiring.poll();
    const auto expired = result(expiring, *timed);
    require(expired.error && !expired.paragraph && expiring.statistics().expired == 2,
            "Expired job disappeared without a terminal error");
    require(result(expiring, *queued_timeout).error != nullptr,
            "Queued deadline disappeared without a terminal error");
    limits.timeout = std::chrono::seconds(30);
    limits.bytes = 4096;
    limits.text_bytes = 512;
    LayoutJobs byte_limited(limits);
    bool oversized = false;
    try {
        (void)byte_limited.submit(std::string(513, 'x'), {});
    } catch (const std::length_error&) {
        oversized = true;
    }
    require(oversized && byte_limited.idle() && byte_limited.statistics().bytes == 0,
            "Oversized admission mutated queue accounting");
    const auto retiring = byte_limited.submit("retire completed 中🙂", {});
    await(byte_limited, [&] {
        return byte_limited.statistics().completed == 1;
    });
    byte_limited.cancel(*retiring);
    await(byte_limited, [&] {
        return byte_limited.idle();
    });
    require(!byte_limited.take(*retiring), "Cancellation published a completed paragraph");
    std::shared_ptr<const TextLayout> retained;
    {
        LayoutJobs temporary;
        retained = result(temporary, *temporary.submit("retained 中🙂", {})).paragraph;
    }
    equal(*retained, engine.layout("retained 中🙂", {}));
    std::cout << "Actual Windows worker shaping: pixels, Unicode, styles, bounds, cancellation, "
                 "deadline and font lifetime: PASS\n";
}

void background_viewport(TextLayoutEngine& engine) {
    {
        LayoutJobs workers;
        const std::string original(12000, 'a');
        const auto first_ticket = workers.submit(original, {});
        require(first_ticket.has_value(), "Glyph seed worker admission failed");
        const auto first = result(workers, *first_ticket);
        require(first.paragraph && !first.error, "Glyph seed worker failed");
        const auto before = GlyphCache::glyph_digest(first.paragraph->glyph_snapshot());
        auto changed = original;
        changed.replace(6000, 1, "x");
        const auto second_ticket = workers.submit(
            changed, {}, first.paragraph->source_snapshot(), first.paragraph->glyph_snapshot());
        require(second_ticket.has_value(), "Glyph edit worker admission failed");
        const auto second = result(workers, *second_ticket);
        require(second.paragraph && !second.error, "Glyph edit worker failed");
        const auto reference = engine.layout(changed);
        equal(*second.paragraph, reference);
        require(GlyphCache::glyph_digest(second.paragraph->glyph_snapshot()) ==
                        GlyphCache::glyph_digest(reference.glyph_snapshot()) &&
                    GlyphCache::glyph_digest(first.paragraph->glyph_snapshot()) == before,
                "Worker splicing changed native raw glyphs or the retained snapshot");
        const auto stats = second.paragraph->glyph_snapshot().statistics();
        require(stats.spliced_runs > 0 && stats.equivalent_fonts > 0 &&
                    stats.shared_glyphs > 10000 && stats.shaped_bytes < original.size(),
                "Actual background owners did not splice persistent native glyphs");
        std::cout << "Worker glyph splice: input=" << changed.size()
                  << ", shaped=" << stats.shaped_bytes << ", shared=" << stats.shared_glyphs
                  << ", equivalent fonts=" << stats.equivalent_fonts << ": PASS\n";
    }
    auto jobs = std::make_shared<LayoutJobs>();
    DocumentViewport viewport(engine, {}, jobs);
    std::string text;
    for (int index = 0; index < 500; ++index) {
        text += "中🙂é abc אבג ";
    }
    viewport.publish(rows({text}));
    TextLayoutOptions style;
    require(viewport.prepare(0, style) == nullptr && viewport.pending(0),
            "Cold long paragraph blocked instead of queuing");
    const auto submitted = jobs->statistics().submitted;
    require(viewport.prepare(0, style) == nullptr && jobs->statistics().submitted == submitted,
            "Repeated preparation duplicated worker jobs");
    await(*jobs, [&] {
        viewport.poll_layouts();
        return !viewport.layouts_pending();
    });
    const auto retained = viewport.layout_snapshot(0);
    const auto fresh = engine.layout(text, style);
    require(retained && pixels(*retained) == pixels(fresh) && retained->width() == fresh.width(),
            "Long worker paragraph differs from complete native shaping");
    auto edited = text;
    const auto change = edited.find("中", edited.size() / 2);
    require(change != edited.npos, "Cannot locate Unicode coordinate edit fixture");
    edited.replace(change, std::string("中").size(), "文🙂");
    viewport.publish(rows({edited}));
    require(!viewport.prepare(0, style), "Changed long row did not use a worker");
    const auto retained_glyphs = retained->glyph_snapshot().statistics();
    // The worker receives only immutable text/coordinate/glyph data. The old native
    // paragraph remains owned by this thread and can paint throughout the job.
    await(*jobs, [&] {
        require(pixels(*retained) == pixels(fresh), "Worker coordinate reuse mutated old pixels");
        viewport.poll_layouts();
        return !viewport.layouts_pending();
    });
    const auto replacement = viewport.layout_snapshot(0);
    require(replacement != nullptr, "Changed long row did not publish native geometry");
    const auto replacement_glyphs = replacement->glyph_snapshot().statistics();
    require(replacement_glyphs.glyphs > 0 && replacement_glyphs.runs > 0 &&
                replacement_glyphs.bytes <= 4 * 1024 * 1024 &&
                retained->glyph_snapshot().statistics().bytes == retained_glyphs.bytes &&
                retained->glyph_snapshot().statistics().shaped_bytes ==
                    retained_glyphs.shaped_bytes,
            "Worker glyph snapshots were missing, oversized or mutated after transfer");
    equal(*replacement, engine.layout(edited, style));
    const auto coordinates = replacement->source_snapshot().statistics();
    require(coordinates.shared_prefix_bytes > 2048 && coordinates.shared_suffix_bytes > 2048,
            "Local Unicode edit rebuilt the measured coordinate tree");
    require(retained->source_snapshot().text() == text,
            "Worker coordinate edit changed the retained source snapshot");
    viewport.publish(rows({text}));
    viewport.invalidate_layouts();
    require(viewport.prepare(0, style) == retained.get(), "Long paragraph cache hit queued work");
    style.font_size += 2;
    require(!viewport.prepare(0, style), "New long paragraph style reused old geometry");
    viewport.publish(rows({"new snapshot"}));
    require(!viewport.layouts_pending() && !viewport.layout(0),
            "Publication retained stale geometry");
    require(viewport.prepare(0, {}) && viewport.layout(0)->text() == "new snapshot",
            "Cancelled paragraph replaced new text");
    await(*jobs, [&] {
        return jobs->idle();
    });
    viewport.poll_layouts();
    require(viewport.layout(0)->text() == "new snapshot", "Late result overwrote a new snapshot");
    require(pixels(*retained) == pixels(fresh), "Cancellation mutated a retained paragraph");
    {
        DocumentViewport temporary(engine, {}, jobs);
        temporary.publish(rows({text}));
        require(!temporary.prepare(0, {}), "Cold destruction case did not queue work");
    }
    await(*jobs, [&] {
        return jobs->idle();
    });
    viewport.publish(rows({text}));
    TextLayoutOptions invalid;
    invalid.tab_size = 0;
    require(!viewport.prepare(0, invalid), "Cold invalid style blocked the UI");
    await(*jobs, [&] {
        viewport.poll_layouts();
        return !viewport.layouts_pending();
    });
    bool failed = false;
    try {
        (void)viewport.prepare(0, invalid);
    } catch (const std::invalid_argument&) {
        failed = true;
    }
    require(failed && !viewport.layout(0), "Worker failure published invalid geometry");
    const auto after_failure = jobs->statistics().submitted;
    try {
        (void)viewport.prepare(0, invalid);
    } catch (const std::invalid_argument&) {
    }
    require(jobs->statistics().submitted == after_failure,
            "Failed style was resubmitted every frame");
    viewport.invalidate_layouts();
    std::cout << "Async viewport: long-row parity, coalescing, cache hits, stale publication, "
                 "cancellation and destruction: PASS\n";
}
} // namespace

void text_surfaces(astra::ui::TextLayoutEngine& engine) {
    using namespace astra::ui;
    auto jobs = std::make_shared<LayoutJobs>();
    TextSurface surface(engine, jobs);
    surface.reset("中🙂\r\né\tvalue\rfinal\n");
    require(surface.lines().size() == 4 && surface.lines().offset(1) == 9,
            "Text surface lost mixed-newline byte extents");
    const auto snapshot = surface.lines();
    surface.show(0, 3);
    TextLayoutOptions style;
    const auto* first = surface.prepare(1, style);
    equal(*first, engine.layout(surface.lines().at(1), style));
    require(surface.prepare(1, style) == first, "Repeated surface paint reshaped a paragraph");
    surface.show(1, 3);
    require(surface.prepare(1, style) == first, "Overlapping text page lost exact cache reuse");
    bool invalid = false;
    try {
        surface.show(0, 257);
    } catch (const std::out_of_range&) {
        invalid = true;
    }
    require(invalid && surface.prepare(1, style) == first, "Invalid page mutated visible text");
    invalid = false;
    try {
        surface.reset(std::string("\xFF", 1));
    } catch (const std::invalid_argument&) {
        invalid = true;
    }
    require(invalid && surface.lines().size() == 4 && surface.prepare(1, style) == first,
            "Malformed UTF-8 reset discarded existing content");
    const auto long_text = std::string(70000, 'a') + "\t中文🙂";
    surface.reset(long_text + "\nnew page\nend");
    surface.show(0, 2);
    require(!surface.prepare(0, style) && surface.pending(),
            "Long surface text blocked instead of queuing");
    const auto submitted = jobs->statistics().submitted;
    for (int frame = 0; frame < 20; ++frame) {
        require(!surface.prepare(0, style), "Pending surface published incomplete geometry");
    }
    require(jobs->statistics().submitted == submitted,
            "Repeated surface frames duplicated pending jobs");
    style.color = 0xFF70C040;
    style.wrap_width = 450;
    require(!surface.prepare(0, style), "Changed style reused an obsolete paragraph");
    await(*jobs, [&] {
        surface.poll();
        return !surface.pending();
    });
    const auto* long_paragraph = surface.prepare(0, style);
    const auto fresh = engine.layout(long_text, style);
    require(long_paragraph && pixels(*long_paragraph) == pixels(fresh) &&
                long_paragraph->width() == fresh.width() &&
                long_paragraph->height() == fresh.height(),
            "Wrapped long text surface differs from fresh native shaping");
    style.dpi_scale = 1.25F;
    require(!surface.prepare(0, style), "DPI change kept obsolete geometry");
    surface.show(1, 3);
    require(!surface.pending() && surface.prepare(1, {}) != nullptr,
            "New page retained old pending geometry");
    await(*jobs, [&] {
        return jobs->idle();
    });
    surface.poll();
    require(surface.prepare(1, {})->text() == "new page", "Late old-page result replaced new text");
    require(snapshot.size() == 4 && snapshot.at(0) == "中🙂",
            "Reset mutated a retained line index");
    std::string many_rows;
    for (int row = 0; row < 1000; ++row) {
        many_rows += "row " + std::to_string(row) + "\n";
    }
    surface.reset(many_rows);
    constexpr unsigned int seed = 0xA57A5F;
    std::mt19937 random(seed);
    for (int iteration = 0; iteration < 60; ++iteration) {
        const auto begin = random() % 960;
        surface.show(begin, begin + 32);
        for (std::size_t row = begin; row < begin + 32; ++row) {
            require(surface.prepare(row, {})->text() == "row " + std::to_string(row),
                    "Paged surface disagrees with flat row identities");
        }
        require(surface.statistics().entries <= 256 &&
                    surface.statistics().accounted_bytes <= 8 * 1024 * 1024,
                "Surface scrolling exceeded cache bounds");
    }
    std::cout
        << "Text surfaces: mixed extents, atomic reset, exact style/DPI pixels, cancellation, "
           "scroll-page differential seed="
        << seed << " and bounded reuse: PASS\n";
}

void editable_geometry(astra::ui::TextLayoutEngine& engine) {
    using namespace astra::ui;
    TextInputLayout input(engine);
    TextLayoutOptions style;
    style.direction = TextDirection::automatic;
    const std::string text = "A🙂é אבג\t中文";
    equal(input.prepare(text, style), engine.layout(text, style));
    require(input.matches(text) && !input.matches("different"),
            "Input geometry accepted stale text");
    const auto before = pixels(*input.layout());
    style.color = 0xFF40C070;
    equal(input.prepare(text, style), engine.layout(text, style));
    require(pixels(*input.layout()) != before, "Input theme color retained an old cache entry");
    style.dpi_scale = 1.25F;
    style.font_size = 19;
    style.font_families = {"Consolas", "Segoe UI Emoji", "Microsoft YaHei UI"};
    equal(input.prepare(text, style), engine.layout(text, style));
    const auto retained = pixels(*input.layout());
    auto invalid = style;
    invalid.dpi_scale = 0;
    bool rejected = false;
    try {
        static_cast<void>(input.prepare("new value", invalid));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && input.matches(text) && pixels(*input.layout()) == retained,
            "Failed input preparation replaced valid geometry");
    for (const auto& value :
         {std::string("\xFF", 1), std::string("line\nbreak"), std::string(16385, 'a')}) {
        rejected = false;
        try {
            static_cast<void>(input.prepare(value, style));
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && input.matches(text), "Invalid input content discarded valid layout");
    }
    std::cout << "Editable input geometry: real Unicode/style pixels, exact cache keys, "
                 "stale text and atomic failure preservation: PASS\n";
}

void editable_background(TextLayoutEngine& engine) {
    auto jobs = std::make_shared<LayoutJobs>();
    TextInputLayout input(engine, jobs);
    TextLayoutOptions style;
    style.direction = TextDirection::automatic;
    std::string text;
    for (int index = 0; index < 900; ++index) {
        text += "中🙂éאב\t";
    }
    require(input.prepare_async(text, style) == nullptr && input.pending(),
            "Cold editable input shaped synchronously");
    for (int frame = 0; frame < 20; ++frame) {
        require(input.prepare_async(text, style) == nullptr,
                "Pending editable input exposed mismatched geometry");
    }
    require(jobs->statistics().submitted == 1, "Repeated editable frames duplicated work");
    std::cout << "Editable background: awaiting original paragraph\n" << std::flush;
    await(*jobs, [&] {
        input.poll();
        return !input.pending();
    });
    const auto* original = input.prepare_async(text, style);
    require(original != nullptr, "Completed editable input has no geometry");
    equal(*original, engine.layout(text, style));
    require(input.prepare_async(text, style) == original && jobs->statistics().submitted == 1,
            "Exact editable cache hit submitted work");
    auto changed = style;
    changed.dpi_scale = 1.25F;
    changed.font_size = 19;
    changed.color = 0xFF32C481;
    require(!input.prepare_async(text, changed), "Changed editable style reused old geometry");
    const auto cancelled = jobs->statistics().cancelled;
    const auto* short_input = input.prepare_async("short 中🙂", style);
    require(short_input && jobs->statistics().cancelled > cancelled,
            "New short input did not cancel obsolete shaping");
    std::cout << "Editable background: awaiting cancellation\n" << std::flush;
    await(*jobs, [&] {
        return jobs->idle();
    });
    input.poll();
    require(input.matches("short 中🙂") && input.layout() == short_input,
            "Late editable result replaced new content");
    auto invalid = style;
    invalid.dpi_scale = 0;
    require(!input.prepare_async(text, invalid), "Invalid long style bypassed the worker");
    std::cout << "Editable background: awaiting invalid style\n" << std::flush;
    await(*jobs, [&] {
        input.poll();
        return !input.pending();
    });
    const auto submitted = jobs->statistics().submitted;
    bool rejected{};
    try {
        static_cast<void>(input.prepare_async(text, invalid));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected && input.matches("short 中🙂") && jobs->statistics().submitted == submitted,
            "Editable worker failure discarded valid geometry or resubmitted");
    require(!input.prepare_async(text, changed), "Corrected editable style did not retry");
    std::cout << "Editable background: awaiting corrected style\n" << std::flush;
    await(*jobs, [&] {
        input.poll();
        return !input.pending();
    });
    equal(*input.prepare_async(text, changed), engine.layout(text, changed));
    {
        TextInputLayout retired(engine, jobs);
        require(!retired.prepare_async(text + "!", style), "Cold retired input was synchronous");
    }
    std::cout << "Editable background: awaiting destruction\n" << std::flush;
    await(*jobs, [&] {
        return jobs->idle();
    });
    require(jobs->statistics().bytes == 0, "Destroyed input retained worker accounting");
    LayoutJobLimits limits;
    limits.jobs = 1;
    auto full = std::make_shared<LayoutJobs>(limits);
    const auto blocker = full->submit(text, style);
    TextInputLayout waiting(engine, full);
    require(!waiting.prepare_async(text + "?", style) && waiting.pending() &&
                full->statistics().submitted == 1,
            "Editable backpressure exceeded the queue limit");
    static_cast<void>(result(*full, *blocker));
    std::cout << "Editable background: awaiting backpressure retry\n" << std::flush;
    await(*full, [&] {
        waiting.poll();
        return !waiting.pending();
    });
    equal(*waiting.prepare_async(text + "?", style), engine.layout(text + "?", style));
    std::cout << "Editable background layout: exact Unicode/style pixels, coalescing, "
                 "late cancellation, terminal errors, destruction and backpressure: PASS\n";
}

void input_event_ownership() {
    InputEvents queue;
    std::string source = "中🙂";
    SDL_Event text{};
    text.type = SDL_EVENT_TEXT_INPUT;
    text.text.text = source.c_str();
    queue.push(text, SDL_KMOD_SHIFT);
    source = "different";
    auto stored = queue.take();
    require(std::string(stored.get().text.text) == "中🙂" && stored.modifiers == SDL_KMOD_SHIFT &&
                queue.bytes() == 0,
            "Deferred text lost ownership or captured modifiers");
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.x = 4;
    queue.push(motion, SDL_KMOD_NONE);
    motion.motion.x = 9;
    queue.push(motion, SDL_KMOD_SHIFT);
    require(queue.size() == 1 && queue.take().get().motion.x == 9,
            "Adjacent motion events did not coalesce");
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    key.key.key = SDLK_LEFT;
    for (std::size_t index = 0; index < InputEvents::max_events; ++index) {
        queue.push(key, SDL_KMOD_NONE);
    }
    bool rejected{};
    try {
        queue.push(key, SDL_KMOD_NONE);
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected && queue.size() == InputEvents::max_events,
            "Event capacity failure discarded accepted input");
    queue.clear();
    source.assign(InputEvents::max_text_bytes, 'a');
    text.text.text = source.c_str();
    for (int index = 0; index < 4; ++index) {
        queue.push(text, SDL_KMOD_NONE);
    }
    rejected = false;
    try {
        queue.push(text, SDL_KMOD_NONE);
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected && queue.size() == 4 && queue.bytes() == InputEvents::max_bytes,
            "Payload capacity failure changed accepted input");
    queue.clear();
    source.push_back('!');
    text.text.text = source.c_str();
    rejected = false;
    try {
        queue.push(text, SDL_KMOD_NONE);
    } catch (const std::length_error&) {
        rejected = true;
    }
    require(rejected && queue.size() == 0, "Oversized payload entered the input queue");
    std::cout << "Deferred SDL input: payload ownership, modifiers, motion coalescing, "
                 "event/byte bounds and atomic admission failure: PASS\n";
}

void tab_bars(TextLayoutEngine& engine) {
    WorkbenchStyle theme;
    theme.font_size = 13;
    theme.font_families = {"Segoe UI"};
    theme.monospace_families = {"Cascadia Code"};
    theme.colors.tabs = 0xFF21252C;
    theme.colors.active_tab = 0xFF101722;
    theme.colors.accent = 0xFF3399FF;
    theme.colors.foreground = 0xFFE2E5EC;
    theme.colors.muted = 0xFF9AABBC;
    theme.colors.border = 0xFF41454C;
    TextLabels labels(engine);
    TabBar bar(labels);
    TabIndex tabs;
    for (unsigned index = 0; index < 64; ++index) {
        tabs.append(std::to_string(index),
                    "source-" + std::to_string(index) + "-中🙂e\xCC\x81.cpp");
    }
    const auto prepare = [&](TabBar& target,
                             TabIndex& model,
                             Box bounds,
                             float dpi,
                             std::size_t active,
                             const WorkbenchStyle& style) {
        target.reveal();
        for (unsigned frame = 0; frame < 256; ++frame) {
            target.measure(bounds, dpi, model, active, style, true);
            if (model.pending() == 0) {
                return;
            }
        }
        throw std::runtime_error("Tab bar did not finish bounded caption preparation");
    };
    const auto render = [&](TabBar& target,
                            const TabIndex& model,
                            std::size_t active,
                            const WorkbenchStyle& style) {
        const auto info = SkImageInfo::MakeN32Premul(640, 180);
        auto surface = SkSurfaces::Raster(info);
        require(surface != nullptr, "Cannot create tab bar pixel surface");
        auto* canvas = surface->getCanvas();
        canvas->clear(0xFF101010);
        std::size_t decorations{};
        const auto saves = canvas->getSaveCount();
        target.paint(canvas, model, active, style, [&](std::size_t index) {
            ++decorations;
            require(index < model.size(), "Tab decoration referenced an absent identity");
            return TabBar::Decoration{Icon::code, 0xFF42BEEF, index != active};
        });
        require(decorations == target.geometry().visible.size() && canvas->getSaveCount() == saves,
                "Tab painter visited offscreen metadata or changed canvas ownership");
        std::vector<std::uint32_t> result(640 * 180);
        require(surface->readPixels(info, result.data(), 640 * sizeof(std::uint32_t), 0, 0),
                "Cannot read tab bar pixels");
        return result;
    };
    for (const auto dpi : {1.0F, 1.5F, 2.0F}) {
        const Box bounds{20, 12, 580, 38 * dpi};
        constexpr std::size_t active = 31;
        prepare(bar, tabs, bounds, dpi, active, theme);
        const auto before = render(bar, tabs, active, theme);
        const auto shaped = labels.statistics().shaped;
        const auto measured = tabs.measurements();
        for (unsigned frame = 0; frame < 8; ++frame) {
            bar.measure(bounds, dpi, tabs, active, theme, true);
            require(render(bar, tabs, active, theme) == before, "Warm tab pixels changed");
        }
        require(labels.statistics().shaped == shaped && tabs.measurements() == measured &&
                    bar.max_measured_pass() <= 8,
                "Warm tab paints prepared captions or exceeded the admission bound");
        for (std::size_t y = 0; y < 180; ++y) {
            for (std::size_t x = 0; x < 640; ++x) {
                if (!bounds.contains(static_cast<float>(x) + 0.5F, static_cast<float>(y) + 0.5F)) {
                    require(before[y * 640 + x] == 0xFF101010, "Tab pixels escaped their bounds");
                }
            }
        }
        for (const auto& box : bar.geometry().visible) {
            const auto caption = bar.hit(box.bounds.x + 10 * dpi, box.bounds.y + bounds.height / 2);
            const auto close =
                bar.hit(box.close.x + box.close.width / 2, box.close.y + bounds.height / 2);
            require(caption && caption->target == TabBar::Target::caption &&
                        caption->index == box.index && close &&
                        close->target == TabBar::Target::close && close->index == box.index,
                    "Drawn caption/close targets did not resolve the same identity");
        }
        const auto previous = bar.geometry().previous;
        const auto next = bar.geometry().next;
        const auto previous_hit = bar.hit(previous.x + previous.width / 2, previous.y + 1);
        const auto next_hit = bar.hit(next.x + next.width / 2, next.y + 1);
        require(previous_hit && previous_hit->target == TabBar::Target::previous && next_hit &&
                    next_hit->target == TabBar::Target::next &&
                    !bar.hit(bounds.right(), bounds.y) && !bar.hit(bounds.x, bounds.bottom()) &&
                    !bar.hit(std::numeric_limits<float>::quiet_NaN(), bounds.y),
                "Tab overflow/boundary hit testing was ambiguous");
        const auto generation = tabs.generation();
        auto recolored = theme;
        recolored.colors.foreground = 0xFFFFB040;
        bar.measure(bounds, dpi, tabs, active, recolored, false);
        require(tabs.measurements() == measured && tabs.generation() == generation &&
                    render(bar, tabs, active, recolored) != before,
                "Palette change remeasured captions or retained obsolete foreground pixels");
        TextLabels fresh_labels(engine);
        TabBar fresh(fresh_labels);
        auto fresh_tabs = tabs;
        fresh.scroll(static_cast<int>(bar.geometry().first), fresh_tabs.size());
        prepare(fresh, fresh_tabs, bounds, dpi, active, recolored);
        require(render(fresh, fresh_tabs, active, recolored) ==
                    render(bar, tabs, active, recolored),
                "Retained tab style pixels disagreed with a cold independent presentation");
        bool rejected{};
        try {
            auto invalid = bounds;
            invalid.width = std::numeric_limits<float>::quiet_NaN();
            bar.measure(invalid, dpi, tabs, active, theme, true);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && tabs.generation() == generation &&
                    render(bar, tabs, active, recolored) ==
                        render(fresh, fresh_tabs, active, recolored),
                "Invalid tab bounds changed the retained model or drawing");
        bar.scroll(std::numeric_limits<int>::max(), tabs.size());
        bar.measure(bounds, dpi, tabs, active, recolored, false);
        require(bar.geometry().first == tabs.size() - 1 && tabs.measurements() == measured,
                "Pointer overflow navigation shaped captions or exceeded the sequence");
        bar.scroll(std::numeric_limits<int>::min(), tabs.size());
        bar.measure(bounds, dpi, tabs, active, recolored, false);
        require(bar.geometry().first == 0, "Tab previous navigation underflowed");

        const auto info = SkImageInfo::MakeN32Premul(640, 180);
        auto surface = SkSurfaces::Raster(info);
        require(surface != nullptr, "Cannot create tab exception surface");
        auto* canvas = surface->getCanvas();
        canvas->translate(3, 4);
        const auto saves = canvas->getSaveCount();
        const auto matrix = canvas->getTotalMatrix();
        const auto clip = canvas->getDeviceClipBounds();
        rejected = false;
        try {
            bar.paint(canvas, tabs, active, recolored, [](std::size_t) -> TabBar::Decoration {
                throw std::runtime_error("Rejected tab metadata fixture");
            });
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(rejected && canvas->getSaveCount() == saves && canvas->getTotalMatrix() == matrix &&
                    canvas->getDeviceClipBounds() == clip,
                "Failed tab drawing leaked canvas clipping or transforms");
    }
    std::cout
        << "Native tab bar: Unicode/DPI pixels, bounded preparation, warm reuse, palette "
           "invalidation, caption/close/overflow identity and canvas failure ownership: PASS\n";
}

int main(int argc, char* argv[]) {
    try {
        TextLayoutEngine engine;
        if (argc == 2 && std::string_view(argv[1]) == "--tabs") {
            tab_bars(engine);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--fields") {
            text_fields(engine);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--labels") {
            text_labels(engine);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--editable") {
            editable_geometry(engine);
            editable_background(engine);
            input_event_ownership();
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--coordinates") {
            background_viewport(engine);
            return 0;
        }
        reuse(engine);
        limits(engine);
        option_keys(engine);
        differential(engine);
        background_layout(engine);
        background_viewport(engine);
        text_surfaces(engine);
        editable_geometry(engine);
        editable_background(engine);
        input_event_ownership();
        text_labels(engine);
        text_fields(engine);
        tab_bars(engine);
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
