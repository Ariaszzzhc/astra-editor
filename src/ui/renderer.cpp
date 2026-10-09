#include "astra/ui/renderer.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#include "include/gpu/graphite/BackendTexture.h"
#include "include/gpu/graphite/Context.h"
#include "include/gpu/graphite/ContextOptions.h"
#include "include/gpu/graphite/Recorder.h"
#include "include/gpu/graphite/Recording.h"
#include "include/gpu/graphite/Surface.h"
#include "include/gpu/graphite/dawn/DawnBackendContext.h"
#include "include/gpu/graphite/dawn/DawnGraphiteTypes.h"
#include "dawn/dawn_proc.h"
#include "dawn/native/DawnNative.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace astra::ui {
namespace {

[[noreturn]] void sdl_failure(const char* operation) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

struct VideoLifetime {
    VideoLifetime() {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) { sdl_failure("SDL video initialization"); }
    }
    ~VideoLifetime() { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
    VideoLifetime(const VideoLifetime&) = delete;
    VideoLifetime& operator=(const VideoLifetime&) = delete;
};

struct WindowDeleter {
    void operator()(SDL_Window* window) const noexcept {
        (void)SDL_StopTextInput(window);
        SDL_DestroyWindow(window);
    }
};

#if defined(__APPLE__)
struct MetalViewDeleter {
    void operator()(void* view) const noexcept { SDL_Metal_DestroyView(view); }
};
#endif

struct DeviceState {
    std::atomic_bool lost{};
    std::mutex mutex;
    std::string error;
};

std::string message_text(wgpu::StringView message) {
    if (!message.data) { return {}; }
    if (message.length == WGPU_STRLEN) { return std::string(message.data); }
    return {message.data, message.length};
}

wgpu::BackendType native_backend() {
#if defined(_WIN32)
    return wgpu::BackendType::D3D12;
#elif defined(__APPLE__)
    return wgpu::BackendType::Metal;
#else
    return wgpu::BackendType::Vulkan;
#endif
}

} // namespace

struct Renderer::Impl {
    VideoLifetime video;
    std::unique_ptr<SDL_Window, WindowDeleter> window;
#if defined(__APPLE__)
    std::unique_ptr<void, MetalViewDeleter> metal_view;
#endif
    std::unique_ptr<dawn::native::Instance> instance;
    std::shared_ptr<DeviceState> device_state;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Surface surface;
    std::unique_ptr<skgpu::graphite::Context> context;
    std::unique_ptr<skgpu::graphite::Recorder> recorder;
    sk_sp<SkSurface> frame;
    wgpu::Texture acquired_texture;
    PixelSize configured_size;
    bool configured{};
    bool reconfigure{true};

    explicit Impl(const std::string& title, int width, int height) {
        if (width <= 0 || height <= 0) { throw std::invalid_argument("Window dimensions must be positive"); }
        auto flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#if defined(__APPLE__)
        flags |= SDL_WINDOW_METAL;
#endif
        window.reset(SDL_CreateWindow(title.c_str(), width, height, flags));
        if (!window) { sdl_failure("Create editor window"); }
        if (!SDL_StartTextInput(window.get())) { sdl_failure("Enable editor text input"); }

        static std::once_flag install_procs;
        std::call_once(install_procs, [] {
            const auto procs = dawn::native::GetProcs();
            dawnProcSetProcs(&procs);
        });
        constexpr auto timed_wait = wgpu::InstanceFeatureName::TimedWaitAny;
        wgpu::InstanceDescriptor descriptor;
        descriptor.requiredFeatureCount = 1;
        descriptor.requiredFeatures = &timed_wait;
        instance = std::make_unique<dawn::native::Instance>(&descriptor);
        create_surface();
        create_device();
    }

    ~Impl() {
        frame.reset();
        acquired_texture = nullptr;
        if (context && device_state && !device_state->lost.load()) {
            (void)context->submit(skgpu::graphite::SyncToCpu::kYes);
        }
        recorder.reset();
        context.reset();
        if (configured && surface) { surface.Unconfigure(); }
        surface = nullptr;
        if (device) { device.Destroy(); }
        device = nullptr;
        adapter = nullptr;
        instance.reset();
    }

    [[nodiscard]] PixelSize pixel_size() const {
        PixelSize size;
        if (!SDL_GetWindowSizeInPixels(window.get(), &size.width, &size.height)) {
            sdl_failure("Query drawable pixel size");
        }
        return size;
    }

