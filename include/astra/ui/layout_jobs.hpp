#pragma once

#include <astra/ui/text_layout.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace astra::ui {
struct LayoutJobLimits {
    std::size_t jobs{16};
    // Accounted input/style and immutable seed payload, not native/GPU heaps.
    std::size_t bytes{64 * 1024 * 1024};
    std::size_t text_bytes{16 * 1024 * 1024};
    std::chrono::milliseconds timeout{30000};
};

struct LayoutJobStatistics {
    std::size_t jobs{}, bytes{}, running{}, submitted{}, completed{}, cancelled{}, expired{};
};

struct LayoutJobResult {
    std::shared_ptr<const TextLayout> paragraph;
    std::exception_ptr error;
};

// Main-thread admission/poll/consumption; one libuv work item runs at a time.
// Every job creates a private font collection and transfers its completed
// paragraph to the caller. No worker ever reuses a published paragraph's fonts.
class LayoutJobs final {
  public:
    using Ticket = std::uint64_t;
    explicit LayoutJobs(LayoutJobLimits = {});
    ~LayoutJobs();
    LayoutJobs(const LayoutJobs&) = delete;
    LayoutJobs& operator=(const LayoutJobs&) = delete;

    // Bounds are checked before copying. Full means backpressure, no mutation.
    // Style/Unicode errors and soft deadlines are returned through take(). A
    // native call cannot be interrupted, so expiry completes after it drains.
    [[nodiscard]] std::optional<Ticket> submit(std::string_view,
                                               const TextLayoutOptions&,
                                               TextSource previous = {},
                                               GlyphSource glyphs = {});
    void poll();
    [[nodiscard]] std::optional<LayoutJobResult> take(Ticket);
    void cancel(Ticket);
    // Nonblocking shutdown. Keep polling until idle before releasing the queue.
    // An active native shaping call is not preemptible; its result is discarded.
    void close();
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] LayoutJobStatistics statistics() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace astra::ui
