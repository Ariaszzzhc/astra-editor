#include <astra/protocol/framing.hpp>
#include <astra/service/editor_service.hpp>
#include <astra/service/metadata_clock.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using astra::protocol::Json;
using astra::service::EditorService;

Json service_request(std::string method, Json params = Json::object(), Json id = "request") {
    return Json{{"jsonrpc", "2.0"},
                {"method", std::move(method)},
                {"params", std::move(params)},
                {"id", std::move(id)}};
}

Json call(EditorService& service, std::string method, Json params = Json::object()) {
    const auto response = service.dispatch(service_request(std::move(method), std::move(params)));
    REQUIRE(response);
    INFO(response->dump());
    REQUIRE_FALSE(response->contains("error"));
    REQUIRE(response->contains("result"));
    return (*response)["result"];
}

void service_error(EditorService& service, std::string method, Json params, int code) {
    const auto response = service.dispatch(service_request(std::move(method), std::move(params)));
    REQUIRE(response);
    INFO(response->dump());
    REQUIRE(response->contains("error"));
    CHECK((*response)["error"]["code"] == code);
    CHECK((*response)["id"] == "request");
}

std::string create_document(EditorService& service, std::string text = {}) {
    return call(service, "document.create", Json{{"text", std::move(text)}})["id"]
        .get<std::string>();
}

Json get_document(EditorService& service, const std::string& id) {
    return call(service, "document.get", Json{{"id", id}});
}

std::optional<Json>
framed_dispatch(EditorService& service, const Json& request, std::mt19937& random) {
    const auto frame = astra::protocol::encode_frame(request.dump());
    astra::protocol::FrameDecoder decoder;
    std::vector<std::string> bodies;
    std::size_t offset = 0;
    while (offset < frame.size()) {
        const auto count = std::min<std::size_t>(1 + random() % 19U, frame.size() - offset);
        auto decoded = decoder.feed(std::string_view(frame).substr(offset, count));
        bodies.insert(bodies.end(), decoded.begin(), decoded.end());
        offset += count;
    }
    decoder.finish();
    REQUIRE(bodies.size() == 1);
    const auto response = service.dispatch_text(bodies.front());
    if (!response) {
        return std::nullopt;
    }
    // Round-trip the response through the same production framing code too.
    astra::protocol::FrameDecoder response_decoder;
    const auto response_bodies = response_decoder.feed(astra::protocol::encode_frame(*response));
    REQUIRE(response_bodies.size() == 1);
    response_decoder.finish();
    return Json::parse(response_bodies.front());
}

} // namespace

TEST_CASE("service metadata clock preserves publication across abandoned preparations") {
    astra::service::DocumentMetadataClock clock;
    const astra::BufferId id{1};
    const Json a{{"revision", "0"}, {"head", 1}}, b{{"revision", "0"}, {"head", 2}};
    const auto original = clock.observe(id, a);
    const auto proposed = clock.prepare(id, b);
    CHECK(proposed.at("metadataSequence") != original.at("metadataSequence"));
    CHECK(clock.observe(id, a) == original);
    CHECK_FALSE(clock.publish(id));
    const auto accepted_b = clock.prepare(id, b);
    REQUIRE(clock.publish(id));
    CHECK(clock.observe(id, b) == accepted_b);
    const auto returned_a = clock.observe(id, a);
    CHECK(std::stoull(returned_a.at("metadataSequence").get<std::string>()) >
          std::stoull(accepted_b.at("metadataSequence").get<std::string>()));
    const auto new_binding = clock.prepare(id, a, true);
    REQUIRE(clock.publish(id));
    CHECK(clock.observe(id, a) == new_binding);
    CHECK(new_binding.at("metadataSequence") != returned_a.at("metadataSequence"));
    auto exposed = clock.observe(id, a);
    exposed["head"] = 99;
    CHECK(clock.observe(id, a) == new_binding);
    CHECK(original.at("head") == 1);
    CHECK(proposed.at("head") == 2);
    std::mt19937 random(0xA57A96);
    std::map<std::uint64_t, Json> current;
    std::map<std::uint64_t, Json> snapshots;
    for (std::uint64_t buffer = 2; buffer <= 9; ++buffer) {
        current[buffer] = a;
        snapshots[buffer] = clock.observe(astra::BufferId{buffer}, a);
    }
    for (int round = 0; round < 2000; ++round) {
        const auto buffer = 2 + random() % 8;
        const astra::BufferId target{buffer};
        const Json next{{"revision", "0"}, {"head", 10 + round}};
        const auto candidate = clock.prepare(target, next);
        CHECK(clock.prepare(target, next) == candidate);
        if (random() % 2) {
            REQUIRE(clock.publish(target));
            CHECK(clock.observe(target, next) == candidate);
            current[buffer] = next;
            snapshots[buffer] = candidate;
        } else {
            CHECK(clock.observe(target, current.at(buffer)) == snapshots.at(buffer));
            CHECK_FALSE(clock.publish(target));
        }
    }
    REQUIRE(clock.retained_buffers() == 9);
    for (std::uint64_t buffer = 1; buffer <= 9; ++buffer)
        clock.forget(astra::BufferId{buffer});
    CHECK(clock.retained_buffers() == 0);
}

