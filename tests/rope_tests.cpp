#include "astra/core/rope.hpp"
#include "astra/core/utf8.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Explicit UTF-8 bytes make the tests independent of the compiler's source
// character set. Includes supplementary, combining and multibyte BMP scalars.
const std::string chinese = "\xE4\xB8\xAD\xE6\x96\x87";
const std::string emoji = "\xF0\x9F\x98\x80";
const std::string combining = "e\xCC\x81";

std::vector<std::size_t> boundaries(std::string_view text) {
    std::vector<std::size_t> result{0};
    for (std::size_t index = 0; index < text.size();) {
        const auto first = static_cast<unsigned char>(text[index]);
        index += first < 0x80U ? 1 : first < 0xE0U ? 2 : first < 0xF0U ? 3 : 4;
        result.push_back(index);
    }
    return result;
}

// Independent reference using only UTF-8 leading-byte widths. All reference
// strings are assembled from valid fixtures, so no production decoder is used.
std::size_t reference_utf16(std::string_view text) {
    std::size_t result = 0;
    for (const auto value : text) {
        const auto byte = static_cast<unsigned char>(value);
        if ((byte & 0xC0U) != 0x80U) {
            result += byte >= 0xF0U ? 2 : 1;
        }
    }
    return result;
}

astra::TextPosition reference_position(std::string_view text, std::size_t byte) {
    const auto prefix = text.substr(0, byte);
    const auto last_break = prefix.rfind('\n');
    return {static_cast<std::size_t>(std::count(prefix.begin(), prefix.end(), '\n')),
            reference_utf16(last_break == std::string_view::npos
                                ? prefix
                                : prefix.substr(last_break + 1))};
}

std::vector<std::size_t> reference_line_starts(std::string_view text) {
    std::vector<std::size_t> result{0};
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '\n') {
            result.push_back(index + 1);
        }
    }
    return result;
}

void check_rope(const astra::Rope& rope, const std::string& expected) {
    REQUIRE(rope.validate());
    CHECK(rope.str() == expected);
    CHECK(rope.size_bytes() == expected.size());
    CHECK(rope.size_utf16() == reference_utf16(expected));
    CHECK(rope.line_count() == reference_line_starts(expected).size());
    CHECK(rope.empty() == expected.empty());
}

void check_position(const astra::Rope& rope, std::string_view expected, std::size_t byte) {
    CAPTURE(byte);
    REQUIRE(rope.is_boundary(byte));
    const auto units = reference_utf16(expected.substr(0, byte));
    CHECK(rope.byte_to_utf16(byte) == units);
    CHECK(rope.utf16_to_byte(units) == byte);
    const auto position = reference_position(expected, byte);
    CHECK(rope.byte_to_position(byte) == position);
    CHECK(rope.position_to_byte(position) == byte);
}

std::string random_text(std::mt19937& random, std::size_t maximum_tokens) {
    const std::vector<std::string> pieces{
        "a", "Z", " ", "\t", "\n", "\r\n", "\r", chinese, emoji, combining,
        std::string(1, '\0'), "\xF4\x8F\xBF\xBF", "{}", "\xC2\xA0"};
    std::string result;
    const auto length = static_cast<std::size_t>(random()) % (maximum_tokens + 1);
    for (std::size_t index = 0; index < length; ++index) {
        result += pieces[random() % pieces.size()];
    }
    return result;
}

} // namespace

