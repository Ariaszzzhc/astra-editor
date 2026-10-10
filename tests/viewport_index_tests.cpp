#include <astra/ui/viewport_index.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace astra::ui;

std::vector<ViewportLine>
page(const std::vector<std::string>& source, std::size_t first, std::size_t count) {
    std::vector<ViewportLine> rows;
    std::size_t offset{};
    for (std::size_t line = 0; line < source.size(); ++line) {
        const auto end = offset + source[line].size();
        const auto next = end + (line % 7 == 0 ? 2 : 1);
        if (line >= first && line - first < count)
            rows.push_back({line, offset, end, next, true, source[line]});
        offset = next;
    }
    return rows;
}

void check_page(const ViewportIndex& index, const std::vector<ViewportLine>& expected) {
    CHECK(index.validate());
    CHECK(index.size() == expected.size());
    std::vector<ViewportLine> actual;
    std::size_t bytes{};
    index.for_each([&](const auto& line) {
        actual.push_back(line);
        bytes += line.text.size();
    });
    CHECK(actual == expected);
    CHECK(index.summary().utf8_bytes == bytes);
    CHECK(index.summary().rows == expected.size());
    for (const auto& line : expected) {
        REQUIRE(index.find(line.number));
        CHECK(*index.find(line.number) == line);
    }
    if (!expected.empty()) {
        CHECK(index.summary().begin == expected.front().begin);
        CHECK(index.summary().end == expected.back().end);
        CHECK_FALSE(index.find(expected.back().number + 1));
        if (expected.front().number)
            CHECK_FALSE(index.find(expected.front().number - 1));
    }
}
} // namespace

TEST_CASE("viewport rows preserve snapshots and share local edits and scroll overlap") {
    std::vector<std::string> source;
    for (int i = 0; i < 128; ++i)
        source.push_back("row " + std::to_string(i) + " 中🙂é");
    ViewportIndex index;
    const auto original = page(source, 0, 128);
    index.reset(original);
    const auto retained = index;
    source[60][0] = 'R';
    const auto edited = page(source, 0, 128);
    index.reset(edited);
    check_page(index, edited);
    check_page(retained, original);
    CHECK(index.shared_prefix_rows(retained) == 60);
    CHECK(index.shared_suffix_rows(retained) == 67);
    const auto before_scroll = index;
    const auto scrolled = page(source, 20, 70);
    index.reset(scrolled);
    check_page(index, scrolled);
    CHECK(index.shared_prefix_rows(before_scroll.row_range(20, 90)) == 70);
    CHECK_THROWS_AS((void)index.row_range(0, 71), std::out_of_range);
    CHECK_THROWS_AS((void)index.row_range(2, 1), std::out_of_range);
    CHECK(index.row_range(70, 70).empty());
}

TEST_CASE("viewport publication validates complete contiguous Unicode rows before mutation") {
    ViewportIndex index;
    const auto original = page({"中🙂", "", "last"}, 0, 3);
    index.reset(original);
    for (int invalid = 0; invalid < 7; ++invalid) {
        auto rows = original;
        if (invalid == 0)
            rows.front().complete = false;
        else if (invalid == 1)
            rows.front().end = 100;
        else if (invalid == 2)
            rows.front().next = 0;
        else if (invalid == 3)
            rows.back().number = 4;
        else if (invalid == 4)
            ++rows.back().begin;
        else if (invalid == 5)
            rows.front().text[0] = static_cast<char>(0xff);
        else
            rows.front().text += "extra";
        CHECK_THROWS_AS(index.reset(std::move(rows)), std::invalid_argument);
        check_page(index, original);
    }
    CHECK_THROWS_AS(index.reset(page(std::vector<std::string>(257, "x"), 0, 257)),
                    std::length_error);
    check_page(index, original);
    index.reset({});
    CHECK(index.empty());
    CHECK(index.summary() == ViewportSummary{});
    CHECK_FALSE(index.find(0));
}

TEST_CASE("viewport measured publication matches flat pages through edits and scrolling") {
    constexpr unsigned int seed = 0xA57AD15;
    INFO(seed);
    std::mt19937 random(seed);
    std::vector<std::string> source(180, "中🙂é\tvalue");
    ViewportIndex index;
    std::vector<std::pair<ViewportIndex, std::vector<ViewportLine>>> snapshots;
    for (int iteration = 0; iteration < 200; ++iteration) {
        INFO(iteration);
        const auto line = random() % source.size();
        if (iteration % 3 == 0)
            source.insert(source.begin() + line, "inserted 中🙂");
        else if (iteration % 3 == 1)
            source.erase(source.begin() + line);
        else
            source[line] = "changed " + std::to_string(random()) + " é中🙂";
        const auto rows = page(source, random() % source.size(), 1 + random() % 70);
        index.reset(rows);
        check_page(index, rows);
        if (iteration % 20 == 0)
            snapshots.emplace_back(index, rows);
        if (rows.size() >= 3) {
            const auto a = MeasureViewportLine{}(rows[0]), b = MeasureViewportLine{}(rows[1]),
                       c = MeasureViewportLine{}(rows[2]);
            CHECK((a + b) + c == a + (b + c));
            CHECK(ViewportSummary{} + a == a);
            CHECK(a + ViewportSummary{} == a);
        }
    }
    for (const auto& [snapshot, rows] : snapshots)
        check_page(snapshot, rows);
}
