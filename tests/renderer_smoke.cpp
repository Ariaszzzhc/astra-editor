#include "astra/ui/renderer.hpp"
#include "support/window_frame.hpp"
#include "../src/ui/window_frame_transaction.hpp"
#include "../src/ui/renderer_acceptance.hpp"

#include "include/codec/SkCodec.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

namespace {

void require(bool success, const char* operation) {
    if (!success) {
        throw std::runtime_error(operation);
    }
}

void frame_failure_paths() {
    using namespace astra::ui::detail;
    std::mt19937 random(0xA57A99);
    for (unsigned round = 0; round < 2000; ++round) {
        std::array<FrameAttribute, 4> attributes{};
        std::array<std::uint32_t, 4> values{};
        for (std::size_t index = 0; index < attributes.size(); ++index) {
            values[index] = random();
            attributes[index] = {
                static_cast<std::uint32_t>(index), random(), values[index], (random() % 3) != 0};
        }
        const auto original = values;
        const auto fail_at = random() % 5;
        bool failed{};
        const auto result =
            update_frame(attributes, [&](std::uint32_t id, std::uint32_t next) -> std::int32_t {
                values[id] = next;
                if (!failed && id == fail_at && next != original[id]) {
                    failed = true;
                    return -11;
                }
                return 0;
            });
        if (failed) {
            require(result.error == -11 && result.rollback_error == 0 && values == original,
                    "Failed native frame write did not restore the complete prior state");
        } else {
            for (std::size_t index = 0; index < values.size(); ++index)
                require(values[index] == (attributes[index].supported ? attributes[index].next
                                                                      : original[index]),
                        "Frame transaction changed unsupported attributes or lost the requested "
                        "palette");
        }
    }
    std::array attributes{FrameAttribute{0, 10, 1, true},
                          FrameAttribute{1, 20, 2, true},
                          FrameAttribute{2, 30, 3, true}};
    std::array<std::uint32_t, 3> values{1, 2, 3};
    const auto result =
        update_frame(attributes, [&](std::uint32_t id, std::uint32_t next) -> std::int32_t {
            if (id == 2 && next == 30) {
                values[id] = next;
                return -11;
            }
            if (id == 2 && next == 3)
                return -22;
            values[id] = next;
            return 0;
        });
    require(result.error == -11 && result.rollback_error == -22 && values[0] == 1 && values[1] == 2,
            "Rollback failure hid the error or prevented other attributes restoring");
    values = {1, 2, 3};
    unsigned refreshes{};
    const auto redraw_failure = update_frame(
        attributes,
        [&](std::uint32_t id, std::uint32_t next) -> std::int32_t {
            values[id] = next;
            return 0;
        },
        [&]() -> std::int32_t {
            return ++refreshes == 1 ? -33 : 0;
        });
    require(redraw_failure.error == -33 && redraw_failure.finalizing &&
                redraw_failure.rollback_error == 0 &&
                values == std::array<std::uint32_t, 3>{1, 2, 3} && refreshes == 2,
            "Failed frame redraw did not restore and redraw the old palette");
    const std::array probes{FrameAttribute{0, 1, 1, true, true},
                            FrameAttribute{1, 2, 2, true, false}};
    unsigned probe_writes{};
    const auto probe_result = update_frame(probes, [&](std::uint32_t id, std::uint32_t next) {
        require(id == 0 && next == 1, "Unchanged supported frame attribute was written");
        return ++probe_writes == 1 ? std::int32_t{-44} : std::int32_t{0};
    });
    require(probe_result.error == -44 && probe_result.rollback_error == 0 && probe_writes == 2,
            "Initial equal-value capability probe was skipped or not restored on failure");
    std::cout << "Window frame transaction: differential seed=0xA57A99, partial failure, "
                 "unsupported attributes and rollback failure: PASS\n";
}

void draw(astra::ui::Renderer& renderer, const std::filesystem::path& screenshot = {}) {
    renderer.poll_events([](const SDL_Event&) {
    });
    auto* canvas = renderer.begin_frame();
    require(canvas != nullptr, "Native surface acquisition failed");
    canvas->clear(SK_ColorRED);
    SkPaint paint;
    paint.setColor(SK_ColorGREEN);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, 64, 64), paint);
    if (!screenshot.empty()) {
        renderer.capture_png(screenshot);
    }
    renderer.end_frame();
}