TEST_CASE("persistent Rope change extents reconstruct Unicode edits without flattening") {
    std::string source;
    for (int i = 0; i < 600; ++i) { source += "line " + chinese + emoji + combining + "\r\n"; }
    astra::Rope before(source);
    std::mt19937 random(0x53484152U);
    for (int round = 0; round < 300; ++round) {
        const auto offsets = boundaries(source);
        const auto index = static_cast<std::size_t>(random() % static_cast<unsigned>(offsets.size()));
        const auto finish = std::min(offsets.size() - 1, index + static_cast<std::size_t>(random() % 7U));
        const auto added = round % 2 == 0 ? emoji + "new\n" : std::string{};
        const auto after = before.replace(offsets[index], offsets[finish], added);
        const auto extent = before.changed_extent(after);
        REQUIRE(before.is_boundary(extent.begin));
        REQUIRE(before.is_boundary(extent.old_end));
        REQUIRE(after.is_boundary(extent.begin));
        REQUIRE(after.is_boundary(extent.new_end));
        CHECK(before.replace(extent.begin, extent.old_end, after.slice(extent.begin, extent.new_end)).str() == after.str());
        CHECK(extent.old_end - extent.begin <= offsets[finish] - offsets[index] + 2 * astra::Rope::max_chunk_bytes);
        source.replace(offsets[index], offsets[finish] - offsets[index], added);
        before = after;
    }
    const auto same = before.changed_extent(before);
    CHECK(same.begin == same.old_end);
    CHECK(same.begin == same.new_end);
    const astra::Rope rebuilt(source);
    const auto whole = before.changed_extent(rebuilt);
    CHECK(before.replace(whole.begin, whole.old_end, rebuilt.slice(whole.begin, whole.new_end)).str() == source);
}

TEST_CASE("UTF-8 accepts Unicode scalars and rejects malformed byte sequences") {
    const std::vector<std::string> valid{
        "", std::string(1, '\0'), "\x7F", "\xC2\x80", "\xDF\xBF",
        "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80", "\xEF\xBF\xBF",
        "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF", "\xEF\xBB\xBF", chinese, emoji};
    for (const auto& text : valid) {
        CHECK(astra::utf8::is_valid(text));
        CHECK_NOTHROW(astra::utf8::validate(text));
        check_rope(astra::Rope(text), text);
    }
    const std::vector<std::string> invalid{
        "\x80", "\xBF", "\xC0\x80", "\xC1\xBF", "\xC2", "\xC2\x7F", "\xDF",
        "\xE0\x80\x80", "\xE0\x9F\xBF", "\xE1\x80", "\xED\xA0\x80",
        "\xED\xBF\xBF", "\xEF\xBF", "\xF0\x80\x80\x80", "\xF0\x8F\xBF\xBF",
        "\xF1\x80\x80", "\xF4\x90\x80\x80", "\xF5\x80\x80\x80", "\xFE", "\xFF"};
    for (const auto& text : invalid) {
        CHECK_FALSE(astra::utf8::is_valid(text));
        CHECK_THROWS_AS((void)(astra::utf8::validate(text)), std::invalid_argument);
        CHECK_THROWS_AS((void)(astra::Rope{text}), std::invalid_argument);
        CHECK_THROWS_AS((void)(astra::Rope("safe").replace(1, 2, text)), std::invalid_argument);
    }
    CHECK(astra::utf8::decode(emoji, 0).value == U'\U0001F600');
    CHECK(astra::utf8::decode(emoji, 0).bytes == 4);
    CHECK_THROWS_AS((void)(astra::utf8::decode("", 0)), std::out_of_range);
    CHECK_THROWS_AS((void)(astra::utf8::byte_offset_from_utf16(emoji, 1)), std::invalid_argument);
    CHECK_THROWS_AS((void)(astra::utf8::byte_offset_from_utf16("a", 2)), std::out_of_range);
}

