#include <astra/ui/icons.hpp>
#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::size_t painted(const SkBitmap& bitmap) {
    std::size_t count{};
    for (int y = 0; y < bitmap.height(); ++y) {
        for (int x = 0; x < bitmap.width(); ++x) {
            count += SkColorGetA(bitmap.getColor(x, y)) > 0;
        }
    }
    return count;
}
} // namespace

int main() {
    try {
        astra::ui::IconRenderer icons;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!icons.ready() && std::chrono::steady_clock::now() < deadline) {
            icons.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(icons.ready(), "Icon preparation did not finish before its deadline");
        const auto before = icons.bytes();
        require(before > 0 && before <= 32 * 1024 * 1024, "Icon cache budget");
        SkBitmap bitmap;
        bitmap.allocN32Pixels(48, 48);
        SkCanvas canvas(bitmap);
        const auto saves = canvas.getSaveCount();
        for (const auto& resource : astra::ui::icon_resources()) {
            const auto asset = astra::ui::icon_asset(resource.id);
            require(asset.has_value(), "Inventory ID did not resolve");
            for (const auto size : {16.0F, 20.0F, 24.0F, 32.0F}) {
                bitmap.eraseColor(SK_ColorTRANSPARENT);
                icons.draw(&canvas, *asset, 4, 4, size, SK_ColorRED);
                if (painted(bitmap) == 0) {
                    throw std::runtime_error("Empty SVG at " + std::to_string(size) + ": " +
                                             std::string(resource.id));
                }
                for (int y = 0; y < bitmap.height(); ++y) {
                    for (int x = 0; x < bitmap.width(); ++x) {
                        if (x < 4 || y < 4 || x >= 4 + size || y >= 4 + size) {
                            require(SkColorGetA(bitmap.getColor(x, y)) == 0,
                                    "Icon escaped viewport");
                        }
                    }
                }
                require(canvas.getSaveCount() == saves, "Icon leaked canvas state");
            }
        }
        const auto tint = [&](astra::ui::IconAsset asset, SkColor color) {
            bitmap.eraseColor(SK_ColorTRANSPARENT);
            icons.draw(&canvas, asset, 0, 0, 24, color);
            for (int y = 0; y < 24; ++y) {
                for (int x = 0; x < 24; ++x) {
                    const auto pixel = bitmap.getColor(x, y);
                    if (SkColorGetA(pixel) > 128) {
                        return pixel;
                    }
                }
            }
            throw std::runtime_error("Missing opaque icon pixels");
        };
        const auto search = astra::ui::icon_asset(astra::ui::Icon::search);
        require(SkColorGetR(tint(search, SK_ColorRED)) == 255, "UI red tint");
        require(SkColorGetB(tint(search, SK_ColorBLUE)) == 255, "UI blue tint");
        const auto python = *astra::ui::icon_asset("material/python");
        require(tint(python, SK_ColorRED) == tint(python, SK_ColorBLUE),
                "Language colors were tinted");
        require(before == icons.bytes(), "Repainting changed the vector cache");
        require(!astra::ui::icon_asset("material/../python"), "Resource traversal admitted");
        for (int i = 0; i < 3; ++i) {
            astra::ui::IconRenderer cancelled;
        }
        std::cout << "Rendered " << astra::ui::icon_resources().size()
                  << " icons at four sizes; cache " << before
                  << " bytes; tint and shutdown passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
