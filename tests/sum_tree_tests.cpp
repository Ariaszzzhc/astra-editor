#include "astra/core/sum_tree.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

struct Entry {
    int value{};
    std::size_t weight{};
    friend bool operator==(const Entry&, const Entry&) = default;
};

// The rolling hash makes this summary noncommutative: a reversed rotation or
// incorrectly composed prefix cannot pass merely because sums still match.
struct EntrySummary {
    std::size_t count{};
    std::size_t weight{};
    std::uint64_t hash{};
    std::uint64_t power{1};

    friend bool operator==(const EntrySummary&, const EntrySummary&) = default;

    friend EntrySummary operator+(const EntrySummary& left, const EntrySummary& right) {
        return {left.count + right.count,
                left.weight + right.weight,
                left.hash * right.power + right.hash,
                left.power * right.power};
    }
};

struct MeasureEntry {
    EntrySummary operator()(const Entry& entry) const {
        return {1, entry.weight, static_cast<std::uint64_t>(entry.value + 10001), 131};
    }
};

using Tree = astra::SumTree<Entry, EntrySummary, MeasureEntry>;

EntrySummary reference_summary(const std::vector<Entry>& entries) {
    EntrySummary result;
    for (const auto& entry : entries) {
        result = result + MeasureEntry{}(entry);
    }
    return result;
}

std::vector<Entry> flatten(const Tree& tree) {
    std::vector<Entry> result;
    tree.for_each([&result](const Entry& entry) {
        result.push_back(entry);
    });
    return result;
}

void check_tree(const Tree& tree, const std::vector<Entry>& expected) {
    REQUIRE(tree.validate());
    CHECK(tree.size() == expected.size());
    CHECK(tree.empty() == expected.empty());
    CHECK(tree.summary() == reference_summary(expected));
    CHECK(flatten(tree) == expected);
}

void check_seek(const Tree& tree, const std::vector<Entry>& expected, std::size_t offset) {
    const auto actual = tree.seek(offset, &EntrySummary::weight);
    EntrySummary prefix;
    std::size_t index = 0;
    while (index < expected.size() && prefix.weight + expected[index].weight <= offset) {
        prefix = prefix + MeasureEntry{}(expected[index]);
        ++index;
    }
    CHECK(actual.index == index);
    CHECK(actual.prefix == prefix);
    if (index == expected.size()) {
        CHECK(actual.item == nullptr);
    } else {
        REQUIRE(actual.item != nullptr);
        CHECK(*actual.item == expected[index]);
    }
}

} // namespace

TEST_CASE("shared tree edges skip immutable interiors and preserve summary order") {
    std::vector<Entry> entries;
    for (int i = 0; i < 4096; ++i) {
        entries.push_back({i, static_cast<std::size_t>(i % 7)});
    }
    const Tree original(entries);
    CHECK(original.shared_prefix_summary(original) == original.summary());
    CHECK(original.shared_suffix_summary(original) == original.summary());
    for (const std::size_t position : {0U, 1U, 31U, 1023U, 2048U, 4095U, 4096U}) {
        const auto inserted = original.insert(position, {-1, 10});
        CHECK(original.shared_prefix_summary(inserted) == original.prefix_summary(position));
        const auto tail = original.split(position).second;
        CHECK(original.shared_suffix_summary(inserted) == tail.summary());
        CHECK(inserted.shared_prefix_summary(original) == original.prefix_summary(position));
        CHECK(inserted.shared_suffix_summary(original) == tail.summary());
    }
    const Tree rebuilt(entries);
    CHECK(original.shared_prefix_summary(rebuilt) == EntrySummary{});
    CHECK(original.shared_suffix_summary(rebuilt) == EntrySummary{});
    CHECK(original.shared_prefix_summary(Tree{}) == EntrySummary{});
}

