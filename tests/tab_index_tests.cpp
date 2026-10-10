#include <astra/ui/tab_index.hpp>
#include <astra/ui/workbench_layout.hpp>
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>
#include <random>

namespace {
struct ExpectedTab {
    std::string id, label;
    float width{110};
    bool prepared{};
};

void compare(const astra::ui::TabIndex& tabs, const std::vector<ExpectedTab>& expected) {
    const auto tree = tabs.snapshot();
    REQUIRE(tree.validate());
    REQUIRE(tabs.size() == expected.size());
    CHECK(tree.summary().count == expected.size());
    double width{};
    std::size_t pending{};
    std::optional<std::size_t> first_pending;
    std::uint64_t previous{};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& entry = tree.at(i);
        CHECK(entry.id == expected[i].id);
        CHECK(entry.label == expected[i].label);
        CHECK(entry.width == expected[i].width);
        CHECK(entry.order > previous);
        CHECK(tabs.position(entry.id) == i);
        CHECK(tabs.prepared(i) == expected[i].prepared);
        CHECK(tree.prefix_summary(i).width == width);
        previous = entry.order;
        width += expected[i].width;
        if (!expected[i].prepared) {
            ++pending;
            if (!first_pending) {
                first_pending = i;
            }
        }
    }
    CHECK(tree.summary().width == width);
    CHECK(tabs.pending() == pending);
    CHECK(tabs.next_pending() == first_pending);
    CHECK(tree.height() <= 2 * static_cast<std::size_t>(std::ceil(std::log2(expected.size() + 1))));
}
} // namespace

