#pragma once

#include <astra/ui/view_index.hpp>
#include <cstddef>
#include <memory>
#include <optional>

namespace astra::ui {

// Pure memory reclamation: no SDL, JS, service or I/O access on the worker.
// Reservations cover current sources, builders and retired projections so that
// cancel/publication never has to allocate a queue entry or free a large graph.
class ViewRetirement final {
    struct Impl;

  public:
    struct Payload {
        std::optional<ViewBuilder> builder;
        std::shared_ptr<const ViewSource> source;
        std::optional<ViewIndex> projection;
        ViewSnapshot rows;
    };

    struct Stats {
        std::size_t capacity{}, reserved{}, queued{}, bytes{}, completed{};
    };

    class Reservation final {
      public:
        Reservation() = default;
        ~Reservation();
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept {
            return owner_ != nullptr;
        }

        void charge(std::size_t bytes);
        void undo_charge(std::size_t bytes) noexcept;

      private:
        friend class ViewRetirement;
        Reservation(Impl& owner, std::size_t index) noexcept;
        void release() noexcept;
        Impl* owner_{}; // Borrowed; reservations must not outlive the queue.
        std::size_t index_{};
    };

    explicit ViewRetirement(std::size_t capacity = 48,
                            std::size_t byte_capacity = 256 * 1024 * 1024);
    ~ViewRetirement();
    ViewRetirement(const ViewRetirement&) = delete;
    ViewRetirement& operator=(const ViewRetirement&) = delete;
    [[nodiscard]] std::optional<Reservation> try_reserve();
    [[nodiscard]] Reservation reserve();
    void retire(Reservation reservation, Payload payload) noexcept;
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] bool idle() const;

  private:
    std::unique_ptr<Impl> impl_;
};
} // namespace astra::ui