TEST_CASE("SumTree empty identity and checked ranges") {
    const Tree tree;
    check_tree(tree, {});
    CHECK(tree.height() == 0);
    CHECK(tree.prefix_summary(0) == EntrySummary{});
    CHECK(tree.seek(0, &EntrySummary::weight).item == nullptr);
    const auto [before, after] = tree.split(0);
    CHECK(before.empty());
    CHECK(after.empty());
    CHECK(tree.concat(tree).empty());
    CHECK(tree.erase(0, 0).empty());
    CHECK_THROWS_AS((void)(tree.at(0)), std::out_of_range);
    CHECK_THROWS_AS((void)(tree.split(1)), std::out_of_range);
    CHECK_THROWS_AS((void)(tree.prefix_summary(1)), std::out_of_range);
    CHECK_THROWS_AS((void)(tree.seek(1, &EntrySummary::weight)), std::out_of_range);
    CHECK_THROWS_AS((void)(tree.insert(1, Entry{1, 1})), std::out_of_range);
    CHECK_THROWS_AS((void)(tree.erase(1, 0)), std::out_of_range);
    CHECK_THROWS_AS((void)(tree.erase(0, 1)), std::out_of_range);
    CHECK_THROWS_AS((void)(tree.for_each_range(0,
                                               1,
                                               [](const Entry&) {
                                               })),
                    std::out_of_range);
}

TEST_CASE("SumTree summary pruning retains original ordinals and skips empty interiors") {
    std::vector<Entry> entries(8192, Entry{1, 0});
    entries[9].weight = 1;
    entries[8000].weight = 2;
    const Tree tree(entries);
    std::vector<std::size_t> ordinals;
    std::size_t nodes{};
    tree.for_each_pruned(
        [&](const EntrySummary& summary) {
            ++nodes;
            return summary.weight != 0;
        },
        [&](std::size_t ordinal, const Entry& value) {
            CHECK(value == entries.at(ordinal));
            ordinals.push_back(ordinal);
        });
    CHECK(ordinals == std::vector<std::size_t>{9, 8000});
    CHECK(nodes < 100);
    const auto retained = tree;
    const auto changed = tree.erase(9, 10);
    ordinals.clear();
    changed.for_each_pruned(
        [](const EntrySummary& summary) {
            return summary.weight != 0;
        },
        [&](std::size_t ordinal, const Entry&) {
            ordinals.push_back(ordinal);
        });
    CHECK(ordinals == std::vector<std::size_t>{7999});
    CHECK(retained.size() == 8192);
    CHECK(retained.at(9).weight == 1);
    auto publishing = tree;
    ordinals.clear();
    publishing.for_each_pruned(
        [](const EntrySummary& summary) {
            return summary.weight != 0;
        },
        [&](std::size_t ordinal, const Entry&) {
            ordinals.push_back(ordinal);
            publishing = Tree{};
        });
    CHECK(ordinals == std::vector<std::size_t>{9, 8000});
    CHECK(publishing.empty());
    CHECK_THROWS_AS(tree.for_each_pruned(
                        [](const EntrySummary&) -> bool {
                            throw std::runtime_error("predicate failure");
                        },
                        [](std::size_t, const Entry&) {
                        }),
                    std::runtime_error);
    CHECK(tree.validate());
}

TEST_CASE("SumTree composition preserves ordering and zero-width leaves") {
    const std::vector<Entry> entries{{7, 0}, {3, 2}, {9, 0}, {4, 3}, {2, 0}};
    const Tree tree(entries);
    check_tree(tree, entries);
    for (std::size_t offset = 0; offset <= tree.summary().weight; ++offset) {
        check_seek(tree, entries, offset);
    }
    EntrySummary prefix;
    for (std::size_t count = 0; count <= entries.size(); ++count) {
        CHECK(tree.prefix_summary(count) == prefix);
        if (count != entries.size()) {
            CHECK(tree.at(count) == entries[count]);
            prefix = prefix + MeasureEntry{}(entries[count]);
        }
    }
    const Tree zeros(std::vector<Entry>{{1, 0}, {2, 0}, {3, 0}});
    CHECK(zeros.seek(0, &EntrySummary::weight).index == 3);
    CHECK(zeros.seek(0, &EntrySummary::weight).prefix == zeros.summary());
}

TEST_CASE("SumTree split at every boundary retains shared leaves") {
    std::vector<Entry> entries;
    for (int value = 0; value < 257; ++value) {
        entries.push_back({value, static_cast<std::size_t>(value % 7)});
    }
    const Tree tree(entries);
    for (std::size_t index = 0; index <= entries.size(); ++index) {
        CAPTURE(index);
        const auto [before, after] = tree.split(index);
        check_tree(before, std::vector<Entry>(entries.begin(), entries.begin() + index));
        check_tree(after, std::vector<Entry>(entries.begin() + index, entries.end()));
        check_tree(before.concat(after), entries);
        if (index != 0) {
            CHECK(std::addressof(before.at(index - 1)) == std::addressof(tree.at(index - 1)));
        }
        if (index != entries.size()) {
            CHECK(std::addressof(after.at(0)) == std::addressof(tree.at(index)));
        }
    }
}

