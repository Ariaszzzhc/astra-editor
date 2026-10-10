#include <astra/core/content_hash.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>

namespace astra {
namespace {
// SHA-256 round constants, FIPS 180-4 section 4.2.2:
// https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf
constexpr std::array<std::uint32_t, 64> rounds{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};
} // namespace

void ContentHash::compress() {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t i = 0; i < 16; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            words[i] = (words[i] << 8U) | std::to_integer<std::uint32_t>(block_[i * 4 + j]);
        }
    }
    for (std::size_t i = 16; i < words.size(); ++i) {
        const auto a = words[i - 15], b = words[i - 2];
        const auto s0 = std::rotr(a, 7) ^ std::rotr(a, 18) ^ (a >> 3U);
        const auto s1 = std::rotr(b, 17) ^ std::rotr(b, 19) ^ (b >> 10U);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }
    auto [a, b, c, d, e, f, g, h] = state_;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto s1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
        const auto choice = (e & f) ^ (~e & g);
        const auto t1 = h + s1 + choice + rounds[i] + words[i];
        const auto s0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
        const auto majority = (a & b) ^ (a & c) ^ (b & c);
        const auto t2 = s0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    const std::array result{a, b, c, d, e, f, g, h};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        state_[i] += result[i];
    }
}

void ContentHash::append(std::span<const std::byte> input) {
    while (!input.empty()) {
        const auto count = std::min(block_.size() - used_, input.size());
        std::copy_n(input.begin(), count, block_.begin() + static_cast<std::ptrdiff_t>(used_));
        used_ += count;
        input = input.subspan(count);
        if (used_ == block_.size()) {
            compress();
            used_ = 0;
        }
    }
}

void ContentHash::update(std::span<const std::byte> input) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max() / 8;
    if (input.size() > maximum - bytes_) {
        throw std::length_error("SHA-256 content exceeds its 64-bit bit length");
    }
    bytes_ += input.size();
    append(input);
}

void ContentHash::update(std::string_view input) {
    update(std::as_bytes(std::span(input.data(), input.size())));
}

ContentDigest ContentHash::digest() const {
    auto copy = *this;
    std::array<std::byte, 128> padding{};
    padding[0] = std::byte{0x80};
    const auto count = used_ < 56 ? 56 - used_ : 120 - used_;
    const auto bits = bytes_ * 8;
    for (std::size_t i = 0; i < 8; ++i) {
        padding[count + i] = std::byte((bits >> (56U - 8U * i)) & 0xffU);
    }
    copy.append(std::span(padding).first(count + 8));
    ContentDigest result{};
    for (std::size_t i = 0; i < copy.state_.size(); ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            result[i * 4 + j] = static_cast<std::uint8_t>(copy.state_[i] >> (24U - 8U * j));
        }
    }
    return result;
}
} // namespace astra
