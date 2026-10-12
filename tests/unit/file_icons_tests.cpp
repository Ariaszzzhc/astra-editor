#include <astra/ui/file_icons.hpp>
#include <doctest/doctest.h>
#include <string>

namespace {
using Json = astra::protocol::Json;

Json resolved(const Json& batch, std::string glyph = "code", std::string tone = "codeIcon") {
    auto result = batch;
    for (auto& item : result.at("items")) {
        item["icon"] = {{"glyph", glyph}, {"tone", tone}};
    }
    return result;
}
} // namespace

TEST_CASE("resource icons preserve upstream variants and legacy descriptors") {
    using namespace astra::ui;
    const auto resource = FileIcon::parse({{"asset", "material/python"},
                                           {"lightAsset", "codicons/warning"},
                                           {"openAsset", "material/folder-open"},
                                           {"openLightAsset", "codicons/search"}});
    WorkbenchStyle::Colors dark;
    dark.background = 0xff181818;
    auto light = dark;
    light.background = 0xfffafafa;
    CHECK(icon_asset_name(resource.drawing_asset(dark)) == "material/python");
    CHECK(icon_asset_name(resource.drawing_asset(light)) == "codicons/warning");
    CHECK(icon_asset_name(resource.drawing_asset(dark, true)) == "material/folder-open");
    CHECK(icon_asset_name(resource.drawing_asset(light, true)) == "codicons/search");
    const auto legacy = FileIcon::parse({{"glyph", "warning"}, {"tone", "#123456"}});
    CHECK(icon_asset_name(legacy.drawing_asset(light, true)) == "codicons/warning");
    CHECK(legacy.color(light) == 0xff123456);
    CHECK_THROWS_AS(static_cast<void>(FileIcon::parse({{"asset", "material/../python"}})),
                    astra::protocol::RpcError);
    CHECK_THROWS_AS(static_cast<void>(FileIcon::parse({{"asset", "material/not-installed"}})),
                    astra::protocol::RpcError);
    CHECK_THROWS_AS(
        static_cast<void>(FileIcon::parse({{"asset", "material/python"}, {"glyph", "warning"}})),
        astra::protocol::RpcError);
    CHECK_THROWS_AS(static_cast<void>(FileIcon::parse({{"asset", 17}})), astra::protocol::RpcError);
    CHECK_THROWS_AS(
        static_cast<void>(FileIcon::parse({{"asset", "material/python"}, {"tone", "missing"}})),
        astra::protocol::RpcError);
    CHECK_THROWS_AS(static_cast<void>(
                        FileIcon::parse({{"asset", "material/python"}, {"lightAsset", "unknown"}})),
                    astra::protocol::RpcError);
}

TEST_CASE("resource icon batch rejection keeps all warm entries") {
    astra::ui::FileIcons icons;
    static_cast<void>(icons.lookup("a.py", false));
    static_cast<void>(icons.lookup("b.py", false));
    auto batch = *icons.begin();
    auto result = batch;
    for (auto& item : result.at("items")) {
        item["icon"] = {{"asset", "material/python"}};
    }
    icons.finish(batch, &result);
    icons.invalidate();
    static_cast<void>(icons.lookup("a.py", false));
    static_cast<void>(icons.lookup("b.py", false));
    batch = *icons.begin();
    result = batch;
    result["items"][0]["icon"] = {{"asset", "material/cpp"}};
    result["items"][1]["icon"] = {{"asset", "material/missing"}};
    CHECK_THROWS_AS(icons.finish(batch, &result), astra::protocol::RpcError);
    for (const auto name : {"a.py", "b.py"}) {
        CHECK(astra::ui::icon_asset_name(icons.lookup(name, false).drawing_asset({})) ==
              "material/python");
    }
    CHECK(icons.state().at("resolutions") == 2);
}

