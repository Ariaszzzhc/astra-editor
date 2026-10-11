#include "astra/ui/renderer.hpp"
#include "astra/ui/text_layout.hpp"
#include "../../src/ui/glyph_cache.hpp"
#include "../../src/ui/glyph_sequence.hpp"
#include "../../src/ui/font_signatures.hpp"
#include "astra/core/utf8.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/core/SkTypeface.h"
#include "modules/skparagraph/include/FontCollection.h"
#include "modules/skparagraph/include/Paragraph.h"
#include "modules/skparagraph/include/ParagraphBuilder.h"
#include "modules/skunicode/include/SkUnicode_icu.h"

#if defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#elif defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#else
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

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

std::vector<std::uint32_t> glyph_pixels(const TextLayout& line) {
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(1024, 256));
    require(surface != nullptr, "Glyph snapshot raster surface was unavailable");
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    line.draw(surface->getCanvas(), 4, 4);
    std::vector<std::uint32_t> result(1024 * 256);
    require(surface->readPixels(SkImageInfo::MakeN32Premul(1024, 256),
                                result.data(),
                                1024 * sizeof(std::uint32_t),
                                0,
                                0),
            "Glyph snapshot pixel read failed");
    return result;
}

void glyph_sequence_snapshots() {
    using Sequence = astra::ui::GlyphSequence;
    Sequence::Glyphs raw;
    for (std::size_t i = 0; i < 4097; ++i) {
        raw.push_back({static_cast<SkGlyphID>(i % 60000),
                       static_cast<std::uint32_t>(i + 7),
                       {0, 0},
                       {1, 0},
                       true,
                       false,
                       i % 3 == 0});
    }
    const auto original = Sequence::capture(raw, 7, 4104);
    require(original.validate() && original.summary().chunks == 17,
            "Glyph chunks exceeded their fixed bound or tree invariants");
    const auto check = [&](const Sequence& sequence, const Sequence::Glyphs& expected) {
        require(sequence.validate(), "Glyph splice broke persistent tree invariants");
        const auto actual = sequence.flatten(7);
        require(actual.size() == expected.size(), "Glyph sequence count differs");
        for (std::size_t i = 0; i < actual.size(); ++i) {
            require(actual[i].cluster == expected[i].cluster && actual[i].id == expected[i].id &&
                        actual[i].advance == expected[i].advance &&
                        actual[i].unsafeToConcat == expected[i].unsafeToConcat,
                    "Glyph relocation rewrote an immutable raw value");
        }
    };
    std::mt19937 random(0xA57A91U);
    for (std::size_t round = 0; round < 500; ++round) {
        const auto first = std::size_t{300} + random() % 1800;
        const auto last = first + random() % 500;
        auto [prefix, rest] = original.split(first, first);
        auto [removed, suffix] = rest.split(last - first, last - first);
        const Sequence::Glyph replacement{
            60000, static_cast<std::uint32_t>(first + 7), {0, 0}, {3, 0}, true, false, false};
        auto middle = Sequence::capture(std::span(&replacement, 1), first + 7, first + 10);
        const auto edited = prefix.concat(middle).concat(suffix);
        Sequence::Glyphs expected(raw.begin(), raw.begin() + first);
        expected.push_back(replacement);
        for (std::size_t i = last; i < raw.size(); ++i) {
            auto glyph = raw[i];
            glyph.cluster = static_cast<std::uint32_t>(i - last + first + 10);
            expected.push_back(glyph);
        }
        check(edited, expected);
        check(original, raw);
        require(edited.shared_glyphs(original) > 2000,
                "Glyph edit rebuilt unchanged persistent interiors");
    }
    // A cluster can span several bounded blocks; flags must combine across
    // seams, and a zero-byte block still contains real glyphs.
    Sequence::Glyphs huge_cluster(600, {1, 10, {0, 0}, {0, 0}, true, false, false});
    huge_cluster[300].unsafeToConcat = true;
    huge_cluster.push_back({2, 12, {0, 0}, {1, 0}, true, false, false});
    const auto grouped = Sequence::capture(huge_cluster, 10, 15);
    const auto cuts = grouped.boundaries(10);
    require(grouped.validate() && cuts.size() == 3 && cuts[0].unsafe && cuts[1].byte == 12 &&
                cuts[1].glyph == 600,
            "Cluster groups or flags were lost across bounded blocks");
    auto [cluster, tail] = grouped.split(600, 2);
    const auto joined = cluster.concat(tail).flatten(10);
    require(joined.size() == huge_cluster.size() && joined[300].unsafeToConcat &&
                joined.back().cluster == 12,
            "Large cluster split damaged raw glyphs");
    bool rejected{};
    try {
        (void)grouped.split(300, 1);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "A split inside a single glyph cluster was accepted");
    const auto invisible = Sequence::capture({}, 10, 17);
    auto [a, b] = invisible.split(0, 3);
    require(a.summary().bytes == 3 && b.summary().bytes == 4 && a.concat(b).summary().bytes == 7 &&
                a.concat(b).flatten(0).empty(),
            "Glyph-free source extents were discarded");
    const auto small = original.split(1, 1).first;
    require(small.summary().payload >= Sequence::chunk_limit * sizeof(Sequence::Glyph),
            "A sliced glyph block was charged only for visible glyphs");
    std::cout
        << "Persistent bounded glyph blocks, random relocation, sharing and cluster seams: PASS\n";
}

