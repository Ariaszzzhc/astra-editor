#include "astra/ui/renderer.hpp"

#include "include/codec/SkCodec.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool success, const char* operation) {
    if (!success) { throw std::runtime_error(operation); }
}

void draw(astra::ui::Renderer& renderer, const std::filesystem::path& screenshot = {}) {
    renderer.poll_events([](const SDL_Event&) {});
    auto* canvas = renderer.begin_frame();
    require(canvas != nullptr, "Native surface acquisition failed");
    canvas->clear(SK_ColorRED);
    SkPaint paint;
    paint.setColor(SK_ColorGREEN);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, 64, 64), paint);
    if (!screenshot.empty()) { renderer.capture_png(screenshot); }
    renderer.end_frame();
}

void verify_png(const std::filesystem::path& screenshot, astra::ui::PixelSize size) {
    auto data = SkData::MakeFromFileName(screenshot.string().c_str());
    require(data != nullptr, "Screenshot is missing");
    auto codec = SkCodec::MakeFromData(std::move(data));
    require(codec != nullptr, "Screenshot is not a decodable PNG");
    require(codec->dimensions().width() == size.width && codec->dimensions().height() == size.height,
            "Screenshot size differs from the native surface");
    const auto info = SkImageInfo::Make(size.width, size.height, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    std::vector<std::uint8_t> pixels(info.computeMinByteSize());
    require(codec->getPixels(info, pixels.data(), info.minRowBytes()) == SkCodec::kSuccess,
            "Screenshot decoding failed");
    const auto last = pixels.size() - 4;
    require(pixels[0] == 0 && pixels[1] == 255 && pixels[2] == 0 && pixels[3] == 255 &&
            pixels[last] == 255 && pixels[last + 1] == 0 && pixels[last + 2] == 0 && pixels[last + 3] == 255,
            "Native screenshot pixels differ from the rendered scene");
}

} // namespace

int main() {
    try {
        const auto screenshot = std::filesystem::path("build/graphics/renderer-smoke.png");
        {
            astra::ui::Renderer renderer("Astra native renderer verification", 640, 480);
            require(SDL_TextInputActive(renderer.window()), "Editor text input is not active");
            renderer.set_text_input_area(SDL_Rect{32, 32, 2, 24}, 0);
            require(renderer.dpi_scale() > 0, "Window DPI scale is invalid");
            draw(renderer, screenshot);
            verify_png(screenshot, renderer.pixel_size());

            require(SDL_SetWindowSize(renderer.window(), 800, 600), "Native resize failed");
            require(SDL_SyncWindow(renderer.window()), "Native resize synchronization failed");
            draw(renderer, screenshot);
            const auto resized = renderer.pixel_size();
            require(resized.width == 800 && resized.height == 600, "Native resize dimensions differ");
            verify_png(screenshot, resized);

            require(SDL_MinimizeWindow(renderer.window()), "Native minimize failed");
            require(SDL_SyncWindow(renderer.window()), "Native minimize synchronization failed");
            renderer.poll_events([](const SDL_Event&) {});
            require(renderer.begin_frame() == nullptr, "Minimized window acquired a texture");
            renderer.end_frame();

            require(SDL_RestoreWindow(renderer.window()), "Native restore failed");
            require(SDL_SyncWindow(renderer.window()), "Native restore synchronization failed");
            draw(renderer);
        }
        require((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0, "Renderer leaked SDL video lifetime");
        // A fresh instance checks teardown of the old native surface and device.
        { astra::ui::Renderer renderer("Astra renderer restart verification", 320, 240);
            for(unsigned frame=0;frame<8;++frame)draw(renderer);
        }
        // Teardown also owns a frame whose recording has never been submitted.
        { astra::ui::Renderer renderer("Astra abandoned frame verification",320,240);
            auto* canvas=renderer.begin_frame();require(canvas!=nullptr,"Abandoned frame acquisition failed");
            canvas->clear(SK_ColorRED);
        }
        { astra::ui::Renderer renderer("Astra restart after abandoned frame",320,240);
            draw(renderer,screenshot);verify_png(screenshot,renderer.pixel_size());
        }
        require((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0, "Repeated teardown leaked SDL video lifetime");
        std::cout << "SDL3 native window + Graphite/Dawn present, GPU PNG, resize, minimize, IME lifetime, restart: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Renderer smoke failed: " << error.what() << '\n';
        return 1;
    }
}
