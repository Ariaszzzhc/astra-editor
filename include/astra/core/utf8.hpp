#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace astra::utf8 {

struct Scalar {
    char32_t value{};
    std::size_t bytes{};
};

// Decode Unicode scalar values only: overlong encodings, surrogates and values
// beyond U+10FFFF are rejected. Embedded NUL and a UTF-8 BOM are ordinary text.
[[nodiscard]] inline Scalar decode(std::string_view text, std::size_t offset) {
    if (offset >= text.size()) {
        throw std::out_of_range("UTF-8 offset is outside the text");
    }
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80U) {
        return {static_cast<char32_t>(first), 1};
    }
    std::size_t width = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xC2U && first <= 0xDFU) {
        width = 2;
        value = first & 0x1FU;
        minimum = 0x80U;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        width = 3;
        value = first & 0x0FU;
        minimum = 0x800U;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        width = 4;
        value = first & 0x07U;
        minimum = 0x10000U;
    } else {
        throw std::invalid_argument("Invalid UTF-8 leading byte");
    }
    if (text.size() - offset < width) {
        throw std::invalid_argument("Truncated UTF-8 sequence");
    }
    for (std::size_t index = 1; index < width; ++index) {
        const auto next = static_cast<unsigned char>(text[offset + index]);
        if ((next & 0xC0U) != 0x80U) {
            throw std::invalid_argument("Invalid UTF-8 continuation byte");
        }
        value = (value << 6U) | (next & 0x3FU);
    }
    if (value < minimum || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
        throw std::invalid_argument("Invalid Unicode scalar value");
    }
    return {static_cast<char32_t>(value), width};
}

[[nodiscard]] inline bool is_valid(std::string_view text) noexcept {
    try {
        for (std::size_t offset = 0; offset < text.size();) {
            offset += decode(text, offset).bytes;
        }
        return true;
    } catch (...) {
        return false;
    }
}

inline void validate(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        offset += decode(text, offset).bytes;
    }
}

// This only checks a boundary; callers must already know text is valid UTF-8.
[[nodiscard]] inline bool is_boundary(std::string_view text,
                                      std::size_t offset) noexcept {
    return offset <= text.size() &&
           (offset == text.size() ||
            (static_cast<unsigned char>(text[offset]) & 0xC0U) != 0x80U);
}

[[nodiscard]] constexpr std::size_t utf16_width(char32_t scalar) noexcept {
    return scalar > 0xFFFF ? 2U : 1U;
}

[[nodiscard]] inline std::size_t utf16_length(std::string_view text) {
    std::size_t length = 0;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto scalar = decode(text, offset);
        offset += scalar.bytes;
        length += utf16_width(scalar.value);
    }
    return length;
}

[[nodiscard]] inline std::size_t byte_offset_from_utf16(std::string_view text,
                                                       std::size_t units) {
    std::size_t offset = 0;
    while (units != 0 && offset < text.size()) {
        const auto scalar = decode(text, offset);
        const auto width = utf16_width(scalar.value);
        if (units < width) {
            throw std::invalid_argument("UTF-16 offset splits a surrogate pair");
        }
        units -= width;
        offset += scalar.bytes;
    }
    if (units != 0) {
        throw std::out_of_range("UTF-16 offset is outside the text");
    }
    return offset;
}

} // namespace astra::utf8
