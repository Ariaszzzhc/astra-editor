#include "window_frame.hpp"

#if defined(_WIN32)
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d11.h>
#include <roapi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <memory>
#include <optional>

namespace astra::test {
namespace {
namespace Capture = winrt::Windows::Graphics::Capture;
namespace CaptureDirectX = winrt::Windows::Graphics::DirectX;

struct Apartment {
    Apartment() {
        auto result = RoInitialize(RO_INIT_SINGLETHREADED);
        if (result == RPC_E_CHANGED_MODE)
            result = RoInitialize(RO_INIT_MULTITHREADED);
        if (FAILED(result))
            throw std::runtime_error("Cannot initialize native capture apartment: " +
                                     std::to_string(static_cast<std::uint32_t>(result)));
    }

    ~Apartment() {
        RoUninitialize();
    }

    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};

struct CaptureState {
    std::mutex mutex;
    std::condition_variable ready;
    bool finished{};
    std::optional<WindowPixels> pixels;
    std::exception_ptr error;
    std::atomic<std::int32_t> close_error{};
};

struct CaptureClose {
    winrt::Windows::Foundation::IClosable resource{nullptr};
    std::shared_ptr<CaptureState> state;

    ~CaptureClose() {
        if (!resource)
            return;
        try {
            resource.Close();
        } catch (const winrt::hresult_error& error) {
            std::int32_t expected{};
            state->close_error.compare_exchange_strong(expected, error.code().value);
        }
    }
};

struct MappedTexture {
    ID3D11DeviceContext* context;
    ID3D11Texture2D* texture;