    void create_surface() {
        wgpu::SurfaceDescriptor descriptor;
        const auto properties = SDL_GetWindowProperties(window.get());
        if (!properties) { sdl_failure("Query native window properties"); }
#if defined(_WIN32)
        wgpu::SurfaceSourceWindowsHWND source;
        source.hwnd = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        source.hinstance = GetModuleHandleW(nullptr);
        if (!source.hwnd) { throw std::runtime_error("SDL window has no Win32 HWND"); }
        descriptor.nextInChain = &source;
        surface = wgpu::Instance(instance->Get()).CreateSurface(&descriptor);
#elif defined(__APPLE__)
        if (!metal_view) { metal_view.reset(SDL_Metal_CreateView(window.get())); }
        if (!metal_view) { sdl_failure("Create Metal window view"); }
        wgpu::SurfaceSourceMetalLayer source;
        source.layer = SDL_Metal_GetLayer(metal_view.get());
        if (!source.layer) { throw std::runtime_error("SDL Metal view has no CAMetalLayer"); }
        descriptor.nextInChain = &source;
        surface = wgpu::Instance(instance->Get()).CreateSurface(&descriptor);
#elif defined(__linux__)
        const auto* driver = SDL_GetCurrentVideoDriver();
        if (driver && std::string_view(driver) == "wayland") {
            wgpu::SurfaceSourceWaylandSurface source;
            source.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
            source.surface = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
            if (!source.display || !source.surface) { throw std::runtime_error("SDL window has no Wayland surface"); }
            descriptor.nextInChain = &source;
            surface = wgpu::Instance(instance->Get()).CreateSurface(&descriptor);
        } else if (driver && std::string_view(driver) == "x11") {
            wgpu::SurfaceSourceXlibWindow source;
            source.display = SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            source.window = static_cast<std::uint64_t>(SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
            if (!source.display || !source.window) { throw std::runtime_error("SDL window has no X11 surface"); }
            descriptor.nextInChain = &source;
            surface = wgpu::Instance(instance->Get()).CreateSurface(&descriptor);
        } else {
            throw std::runtime_error("SDL video driver does not expose a supported native surface");
        }
#else
        throw std::runtime_error("This platform has no native Dawn window surface adapter");
#endif
        if (!surface) { throw std::runtime_error("Create native Dawn window surface failed"); }
        configured = false;
        reconfigure = true;
    }

    void create_device() {
        wgpu::RequestAdapterOptions options;
        options.backendType = native_backend();
        options.compatibleSurface = surface;
        options.powerPreference = wgpu::PowerPreference::HighPerformance;
        const auto adapters = instance->EnumerateAdapters(&options);
        if (adapters.empty()) { throw std::runtime_error("No compatible native GPU adapter is available"); }
        adapter = wgpu::Adapter(adapters.front().Get());
        auto state = std::make_shared<DeviceState>();
        wgpu::DeviceDescriptor descriptor;
        descriptor.SetDeviceLostCallback(wgpu::CallbackMode::AllowSpontaneous,
            [state](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
                if (reason == wgpu::DeviceLostReason::Destroyed) { return; }
                {
                    const std::lock_guard lock(state->mutex);
                    state->error = "GPU device lost: " + message_text(message);
                }
                state->lost.store(true);
            });
        descriptor.SetUncapturedErrorCallback(
            [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message, DeviceState* state) {
                const std::lock_guard lock(state->mutex);
                state->error = "GPU error: " + message_text(message);
            }, state.get());
        device = adapter.CreateDevice(&descriptor);
        if (!device) { throw std::runtime_error("Create native GPU device failed"); }
        device_state = std::move(state);
        skgpu::graphite::DawnBackendContext backend;
        backend.fInstance = wgpu::Instance(instance->Get());
        backend.fDevice = device;
        backend.fQueue = device.GetQueue();
        context = skgpu::graphite::ContextFactory::MakeDawn(backend, {});
        if (!context) { throw std::runtime_error("Create Skia Graphite context failed"); }
        recorder = context->makeRecorder();
        if (!recorder) { throw std::runtime_error("Create Skia Graphite recorder failed"); }
        reconfigure = true;
    }

    void check_errors() const {
        if (!device_state) { return; }
        // Device loss is recovered at the next frame boundary. Its callback is
        // not a fatal document/editor error.
        if (device_state->lost.load()) { return; }
        const std::lock_guard lock(device_state->mutex);
        if (!device_state->error.empty()) { throw std::runtime_error(device_state->error); }
    }

    void recover_device() {
        frame.reset();
        acquired_texture = nullptr;
        recorder.reset();
        context.reset();
        if (configured) { surface.Unconfigure(); }
        surface = nullptr;
        if (device) { device.Destroy(); }
        device = nullptr;
        adapter = nullptr;
        create_surface();
        create_device();
    }

    void configure(PixelSize size) {
        wgpu::SurfaceCapabilities capabilities;
        if (surface.GetCapabilities(adapter, &capabilities) != wgpu::Status::Success || capabilities.formatCount == 0) {
            throw std::runtime_error("Query native surface capabilities failed");
        }
        const std::span formats(capabilities.formats, capabilities.formatCount);
        auto format = wgpu::TextureFormat::Undefined;
        for (const auto desired : {wgpu::TextureFormat::BGRA8Unorm, wgpu::TextureFormat::RGBA8Unorm}) {
            if (std::ranges::find(formats, desired) != formats.end()) { format = desired; break; }
        }
        if (format == wgpu::TextureFormat::Undefined) {
            throw std::runtime_error("Native surface lacks a supported 8-bit color format");
        }
        const wgpu::TextureUsage required = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
        if (static_cast<wgpu::TextureUsage>(capabilities.usages & required) != required) {
            throw std::runtime_error("Native surface cannot support drawing and GPU screenshots");
        }
        const std::span present_modes(capabilities.presentModes, capabilities.presentModeCount);
        if (std::ranges::find(present_modes, wgpu::PresentMode::Fifo) == present_modes.end()) {
            throw std::runtime_error("Native surface lacks FIFO presentation");
        }
        if (capabilities.alphaModeCount == 0) { throw std::runtime_error("Native surface has no alpha mode"); }
        const std::span alpha_modes(capabilities.alphaModes, capabilities.alphaModeCount);
        wgpu::SurfaceConfiguration configuration;
        configuration.device = device;
        configuration.format = format;
        configuration.usage = required | (capabilities.usages &
            (wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst));
        configuration.width = static_cast<std::uint32_t>(size.width);
        configuration.height = static_cast<std::uint32_t>(size.height);
        configuration.presentMode = wgpu::PresentMode::Fifo;
        configuration.alphaMode = std::ranges::find(alpha_modes, wgpu::CompositeAlphaMode::Opaque) != alpha_modes.end()
            ? wgpu::CompositeAlphaMode::Opaque : alpha_modes.front();
        surface.Configure(&configuration);
        configured_size = size;
        configured = true;
        reconfigure = false;
        check_errors();
    }

    [[nodiscard]] bool insert_pending() {
        auto recording = recorder->snap();
        if (!recording) { return true; }
        skgpu::graphite::InsertRecordingInfo info;
        info.fRecording = recording.get();
        if (!context->insertRecording(info)) {
            if (device_state->lost.load() || context->isDeviceLost()) {
                device_state->lost.store(true);
                return false;
            }
            throw std::runtime_error("Graphite recording submission failed");
        }
        return true;
    }
};

Renderer::Renderer(std::string title, int width, int height)
    : impl_(std::make_unique<Impl>(title, width, height)) {}

Renderer::~Renderer() = default;

SDL_Window* Renderer::window() const noexcept { return impl_->window.get(); }

PixelSize Renderer::pixel_size() const { return impl_->pixel_size(); }

float Renderer::dpi_scale() const {
    const auto scale = SDL_GetWindowDisplayScale(window());
    if (scale <= 0) { sdl_failure("Query window DPI scale"); }
    return scale;
}

void Renderer::poll_events(const std::function<void(const SDL_Event&)>& handler) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        case SDL_EVENT_WINDOW_RESTORED:
            impl_->reconfigure = true;
            break;
        default:
            break;
        }
        handler(event);
    }
    wgpu::Instance(impl_->instance->Get()).ProcessEvents();
}

