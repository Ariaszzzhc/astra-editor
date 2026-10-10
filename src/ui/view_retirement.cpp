#include <astra/ui/view_retirement.hpp>

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace astra::ui {
struct ViewRetirement::Impl {
    enum class State { free, reserved, queued, working };

    struct Slot {
        State state{State::free};
        std::size_t bytes{};
        std::optional<Payload> payload;
    };

    explicit Impl(std::size_t capacity, std::size_t bytes)
        : slots(capacity), byte_capacity(bytes), worker([this] {
              run();
          }) {
    }

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        worker.join();
    }

    void release(std::size_t index) noexcept {
        std::lock_guard lock(mutex);
        auto& slot = slots[index];
        assert(slot.state == State::reserved);
        retained_bytes -= slot.bytes;
        slot.bytes = 0;
        slot.state = State::free;
        --reserved;
    }

    void run() noexcept {
        std::unique_lock lock(mutex);
        for (;;) {
            changed.wait(lock, [this] {
                return stopping || queued != 0;
            });
            if (!queued && stopping)
                return;
            const auto pending = [](const auto& slot) {
                return slot.state == State::queued;
            };
            const auto next = slots.begin() + static_cast<std::ptrdiff_t>(next_slot);
            auto found = std::find_if(next, slots.end(), pending);
            if (found == slots.end())
                found = std::find_if(slots.begin(), next, pending);
            next_slot = (static_cast<std::size_t>(found - slots.begin()) + 1) % slots.size();
            found->state = State::working;
            auto payload = std::move(*found->payload);
            found->payload.reset();
            lock.unlock();
            // Destruct every owning edge here, including unfiltered source maps,
            // abandoned builders and projections that share immutable leaves.
            payload = Payload{};
            lock.lock();
            retained_bytes -= found->bytes;
            found->bytes = 0;
            found->state = State::free;
            --queued;
            --reserved;
            ++completed;
        }
    }

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<Slot> slots;
    std::size_t byte_capacity{}, retained_bytes{}, reserved{}, queued{}, completed{}, next_slot{};
    bool stopping{};
    std::jthread worker;
};

ViewRetirement::Reservation::Reservation(Impl& owner, std::size_t index) noexcept
    : owner_(&owner), index_(index) {
}

ViewRetirement::Reservation::~Reservation() {
    release();
}

ViewRetirement::Reservation::Reservation(Reservation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), index_(other.index_) {
}

ViewRetirement::Reservation& ViewRetirement::Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) {
        release();
        owner_ = std::exchange(other.owner_, nullptr);
        index_ = other.index_;
    }
    return *this;
}

void ViewRetirement::Reservation::release() noexcept {
    if (owner_)
        std::exchange(owner_, nullptr)->release(index_);
}

void ViewRetirement::Reservation::charge(std::size_t bytes) {
    if (!owner_)
        throw std::logic_error("Missing view retirement reservation");
    std::lock_guard lock(owner_->mutex);
    if (bytes > owner_->byte_capacity - owner_->retained_bytes)
        throw protocol::RpcError(-32004, "View refresh memory capacity exceeded; retry later");
    owner_->slots[index_].bytes += bytes;
    owner_->retained_bytes += bytes;
}

void ViewRetirement::Reservation::undo_charge(std::size_t bytes) noexcept {
    assert(owner_);
    std::lock_guard lock(owner_->mutex);
    assert(bytes <= owner_->slots[index_].bytes);
    owner_->slots[index_].bytes -= bytes;
    owner_->retained_bytes -= bytes;
}

ViewRetirement::ViewRetirement(std::size_t capacity, std::size_t byte_capacity) {
    if (!capacity || capacity > 64 || !byte_capacity)
        throw std::invalid_argument("Invalid view retirement capacity");
    impl_ = std::make_unique<Impl>(capacity, byte_capacity);
}

ViewRetirement::~ViewRetirement() = default;

std::optional<ViewRetirement::Reservation> ViewRetirement::try_reserve() {
    std::lock_guard lock(impl_->mutex);
    for (std::size_t index = 0; index < impl_->slots.size(); ++index) {
        auto& slot = impl_->slots[index];
        if (slot.state == Impl::State::free) {
            slot.state = Impl::State::reserved;
            ++impl_->reserved;
            return Reservation(*impl_, index);
        }
    }
    return std::nullopt;
}

ViewRetirement::Reservation ViewRetirement::reserve() {
    auto reservation = try_reserve();
    if (!reservation)
        throw protocol::RpcError(-32004, "Too many view refreshes are retiring; retry later");
    return std::move(*reservation);
}

void ViewRetirement::retire(Reservation reservation, Payload payload) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<Payload>);
    static_assert(std::is_nothrow_move_assignable_v<Payload>);
    assert(reservation.owner_ == impl_.get());
    {
        std::lock_guard lock(impl_->mutex);
        auto& slot = impl_->slots[reservation.index_];
        slot.payload.emplace(std::move(payload));
        slot.state = Impl::State::queued;
        ++impl_->queued;
        reservation.owner_ = nullptr;
    }
    impl_->changed.notify_one();
}

ViewRetirement::Stats ViewRetirement::stats() const {
    std::lock_guard lock(impl_->mutex);
    return {impl_->slots.size(),
            impl_->reserved,
            impl_->queued,
            impl_->retained_bytes,
            impl_->completed};
}

bool ViewRetirement::idle() const {
    return stats().queued == 0;
}
} // namespace astra::ui
