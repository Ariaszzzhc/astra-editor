#pragma once

#include <cstddef>
#include <string>

namespace astra::ui {
// Native, bounded text acquisition. This synchronous mechanism must not be
// mistaken for an asynchronous request or used to promise owner-loop latency.
// Non-Windows SDL implementations require invocation on the SDL main thread.
[[nodiscard]] std::string read_clipboard_text(std::size_t max_bytes);
} // namespace astra::ui
