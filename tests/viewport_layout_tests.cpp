#include <astra/ui/document_viewport.hpp>
#include <astra/ui/text_surface.hpp>
#include <astra/ui/text_input_layout.hpp>
#include <astra/ui/input_events.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <iostream>
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

int main(int argc, char* argv[]) {
    try {
        TextLayoutEngine engine;
        if (argc == 2 && std::string_view(argv[1]) == "--editable") {
            editable_geometry(engine);
            editable_background(engine);
            input_event_ownership();
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