void verify_png(const std::filesystem::path& screenshot, astra::ui::PixelSize size) {
    auto data = SkData::MakeFromFileName(screenshot.string().c_str());
    require(data != nullptr, "Screenshot is missing");
    auto codec = SkCodec::MakeFromData(std::move(data));
    require(codec != nullptr, "Screenshot is not a decodable PNG");
    require(codec->dimensions().width() == size.width &&
                codec->dimensions().height() == size.height,
            "Screenshot size differs from the native surface");
    const auto info =
        SkImageInfo::Make(size.width, size.height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    std::vector<std::uint8_t> pixels(info.computeMinByteSize());
    require(codec->getPixels(info, pixels.data(), info.minRowBytes()) == SkCodec::kSuccess,
            "Screenshot decoding failed");
    const auto last = pixels.size() - 4;
    require(pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0 && pixels[3] == 255 &&
                pixels[last] == 255 && pixels[last + 1] == 0 && pixels[last + 2] == 0 &&
                pixels[last + 3] == 255,
            "Native screenshot pixels differ from the rendered scene");
}

} // namespace

int main() {
    try {
        frame_failure_paths();
        const auto screenshot = std::filesystem::path("build/graphics/renderer-smoke.png");
        {
            astra::ui::Renderer renderer("Astra native renderer verification", 640, 480);
#if defined(_WIN32)
            const astra::ui::WindowFrameColors dark{0xFF18212A, 0xFFF0EDE8, 0xFF314457};
            const astra::ui::WindowFrameColors light{0xFFF1E8D6, 0xFF18212A, 0xFFB0A088};
            renderer.set_window_frame(dark);
            draw(renderer);
            const auto support = renderer.window_frame_support();
            std::cout << "DWM write support: dark=" << support.dark_mode
                      << ", caption=" << support.caption << ", text=" << support.text
                      << ", border=" << support.border << '\n';
            require(support.dark_mode && support.caption && support.text && support.border,
                    "This Windows frame acceptance requires supported DWM attributes");
            astra::test::verify_window_frame(renderer.window(), dark);
            astra::test::capture_window_frame(renderer.window(),
                                              "build/graphics/renderer-native-frame-dark.png");
            renderer.set_window_frame(light);
            astra::test::verify_window_frame(renderer.window(), light);
            astra::test::capture_window_frame(renderer.window(),
                                              "build/graphics/renderer-native-frame-light.png");
            const auto before = astra::test::read_window_frame(renderer.window());
            bool rejected{};
            try {
                renderer.set_window_frame({dark.caption, dark.text, 0x00112233});
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            require(rejected && astra::test::read_window_frame(renderer.window()) == before,
                    "Invalid frame palette changed native window state");
            renderer.set_window_frame(dark);
            renderer.set_window_frame(dark);
#endif
            require(SDL_TextInputActive(renderer.window()), "Editor text input is not active");
            renderer.set_text_input_area(SDL_Rect{32, 32, 2, 24}, 0);
            require(renderer.dpi_scale() > 0, "Window DPI scale is invalid");
            draw(renderer, screenshot);
            verify_png(screenshot, renderer.pixel_size());

            const auto original_window = renderer.window();
            const auto original_size = renderer.pixel_size();
            for (unsigned round = 0; round < 4; ++round) {
                // Odd rounds lose the real device while a recording is active.
                if ((round % 2) != 0) {
                    auto* canvas = renderer.begin_frame();
                    require(canvas != nullptr, "Pre-loss active frame acquisition failed");
                    canvas->clear(SK_ColorBLUE);
                }
                astra::ui::detail::RendererAcceptance::force_device_loss(renderer);
                require(astra::ui::detail::RendererAcceptance::native_device_lost(renderer),
                        "Dawn did not lose its actual owned device");
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!astra::ui::detail::RendererAcceptance::loss_callback_received(renderer)) {
                    require(std::chrono::steady_clock::now() < deadline,
                            "Actual Dawn loss callback did not arrive");
                    renderer.poll_events([](const SDL_Event&) {
                    });
                    SDL_Delay(1);
                }
                if ((round % 2) != 0) {
                    renderer.end_frame();
                }
                draw(renderer, screenshot);
                verify_png(screenshot, original_size);
                require(
                    !astra::ui::detail::RendererAcceptance::native_device_lost(renderer) &&
                        !astra::ui::detail::RendererAcceptance::loss_callback_received(renderer),
                    "Frame recovery retained the lost native device or callback state");
                require(renderer.window() == original_window &&
                            renderer.pixel_size() == original_size &&
                            SDL_TextInputActive(renderer.window()),
                        "GPU loss changed window identity, size or text-input lifetime");
#if defined(_WIN32)
                astra::test::verify_window_frame(renderer.window(), dark);
#endif
            }
            std::cout << "Actual owned Dawn device loss: callback, idle/active frame recovery, "
                         "GPU pixels and native window/input preservation (four rounds): PASS\n";

            require(SDL_SetWindowSize(renderer.window(), 800, 600), "Native resize failed");
            require(SDL_SyncWindow(renderer.window()), "Native resize synchronization failed");
            draw(renderer, screenshot);
            const auto resized = renderer.pixel_size();
            require(resized.width == 800 && resized.height == 600,
                    "Native resize dimensions differ");
            verify_png(screenshot, resized);

            require(SDL_MinimizeWindow(renderer.window()), "Native minimize failed");
            require(SDL_SyncWindow(renderer.window()), "Native minimize synchronization failed");
            renderer.poll_events([](const SDL_Event&) {
            });
            require(renderer.begin_frame() == nullptr, "Minimized window acquired a texture");
            renderer.end_frame();

            require(SDL_RestoreWindow(renderer.window()), "Native restore failed");
            require(SDL_SyncWindow(renderer.window()), "Native restore synchronization failed");
            draw(renderer);
#if defined(_WIN32)
            astra::test::verify_window_frame(renderer.window(), dark);
            std::cout << "Actual Windows DWM frame: dark/light caption, foreground, border, "
                         "invalid palette preservation and minimize/restore lifetime: PASS\n";
#endif
        }
        require((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0,
                "Renderer leaked SDL video lifetime");
        // A fresh instance checks teardown of the old native surface and device.
        {
            astra::ui::Renderer renderer("Astra renderer restart verification", 320, 240);
            for (unsigned frame = 0; frame < 8; ++frame)
                draw(renderer);
        }
        // Teardown also owns a frame whose recording has never been submitted.
        {
            astra::ui::Renderer renderer("Astra abandoned frame verification", 320, 240);
            auto* canvas = renderer.begin_frame();
            require(canvas != nullptr, "Abandoned frame acquisition failed");
            canvas->clear(SK_ColorRED);
        }
        {
            astra::ui::Renderer renderer("Astra restart after abandoned frame", 320, 240);
            draw(renderer, screenshot);
            verify_png(screenshot, renderer.pixel_size());
        }
        require((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0,
                "Repeated teardown leaked SDL video lifetime");
        std::cout << "SDL3 native window + Graphite/Dawn present, GPU PNG, resize, minimize, IME "
                     "lifetime, restart: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Renderer smoke failed: " << error.what() << '\n';
        return 1;
    }
}
