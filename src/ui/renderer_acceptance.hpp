#pragma once

#include "astra/ui/renderer.hpp"

#include <future>

namespace astra::ui::detail {

// Internal acceptance seam: uses the owned real Dawn device and callback.
// This does not emulate rendering or change the document/command APIs.
struct RendererAcceptance final {
    static void force_device_loss(Renderer& renderer);
    [[nodiscard]] static std::future<void> force_device_loss_concurrently(Renderer& renderer);
    static void check_device_errors(const Renderer& renderer);
    [[nodiscard]] static bool native_device_lost(const Renderer& renderer);
    [[nodiscard]] static bool loss_callback_received(const Renderer& renderer);
};

} // namespace astra::ui::detail