void glyph_parity(const TextLayout& edited, const TextLayout& fresh) {
    require(
        astra::ui::GlyphCache::glyph_digest(edited.glyph_snapshot()) ==
            astra::ui::GlyphCache::glyph_digest(fresh.glyph_snapshot()),
        "Incremental raw glyph IDs/clusters/advances/offsets/flags differ from complete shaping: " +
            astra::ui::GlyphCache::glyph_difference(edited.glyph_snapshot(),
                                                    fresh.glyph_snapshot()));
    require(glyph_pixels(edited) == glyph_pixels(fresh),
            "Incremental glyph pixels differ from independent complete shaping");
    near(edited.width(), fresh.width(), "Incremental glyph width differs", 0.001F);
    near(edited.height(), fresh.height(), "Incremental glyph height differs", 0.001F);
    for (const auto offset : fresh.grapheme_boundaries()) {
        for (const auto affinity : {CaretAffinity::upstream, CaretAffinity::downstream}) {
            const auto a = edited.caret(offset, affinity);
            const auto b = fresh.caret(offset, affinity);
            near(a.x, b.x, "Incremental caret X differs", 0.001F);
            near(a.y, b.y, "Incremental caret Y differs", 0.001F);
        }
    }
    for (float x = -2; x < fresh.width() + 2; x += 29.75F) {
        const auto a = edited.hit_test(x, fresh.height() / 2);
        const auto b = fresh.hit_test(x, fresh.height() / 2);
        require(a.byte_offset == b.byte_offset && a.affinity == b.affinity,
                "Incremental hit testing differs");
    }
}