TEST_CASE("SumTree visits only the requested half-open range") {
    std::vector<Entry> entries;
    for (int value = 0; value < 33; ++value) {
        entries.push_back({value, 1});
    }
    const Tree tree(entries);
    for (std::size_t begin = 0; begin <= entries.size(); ++begin) {
        for (std::size_t end = begin; end <= entries.size(); ++end) {
            std::vector<Entry> actual;
            tree.for_each_range(begin, end, [&actual](const Entry& entry) {
                actual.push_back(entry);
            });
            CHECK(actual == std::vector<Entry>(entries.begin() + begin, entries.begin() + end));
        }
    }
    CHECK_THROWS_AS((void)(tree.for_each_range(2,
                                               1,
                                               [](const Entry&) {
                                               })),
                    std::out_of_range);
}

TEST_CASE("SumTree AVL joins stay balanced under adversarial append and prepend") {
    Tree forward;
    Tree backward;
    constexpr int count = 4096;
    for (int index = 0; index < count; ++index) {
        const auto item = Tree::singleton({index, 1});
        forward = forward.concat(item);
        backward = item.concat(backward);
        if (index % 31 == 0) {
            REQUIRE(forward.validate());
            REQUIRE(backward.validate());
        }
    }
    CHECK(forward.height() <= 18);
    CHECK(backward.height() <= 18);
    REQUIRE(forward.validate());
    REQUIRE(backward.validate());
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        CHECK(forward.at(index).value == static_cast<int>(index));
        CHECK(backward.at(index).value == count - 1 - static_cast<int>(index));
    }
    const auto old = forward;
    auto middle = forward.split(1777).second;
    middle = middle.erase(100, 200);
    REQUIRE(middle.validate());
    CHECK(old.size() == count);
    CHECK(old.at(1877).value == 1877);
    CHECK(middle.at(100).value == 1977);
}

TEST_CASE("SumTree deterministic randomized edits match a vector and preserve snapshots") {
    constexpr std::uint32_t seed = 0x51A7E202U;
    CAPTURE(seed);
    std::mt19937 random(seed);
    Tree tree;
    std::vector<Entry> expected;
    std::vector<std::pair<Tree, std::vector<Entry>>> snapshots;
    for (std::size_t step = 0; step < 5000; ++step) {
        CAPTURE(step);
        const auto operation = random() % 5U;
        const auto first = static_cast<std::size_t>(random()) % (expected.size() + 1);
        if (operation < 2 || expected.empty()) {
            const Entry entry{static_cast<int>(random() % 1000U), random() % 9U};
            tree = tree.insert(first, entry);
            expected.insert(expected.begin() + first, entry);
        } else if (operation == 2) {
            const auto second = static_cast<std::size_t>(random()) % (expected.size() + 1);
            const auto begin = std::min(first, second);
            const auto end = std::max(first, second);
            tree = tree.erase(begin, end);
            expected.erase(expected.begin() + begin, expected.begin() + end);
        } else if (operation == 3) {
            const auto [before, after] = tree.split(first);
            REQUIRE(before.validate());
            REQUIRE(after.validate());
            tree = before.concat(after);
        } else {
            const auto [before, after] = tree.split(first);
            tree = after.concat(before);
            std::rotate(expected.begin(), expected.begin() + first, expected.end());
        }
        check_tree(tree, expected);
        const auto offset = static_cast<std::size_t>(random()) % (tree.summary().weight + 1);
        check_seek(tree, expected, offset);
        if (step % 97 == 0) {
            snapshots.emplace_back(tree, expected);
        }
    }
    for (const auto& [snapshot, contents] : snapshots) {
        check_tree(snapshot, contents);
    }
}

TEST_CASE("SumTree rejects item count overflow even with tiny shared storage") {
    auto tree = Tree::singleton({1, 0});
    for (int bit = 1; bit < std::numeric_limits<std::size_t>::digits; ++bit) {
        tree = tree.concat(tree);
    }
    const auto count = tree.size();
    CHECK(count == (std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 1)));
    CHECK_THROWS_AS((void)(tree.concat(tree)), std::length_error);
    CHECK(tree.size() == count);
    CHECK(tree.at(count - 1) == Entry{1, 0});
}
