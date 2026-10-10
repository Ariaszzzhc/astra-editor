#pragma once

#include <SDL3/SDL.h>

#include <cstddef>
#include <deque>
#include <string>

namespace astra::ui {
// Owns SDL text payloads while geometry-dependent input waits. Rebind pointers
// with get() only for the duration of processing the returned stored event.
struct StoredInputEvent {
    SDL_Event event{};
    SDL_Keymod modifiers{};
    std::string text;
    bool has_text{};
    [[nodiscard]] SDL_Event get() const noexcept;
};

class InputEvents final {
  public:
    static constexpr std::size_t max_events = 256;
    static constexpr std::size_t max_bytes = 64 * 1024;
    static constexpr std::size_t max_text_bytes = 16 * 1024;
    [[nodiscard]] static bool accepts(Uint32 type) noexcept;
    // Adjacent motion packets coalesce; transitions and text retain FIFO order.
    // Capacity/ownership failures preserve all already accepted events.
    void push(const SDL_Event&, SDL_Keymod);
    [[nodiscard]] StoredInputEvent take();
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t bytes() const noexcept;

  private:
    std::deque<StoredInputEvent> events_;
    std::size_t bytes_{};
};
} // namespace astra::ui
