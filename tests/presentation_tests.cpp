#include <astra/ui/file_tree_index.hpp>
#include <astra/ui/picker_index.hpp>
#include <astra/ui/workbench_layout.hpp>
#include <astra/ui/text_line_index.hpp>
#include <astra/ui/text_panel.hpp>
#include <astra/ui/decoration_indexes.hpp>
#include <astra/ui/inline_form.hpp>
#include <astra/ui/form_registry.hpp>
#include <astra/ui/view_store.hpp>
#include <astra/ui/view_rows_layout.hpp>
#include <astra/ui/document_metadata.hpp>
#include <astra/ui/workbench_style.hpp>
#include <astra/script/runtime.hpp>
#include <astra/io/file_store.hpp>
#include <doctest/doctest.h>

#include <random>
#include <limits>
#include <set>
#include <string>
#include <future>
#include <thread>
#include <cmath>

namespace {
astra::ui::TabStrip measure_tabs(astra::ui::Box bounds,
                                 float scale,
                                 const std::vector<float>& labels,
                                 std::size_t first,
                                 std::size_t active,
                                 bool reveal) {
    astra::ui::TabIndex index;
    for (const auto width : labels) {
        index.append(std::to_string(index.size()), "test");
        index.measured(index.size() - 1, width, scale);
    }
    return astra::ui::TabStrip::measure(bounds, scale, index.snapshot(), first, active, reveal);
}

astra::protocol::Json shipped_style_settings() {
    astra::script::Runtime runtime(
        [](std::string_view, const astra::protocol::Json&) -> astra::protocol::Json {
            throw std::logic_error("Defaults must not perform bootstrap I/O");
        });
    static_cast<void>(runtime.evaluate(astra::io::read_file("data/defaults.js").text));
    return runtime.settings();
}
} // namespace

TEST_CASE("presentation document metadata orders snapshots without advancing text revisions") {
    using astra::ui::DocumentMetadataStamp;
    const auto stamp = [](std::string revision, std::string sequence) {
        return DocumentMetadataStamp::read(
            {{"revision", revision}, {"metadataSequence", sequence}});
    };
    const auto current = stamp("7", "9007199254740993");
    CHECK(stamp("7", "9007199254740994").follows(current));
    CHECK(stamp("8", "9007199254740994").follows(current));
    CHECK(current.follows(current));
    CHECK_FALSE(stamp("7", "9007199254740992").follows(current));
    CHECK_FALSE(stamp("8", "9007199254740992").follows(current));
    CHECK_FALSE(stamp("6", "9007199254740994").follows(current));
    CHECK_FALSE(stamp("8", "9007199254740993").follows(current));
    CHECK(stamp("18446744073709551615", "18446744073709551615").follows(current));
    for (const auto field : {"revision", "metadataSequence"}) {
        for (const auto value : {astra::protocol::Json(nullptr),
                                 astra::protocol::Json(1),
                                 astra::protocol::Json(""),
                                 astra::protocol::Json("01"),
                                 astra::protocol::Json("-1"),
                                 astra::protocol::Json("+1"),
                                 astra::protocol::Json("1x"),
                                 astra::protocol::Json(" 1"),
                                 astra::protocol::Json("18446744073709551616")}) {
            astra::protocol::Json metadata{{"revision", "7"}, {"metadataSequence", "8"}};
            metadata[field] = value;
            CHECK_THROWS(static_cast<void>(DocumentMetadataStamp::read(metadata)));
        }
    }
    CHECK_THROWS(static_cast<void>(stamp("0", "0")));
    CHECK_THROWS(static_cast<void>(DocumentMetadataStamp::read({{"revision", "0"}})));
    CHECK_THROWS(static_cast<void>(DocumentMetadataStamp::read(astra::protocol::Json::array())));
    std::mt19937 random(0xA57A95);
    DocumentMetadataStamp accepted{7, 1};
    std::uint64_t highest = 1;
    for (int round = 0; round < 2000; ++round) {
        const std::uint64_t sequence = 1 + random() % 4000;
        const auto incoming = stamp("7", std::to_string(sequence));
        if (incoming.follows(accepted))
            accepted = incoming;
        highest = std::max(highest, sequence);
        CHECK(accepted.revision == 7);
        CHECK(accepted.sequence == highest);
    }
}

TEST_CASE("presentation row viewport shares clipped paint pointer and page boundaries") {
    using astra::ui::ViewRowsLayout;
    const auto layout = ViewRowsLayout::measure({10, 20, 200, 50.5F}, 1.5F, 24, 10000, 5000);
    CHECK(layout.first() == 5000);
    CHECK(layout.end() == 5003);
    CHECK(layout.page() == 2);
    CHECK(layout.row(5002).y == 68);
    CHECK(layout.row(5002).height == 2.5F);
    CHECK(layout.hit(11, 20) == 5000);
    CHECK(layout.hit(11, 44) == 5001);
    CHECK(layout.hit(11, 70.25F) == 5002);
    CHECK_FALSE(layout.hit(11, 70.5F));
    CHECK_FALSE(layout.hit(210, 20));
    CHECK_FALSE(layout.hit(9, 20));
    CHECK_THROWS(static_cast<void>(layout.row(4999)));
    CHECK_THROWS(static_cast<void>(layout.row(5003)));
    const auto tail = ViewRowsLayout::measure({10, 20, 200, 50.5F}, 1, 24, 4, 9999);
    CHECK(tail.first() == 3);
    CHECK(tail.end() == 4);
    CHECK(tail.hit(11, 43) == 3);
    CHECK_FALSE(tail.hit(11, 44));
    CHECK_FALSE(tail.hit(11, 70));
    const auto maximum = std::numeric_limits<std::size_t>::max();
    const auto huge = ViewRowsLayout::measure({0, 0, 100, 2.5F}, 1, 1, maximum, maximum - 2);
    CHECK(huge.first() == maximum - 2);
    CHECK(huge.end() == maximum);
    CHECK(huge.hit(1, 1.5F) == maximum - 1);
    CHECK_FALSE(huge.hit(1, 2.25F));
    for (const auto total : {std::size_t{}, std::size_t{100}}) {
        const auto zero_width = ViewRowsLayout::measure({0, 0, 0, 60}, 1, 24, total, 80);
        const auto zero_height = ViewRowsLayout::measure({0, 0, 100, 0}, 1, 24, total, 80);
        CHECK(zero_width.end() == zero_width.first());
        CHECK(zero_height.end() == zero_height.first());
        CHECK_FALSE(zero_width.hit(0, 1));
        CHECK_FALSE(zero_height.hit(1, 0));
    }
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();
    const auto largest = std::numeric_limits<float>::max();
    for (const auto box : {astra::ui::Box{nan, 0, 100, 50},
                           {0, infinity, 100, 50},
                           {0, 0, -1, 50},
                           {0, 0, 100, -1},
                           {largest, 0, largest, 50},
                           {0, largest, 100, 1}})
        CHECK_THROWS(static_cast<void>(ViewRowsLayout::measure(box, 1, 24, 10, 0)));
    for (const auto invalid : {nan, infinity, 0.0F, -1.0F}) {
        CHECK_THROWS(
            static_cast<void>(ViewRowsLayout::measure({0, 0, 100, 60}, invalid, 24, 10, 0)));
        CHECK_THROWS(
            static_cast<void>(ViewRowsLayout::measure({0, 0, 100, 60}, 1, invalid, 10, 0)));
    }
    CHECK_THROWS(static_cast<void>(ViewRowsLayout::measure({0, 0, 100, 60}, 1, 0.5F, 10, 0)));
    CHECK_FALSE(layout.hit(nan, 22));
    CHECK_FALSE(layout.hit(11, infinity));
}

TEST_CASE("presentation fractional row edges agree with physical pointer hit testing") {
    using astra::ui::ViewRowsLayout;
    std::mt19937 random(0xA57A94);
    for (int round = 0; round < 2000; ++round) {
        const auto scale = 0.5F + static_cast<float>(random() % 16) / 8;
        const auto height = 19.3F * scale;
        const astra::ui::Box bounds{static_cast<float>(random() % 123) / 7,
                                    static_cast<float>(random() % 600) / 17,
                                    217 * scale,
                                    height * static_cast<float>(random() % 30) / 2};
        const auto layout = ViewRowsLayout::measure(bounds, scale, height, 1000, random() % 1100);
        const auto x = bounds.x + bounds.width / 2;
        for (auto index = layout.first(); index < layout.end(); ++index) {
            const auto box = layout.row(index);
            CAPTURE(round);
            CAPTURE(index);
            CAPTURE(box.y);
            CAPTURE(box.height);
            REQUIRE(box.height > 0);
            const auto middle =
                std::min(box.y + box.height / 2, std::nextafter(box.bottom(), box.y));
            REQUIRE(box.contains(x, middle));
            CHECK(layout.hit(x, middle) == index);
            CHECK(layout.hit(x, box.y) == index);
            if (index + 1 < layout.end()) {
                CHECK(box.bottom() == layout.row(index + 1).y);
                CHECK(layout.hit(x, box.bottom()) == index + 1);
            } else {
                CHECK_FALSE(layout.hit(x, box.bottom()));
            }
        }
        CHECK_FALSE(layout.hit(x, bounds.bottom()));
        CHECK_FALSE(layout.hit(bounds.right(), bounds.y));
        CHECK(layout.page() >= 1);
        CHECK(layout.page() <= 256);
    }
}

TEST_CASE("presentation style derives every UI text role from JS defaults") {
    using astra::ui::WorkbenchStyle;
    auto settings = shipped_style_settings();
    for (const float size : {9.0F, 13.0F, 16.0F}) {
        settings["workbench"]["uiFontSize"] = size;
        settings["workbench"]["uiFontFamilies"] = {"Consolas", "Microsoft YaHei UI"};
        const auto style = WorkbenchStyle::compile(settings);
        CHECK(style.colors.background == 0xFF1F1F1FU);
        CHECK(style.colors.panel == 0xFF252526U);
        CHECK(style.colors.inactive_selection == 0xFF37373DU);
        CHECK(style.colors.tabs == 0xFF181818U);
        CHECK(style.syntax.at("error") == 0xFFF44747U);
        for (const float reference : {12.0F, 13.0F, 14.0F, 16.0F}) {
            const auto text = style.text(reference, 1.5F, style.colors.foreground);
            CHECK(text.font_size == doctest::Approx(reference * size / 13));
            CHECK(text.dpi_scale == 1.5F);
            CHECK(text.font_families == style.font_families);
            CHECK(text.direction == astra::ui::TextDirection::automatic);
        }
        const auto mono = style.text(13, 1, style.colors.muted, 1.4F, true, 250);
        CHECK(mono.font_families == style.monospace_families);
        CHECK(mono.wrap_width == 250);
        CHECK(mono.line_height == 1.4F);
    }
}

