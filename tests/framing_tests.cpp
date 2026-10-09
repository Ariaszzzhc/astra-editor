#include <astra/protocol/framing.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using astra::protocol::encode_frame;
using astra::protocol::FrameDecoder;
using astra::protocol::FramingError;

TEST_CASE("framing counts UTF-8 bytes and accepts coalesced frames") {
    const std::string first = "{\"text\":\"中文🙂\"}";
    FrameDecoder decoder;
    const auto output = decoder.feed(encode_frame(first) + encode_frame("{}"));
    REQUIRE(output.size() == 2);
    CHECK(output[0] == first);
    CHECK(output[1] == "{}");
    CHECK(decoder.idle());
    CHECK_NOTHROW(decoder.finish());
}

TEST_CASE("all two-piece network fragment boundaries decode identically") {
    const auto frame = encode_frame("{\"method\":\"document.edit\",\"id\":1}");
    for (std::size_t split = 0; split <= frame.size(); ++split) {
        CAPTURE(split);
        FrameDecoder decoder;
        auto output = decoder.feed(std::string_view(frame).substr(0, split));
        auto tail = decoder.feed(std::string_view(frame).substr(split));
        output.insert(output.end(), tail.begin(), tail.end());
        REQUIRE(output.size() == 1);
        CHECK(output[0] == "{\"method\":\"document.edit\",\"id\":1}");
        CHECK(decoder.idle());
    }
}

TEST_CASE("bounded decoder handles random fragmentation of many frames") {
    std::mt19937 generator(0xA57A);
    std::vector<std::string> expected;
    std::string stream;
    for (int index = 0; index != 500; ++index) {
        expected.push_back(std::string(generator() % 100, static_cast<char>('a' + index % 26)));
        stream += encode_frame(expected.back());
    }
    FrameDecoder decoder(100, 100);
    std::vector<std::string> actual;
    std::size_t offset{};
    while (offset < stream.size()) {
        const auto count = std::min<std::size_t>(1 + generator() % 73, stream.size() - offset);
        auto messages = decoder.feed(std::string_view(stream).substr(offset, count));
        actual.insert(actual.end(), messages.begin(), messages.end());
        CHECK(decoder.buffered_bytes() <= 100);
        offset += count;
    }
    CHECK(actual == expected);
    CHECK_NOTHROW(decoder.finish());
}

TEST_CASE("framing permits optional content type and ASCII case insensitivity") {
    FrameDecoder decoder;
    const auto output = decoder.feed("Content-Type: application/vscode-jsonrpc; charset=utf-8\r\ncOnTeNt-LeNgTh:\t2 \r\n\r\n{}");
    REQUIRE(output.size() == 1);
    CHECK(output[0] == "{}");
}

TEST_CASE("malformed and excessive headers permanently fail closed") {
    const std::vector<std::string> invalid{
        "Content-Length: -1\r\n\r\n", "Content-Length: +1\r\n\r\n",
        "Content-Length: 1x\r\n\r\n", "Content-Length: \r\n\r\n",
        "Content-Length: 184467440737095516160\r\n\r\n",
        "Content-Length: 101\r\n\r\n", "Other: 1\r\n\r\n",
        "Content-Length: 1\r\ncontent-length: 1\r\n\r\n",
        "Content-Length : 1\r\n\r\n", "Content Length: 1\r\n\r\n",
        "Content-Length 1\r\n\r\n", "\r\n\r\n",
        std::string(101, 'x'), std::string("Content-Length: 1\0", 19),
        "中文: 1\r\n\r\n"
    };
    for (const auto& header : invalid) {
        CAPTURE(header);
        FrameDecoder decoder(100, 100);
        CHECK_THROWS_AS((void)decoder.feed(header), FramingError);
        CHECK(decoder.failed());
        CHECK_THROWS_AS((void)decoder.feed(encode_frame("{}")), FramingError);
        CHECK_THROWS_AS(decoder.finish(), FramingError);
    }
}

TEST_CASE("EOF distinguishes complete streams from truncated headers and bodies") {
    FrameDecoder empty;
    CHECK_NOTHROW(empty.finish());
    for (const auto& partial : {"Content-Len", "Content-Length: 2\r\n\r\n", "Content-Length: 2\r\n\r\na"}) {
        FrameDecoder decoder;
        CHECK(decoder.feed(partial).empty());
        CHECK_THROWS_AS(decoder.finish(), FramingError);
    }
}

TEST_CASE("zero-length frames do not stall and invalid limits fail immediately") {
    FrameDecoder decoder;
    const auto messages = decoder.feed(encode_frame("") + encode_frame("x"));
    CHECK(messages == std::vector<std::string>{"", "x"});
    CHECK_THROWS_AS(FrameDecoder(0), std::invalid_argument);
    CHECK_THROWS_AS(FrameDecoder(1, 3), std::invalid_argument);
    CHECK_THROWS_AS((void)encode_frame("too large", 2), FramingError);
}
