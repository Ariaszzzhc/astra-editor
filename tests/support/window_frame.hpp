#pragma once

#include <astra/ui/renderer.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include "include/core/SkImageInfo.h"
#include "include/core/SkData.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dwmapi.h>

namespace astra::test {
// Test-only capture of the SDL-owned window, including its OS title bar.
// Production GPU capture intentionally contains only the client surface.
struct WindowPixels {
    int width{}, height{}, caption_height{};
    std::vector<std::uint32_t> pixels;
};

WindowPixels capture_window_pixels(SDL_Window* window);

inline void capture_window_frame(SDL_Window* window, const std::filesystem::path& destination) {
    const auto captured = capture_window_pixels(window);
    const SkPixmap pixmap(
        SkImageInfo::Make(
            captured.width, captured.height, kBGRA_8888_SkColorType, kPremul_SkAlphaType),
        captured.pixels.data(),
        static_cast<std::size_t>(captured.width) * 4);
    SkDynamicMemoryWStream encoded;
    if (!SkPngEncoder::Encode(&encoded, pixmap, {}))
        throw std::runtime_error("Cannot encode native frame capture");
    const auto data = encoded.detachAsData();
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output)
        throw std::runtime_error("Cannot open native frame capture output");
    output.write(static_cast<const char*>(data->data()),
                 static_cast<std::streamsize>(data->size()));
    output.close();
    if (!output)
        throw std::runtime_error("Cannot write native frame capture output");
}

struct WindowFrameSnapshot {
    std::uint32_t dark_mode{};
    int width{}, height{};
    std::vector<std::uint32_t> pixels;
    friend bool operator==(const WindowFrameSnapshot&, const WindowFrameSnapshot&) = default;
};

inline WindowFrameSnapshot read_window_frame(SDL_Window* window) {
    const auto native = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!native)
        throw std::runtime_error("Native frame acceptance requires a Win32 window");
    WindowFrameSnapshot result;
    if (FAILED(DwmGetWindowAttribute(
            native, DWMWA_USE_IMMERSIVE_DARK_MODE, &result.dark_mode, sizeof(result.dark_mode))))
        throw std::runtime_error("Cannot read actual DWM dark-mode acceptance attribute");
    const auto captured = capture_window_pixels(window);
    result.width = captured.width;
    result.height = captured.caption_height;
    result.pixels.assign(captured.pixels.begin(),
                         captured.pixels.begin() +
                             static_cast<std::size_t>(result.width) * result.height);
    return result;
}

inline void verify_window_frame(SDL_Window* window, astra::ui::WindowFrameColors colors) {
    WindowFrameSnapshot actual;
    const auto weighted = 299 * ((colors.caption >> 16) & 255) +
                          587 * ((colors.caption >> 8) & 255) + 114 * (colors.caption & 255);
    const auto matches = [&](std::uint32_t color) {
        return std::count(actual.pixels.begin(), actual.pixels.end(), color);
    };
    const auto deadline = SDL_GetTicks() + 2000;
    do {
        actual = read_window_frame(window);
        if (actual.dark_mode == static_cast<std::uint32_t>(weighted < 128000) &&
            matches(colors.caption) >= actual.width && matches(colors.text) > 0 &&
            matches(colors.border) > 0)
            return;
        SDL_Delay(1);
    } while (SDL_GetTicks() < deadline);
    throw std::runtime_error("Actual OS frame differs from the compiled JS palette: caption=" +
                             std::to_string(matches(colors.caption)) +
                             ", text=" + std::to_string(matches(colors.text)) +
                             ", border=" + std::to_string(matches(colors.border)) +
                             ", dark=" + std::to_string(actual.dark_mode));
}
} // namespace astra::test
#endif