TEST_CASE("presentation style rejects partial invalid palettes and font geometry") {
    using astra::ui::WorkbenchStyle;
    const auto defaults = shipped_style_settings();
    for (const auto* invalid : {"# 12345", "#+12345", "#12345Z", "1234567", "#12345678"}) {
        auto candidate = defaults;
        candidate["theme"]["warning"] = invalid;
        CHECK_THROWS(static_cast<void>(WorkbenchStyle::compile(candidate)));
    }
    for (const auto& patch : std::vector<astra::protocol::Json>{
             {{"theme", {{"foreground", nullptr}}}},
             {{"theme", {{"panel", 7}}}},
             {{"workbench", {{"uiFontSize", 17}}}},
             {{"workbench", {{"uiFontSize", "13"}}}},
             {{"workbench", {{"uiFontFamilies", astra::protocol::Json::array()}}}},
             {{"workbench", {{"uiFontFamilies", {"Bad\nFamily"}}}}},
             {{"workbench", {{"uiMonospaceFontFamilies", {17}}}}}}) {
        auto candidate = defaults;
        candidate.merge_patch(patch);
        CHECK_THROWS(static_cast<void>(WorkbenchStyle::compile(candidate)));
    }
    auto candidate = defaults;
    candidate["workbench"]["sidebarWidth"] = 1;
    CHECK(WorkbenchStyle::compile(candidate).sidebar_width == 180);
    candidate["workbench"]["sidebarWidth"] = 9999;
    CHECK(WorkbenchStyle::compile(candidate).sidebar_width == 480);
    const auto style = WorkbenchStyle::compile(defaults);
    CHECK_THROWS(static_cast<void>(style.text(13, 0, 0)));
    CHECK_THROWS(static_cast<void>(style.text(0, 1, 0)));
    CHECK_THROWS(static_cast<void>(style.text(13, 1, 0, 0.5F)));
    CHECK_THROWS(static_cast<void>(WorkbenchStyle{}.text(13, 1, 0)));
    CHECK(defaults == shipped_style_settings());
}

namespace {
astra::protocol::Json form_spec() {
    using Json = astra::protocol::Json;
    return {{"id", "form"},
            {"title", "Find"},
            {"fields",
             Json::array({{{"id", "query"}, {"value", "中🙂e\xCC\x81"}, {"submit", "next"}}})},
            {"toggles", Json::array({{{"id", "regex"}, {"label", "Regex"}, {"value", false}}})},
            {"actions", Json::array({{{"id", "next"}, {"label", "Next"}}})}};
}

struct ReleaseGate {
    std::promise<void> release;
    std::shared_future<void> future = release.get_future().share();
    bool released{};

    ~ReleaseGate() {
        open();
    }

    void open() {
        if (!released) {
            released = true;
            release.set_value();
        }
    }
};

struct RetirementProbe {
    astra::protocol::Json row{{"id", "probe"}, {"label", "中🙂"}};
    std::promise<std::thread::id>& entered;
    std::shared_future<void> gate;

    RetirementProbe(std::promise<std::thread::id>& entry, std::shared_future<void> release)
        : entered(entry), gate(std::move(release)) {
    }

    ~RetirementProbe() {
        entered.set_value(std::this_thread::get_id());
        gate.wait();
    }
};
} // namespace

