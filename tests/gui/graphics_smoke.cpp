#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSurface.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "modules/skunicode/include/SkUnicode_icu.h"
#include "dawn/native/DawnNative.h"
#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>

int main() {
    wgpu::InstanceDescriptor descriptor;
    constexpr auto timed = wgpu::InstanceFeatureName::TimedWaitAny;
    descriptor.requiredFeatureCount = 1;
    descriptor.requiredFeatures = &timed;
    dawn::native::Instance instance(&descriptor);
    wgpu::RequestAdapterOptions options;
    options.backendType = wgpu::BackendType::D3D12;
    auto adapters = instance.EnumerateAdapters(&options);
    if (adapters.empty()) {
        std::cerr << "No real D3D12 adapter\n";
        return 1;
    }
    wgpu::Adapter adapter(adapters.front().Get());
    std::atomic_bool gpu_error{};
    wgpu::DeviceDescriptor device_descriptor;
    device_descriptor.SetDeviceLostCallback(
        wgpu::CallbackMode::AllowSpontaneous,
        [](const wgpu::Device&,
           wgpu::DeviceLostReason reason,
           wgpu::StringView,
           std::atomic_bool* failed) {
            if (reason != wgpu::DeviceLostReason::Destroyed) {
                failed->store(true);
            }
        },
        &gpu_error);
    device_descriptor.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView, std::atomic_bool* failed) {
            failed->store(true);
        },
        &gpu_error);
    auto device = adapter.CreateDevice(&device_descriptor);
    if (!device) {
        std::cerr << "Device creation failed\n";
        return 2;
    }
    skgpu::graphite::DawnBackendContext backend;
    backend.fInstance = wgpu::Instance(instance.Get());
    backend.fDevice = device;
    backend.fQueue = device.GetQueue();
    auto context = skgpu::graphite::ContextFactory::MakeDawn(backend, {});
    if (!context) {
        std::cerr << "Graphite context failed\n";
        return 3;
    }
    auto recorder = context->makeRecorder();
    const auto info = SkImageInfo::Make(4, 4, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    auto surface = SkSurfaces::RenderTarget(recorder.get(), info);
    if (!surface) {
        std::cerr << "Graphite surface failed\n";
        return 4;
    }
    surface->getCanvas()->clear(SK_ColorRED);
    SkPaint paint;
    paint.setColor(SK_ColorGREEN);
    surface->getCanvas()->drawRect(SkRect::MakeXYWH(0, 0, 2, 2), paint);
    auto recording = recorder->snap();
    skgpu::graphite::InsertRecordingInfo insert;
    insert.fRecording = recording.get();
    if (!context->insertRecording(insert)) {
        std::cerr << "Recording failed\n";
        return 5;
    }

    struct Readback {
        std::unique_ptr<const SkImage::AsyncReadResult> pixels;
    };

    const auto readback = std::make_shared<Readback>();
    auto callback_state = std::make_unique<std::shared_ptr<Readback>>(readback);
    context->asyncRescaleAndReadPixels(
        surface.get(),
        info,
        SkIRect::MakeWH(4, 4),
        SkImage::RescaleGamma::kSrc,
        SkImage::RescaleMode::kNearest,
        [](void* output, std::unique_ptr<const SkImage::AsyncReadResult> result) {
            const std::unique_ptr<std::shared_ptr<Readback>> state(
                static_cast<std::shared_ptr<Readback>*>(output));
            (*state)->pixels = std::move(result);
        },
        callback_state.release());
    if (!context->submit(skgpu::graphite::SyncToCpu::kYes) || !readback->pixels) {
        std::cerr << "GPU readback failed\n";
        return 6;
    }
    const auto* first = static_cast<const std::uint8_t*>(readback->pixels->data(0));
    const auto* last = first + 3 * readback->pixels->rowBytes(0) + 3 * 4;
    if (first[0] != 0 || first[1] != 255 || first[2] != 0 || first[3] != 255 || last[0] != 255 ||
        last[1] != 0 || last[2] != 0 || last[3] != 255) {
        std::cerr << "GPU pixels differ from the drawn scene\n";
        return 7;
    }
    if (!SkUnicodes::ICU::Make()) {
        std::cerr << "ICU runtime data missing\n";
        return 8;
    }
    if (gpu_error.load()) {
        std::cerr << "GPU reported an unexpected error\n";
        return 9;
    }
    std::cout << "Real D3D12 + Dawn + Graphite pixels and ICU runtime: PASS\n";
}
