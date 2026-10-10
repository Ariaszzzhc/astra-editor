#include <astra/ui/text_source.hpp>
#include <astra/core/rope.hpp>
#include <ostream>
#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace {
// Independent leading-byte reference for valid fixture text, including the
// interior UTF-16 surrogate column which layout APIs snap to its scalar start.
std::vector<std::size_t> columns(std::string_view text) {
    std::vector<std::size_t> result;
    for (std::size_t byte = 0; byte < text.size();) {
        const auto lead = static_cast<unsigned char>(text[byte]);
        const auto width = lead < 0x80 ? 1U : lead < 0xE0 ? 2U : lead < 0xF0 ? 3U : 4U;
        result.push_back(byte);
        if (width == 4)
            result.push_back(byte);
        byte += width;
    }
    result.push_back(text.size());
    return result;
}

void check(const astra::ui::TextSource& source, std::string_view expected) {
    REQUIRE(source.text() == expected);
    const auto reference = columns(expected);
    CHECK(source.statistics().bytes == expected.size());
    CHECK(source.statistics().utf16_units == reference.size() - 1);
    for (std::size_t column = 0; column < reference.size(); ++column) {
        CHECK(source.utf16_to_byte(column) == reference[column]);
        if (column == 0 || reference[column] != reference[column - 1])
            CHECK(source.byte_to_utf16(reference[column]) == column);
    }
    CHECK(source.utf16_to_byte(std::numeric_limits<std::size_t>::max()) == expected.size());
}
} // namespace

TEST_CASE("text source uses measured coordinates and snaps surrogate interiors") {
    check({}, "");
    check(astra::ui::TextSource("A中🙂é\tאבג\r\nZ"), "A中🙂é\tאבג\r\nZ");
    const std::string edge = std::string(1023, 'a') + "🙂中";
    check(astra::ui::TextSource(edge), edge);
    const astra::Rope rope(edge);
    CHECK(rope.utf16_to_byte_floor(1024) == 1023);
    CHECK_THROWS_AS((void)rope.utf16_to_byte(1024), std::invalid_argument);
    CHECK(rope.utf16_to_byte_floor(1025) == 1027);
    CHECK(rope.utf16_to_byte_floor(std::numeric_limits<std::size_t>::max()) == edge.size());
    CHECK_THROWS_AS((void)astra::ui::TextSource("中").byte_to_utf16(1), std::invalid_argument);
    CHECK_THROWS_AS((void)astra::ui::TextSource{}.byte_to_utf16(1), std::out_of_range);
}

TEST_CASE("text source localized edits retain measured subtrees and immutable text") {
    const std::string original = std::string(32768, 'a') + "中🙂é" + std::string(32768, 'z');
    astra::ui::TextSource source(original);
    auto changed = original;
    changed.replace(32768, 3, "文");
    const auto updated = source.updated(changed);
    CHECK(updated.text() == changed);
    CHECK(source.text() == original);
    CHECK(updated.statistics().shared_prefix_bytes >= 32768 - 2 * astra::Rope::max_chunk_bytes);
    CHECK(updated.statistics().shared_suffix_bytes >= 32768 - 2 * astra::Rope::max_chunk_bytes);
    CHECK(source.statistics().chunks > 32);
    CHECK(updated.utf16_to_byte(32769) == 32771);
    CHECK(updated.byte_to_utf16(32775) == 32771);
    CHECK(updated.updated(changed).text().data() == updated.text().data());
    CHECK_THROWS_AS((void)source.updated(original.substr(0, 32769)), std::invalid_argument);
    auto malformed = original;
    malformed[32769] = '\xFF';
    CHECK_THROWS_AS((void)source.updated(malformed), std::invalid_argument);
    CHECK(source.text() == original);
    check(source.updated(""), "");
}

TEST_CASE("text source randomized edits match flat Unicode coordinates and retained snapshots") {
    constexpr unsigned seed = 0xA57A82;
    INFO(seed);
    std::mt19937 random(seed);
    const std::vector<std::string> pieces{"a", "中", "🙂", "é", "אב", "\t", "\r\n", ""};
    std::string expected;
    for (int i = 0; i < 800; ++i)
        expected += pieces[random() % pieces.size()];
    astra::ui::TextSource source(expected);
    std::vector<std::pair<astra::ui::TextSource, std::string>> snapshots;
    for (int edit = 0; edit < 250; ++edit) {
        if (edit % 40 == 0)
            snapshots.emplace_back(source, expected);
        auto positions = columns(expected);
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
        auto begin = positions[random() % positions.size()];
        auto end = positions[random() % positions.size()];
        if (begin > end)
            std::swap(begin, end);
        // Keep most of the source live while testing adjacent scalar edits.
        end = std::min(end,
                       *std::lower_bound(positions.begin(),
                                         positions.end(),
                                         std::min(expected.size(), begin + 20)));
        const auto replacement = pieces[random() % pieces.size()];
        expected.replace(begin, end - begin, replacement);
        source = source.updated(expected);
        check(source, expected);
    }
    for (const auto& [snapshot, text] : snapshots)
        check(snapshot, text);
}

TEST_CASE("text source invalid scalar updates do not publish partial coordinate state") {
    const astra::ui::TextSource source("a中🙂z");
    for (const auto& invalid : std::vector<std::string>{std::string("a\xE4\xB8", 3),
                                                        std::string("\x80", 1),
                                                        std::string("a\xC0\x80", 3),
                                                        std::string("a\xED\xA0\x80", 4),
                                                        std::string("a\xF4\x90\x80\x80", 5),
                                                        std::string("a\xF0\x9F\x99z", 5)}) {
        CHECK_THROWS_AS((void)source.updated(invalid), std::invalid_argument);
        check(source, "a中🙂z");
    }
}
