#include <astra/ui/editor_tooltip.hpp>
#include <doctest/doctest.h>

namespace {
using Json = astra::protocol::Json;

Json target(std::string revision = "1", std::size_t offset = 7) {
    return {{"id", "document"}, {"baseRevision", revision}, {"byteOffset", offset}};
}
} // namespace

TEST_CASE("editor tooltip coalesces pointer changes and fences stale publication") {
    astra::ui::EditorTooltip tooltip;
    CHECK_FALSE(tooltip.take_request());
    CHECK_FALSE(tooltip.publish({{"version", "0"}, {"text", "old"}}));
    tooltip.sync(target());
    const auto first = *tooltip.take_request();
    CHECK(tooltip.pending());
    tooltip.sync(target());
    CHECK_FALSE(tooltip.take_request());
    CHECK(tooltip.publish({{"version", first.at("version")}, {"text", "函数 🙂"}}));
    CHECK_FALSE(tooltip.pending());
    tooltip.sync(target("1", 8));
    tooltip.sync(target("2", 8));
    CHECK(tooltip.text().empty());
    const auto latest = *tooltip.take_request();
    CHECK(latest.at("target") == target("2", 8));
    CHECK_FALSE(tooltip.publish({{"version", first.at("version")}, {"text", "obsolete"}}));
    CHECK(tooltip.pending());
    CHECK(tooltip.publish({{"version", latest.at("version")}, {"text", "current"}}));
    tooltip.sync(nullptr);
    CHECK_FALSE(tooltip.pending());
    CHECK(tooltip.text().empty());
    CHECK(tooltip.take_request()->at("target").is_null());
    CHECK_FALSE(tooltip.publish({{"version", latest.at("version")}, {"text", "late"}}));
}

TEST_CASE("editor tooltip validates bounded Unicode atomically") {
    astra::ui::EditorTooltip tooltip;
    tooltip.sync(target());
    const auto version = tooltip.take_request()->at("version");
    REQUIRE(tooltip.publish({{"version", version}, {"text", "kept"}}));
    for (const auto& text :
         {std::string(16385, 'a'), std::string("a\0b", 3), std::string("\xff", 1)}) {
        CHECK_THROWS(static_cast<void>(tooltip.publish({{"version", version}, {"text", text}})));
        CHECK(tooltip.text() == "kept");
    }
    CHECK_THROWS(
        static_cast<void>(tooltip.publish({{"version", version}, {"text", "x"}, {"extra", true}})));
    CHECK(tooltip.text() == "kept");
    CHECK(tooltip.publish({{"version", version}, {"text", std::string(16384, 'a')}}));
    CHECK(tooltip.text().size() == 16384);
    CHECK(tooltip.publish({{"version", version}, {"text", ""}}));
    CHECK_FALSE(tooltip.pending());
    CHECK(tooltip.text().empty());
}
