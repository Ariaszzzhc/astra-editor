#include <astra/ui/icons.hpp>

#include "include/core/SkCanvas.h"
#include "include/core/SkColorFilter.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPicture.h"
#include "include/core/SkPictureRecorder.h"
#include "include/core/SkStream.h"
#include "modules/svg/include/SkSVGDOM.h"
#include <chrono>
#include <cmath>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace astra::ui {
namespace {
constexpr std::size_t max_cache_bytes = 32 * 1024 * 1024;

struct Pictures {
    std::vector<sk_sp<SkPicture>> items;
    std::size_t bytes{};
};

void record(Pictures& pictures, IconAsset asset) {
    if (pictures.items.at(asset.value)) {
        return;
    }
    const auto& resource = icon_resources()[asset.value];
    SkMemoryStream stream(resource.svg.data(), resource.svg.size(), false);
    const auto dom = SkSVGDOM::Builder().make(stream);
    if (!dom) {
        throw std::runtime_error("Parse icon SVG: " + std::string(resource.id));
    }
    SkPictureRecorder recorder;
    dom->setContainerSize({24, 24});
    dom->render(recorder.beginRecording(24, 24));
    auto picture = recorder.finishRecordingAsPicture();
    const auto bytes = picture->approximateBytesUsed();
    if (bytes > max_cache_bytes - pictures.bytes) {
        throw std::length_error("Icon pictures exceed the cache budget");
    }
    pictures.bytes += bytes;
    pictures.items[asset.value] = std::move(picture);
}
} // namespace

struct IconRenderer::Impl {
    std::shared_ptr<const Pictures> pictures;
    std::future<std::shared_ptr<const Pictures>> pending;
    bool ready{};
    std::jthread worker;

    Impl() {
        auto warm = std::make_shared<Pictures>();
        warm->items.resize(icon_resources().size());
        for (std::size_t i = 0; i <= static_cast<std::size_t>(Icon::warning); ++i) {
            record(*warm, icon_asset(static_cast<Icon>(i)));
        }
        for (const auto name : {"material/file", "material/folder", "material/folder-open"}) {
            record(*warm, *icon_asset(name));
        }
        pictures = warm;
        std::promise<std::shared_ptr<const Pictures>> result;
        pending = result.get_future();
        worker = std::jthread([warm, result = std::move(result)](std::stop_token stop) mutable {
            try {
                auto prepared = std::make_shared<Pictures>(*warm);
                for (std::size_t i = 0; i < prepared->items.size(); ++i) {
                    if (stop.stop_requested()) {
                        return;
                    }
                    record(*prepared, IconAsset{static_cast<std::uint16_t>(i)});
                }
                result.set_value(std::move(prepared));
            } catch (...) {
                result.set_exception(std::current_exception());
            }
        });
    }
};

IconRenderer::IconRenderer() : impl_(std::make_unique<Impl>()) {
}

IconRenderer::~IconRenderer() = default;

void IconRenderer::poll() {
    if (impl_->pending.valid() &&
        impl_->pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        impl_->pictures = impl_->pending.get();
        impl_->ready = true;
    }
}

bool IconRenderer::ready() const {
    return impl_->ready;
}

std::size_t IconRenderer::bytes() const {
    return impl_->pictures->bytes;
}

void IconRenderer::draw(
    SkCanvas* canvas, Icon icon, float x, float y, float size, std::uint32_t color) const {
    draw(canvas, icon_asset(icon), x, y, size, color);
}

void IconRenderer::draw(
    SkCanvas* canvas, IconAsset asset, float x, float y, float size, std::uint32_t color) const {
    if (!canvas || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(size) || size <= 0 ||
        asset.value >= icon_resources().size()) {
        throw std::invalid_argument("Icon drawing requires a valid canvas, asset and geometry");
    }
    const auto& pictures = *impl_->pictures;
    if (!pictures.items[asset.value]) {
        asset = *icon_asset("material/file");
    }
    const SkAutoCanvasRestore restore(canvas, true);
    canvas->translate(x, y);
    canvas->scale(size / 24, size / 24);
    canvas->clipRect(SkRect::MakeWH(24, 24));
    SkPaint paint;
    if (icon_resources()[asset.value].monochrome) {
        paint.setColorFilter(SkColorFilters::Blend(color, SkBlendMode::kSrcIn));
    }
    canvas->drawPicture(pictures.items[asset.value],
                        nullptr,
                        icon_resources()[asset.value].monochrome ? &paint : nullptr);
}
} // namespace astra::ui