TEST_CASE("Rope empty text has one empty line and checked boundaries") {
    const astra::Rope rope;
    check_rope(rope, "");
    CHECK(rope.line_count() == 1);
    CHECK(rope.chunk_count() == 0);
    CHECK(rope.tree_height() == 0);
    CHECK(rope.line_start(0) == 0);
    CHECK(rope.line_end(0) == 0);
    CHECK(rope.slice(0, 0).empty());
    CHECK(rope.byte_to_position(0) == astra::TextPosition{0, 0});
    CHECK(rope.position_to_byte({0, 0}) == 0);
    CHECK(rope.byte_to_utf16(0) == 0);
    CHECK(rope.utf16_to_byte(0) == 0);
    CHECK(rope.is_boundary(0));
    CHECK_FALSE(rope.is_boundary(1));
    const auto [before, after] = rope.split(0);
    CHECK(before.empty());
    CHECK(after.empty());
    CHECK_THROWS_AS((void)(rope.split(1)), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.slice(1, 0)), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.slice(0, 1)), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.replace(1, 0, "a")), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.byte_to_utf16(1)), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.utf16_to_byte(1)), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.byte_to_position(1)), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.position_to_byte({1, 0})), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.position_to_byte({0, 1})), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.line_start(1)), std::out_of_range);
    CHECK_THROWS_AS((void)(rope.line_end(1)), std::out_of_range);
}

TEST_CASE("Rope UTF-8 byte edits reject scalar interiors and UTF-16 surrogate interiors") {
    const auto text = "A" + chinese + emoji + combining + "Z";
    const astra::Rope rope(text);
    const auto valid = boundaries(text);
    for (std::size_t byte = 0; byte <= text.size(); ++byte) {
        const auto is_valid = std::binary_search(valid.begin(), valid.end(), byte);
        CHECK(rope.is_boundary(byte) == is_valid);
        if (is_valid) {
            check_position(rope, text, byte);
        } else {
            CHECK_THROWS_AS((void)(rope.split(byte)), std::invalid_argument);
            CHECK_THROWS_AS((void)(rope.slice(byte, byte)), std::invalid_argument);
            CHECK_THROWS_AS((void)(rope.replace(byte, byte, "x")), std::invalid_argument);
            CHECK_THROWS_AS((void)(rope.byte_to_utf16(byte)), std::invalid_argument);
            CHECK_THROWS_AS((void)(rope.byte_to_position(byte)), std::invalid_argument);
        }
    }
    CHECK_THROWS_AS((void)(rope.utf16_to_byte(4)), std::invalid_argument);
    CHECK_THROWS_AS((void)(rope.position_to_byte({0, 4})), std::invalid_argument);
    // Combining marks remain separate scalar boundaries; grapheme-aware cursor
    // commands are a higher-level concern and must not change Rope semantics.
    CHECK(rope.is_boundary(12));
}

TEST_CASE("Rope preserves CRLF and computes content ends and exact raw positions") {
    const std::string text = "one\r\n\n" + chinese + "\r\n" + emoji + "\rfinal\n";
    const astra::Rope rope(text);
    const auto starts = reference_line_starts(text);
    REQUIRE(rope.line_count() == 5);
    for (std::size_t line = 0; line < starts.size(); ++line) {
        CHECK(rope.line_start(line) == starts[line]);
        auto end = line + 1 == starts.size() ? text.size() : starts[line + 1] - 1;
        if (line + 1 != starts.size() && end > starts[line] && text[end - 1] == '\r') {
            --end;
        }
        CHECK(rope.line_end(line) == end);
    }
    for (const auto byte : boundaries(text)) {
        check_position(rope, text, byte);
    }
    CHECK(rope.position_to_byte({0, 4}) == 4); // Between CR and LF is representable.
    CHECK(rope.line_end(0) == 3);
    CHECK_THROWS_AS((void)(rope.position_to_byte({0, 5})), std::out_of_range);
    CHECK(astra::Rope("a\rb").line_count() == 1);
    CHECK(astra::Rope("a\r").line_end(0) == 2);
}