TEST_CASE("service metadata clock uses exact decimal markers and rejects exhausted changes") {
    const astra::BufferId id{1};
    const Json a{{"revision", "0"}, {"head", 1}}, b{{"revision", "0"}, {"head", 2}};
    astra::service::DocumentMetadataClock precise(9007199254740993ULL);
    CHECK(precise.observe(id, a).at("metadataSequence") == "9007199254740993");
    CHECK(precise.observe(id, b).at("metadataSequence") == "9007199254740994");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    astra::service::DocumentMetadataClock exhausted(maximum - 1);
    const auto baseline = exhausted.observe(id, a);
    CHECK(baseline.at("metadataSequence") == std::to_string(maximum - 1));
    CHECK_THROWS_AS(static_cast<void>(exhausted.prepare(id, b)), astra::protocol::RpcError);
    CHECK_THROWS_AS(static_cast<void>(exhausted.observe(id, b)), astra::protocol::RpcError);
    CHECK(exhausted.observe(id, a) == baseline);
    CHECK_THROWS_AS(exhausted.require_capacity(2), astra::protocol::RpcError);
    CHECK_THROWS_AS(static_cast<void>(exhausted.observe({}, a)), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(exhausted.observe(id, Json::array())), std::invalid_argument);
    CHECK_THROWS_AS(static_cast<void>(exhausted.prepare(id, {{"metadataSequence", "7"}})),
                    std::invalid_argument);
    CHECK(exhausted.observe(id, a) == baseline);
}

TEST_CASE("service metadata snapshots order selection-only changes and stable reads") {
    EditorService service;
    const auto first = call(service, "document.create", {{"text", "0123456789"}});
    const auto id = first.at("id");
    const auto sequence = [](const Json& metadata) {
        REQUIRE(metadata.at("metadataSequence").is_string());
        return std::stoull(metadata.at("metadataSequence").get<std::string>());
    };
    auto latest = sequence(first);
    const auto created = service.take_notifications();
    REQUIRE(created.size() == 1);
    CHECK(sequence(created.front().at("params")) == latest);
    for (std::size_t round = 1; round <= 200; ++round) {
        const auto head = 1 + round % 9;
        const auto moved =
            call(service,
                 "document.setSelections",
                 {{"id", id},
                  {"baseRevision", "0"},
                  {"selections", Json::array({{{"anchor", head}, {"head", head}}})}});
        CHECK(moved.at("revision") == "0");
        CHECK(moved.at("dirty") == false);
        CHECK(moved.at("canUndo") == false);
        CHECK(sequence(moved) > latest);
        const auto events = service.take_notifications();
        REQUIRE(events.size() == 1);
        const auto& notification = events.front().at("params");
        CHECK(sequence(notification) > latest);
        CHECK(sequence(notification) == sequence(moved));
        CHECK(notification.at("selections") == moved.at("selections"));
        const auto read = call(service, "document.get", {{"id", id}});
        CHECK(read.at("revision") == "0");
        CHECK(read.at("text") == "0123456789");
        CHECK(read.at("selections") == moved.at("selections"));
        CHECK(sequence(read) == sequence(moved));
        latest = sequence(read);
    }
    const auto other = call(service, "document.create", {{"text", "other"}});
    CHECK(sequence(other) > latest);
    const auto listed = call(service, "document.list");
    REQUIRE(listed.size() == 2);
    CHECK(sequence(listed[0]) == latest);
    CHECK(sequence(listed[1]) == sequence(other));
}

