#include <astra/ui/document_viewport.hpp>
#include <astra/ui/text_surface.hpp>

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
        require(std::chrono::steady_clock::now() < deadline, "Layout worker did not drain");
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

int main() {
    try {
        TextLayoutEngine engine;
        reuse(engine);
        limits(engine);
        option_keys(engine);
        differential(engine);
        background_layout(engine);
        background_viewport(engine);
        text_surfaces(engine);
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
