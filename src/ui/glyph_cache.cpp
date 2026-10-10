#include "glyph_cache.hpp"
#include "font_signatures.hpp"
#include "glyph_sequence.hpp"

#include <astra/core/sum_tree.hpp>
#include <astra/core/utf8.hpp>
#include <modules/skunicode/include/SkUnicode_icu.h>
#include <algorithm>
#include <bit>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace astra::ui {
namespace {
constexpr std::size_t maximum_bytes = 4 * 1024 * 1024;
constexpr std::size_t maximum_runs = 1024;
using Input = SkShapers::HB::CacheInput;
using Glyphs = SkShapers::HB::RunCache::Glyphs;
using Shape = SkShapers::HB::RunCache::Shape;

struct Feature {
    SkFourByteTag tag{};
    std::uint32_t value{};
    std::size_t begin{}, end{};
    friend bool operator==(const Feature&, const Feature&) = default;
};

struct Key {
    std::string text, language;
    std::size_t begin{}, end{};
    SkTypefaceID typeface{};
    std::optional<ContentDigest> signature;
    SkFont font;
    std::uint8_t level{};
    SkFourByteTag script{};
    float tracking{};
    std::vector<Feature> features;

    bool compatible(const Input& input, const std::optional<ContentDigest>& identity) const {
        if (!input.font.getTypeface() || !input.language ||
            (typeface != input.font.getTypeface()->uniqueID() &&
             (!signature || !identity || signature != identity)) ||
            level != input.level || script != input.script || tracking != input.tracking ||
            language != input.language) {
            return false;
        }
        auto candidate = input.font;
        candidate.setTypeface(nullptr);
        return font == candidate;
    }

    bool matches(const Input& input, const std::optional<ContentDigest>& identity) const {
        if (!compatible(input, identity) || begin != input.range.begin() ||
            end != input.range.end() ||
            text != std::string_view(input.text.data(), input.text.size()) ||
            features.size() != input.features.size()) {
            return false;
        }
        for (std::size_t i = 0; i < features.size(); ++i) {
            const auto& feature = input.features[i];
            if (features[i] != Feature{feature.tag, feature.value, feature.start, feature.end}) {
                return false;
            }
        }
        return true;
    }

    // Partial feature ranges can change at relocated cuts. Only whole-item
    // feature applications certify this initial incremental path.
    bool whole_features(const Input& input) const {
        std::vector<std::pair<SkFourByteTag, std::uint32_t>> old, current;
        for (const auto& feature : features) {
            if (feature.end <= begin || feature.begin >= end) {
                continue;
            }
            if (feature.begin > begin || feature.end < end) {
                return false;
            }
            old.emplace_back(feature.tag, feature.value);
        }
        for (const auto& feature : input.features) {
            if (feature.end <= input.range.begin() || feature.start >= input.range.end()) {
                continue;
            }
            if (feature.start > input.range.begin() || feature.end < input.range.end()) {
                return false;
            }
            current.emplace_back(feature.tag, feature.value);
        }
        return old == current;
    }
};

struct Run {
    Key key;
    GlyphSequence glyphs;
    std::size_t bytes{};
};

struct Summary {
    std::size_t bytes{}, glyphs{};