TEST_CASE("service viewport pages long Unicode lines without losing bytes") {
    EditorService service;
    const std::string line = std::string(65534, 'x') + "\xF0\x9F\x98\x80" + "tail";
    const auto id = create_document(service, line + "\r\nnext\n");
    auto view = call(service, "document.lines", {{"id", id}, {"firstLine", 0}, {"count", 3}});
    REQUIRE(view["lines"].size() == 1);
    CHECK(view["lines"][0]["text"] == std::string(65534, 'x'));
    CHECK_FALSE(view["lines"][0]["complete"].get<bool>());
    REQUIRE(view.contains("continuation"));
    CHECK(view["continuation"]["begin"] == 65534);
    view = call(
        service, "document.lines", {{"id", id}, {"firstLine", 0}, {"begin", 65534}, {"count", 3}});
    REQUIRE(view["lines"].size() == 3);
    CHECK(view["lines"][0]["text"] == "\xF0\x9F\x98\x80tail");
    CHECK(view["lines"][0]["nextBegin"] == line.size() + 2);
    CHECK(view["lines"][1]["text"] == "next");
    CHECK(view["lines"][2]["text"] == "");
    CHECK(view["nextLine"] == 3);
    service_error(
        service, "document.lines", {{"id", id}, {"firstLine", 0}, {"begin", 65535}}, -32602);
    service_error(service, "document.lines", {{"id", id}, {"firstLine", 3}}, -32602);
    service_error(service, "document.lines", {{"id", id}, {"count", 0}}, -32602);
}

TEST_CASE("service editing commands preserve revision and Unicode semantics") {
    EditorService service;
    const auto id = create_document(service, "a\xCC\x81\nword");
    auto result = call(service,
                       "document.command",
                       {{"id", id}, {"baseRevision", "0"}, {"command", "move_right"}});
    CHECK(result["revision"] == "0");
    CHECK(result["selections"][0]["head"] == 3);
    CHECK(result["selectionPositions"][0]["utf16Column"] == 2);
    result = call(
        service, "document.command", {{"id", id}, {"baseRevision", "0"}, {"command", "backspace"}});
    CHECK(result["revision"] == "1");
    CHECK(get_document(service, id)["text"] == "\nword");
    service_error(service,
                  "document.command",
                  {{"id", id}, {"baseRevision", "0"}, {"command", "backspace"}},
                  -32001);
    service_error(service,
                  "document.command",
                  {{"id", id}, {"baseRevision", "1"}, {"command", "unknown"}},
                  -32602);
    service_error(service,
                  "document.command",
                  {{"id", id}, {"baseRevision", "1"}, {"command", "move_left"}, {"extend", 1}},
                  -32602);
    const auto listed = call(service, "document.list");
    REQUIRE(listed.size() == 1);
    CHECK(listed[0]["revision"] == "1");
    CHECK_FALSE(listed[0].contains("text"));
}

TEST_CASE("service creates isolated documents and exposes complete small snapshots") {
    EditorService service;
    CHECK(service.document_count() == 0);
    const auto created = call(service, "document.create", Json{{"text", "one\r\ntwo"}});
    CHECK(created["id"] == "1");
    CHECK(created["revision"] == "0");
    CHECK(created["sizeBytes"] == 8);
    CHECK(created["sizeUtf16"] == 8);
    CHECK(created["lineCount"] == 2);
    CHECK(created["dirty"] == false);
    CHECK(created["canUndo"] == false);
    CHECK(created["canRedo"] == false);
    CHECK_FALSE(created.contains("text"));
    CHECK(created["selections"] == Json::array({Json{{"anchor", 0}, {"head", 0}}}));
    const auto second = call(service, "document.create");
    CHECK(second["id"] == "2");
    CHECK(service.document_count() == 2);
    const auto snapshot = get_document(service, "1");
    CHECK(snapshot["text"] == "one\r\ntwo");
    CHECK(snapshot["range"] == Json{{"begin", 0}, {"end", 8}});
    CHECK(snapshot["full"] == true);
    CHECK(get_document(service, "2")["text"] == "");
    const auto events = service.take_notifications();
    REQUIRE(events.size() == 2);
    CHECK(events[0]["method"] == "document.created");
    CHECK(events[0]["params"]["id"] == "1");
    CHECK_FALSE(events[0]["params"].contains("text"));
    CHECK(service.take_notifications().empty());
}

