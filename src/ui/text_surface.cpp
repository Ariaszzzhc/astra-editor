#include <astra/ui/text_surface.hpp>
#include <astra/core/utf8.hpp>

#include <stdexcept>
#include <utility>
#include <vector>

namespace astra::ui {
TextSurface::TextSurface(const TextLayoutEngine& fonts,
                         std::shared_ptr<LayoutJobs> jobs,
                         ViewportCacheLimits limits)
    : viewport_(fonts, limits, std::move(jobs)) {
}

void TextSurface::reset(std::string_view text) {
    if (text.size() > TextLineIndex::max_bytes) {
        throw std::length_error("Text surface exceeds its content limit");
    }
    // Validate before replacing the visible content or cancelling its work.
    utf8::validate(text);
    TextLineIndex replacement;
    replacement.reset(text);
    viewport_.publish({});
    lines_ = std::move(replacement);
    visible_.reset();
    styles_.clear();
}

void TextSurface::show(std::size_t begin, std::size_t end) {
    if (begin > end || end > lines_.size() || end - begin > 256) {
        throw std::out_of_range("Text surface page exceeds its visible row bounds");
    }
    const auto range = std::pair{begin, end};
    if (visible_ == range) {
        return;
    }
    std::vector<ViewportLine> rows;
    rows.reserve(end - begin);
    lines_.for_each_range(begin, end, [&](std::size_t row, const std::string& text) {
        const auto offset = lines_.offset(row);
        const auto next = row + 1 < lines_.size() ? lines_.offset(row + 1) : lines_.bytes();
        rows.push_back({row, offset, offset + text.size(), next, true, text});
    });
    viewport_.publish(std::move(rows));
    visible_ = range;
    styles_.clear();
}

const TextLineIndex& TextSurface::lines() const noexcept {
    return lines_;
}

const TextLayout* TextSurface::prepare(std::size_t row, const TextLayoutOptions& options) {
    const auto found = styles_.find(row);
    if (found != styles_.end() && found->second == options) {
        if (const auto* paragraph = viewport_.layout(row)) {
            return paragraph;
        }
        if (viewport_.pending(row)) {
            return nullptr;
        }
    }
    const auto* paragraph = viewport_.prepare(row, options);
    styles_[row] = options;
    return paragraph;
}

void TextSurface::poll() {
    viewport_.poll_layouts();
}

bool TextSurface::pending() const noexcept {
    return viewport_.layouts_pending();
}

ViewportCacheStatistics TextSurface::statistics() const noexcept {
    return viewport_.statistics();
}

std::optional<std::pair<std::size_t, std::size_t>> TextSurface::visible() const noexcept {
    return visible_;
}
} // namespace astra::ui