void incremental_glyphs() {
    std::size_t spliced{}, shared{};
    for (const std::string unit : {"office ffi affinity constant; ",
                                   "中文编辑文本 字形连接 ",
                                   "مرحبا بالعالم سلام ",
                                   "שלום עולם ",
                                   "हिन्दी परीक्षण ",
                                   "é 👩‍💻 🇨🇳 😀 ",
                                   "abc\t中文\tאבג\tffi "}) {
        std::string text;
        for (int i = 0; i < 24; ++i) {
            text += unit;
        }
        for (const auto dpi : {1.0F, 1.5F, 2.0F}) {
            TextLayoutEngine owner, reference;
            TextLayoutOptions options;
            options.dpi_scale = dpi;
            options.direction = TextDirection::automatic;
            auto old = owner.layout(text, options);
            const auto before = astra::ui::GlyphCache::glyph_digest(old.glyph_snapshot());
            const auto retained_pixels = glyph_pixels(old);
            auto cut = text.size() / 2;
            while (!astra::utf8::is_boundary(text, cut)) {
                --cut;
            }
            const auto size = astra::utf8::decode(text, cut).bytes;
            const auto changed = text.substr(0, cut) + "x" + text.substr(cut + size);
            auto edited =
                owner.layout(changed, options, old.source_snapshot(), old.glyph_snapshot());
            auto fresh = reference.layout(changed, options);
            glyph_parity(edited, fresh);
            const auto stats = edited.glyph_snapshot().statistics();
            spliced += stats.spliced_runs;
            shared += stats.shared_glyphs;
            require(glyph_pixels(old) == retained_pixels &&
                        astra::ui::GlyphCache::glyph_digest(old.glyph_snapshot()) == before,
                    "Local glyph splicing mutated the retained paragraph or blocks");
        }
    }
    require(spliced > 0 && shared > 1000,
            "Native local edits did not share real glyph tree interiors");
    TextLayoutEngine owner, reference;
    TextLayoutOptions options;
    options.direction = TextDirection::automatic;
    std::string text;
    for (int i = 0; i < 15; ++i) {
        text += "office ffi 中文 é 👩‍💻 אבג مرحبا हिन्दी ";
    }
    auto current = owner.layout(text, options);
    std::mt19937 random(0xA57A92U);
    const std::array<std::string_view, 8> replacements{"", "ffi", "中", "🙂", "́", "a", "م", "\t"};
    for (std::size_t round = 0; round < 80; ++round) {
        std::vector<std::size_t> scalars{0};
        for (std::size_t offset = 0; offset < text.size();) {
            offset += astra::utf8::decode(text, offset).bytes;
            scalars.push_back(offset);
        }
        const auto first = random() % scalars.size();
        const auto last = std::min(scalars.size() - 1, first + random() % 4);
        const auto replacement = replacements[random() % replacements.size()];
        text.replace(scalars[first], scalars[last] - scalars[first], replacement);
        auto next =
            owner.layout(text, options, current.source_snapshot(), current.glyph_snapshot());
        auto fresh = reference.layout(text, options);
        try {
            glyph_parity(next, fresh);
        } catch (const std::exception& error) {
            throw std::runtime_error("glyph random seed 0xA57A92 round " + std::to_string(round) +
                                     " edit [" + std::to_string(scalars[first]) + "," +
                                     std::to_string(scalars[last]) + ") replacement " +
                                     std::string(replacement) + ": " + error.what());
        }
        current = std::move(next);
    }
    // Independent managers emulate retired worker ownership. Content identity
    // must certify reuse even when native typeface instance IDs differ.
    TextLayoutEngine first(TextLayoutEngine::FontIdentity::content);
    auto retained = first.layout(std::string(1500, 'a'));
    require(retained.glyph_snapshot().statistics().font_bytes_hashed > 0,
            "Background font content identity was not derived from native tables");
    TextLayoutEngine second(TextLayoutEngine::FontIdentity::content);
    auto changed = std::string(1500, 'a');
    changed.replace(750, 1, "x");
    auto moved = second.layout(changed, {}, retained.source_snapshot(), retained.glyph_snapshot());
    auto fresh = reference.layout(changed);
    glyph_parity(moved, fresh);
    const auto stats = moved.glyph_snapshot().statistics();
    require(stats.spliced_runs > 0 && stats.shaped_bytes < changed.size() &&
                stats.shared_glyphs > 1000 && stats.equivalent_fonts > 0,
            "Distinct native font owners did not skip unchanged glyph shaping");
    std::cout << "Native local glyph splicing, Unicode random edits, raw/pixel/caret/hit parity, "
                 "font content identity and cross-owner sharing: PASS\n";
}

