#include <astra/core/content_hash.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#endif

namespace {
std::string hex(const astra::ContentDigest& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    for (const auto value : digest) {
        result += digits[value >> 4U];
        result += digits[value & 15U];
    }
    return result;
}

std::string hashed(std::string_view input) {
    astra::ContentHash hash;
    hash.update(input);
    return hex(hash.digest());
}
} // namespace

TEST_CASE("content hash SHA256 standard empty short multiblock and million-byte vectors") {
    CHECK(hashed("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hashed("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hashed("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(hashed(std::string(1000000, 'a')) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("content hash digest and branched streaming snapshots remain immutable") {
    astra::ContentHash hash;
    hash.update("a");
    const auto retained = hash;
    const auto before = hash.digest();
    hash.update("bc");
    CHECK(hex(hash.digest()) == hashed("abc"));
    CHECK(retained.digest() == before);
    CHECK(hash.digest() == hash.digest());
    hash.update("d");
    CHECK(hex(hash.digest()) == hashed("abcd"));
}

#if defined(_WIN32)
TEST_CASE("content hash streaming matches independent Windows BCrypt through padding boundaries") {
    std::mt19937 random(0xA57A256U);
    for (std::size_t size = 0; size <= 2049; ++size) {
        std::string data(size, '\0');
        for (auto& byte : data) {
            byte = static_cast<char>(random() & 255U);
        }
        astra::ContentDigest reference{};
        const auto result = BCryptHash(BCRYPT_SHA256_ALG_HANDLE,
                                       nullptr,
                                       0,
                                       reinterpret_cast<PUCHAR>(data.data()),
                                       static_cast<ULONG>(data.size()),
                                       reference.data(),
                                       static_cast<ULONG>(reference.size()));
        REQUIRE(result >= 0);
        astra::ContentHash hash;
        for (std::size_t offset = 0; offset < size;) {
            const auto count = std::min(size - offset, std::size_t{1} + random() % 193);
            hash.update(std::string_view(data).substr(offset, count));
            offset += count;
        }
        CHECK(hash.digest() == reference);
    }
}
#endif