    friend Summary operator+(Summary a, Summary b) {
        return {a.bytes + b.bytes, a.glyphs + b.glyphs};
    }
};

struct Measure {
    Summary operator()(const std::shared_ptr<const Run>& run) const {
        return {run->bytes, run->glyphs.summary().glyphs};
    }
};

using Runs = SumTree<std::shared_ptr<const Run>, Summary, Measure>;

std::optional<std::size_t> admission(const Input& input, std::size_t payload) {
    std::size_t bytes = sizeof(Run);
    const auto add = [&](std::size_t amount) {
        if (amount > maximum_bytes - bytes) {
            return false;
        }
        bytes += amount;
        return true;
    };
    if (!input.language || input.features.size() > maximum_bytes / sizeof(Feature) ||
        !add(input.text.size()) || !add(std::char_traits<char>::length(input.language)) ||
        !add(payload) || !add(input.features.size() * sizeof(Feature))) {
        return {};
    }
    return bytes;
}

struct Difference {
    std::size_t prefix{}, suffix{};
};

std::optional<Difference> difference(const Key& key, const Input& input) {
    const std::string_view previous(key.text), current(input.text.data(), input.text.size());
    if (key.begin > previous.size() || key.end > previous.size() ||
        input.range.end() > current.size() ||
        previous.substr(0, key.begin) != current.substr(0, input.range.begin()) ||
        previous.substr(key.end) != current.substr(input.range.end()) ||
        !key.whole_features(input)) {
        return {};
    }
    const auto old = previous.substr(key.begin, key.end - key.begin);
    const auto now = current.substr(input.range.begin(), input.range.size());
    std::size_t prefix{};
    while (prefix < std::min(old.size(), now.size()) && old[prefix] == now[prefix]) {
        ++prefix;
    }
    while (!utf8::is_boundary(old, prefix) || !utf8::is_boundary(now, prefix)) {
        --prefix;
    }
    std::size_t suffix{};
    while (suffix < std::min(old.size(), now.size()) - prefix &&
           old[old.size() - suffix - 1] == now[now.size() - suffix - 1]) {
        ++suffix;
    }
    while (!utf8::is_boundary(old, old.size() - suffix) ||
           !utf8::is_boundary(now, now.size() - suffix)) {
        --suffix;
    }
    return Difference{prefix, suffix};
}

bool clear_boundary(const Glyphs& glyphs, std::size_t byte) {
    auto found = std::lower_bound(
        glyphs.begin(), glyphs.end(), byte, [](const auto& glyph, std::size_t target) {
            return glyph.cluster < target;
        });
    if (found == glyphs.end() || found->cluster != byte) {
        return false;
    }
    while (found != glyphs.end() && found->cluster == byte) {
        if (found->unsafeToConcat) {
            return false;
        }
        ++found;
    }
    return true;
}

std::vector<std::size_t> grapheme_cuts(SkUnicode& unicode, std::string_view text) {
    auto iterator = unicode.makeBreakIterator(SkUnicode::BreakType::kGraphemes);
    if (!iterator || !iterator->setText(text.data(), static_cast<int>(text.size()))) {
        throw std::runtime_error("Cannot establish incremental glyph grapheme boundaries");
    }
    std::vector<std::size_t> result;
    for (auto offset = iterator->first(); !iterator->isDone(); offset = iterator->next()) {
        result.push_back(static_cast<std::size_t>(offset));
    }
    return result;
}

// Follow HarfBuzz's two-sided unsafe-to-concat algorithm, including an
// overread cluster to obtain the otherwise unavailable end-of-text flag.
// Full original context is passed by the native callback on every attempt.
std::optional<GlyphSequence> splice(const Run& old,
                                    const Input& input,
                                    Difference changed,
                                    const Shape& shape,
                                    GlyphSourceStatistics& stats,
                                    SkUnicode& unicode) {
    const auto boundaries = old.glyphs.boundaries(old.key.begin);
    if (boundaries.size() < 2) {
        return {};
    }
    const auto old_cuts = grapheme_cuts(unicode, old.key.text);
    const auto new_cuts = grapheme_cuts(unicode, {input.text.data(), input.text.size()});
    const auto left_cut = [&](std::size_t index) {
        return index == 0 ||
               (std::binary_search(old_cuts.begin(), old_cuts.end(), boundaries[index].byte) &&
                std::binary_search(new_cuts.begin(),
                                   new_cuts.end(),
                                   input.range.begin() + boundaries[index].byte - old.key.begin));
    };
    const auto right_cut = [&](std::size_t index) {
        return index + 1 == boundaries.size() ||
               (std::binary_search(old_cuts.begin(), old_cuts.end(), boundaries[index].byte) &&
                std::binary_search(new_cuts.begin(),
                                   new_cuts.end(),
                                   input.range.end() - (old.key.end - boundaries[index].byte)));
    };
    std::size_t left{}, right = boundaries.size() - 1;
    for (std::size_t i = 0; i + 1 < boundaries.size(); ++i) {
        if (boundaries[i].byte <= old.key.begin + changed.prefix && !boundaries[i].unsafe &&
            left_cut(i)) {
            left = i;
        }
    }
    for (std::size_t i = left; i < boundaries.size(); ++i) {
        if (boundaries[i].byte >= old.key.end - changed.suffix &&
            (!boundaries[i].unsafe || i + 1 == boundaries.size()) && right_cut(i)) {
            right = i;
            break;
        }
    }
    for (std::size_t attempt = 0; attempt < 8; ++attempt) {
        const bool keep_prefix = boundaries[left].byte != old.key.begin;
        const bool keep_suffix = boundaries[right].byte != old.key.end;
        if (!keep_prefix && !keep_suffix) {
            return {};
        }
        const auto begin = input.range.begin() + boundaries[left].byte - old.key.begin;
        const auto end = input.range.end() - (old.key.end - boundaries[right].byte);
        const auto probe_end =
            keep_suffix ? input.range.end() - (old.key.end - boundaries[right + 1].byte) : end;
        if (begin > end || end > probe_end) {
            return {};
        }
        ++stats.shape_attempts;
        stats.shaped_bytes += probe_end - begin;
        auto probe = shape({begin, probe_end - begin});
        const bool left_clear = !keep_prefix || clear_boundary(probe, begin);
        const bool right_clear = !keep_suffix || clear_boundary(probe, end);
        if (left_clear && right_clear) {
            const auto cut = std::lower_bound(
                probe.begin(), probe.end(), end, [](const auto& glyph, std::size_t target) {
                    return glyph.cluster < target;
                });
            const auto middle = GlyphSequence::capture(
                std::span<const GlyphSequence::Glyph>(probe.begin(), cut), begin, end);
            auto [prefix, remaining] =
                old.glyphs.split(boundaries[left].glyph, boundaries[left].byte - old.key.begin);
            auto [discarded, suffix] =
                remaining.split(boundaries[right].glyph - boundaries[left].glyph,
                                boundaries[right].byte - boundaries[left].byte);
            auto result = prefix.concat(middle).concat(suffix);
            stats.reused_bytes += prefix.summary().bytes + suffix.summary().bytes;
            stats.shared_glyphs += result.shared_glyphs(old.glyphs);
            ++stats.spliced_runs;
            return result;
        }
        // Expand geometrically, then choose genuine safe cluster boundaries.
        const auto step = std::size_t{1} << attempt;
        if (!left_clear) {
            left = left > step ? left - step : 0;
            while (left != 0 && (boundaries[left].unsafe || !left_cut(left))) {
                --left;
            }
        }
        if (!right_clear) {
            right = std::min(boundaries.size() - 1, right + step);
            while (right + 1 < boundaries.size() &&
                   (boundaries[right].unsafe || !right_cut(right))) {
                ++right;
            }
        }
    }
    return {};
}
} // namespace