TEST_CASE("Rope chunks never split UTF-8 and newline summaries cross chunk boundaries") {
    const std::vector<std::string> endings{emoji, chinese, combining, "\r\n", "\n"};
    for (const auto& ending : endings) {
        for (std::size_t padding = 1019; padding < 1026; ++padding) {
            const auto text = std::string(padding, 'a') + ending + std::string(2050, 'b') + "\n";
            const astra::Rope rope(text);
            check_rope(rope, text);
            CHECK(rope.chunk_count() >= 3);
            CHECK(rope.chunk_count() <= 5);
            for (const auto byte : boundaries(text)) {
                if (byte < 10 || (byte >= 1015 && byte <= 1040) || byte >= text.size() - 5) {
                    check_position(rope, text, byte);
                }
            }
            const auto starts = reference_line_starts(text);
            for (std::size_t line = 0; line < starts.size(); ++line) {
                CHECK(rope.line_start(line) == starts[line]);
            }
        }
    }
}

TEST_CASE("Text summaries are associative across LF CRLF and Unicode splits") {
    const std::vector<std::string> pieces{"", "a", "\r", "\n", "\r\n", chinese, emoji, "x\ny"};
    for (const auto& first : pieces) {
        for (const auto& second : pieces) {
            for (const auto& third : pieces) {
                const auto a = astra::MeasureText{}(first);
                const auto b = astra::MeasureText{}(second);
                const auto c = astra::MeasureText{}(third);
                CHECK((a + b) + c == a + (b + c));
                CHECK((a + b) + c == astra::MeasureText{}(first + second + third));
                CHECK(a + astra::TextSummary{} == a);
                CHECK(astra::TextSummary{} + a == a);
            }
        }
    }
}

TEST_CASE("Rope split concat slice and replacement preserve earlier snapshots") {
    const auto text = std::string(1018, 'x') + chinese + emoji + "\r\n" +
                      std::string(2050, 'y') + "\nend";
    const astra::Rope original(text);
    const auto valid = boundaries(text);
    for (const auto byte : valid) {
        if (byte % 37 != 0 && !(byte >= 1015 && byte <= 1035) && byte != text.size()) {
            continue;
        }
        CAPTURE(byte);
        const auto [before, after] = original.split(byte);
        check_rope(before, text.substr(0, byte));
        check_rope(after, text.substr(byte));
        check_rope(before.concat(after), text);
        CHECK(original.slice(0, byte) == text.substr(0, byte));
        CHECK(original.slice(byte, text.size()) == text.substr(byte));
        const auto modified = original.replace(byte, byte, emoji + "\n");
        check_rope(modified, text.substr(0, byte) + emoji + "\n" + text.substr(byte));
        check_rope(original, text);
    }
    check_rope(original.replace(0, text.size(), ""), "");
    check_rope(original.replace(0, text.size(), chinese), chinese);
    check_rope(original.replace(0, 0, ""), text);
    check_rope(astra::Rope{}.concat(original), text);
    check_rope(original.concat(astra::Rope{}), text);
}

TEST_CASE("Rope repeated small insertions coalesce chunks and retain logarithmic height") {
    astra::Rope rope;
    std::string expected;
    for (std::size_t index = 0; index < 12000; ++index) {
        const std::size_t byte = index % 2 == 0 ? 0 : rope.size_bytes();
        rope = rope.replace(byte, byte, "x");
        expected.insert(byte, "x");
        if (index % 251 == 0) {
            REQUIRE(rope.validate());
        }
    }
    check_rope(rope, expected);
    CHECK(rope.chunk_count() <= 14);
    CHECK(rope.tree_height() <= 6);
    const auto snapshot = rope;
    for (std::size_t index = 0; index < 3000; ++index) {
        rope = rope.replace(500, 501, emoji);
        rope = rope.replace(500, 504, "x");
    }
    check_rope(rope, expected);
    check_rope(snapshot, expected);
    CHECK(rope.chunk_count() <= 16);
}