    ~MappedTexture() {
        context->Unmap(texture, 0);
    }
};
} // namespace

WindowPixels capture_window_pixels(SDL_Window* window) {
    SDL_PumpEvents();
    const auto native = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    RECT visible{}, window_bounds{};
    POINT client{};
    if (!native || !GetWindowRect(native, &window_bounds) || !ClientToScreen(native, &client) ||
        FAILED(
            DwmGetWindowAttribute(native, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible))))
        throw std::runtime_error("Cannot measure the owned native capture window");
    if (FAILED(DwmFlush()))
        throw std::runtime_error("Cannot synchronize native window composition");
    Apartment apartment;
    auto state = std::make_shared<CaptureState>();
    {
        CaptureClose pool_close{{nullptr}, state};
        CaptureClose session_close{{nullptr}, state};
        try {
            if (!Capture::GraphicsCaptureSession::IsSupported())
                throw std::runtime_error("Windows Graphics Capture is unavailable");
            const auto factory = winrt::get_activation_factory<Capture::GraphicsCaptureItem,
                                                               IGraphicsCaptureItemInterop>();
            Capture::GraphicsCaptureItem item{nullptr};
            winrt::check_hresult(factory->CreateForWindow(
                native, winrt::guid_of<Capture::GraphicsCaptureItem>(), winrt::put_abi(item)));
            const auto size = item.Size();
            if (size.Width <= 0 || size.Height <= 0 || size.Width > 16384 || size.Height > 16384)
                throw std::runtime_error("Invalid owned-window capture dimensions");
            int caption_height{};
            if (size.Width == visible.right - visible.left &&
                size.Height == visible.bottom - visible.top)
                caption_height = client.y - visible.top;
            else if (size.Width == window_bounds.right - window_bounds.left &&
                     size.Height == window_bounds.bottom - window_bounds.top)
                caption_height = client.y - window_bounds.top;
            else
                throw std::runtime_error(
                    "Graphics capture dimensions do not match the owned window");
            if (caption_height <= 0 || caption_height >= size.Height)
                throw std::runtime_error("Owned window capture has no title bar");
            winrt::com_ptr<ID3D11Device> device;
            winrt::com_ptr<ID3D11DeviceContext> context;
            winrt::check_hresult(D3D11CreateDevice(nullptr,
                                                   D3D_DRIVER_TYPE_HARDWARE,
                                                   nullptr,
                                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                                   nullptr,
                                                   0,
                                                   D3D11_SDK_VERSION,
                                                   device.put(),
                                                   nullptr,
                                                   context.put()));
            const auto dxgi = device.as<IDXGIDevice>();
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(
                CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
            const auto projected = inspectable.as<CaptureDirectX::Direct3D11::IDirect3DDevice>();
            auto pool = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                projected, CaptureDirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, size);
            pool_close.resource = pool.as<winrt::Windows::Foundation::IClosable>();
            pool.FrameArrived([state, device, context, size, caption_height](const auto& sender,
                                                                             const auto&) {
                std::unique_lock lock(state->mutex);
                if (state->finished)
                    return;
                try {
                    {
                        auto frame = sender.TryGetNextFrame();
                        if (!frame)
                            return;
                        CaptureClose frame_close{frame.as<winrt::Windows::Foundation::IClosable>(),
                                                 state};
                        if (frame.ContentSize().Width != size.Width ||
                            frame.ContentSize().Height != size.Height)
                            throw std::runtime_error("Owned window resized during capture");
                        const auto access = frame.Surface()
                                                .as<::Windows::Graphics::DirectX::Direct3D11::
                                                        IDirect3DDxgiInterfaceAccess>();
                        winrt::com_ptr<ID3D11Texture2D> texture;
                        winrt::check_hresult(
                            access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
                        D3D11_TEXTURE2D_DESC descriptor{};
                        texture->GetDesc(&descriptor);
                        descriptor.BindFlags = 0;
                        descriptor.MiscFlags = 0;
                        descriptor.Usage = D3D11_USAGE_STAGING;
                        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        winrt::com_ptr<ID3D11Texture2D> staging;
                        winrt::check_hresult(
                            device->CreateTexture2D(&descriptor, nullptr, staging.put()));
                        context->CopyResource(staging.get(), texture.get());
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        winrt::check_hresult(
                            context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
                        const MappedTexture unmap{context.get(), staging.get()};
                        WindowPixels pixels{size.Width, size.Height, caption_height, {}};
                        pixels.pixels.resize(static_cast<std::size_t>(size.Width) * size.Height);
                        for (int y = 0; y < size.Height; ++y)
                            std::memcpy(pixels.pixels.data() +
                                            static_cast<std::size_t>(y) * size.Width,
                                        static_cast<const std::byte*>(mapped.pData) +
                                            static_cast<std::size_t>(y) * mapped.RowPitch,
                                        static_cast<std::size_t>(size.Width) * 4);
                        state->pixels = std::move(pixels);
                    }
                } catch (...) {
                    state->error = std::current_exception();
                }
                state->finished = true;
                lock.unlock();
                state->ready.notify_one();
            });
            auto session = pool.CreateCaptureSession(item);
            session_close.resource = session.as<winrt::Windows::Foundation::IClosable>();
            session.IsCursorCaptureEnabled(false);
            session.StartCapture();
            std::unique_lock lock(state->mutex);
            if (!state->ready.wait_for(lock, std::chrono::seconds(3), [&] {
                    return state->finished;
                })) {
                state->finished = true;
                state->error = std::make_exception_ptr(
                    std::runtime_error("Owned native window capture timed out"));
            }
        } catch (...) {
            std::lock_guard lock(state->mutex);
            state->finished = true;
            state->error = std::current_exception();
        }
    }
    if (const auto error = state->close_error.load(); error < 0)
        throw std::runtime_error("Native capture resource close failed: " +
                                 std::to_string(static_cast<std::uint32_t>(error)));
    if (state->error) {
        try {
            std::rethrow_exception(state->error);
        } catch (const winrt::hresult_error& error) {
            throw std::runtime_error("Native window capture failed: " +
                                     winrt::to_string(error.message()));
        }
    }
    if (!state->pixels)
        throw std::runtime_error("Owned native capture produced no frame");
    return std::move(*state->pixels);
}
} // namespace astra::test
#endif