struct GlyphSource::Data {
    Runs runs;
    GlyphSourceStatistics statistics;
};

struct GlyphCache::Impl {
    sk_sp<SkUnicode> unicode;
    GlyphSource previous;
    std::shared_ptr<FontSignatures> signatures;
    Runs runs;
    GlyphSourceStatistics statistics;
    std::size_t next_previous{}, initial_font_bytes{};
    std::exception_ptr error;
};

GlyphSourceStatistics GlyphSource::statistics() const noexcept {
    return data_ ? data_->statistics : GlyphSourceStatistics{};
}

GlyphCache::GlyphCache(GlyphSource previous, std::shared_ptr<FontSignatures> signatures)
    : impl_(std::make_unique<Impl>()) {
    impl_->previous = std::move(previous);
    impl_->signatures = std::move(signatures);
    impl_->initial_font_bytes = impl_->signatures ? impl_->signatures->bytes_hashed() : 0;
}

GlyphCache::~GlyphCache() = default;

GlyphCache::Glyphs GlyphCache::shape(const Input& input, const Shape& uncached) noexcept {
    if (impl_->error) {
        return {};
    }
    try {
        return shape_checked(input, uncached);
    } catch (...) {
        // Skia is compiled without C++ exception unwinding. Preserve the first
        // error inside this adapter and rethrow outside native layout.
        impl_->error = std::current_exception();
        return {};
    }
}

void GlyphCache::rethrow_error() const {
    if (impl_->error) {
        std::rethrow_exception(impl_->error);
    }
}