TEST_CASE("Rope deterministic randomized differential editing and coordinate mapping") {
    constexpr std::uint32_t seed = 0xA57A2026U;
    CAPTURE(seed);
    std::mt19937 random(seed);
    std::string expected = random_text(random, 3000);
    astra::Rope rope(expected);
    std::vector<std::pair<astra::Rope, std::string>> snapshots;
    for (std::size_t step = 0; step < 3000; ++step) {
        CAPTURE(step);
        const auto valid = boundaries(expected);
        auto begin = valid[random() % valid.size()];
        auto end = valid[random() % valid.size()];
        if (begin > end) {
            std::swap(begin, end);
        }
        if (random() % 3U != 0) {
            end = begin;
        }
        const auto inserted = random_text(random, step % 31 == 0 ? 1400 : 30);
        rope = rope.replace(begin, end, inserted);
        expected.replace(begin, end - begin, inserted);
        check_rope(rope, expected);
        const auto after_boundaries = boundaries(expected);
        for (std::size_t sample = 0; sample < 5; ++sample) {
            const auto byte = after_boundaries[random() % after_boundaries.size()];
            check_position(rope, expected, byte);
        }
        auto slice_begin = after_boundaries[random() % after_boundaries.size()];
        auto slice_end = after_boundaries[random() % after_boundaries.size()];
        if (slice_begin > slice_end) {
            std::swap(slice_begin, slice_end);
        }
        CHECK(rope.slice(slice_begin, slice_end) ==
              expected.substr(slice_begin, slice_end - slice_begin));
        if (step % 37 == 0) {
            const auto [before, after] = rope.split(slice_begin);
            check_rope(before, expected.substr(0, slice_begin));
            check_rope(after, expected.substr(slice_begin));
            check_rope(before.concat(after), expected);
        }
        if (step % 101 == 0) {
            snapshots.emplace_back(rope, expected);
        }
    }
    for (const auto& [snapshot, contents] : snapshots) {
        check_rope(snapshot, contents);
        const auto starts = reference_line_starts(contents);
        for (std::size_t line = 0; line < starts.size(); ++line) {
            CHECK(snapshot.line_start(line) == starts[line]);
        }
    }
}

TEST_CASE("Rope rejects summary overflow before changing shared snapshots") {
    auto rope = astra::Rope("x");
    for (int bit = 1; bit < std::numeric_limits<std::size_t>::digits; ++bit) {
        rope = rope.concat(rope);
    }
    const auto length = rope.size_bytes();
    CHECK(length == (std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1)));
    CHECK_THROWS_AS((void)(rope.concat(rope)), std::length_error);
    CHECK(rope.size_bytes() == length);
    CHECK(rope.slice(length - 1, length) == "x");
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const astra::TextSummary first{1, 1, maximum - 1, 0, 0};
    const astra::TextSummary last{1, 1, 1, 0, 0};
    CHECK_THROWS_AS((void)(first + last), std::length_error);
}

TEST_CASE("Rope changed extents refine rebuilt chunks without including a neighboring long line") {
    const std::string source = "int before;\r\n// \xf0\x9f\x98\x80\r\nint after;\r\n" + std::string(30000, ' ');
    const astra::Rope before(source);
    const auto position = source.find("after");
    const auto after = before.replace(position, position + 5, "changed");
    const auto extent = before.changed_extent(after);
    CHECK(extent.begin == position);
    CHECK(extent.old_end == position + 5);
    CHECK(extent.new_end == position + 7);
    CHECK(before.replace(extent.begin, extent.old_end, after.slice(extent.begin, extent.new_end)).str() == after.str());
    const auto independent = before.changed_extent(astra::Rope(after.str()));
    CHECK(independent.begin == extent.begin);
    CHECK(independent.old_end == extent.old_end);
    CHECK(independent.new_end == extent.new_end);
    const astra::Rope before_emoji("\xf0\x9f\x98\x80");
    const auto scalar = before_emoji.changed_extent(astra::Rope("\xf0\x9f\x98\x81"));
    CHECK(scalar.begin == 0);
    CHECK(scalar.old_end == 4);
    CHECK(scalar.new_end == 4);
}
