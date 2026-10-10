#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace astra {
using ContentDigest = std::array<std::uint8_t, 32>;

// Incremental SHA-256 content identity (FIPS 180-4). No native handles, external
// I/O, dynamic allocation or shared mutable state. digest() preserves its input.
class ContentHash final {
  public:
    void update(std::span<const std::byte>);
    void update(std::string_view);
    [[nodiscard]] ContentDigest digest() const;

  private:
    std::array<std::uint32_t, 8> state_{0x6a09e667U,
                                        0xbb67ae85U,
                                        0x3c6ef372U,
                                        0xa54ff53aU,
                                        0x510e527fU,
                                        0x9b05688cU,
                                        0x1f83d9abU,
                                        0x5be0cd19U};
    std::array<std::byte, 64> block_{};
    std::uint64_t bytes_{};
    std::size_t used_{};
    void compress();
    void append(std::span<const std::byte>);
};
} // namespace astra