TEST_CASE("presentation retirement reclaims off owner with bounded reservations and bytes") {
    std::promise<std::thread::id> entered;
    auto worker = entered.get_future();
    astra::ui::ViewRetirement retirement(1, 100);
    ReleaseGate gate; // Opens before queue destruction even on a failed assertion.
    auto probe = std::make_shared<RetirementProbe>(entered, gate.future);
    std::shared_ptr<const astra::protocol::Json> value(probe, &probe->row);
    std::weak_ptr<const astra::protocol::Json> weak = value;
    auto reservation = retirement.reserve();
    reservation.charge(99);
    CHECK_THROWS(reservation.charge(2));
    CHECK(retirement.stats().bytes == 99);
    CHECK_FALSE(retirement.try_reserve());
    astra::ui::ViewRetirement::Payload payload;
    payload.rows = astra::ui::ViewSnapshot::singleton({std::move(value), 0, 0, false});
    probe.reset();
    retirement.retire(std::move(reservation), std::move(payload));
    REQUIRE(worker.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(worker.get() != std::this_thread::get_id());
    CHECK_FALSE(retirement.idle());
    CHECK(retirement.stats().queued == 1);
    CHECK(retirement.stats().bytes == 99);
    CHECK_FALSE(retirement.try_reserve());
    CHECK_THROWS(static_cast<void>(retirement.reserve()));
    gate.open();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!retirement.idle() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(retirement.idle());
    CHECK(weak.expired());
    CHECK(retirement.stats().reserved == 0);
    CHECK(retirement.stats().bytes == 0);
    CHECK(retirement.stats().completed == 1);
    auto reused = retirement.reserve();
    reused.charge(100);
    CHECK(retirement.stats().bytes == 100);
}

TEST_CASE("presentation retirement shutdown drains and external snapshots remain valid") {
    std::promise<std::thread::id> entered;
    auto worker = entered.get_future();
    ReleaseGate gate;
    gate.open();
    auto probe = std::make_shared<RetirementProbe>(entered, gate.future);
    std::shared_ptr<const astra::protocol::Json> value(probe, &probe->row);
    std::weak_ptr<const astra::protocol::Json> weak = value;
    auto retained = astra::ui::ViewSnapshot::singleton({std::move(value), 0, 0, false});
    probe.reset();
    {
        astra::ui::ViewRetirement retirement;
        astra::ui::ViewRetirement::Payload payload;
        payload.rows = retained;
        retirement.retire(retirement.reserve(), std::move(payload));
    }
    CHECK_FALSE(weak.expired());
    CHECK(retained.at(0).value->at("label") == "中🙂");
    CHECK(worker.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    retained = {};
    CHECK(weak.expired());
    CHECK(worker.get() == std::this_thread::get_id());

    std::promise<std::thread::id> drained;
    auto drained_on = drained.get_future();
    auto second = std::make_shared<RetirementProbe>(drained, gate.future);
    std::shared_ptr<const astra::protocol::Json> second_value(second, &second->row);
    std::weak_ptr<const astra::protocol::Json> second_weak = second_value;
    second.reset();
    {
        astra::ui::ViewRetirement retirement;
        astra::ui::ViewRetirement::Payload payload;
        payload.rows = astra::ui::ViewSnapshot::singleton({std::move(second_value), 0, 0, false});
        retirement.retire(retirement.reserve(), std::move(payload));
    }
    CHECK(second_weak.expired());
    CHECK(drained_on.get() != std::this_thread::get_id());
}

TEST_CASE("presentation view memory admission failures preserve builders and published data") {
    using Json = astra::protocol::Json;
    astra::ui::ViewStore store(2, 200);
    const Json spec{{"id", "budget"}, {"title", "Budget"}};
    const auto initial = store.begin(spec);
    REQUIRE(store.append("budget", initial, Json::array({{{"id", "old"}, {"label", "Old"}}})));
    auto committed = store.commit("budget", initial, "", "Empty");
    store.poll();
    REQUIRE(committed.get().get<bool>());
    const auto next = store.begin(spec);
    CHECK_THROWS(static_cast<void>(store.begin(spec))); // Preserve this reserved builder.
    const auto bytes = store.retirement_stats().bytes;
    CHECK_THROWS(static_cast<void>(store.append(
        "budget", next, Json::array({{{"id", "large"}, {"label", std::string(200, 'x')}}}))));
    CHECK(store.retirement_stats().bytes == bytes);
    CHECK_THROWS(static_cast<void>(store.append(
        "budget", next, Json::array({{{"id", "bad"}, {"label", "Bad"}, {"depth", 1}}}))));
    CHECK(store.retirement_stats().bytes == bytes);
    REQUIRE(store.append("budget", next, Json::array({{{"id", "new"}, {"label", "New"}}})));
    CHECK(store.at("budget").version == initial);
    CHECK(store.at("budget").index.at(0).value->at("id") == "old");
    CHECK(store.abort("budget", next));
    store.shutdown();
    CHECK_THROWS(static_cast<void>(store.begin(spec)));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!store.quiescent() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    REQUIRE(store.quiescent());
    CHECK(store.retirement_stats().bytes == 0);
    CHECK(store.retirement_stats().reserved == 0);
}

TEST_CASE("presentation view pages validate atomically across hierarchy boundaries") {
    using Json = astra::protocol::Json;
    astra::ui::ViewBuilder builder;
    builder.append(Json::array({{{"id", "root"}, {"label", "Root"}, {"group", true}}}));
    CHECK_THROWS(builder.append(Json::array({{{"id", "child"}, {"label", "Child"}, {"depth", 1}},
                                             {{"id", "root"}, {"label", "Duplicate"}}})));
    CHECK(builder.size() == 1);
    CHECK_THROWS(builder.append(Json::array({{{"id", "bad"}, {"label", "Bad"}, {"depth", 2}}})));
    CHECK_THROWS(builder.append(Json::array({{{"id", "bad"}, {"label", "Bad\nlabel"}}})));
    CHECK_THROWS(
        builder.append(Json::array({{{"id", "bad"}, {"label", "Bad"}, {"icon", "unknown"}}})));
    CHECK(builder.size() == 1);
    builder.append(Json::array({{{"id", "child"}, {"label", "中🙂"}, {"depth", 1}}}));
    CHECK(builder.size() == 2);
    auto source = std::move(builder).finish();
    CHECK(builder.size() == 0);
    CHECK_THROWS(builder.append(Json::array()));
    astra::ui::ViewIndex index(source);
    REQUIRE(index.filter_step());
    const auto snapshot = index.snapshot();
    static_cast<void>(index.toggle(0));
    CHECK(index.size() == 1);
    CHECK(snapshot.size() == 2);
    CHECK(snapshot.at(1).value->at("label") == "中🙂");
    static_cast<void>(index.toggle(0));
    CHECK(index.size() == 2);
    CHECK(index.snapshot().validate());
    CHECK_THROWS(static_cast<void>(index.toggle(1)));
    CHECK_THROWS(static_cast<void>(index.filter_step(0)));
}

TEST_CASE("presentation hierarchical view toggles match a flat oracle with retained snapshots") {
    using Json = astra::protocol::Json;
    std::vector<Json> rows;
    for (int group = 0; group < 12; ++group) {
        rows.push_back({{"id", "g" + std::to_string(group)}, {"label", "Group"}, {"group", true}});
        for (int subgroup = 0; subgroup < 4; ++subgroup) {
            rows.push_back({{"id", "s" + std::to_string(rows.size())},
                            {"label", "Subgroup"},
                            {"depth", 1},
                            {"group", true}});
            for (int leaf = 0; leaf < 8; ++leaf)
                rows.push_back(
                    {{"id", "l" + std::to_string(rows.size())}, {"label", "中🙂"}, {"depth", 2}});
        }
    }
    astra::ui::ViewBuilder builder;
    for (std::size_t first = 0; first < rows.size(); first += 73)
        builder.append(Json(std::vector<Json>(rows.begin() + first,
                                              rows.begin() + std::min(first + 73, rows.size()))));
    auto source = std::move(builder).finish();
    astra::ui::ViewIndex index(source);
    REQUIRE(index.filter_step());
    const auto original = index.snapshot();
    std::set<std::string> collapsed;
    std::mt19937 random(0xA57BA22U);
    for (int round = 0; round < 800; ++round) {
        const auto target = static_cast<std::size_t>(random()) % index.size();
        const auto& item = index.at(target);
        if (item.group) {
            const auto id = item.value->at("id").get<std::string>();
            if (!collapsed.erase(id))
                collapsed.insert(id);
            static_cast<void>(index.toggle(target));
        }
        std::vector<std::string> expected;
        std::optional<std::size_t> hidden_depth;
        for (const auto& row : rows) {
            const auto depth = row.value("depth", std::size_t{0});
            if (hidden_depth && depth > *hidden_depth)
                continue;
            hidden_depth.reset();
            const auto id = row.at("id").get<std::string>();
            expected.push_back(id);
            if (row.value("group", false) && collapsed.contains(id))
                hidden_depth = depth;
        }
        REQUIRE(index.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            CHECK(index.at(i).value->at("id") == expected[i]);
            CHECK(index.position(expected[i]) == i);
            auto parent = i;
            while (parent && index.at(parent).depth >= index.at(i).depth)
                --parent;
            if (index.at(i).depth)
                CHECK(index.parent(i) == parent);
            else
                CHECK_FALSE(index.parent(i));
        }
        const auto probe = static_cast<std::size_t>(random()) % rows.size();
        const auto probe_id = rows[probe].at("id").get<std::string>();
        auto ancestor = probe;
        auto found = std::ranges::find(expected, probe_id);
        CHECK(index.position(probe_id).has_value() == (found != expected.end()));
        while (found == expected.end()) {
            const auto depth = rows[ancestor].value("depth", std::size_t{0});
            while (ancestor && rows[ancestor].value("depth", std::size_t{0}) >= depth)
                --ancestor;
            found = std::ranges::find(expected, rows[ancestor].at("id").get<std::string>());
        }
        CHECK(index.position(probe_id, true) == static_cast<std::size_t>(found - expected.begin()));
        CHECK(index.snapshot().validate());
        CHECK(original.size() == rows.size());
    }
    astra::ui::ViewIndex rebuilding(source, collapsed);
    std::size_t previous = 0;
    while (!rebuilding.ready()) {
        static_cast<void>(rebuilding.filter_step(7));
        CHECK(rebuilding.size() - previous <= 7);
        previous = rebuilding.size();
    }
    CHECK(rebuilding.size() == index.size());
    for (std::size_t i = 0; i < index.size(); ++i)
        CHECK(rebuilding.at(i).value->at("id") == index.at(i).value->at("id"));
}

TEST_CASE(
    "presentation view publication retains old data and rejects stale or cancelled refreshes") {
    using Json = astra::protocol::Json;
    astra::ui::ViewStore store;
    const Json spec{{"id", "custom"}, {"title", "Custom 中🙂"}, {"icon", "symbol"}, {"order", 3}};
    const auto old = store.begin(spec);
    REQUIRE(store.append("custom", old, Json::array({{{"id", "old"}, {"label", "Old"}}})));
    auto published = store.commit("custom", old, "Subtitle", "Empty");
    CHECK(published.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    store.poll();
    CHECK(published.get().get<bool>());
    const auto snapshot = store.at("custom").index.snapshot();
    const auto cancelled = store.begin(spec);
    REQUIRE(store.append("custom", cancelled, Json::array({{{"id", "new"}, {"label", "New"}}})));
    auto completion = store.commit("custom", cancelled, "New", "Empty");
    const auto newer = store.begin(spec);
    CHECK_FALSE(completion.get().get<bool>());
    CHECK_FALSE(store.append("custom", cancelled, Json::array()));
    CHECK_FALSE(store.abort("custom", cancelled));
    CHECK(store.at("custom").version == old);
    CHECK(store.at("custom").index.at(0).value->at("id") == "old");
    CHECK_THROWS(static_cast<void>(store.append(
        "custom", newer, Json::array({{{"id", "bad"}, {"label", "Bad"}, {"depth", 1}}}))));
    CHECK(store.abort("custom", newer));
    CHECK(store.idle());
    CHECK(snapshot.at(0).value->at("id") == "old");
    const auto shutdown = store.begin(spec);
    auto closing = store.commit("custom", shutdown, "", "Empty");
    store.cancel_all();
    CHECK_FALSE(closing.get().get<bool>());
    CHECK(store.at("custom").index.size() == 1);
}

TEST_CASE("presentation refresh preserves folds changed during bounded publication") {
    using Json = astra::protocol::Json;
    astra::ui::ViewStore store;
    const Json spec{{"id", "custom"}, {"title", "Custom"}};
    Json rows = Json::array();
    for (int group = 0; group < 200; ++group) {
        rows.push_back({{"id", "g" + std::to_string(group)}, {"label", "Group"}, {"group", true}});
        rows.push_back({{"id", "l" + std::to_string(group)}, {"label", "Leaf"}, {"depth", 1}});
    }
    const auto fill = [&](const std::string& version) {
        for (std::size_t first = 0; first < rows.size(); first += 100)
            REQUIRE(store.append("custom",
                                 version,
                                 Json::array_t(rows.begin() + first,
                                               rows.begin() + std::min(first + 100, rows.size()))));
    };
    const auto first = store.begin(spec);
    fill(first);
    auto initial = store.commit("custom", first, "", "Empty");
    store.poll();
    REQUIRE(initial.get().get<bool>());
    store.toggle("custom", 0);
    const auto next = store.begin(spec);
    fill(next);
    auto replacing = store.commit("custom", next, "", "Empty");
    store.poll();
    CHECK(replacing.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    store.toggle("custom", 1);
    while (!store.idle())
        store.poll();
    REQUIRE(replacing.get().get<bool>());
    CHECK(store.at("custom").index.collapsed("g0"));
    CHECK(store.at("custom").index.collapsed("g1"));
    CHECK(store.at("custom").index.size() == 398);
}

TEST_CASE("presentation cursor navigates hierarchy and survives reordered refreshes") {
    using Json = astra::protocol::Json;
    astra::ui::ViewStore store;
    const Json spec{{"id", "navigation"}, {"title", "Navigation"}};
    const auto publish = [&](Json rows) {
        const auto version = store.begin(spec);
        REQUIRE(store.append("navigation", version, rows));
        auto result = store.commit("navigation", version, "", "Empty");
        while (!store.idle())
            store.poll();
        REQUIRE(result.get().get<bool>());
    };
    publish(Json::array({{{"id", "root"}, {"label", "Root"}, {"group", true}},
                         {{"id", "nested"}, {"label", "Nested"}, {"depth", 1}, {"group", true}},
                         {{"id", "leaf"}, {"label", "中🙂"}, {"depth", 2}},
                         {{"id", "sibling"}, {"label", "Sibling"}, {"depth", 1}},
                         {{"id", "tail"}, {"label", "Tail"}}}));
    CHECK(store.navigate("navigation", "retain", 2).at("row").at("id") == "root");
    CHECK(store.navigate("navigation", "right", 2).at("row").at("id") == "nested");
    CHECK(store.navigate("navigation", "right", 2).at("row").at("id") == "leaf");
    CHECK(store.state("navigation").at("scroll") == 1);
    CHECK(store.navigate("navigation", "activate", 2).at("activate") == true);
    CHECK(store.navigate("navigation", "left", 2).at("row").at("id") == "nested");
    CHECK(store.navigate("navigation", "left", 2).at("collapsed") == true);
    CHECK(store.navigate("navigation", "right", 2).at("collapsed") == false);
    REQUIRE(store.select("navigation", "leaf", 2));
    store.toggle("navigation", 0);
    CHECK(store.state("navigation").at("row").at("id") == "root");
    CHECK_FALSE(store.select("navigation", "leaf", 2));
    CHECK_FALSE(store.select("navigation", "missing", 2));
    CHECK(store.navigate("navigation", "next", 2).at("row").at("id") == "tail");
    CHECK(store.navigate("navigation", "previous", 2).at("row").at("id") == "root");
    CHECK(store.navigate("navigation", "activate", 2).at("activate") == false);
    CHECK(store.navigate("navigation", "last", 2).at("row").at("id") == "tail");
    CHECK(store.navigate("navigation", "pageUp", 2).at("row").at("id") == "leaf");
    CHECK(store.navigate("navigation", "pageDown", 2).at("row").at("id") == "tail");
    const auto before = store.state("navigation");
    CHECK_THROWS(static_cast<void>(store.navigate("navigation", "invalid", 2)));
    CHECK_THROWS(static_cast<void>(store.navigate("navigation", "first", 0)));
    CHECK(store.state("navigation") == before);
    publish(Json::array(
        {{{"id", "tail"}, {"label", "Tail moved"}}, {{"id", "new"}, {"label", "New"}}}));
    CHECK(store.state("navigation").at("index") == 0);
    CHECK(store.state("navigation").at("row").at("id") == "tail");
    publish(Json::array({{{"id", "new"}, {"label", "New"}}}));
    CHECK(store.state("navigation").at("row").at("id") == "new");
    publish(Json::array());
    CHECK(store.navigate("navigation", "next", 2).at("row").is_null());
    CHECK_FALSE(store.cursor("navigation"));
}

TEST_CASE("presentation reveal expands only ancestors and preserves retained snapshots") {
    using Json = astra::protocol::Json;
    Json rows = Json::array();
    for (int group = 0; group < 100; ++group) {
        const auto root = "g" + std::to_string(group);
        rows.push_back({{"id", root}, {"label", root}, {"group", true}});
        for (int nested = 0; nested < 4; ++nested) {
            const auto name = root + "/n" + std::to_string(nested);
            rows.push_back({{"id", name}, {"label", name}, {"group", true}, {"depth", 1}});
            rows.push_back({{"id", name + "/中🙂"}, {"label", "中🙂"}, {"depth", 2}});
        }
    }
    astra::ui::ViewBuilder builder;
    for (std::size_t first = 0; first < rows.size(); first += 100)
        builder.append(
            Json::array_t(rows.begin() + first, rows.begin() + std::min(first + 100, rows.size())));
    auto source = std::move(builder).finish();
    astra::ui::ViewIndex index(source);
    REQUIRE(index.filter_step());
    std::mt19937 random(0xA57A93);
    std::set<std::string> folded;
    for (int round = 0; round < 400; ++round) {
        const auto old = index.snapshot();
        const auto old_folds = folded;
        std::vector<std::string> old_ids;
        old.for_each_range(0, old.size(), [&](const auto& row) {
            old_ids.push_back(row.value->at("id").template get<std::string>());
        });
        if (round % 2 == 0) {
            std::vector<std::size_t> groups;
            for (std::size_t i = 0; i < index.size(); ++i)
                if (index.at(i).group)
                    groups.push_back(i);
            const auto position = groups.at(random() % groups.size());
            const auto id = index.at(position).value->at("id").get<std::string>();
            if (!folded.erase(id))
                folded.insert(id);
            static_cast<void>(index.toggle(position));
        } else {
            const auto group = "g" + std::to_string(random() % 100);
            const auto nested = group + "/n" + std::to_string(random() % 4);
            const auto target = nested + "/中🙂";
            auto prepared = index.revealing(target);
            REQUIRE(prepared);
            CHECK(index.collapsed() == old_folds);
            folded.erase(group);
            folded.erase(nested);
            index = std::move(*prepared);
            REQUIRE(index.position(target));
        }
        CHECK(index.collapsed() == folded);
        std::vector<std::string> expected;
        std::optional<std::size_t> hidden_depth;
        for (const auto& row : rows) {
            const auto depth = row.value("depth", std::size_t{});
            if (hidden_depth && depth > *hidden_depth)
                continue;
            hidden_depth.reset();
            const auto id = row.at("id").get<std::string>();
            expected.push_back(id);
            if (folded.contains(id))
                hidden_depth = depth;
        }
        REQUIRE(index.size() == expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
            CHECK(index.at(i).value->at("id") == expected.at(i));
        CHECK(old.validate());
        std::size_t retained_position{};
        old.for_each_range(0, old.size(), [&](const auto& row) {
            CHECK(row.value->at("id") == old_ids.at(retained_position++));
        });
        CHECK(index.snapshot().validate());
    }
    const auto before = index.snapshot();
    const auto before_folds = index.collapsed();
    CHECK_FALSE(index.revealing("absent"));
    CHECK(index.collapsed() == before_folds);
    CHECK(index.size() == before.size());
}

TEST_CASE("presentation reveal supports maximum depth and retains target and sibling folds") {
    using Json = astra::protocol::Json;
    Json rows = Json::array();
    std::set<std::string> folded{"leaf", "unrelated"};
    for (std::size_t depth = 0; depth < 64; ++depth) {
        const auto name = "g" + std::to_string(depth);
        rows.push_back({{"id", name}, {"label", name}, {"depth", depth}, {"group", true}});
        folded.insert(name);
    }
    rows.push_back({{"id", "leaf"}, {"label", "Leaf"}, {"depth", 64}, {"group", true}});
    rows.push_back({{"id", "unrelated"}, {"label", "Other"}, {"group", true}});
    rows.push_back({{"id", "unrelated/leaf"}, {"label", "Other leaf"}, {"depth", 1}});
    astra::ui::ViewBuilder builder;
    builder.append(rows);
    astra::ui::ViewIndex index(std::move(builder).finish(), folded);
    while (!index.ready())
        static_cast<void>(index.filter_step(1));
    REQUIRE(index.size() == 2);
    auto prepared = index.revealing("leaf");
    REQUIRE(prepared);
    REQUIRE(prepared->size() == 66);
    CHECK(prepared->position("leaf") == 64);
    CHECK(prepared->collapsed() == std::set<std::string>{"leaf", "unrelated"});
    CHECK(index.collapsed() == folded);
    CHECK(index.size() == 2);
    CHECK_FALSE(prepared->position("unrelated/leaf"));
    CHECK(prepared->snapshot().validate());
}

TEST_CASE("presentation reveal invalidates a refresh projection without losing its cursor") {
    using Json = astra::protocol::Json;
    astra::ui::ViewStore store;
    const Json spec{{"id", "reveal"}, {"title", "Reveal"}};
    const auto fill = [&](const std::string& version) {
        for (int group = 0; group < 200; ++group) {
            const auto name = "g" + std::to_string(group);
            REQUIRE(store.append(
                "reveal",
                version,
                Json::array({{{"id", name}, {"label", name}, {"group", true}},
                             {{"id", name + "/leaf"}, {"label", "Leaf"}, {"depth", 1}}})));
        }
    };
    const auto first = store.begin(spec);
    fill(first);
    auto initial = store.commit("reveal", first, "", "Empty");
    store.poll();
    REQUIRE(initial.get().get<bool>());
    store.toggle("reveal", 0);
    const auto next = store.begin(spec);
    fill(next);
    auto completion = store.commit("reveal", next, "", "Empty");
    store.poll();
    REQUIRE(completion.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    REQUIRE(store.reveal("reveal", "g0/leaf", 3));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!store.idle()) {
        REQUIRE(std::chrono::steady_clock::now() < until);
        store.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(completion.get().get<bool>());
    CHECK(store.at("reveal").version == next);
    CHECK_FALSE(store.at("reveal").index.collapsed("g0"));
    CHECK(store.state("reveal").at("row").at("id") == "g0/leaf");
    CHECK(store.at("reveal").index.size() == 400);
}

TEST_CASE("presentation reveal is atomic under retirement backpressure and refresh") {
    using Json = astra::protocol::Json;
    astra::ui::ViewStore store(2);
    const Json spec{{"id", "reveal"}, {"title", "Reveal"}};
    const auto fill = [&](const std::string& version) {
        for (int group = 0; group < 200; ++group) {
            const auto name = "g" + std::to_string(group);
            REQUIRE(store.append(
                "reveal",
                version,
                Json::array({{{"id", name}, {"label", name}, {"group", true}},
                             {{"id", name + "/leaf"}, {"label", "Leaf"}, {"depth", 1}}})));
        }
    };
    const auto first = store.begin(spec);
    fill(first);
    auto initial = store.commit("reveal", first, "", "Empty");
    store.poll();
    REQUIRE(initial.get().get<bool>());
    store.toggle("reveal", 0);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!store.quiescent()) {
        REQUIRE(std::chrono::steady_clock::now() < until);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto retained = store.at("reveal").index.snapshot();
    const auto refresh = store.begin(spec);
    fill(refresh);
    auto completion = store.commit("reveal", refresh, "", "Empty");
    store.poll();
    REQUIRE(completion.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    const auto state = store.state("reveal");
    const auto generation = store.at("reveal").fold_generation;
    CHECK_THROWS(static_cast<void>(store.reveal("reveal", "g0/leaf", 3)));
    CHECK_THROWS(static_cast<void>(store.reveal("reveal", "g0/leaf", 0)));
    CHECK_FALSE(store.reveal("reveal", "missing", 3));
    CHECK(store.state("reveal") == state);
    CHECK(store.at("reveal").fold_generation == generation);
    REQUIRE(store.abort("reveal", refresh));
    CHECK_FALSE(completion.get().get<bool>());
    while (!store.quiescent()) {
        REQUIRE(std::chrono::steady_clock::now() < until);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(store.reveal("reveal", "g0/leaf", 3));
    CHECK(store.state("reveal").at("row").at("id") == "g0/leaf");
    CHECK_FALSE(store.at("reveal").index.collapsed("g0"));
    CHECK(store.at("reveal").fold_generation == generation + 1);
    CHECK(retained.size() == 399);
    CHECK(retained.at(1).value->at("id") == "g1");
    CHECK(store.reveal("reveal", "g199/leaf", 3));
    CHECK(store.state("reveal").at("scroll") == 397);
}

TEST_CASE("presentation inline inputs preserve graphemes history and result generations") {
    using astra::editing::Command;
    astra::ui::InlineForm form(form_spec(), "epoch");
    const auto initial = form.state();
    CHECK(initial.at("version") == "epoch-1");
    REQUIRE(form.update(
        {{"id", "form"}, {"version", form.version()}, {"busy", true}, {"message", "Working"}}));
    CHECK_FALSE(form.edit(Command::move_end));
    CHECK(form.busy());
    REQUIRE(form.edit(Command::backspace));
    CHECK(form.state().at("values").at("query") == "中🙂");
    CHECK_FALSE(form.busy());
    CHECK(form.message().empty());
    CHECK_FALSE(form.update({{"id", "form"}, {"version", initial.at("version")}, {"busy", true}}));
    REQUIRE(form.history(false));
    CHECK(form.state().at("values").at("query") == initial.at("values").at("query"));
    REQUIRE(form.history(true));
    CHECK(form.state().at("values").at("query") == "中🙂");
    const auto before = form.state();
    form.focus(1);
    CHECK_FALSE(form.edit(Command::insert_text, {.text = "body"}));
    form.toggle(0);
    CHECK(form.state().at("values").at("regex") == true);
    CHECK(form.version() != before.at("version").get<std::string>());
    const auto changed = form.state();
    CHECK_THROWS(form.toggle(8));
    CHECK(form.state() == changed);
    form.focus(std::nullopt);
    form.cycle_focus(false);
    CHECK(form.focus() == 0);
    form.cycle_focus(true);
    CHECK(form.focus() == 2);
    form.cycle_focus(false);
    CHECK(form.focus() == 0);
    form.select(0, astra::ByteOffset{3}, false);
    CHECK(form.fields()[0].text.selections().front().head.value == 3);
    CHECK_FALSE(form.edit(Command::move_left));
    CHECK(form.fields()[0].text.selections().front().head.value == 0);
    CHECK_FALSE(
        form.update({{"id", "different"}, {"version", form.version()}, {"message", "late"}}));
    astra::ui::InlineForm reopened(form_spec(), "next-epoch");
    CHECK_FALSE(reopened.update(
        {{"id", "form"}, {"version", initial.at("version")}, {"message", "stale"}}));
}

TEST_CASE("presentation inline input rejects invalid text without changing content selection or "
          "history") {
    using astra::editing::Command;
    auto spec = form_spec();
    spec["fields"][0]["value"] = std::string(astra::ui::InlineForm::max_input_bytes, 'a');
    astra::ui::InlineForm form(spec);
    static_cast<void>(form.edit(Command::move_end));
    const auto before = form.state();
    const auto selections = form.fields()[0].text.selections();
    for (const auto& invalid :
         std::vector<std::string>{"x", "a\nb", std::string("a\0b", 3), "\xFF"}) {
        CHECK_THROWS((void)form.edit(Command::insert_text, {.text = invalid}));
        CHECK(form.state() == before);
        CHECK(form.fields()[0].text.selections() == selections);
        CHECK_FALSE(form.fields()[0].text.can_undo());
    }
    CHECK_THROWS((void)form.edit(Command::newline));
    CHECK(form.state() == before);
    CHECK_THROWS((void)form.update({{"id", "form"},
                                    {"version", form.version()},
                                    {"message", std::string(513, 'x')},
                                    {"busy", true}}));
    CHECK_FALSE(form.busy());
    form.focus(0, true);
    REQUIRE(form.edit(Command::insert_text, {.text = "🙂"}));
    CHECK(form.state().at("values").at("query") == "🙂");
    spec["fields"][0]["submit"] = "missing";
    CHECK_THROWS(astra::ui::InlineForm{spec});
    spec = form_spec();
    spec["toggles"][0]["id"] = "query";
    CHECK_THROWS(astra::ui::InlineForm{spec});
    spec = form_spec();
    spec["focus"] = "missing";
    CHECK_THROWS(astra::ui::InlineForm{spec});
}

TEST_CASE("presentation inline geometry keeps all controls inside the editor across DPI") {
    for (const auto scale : {1.0F, 1.25F, 1.5F, 2.0F})
        for (const auto width : {220.0F, 490.0F, 1200.0F})
            for (const auto height : {80.0F, 800.0F}) {
                const astra::ui::Box editor{300 * scale, 90 * scale, width * scale, height * scale};
                const auto layout = astra::ui::InlineFormLayout::measure(editor, scale, 2, 3, 3);
                const auto bounded = [&](astra::ui::Box box, astra::ui::Box outer) {
                    CHECK(box.x >= outer.x);
                    CHECK(box.y >= outer.y);
                    CHECK(box.width >= 0);
                    CHECK(box.height >= 0);
                    CHECK(box.right() <= doctest::Approx(outer.right()).epsilon(0.00001));
                    CHECK(box.bottom() <= doctest::Approx(outer.bottom()).epsilon(0.00001));
                };
                bounded(layout.panel, editor);
                bounded(layout.title, layout.panel);
                bounded(layout.close, layout.panel);
                bounded(layout.message, layout.panel);
                for (const auto& boxes : {layout.fields, layout.toggles, layout.actions})
                    for (const auto box : boxes)
                        bounded(box, layout.panel);
                CHECK(layout.fields[0].bottom() <= layout.fields[1].y);
                CHECK(layout.fields[1].bottom() <= layout.toggles[0].y);
                CHECK(layout.toggles[0].bottom() <= layout.actions[0].y);
            }
}

TEST_CASE("presentation form value publication validates all fields before advancing generation") {
    using Json = astra::protocol::Json;
    astra::ui::InlineForm form(form_spec(), "epoch");
    const auto original = form.state();
    const auto selection = form.fields()[0].text.selections();
    for (const auto& values : std::vector<Json>{
             {{"query", "new"}, {"regex", "invalid"}},
             {{"query", "new"}, {"unknown", true}},
             {{"query", "a\nb"}, {"regex", true}},
             {{"query", std::string("a\0b", 3)}},
             {{"query", "\xFF"}},
             {{"query", std::string(astra::ui::InlineForm::max_input_bytes + 1, 'x')}}}) {
        CHECK_THROWS((void)form.set_values(
            {{"id", "form"}, {"version", form.version()}, {"values", values}}));
        CHECK(form.state() == original);
        CHECK(form.fields()[0].text.selections() == selection);
        CHECK_FALSE(form.fields()[0].text.can_undo());
    }
    CHECK_FALSE(form.set_values({{"id", "form"}, {"version", "stale"}, {"values", {}}}));
    CHECK_FALSE(form.set_values({{"id", "other"}, {"version", form.version()}, {"values", {}}}));
    CHECK_FALSE(form.set_values(
        {{"id", "form"}, {"version", form.version()}, {"values", original.at("values")}}));
    CHECK(form.state() == original);
    REQUIRE(form.set_values({{"id", "form"},
                             {"version", form.version()},
                             {"values", {{"query", "🙂中"}, {"regex", true}}}}));
    CHECK(form.version() == "epoch-2");
    CHECK(form.state().at("values").at("query") == "🙂中");
    CHECK(form.state().at("values").at("regex") == true);
    CHECK(form.fields()[0].text.selections().front().head.value == 7);
    REQUIRE(form.history(false));
    CHECK(form.state().at("values").at("query") == original.at("values").at("query"));
    CHECK(form.fields()[0].text.selections() == selection);
    REQUIRE(form.history(true));
    CHECK(form.state().at("values").at("query") == "🙂中");
}

TEST_CASE("presentation mounted forms retain independent input history and exclusive focus") {
    using astra::ui::InlineForm;
    astra::ui::FormRegistry forms;
    auto editor = form_spec();
    auto sidebar = form_spec();
    sidebar["id"] = "workspace";
    sidebar["view"] = "search";
    CHECK_FALSE(forms.open(std::make_unique<InlineForm>(editor)));
    REQUIRE(forms.focused());
    CHECK(forms.focused()->id() == "form");
    REQUIRE(forms.find("form")->edit(astra::editing::Command::insert_text, {.text = "editor"}));
    CHECK_FALSE(forms.open(std::make_unique<InlineForm>(sidebar)));
    CHECK(forms.focused()->id() == "workspace");
    CHECK_FALSE(forms.find("form")->focus());
    REQUIRE(
        forms.find("workspace")->edit(astra::editing::Command::insert_text, {.text = "sidebar"}));
    forms.focus("form", 0);
    CHECK_FALSE(forms.find("workspace")->focus());
    CHECK(forms.find("workspace")->state().at("values").at("query") == "sidebar");
    REQUIRE(forms.find("form")->history(false));
    CHECK(forms.find("form")->state().at("values").at("query") ==
          editor.at("fields")[0].at("value"));
    CHECK_THROWS(forms.focus("workspace", 20));
    CHECK(forms.focused()->id() == "form");
    auto duplicate = sidebar;
    duplicate["id"] = "form";
    CHECK_THROWS((void)forms.open(std::make_unique<InlineForm>(duplicate)));
    CHECK(forms.focused()->id() == "form");
    CHECK(forms.visible("search", true)[0] == forms.find("form"));
    CHECK(forms.visible("search", true)[1] == forms.find("workspace"));
    CHECK_FALSE(forms.visible("search", false)[1]);
    CHECK_FALSE(forms.visible("", true)[1]);
    auto replacement = sidebar;
    replacement["id"] = "replacement";
    const auto retired = forms.open(std::make_unique<InlineForm>(replacement));
    REQUIRE(retired);
    CHECK(retired->id() == "workspace");
    CHECK(retired->state().at("values").at("query") == "sidebar");
    CHECK_FALSE(forms.find("workspace"));
    CHECK(forms.focused()->id() == "replacement");
    REQUIRE(forms.close("form"));
    CHECK(forms.find("replacement"));
    forms.blur();
    CHECK_FALSE(forms.focused());
    CHECK(forms.mounted("search")->state().at("view") == "search");
}

TEST_CASE("presentation sidebar forms reserve bounded controls without overlap at every DPI") {
    for (const auto scale : {1.0F, 1.25F, 1.5F, 2.0F})
        for (const auto width : {0.0F, 80.0F, 180.0F, 300.0F})
            for (const auto height : {0.0F, 60.0F, 900.0F}) {
                const astra::ui::Box available{
                    48 * scale, 80 * scale, width * scale, height * scale};
                const auto layout =
                    astra::ui::InlineFormLayout::measure_sidebar(available, scale, 2, 3, 3);
                const auto bounded = [&](astra::ui::Box box) {
                    CHECK(box.x >= available.x);
                    CHECK(box.y >= available.y);
                    CHECK(box.width >= 0);
                    CHECK(box.height >= 0);
                    CHECK(box.right() <= doctest::Approx(available.right()).epsilon(0.00001));
                    CHECK(box.bottom() <= doctest::Approx(layout.panel.bottom()).epsilon(0.00001));
                };
                bounded(layout.panel);
                bounded(layout.title);
                bounded(layout.close);
                bounded(layout.message);
                for (const auto& boxes : {layout.fields, layout.toggles, layout.actions})
                    for (const auto box : boxes)
                        bounded(box);
                CHECK(layout.fields[0].bottom() <= doctest::Approx(layout.fields[1].y));
                CHECK(layout.fields[1].bottom() <= doctest::Approx(layout.toggles[0].y));
                CHECK(layout.toggles[0].bottom() <= doctest::Approx(layout.message.y));
                CHECK(layout.message.bottom() <= doctest::Approx(layout.actions[0].y));
            }
}

TEST_CASE("presentation text panels index mixed line endings and preserve snapshots") {
    astra::ui::TextLineIndex lines;
    lines.reset("中\r\n🙂\re\xCC\x81\n");
    REQUIRE(lines.size() == 4);
    CHECK(lines.bytes() == 14);
    CHECK(lines.at(0) == "中");
    CHECK(lines.at(1) == "🙂");
    CHECK(lines.at(2) == "e\xCC\x81");
    CHECK(lines.at(3).empty());
    CHECK(lines.offset(1) == 5);
    CHECK(lines.offset(2) == 10);
    CHECK(lines.offset(3) == 14);
    const auto old = lines;
    std::string many;
    for (int row = 0; row < 20000; ++row)
        many += std::to_string(row) + "\n";
    lines.reset(many);
    std::vector<std::string> viewport;
    lines.for_each_range(18000, 18004, [&](std::size_t row, const std::string& text) {
        CHECK(row == 18000 + viewport.size());
        viewport.push_back(text);
    });
    CHECK(viewport == std::vector<std::string>{"18000", "18001", "18002", "18003"});
    CHECK(old.at(0) == "中");
    CHECK(old.size() == 4);
    CHECK_THROWS_AS(lines.reset(std::string(8 * 1024 * 1024 + 1, 'x')), std::length_error);
    CHECK(lines.size() == 20001);
    lines.reset("");
    CHECK(lines.size() == 1);
    CHECK(lines.bytes() == 0);
    CHECK(lines.at(0).empty());
}

TEST_CASE("presentation range queries prune overlaps and retain shared snapshots") {
    using Index = astra::ui::RangeIndex<std::size_t, astra::ui::Utf8ByteUnit>;
    std::vector<Index::Entry> entries;
    for (std::size_t i = 0; i < 99999; ++i)
        entries.push_back({i * 10, i * 10 + 2, i});
    entries.push_back({0, 1000000, 100000});
    Index index;
    index.reset(entries);
    const auto retained = index;
    std::vector<std::size_t> found;
    const auto statistics = index.intersect(999000, 999003, [&](const auto& entry) {
        found.push_back(entry.value);
    });
    CHECK(found == std::vector<std::size_t>{100000, 99900});
    CHECK(statistics.matches == 2);
    CHECK(statistics.nodes < 150);
    CHECK(index
              .intersect(500,
                         500,
                         [](const auto&) {
                         })
              .nodes == 0);
    CHECK(index.validate());
    entries[50000].value = 200000;
    index.reset(entries);
    CHECK(index.shared_prefix_items(retained) == 50001);
    CHECK(index.shared_suffix_items(retained) == 49998);
    found.clear();
    retained.intersect(500000, 500001, [&](const auto& entry) {
        found.push_back(entry.value);
    });
    CHECK(found == std::vector<std::size_t>{100000, 50000});
    const auto unchanged = index;
    CHECK_THROWS_AS(index.reset({{2, 2, 0}}), std::invalid_argument);
    CHECK(index.shared_prefix_items(unchanged) == 100000);
    CHECK_THROWS_AS(index.intersect(3,
                                    2,
                                    [](const auto&) {
                                    }),
                    std::invalid_argument);
    index.clear();
    CHECK(index.size() == 0);
    CHECK(retained.size() == 100000);
}

TEST_CASE("presentation overlapping ranges match a flat reference after random replacements") {
    using Index = astra::ui::RangeIndex<std::size_t, astra::ui::DocumentLineUnit>;
    std::mt19937 random(0xA57A1D);
    Index index;
    for (unsigned round = 0; round < 150; ++round) {
        const auto retained = index;
        std::vector<Index::Entry> entries;
        for (std::size_t item = 0; item < 500; ++item) {
            const auto first = static_cast<std::size_t>(random() % 5000);
            entries.push_back({first, first + 1 + random() % 200, item});
        }
        index.reset(entries);
        std::stable_sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            return left.begin < right.begin;
        });
        CHECK(index.validate());
        CHECK(retained.validate());
        for (unsigned query = 0; query < 20; ++query) {
            const auto begin = static_cast<std::size_t>(random() % 5200);
            const auto end = begin + random() % 20;
            std::vector<Index::Entry> expected, actual;
            if (begin != end)
                for (const auto& entry : entries)
                    if (entry.begin < end && entry.end > begin)
                        expected.push_back(entry);
            index.intersect(begin, end, [&](const auto& entry) {
                actual.push_back(entry);
            });
            CHECK(actual == expected);
        }
        const auto old = index;
        index.reset(entries);
        CHECK(index.shared_prefix_items(old) == entries.size());
    }
}

TEST_CASE("presentation decoration adapters distinguish bytes lines and UTF16 columns") {
    using Json = astra::protocol::Json;
    astra::ui::DecorationIndexes indexes;
    indexes.set_highlights(Json::array({{{"begin", 3}, {"end", 7}, {"kind", "string"}}}));
    indexes.set_diagnostics(Json::array(
        {{{"range",
           {{"start", {{"line", 2}, {"character", 4}}}, {"end", {{"line", 4}, {"character", 6}}}}},
          {"severity", 2}}}));
    indexes.set_gutter(Json::array({{{"line", 4}, {"count", 0}, {"kind", "deleted"}},
                                    {{"line", 8}, {"count", 3}, {"kind", "added"}}}));
    std::size_t matches{};
    indexes.highlights.intersect(4, 5, [&](const auto& entry) {
        ++matches;
        CHECK(entry.begin == 3);
        CHECK(entry.end == 7);
        CHECK(entry.value == "string");
    });
    CHECK(matches == 1);
    matches = 0;
    indexes.diagnostics.intersect(3, 4, [&](const auto& entry) {
        ++matches;
        CHECK(entry.begin == 2);
        CHECK(entry.end == 5);
        CHECK(entry.value.start_utf16 == 4);
        CHECK(entry.value.end_utf16 == 6);
        CHECK(entry.value.severity == 2);
    });
    CHECK(matches == 1);
    CHECK(indexes.diagnostics
              .intersect(5,
                         6,
                         [](const auto&) {
                         })
              .matches == 0);
    CHECK(indexes.gutter
              .intersect(4,
                         5,
                         [](const auto&) {
                         })
              .matches == 1);
    CHECK(indexes.gutter
              .intersect(11,
                         12,
                         [](const auto&) {
                         })
              .matches == 0);
    const auto retained = indexes;
    CHECK_THROWS_AS(
        indexes.set_highlights(Json::array({{{"begin", -1}, {"end", 3}, {"kind", "type"}}})),
        std::invalid_argument);
    CHECK_THROWS_AS(
        indexes.set_gutter(Json::array({{{"line", std::numeric_limits<std::size_t>::max()},
                                         {"count", 1},
                                         {"kind", "added"}}})),
        std::length_error);
    CHECK(indexes.highlights.shared_prefix_items(retained.highlights) == 1);
    CHECK(indexes.gutter.shared_prefix_items(retained.gutter) == 2);
    indexes.set_diagnostics(Json::array());
    CHECK(indexes.diagnostics.size() == 0);
    CHECK(retained.diagnostics.size() == 1);
}

TEST_CASE("presentation confirmation layout shares bounded review and button regions") {
    for (const auto scale : {1.0F, 1.25F, 1.5F, 2.0F})
        for (const auto width : {260.0F, 640.0F, 1440.0F})
            for (const std::size_t rows : {0, 1, 5, 40}) {
                const auto layout =
                    astra::ui::ConfirmationLayout::measure(width * scale, 600 * scale, scale, rows);
                CHECK(layout.panel.x >= 0);
                CHECK(layout.panel.right() <= width * scale);
                CHECK(layout.panel.bottom() <= 600 * scale);
                CHECK(layout.accept.right() <= layout.cancel.x);
                CHECK(layout.items.bottom() <= layout.accept.y);
                CHECK(layout.accept.contains(layout.accept.x + layout.accept.width / 2,
                                             layout.accept.y + layout.accept.height / 2));
                CHECK((layout.items.height > 0) == (rows > 0));
                if (rows == 1)
                    CHECK(layout.items.height <= 28 * scale + 1);
                if (rows == 40)
                    CHECK(layout.items.height < 40 * 28 * scale);
            }
}

TEST_CASE("presentation choice buttons remain distinct and inside narrow and scaled dialogs") {
    for (const auto scale : {1.0F, 1.25F, 1.5F, 2.0F})
        for (const auto width : {40.0F, 260.0F, 640.0F, 1440.0F})
            for (const auto height : {40.0F, 200.0F, 600.0F})
                for (std::size_t choices = 2; choices <= 4; ++choices) {
                    const auto layout = astra::ui::ChoiceLayout::measure(
                        width * scale, height * scale, scale, choices);
                    REQUIRE(layout.buttons.size() == choices);
                    CHECK(layout.panel.x >= 0);
                    CHECK(layout.panel.y >= 0);
                    CHECK(layout.panel.right() <= width * scale);
                    CHECK(layout.panel.bottom() <= height * scale);
                    CHECK(layout.title.bottom() <= layout.detail.y);
                    CHECK(layout.detail.bottom() <= layout.buttons.front().y);
                    for (std::size_t i = 0; i < choices; ++i) {
                        const auto& box = layout.buttons[i];
                        CHECK(box.width > 0);
                        CHECK(box.height > 0);
                        CHECK(box.x >= layout.panel.x);
                        CHECK(box.y >= layout.panel.y);
                        CHECK(box.right() <= layout.panel.right());
                        CHECK(box.bottom() <= layout.panel.bottom());
                        const auto cx = box.x + box.width / 2, cy = box.y + box.height / 2;
                        CHECK(box.contains(cx, cy));
                        for (std::size_t j = 0; j < choices; ++j)
                            if (i != j)
                                CHECK_FALSE(layout.buttons[j].contains(cx, cy));
                    }
                }
    CHECK_THROWS_AS((void)astra::ui::ChoiceLayout::measure(100, 100, 1, 1), std::invalid_argument);
    CHECK_THROWS_AS((void)astra::ui::ChoiceLayout::measure(100, 100, 1, 5), std::invalid_argument);
}

namespace {
using Json = astra::protocol::Json;

Json entry(std::string path, bool directory) {
    return {{"path", path}, {"directory", directory}};
}

std::vector<std::string> flattened(const Json& source, const std::set<std::string>& collapsed) {
    std::vector<std::string> result;
    for (const auto& item : source) {
        const auto path = item.at("path").get<std::string>();
        bool hidden{};
        for (const auto& parent : collapsed) {
            hidden |= path.size() > parent.size() && path.starts_with(parent) &&
                      path[parent.size()] == '/';
        }
        if (!hidden) {
            result.push_back(path);
        }
    }
    return result;
}

std::vector<std::string> paths(const astra::ui::FileTreeIndex::Snapshot& snapshot) {
    std::vector<std::string> result;
    snapshot.for_each([&](const auto& row) {
        result.push_back(row.value->at("path").template get<std::string>());
    });
    return result;
}
} // namespace

TEST_CASE("presentation file tree collapse preserves nested state and old snapshots") {
    astra::ui::FileTreeIndex index;
    const auto source = Json::array({entry("a", true),
                                     entry("a/b", true),
                                     entry("a/b/x.cpp", false),
                                     entry("a/y.js", false),
                                     entry("z.md", false)});
    index.reset(source);
    const auto snapshot = index.snapshot();
    index.toggle(1);
    CHECK(paths(index.snapshot()) == std::vector<std::string>{"a", "a/b", "a/y.js", "z.md"});
    index.toggle(0);
    CHECK(paths(index.snapshot()) == std::vector<std::string>{"a", "z.md"});
    index.toggle(0);
    CHECK(paths(index.snapshot()) == std::vector<std::string>{"a", "a/b", "a/y.js", "z.md"});
    index.toggle(1);
    CHECK(paths(index.snapshot()) == flattened(source, {}));
    CHECK(paths(snapshot) == flattened(source, {}));
    CHECK(index.snapshot().validate());
    CHECK_THROWS_AS(index.toggle(4), std::invalid_argument);
    index.toggle(0);
    index.reset(source);
    CHECK(index.size() == 2);
    index.reset(Json::array({entry("z.md", false)}));
    CHECK(index.size() == 1);
    CHECK_FALSE(index.collapsed("a"));
    CHECK_THROWS_AS(index.reset(Json::array({entry("z", false), entry("z", false)})),
                    std::invalid_argument);
    CHECK(index.at(0)["path"] == "z.md");
}

TEST_CASE("presentation picker filtering preserves source identity and old snapshots") {
    astra::ui::PickerIndex index;
    std::vector<std::string> labels;
    for (std::size_t i = 0; i < 2000; ++i)
        labels.push_back("File " + std::to_string(i) + " 中🙂");
    index.reset(labels);
    const auto retained = index.snapshot();
    std::mt19937 random(0xA57A91);
    for (int iteration = 0; iteration < 150; ++iteration) {
        const auto query =
            iteration % 3 == 0 ? std::string("中🙂") : "fILE " + std::to_string(random() % 2000);
        index.filter(query);
        std::vector<std::size_t> expected;
        auto normalized = query;
        for (auto& byte : normalized)
            if (byte >= 'A' && byte <= 'Z')
                byte = static_cast<char>(byte - 'A' + 'a');
        for (std::size_t i = 0; i < labels.size(); ++i) {
            auto folded = labels[i];
            folded[0] = 'f';
            if (folded.find(normalized) != folded.npos)
                expected.push_back(i);
        }
        REQUIRE(index.size() == expected.size());
        index.for_each_range(0, index.size(), [&](std::size_t row, const auto& item) {
            CHECK(item.source == expected[row]);
            CHECK(item.label == labels[item.source]);
        });
        CHECK(index.snapshot().validate());
        CHECK(retained.size() == labels.size());
        CHECK(retained.at(1999).label == labels.back());
    }
    index.filter("");
    CHECK(index.size() == 2000);
    CHECK_THROWS_AS(index.reset({"invalid\nlabel"}), std::length_error);
    CHECK(index.size() == 2000);
    CHECK_THROWS_AS(index.reset(std::vector<std::string>(4097, "row")), std::length_error);
    CHECK_THROWS_AS(index.reset({std::string(4097, 'x')}), std::length_error);
    CHECK_THROWS_AS(index.reset(std::vector<std::string>(17, std::string(4096, 'x'))),
                    std::length_error);
}

TEST_CASE("presentation dynamic picker rejects stale publications and preserves snapshots") {
    astra::ui::PickerIndex index;
    index.reset({}, true);
    CHECK_FALSE(index.ready());
    const auto initial = index.generation();
    REQUIRE(index.publish(initial, {"旧🙂", "Other"}, "2 matches", false, {"old.cpp", "other"}));
    const auto retained = index.snapshot();
    index.filter("中🙂");
    CHECK_FALSE(index.ready());
    CHECK(index.size() == 2);
    const auto current = index.generation();
    CHECK(current != initial);
    index.filter("中🙂");
    CHECK(index.generation() == current);
    CHECK_FALSE(index.publish(initial, {"obsolete"}, "stale"));
    CHECK(index.at(0).label == "旧🙂");
    REQUIRE(index.publish(current, {"new"}, "1 match", false, {"src/new.cpp"}));
    CHECK(index.ready());
    CHECK(index.at(0).source == 0);
    CHECK(index.at(0).icon_path == "src/new.cpp");
    CHECK(retained.at(0).label == "旧🙂");
    CHECK(retained.at(0).icon_path == "old.cpp");
    CHECK(retained.summary().rows == 2);
    CHECK(index.snapshot().validate());
    CHECK_THROWS(index.publish(current, {"invalid\nrow"}, "invalid"));
    CHECK_THROWS(index.publish(current, {"new"}, "bad\nmessage"));
    CHECK_THROWS(index.publish(current, {"new"}, "valid", false, {"a", "b"}));
    CHECK_THROWS(index.publish(current, {std::string(1, '\xFF')}, "invalid UTF-8"));
    CHECK_THROWS(index.publish(current, {"new"}, "valid", false, {std::string(32769, 'x')}));
    CHECK_THROWS(index.filter(std::string(1, '\xFF')));
    CHECK(index.ready());
    CHECK(index.query() == "中🙂");
    CHECK(index.message() == "1 match");
    CHECK(index.at(0).icon_path == "src/new.cpp");
    index.filter("none");
    REQUIRE(index.publish(index.generation(), {}, "Provider failed", true));
    CHECK(index.ready());
    CHECK(index.error());
    CHECK(index.size() == 0);
    index.filter("retry");
    CHECK_FALSE(index.error());
    CHECK_FALSE(index.ready());
    REQUIRE(index.publish(index.generation(), {"retry"}, "Recovered"));
    index.reset({"static"});
    CHECK(index.ready());
    CHECK_FALSE(index.dynamic());
    CHECK_FALSE(index.publish(index.generation(), {"dynamic"}, "ignored"));
    CHECK(index.at(0).label == "static");
}

TEST_CASE("presentation modal layouts share bounded rows and pointer geometry") {
    for (const auto scale : {1.0F, 1.25F, 1.5F, 2.0F})
        for (const auto width : {160.0F, 260.0F, 584.0F, 640.0F, 680.0F, 1440.0F}) {
            const auto picker = astra::ui::PickerLayout::measure(width * scale, 600 * scale, scale);
            CHECK(picker.panel.x >= 0);
            CHECK(picker.panel.right() <= width * scale);
            CHECK(picker.input.right() <= picker.panel.right());
            CHECK(picker.rows.bottom() <= picker.panel.bottom());
            CHECK(picker.capacity * picker.row_height <= picker.rows.height);
            CHECK(picker.rows.contains(picker.rows.x + 1, picker.rows.y + 1));
            const auto text =
                astra::ui::TextPanelLayout::measure(width * scale, 600 * scale, scale);
            CHECK(text.panel.x >= 0);
            CHECK(text.panel.right() <= width * scale);
            CHECK(text.content.bottom() <= text.panel.bottom());
            for (std::size_t count = 1; count <= 8; ++count) {
                const auto panel = astra::ui::TextPanelLayout::measure(
                    width * scale, 600 * scale, scale, 80, count);
                REQUIRE(panel.actions.size() == count);
                for (const auto& button : panel.actions) {
                    CHECK(button.x >= panel.panel.x);
                    CHECK(button.right() <= panel.panel.right());
                    CHECK(button.y >= panel.content.bottom());
                    CHECK(button.bottom() <= panel.panel.bottom());
                    CHECK(
                        button.contains(button.x + button.width / 2, button.y + button.height / 2));
                }
            }
        }
    CHECK_THROWS_AS((void)astra::ui::PickerLayout::measure(0, 100, 1), std::invalid_argument);
    CHECK_THROWS_AS((void)astra::ui::TextPanelLayout::measure(100, 100, 0), std::invalid_argument);
    CHECK_THROWS_AS((void)astra::ui::TextPanelLayout::measure(100, 100, 1, 10, 9),
                    std::invalid_argument);
    CHECK(astra::ui::PickerLayout::measure(1440, 900, 1, 1).capacity == 1);
    CHECK(astra::ui::PickerLayout::measure(1440, 900, 1, 1).panel.height < 150);
    CHECK(astra::ui::TextPanelLayout::measure(1440, 900, 1, 6).panel.height < 200);
}

TEST_CASE("presentation file tree random collapse matches a flat reference") {
    constexpr std::uint32_t seed = 0xA57A2026;
    INFO(seed);
    std::mt19937 random(seed);
    Json source = Json::array();
    for (int folder = 0; folder < 80; ++folder) {
        const auto path = "folder" + std::to_string(folder);
        source.push_back(entry(path, true));
        for (int sub = 0; sub < 8; ++sub) {
            const auto child = path + "/sub" + std::to_string(sub);
            source.push_back(entry(child, true));
            for (int file = 0; file < 4; ++file) {
                source.push_back(entry(child + "/file" + std::to_string(file) + ".cpp", false));
            }
        }
    }
    astra::ui::FileTreeIndex index;
    index.reset(source);
    std::set<std::string> collapsed;
    for (int iteration = 0; iteration < 300; ++iteration) {
        const auto expected = flattened(source, collapsed);
        const auto retained = index.snapshot();
        const auto position = random() % index.size();
        if (index.at(position)["directory"].get<bool>()) {
            const auto path = index.at(position)["path"].get<std::string>();
            if (!collapsed.erase(path)) {
                collapsed.insert(path);
            }
            index.toggle(position);
        }
        CHECK(paths(index.snapshot()) == flattened(source, collapsed));
        CHECK(paths(retained) == expected);
        CHECK(index.snapshot().validate());
        std::size_t visited{};
        const auto count = std::min(index.size(), std::size_t{20});
        index.for_each_range(
            index.size() - count, index.size(), [&](std::size_t row, const Json& value) {
                CHECK(value == index.at(row));
                ++visited;
            });
        CHECK(visited == count);
    }
}

TEST_CASE("presentation measured prefix seeking uses ordered nonadditive summaries") {
    using Index = astra::ui::FileTreeIndex;
    std::vector<Index::Row> rows;
    for (std::size_t i = 0; i < 100; ++i) {
        rows.push_back(
            {std::make_shared<const Json>(entry(std::to_string(i), false)), i * 3, i * 3 + 1});
    }
    Index::Snapshot tree(std::move(rows));
    for (std::size_t target = 0; target < 310; ++target) {
        const auto result = tree.seek_prefix([&](const Index::Summary& value) {
            return value.last_source > target;
        });
        const auto expected = std::min((target + 2) / 3, std::size_t{100});
        CHECK(result.index == expected);
        if (expected < 100) {
            REQUIRE(result.item);
            CHECK(result.item->ordinal == expected * 3);
        } else {
            CHECK(result.item == nullptr);
        }
    }
}

TEST_CASE("presentation workbench layout stays bounded across DPI and narrow windows") {
    for (const auto scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        for (const auto width : {260.0F, 640.0F, 1440.0F}) {
            const auto layout = astra::ui::WorkbenchLayout::measure(
                width * scale, 900 * scale, scale, true, 300, true, true);
            CHECK(layout.editor.x == layout.sidebar.right());
            CHECK(layout.editor.right() == doctest::Approx(width * scale));
            CHECK(layout.editor.bottom() == layout.status.y);
            CHECK(layout.sidebar_rows.bottom() == layout.outline_header.y);
            CHECK(layout.outline_rows.bottom() == layout.status.y);
            CHECK(layout.editor.width >= 0);
            CHECK(layout.sidebar.width >= 0);
            if (layout.sidebar.width > 0) {
                CHECK(layout.editor.width >= 360 * scale);
            }
            CHECK(layout.command_center.x >= layout.header.x);
            CHECK(layout.command_center.right() <= layout.header.right());
            CHECK(layout.command_center.bottom() <= layout.header.bottom());
            CHECK(layout.command_center.width >= 0);
            CHECK(layout.sidebar_handle.contains(layout.editor.x, 100 * scale) ==
                  (layout.sidebar.width > 0));
        }
    }
    const auto closed = astra::ui::WorkbenchLayout::measure(1440, 900, 1, false, 300, false, true);
    CHECK(closed.editor.x == 44);
    CHECK(closed.sidebar.width == 0);
    CHECK_THROWS_AS((void)astra::ui::WorkbenchLayout::measure(0, 900, 1, true, 300, true, true),
                    std::invalid_argument);
}

TEST_CASE("presentation tabs use measured labels reveal active tabs and expose close hits") {
    auto strip = measure_tabs({44, 32, 600, 34}, 1, {40, 150, 20}, 0, 0, true);
    REQUIRE(strip.visible.size() == 3);
    CHECK(strip.visible[0].bounds.width != strip.visible[1].bounds.width);
    CHECK(strip.next.width == 0);
    CHECK(strip.visible[1].close.contains(strip.visible[1].bounds.right() - 10, 40));
    strip = measure_tabs({44, 32, 360, 34}, 1, {100, 100, 100, 100}, 0, 3, true);
    REQUIRE_FALSE(strip.visible.empty());
    CHECK(strip.visible.back().index == 3);
    CHECK(strip.next.right() == 404);
    CHECK(strip.previous.width > 0);
    for (const auto& tab : strip.visible) {
        CHECK(tab.bounds.right() <= strip.previous.x);
    }
}

TEST_CASE("presentation tab overflow preserves separate controls at every width") {
    for (const auto scale : {1.0F, 1.25F, 1.5F, 2.0F}) {
        for (const auto width : {0.0F, 20.0F, 70.0F, 160.0F, 240.0F, 308.0F, 600.0F}) {
            const astra::ui::Box bounds{17 * scale, 32 * scale, width * scale, 34 * scale};
            const auto strip =
                measure_tabs(bounds, scale, {180 * scale, 150 * scale, 40 * scale}, 0, 0, false);
            float previous_right = bounds.x;
            const auto content_right = strip.next.width > 0 ? strip.previous.x : bounds.right();
            for (const auto& tab : strip.visible) {
                CHECK(tab.bounds.width >= 96 * scale);
                CHECK(tab.bounds.x >= previous_right);
                CHECK(tab.bounds.right() <= content_right);
                CHECK(tab.close.x >= tab.bounds.x + 34 * scale);
                CHECK(tab.close.right() <= tab.bounds.right());
                CHECK(tab.close.contains(tab.close.x + tab.close.width / 2,
                                         tab.close.y + tab.close.height / 2));
                previous_right = tab.bounds.right();
            }
            if (strip.next.width > 0) {
                CHECK(strip.previous.x >= bounds.x);
                CHECK(strip.previous.right() == doctest::Approx(strip.next.x));
                CHECK(strip.next.right() == doctest::Approx(bounds.right()));
                CHECK(strip.next.width <= 26 * scale);
            }
        }
        // The old layout painted a close button over the next file icon in
        // this remaining 64-unit sliver. Only a usable first tab may be shown.
        const auto narrow = measure_tabs({0, 0, 308 * scale, 34 * scale},
                                         scale,
                                         {128 * scale, 128 * scale, 128 * scale},
                                         0,
                                         0,
                                         false);
        REQUIRE(narrow.visible.size() == 1);
        CHECK(narrow.visible.front().index == 0);
        const auto revealed = measure_tabs({0, 0, 308 * scale, 34 * scale},
                                           scale,
                                           {128 * scale, 128 * scale, 128 * scale},
                                           0,
                                           2,
                                           true);
        REQUIRE(revealed.visible.size() == 1);
        CHECK(revealed.visible.front().index == 2);
    }
    CHECK_THROWS_AS((void)measure_tabs({0, 0, -1, 34}, 1, {20}, 0, 0, false),
                    std::invalid_argument);
    CHECK_THROWS_AS((void)measure_tabs({0, 0, 100, 34}, 0, {}, 0, 0, false), std::invalid_argument);
}

TEST_CASE("presentation text panels validate atomic actions and measured tone ranges") {
    using Json = astra::protocol::Json;
    using Tone = astra::ui::TextPanelTone;
    astra::ui::TextPanelPresentation panel;
    const Json valid{
        {"actions", Json::array({{{"id", "inspect"}, {"label", "Inspect 中🙂"}, {"key", "I"}}})},
        {"lineTones",
         Json::array({{{"begin", 1}, {"end", 3}, {"tone", "added"}},
                      {{"begin", 3}, {"end", 4}, {"tone", "removed"}}})}};
    panel.reset(valid, 5);
    REQUIRE(panel.shortcut('i'));
    CHECK(panel.shortcut('I')->id == "inspect");
    CHECK(panel.shortcut('x') == nullptr);
    CHECK(panel.tone(0) == Tone::normal);
    CHECK(panel.tone(1) == Tone::added);
    CHECK(panel.tone(3) == Tone::removed);
    CHECK(panel.tone(4) == Tone::normal);
    CHECK(panel.tone(std::numeric_limits<std::size_t>::max()) == Tone::normal);
    std::vector<Json> invalid;
    auto candidate = valid;
    candidate["actions"].push_back(candidate["actions"][0]);
    invalid.push_back(candidate);
    candidate = valid;
    candidate["actions"][0]["key"] = "Enter";
    invalid.push_back(candidate);
    candidate = valid;
    candidate["actions"][0]["label"] = "bad\nlabel";
    invalid.push_back(candidate);
    candidate = valid;
    candidate["actions"][0]["id"] = std::string("bad\0id", 6);
    invalid.push_back(candidate);
    candidate = valid;
    candidate["actions"][0]["label"] = std::string(257, 'x');
    invalid.push_back(candidate);
    candidate = valid;
    candidate["actions"] = Json::array();
    for (int i = 0; i < 9; ++i)
        candidate["actions"].push_back({{"id", std::to_string(i)}, {"label", "Action"}});
    invalid.push_back(candidate);
    candidate = valid;
    candidate["lineTones"][0]["begin"] = -1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate["lineTones"][0]["begin"] = 1.5;
    invalid.push_back(candidate);
    candidate = valid;
    candidate["lineTones"][0]["end"] = 1;
    invalid.push_back(candidate);
    candidate = valid;
    candidate["lineTones"][1]["begin"] = 2;
    invalid.push_back(candidate);
    candidate = valid;
    candidate["lineTones"][1]["end"] = 6;
    invalid.push_back(candidate);
    candidate = valid;
    candidate["lineTones"][0]["tone"] = "business-command";
    invalid.push_back(candidate);
    for (const auto& spec : invalid) {
        CHECK_THROWS_AS(panel.reset(spec, 5), std::invalid_argument);
        CHECK(panel.actions().size() == 1);
        REQUIRE(panel.shortcut('i'));
        CHECK(panel.shortcut('i')->id == "inspect");
        CHECK(panel.tone(3) == Tone::removed);
    }
    auto retained = panel;
    panel.reset(Json::object(), 5);
    CHECK(panel.actions().empty());
    CHECK(panel.tone(3) == Tone::normal);
    CHECK(retained.tone(3) == Tone::removed);
}

TEST_CASE("presentation text panel row tones match an independent flat oracle") {
    using Json = astra::protocol::Json;
    using Tone = astra::ui::TextPanelTone;
    constexpr std::uint32_t seed = 0xA57A71;
    INFO(seed);
    std::mt19937 random(seed);
    astra::ui::TextPanelPresentation panel;
    for (int publication = 0; publication < 80; ++publication) {
        std::vector<Tone> expected(1024, Tone::normal);
        Json ranges = Json::array();
        for (std::size_t row = 0; row < expected.size();) {
            const auto end = std::min(expected.size(), row + 1 + random() % 17);
            if (random() % 2) {
                const auto added = random() % 2 != 0;
                for (auto index = row; index < end; ++index)
                    expected[index] = added ? Tone::added : Tone::removed;
                ranges.push_back(
                    {{"begin", row}, {"end", end}, {"tone", added ? "added" : "removed"}});
            }
            row = end;
        }
        panel.reset({{"lineTones", ranges}}, expected.size());
        for (std::size_t row = 0; row < expected.size(); ++row)
            CHECK(panel.tone(row) == expected[row]);
        CHECK(panel.tone(expected.size()) == Tone::normal);
    }
}