SkCanvas* Renderer::begin_frame() {
    if (impl_->frame) { throw std::logic_error("begin_frame called before the previous frame ended"); }
    wgpu::Instance(impl_->instance->Get()).ProcessEvents();
    if (impl_->device_state->lost.load() || impl_->context->isDeviceLost()) { impl_->recover_device(); }
    impl_->check_errors();
    const auto size = impl_->pixel_size();
    if (size.width <= 0 || size.height <= 0 || (SDL_GetWindowFlags(window()) & SDL_WINDOW_MINIMIZED)) { return nullptr; }
    if (impl_->reconfigure || size != impl_->configured_size) { impl_->configure(size); }

    for (int attempt = 0; attempt != 2; ++attempt) {
        wgpu::SurfaceTexture texture;
        impl_->surface.GetCurrentTexture(&texture);
        switch (texture.status) {
        case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
            impl_->reconfigure = true;
            [[fallthrough]];
        case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal: {
            if (!texture.texture) { throw std::runtime_error("Surface acquisition returned no texture"); }
            impl_->acquired_texture = std::move(texture.texture);
            const auto backend = skgpu::graphite::BackendTextures::MakeDawn(impl_->acquired_texture.Get());
            impl_->frame = SkSurfaces::WrapBackendTexture(impl_->recorder.get(), backend, SkColorSpace::MakeSRGB(), nullptr);
            if (!impl_->frame) { throw std::runtime_error("Graphite could not wrap the native surface texture"); }
            return impl_->frame->getCanvas();
        }
        case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
            return nullptr;
        case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
            impl_->configure(size);
            break;
        case wgpu::SurfaceGetCurrentTextureStatus::Lost:
            if (impl_->configured) { impl_->surface.Unconfigure(); }
            impl_->surface = nullptr;
            impl_->create_surface();
            impl_->configure(size);
            break;
        default:
            if (impl_->device_state->lost.load() || impl_->context->isDeviceLost()) { return nullptr; }
            impl_->check_errors();
            throw std::runtime_error("Native surface texture acquisition failed");
        }
    }
    return nullptr;
}

