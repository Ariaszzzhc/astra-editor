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
        std::size_t number{};
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

    struct Pending {
        TextLayoutOptions options;
        std::optional<LayoutJobs::Ticket> ticket;
        std::exception_ptr error;
    };

    std::shared_ptr<LayoutJobs> jobs;
    std::map<std::size_t, Pending> pending;

    Impl(const TextLayoutEngine& engine,
         ViewportCacheLimits limits,
         std::shared_ptr<LayoutJobs> jobs)
        : engine(engine), limits(limits), jobs(std::move(jobs)) {
        if (limits.entries > 4096 || limits.bytes > 256 * 1024 * 1024)
            throw std::invalid_argument("Viewport cache exceeds bounded limits");
    }

    ~Impl() {
        cancel();
    }

    void cancel() {
        for (const auto& [number, value] : pending) {
            static_cast<void>(number);
            if (value.ticket) {
                jobs->cancel(*value.ticket);
            }
        }
        pending.clear();
    }

    static std::size_t accounted(std::string_view text, const TextLayoutOptions& options) {
        std::size_t bytes = sizeof(Entry);
        const auto add = [&](std::size_t amount) {
            if (amount > std::numeric_limits<std::size_t>::max() - bytes)
                throw std::length_error("Viewport cache accounting overflow");
            bytes += amount;
        };
        add(text.size());
        add(text.size()); // Immutable coordinate Rope's bounded chunk payload.
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

    std::shared_ptr<const TextLayout>
    find(std::string_view text, const TextLayoutOptions& options, std::size_t number) {
        const auto hash = std::hash<std::string_view>{}(text);
        const auto [begin, end] = lookup.equal_range(hash);
        for (auto found = begin; found != end; ++found) {
            auto entry = found->second;
            // Hashing only selects candidates. Exact text/style certify reuse.
            if (entry->paragraph->text() == text && entry->options == options) {
                auto paragraph = entry->paragraph;
                entry->number = number;
                recent.splice(recent.begin(), recent, entry);
                ++stats.reused;
                return paragraph;
            }
        }
        return {};
    }

    void store(const std::shared_ptr<const TextLayout>& paragraph,
               const TextLayoutOptions& options,
               std::size_t number) {
        const auto text = paragraph->text();
        const auto hash = std::hash<std::string_view>{}(text);
        const auto bytes = accounted(text, options);
        ++stats.shaped;
        if (!limits.entries || bytes > limits.bytes)
            return;
        recent.push_front({hash, number, bytes, options, paragraph});
        try {
            lookup.emplace(hash, recent.begin());
        } catch (...) {
            recent.pop_front();
            throw;
        }
        stats.accounted_bytes += bytes;
        while (recent.size() > limits.entries || stats.accounted_bytes > limits.bytes)
            evict();
    }

    TextSource seed(std::size_t number) const {
        for (const auto& entry : recent)
            if (entry.number == number)
                return entry.paragraph->source_snapshot();
        return {};
    }

    std::shared_ptr<const TextLayout>
    get(std::string_view text, const TextLayoutOptions& options, std::size_t number) {
        if (auto cached = find(text, options, number)) {
            return cached;
        }
        auto paragraph = std::make_shared<const TextLayout>(
            engine.layout(std::string(text), options, seed(number)));
        store(paragraph, options, number);
        return paragraph;
    }
};

DocumentViewport::DocumentViewport(const TextLayoutEngine& engine,
                                   ViewportCacheLimits limits,
                                   std::shared_ptr<LayoutJobs> jobs)
    : impl_(std::make_unique<Impl>(engine, limits, std::move(jobs))) {
}

DocumentViewport::~DocumentViewport() = default;
DocumentViewport::DocumentViewport(DocumentViewport&&) noexcept = default;
DocumentViewport& DocumentViewport::operator=(DocumentViewport&&) noexcept = default;

void DocumentViewport::publish(std::vector<ViewportLine> lines) {
    impl_->lines.reset(std::move(lines));
    invalidate_layouts();
}

void DocumentViewport::invalidate_layouts() {
    impl_->cancel();
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
    if (const auto found = impl_->pending.find(number); found != impl_->pending.end()) {
        if (found->second.ticket) {
            impl_->jobs->cancel(*found->second.ticket);
        }
        impl_->pending.erase(found);
    }
    auto paragraph = impl_->get(line->text, options, number);
    auto& current = impl_->current[number];
    current = std::move(paragraph);
    return *current;
}

const TextLayout* DocumentViewport::prepare(std::size_t number, const TextLayoutOptions& options) {
    const auto* line = impl_->lines.find(number);
    if (!line) {
        throw std::out_of_range("Cannot prepare a line outside the published viewport");
    }
    if (!impl_->jobs || line->text.size() < 4096) {
        return &shape(number, options);
    }
    if (auto cached = impl_->find(line->text, options, number)) {
        if (const auto found = impl_->pending.find(number); found != impl_->pending.end()) {
            if (found->second.ticket) {
                impl_->jobs->cancel(*found->second.ticket);
            }
            impl_->pending.erase(found);
        }
        impl_->current[number] = std::move(cached);
        return layout(number);
    }
    auto found = impl_->pending.find(number);
    if (found != impl_->pending.end() && found->second.options != options) {
        if (found->second.ticket) {
            impl_->jobs->cancel(*found->second.ticket);
        }
        impl_->pending.erase(found);
        found = impl_->pending.end();
    }
    if (found == impl_->pending.end()) {
        found = impl_->pending.emplace(number, Impl::Pending{options, {}, {}}).first;
    }
    impl_->current.erase(number);
    auto& value = found->second;
    if (value.error) {
        std::rethrow_exception(value.error);
    }
    if (!value.ticket) {
        value.ticket = impl_->jobs->submit(line->text, value.options, impl_->seed(number));
    }
    return nullptr;
}

bool DocumentViewport::pending(std::size_t number) const {
    const auto found = impl_->pending.find(number);
    return found != impl_->pending.end() && !found->second.error;
}

bool DocumentViewport::layouts_pending() const noexcept {
    for (const auto& [number, value] : impl_->pending) {
        static_cast<void>(number);
        if (!value.error) {
            return true;
        }
    }
    return false;
}

void DocumentViewport::poll_layouts() {
    for (auto it = impl_->pending.begin(); it != impl_->pending.end();) {
        auto& [number, value] = *it;
        if (value.error) {
            ++it;
            continue;
        }
        const auto* line = impl_->lines.find(number);
        if (!line) {
            throw std::logic_error("Pending layout lost its viewport snapshot");
        }
        if (!value.ticket) {
            value.ticket = impl_->jobs->submit(line->text, value.options, impl_->seed(number));
        }
        auto result = value.ticket ? impl_->jobs->take(*value.ticket) : std::nullopt;
        if (!result) {
            ++it;
            continue;
        }
        value.ticket.reset();
        if (result->error) {
            value.error = result->error;
            ++it;
            continue;
        }
        if (!result->paragraph) {
            throw std::logic_error("Layout job completed without a paragraph");
        }
        impl_->store(result->paragraph, value.options, number);
        impl_->current[number] = std::move(result->paragraph);
        it = impl_->pending.erase(it);
    }
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