TEST_CASE("presentation file icons retain generations and warm descriptors") {
    astra::ui::FileIcons icons;
    CHECK(icons.lookup("source.cpp", false).glyph == astra::ui::Icon::document);
    CHECK(icons.lookup("source.cpp", true).glyph == astra::ui::Icon::document);
    auto first = icons.begin();
    REQUIRE(first);
    REQUIRE(first->at("items").size() == 2);
    CHECK_FALSE(icons.begin());
    auto result = resolved(*first);
    icons.finish(*first, &result);
    CHECK(icons.idle());
    for (int i = 0; i < 100; ++i) {
        CHECK(icons.lookup("source.cpp", false).glyph == astra::ui::Icon::code);
    }
    CHECK(icons.idle());
    CHECK(icons.state().at("resolutions") == 2);
    icons.invalidate();
    CHECK(icons.lookup("source.cpp", false).glyph == astra::ui::Icon::code);
    auto obsolete = icons.begin();
    REQUIRE(obsolete);
    icons.invalidate();
    CHECK(icons.lookup("source.cpp", false).glyph == astra::ui::Icon::code);
    auto stale = resolved(*obsolete, "warning", "error");
    icons.finish(*obsolete, &stale);
    CHECK(icons.lookup("source.cpp", false).glyph == astra::ui::Icon::code);
    auto current = icons.begin();
    REQUIRE(current);
    auto changed = resolved(*current, "braces", "#123456");
    icons.finish(*current, &changed);
    const auto icon = icons.lookup("source.cpp", false);
    CHECK(icon.glyph == astra::ui::Icon::braces);
    CHECK(icon.color({}) == 0xff123456U);
    CHECK(icons.idle());
}

TEST_CASE("presentation file icons reject malformed batches atomically") {
    astra::ui::FileIcons icons;
    static_cast<void>(icons.lookup("a", false));
    static_cast<void>(icons.lookup("b", false));
    const auto batch = icons.begin();
    REQUIRE(batch);
    auto result = resolved(*batch);
    SUBCASE("unknown glyph") {
        result["items"][1]["icon"]["glyph"] = "missing";
    }
    SUBCASE("unknown tone") {
        result["items"][1]["icon"]["tone"] = "missing";
    }
    SUBCASE("identity mismatch") {
        result["items"][1]["path"] = "a";
    }
    SUBCASE("incomplete batch") {
        result["items"].erase(1);
    }
    SUBCASE("extra descriptor field") {
        result["items"][1]["icon"]["extra"] = true;
    }
    CHECK_THROWS(icons.finish(*batch, &result));
    CHECK(icons.lookup("a", false).glyph == astra::ui::Icon::document);
    CHECK(icons.lookup("b", false).glyph == astra::ui::Icon::document);
    CHECK(icons.state().at("resolutions") == 0);
    CHECK(icons.idle());
    icons.invalidate();
    static_cast<void>(icons.lookup("a", false));
    const auto retry = icons.begin();
    REQUIRE(retry);
    const auto valid = resolved(*retry);
    icons.finish(*retry, &valid);
    CHECK(icons.lookup("a", false).glyph == astra::ui::Icon::code);
}

TEST_CASE("presentation file icons bound admission eviction and batches") {
    astra::ui::FileIcons icons;
    for (int i = 0; i < 10000; ++i) {
        static_cast<void>(icons.lookup("path/" + std::to_string(i), false));
        CHECK(icons.state().at("entries").get<std::size_t>() <= icons.max_entries);
        CHECK(icons.state().at("pending").get<std::size_t>() <= icons.max_pending);
        CHECK(icons.state().at("bytes").get<std::size_t>() <= icons.max_bytes);
    }
    while (const auto batch = icons.begin()) {
        CHECK(batch->at("items").size() <= icons.batch_size);
        const auto result = resolved(*batch);
        icons.finish(*batch, &result);
    }
    CHECK(icons.idle());
    for (int i = 0; i < 200; ++i) {
        static_cast<void>(icons.lookup(std::string(32760, 'x') + std::to_string(i), false));
    }
    CHECK(icons.state().at("bytes").get<std::size_t>() <= icons.max_bytes);
    while (const auto batch = icons.begin()) {
        std::size_t bytes{};
        for (const auto& item : batch->at("items"))
            bytes += item.at("path").get<std::string>().size();
        CHECK(bytes <= icons.max_batch_bytes);
        const auto result = resolved(*batch);
        icons.finish(*batch, &result);
    }
    const auto before = icons.state();
    CHECK_THROWS(static_cast<void>(icons.lookup(std::string(32769, 'x'), false)));
    CHECK_THROWS(static_cast<void>(icons.lookup(std::string("a\0b", 3), false)));
    CHECK_THROWS(static_cast<void>(icons.lookup("\xff", false)));
    CHECK(icons.state() == before);
}
