#pragma once

#include <astra/ui/text_layout.hpp>
#include <astra/ui/layout_jobs.hpp>
#include <astra/ui/viewport_index.hpp>

#include <cstddef>
#include <memory>
#include <vector>

namespace astra::ui {

struct ViewportCacheLimits {
    std::size_t entries{256};
    // Accounted UTF-8, coordinate/glyph and style payload, not native/GPU heap.
    std::size_t bytes{8 * 1024 * 1024};
};

struct ViewportCacheStatistics {
    std::size_t entries{};
    std::size_t accounted_bytes{};
    std::size_t shaped{};
    std::size_t reused{};
    std::size_t evicted{};
};

// UI-thread component. The engine must outlive it. Row snapshots and paragraph
// handles are immutable; layout keys have relative UTF-8 colors and all options.
class DocumentViewport final {
  public:
    explicit DocumentViewport(const TextLayoutEngine&,
                              ViewportCacheLimits = {},
                              std::shared_ptr<LayoutJobs> = {});
    ~DocumentViewport();
    DocumentViewport(DocumentViewport&&) noexcept;
    DocumentViewport& operator=(DocumentViewport&&) noexcept;
    DocumentViewport(const DocumentViewport&) = delete;
    DocumentViewport& operator=(const DocumentViewport&) = delete;

    void publish(std::vector<ViewportLine> lines);
    void invalidate_layouts();
    [[nodiscard]] const ViewportIndex& lines() const noexcept;
    [[nodiscard]] const TextLayout* layout(std::size_t number) const;
    [[nodiscard]] const TextLayout& shape(std::size_t number, const TextLayoutOptions&);
    // Short rows/cache hits are immediate. Cold long rows return null until
    // poll_layouts() transfers their completed paragraph onto the UI thread.
    [[nodiscard]] const TextLayout* prepare(std::size_t number, const TextLayoutOptions&);
    [[nodiscard]] bool pending(std::size_t number) const;
    [[nodiscard]] bool layouts_pending() const noexcept;
    void poll_layouts();
    [[nodiscard]] std::shared_ptr<const TextLayout> layout_snapshot(std::size_t number) const;
    [[nodiscard]] ViewportCacheStatistics statistics() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::ui
