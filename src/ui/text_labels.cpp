#include <astra/ui/text_labels.hpp>
#include <astra/core/utf8.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
std::string display_prefix(std::string_view source) {
    std::string display;
    display.reserve(std::min(source.size(), TextLabels::max_source_bytes));
    for (std::size_t offset = 0; offset < source.size();) {
        if (offset >= TextLabels::max_source_bytes) {
            break;
        }
        const auto scalar = utf8::decode(source, offset);
        if (scalar.bytes > TextLabels::max_source_bytes - offset) {
            break;
        }
        // Captions remain one visual line, including Unicode separators. The
        // full original string remains in the caller's model for reads/actions.
        if (scalar.value < 0x20 || scalar.value == 0x7F || scalar.value == 0x85 ||
            scalar.value == 0x2028 || scalar.value == 0x2029) {
            display.push_back(' ');
        } else {
            display.append(source.substr(offset, scalar.bytes));
        }
        offset += scalar.bytes;
    }
    return display;
}

void require_plain_style(const TextLayoutOptions& style) {
    if (style.wrap_width != 0 || !style.color_spans.empty()) {
        throw std::invalid_argument("Chrome labels require unwrapped, plain text");
    }
}
} // namespace

TextLabels::TextLabels(const TextLayoutEngine& engine, ViewportCacheLimits limits)
    : paragraphs_(engine, limits) {
}

std::shared_ptr<const TextLayout> TextLabels::shape(std::string text,
                                                    const TextLayoutOptions& style) {
    const auto bytes = text.size();
    paragraphs_.publish({{0, 0, bytes, bytes, true, std::move(text)}});
    static_cast<void>(paragraphs_.shape(0, style));
    return paragraphs_.layout_snapshot(0);
}

std::shared_ptr<const TextLayout> TextLabels::prepare(std::string_view source,
                                                      const TextLayoutOptions& style) {
    require_plain_style(style);
    auto display = display_prefix(source);
    auto paragraph = shape(display, style);
    if (source.size() > max_source_bytes) {
        // A bounded prefix may finish inside a combining/ZWJ cluster whose
        // continuation is not scanned. Conservatively omit its final cluster.
        const auto boundaries = paragraph->grapheme_boundaries();
        const auto end = boundaries.size() > 1 ? boundaries[boundaries.size() - 2] : 0;
        display.resize(end);
        display += "…";
        paragraph = shape(std::move(display), style);
    }
    return paragraph;
}

std::shared_ptr<const TextLayout>
TextLabels::elide(std::string_view source, const TextLayoutOptions& style, float width) {
    if (!std::isfinite(width)) {
        throw std::invalid_argument("Chrome label width must be finite");
    }
    if (source.empty() || width <= 0) {
        return {};
    }
    const auto full = prepare(source, style);
    if (full->width() <= width) {
        return full;
    }
    const auto ellipsis = shape("…", style);
    if (ellipsis->width() > width) {
        return {};
    }
    auto text = full->text();
    if (source.size() > max_source_bytes) {
        text.remove_suffix(std::string_view("…").size());
    }
    const auto boundaries = full->grapheme_boundaries();
    const auto end = std::upper_bound(boundaries.begin(), boundaries.end(), text.size());
    std::size_t low{}, high = static_cast<std::size_t>(std::distance(boundaries.begin(), end)) - 1;
    auto result = ellipsis;
    while (low < high) {
        const auto middle = (low + high + 1) / 2;
        auto candidate = shape(std::string(text.substr(0, boundaries[middle])) + "…", style);
        if (candidate->width() <= width) {
            low = middle;
            result = std::move(candidate);
        } else {
            high = middle - 1;
        }
    }
    return result;
}

ViewportCacheStatistics TextLabels::statistics() const noexcept {
    return paragraphs_.statistics();
}
} // namespace astra::ui