void native_ligature_splicing() {
#if defined(_WIN32)
    class Observed final : public SkShapers::HB::RunCache {
      public:
        explicit Observed(astra::ui::GlyphSource previous)
            : cache(std::move(previous), std::make_shared<astra::ui::FontSignatures>()) {
        }

        astra::ui::GlyphCache cache;
        std::size_t bytes{}, largest{};
        bool merged{};

        Glyphs shape(const SkShapers::HB::CacheInput& input,
                     const Shape& uncached) noexcept override {
            return cache.shape(input, [&](SkShaper::RunHandler::Range range) {
                bytes += range.size();
                largest = std::max(largest, range.size());
                auto glyphs = uncached(range);
                if (std::string_view(input.text.data(), input.text.size()) == "ffi" &&
                    range.size() == 3) {
                    merged |= glyphs.size() < 3;
                }
                return glyphs;
            });
        }

        bool failed() const noexcept override {
            return cache.failed();
        }
    };

    auto fonts = sk_make_sp<skia::textlayout::FontCollection>();
    fonts->setDefaultFontManager(SkFontMgr_New_DirectWrite());
    fonts->getParagraphCache()->turnOn(false);
    const auto layout =
        [&](std::string_view text, astra::ui::GlyphSource previous, bool kerning = true) {
            auto collector = std::make_shared<Observed>(std::move(previous));
            fonts->setShapingCache(collector);
            struct Detach {
                skia::textlayout::FontCollection& fonts;
                ~Detach() {
                    fonts.setShapingCache({});
                }
            } detach{*fonts};
            skia::textlayout::TextStyle text_style;
            text_style.setFontFamilies({SkString("Calibri")});
            text_style.setFontSize(18);
            text_style.setLocale(SkString("en"));
            text_style.addFontFeature(SkString("liga"), 1);
            text_style.addFontFeature(SkString("dlig"), 1);
            text_style.addFontFeature(SkString("kern"), kerning ? 1 : 0);
            skia::textlayout::ParagraphStyle style;
            style.setTextStyle(text_style);
            auto builder =
                skia::textlayout::ParagraphBuilder::make(style, fonts, SkUnicodes::ICU::Make());
            builder->addText(text.data(), text.size());
            auto paragraph = builder->Build();
            paragraph->layout(100000);
            collector->cache.rethrow_error();
            return std::pair{std::move(paragraph), std::move(collector)};
        };
    const auto [ffi, observed] = layout("ffi", {});
    require(observed->merged, "Native Calibri fixture did not form an actual Latin ligature");
    std::string text;
    for (int i = 0; i < 40; ++i) {
        text += "office ffi AVATAR fXfi ";
    }
    auto [old, seed] = layout(text, {});
    const auto original = astra::ui::GlyphCache::glyph_digest(seed->cache.snapshot());
    for (const std::string_view needle : {"fXfi", "ffi", "AVATAR"}) {
        auto changed = text;
        const auto at = changed.find(needle, changed.size() / 2);
        require(at != changed.npos, "Cannot locate native ligature edit fixture");
        changed.erase(at + 1, 1);
        auto [incremental, reused] = layout(changed, seed->cache.snapshot());
        auto [fresh, cold] = layout(changed, {});
        require(astra::ui::GlyphCache::glyph_digest(reused->cache.snapshot()) ==
                    astra::ui::GlyphCache::glyph_digest(cold->cache.snapshot()),
                "Native Latin ligature or kerning splice differs from complete shaping: " +
                    astra::ui::GlyphCache::glyph_difference(reused->cache.snapshot(),
                                                            cold->cache.snapshot()));
        near(incremental->getLongestLine(),
             fresh->getLongestLine(),
             "Ligature splice width differs",
             0.001F);
        const auto stats = reused->cache.snapshot().statistics();
        require(reused->bytes == stats.shaped_bytes,
                "Native callback byte count differs from published shaping statistics");
        if (seed->cache.snapshot().statistics().safe_boundaries == 0) {
            require(stats.spliced_runs == 0 && stats.reused_bytes == 0 &&
                        reused->bytes == cold->bytes && reused->largest == changed.size(),
                    "Font with no native concat certificate bypassed complete shaping");
        } else {
            require(stats.spliced_runs > 0 && reused->bytes < cold->bytes &&
                        reused->largest < changed.size(),
                    "Certified native ligature edit did not skip unchanged shaping");
        }
        require(astra::ui::GlyphCache::glyph_digest(seed->cache.snapshot()) == original,
                "Ligature edits mutated the original raw snapshot");
    }
    std::cout << "Actual native Latin ligature formation/deletion, kerning and observed safe "
                 "fallback/shaping: PASS\n";
#endif
}