TEST_CASE("presentation tab index differential retains identities widths and old snapshots") {
    constexpr std::uint32_t seed = 0xA57A7AB;
    CAPTURE(seed);
    std::mt19937 random(seed);
    astra::ui::TabIndex tabs;
    std::vector<ExpectedTab> expected;
    std::vector<std::pair<astra::ui::TabIndex::Snapshot, std::vector<ExpectedTab>>> snapshots;
    unsigned next_id{};
    for (unsigned step = 0; step < 2000; ++step) {
        if (step % 71 == 0) {
            snapshots.emplace_back(tabs.snapshot(), expected);
        }
        const auto action = random() % 6;
        if (expected.empty() || (action == 0 && expected.size() < 128)) {
            const auto id = std::to_string(next_id++);
            const auto label = "中🙂 tab " + id;
            tabs.append(id, label);
            expected.push_back({id, label});
        } else if (action == 1) {
            const auto index = random() % expected.size();
            tabs.erase(expected[index].id);
            expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (action == 2) {
            const auto source = random() % expected.size(), target = random() % expected.size();
            tabs.move(expected[source].id, target);
            auto entry = expected[source];
            expected.erase(expected.begin() + static_cast<std::ptrdiff_t>(source));
            expected.insert(expected.begin() + static_cast<std::ptrdiff_t>(target),
                            std::move(entry));
        } else if (action == 3) {
            const auto index = random() % expected.size();
            auto& entry = expected[index];
            entry.label = "renamed é " + std::to_string(step);
            entry.prepared = false;
            tabs.rename(entry.id, entry.label);
        } else if (action == 4) {
            tabs.invalidate();
            for (auto& entry : expected) {
                entry.prepared = false;
            }
        } else {
            const auto index = random() % expected.size();
            const float pixels = static_cast<float>(random() % 2000) / 4;
            tabs.measured(index, pixels, 1.25F);
            expected[index].width =
                static_cast<float>(std::clamp(pixels / 1.25 + 64, 110.0, 260.0));
            expected[index].prepared = true;
        }
        compare(tabs, expected);
    }
    for (const auto& [tree, previous] : snapshots) {
        REQUIRE(tree.validate());
        CHECK(tree.size() == previous.size());
        for (std::size_t i = 0; i < previous.size(); ++i) {
            CHECK(tree.at(i).id == previous[i].id);
            CHECK(tree.at(i).label == previous[i].label);
            CHECK(tree.at(i).width == previous[i].width);
        }
    }
}

TEST_CASE("presentation tab index count admission preserves a full bounded snapshot") {
    astra::ui::TabIndex tabs;
    for (std::size_t i = 0; i < astra::ui::TabIndex::max_tabs; ++i) {
        tabs.append(std::to_string(i), "tab");
    }
    const auto retained = tabs.snapshot();
    CHECK(retained.validate());
    CHECK_THROWS_AS(tabs.append("overflow", "tab"), std::length_error);
    CHECK(tabs.size() == astra::ui::TabIndex::max_tabs);
    CHECK(tabs.snapshot().shared_prefix_summary(retained).count == retained.size());
    CHECK(tabs.position("overflow") == std::nullopt);
}

TEST_CASE("presentation tab index font invalidation shares geometry and retires exact leaves") {
    astra::ui::TabIndex tabs;
    for (unsigned i = 0; i < 1000; ++i) {
        tabs.append(std::to_string(i), "label " + std::to_string(i));
        tabs.measured(i, static_cast<float>(i % 200), 1);
    }
    const auto retained = tabs.snapshot();
    tabs.invalidate();
    CHECK(tabs.pending() == 1000);
    CHECK(tabs.next_pending() == 0);
    CHECK(tabs.snapshot().shared_prefix_summary(retained).count == 1000);
    tabs.measured(500, 190, 1);
    const auto current = tabs.snapshot();
    CHECK(current.shared_prefix_summary(retained).count == 500);
    CHECK(current.shared_suffix_summary(retained).count == 499);
    CHECK(retained.at(500).width == 164);
    CHECK(current.at(500).width == 254);
    CHECK(tabs.pending() == 999);
    const auto ids = tabs.ids();
    CHECK_THROWS_AS(tabs.append("0", "duplicate"), std::invalid_argument);
    CHECK_THROWS_AS(tabs.move("missing", 0), std::out_of_range);
    CHECK_THROWS_AS(tabs.move("0", tabs.size()), std::out_of_range);
    CHECK_THROWS_AS(tabs.rename("0", std::string(4097, 'x')), std::length_error);
    CHECK_THROWS_AS(tabs.rename("0", "\xFF"), std::invalid_argument);
    CHECK_THROWS_AS(tabs.measured(0, -1, 1), std::invalid_argument);
    CHECK_THROWS_AS(tabs.measured(0, std::numeric_limits<float>::quiet_NaN(), 1),
                    std::invalid_argument);
    CHECK_THROWS_AS(tabs.measured(0, 10, 0), std::invalid_argument);
    CHECK(tabs.ids() == ids);
    CHECK(tabs.snapshot().shared_prefix_summary(current).count == 1000);
}

TEST_CASE("presentation tab index adversarial reordering relabels without changing measurements") {
    astra::ui::TabIndex tabs;
    for (unsigned i = 0; i < 64; ++i) {
        tabs.append(std::to_string(i), "tab");
        tabs.measured(i, 80, 1);
    }
    const auto retained = tabs.snapshot();
    for (unsigned step = 0; step < 256; ++step) {
        const auto moved = tabs.id_at(tabs.size() - 1);
        tabs.move(moved, 0);
        CHECK(tabs.position(moved) == 0);
        CHECK(tabs.snapshot().validate());
        CHECK(tabs.pending() == 0);
        CHECK(tabs.snapshot().summary().width == 64 * 144);
    }
    CHECK(tabs.relabels() > 0);
    for (unsigned i = 0; i < 64; ++i) {
        CHECK(retained.at(i).id == std::to_string(i));
        CHECK(retained.at(i).width == 144);
    }
}

TEST_CASE("presentation indexed tab strip agrees with flat reveal and clipped hit geometry") {
    constexpr std::uint32_t seed = 0xA571D3;
    CAPTURE(seed);
    std::mt19937 random(seed);
    astra::ui::TabIndex tabs;
    std::vector<double> widths;
    for (unsigned i = 0; i < 8192; ++i) {
        const auto label = static_cast<float>(random() % 1000) / 4;
        tabs.append(std::to_string(i), "tab");
        tabs.measured(i, label, 1);
        widths.push_back(std::clamp(static_cast<double>(label) + 64, 110.0, 260.0));
    }
    const auto tree = tabs.snapshot();
    for (unsigned step = 0; step < 1000; ++step) {
        const auto scale = std::array{1.0F, 1.25F, 1.5F, 2.0F}[random() % 4];
        const auto physical = static_cast<float>(random() % 2000);
        auto first = random() % widths.size();
        const auto active = random() % widths.size();
        const bool reveal = random() % 2 != 0;
        const auto strip = astra::ui::TabStrip::measure(
            {17, 32, physical, 34}, scale, tree, first, active, reveal);
        const auto available =
            static_cast<double>(physical - 2 * std::min(26 * scale, physical / 2));
        if (reveal) {
            first = std::min(first, active);
            double length{};
            for (auto i = first; i <= active; ++i) {
                length += widths[i] * scale;
            }
            while (first < active && length > available) {
                length -= widths[first++] * scale;
            }
        }
        CHECK(strip.first == first);
        double x = 17;
        std::size_t visible{};
        for (auto i = first; i < widths.size(); ++i) {
            const auto width = std::min(widths[i] * scale, 17 + available - x);
            if (width < 96 * scale) {
                break;
            }
            REQUIRE(visible < strip.visible.size());
            const auto& box = strip.visible[visible++];
            CHECK(box.index == i);
            CHECK(box.bounds.x == doctest::Approx(x));
            CHECK(box.bounds.width == doctest::Approx(width));
            CHECK(box.close.right() == doctest::Approx(box.bounds.right()));
            CHECK(box.bounds.right() <= strip.previous.x);
            x += width;
        }
        CHECK(strip.visible.size() == visible);
        CHECK(strip.visible.size() <= static_cast<std::size_t>(physical / (110 * scale)) + 1);
    }
}
