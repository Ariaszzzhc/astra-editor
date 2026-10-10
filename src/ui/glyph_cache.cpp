#include "glyph_cache.hpp"

#include <astra/core/sum_tree.hpp>
#include <include/core/SkTypeface.h>
#include <optional>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace astra::ui {
namespace {
constexpr std::size_t maximum_bytes = 4 * 1024 * 1024;
constexpr std::size_t maximum_runs = 1024;

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
    SkFont font;
    std::uint8_t level{};
    SkFourByteTag script{};
    float tracking{};
    std::vector<Feature> features;

    bool matches(const SkShapers::HB::CacheInput& input) const {
        if (typeface != input.font.getTypeface()->uniqueID() || begin != input.range.begin() ||
            end != input.range.end() || level != input.level || script != input.script ||
            tracking != input.tracking || language != input.language ||
            text != std::string_view(input.text.data(), input.text.size()) ||
            features.size() != input.features.size()) {
            return false;
        }
        auto candidate = input.font;
        candidate.setTypeface(nullptr);
        if (font != candidate) {
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
};

struct Run {
    Key key;
    SkShapers::HB::RunCache::Glyphs glyphs;
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
        return {run->bytes, run->glyphs.size()};
    }
};

using Runs = SumTree<std::shared_ptr<const Run>, Summary, Measure>;

std::optional<std::size_t> admission(const SkShapers::HB::CacheInput& input,
                                     const SkShapers::HB::RunCache::Glyphs& glyphs) {
    std::size_t bytes = sizeof(Run);
    const auto add = [&](std::size_t amount) {
        if (amount > maximum_bytes - bytes) {
            return false;
        }
        bytes += amount;
        return true;
    };
    if (!input.language || glyphs.size() > maximum_bytes / sizeof(SkShapers::HB::CachedGlyph) ||
        input.features.size() > maximum_bytes / sizeof(Feature) || !add(input.text.size()) ||
        !add(std::char_traits<char>::length(input.language)) ||
        !add(glyphs.size() * sizeof(SkShapers::HB::CachedGlyph)) ||
        !add(input.features.size() * sizeof(Feature))) {
        return {};
    }
    return bytes;
}
} // namespace

struct GlyphSource::Data {
    Runs runs;
    GlyphSourceStatistics statistics;
};

struct GlyphCache::Impl {
    GlyphSource previous;
    Runs runs;
    GlyphSourceStatistics statistics;
    std::size_t next_previous{};
    std::exception_ptr error;
};

GlyphSourceStatistics GlyphSource::statistics() const noexcept {
    return data_ ? data_->statistics : GlyphSourceStatistics{};
}

GlyphCache::GlyphCache(GlyphSource previous) : impl_(std::make_unique<Impl>()) {
    impl_->previous = std::move(previous);
}

GlyphCache::~GlyphCache() = default;

GlyphCache::Glyphs GlyphCache::shape(const SkShapers::HB::CacheInput& input,
                                     const Shape& uncached) noexcept {
    if (impl_->error) {
        return {};
    }
    try {
        return shape_checked(input, uncached);
    } catch (...) {
        // Skia is compiled without C++ exception unwinding. Preserve the first
        // error inside this adapter; the caller rethrows after native layout
        // returns, before any paragraph geometry can be published.
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

GlyphCache::Glyphs GlyphCache::shape_checked(const SkShapers::HB::CacheInput& input,
                                             const Shape& uncached) {
    auto& state = *impl_;
    std::shared_ptr<const Run> reused;
    // Full context, absolute feature ranges and native typeface generation must
    // match. No font-name, ASCII or whitespace approximation certifies reuse.
    if (input.font.getTypeface() && input.language && state.previous.data_) {
        const auto& runs = state.previous.data_->runs;
        for (std::size_t i = state.next_previous; i < runs.size(); ++i) {
            const auto& run = runs.at(i);
            if (run->key.matches(input)) {
                state.next_previous = i + 1;
                reused = run;
                break;
            }
        }
    }
    auto glyphs = reused ? reused->glyphs : uncached(input.range);
    if (reused) {
        state.statistics.reused_bytes += input.range.size();
    } else {
        state.statistics.shaped_bytes += input.range.size();
    }
    const auto bytes = admission(input, glyphs);
    if (!bytes || !input.font.getTypeface() || state.runs.size() == maximum_runs ||
        *bytes > maximum_bytes - state.runs.summary().bytes) {
        return glyphs;
    }
    if (!reused) {
        auto run = std::make_shared<Run>();
        run->key.text.assign(input.text.data(), input.text.size());
        run->key.language = input.language;
        run->key.begin = input.range.begin();
        run->key.end = input.range.end();
        run->key.font = input.font;
        run->key.font.setTypeface(nullptr);
        run->key.typeface = input.font.getTypeface()->uniqueID();
        run->key.level = input.level;
        run->key.script = input.script;
        run->key.tracking = input.tracking;
        for (const auto& feature : input.features) {
            run->key.features.push_back({feature.tag, feature.value, feature.start, feature.end});
        }
        run->glyphs = glyphs;
        run->bytes = *bytes;
        reused = std::move(run);
    }
    state.runs = state.runs.concat(Runs::singleton(std::move(reused)));
    return glyphs;
}

GlyphSource GlyphCache::snapshot() const {
    auto data = std::make_shared<GlyphSource::Data>();
    data->runs = impl_->runs;
    data->statistics = impl_->statistics;
    data->statistics.runs = data->runs.size();
    data->statistics.bytes = data->runs.summary().bytes;
    data->statistics.glyphs = data->runs.summary().glyphs;
    for (std::size_t i = 0; i < data->runs.size(); ++i) {
        const auto& glyphs = data->runs.at(i)->glyphs;
        for (std::size_t j = 0; j < glyphs.size();) {
            const auto cluster = glyphs[j].cluster;
            bool unsafe{};
            do {
                unsafe |= glyphs[j++].unsafeToConcat;
            } while (j < glyphs.size() && glyphs[j].cluster == cluster);
            if (unsafe) {
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
} // namespace astra::ui