bool GlyphCache::failed() const noexcept {
    return static_cast<bool>(impl_->error);
}

GlyphCache::Glyphs GlyphCache::shape_checked(const Input& input, const Shape& uncached) {
    auto& state = *impl_;
    const auto identity = input.font.getTypeface() && state.signatures
                              ? state.signatures->read(*input.font.getTypeface())
                              : std::optional<ContentDigest>{};
    std::shared_ptr<const Run> exact, candidate;
    std::optional<Difference> changed;
    std::size_t candidate_index{}, score{};
    if (input.font.getTypeface() && input.language && state.previous.data_) {
        const auto& runs = state.previous.data_->runs;
        for (std::size_t i = state.next_previous; i < runs.size(); ++i) {
            const auto& run = runs.at(i);
            if (run->key.matches(input, identity)) {
                state.next_previous = i + 1;
                exact = run;
                break;
            }
            if (run->key.compatible(input, identity)) {
                const auto diff = difference(run->key, input);
                if (diff && diff->prefix + diff->suffix > score) {
                    score = diff->prefix + diff->suffix;
                    candidate = run;
                    candidate_index = i;
                    changed = diff;
                }
            }
        }
    }
    std::optional<GlyphSequence> sequence;
    Glyphs glyphs;
    if (exact) {
        sequence = exact->glyphs;
        glyphs = sequence->flatten(input.range.begin());
        state.statistics.reused_bytes += input.range.size();
        state.statistics.shared_glyphs += sequence->summary().glyphs;
    } else if (candidate) {
        if (!state.unicode) {
            state.unicode = SkUnicodes::ICU::Make();
            if (!state.unicode) {
                throw std::runtime_error("ICU incremental glyph initialization failed");
            }
        }
        sequence = splice(*candidate, input, *changed, uncached, state.statistics, *state.unicode);
        if (sequence) {
            state.next_previous = candidate_index + 1;
            glyphs = sequence->flatten(input.range.begin());
        }
    }
    if (sequence &&
        (exact ? exact : candidate)->key.typeface != input.font.getTypeface()->uniqueID()) {
        ++state.statistics.equivalent_fonts;
    }
    if (!sequence) {
        ++state.statistics.shape_attempts;
        state.statistics.shaped_bytes += input.range.size();
        glyphs = uncached(input.range);
    }
    // Bypass oversized captures before allocating another persistent glyph copy.
    const auto estimated = admission(input, glyphs.size() * sizeof(GlyphSequence::Glyph));
    if (!estimated || !input.font.getTypeface() || state.runs.size() == maximum_runs ||
        *estimated > maximum_bytes - state.runs.summary().bytes) {
        return glyphs;
    }
    if (!sequence) {
        sequence = GlyphSequence::capture(glyphs, input.range.begin(), input.range.end());
    }
    const auto bytes = admission(input, sequence->summary().payload);
    if (!bytes || *bytes > maximum_bytes - state.runs.summary().bytes) {
        return glyphs;
    }
    auto run = std::make_shared<Run>();
    run->key.text.assign(input.text.data(), input.text.size());
    run->key.language = input.language;
    run->key.begin = input.range.begin();
    run->key.end = input.range.end();
    run->key.font = input.font;
    run->key.font.setTypeface(nullptr);
    run->key.typeface = input.font.getTypeface()->uniqueID();
    run->key.signature = identity;
    run->key.level = input.level;
    run->key.script = input.script;
    run->key.tracking = input.tracking;
    for (const auto& feature : input.features) {
        run->key.features.push_back({feature.tag, feature.value, feature.start, feature.end});
    }
    run->glyphs = std::move(*sequence);
    run->bytes = *bytes;
    state.runs = state.runs.concat(Runs::singleton(std::move(run)));
    return glyphs;
}

