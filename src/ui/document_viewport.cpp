#include <astra/ui/document_viewport.hpp>

#include <functional>
#include <iterator>
#include <list>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace astra::ui {

struct DocumentViewport::Impl {
    struct Entry {
        std::size_t hash{};
        std::size_t bytes{};
        TextLayoutOptions options;
        std::shared_ptr<const TextLayout> paragraph;
    };

    using Entries = std::list<Entry>;
    const TextLayoutEngine& engine;
    ViewportCacheLimits limits;
    ViewportCacheStatistics stats;
    ViewportIndex lines;
    std::map<std::size_t, std::shared_ptr<const TextLayout>> current;
    Entries recent;
    std::multimap<std::size_t, Entries::iterator> lookup;

    Impl(const TextLayoutEngine& engine, ViewportCacheLimits limits)
        : engine(engine), limits(limits) {
        if (limits.entries > 4096 || limits.bytes > 256 * 1024 * 1024)
            throw std::invalid_argument("Viewport cache exceeds bounded limits");
    }

    static std::size_t accounted(std::string_view text, const TextLayoutOptions& options) {
        std::size_t bytes = sizeof(Entry);
        const auto add = [&](std::size_t amount) {
            if (amount > std::numeric_limits<std::size_t>::max() - bytes)
                throw std::length_error("Viewport cache accounting overflow");
            bytes += amount;
        };
        add(text.size());
        add(options.locale.size());
        for (const auto& family : options.font_families) {
            add(sizeof(std::string));
            add(family.size());
        }
        if (options.color_spans.size() >
            std::numeric_limits<std::size_t>::max() / sizeof(TextColorSpan))
            throw std::length_error("Viewport color span accounting overflow");
        add(options.color_spans.size() * sizeof(TextColorSpan));
        return bytes;
    }

    void evict() {
        const auto oldest = std::prev(recent.end());
        const auto [begin, end] = lookup.equal_range(oldest->hash);
        for (auto found = begin; found != end; ++found) {
            if (found->second == oldest) {
                lookup.erase(found);
                break;
            }
        }
        stats.accounted_bytes -= oldest->bytes;
        recent.erase(oldest);
        ++stats.evicted;
    }

    std::shared_ptr<const TextLayout> get(std::string_view text, const TextLayoutOptions& options) {
        const auto hash = std::hash<std::string_view>{}(text);
        const auto [begin, end] = lookup.equal_range(hash);
        for (auto found = begin; found != end; ++found) {
            auto entry = found->second;
            // Hashing only selects candidates. Exact text/style certify reuse.
            if (entry->paragraph->text() == text && entry->options == options) {
                auto paragraph = entry->paragraph;
                recent.splice(recent.begin(), recent, entry);
                ++stats.reused;
                return paragraph;
            }
        }
        const auto bytes = accounted(text, options);
        auto paragraph =
            std::make_shared<const TextLayout>(engine.layout(std::string(text), options));
        ++stats.shaped;
        if (!limits.entries || bytes > limits.bytes)
            return paragraph;
        recent.push_front({hash, bytes, options, paragraph});
        try {
            lookup.emplace(hash, recent.begin());
        } catch (...) {
            recent.pop_front();
            throw;
        }
        stats.accounted_bytes += bytes;
        while (recent.size() > limits.entries || stats.accounted_bytes > limits.bytes)
            evict();
        return paragraph;
    }
};

DocumentViewport::DocumentViewport(const TextLayoutEngine& engine, ViewportCacheLimits limits)
    : impl_(std::make_unique<Impl>(engine, limits)) {
}

DocumentViewport::~DocumentViewport() = default;
DocumentViewport::DocumentViewport(DocumentViewport&&) noexcept = default;
DocumentViewport& DocumentViewport::operator=(DocumentViewport&&) noexcept = default;

void DocumentViewport::publish(std::vector<ViewportLine> lines) {
    impl_->lines.reset(std::move(lines));
    invalidate_layouts();
}

void DocumentViewport::invalidate_layouts() {
    impl_->current.clear();
}

const ViewportIndex& DocumentViewport::lines() const noexcept {
    return impl_->lines;
}

const TextLayout* DocumentViewport::layout(std::size_t number) const {
    const auto found = impl_->current.find(number);
    return found == impl_->current.end() ? nullptr : found->second.get();
}

const TextLayout& DocumentViewport::shape(std::size_t number, const TextLayoutOptions& options) {
    const auto* line = impl_->lines.find(number);
    if (!line)
        throw std::out_of_range("Cannot shape a line outside the published viewport");
    auto paragraph = impl_->get(line->text, options);
    auto& current = impl_->current[number];
    current = std::move(paragraph);
    return *current;
}

std::shared_ptr<const TextLayout> DocumentViewport::layout_snapshot(std::size_t number) const {
    const auto found = impl_->current.find(number);
    return found == impl_->current.end() ? nullptr : found->second;
}

ViewportCacheStatistics DocumentViewport::statistics() const noexcept {
    auto stats = impl_->stats;
    stats.entries = impl_->recent.size();
    return stats;
}

} // namespace astra::ui
