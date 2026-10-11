#pragma once

#include <astra/protocol/rpc.hpp>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace astra::ui {
// Owner-loop facade. Native clipboard providers run in owned helper processes;
// poll only observes ready replies and completed process lifetimes.
class ClipboardClient final {
  public:
    static constexpr std::size_t max_text_bytes = 8 * 1024 * 1024;
    // JSON control bytes can expand to six-byte escapes. Bound the encoded
    // Content-Length frame separately from the actual UTF-8 text budget.
    static constexpr std::size_t max_wire_bytes = 6 * max_text_bytes + 4096;
    static constexpr std::size_t max_jobs = 8;

    struct Options {
        std::filesystem::path executable;
        std::chrono::milliseconds timeout{10000};
    };

    explicit ClipboardClient(Options options);
    ~ClipboardClient();
    ClipboardClient(const ClipboardClient&) = delete;
    ClipboardClient& operator=(const ClipboardClient&) = delete;
    [[nodiscard]] std::future<protocol::Json> read(std::string id,
                                                   std::size_t limit = max_text_bytes);
    [[nodiscard]] std::future<protocol::Json> write(std::string id, std::string text);
    void cancel(const std::string& id);
    // Cancels unfinished work, preserving a successfully published clipboard owner.
    void cancel_pending();
    void poll();
    void close();
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] bool drained() const noexcept;
    [[nodiscard]] std::vector<std::string> take_errors();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace astra::ui