GlyphSource GlyphCache::snapshot() const {
    auto data = std::make_shared<GlyphSource::Data>();
    data->runs = impl_->runs;
    data->statistics = impl_->statistics;
    data->statistics.runs = data->runs.size();
    data->statistics.bytes = data->runs.summary().bytes;
    data->statistics.glyphs = data->runs.summary().glyphs;
    data->statistics.font_bytes_hashed =
        impl_->signatures ? impl_->signatures->bytes_hashed() - impl_->initial_font_bytes : 0;
    for (std::size_t i = 0; i < data->runs.size(); ++i) {
        const auto& run = *data->runs.at(i);
        const auto boundaries = run.glyphs.boundaries(run.key.begin);
        for (std::size_t j = 0; j + 1 < boundaries.size(); ++j) {
            if (boundaries[j].unsafe) {
                ++data->statistics.unsafe_boundaries;
            } else {
                ++data->statistics.safe_boundaries;
            }
        }
    }
    GlyphSource result;
    result.data_ = std::move(data);
    return result;
}

ContentDigest GlyphCache::glyph_digest(const GlyphSource& source) {
    ContentHash hash;
    const auto number = [&](std::uint64_t value) {
        std::array<std::byte, 8> bytes{};
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            bytes[i] = std::byte((value >> (i * 8U)) & 255U);
        }
        hash.update(bytes);
    };
    hash.update("Astra logical glyph diagnostic v1");
    if (source.data_) {
        number(source.data_->runs.size());
        source.data_->runs.for_each([&](const auto& run) {
            number(run->key.begin);
            number(run->key.end);
            number(run->glyphs.summary().glyphs);
            for (const auto& glyph : run->glyphs.flatten(run->key.begin)) {
                number(glyph.id);
                number(glyph.cluster);
                number(std::bit_cast<std::uint32_t>(glyph.offset.x()));
                number(std::bit_cast<std::uint32_t>(glyph.offset.y()));
                number(std::bit_cast<std::uint32_t>(glyph.advance.x()));
                number(std::bit_cast<std::uint32_t>(glyph.advance.y()));
                number(glyph.hasVisual);
                number(glyph.unsafeToBreak);
                number(glyph.unsafeToConcat);
            }
        });
    }
    return hash.digest();
}

std::string GlyphCache::glyph_difference(const GlyphSource& first, const GlyphSource& second) {
    if (!first.data_ || !second.data_) {
        return "missing snapshot";
    }
    if (first.data_->runs.size() != second.data_->runs.size()) {
        return "run count " + std::to_string(first.data_->runs.size()) + " vs " +
               std::to_string(second.data_->runs.size());
    }
    for (std::size_t r = 0; r < first.data_->runs.size(); ++r) {
        const auto& a = *first.data_->runs.at(r);
        const auto& b = *second.data_->runs.at(r);
        const auto label = "run " + std::to_string(r) + " [" + std::to_string(a.key.begin) + "," +
                           std::to_string(a.key.end) + ") script=" + std::to_string(a.key.script) +
                           "/" + std::to_string(b.key.script) +
                           " level=" + std::to_string(a.key.level) + "/" +
                           std::to_string(b.key.level) + " ";
        const auto left = a.glyphs.flatten(a.key.begin);
        const auto right = b.glyphs.flatten(b.key.begin);
        if (a.key.begin != b.key.begin || a.key.end != b.key.end || left.size() != right.size()) {
            return label + "range/count differs";
        }
        for (std::size_t i = 0; i < left.size(); ++i) {
            const auto& x = left[i];
            const auto& y = right[i];
            const auto position = label + "glyph " + std::to_string(i) + " ";
            if (x.id != y.id || x.cluster != y.cluster || x.offset != y.offset ||
                x.advance != y.advance || x.hasVisual != y.hasVisual) {
                return position + "geometry differs: clusters " + std::to_string(x.cluster) + "/" +
                       std::to_string(y.cluster) + ", ids " + std::to_string(x.id) + "/" +
                       std::to_string(y.id);
            }
            if (x.unsafeToBreak != y.unsafeToBreak || x.unsafeToConcat != y.unsafeToConcat) {
                return position + "text=" + a.key.text + " flags differ at cluster " +
                       std::to_string(x.cluster) + ": break " + std::to_string(x.unsafeToBreak) +
                       "/" + std::to_string(y.unsafeToBreak) + ", concat " +
                       std::to_string(x.unsafeToConcat) + "/" + std::to_string(y.unsafeToConcat);
            }
        }
    }
    return "no field difference";
}
} // namespace astra::ui