TEST_CASE("service multicursor insertion undo and redo share Document semantics") {
    EditorService service;
    const auto id = create_document(service, "abcd");
    const auto selected = call(
        service,
        "document.setSelections",
        Json{{"id", id},
             {"baseRevision", "0"},
             {"selections",
              Json::array({Json{{"anchor", 1}, {"head", 1}}, Json{{"anchor", 3}, {"head", 3}}})}});
    CHECK(selected["revision"] == "0");
    const auto inserted =
        call(service, "document.insert", Json{{"id", id}, {"baseRevision", "0"}, {"text", "!"}});
    CHECK(inserted["revision"] == "1");
    CHECK(inserted["dirty"] == true);
    CHECK(inserted["canUndo"] == true);
    CHECK(inserted["canRedo"] == false);
    CHECK_FALSE(inserted.contains("text"));
    CHECK(get_document(service, id)["text"] == "a!bc!d");
    CHECK(inserted["selections"] ==
          Json::array({Json{{"anchor", 2}, {"head", 2}}, Json{{"anchor", 5}, {"head", 5}}}));
    const auto undone = call(service, "document.undo", Json{{"id", id}, {"baseRevision", "1"}});
    CHECK(undone["revision"] == "2");
    CHECK(undone["dirty"] == false);
    CHECK(undone["canRedo"] == true);
    CHECK(undone["selections"] == selected["selections"]);
    CHECK(get_document(service, id)["text"] == "abcd");
    const auto redone = call(service, "document.redo", Json{{"id", id}, {"baseRevision", "2"}});
    CHECK(redone["revision"] == "3");
    CHECK(get_document(service, id)["text"] == "a!bc!d");
    const auto events = service.take_notifications();
    REQUIRE(events.size() == 5);
    CHECK(events[1]["params"]["reason"] == "selections");
    CHECK(events[2]["params"]["reason"] == "insert");
    CHECK(events[3]["params"]["reason"] == "undo");
    CHECK(events[4]["params"]["reason"] == "redo");
}

TEST_CASE("service edits use one base snapshot and restore selection history") {
    EditorService service;
    const auto id = create_document(service, "abcdef");
    const auto result =
        call(service,
             "document.edit",
             Json{{"id", id},
                  {"baseRevision", "0"},
                  {"edits",
                   Json::array({Json{{"begin", 4}, {"end", 6}, {"text", "Y"}},
                                Json{{"begin", 0}, {"end", 2}, {"text", "XX"}}})},
                  {"selectionsAfter", Json::array({Json{{"anchor", 5}, {"head", 2}}})}});
    CHECK(result["revision"] == "1");
    CHECK(result["selections"] == Json::array({Json{{"anchor", 5}, {"head", 2}}}));
    CHECK(get_document(service, id)["text"] == "XXcdY");
    (void)call(service, "document.undo", Json{{"id", id}, {"baseRevision", "1"}});
    CHECK(get_document(service, id)["text"] == "abcdef");
    (void)call(service, "document.redo", Json{{"id", id}, {"baseRevision", "2"}});
    CHECK(get_document(service, id)["text"] == "XXcdY");
    (void)call(service, "document.undo", Json{{"id", id}, {"baseRevision", "3"}});
    const auto branch =
        call(service, "document.insert", Json{{"id", id}, {"baseRevision", "4"}, {"text", "new "}});
    CHECK(branch["canRedo"] == false);
    const auto no_redo = call(service, "document.redo", Json{{"id", id}, {"baseRevision", "5"}});
    CHECK(no_redo["revision"] == "5");
}

TEST_CASE(
    "service invalid and stale transactions leave text selection history and events unchanged") {
    EditorService service;
    const std::string emoji = "\xF0\x9F\x98\x80";
    const auto id = create_document(service, "abc" + emoji + "def");
    (void)service.take_notifications();
    const auto before = get_document(service, id);
    const std::vector<Json> invalid_edits{
        Json::array({Json{{"begin", 0}, {"end", 1}, {"text", "X"}},
                     Json{{"begin", 99}, {"end", 99}, {"text", "!"}}}),
        Json::array({Json{{"begin", 0}, {"end", 2}, {"text", "X"}},
                     Json{{"begin", 1}, {"end", 3}, {"text", "Y"}}}),
        Json::array({Json{{"begin", 4}, {"end", 4}, {"text", "X"}}}),
        Json::array({Json{{"begin", 2}, {"end", 1}, {"text", "X"}}}),
        Json::array({Json{{"begin", -1}, {"end", 0}, {"text", "X"}}}),
        Json::array({Json{{"begin", 0}, {"end", 0}, {"text", 9}}}),
        Json::array({Json{{"begin", 0}, {"end", 0}, {"text", "A"}},
                     Json{{"begin", 0}, {"end", 0}, {"text", "B"}}})};
    for (const auto& edits : invalid_edits) {
        service_error(service,
                      "document.edit",
                      Json{{"id", id}, {"baseRevision", "0"}, {"edits", edits}},
                      -32602);
        CHECK(get_document(service, id) == before);
        CHECK(service.take_notifications().empty());
    }
    service_error(service,
                  "document.edit",
                  Json{{"id", id},
                       {"baseRevision", "0"},
                       {"edits", Json::array({Json{{"begin", 0}, {"end", 1}, {"text", "valid"}}})},
                       {"selectionsAfter", Json::array({Json{{"anchor", 999}, {"head", 999}}})}},
                  -32602);
    CHECK(get_document(service, id) == before);
    service_error(service,
                  "document.insert",
                  Json{{"id", id}, {"baseRevision", "99"}, {"text", "stale"}},
                  -32001);
    CHECK(get_document(service, id) == before);
    CHECK(service.take_notifications().empty());
}

