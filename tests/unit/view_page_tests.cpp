#include <astra/ui/view_page.hpp>
#include <doctest/doctest.h>
#include <random>
#include <string>

namespace {
using Json = astra::protocol::Json;

Json publication(const Json& identity, std::size_t total) {
    Json result{{"version", identity.at("version")},
                {"id", identity.at("id")},
                {"revision", identity.at("revision")},
                {"activity", identity.at("activity")},
                {"offset", identity.at("offset")},
                {"total", total},
                {"empty", "No symbols"},
                {"rows", Json::array()}};
    const auto offset = result.at("offset").get<std::size_t>();
    for (std::size_t index = offset;
         index < offset + std::min(identity.at("limit").get<std::size_t>(), total - offset);
         ++index)
        result["rows"].push_back({{"id", std::to_string(index)},
                                  {"label", "Nested 中🙂"},
                                  {"depth", 7},
                                  {"icon", "function"},
                                  {"iconTone", "accent"}});
    return result;
}
} // namespace

TEST_CASE("view page identities reject retired content and preserve immutable snapshots") {
    astra::ui::ViewPage page;
    std::mt19937 random(0xA57A9A);
    Json previous;
    for (unsigned round = 0; round < 2000; ++round) {
        const auto offset = static_cast<std::size_t>(random() % 2000);
        const auto limit = static_cast<std::size_t>(1 + random() % 20);
        page.sync({"1", std::to_string(round + 1), std::to_string(round + 1), offset, limit, true});
        const auto request = page.take_request();
        REQUIRE(request);
        CHECK_FALSE(page.needs_request());
        CHECK(page.pending());
        CHECK(page.rows().size() == 0);
        if (!previous.is_null())
            CHECK_FALSE(page.publish(previous));
        auto prepared = publication(*request, offset + limit + 5);
        CHECK(page.publish(prepared));
        CHECK_FALSE(page.pending());
        CHECK(page.rows().size() == limit);
        const auto retained = page.rows().snapshot();
        const auto before = page.state();
        CHECK_FALSE(page.publish(prepared));
        CHECK(page.state() == before);
        for (std::size_t index = 0; index < limit; ++index) {
            CHECK(page.rows().at(index).depth == 7);
            CHECK(page.rows().at(index).value->at("id") == std::to_string(offset + index));
        }
        const auto selected =
            page.activate(request->at("version").get<std::string>(), std::to_string(offset));
        REQUIRE_FALSE(selected.is_null());
        CHECK(selected.at("request").at("revision") == std::to_string(round + 1));
        previous = prepared;
        page.refresh();
        CHECK(page.needs_request());
        CHECK(page.activate(request->at("version").get<std::string>(), std::to_string(offset))
                  .is_null());
        CHECK(retained.size() == limit);
        CHECK(retained.at(0).value->at("id") == std::to_string(offset));
    }
}

TEST_CASE("view page invalid publication is atomic and hidden pages cannot activate") {
    astra::ui::ViewPage page;
    page.sync({"1", "9", "2", 40, 3, true});
    const auto request = *page.take_request();
    const auto before = page.state();
    auto valid = publication(request, 100);
    for (const auto bad : {"offset", "total", "depth", "group", "duplicate", "iconTone"}) {
        auto invalid = valid;
        if (std::string_view(bad) == "offset")
            invalid["offset"] = 39;
        if (std::string_view(bad) == "total")
            invalid["total"] = -1;
        if (std::string_view(bad) == "depth")
            invalid["rows"][0]["depth"] = 65;
        if (std::string_view(bad) == "group")
            invalid["rows"][0]["group"] = true;
        if (std::string_view(bad) == "duplicate")
            invalid["rows"][1]["id"] = "40";
        if (std::string_view(bad) == "iconTone")
            invalid["rows"][0]["iconTone"] = "unknown";
        CHECK_THROWS(static_cast<void>(page.publish(invalid)));
        CHECK(page.state() == before);
    }
    auto shrunk = valid;
    shrunk["offset"] = 2;
    shrunk["total"] = 5;
    CHECK(page.publish(shrunk));
    CHECK(page.offset() == 2);
    CHECK(page.rows().size() == 3);
    page.sync({"1", "9", "2", 2, 0, false});
    CHECK_FALSE(page.publish(valid));
    CHECK(page.rows().size() == 0);
    CHECK(page.activate(request.at("version").get<std::string>(), "40").is_null());
    page.sync({"1", "9", "3", 2, 3, true});
    CHECK_FALSE(page.publish(valid));
    const auto fresh = page.state();
    auto oversized = publication(fresh, 10);
    oversized["rows"][0]["data"] = std::string(8193, 'a');
    CHECK_THROWS(static_cast<void>(page.publish(oversized)));
    CHECK(page.state() == fresh);
}