void glyph_snapshots() {
    {
        astra::ui::GlyphCache cache({});
        SkFont font;
        const SkShapers::HB::CacheInput input{{"x", 1}, {0, 1}, font, 0, 0, "und", {}, 0};
        int called{};
        const auto failure = [&](SkShaper::RunHandler::Range) -> SkShapers::HB::RunCache::Glyphs {
            ++called;
            throw std::runtime_error("injected native adapter failure");
        };
        require(cache.shape(input, failure).empty() && called == 1,
                "Adapter error escaped the native callback boundary");
        require(cache.shape(input, failure).empty() && called == 1,
                "Failed collector resumed native callbacks");
        bool reported{};
        try {
            cache.rethrow_error();
        } catch (const std::runtime_error& error) {
            reported = std::string_view(error.what()) == "injected native adapter failure";
        }
        require(reported, "Adapter swallowed its native-boundary error");
    }
    {
        // Execute a failure through the actual exception-disabled native stack,
        // not just a standalone cache callback. No failed glyph buffer is emitted.
        class Failure final : public SkShapers::HB::RunCache {
          public:
            astra::ui::GlyphCache collector{{}};
            int calls{};

            Glyphs shape(const SkShapers::HB::CacheInput& input, const Shape&) noexcept override {
                ++calls;
                return collector.shape(input, [](SkShaper::RunHandler::Range) -> Glyphs {
                    throw std::runtime_error("injected SkParagraph adapter failure");
                });
            }

            bool failed() const noexcept override {
                return collector.failed();
            }
        };

        auto failure = std::make_shared<Failure>();
        auto fonts = sk_make_sp<skia::textlayout::FontCollection>();
#if defined(_WIN32)
        fonts->setDefaultFontManager(SkFontMgr_New_DirectWrite());
#elif defined(__APPLE__)
        fonts->setDefaultFontManager(SkFontMgr_New_CoreText(nullptr));
#else
        fonts->setDefaultFontManager(
            SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType()));
#endif
        fonts->getParagraphCache()->turnOn(false);
        fonts->setShapingCache(failure);
        skia::textlayout::ParagraphStyle style;
        auto builder =
            skia::textlayout::ParagraphBuilder::make(style, fonts, SkUnicodes::ICU::Make());
        builder->addText("office ffi אבג 中文🙂");
        auto native = builder->Build();
        native->layout(300);
        fonts->setShapingCache({});
        require(failure->failed() && failure->calls == 1,
                "Native shaping did not stop before emitting failed glyph buffers");
        bool reported{};
        try {
            failure->collector.rethrow_error();
        } catch (const std::runtime_error& error) {
            reported = std::string_view(error.what()) == "injected SkParagraph adapter failure";
        }
        require(reported, "Actual native adapter failed without reporting its original error");
    }
    for (const auto& text : {"office ffi affinity -- const value = 42;",
                             "abc אבג مرحبا 中文 🙂 👩‍💻 é ffi",
                             "a\tb\t中文\tz"}) {
        for (const auto dpi : {1.0F, 1.5F, 2.0F}) {
            TextLayoutEngine owner;
            TextLayoutOptions options;
            options.dpi_scale = dpi;
            options.direction = TextDirection::automatic;
            auto old = owner.layout(text, options);
            const auto source = old.glyph_snapshot();
            const auto old_stats = source.statistics();
            const auto old_pixels = glyph_pixels(old);
            require(old_stats.runs > 0 && old_stats.glyphs > 0 && old_stats.shaped_bytes > 0,
                    "Native HarfBuzz glyphs were not captured");
            require(old_stats.font_bytes_hashed == 0,
                    "Interactive caption owner performed background font fingerprint work");
            require(old_stats.safe_boundaries + old_stats.unsafe_boundaries > 0,
                    "Native concatenation boundary metadata was not captured");

            // Different paint/strut require a new paragraph while the raw
            // shaping/font/context remains identical. Inspect actual skipped work.
            options.color = 0xFF73C1A9;
            // Strut height changes paragraph geometry independently of glyphs.
            options.line_height += 0.1F;
            auto replay = owner.layout(text, options, old.source_snapshot(), source);
            const auto stats = replay.glyph_snapshot().statistics();
            require(stats.reused_bytes > 0,
                    "Exact native glyph replay did not skip shaping: " + std::string(text));
            TextLayoutEngine independent;
            auto fresh = independent.layout(text, options);
            require(glyph_pixels(replay) == glyph_pixels(fresh),
                    "Replayed native glyph pixels differ from independent complete shaping");
            near(replay.width(), fresh.width(), "Replayed native glyph width differs", 0.001F);
            near(replay.height(), fresh.height(), "Replayed native glyph height differs", 0.001F);
            for (const auto offset : fresh.grapheme_boundaries()) {
                for (const auto affinity : {CaretAffinity::upstream, CaretAffinity::downstream}) {
                    const auto a = replay.caret(offset, affinity);
                    const auto b = fresh.caret(offset, affinity);
                    near(a.x, b.x, "Replayed glyph caret X differs", 0.001F);
                    near(a.y, b.y, "Replayed glyph caret Y differs", 0.001F);
                }
            }
            for (float x = -2; x < fresh.width() + 2; x += 1.75F) {
                const auto a = replay.hit_test(x, fresh.height() / 2);
                const auto b = fresh.hit_test(x, fresh.height() / 2);
                require(a.byte_offset == b.byte_offset && a.affinity == b.affinity,
                        "Replayed glyph hit testing differs");
            }
            require(old_pixels == glyph_pixels(old) &&
                        source.statistics().bytes == old_stats.bytes &&
                        source.statistics().shaped_bytes == old_stats.shaped_bytes,
                    "Glyph replay mutated the retained native/source snapshot");

            options.font_size += 3;
            auto changed_font = owner.layout(text, options, old.source_snapshot(), source);
            require(changed_font.glyph_snapshot().statistics().reused_bytes == 0,
                    "Changed font geometry reused incompatible glyphs");
            auto changed_fresh = independent.layout(text, options);
            require(glyph_pixels(changed_font) == glyph_pixels(changed_fresh),
                    "Changed font fallback differs from complete shaping");

            options.font_size -= 3;
            auto edited =
                owner.layout(std::string(text) + "x", options, old.source_snapshot(), source);
            auto edited_fresh = independent.layout(std::string(text) + "x", options);
            require(glyph_pixels(edited) == glyph_pixels(edited_fresh),
                    "Edited context did not safely reshape");
        }
    }
    std::string long_text(180000, 'a');
    TextLayoutEngine engine;
    auto long_line = engine.layout(long_text);
    const auto stats = long_line.glyph_snapshot().statistics();
    require(stats.bytes <= 4 * 1024 * 1024 && stats.runs <= 1024 &&
                stats.shaped_bytes >= long_text.size(),
            "Oversized native glyph capture violated admission or skipped complete shaping");
    std::cout << "Native immutable glyph snapshots, exact replay, font/context invalidation, "
                 "bidi/ligatures/fallback/tabs/DPI pixel parity and admission: PASS\n";
}

} // namespace

int main() {
    try {
        glyph_sequence_snapshots();
        incremental_glyphs();
        native_ligature_splicing();
        glyph_snapshots();
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