TEST_CASE("service requires canonical string IDs revisions and safe integer byte coordinates") {
    EditorService service;
    const auto id = create_document(service, "abc");
    const std::vector<Json> invalid_ids{
        1, -1, 1.0, true, nullptr, "", "01", "-1", "+1", " 1", "1 ", "18446744073709551616"};
    for (const auto& value : invalid_ids) {
        service_error(service, "document.get", Json{{"id", value}}, -32602);
    }
    service_error(service, "document.get", Json{{"id", "18446744073709551615"}}, -32002);
    service_error(service, "document.undo", Json{{"id", id}, {"baseRevision", 0}}, -32602);
    service_error(service, "document.undo", Json{{"id", id}, {"baseRevision", "00"}}, -32602);
    service_error(service,
                  "document.undo",
                  Json{{"id", id}, {"baseRevision", "18446744073709551615"}},
                  -32001);
    const std::vector<Json> invalid_offsets{-1, 0.5, 1.0, true, "0", nullptr, 9007199254740992ULL};
    for (const auto& value : invalid_offsets) {
        service_error(service, "document.get", Json{{"id", id}, {"begin", value}}, -32602);
    }
    service_error(service, "document.get", Json{{"id", id}, {"unexpected", true}}, -32602);
    service_error(service, "document.get", Json::array({id}), -32602);
    service_error(service, "document.insert", Json{{"id", id}, {"text", "x"}}, -32602);
    service_error(service, "document.create", Json{{"text", false}}, -32602);
    CHECK(service.document_count() == 1);
}

TEST_CASE("service read paging is UTF-8 aligned and explicit invalid ranges are rejected") {
    EditorService service;
    const std::string emoji = "\xF0\x9F\x98\x80";
    const auto text = std::string(EditorService::max_read_bytes - 1, 'a') + emoji + "tail";
    const auto id = create_document(service, text);
    const auto first = get_document(service, id);
    CHECK(first["text"] == std::string(EditorService::max_read_bytes - 1, 'a'));
    CHECK(first["range"]["begin"] == 0);
    CHECK(first["range"]["end"] == EditorService::max_read_bytes - 1);
    CHECK(first["sizeBytes"] == text.size());
    CHECK(first["full"] == false);
    const auto next =
        call(service,
             "document.get",
             Json{{"id", id}, {"begin", first["range"]["end"]}, {"end", text.size()}});
    CHECK(next["text"] == emoji + "tail");
    CHECK(next["range"]["end"] == text.size());
    CHECK(next["full"] == false);
    const auto empty = call(
        service, "document.get", Json{{"id", id}, {"begin", text.size()}, {"end", text.size()}});
    CHECK(empty["text"] == "");
    service_error(service, "document.get", Json{{"id", id}, {"begin", 4}, {"end", 2}}, -32602);
    service_error(service, "document.get", Json{{"id", id}, {"end", text.size() + 1}}, -32602);
    service_error(service,
                  "document.get",
                  Json{{"id", id}, {"begin", EditorService::max_read_bytes}},
                  -32602);
    service_error(
        service, "document.get", Json{{"id", id}, {"end", EditorService::max_read_bytes}}, -32602);
}

