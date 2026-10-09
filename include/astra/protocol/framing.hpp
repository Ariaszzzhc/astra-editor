#pragma once

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace astra::protocol {

class FramingError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Content-Length framing is shared by Astra RPC and the LSP process adapter.
// A malformed stream is terminal: callers must close it, not guess a resync point.
class FrameDecoder {
public:
    static constexpr std::size_t default_body_limit = 8 * 1024 * 1024;
    static constexpr std::size_t default_header_limit = 8 * 1024;

    explicit FrameDecoder(std::size_t body_limit = default_body_limit,
                          std::size_t header_limit = default_header_limit);
    [[nodiscard]] std::vector<std::string> feed(std::string_view bytes);
    void finish();
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] std::size_t buffered_bytes() const noexcept;

private:
    [[noreturn]] void fail(const char* message);
    [[nodiscard]] std::size_t parse_header();

    std::size_t body_limit_;
    std::size_t header_limit_;
    bool failed_{};
    std::string header_;
    std::string body_;
    std::optional<std::size_t> expected_;
};

[[nodiscard]] std::string encode_frame(std::string_view body,
    std::size_t body_limit = FrameDecoder::default_body_limit);

} // namespace astra::protocol
