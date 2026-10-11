#pragma once

#include <SDL3/SDL.h>

#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class SkCanvas;

namespace astra::ui {

namespace detail {
struct RendererAcceptance;
}

struct PixelSize {
    int width{};
    int height{};
    bool operator==(const PixelSize&) const = default;
};

struct WindowFrameColors {
    // Opaque ARGB, matching the compiled JS workbench palette.
    std::uint32_t caption{}, text{}, border{};
    friend bool operator==(const WindowFrameColors&, const WindowFrameColors&) = default;
};

struct WindowFrameSupport {
    bool dark_mode{}, caption{}, text{}, border{};
};

// Main-thread owner of the native window, IME, and Graphite/Dawn rendering state.
// Errors are reported as exceptions; callers retain ownership of document state.
class Renderer final {
  public:
    explicit Renderer(std::string title = "Astra", int width = 1280, int height = 800);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    Renderer(Renderer&&) = delete;
    Renderer& operator=(Renderer&&) = delete;

    // Events are consumed synchronously: SDL owns pointers in text/drop events.
    void poll_events(const std::function<void(const SDL_Event&)>& handler);
    [[nodiscard]] SDL_Window* window() const noexcept;
    [[nodiscard]] PixelSize pixel_size() const;
    [[nodiscard]] float dpi_scale() const;

    // Applies supported OS-owned frame attributes transactionally. Unsupported
    // attributes remain under OS control and are reported explicitly.
    void set_window_frame(WindowFrameColors);
    [[nodiscard]] WindowFrameSupport window_frame_support() const noexcept;

    // Canvas coordinates are physical pixels. A null canvas means that this frame
    // must be skipped (minimized, zero-sized, or an acquisition timeout).
    [[nodiscard]] SkCanvas* begin_frame();
    void end_frame();

    // Call after drawing and before end_frame. This performs real GPU readback.
    void capture_png(const std::filesystem::path& destination);

    // SDL expects window coordinates, which can differ from physical pixels.
    void set_text_input_area(const SDL_Rect& area, int cursor_offset);
    // Dismiss the OS composition without disabling text input.
    void clear_composition();

  private:
    friend struct detail::RendererAcceptance;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::ui