TEST_CASE("service documents can grow beyond the frame limit without flattening mutation results") {
    EditorService service;
    const auto id = create_document(service);
    const std::string chunk(3 * 1024 * 1024, 'x');
    for (std::size_t index = 0; index < 3; ++index) {
        const auto result =
            call(service,
                 "document.insert",
                 Json{{"id", id}, {"baseRevision", std::to_string(index)}, {"text", chunk}});
        CHECK(result["sizeBytes"] == (index + 1) * chunk.size());
        CHECK_FALSE(result.contains("text"));
        CHECK(result.dump().size() < 1024);
    }
    const auto whole_bytes = 3 * chunk.size();
    REQUIRE(whole_bytes > astra::protocol::Dispatcher::max_body_bytes);
    const auto first = get_document(service, id);
    CHECK(first["text"].get_ref<const std::string&>().size() == EditorService::max_read_bytes);
    CHECK(first["full"] == false);
    const auto tail = call(service, "document.get", Json{{"id", id}, {"begin", whole_bytes - 20}});
    CHECK(tail["text"] == std::string(20, 'x'));
    CHECK(tail["range"]["end"] == whole_bytes);
    const auto events = service.take_notifications();
    REQUIRE(events.size() == 4);
    for (const auto& event : events) {
        CHECK(event.dump().size() < 1024);
        CHECK_FALSE(event["params"].contains("text"));
    }
}

TEST_CASE("service notifications execute edits without protocol responses") {
    EditorService service;
    const auto id = create_document(service, "abc");
    (void)service.take_notifications();
    auto insert =
        service_request("document.insert", Json{{"id", id}, {"baseRevision", "0"}, {"text", "!"}});
    insert.erase("id");
    CHECK_FALSE(service.dispatch(insert));
    CHECK(get_document(service, id)["text"] == "!abc");
    const auto events = service.take_notifications();
    REQUIRE(events.size() == 1);
    CHECK(events[0]["method"] == "document.changed");
    CHECK(events[0]["params"]["revision"] == "1");
    CHECK_FALSE(events[0].contains("id"));
    // A stale notification also has no reply and must not mutate the buffer.
    CHECK_FALSE(service.dispatch_text(insert.dump()));
    CHECK(get_document(service, id)["text"] == "!abc");
    CHECK(service.take_notifications().empty());
}

TEST_CASE("service close protects unsaved documents and reports closed identities") {
    EditorService service;
    const auto id = create_document(service, "abc");
    (void)call(service, "document.insert", Json{{"id", id}, {"baseRevision", "0"}, {"text", "!"}});
    (void)service.take_notifications();
    service_error(service, "document.close", Json{{"id", id}, {"baseRevision", "0"}}, -32001);
    service_error(service, "document.close", Json{{"id", id}, {"baseRevision", "1"}}, -32003);
    service_error(service,
                  "document.close",
                  Json{{"id", id}, {"baseRevision", "1"}, {"discardChanges", 1}},
                  -32602);
    CHECK(service.document_count() == 1);
    CHECK(service.take_notifications().empty());
    const auto closed = call(service,
                             "document.close",
                             Json{{"id", id}, {"baseRevision", "1"}, {"discardChanges", true}});
    CHECK(closed == Json{{"id", id}, {"closed", true}});
    CHECK(service.document_count() == 0);
    const auto events = service.take_notifications();
    REQUIRE(events.size() == 1);
    CHECK(events[0]["method"] == "document.closed");
    CHECK(events[0]["params"]["id"] == id);
    service_error(service, "document.get", Json{{"id", id}}, -32002);
    const auto next = create_document(service);
    CHECK(next != id);
    CHECK(call(service, "document.close", Json{{"id", next}, {"baseRevision", "0"}})["closed"] ==
          true);
}

TEST_CASE("service no-op commands do not advance revision or enqueue changes") {
    EditorService service;
    const auto id = create_document(service, "abc");
    (void)service.take_notifications();
    const auto before = get_document(service, id);
    (void)call(service, "document.undo", Json{{"id", id}, {"baseRevision", "0"}});
    (void)call(service, "document.redo", Json{{"id", id}, {"baseRevision", "0"}});
    (void)call(service, "document.insert", Json{{"id", id}, {"baseRevision", "0"}, {"text", ""}});
    (void)call(service,
               "document.edit",
               Json{{"id", id}, {"baseRevision", "0"}, {"edits", Json::array()}});
    (void)call(service,
               "document.setSelections",
               Json{{"id", id}, {"baseRevision", "0"}, {"selections", before["selections"]}});
    CHECK(get_document(service, id) == before);
    CHECK(service.take_notifications().empty());
}