void Renderer::end_frame() {
    if (!impl_->frame) { return; }
    if (!impl_->insert_pending()) {
        impl_->frame.reset();
        impl_->acquired_texture = nullptr;
        return;
    }
    if (!impl_->context->submit()) {
        impl_->frame.reset();
        impl_->acquired_texture = nullptr;
        if (impl_->device_state->lost.load() || impl_->context->isDeviceLost()) { return; }
        throw std::runtime_error("Submit Graphite GPU work failed");
    }
    const auto presented = impl_->surface.Present();
    impl_->frame.reset();
    impl_->acquired_texture = nullptr;
    if (presented != wgpu::Status::Success) { impl_->reconfigure = true; }
    impl_->check_errors();
}

void Renderer::capture_png(const std::filesystem::path& destination) {
    if (!impl_->frame) { throw std::logic_error("capture_png requires a currently drawn frame"); }
    if (!impl_->insert_pending()) { throw std::runtime_error("GPU device was lost during screenshot capture"); }
    const auto size = impl_->configured_size;
    const auto info = SkImageInfo::Make(size.width, size.height, kRGBA_8888_SkColorType,
                                       kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    struct Readback {
        std::mutex mutex;
        std::unique_ptr<const SkImage::AsyncReadResult> pixels;
    };
    auto readback = std::make_shared<Readback>();
    auto callback_state = std::make_unique<std::shared_ptr<Readback>>(readback);
    impl_->context->asyncRescaleAndReadPixels(impl_->frame.get(), info, SkIRect::MakeWH(size.width, size.height),
        SkImage::RescaleGamma::kSrc, SkImage::RescaleMode::kNearest,
        [](void* output, std::unique_ptr<const SkImage::AsyncReadResult> pixels) {
            // Skia owns the callback context until completion, including failure.
            const std::unique_ptr<std::shared_ptr<Readback>> state(static_cast<std::shared_ptr<Readback>*>(output));
            const std::lock_guard lock((*state)->mutex);
            (*state)->pixels = std::move(pixels);
        }, callback_state.release());
    const bool submitted = impl_->context->submit(skgpu::graphite::SyncToCpu::kYes);
    const std::lock_guard readback_lock(readback->mutex);
    if (!submitted || !readback->pixels) {
        throw std::runtime_error("Read native GPU frame pixels failed");
    }
    const SkPixmap pixels(info, readback->pixels->data(0), readback->pixels->rowBytes(0));
    SkDynamicMemoryWStream encoded;
    if (!SkPngEncoder::Encode(&encoded, pixels, {})) { throw std::runtime_error("Encode GPU screenshot as PNG failed"); }
    const auto data = encoded.detachAsData();
    if (destination.has_parent_path()) { std::filesystem::create_directories(destination.parent_path()); }
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) { throw std::runtime_error("Open screenshot destination failed"); }
    output.write(static_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
    output.close();
    if (!output) { throw std::runtime_error("Write GPU screenshot failed"); }
}

void Renderer::set_text_input_area(const SDL_Rect& area, int cursor_offset) {
    if (!SDL_SetTextInputArea(window(), &area, cursor_offset)) { sdl_failure("Update IME text input area"); }
}

} // namespace astra::ui