TEST_CASE("service bounds events and refuses mutations before committing when the queue is full") {
    EditorService service;
    for (std::size_t index = 0; index < EditorService::max_pending_notifications; ++index) {
        CHECK(create_document(service) == std::to_string(index + 1));
    }
    REQUIRE(service.document_count() == EditorService::max_pending_notifications);
    const auto before = get_document(service, "1");
    service_error(service, "document.create", Json::object(), -32004);
    service_error(service,
                  "document.insert",
                  Json{{"id", "1"}, {"baseRevision", "0"}, {"text", "x"}},
                  -32004);
    service_error(service, "document.close", Json{{"id", "1"}, {"baseRevision", "0"}}, -32004);
    CHECK(get_document(service, "1") == before);
    CHECK(service.document_count() == EditorService::max_pending_notifications);
    CHECK(service.take_notifications().size() == EditorService::max_pending_notifications);
    CHECK(create_document(service) == std::to_string(EditorService::max_pending_notifications + 1));
    const auto inserted =
        call(service, "document.insert", Json{{"id", "1"}, {"baseRevision", "0"}, {"text", "x"}});
    CHECK(inserted["revision"] == "1");
}

TEST_CASE("service validates bounded edit and selection collections atomically") {
    EditorService service;
    const auto id = create_document(service, "abc");
    const auto before = get_document(service, id);
    auto selections = Json::array();
    for (std::size_t index = 0; index <= EditorService::max_selections; ++index) {
        selections.push_back(Json{{"anchor", 0}, {"head", 0}});
    }
    service_error(service,
                  "document.setSelections",
                  Json{{"id", id}, {"baseRevision", "0"}, {"selections", selections}},
                  -32602);
    service_error(service,
                  "document.setSelections",
                  Json{{"id", id}, {"baseRevision", "0"}, {"selections", Json::array()}},
                  -32602);
    auto edits = Json::array();
    for (std::size_t index = 0; index <= EditorService::max_transaction_edits; ++index) {
        edits.push_back(Json{{"begin", 0}, {"end", 0}, {"text", ""}});
    }
    service_error(service,
                  "document.edit",
                  Json{{"id", id}, {"baseRevision", "0"}, {"edits", edits}},
                  -32602);
    CHECK(get_document(service, id) == before);
}

TEST_CASE("service in-process dispatch equals fragmented framed JSON dispatch") {
    constexpr std::uint32_t seed = 0x5E7A1CEU;
    CAPTURE(seed);
    std::mt19937 random(seed);
    EditorService local;
    EditorService remote;
    const std::vector<Json> script{
        service_request("document.create", Json{{"text", "abc\nxyz"}}),
        service_request("document.get", Json{{"id", "1"}}),
        service_request("document.setSelections",
                        Json{{"id", "1"},
                             {"baseRevision", "0"},
                             {"selections",
                              Json::array({Json{{"anchor", 1}, {"head", 1}},
                                           Json{{"anchor", 5}, {"head", 5}}})}}),
        service_request("document.insert", Json{{"id", "1"}, {"baseRevision", "0"}, {"text", "!"}}),
        service_request("document.undo", Json{{"id", "1"}, {"baseRevision", "1"}}),
        service_request("document.redo", Json{{"id", "1"}, {"baseRevision", "2"}}),
        service_request(
            "document.edit",
            Json{{"id", "1"},
                 {"baseRevision", "3"},
                 {"edits", Json::array({Json{{"begin", 0}, {"end", 1}, {"text", "A"}}})}}),
        service_request("document.get", Json{{"id", "1"}, {"begin", 1}, {"end", 6}}),
        service_request("document.insert",
                        Json{{"id", "1"}, {"baseRevision", "0"}, {"text", "stale"}}),
        service_request("document.close", Json{{"id", "1"}, {"baseRevision", "4"}}),
        service_request("document.close",
                        Json{{"id", "1"}, {"baseRevision", "4"}, {"discardChanges", true}}),
        service_request("document.get", Json{{"id", "1"}})};
    for (const auto& request : script) {
        CAPTURE(request.dump());
        CHECK(local.dispatch(request) == framed_dispatch(remote, request, random));
        CHECK(local.take_notifications() == remote.take_notifications());
        CHECK(local.document_count() == remote.document_count());
    }
    CHECK(local.document_count() == 0);
}

TEST_CASE("service batches execute in order and only respond to requests") {
    EditorService service;
    const auto id = create_document(service, "a");
    auto notification =
        service_request("document.insert", Json{{"id", id}, {"baseRevision", "0"}, {"text", "!"}});
    notification.erase("id");
    const auto batch =
        Json::array({notification,
                     service_request("document.get", Json{{"id", id}}, "read"),
                     service_request("document.insert",
                                     Json{{"id", id}, {"baseRevision", "0"}, {"text", "stale"}},
                                     "stale")});
    const auto response = service.dispatch(batch);
    REQUIRE(response);
    REQUIRE(response->size() == 2);
    CHECK((*response)[0]["id"] == "read");
    CHECK((*response)[0]["result"]["text"] == "!a");
    CHECK((*response)[1]["id"] == "stale");
    CHECK((*response)[1]["error"]["code"] == -32001);
    CHECK(get_document(service, id)["text"] == "!a");
}

TEST_CASE("service large paged-read batches remain within one frame including control-character "
          "escaping") {
    for (const auto character : {'x', '\0'}) {
        CAPTURE(static_cast<int>(character));
        EditorService service;
        const auto id =
            create_document(service, std::string(EditorService::max_read_bytes, character));
        (void)service.take_notifications();
        auto batch = Json::array();
        for (std::size_t index = 0; index < astra::protocol::Dispatcher::max_batch_size; ++index) {
            batch.push_back(service_request("document.get", Json{{"id", id}}, index));
        }
        const auto response = service.dispatch_text(batch.dump());
        REQUIRE(response);
        CHECK(response->size() <= astra::protocol::Dispatcher::max_body_bytes);
        CHECK_NOTHROW((void)(astra::protocol::encode_frame(*response)));
        const auto parsed = Json::parse(*response);
        REQUIRE(parsed.size() == batch.size());
        std::size_t successes = 0;
        std::size_t resyncs = 0;
        for (std::size_t index = 0; index < parsed.size(); ++index) {
            CHECK(parsed[index]["id"] == index);
            if (parsed[index].contains("result")) {
                ++successes;
                CHECK(parsed[index]["result"]["full"] == true);
                CHECK(parsed[index]["result"]["text"].get_ref<const std::string&>().size() ==
                      EditorService::max_read_bytes);
            } else {
                ++resyncs;
                CHECK(parsed[index]["error"]["code"] == -32005);
                CHECK(parsed[index]["error"]["data"]["handlerCompleted"] == true);
                CHECK(parsed[index]["error"]["data"]["resyncRequired"] == true);
            }
        }
        CHECK(successes > 0);
        CHECK(resyncs > 0);
        // Overflow is an ordinary protocol response. Subsequent requests and
        // complete single-page reads still work without reconnecting or editing.
        const auto current = get_document(service, id);
        CHECK(current["revision"] == "0");
        CHECK(current["text"] == std::string(EditorService::max_read_bytes, character));
        CHECK(service.take_notifications().empty());
    }
}

TEST_CASE("service metadata and versioned edits survive a preceding oversized read batch") {
    EditorService service;
    const auto id = create_document(service, std::string(EditorService::max_read_bytes, '\0'));
    (void)service.take_notifications();
    auto batch = Json::array();
    for (std::size_t index = 0; index < astra::protocol::Dispatcher::max_batch_size - 2; ++index) {
        batch.push_back(service_request("document.get", Json{{"id", id}}, index));
    }
    batch.push_back(service_request(
        "document.insert", Json{{"id", id}, {"baseRevision", "0"}, {"text", "changed"}}, "edit"));
    batch.push_back(
        service_request("document.get", Json{{"id", id}, {"begin", 0}, {"end", 7}}, "snapshot"));
    const auto response = service.dispatch_text(batch.dump());
    REQUIRE(response);
    CHECK(response->size() <= astra::protocol::Dispatcher::max_body_bytes);
    const auto parsed = Json::parse(*response);
    REQUIRE(parsed.size() == batch.size());
    const auto& edited = parsed[parsed.size() - 2];
    REQUIRE(edited.contains("result"));
    CHECK(edited["id"] == "edit");
    CHECK(edited["result"]["revision"] == "1");
    CHECK_FALSE(edited["result"].contains("text"));
    const auto& snapshot = parsed.back();
    REQUIRE(snapshot.contains("result"));
    CHECK(snapshot["result"]["text"] == "changed");
    CHECK(snapshot["result"]["revision"] == "1");
    const auto events = service.take_notifications();
    REQUIRE(events.size() == 1);
    CHECK(events[0]["params"]["revision"] == "1");
    service_error(service,
                  "document.insert",
                  Json{{"id", id}, {"baseRevision", "0"}, {"text", "changed"}},
                  -32001);
}
