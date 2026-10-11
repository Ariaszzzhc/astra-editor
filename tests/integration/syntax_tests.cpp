#include <astra/language/syntax.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <optional>
#include <random>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace astra::language;

[[nodiscard]] SyntaxRange bytes(std::size_t begin, std::size_t end) {
    return {{begin}, {end}};
}

[[nodiscard]] astra::Edit edit(std::size_t begin, std::size_t end, std::string text) {
    return {{begin}, {end}, std::move(text)};
}

[[nodiscard]] astra::Transaction transaction(const astra::Document& document,
                                             std::vector<astra::Edit> edits) {
    return {document.revision(), std::move(edits), std::nullopt};
}

[[nodiscard]] ParseOptions relaxed() {
    ParseOptions options;
    options.timeout = std::chrono::seconds(10);
    return options;
}

[[nodiscard]] std::vector<HighlightSpan> all_highlights(const SyntaxSnapshot& snapshot) {
    return snapshot.highlights().intersecting(bytes(0, snapshot.text().size_bytes()));
}

[[nodiscard]] std::optional<HighlightKind> at(const SyntaxSnapshot& snapshot, std::size_t offset) {
    const auto spans = snapshot.highlights().intersecting(bytes(offset, offset + 1));
    return spans.empty() ? std::nullopt : std::optional(spans.front().kind);
}

[[nodiscard]] bool named(const SyntaxSnapshot& snapshot, std::string_view name, OutlineKind kind) {
    return std::ranges::any_of(snapshot.outline(), [&](const auto& item) {
        return item.name == name && item.kind == kind;
    });
}

void check_same_analysis(const SyntaxSnapshot& incremental, const SyntaxSnapshot& full) {
    CHECK(incremental.text().str() == full.text().str());
    CHECK(incremental.has_errors() == full.has_errors());
    const auto actual = all_highlights(incremental), expected = all_highlights(full);
    if (actual != expected) {
        std::size_t index{};
        while (index < std::min(actual.size(), expected.size()) && actual[index] == expected[index])
            ++index;
        const auto describe = [](const auto& spans, std::size_t item) {
            return item < spans.size()
                       ? std::to_string(spans[item].range.begin.value) + ".." +
                             std::to_string(spans[item].range.end.value) + " kind " +
                             std::to_string(static_cast<int>(spans[item].kind))
                       : std::string("end");
        };
        INFO("actual ", describe(actual, index));
        INFO("expected ", describe(expected, index));
        const auto point = index < actual.size() ? actual[index].range.begin.value : 0;
        auto source_begin = point > 50 ? point - 50 : 0,
             source_end = std::min(point + 100, incremental.text().size_bytes());
        while (!incremental.text().is_boundary(source_begin))
            --source_begin;
        while (!incremental.text().is_boundary(source_end))
            --source_end;
        INFO(incremental.text().slice(source_begin, source_end));
        CHECK(actual == expected);
    } else
        CHECK(actual == expected);
    CHECK(incremental.outline() == full.outline());
    CHECK(incremental.text_objects() == full.text_objects());
    CHECK(incremental.highlights().validate());
}

} // namespace

TEST_CASE("syntax extension selection uses explicit supported grammars") {
    CHECK(language_for_path("src/a.c") == LanguageId::c);
    CHECK(language_for_path("src/A.CPP") == LanguageId::cpp);
    CHECK(language_for_path("src/a.h") == LanguageId::cpp);
    CHECK(language_for_path("a.mjs") == LanguageId::javascript);
    CHECK(language_for_path("a.ts") == LanguageId::typescript);
    CHECK(language_for_path("a.tsx") == LanguageId::tsx);
    CHECK(language_for_path("a.json") == LanguageId::json);
    CHECK(language_for_path("a.pyi") == LanguageId::python);
    CHECK(language_for_path("a.md") == LanguageId::markdown);
    CHECK_FALSE(language_for_path("README"));
    CHECK_FALSE(language_for_path("a.rs"));
    CHECK_FALSE(language_for_path("a.cpp/README"));
    CHECK(language_name(LanguageId::cpp) == "cpp");
    CHECK_THROWS_AS((void)language_name(static_cast<LanguageId>(999)), std::invalid_argument);
    CHECK_THROWS_AS((void)SyntaxParser(static_cast<LanguageId>(999)), std::invalid_argument);
}

TEST_CASE("every bundled grammar parses actual language syntax") {
    const std::array cases{
        std::pair(LanguageId::c, "int main(void) { return 0; }\n"),
        std::pair(LanguageId::cpp,
                  "namespace app { class Box { public: int value() const { return 1; } }; }\n"),
        std::pair(LanguageId::javascript, "export function sum(a, b) { return a + b; }\n"),
        std::pair(LanguageId::typescript,
                  "interface Item { name: string; }\nconst item: Item = { name: 'x' };\n"),
        std::pair(LanguageId::tsx, "const App = () => <div title='x'>hello</div>;\n"),
        std::pair(LanguageId::json, "{\"items\": [1, true, null, \"text\"]}\n"),
        std::pair(LanguageId::python, "class Box:\n    def value(self):\n        return 1\n"),
        std::pair(LanguageId::markdown,
                  "# Title\n\nA `code` span and [link](https://example.com).\n")};
    for (const auto& [language, source] : cases) {
        INFO(language_name(language));
        astra::Document document({1}, source);
        SyntaxParser parser(language);
        const auto result = parser.parse(document.snapshot(), relaxed());
        CHECK_FALSE(result.has_errors());
        CHECK_FALSE(result.incremental());
        CHECK(result.matches(document.snapshot()));
        CHECK(result.highlights().size_bytes() == document.text().size_bytes());
        CHECK(result.highlights().validate());
        CHECK_FALSE(all_highlights(result).empty());
    }
}

TEST_CASE("syntax snapshots tag exact buffer revision and content identity") {
    astra::Document document({21}, "{}"), foreign({22}, "{}");
    SyntaxParser parser(LanguageId::json);
    const auto syntax = parser.parse(document.snapshot());
    CHECK(syntax.id() == document.id());
    CHECK(syntax.revision() == document.revision());
    CHECK(syntax.content_identity() == document.content_identity());
    CHECK(syntax.matches(document.snapshot()));
    CHECK_FALSE(syntax.matches(foreign.snapshot()));
    REQUIRE(document.apply(transaction(document, {edit(1, 1, "\"v\": 1")})));
    CHECK_FALSE(syntax.matches(document.snapshot()));
    REQUIRE(document.undo());
    CHECK_FALSE(syntax.matches(document.snapshot()));
    CHECK(syntax.text().str() == "{}");
}

TEST_CASE("JSON syntactic colors distinguish properties strings numbers and literals") {
    const std::string source = "{\"name\": \"\xe6\x98\x9f\", \"value\": 42, \"ok\": true}";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::json);
    const auto syntax = parser.parse(document.snapshot());
    CHECK(at(syntax, source.find("name")) == HighlightKind::property);
    CHECK(at(syntax, source.find("\xe6\x98\x9f")) == HighlightKind::string);
    CHECK(at(syntax, source.find("42")) == HighlightKind::number);
    CHECK(at(syntax, source.find("true")) == HighlightKind::keyword);
    CHECK(at(syntax, 0) == HighlightKind::punctuation);
    CHECK_FALSE(at(syntax, source.find(' ')));
    CHECK(named(syntax, "\"name\"", OutlineKind::property));
}

TEST_CASE("C and C++ outline preserves declaration hierarchy and function names") {
    astra::Document document(
        {1}, "namespace app { struct Box { int get() { return 1; } }; int run() { return 0; } }");
    SyntaxParser parser(LanguageId::cpp);
    const auto syntax = parser.parse(document.snapshot(), relaxed());
    CHECK_FALSE(syntax.has_errors());
    REQUIRE(syntax.outline().size() == 4);
    CHECK(syntax.outline()[0].name == "app");
    CHECK(syntax.outline()[0].depth == 0);
    CHECK(syntax.outline()[1].name == "Box");
    CHECK(syntax.outline()[1].depth == 1);
    CHECK(syntax.outline()[2].name == "get");
    CHECK(syntax.outline()[2].depth == 2);
    CHECK(syntax.outline()[3].name == "run");
    CHECK(syntax.outline()[3].depth == 1);
    CHECK(syntax.outline()[2].kind == OutlineKind::function);
}

TEST_CASE("Python and TypeScript declarations form real syntax outline entries") {
    SUBCASE("Python") {
        astra::Document document({1}, "class Box:\n    def value(self):\n        return 1\n");
        SyntaxParser parser(LanguageId::python);
        const auto syntax = parser.parse(document.snapshot());
        REQUIRE(syntax.outline().size() == 2);
        CHECK(syntax.outline()[0].name == "Box");
        CHECK(syntax.outline()[1].name == "value");
        CHECK(syntax.outline()[1].depth == 1);
    }
    SUBCASE("TypeScript") {
        astra::Document document({1},
                                 "interface Item { name: string; }\ntype Key = string;\nfunction "
                                 "read(): Key { return 'x'; }\n");
        SyntaxParser parser(LanguageId::typescript);
        const auto syntax = parser.parse(document.snapshot());
        CHECK(named(syntax, "Item", OutlineKind::class_));
        CHECK(named(syntax, "Key", OutlineKind::type));
        CHECK(named(syntax, "read", OutlineKind::function));
    }
}

TEST_CASE("text objects choose enclosing function body and nested block") {
    const std::string source = "function go(x) { if (x) { return x; } }";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::javascript);
    const auto syntax = parser.parse(document.snapshot());
    const auto name = source.find("go");
    const auto outer = syntax.enclosing_object(bytes(name, name), TextObjectKind::function);
    REQUIRE(outer);
    CHECK(*outer == bytes(0, source.size()));
    const auto inner = syntax.enclosing_object(bytes(name, name), TextObjectKind::function, true);
    REQUIRE(inner);
    CHECK(syntax.text().slice(inner->begin.value, inner->end.value) == " if (x) { return x; } ");
    const auto position = source.find("return");
    const auto block = syntax.enclosing_object(bytes(position, position), TextObjectKind::block);
    REQUIRE(block);
    CHECK(syntax.text().slice(block->begin.value, block->end.value) == "{ return x; }");
    const auto bigger = syntax.enclosing_object(*block, TextObjectKind::block, false, true);
    REQUIRE(bigger);
    CHECK(bigger->begin < block->begin);
    CHECK(bigger->end > block->end);
    CHECK_FALSE(syntax.enclosing_object(*outer, TextObjectKind::function, false, true));
}

TEST_CASE("string comment and parameter text objects retain exact UTF-8 byte boundaries") {
    const std::string source = "// hello\nfunction go(name) { return '\xf0\x9f\x98\x80'; }";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::javascript);
    const auto syntax = parser.parse(document.snapshot());
    const auto comment = syntax.enclosing_object(bytes(3, 3), TextObjectKind::comment, true);
    REQUIRE(comment);
    CHECK(syntax.text().slice(comment->begin.value, comment->end.value) == "hello");
    const auto emoji = source.find("\xf0\x9f\x98\x80");
    const auto string = syntax.enclosing_object(bytes(emoji, emoji), TextObjectKind::string, true);
    REQUIRE(string);
    CHECK(*string == bytes(emoji, emoji + 4));
    const auto name = source.find("name");
    const auto parameter = syntax.enclosing_object(bytes(name, name), TextObjectKind::parameter);
    REQUIRE(parameter);
    CHECK(*parameter == bytes(name, name + 4));
    CHECK_THROWS_AS(
        (void)syntax.enclosing_object(bytes(emoji + 1, emoji + 1), TextObjectKind::string),
        std::invalid_argument);
}

TEST_CASE("Markdown uses block headings and genuine inline code parsing") {
    const std::string source = "# Title\n\nA `code` span.\n\n## Detail\n\n```cpp\nint x;\n```\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::markdown);
    const auto syntax = parser.parse(document.snapshot(), relaxed());
    CHECK_FALSE(syntax.has_errors());
    REQUIRE(syntax.outline().size() == 2);
    CHECK(syntax.outline()[0].name == "Title");
    CHECK(syntax.outline()[0].depth == 0);
    CHECK(syntax.outline()[1].name == "Detail");
    CHECK(syntax.outline()[1].depth == 1);
    CHECK(at(syntax, source.find("Title")) == HighlightKind::heading);
    CHECK(at(syntax, source.find("code")) == HighlightKind::string);
    const auto offset = source.find("code");
    const auto object =
        syntax.enclosing_object(bytes(offset, offset), TextObjectKind::string, true);
    REQUIRE(object);
    CHECK(syntax.text().slice(object->begin.value, object->end.value) == "code");
}

TEST_CASE("syntax errors remain inspectable and later tasks recover") {
    SyntaxParser parser(LanguageId::json);
    astra::Document broken({1}, "{\"x\": [1,, }"), valid({2}, "{\"x\": 1}");
    const auto failed_syntax = parser.parse(broken.snapshot());
    CHECK(failed_syntax.has_errors());
    CHECK(failed_syntax.highlights().validate());
    const auto recovered = parser.parse(valid.snapshot());
    CHECK_FALSE(recovered.has_errors());
    CHECK(recovered.matches(valid.snapshot()));
}

TEST_CASE("Markdown inline delimiters cannot span separate paragraphs") {
    const std::string source = "An `open delimiter.\n\nA close` delimiter.\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::markdown);
    const auto syntax = parser.parse(document.snapshot());
    const auto inside = source.find("open");
    CHECK(at(syntax, inside) != HighlightKind::string);
    CHECK_FALSE(syntax.enclosing_object(bytes(inside, inside), TextObjectKind::string));
}

TEST_CASE("incremental syntax applies original-coordinate multicursor edits atomically") {
    astra::Document document({1}, "{\"a\":1,\"b\":2,\"c\":3}");
    SyntaxParser parser(LanguageId::json);
    const auto old = parser.parse(document.snapshot());
    const auto before_text = old.text().str();
    auto edits = transaction(document, {edit(17, 18, "300"), edit(5, 6, "10")});
    REQUIRE(document.apply(edits));
    const auto updated =
        parser.update(old, edits, document.revision(), document.content_identity());
    const auto full = parser.parse(document.snapshot());
    CHECK(updated.incremental());
    CHECK(updated.matches(document.snapshot()));
    CHECK_FALSE(updated.changed_ranges().empty());
    CHECK(old.text().str() == before_text);
    CHECK_FALSE(old.incremental());
    check_same_analysis(updated, full);
}

TEST_CASE("incremental syntax handles Unicode byte columns CRLF and multiline replacements") {
    const std::string source =
        "{\r\n  \"\xe5\x90\x8d\xe5\xad\x97\": \"\xf0\x9f\x98\x80\",\r\n  \"v\": 1\r\n}";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::json);
    const auto old = parser.parse(document.snapshot());
    const auto emoji = source.find("\xf0\x9f\x98\x80");
    const auto digit = source.find('1');
    auto edits = transaction(
        document,
        {edit(emoji, emoji + 4, "\xe6\x98\x9f"), edit(digit, digit + 1, "[\r\n    2, 3\r\n  ]")});
    REQUIRE(document.apply(edits));
    const auto updated =
        parser.update(old, edits, document.revision(), document.content_identity());
    check_same_analysis(updated, parser.parse(document.snapshot()));
    CHECK_FALSE(updated.has_errors());
}

TEST_CASE("incremental rename invalidates changed bytes even when tree shape is unchanged") {
    astra::Document document({1}, "function old() { return 1; }");
    SyntaxParser parser(LanguageId::javascript);
    const auto old = parser.parse(document.snapshot());
    auto edits = transaction(document, {edit(9, 12, "newName")});
    REQUIRE(document.apply(edits));
    const auto updated =
        parser.update(old, edits, document.revision(), document.content_identity());
    CHECK(named(updated, "newName", OutlineKind::function));
    CHECK(std::ranges::any_of(updated.changed_ranges(), [](auto changed) {
        return changed.begin.value <= 9 && changed.end.value >= 16;
    }));
    CHECK(named(old, "old", OutlineKind::function));
}

TEST_CASE("incremental syntax rejects stale invalid and overlapping edit batches") {
    astra::Document document({1}, "{\"x\":1}");
    SyntaxParser parser(LanguageId::json);
    const auto old = parser.parse(document.snapshot());
    SUBCASE("stale") {
        CHECK_THROWS_AS((void)parser.update(old, {{99}, {}, {}}, {1}, {1}), astra::StaleRevision);
    }
    SUBCASE("revision must advance") {
        CHECK_THROWS_AS((void)parser.update(old, transaction(document, {}), {0}, {1}),
                        std::invalid_argument);
    }
    SUBCASE("out of bounds") {
        CHECK_THROWS_AS(
            (void)parser.update(old, transaction(document, {edit(0, 80, "")}), {1}, {1}),
            std::invalid_argument);
    }
    SUBCASE("overlap") {
        CHECK_THROWS_AS(
            (void)parser.update(
                old, transaction(document, {edit(1, 3, "a"), edit(2, 4, "b")}), {1}, {1}),
            std::invalid_argument);
    }
    SUBCASE("ambiguous adjacent insertion") {
        CHECK_THROWS_AS(
            (void)parser.update(
                old, transaction(document, {edit(1, 3, "a"), edit(3, 3, "b")}), {1}, {1}),
            std::invalid_argument);
    }
    SUBCASE("invalid UTF-8") {
        CHECK_THROWS_AS(
            (void)parser.update(old, transaction(document, {edit(1, 1, "\xc0\xaf")}), {1}, {1}),
            std::invalid_argument);
    }
    SUBCASE("different language") {
        SyntaxParser other(LanguageId::javascript);
        CHECK_THROWS_AS((void)other.update(old, transaction(document, {}), {1}, {1}),
                        std::invalid_argument);
    }
    CHECK(old.text().str() == "{\"x\":1}");
    CHECK(old.matches(document.snapshot()));
    CHECK_FALSE(parser.parse(document.snapshot()).has_errors());
}

TEST_CASE("syntax cancellation and resource failures preserve previous snapshot") {
    astra::Document document({1}, "{\"x\": [1, 2, 3], \"y\": \"text\"}");
    SyntaxParser parser(LanguageId::json);
    const auto old = parser.parse(document.snapshot());
    auto options = relaxed();
    SUBCASE("cancelled") {
        std::stop_source cancel;
        (void)cancel.request_stop();
        options.stop = cancel.get_token();
        CHECK_THROWS_AS((void)parser.parse(document.snapshot(), options), ParseCancelled);
    }
    SUBCASE("bytes") {
        options.max_bytes = 3;
        CHECK_THROWS_AS((void)parser.parse(document.snapshot(), options), std::length_error);
    }
    SUBCASE("nodes") {
        options.max_nodes = 1;
        CHECK_THROWS_AS((void)parser.parse(document.snapshot(), options), std::length_error);
    }
    SUBCASE("spans") {
        options.max_spans = 1;
        CHECK_THROWS_AS((void)parser.parse(document.snapshot(), options), std::length_error);
    }
    SUBCASE("outline") {
        options.max_outline_items = 0;
        CHECK_THROWS_AS((void)parser.parse(document.snapshot(), options), std::length_error);
    }
    SUBCASE("text objects") {
        options.max_text_objects = 0;
        CHECK_THROWS_AS((void)parser.parse(document.snapshot(), options), std::length_error);
    }
    SUBCASE("invalid deadline") {
        options.timeout = std::chrono::milliseconds::zero();
        CHECK_THROWS_AS((void)parser.parse(document.snapshot(), options), std::invalid_argument);
    }
    CHECK(old.matches(document.snapshot()));
    check_same_analysis(old, parser.parse(document.snapshot()));
}

TEST_CASE("cancelled incremental parse keeps old result usable by another parser") {
    astra::Document document({1}, "{\"x\":1}");
    SyntaxParser parser(LanguageId::json);
    const auto old = parser.parse(document.snapshot());
    auto edits = transaction(document, {edit(5, 6, "20")});
    REQUIRE(document.apply(edits));
    std::stop_source cancel;
    (void)cancel.request_stop();
    auto options = relaxed();
    options.stop = cancel.get_token();
    CHECK_THROWS_AS(
        (void)parser.update(old, edits, document.revision(), document.content_identity(), options),
        ParseCancelled);
    SyntaxParser recovered(LanguageId::json);
    const auto updated =
        recovered.update(old, edits, document.revision(), document.content_identity());
    check_same_analysis(updated, recovered.parse(document.snapshot()));
    CHECK(old.text().str() == "{\"x\":1}");
}

TEST_CASE("highlight index seeks clipped viewport ranges and retains old copies") {
    HighlightIndex index({{3, std::nullopt},
                          {4, HighlightKind::string},
                          {2, std::nullopt},
                          {5, HighlightKind::number}});
    const auto old = index;
    CHECK(index.size_bytes() == 14);
    CHECK(index.run_count() == 4);
    CHECK(index.validate());
    const std::vector expected{HighlightSpan{bytes(5, 7), HighlightKind::string},
                               HighlightSpan{bytes(9, 11), HighlightKind::number}};
    CHECK(index.intersecting(bytes(5, 11)) == expected);
    CHECK(index.intersecting(bytes(14, 14)).empty());
    CHECK_THROWS_AS((void)index.intersecting(bytes(0, 15)), std::out_of_range);
    CHECK_THROWS_AS((void)index.intersecting(bytes(4, 2)), std::out_of_range);
    CHECK_THROWS_AS((void)HighlightIndex({{0, HighlightKind::string}}), std::invalid_argument);
    index = HighlightIndex{};
    CHECK(old.intersecting(bytes(5, 11)) == expected);
}

TEST_CASE("empty documents and parser moves have defined behavior") {
    astra::Document document({1});
    SyntaxParser first(LanguageId::javascript);
    SyntaxParser parser(std::move(first));
    CHECK_THROWS_AS((void)first.parse(document.snapshot()), std::logic_error);
    const auto empty = parser.parse(document.snapshot());
    CHECK(empty.highlights().size_bytes() == 0);
    CHECK(all_highlights(empty).empty());
    CHECK(empty.outline().empty());
    CHECK(empty.changed_ranges().empty());
    CHECK_FALSE(empty.has_errors());
}

TEST_CASE("Rope callback handles supplementary UTF-8 scalars at read chunk boundaries") {
    std::string source = "{\"text\":\"";
    source.append(4085, 'a');
    source += "\xf0\x9f\x98\x80\xe6\x98\x9f\"}";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::json);
    const auto syntax = parser.parse(document.snapshot(), relaxed());
    CHECK_FALSE(syntax.has_errors());
    CHECK(at(syntax, source.find("\xf0\x9f\x98\x80")) == HighlightKind::string);
    CHECK(syntax.highlights().size_bytes() == source.size());
}

TEST_CASE("Markdown incremental inline edits equal a fresh two-stage parse") {
    astra::Document document({1}, "# Title\n\nFirst `one`.\n\nSecond `two`.\n");
    SyntaxParser parser(LanguageId::markdown);
    const auto old = parser.parse(document.snapshot());
    const auto source = document.text().str();
    auto edits =
        transaction(document,
                    {edit(source.find("one"), source.find("one") + 3, "new"),
                     edit(source.find("Second"), source.find("Second") + 6, "**Second**")});
    REQUIRE(document.apply(edits));
    const auto updated =
        parser.update(old, edits, document.revision(), document.content_identity());
    check_same_analysis(updated, parser.parse(document.snapshot()));
}

TEST_CASE("independent workers can derive new syntax from one immutable snapshot") {
    astra::Document document({1}, "{\"x\":1}");
    SyntaxParser parser(LanguageId::json);
    const auto old = parser.parse(document.snapshot());
    auto first = std::async(std::launch::async, [old] {
        SyntaxParser worker(LanguageId::json);
        return worker.update(old, {old.revision(), {edit(5, 6, "2")}, {}}, {1}, {1}, relaxed());
    });
    auto second = std::async(std::launch::async, [old] {
        SyntaxParser worker(LanguageId::json);
        return worker.update(old, {old.revision(), {edit(5, 6, "3")}, {}}, {2}, {2}, relaxed());
    });
    CHECK(first.get().text().str() == "{\"x\":2}");
    CHECK(second.get().text().str() == "{\"x\":3}");
    CHECK(old.text().str() == "{\"x\":1}");
}

TEST_CASE("incremental semantic indexes reuse large declaration interiors and byte runs") {
    std::string source;
    for (std::size_t i = 0; i < 400; ++i)
        source += "int function" + std::to_string(i) + "(int value) { return value + " +
                  std::to_string(i) + "; } // 中🙂\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::cpp), reference(LanguageId::cpp);
    auto options = relaxed(), full_options = options;
    full_options.incremental_analysis = false;
    const auto old = parser.parse(document.snapshot(), options);
    check_same_analysis(old, reference.parse(document.snapshot(), full_options));
    const auto begin = source.find("+ 200") + 2;
    auto edits = transaction(document, {edit(begin, begin + 3, "999")});
    REQUIRE(document.apply(edits));
    const auto updated =
        parser.update(old, edits, document.revision(), document.content_identity(), options);
    const auto full = reference.parse(document.snapshot(), full_options);
    check_same_analysis(updated, full);
    const auto stats = updated.analysis_statistics();
    CHECK(stats.reused_units > 300);
    CHECK(stats.reused_bytes > source.size() * 3 / 4);
    CHECK(stats.visited_nodes < full.analysis_statistics().visited_nodes / 10);
    CHECK(updated.highlights().shared_prefix_bytes(old.highlights()) > source.size() / 3);
    CHECK(updated.highlights().shared_suffix_bytes(old.highlights()) > source.size() / 3);
    REQUIRE(updated.outline_size() == 400);
    const auto page = updated.outline_range(390, 400);
    CHECK(page == std::vector<OutlineItem>(full.outline().begin() + 390, full.outline().end()));
    CHECK_THROWS_AS((void)updated.outline_range(400, 401), std::out_of_range);
    CHECK(updated.outline_range(400, 400).empty());
    for (std::size_t i = 0; i < 400; ++i) {
        const auto caret = document.text().str().find("function" + std::to_string(i) + "(");
        CHECK(updated.enclosing_object(bytes(caret, caret), TextObjectKind::function) ==
              full.enclosing_object(bytes(caret, caret), TextObjectKind::function));
    }
    CHECK(old.text().str() == source);
    CHECK(old.outline().at(200).name == "function200");
    SUBCASE("cached units still honor stricter node limits") {
        options.max_nodes = 1;
        CHECK_THROWS_AS((void)parser.update(updated,
                                            {updated.revision(), {}, std::nullopt},
                                            {updated.revision().value + 1},
                                            document.content_identity(),
                                            options),
                        std::length_error);
    }
    SUBCASE("cached units still honor stricter span limits") {
        options.max_spans = 1;
        CHECK_THROWS_AS((void)parser.update(updated,
                                            {updated.revision(), {}, std::nullopt},
                                            {updated.revision().value + 1},
                                            document.content_identity(),
                                            options),
                        std::length_error);
    }
}

TEST_CASE("incremental semantic partitions inside a large function equal full traversal") {
    constexpr unsigned int seed = 0xA57A5E;
    INFO(seed);
    std::mt19937 random(seed);
    std::string source = "namespace app { struct Box { int compute(int value) {\n";
    for (int i = 0; i < 180; ++i)
        source += "{ int local" + std::to_string(i) + " = value + " + std::to_string(i) +
                  "; value += local" + std::to_string(i) + "; } // 中🙂\n";
    source += "return value; } }; }\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::cpp), reference(LanguageId::cpp);
    auto options = relaxed(), full_options = options;
    full_options.incremental_analysis = false;
    auto syntax = parser.parse(document.snapshot(), options);
    check_same_analysis(syntax, reference.parse(document.snapshot(), full_options));
    std::vector<SyntaxSnapshot> retained;
    for (int iteration = 0; iteration < 40; ++iteration) {
        INFO(iteration);
        const auto text = document.text().str(),
                   needle = "local" + std::to_string(random() % 180) + " = value + ";
        const auto begin = text.find(needle) + needle.size(), end = text.find(';', begin);
        auto edits = transaction(document, {edit(begin, end, std::to_string(random() % 100000))});
        if (!document.apply(edits))
            continue;
        if (iteration % 10 == 0)
            retained.push_back(syntax);
        syntax =
            parser.update(syntax, edits, document.revision(), document.content_identity(), options);
        const auto full = reference.parse(document.snapshot(), full_options);
        check_same_analysis(syntax, full);
        CHECK(syntax.analysis_statistics().reused_bytes > source.size() / 2);
        CHECK(syntax.analysis_statistics().visited_nodes <
              full.analysis_statistics().visited_nodes / 10);
        const auto caret = begin;
        for (const auto kind :
             {TextObjectKind::function, TextObjectKind::class_, TextObjectKind::block})
            CHECK(syntax.enclosing_object(bytes(caret, caret), kind) ==
                  full.enclosing_object(bytes(caret, caret), kind));
    }
    for (const auto& snapshot : retained) {
        CHECK(snapshot.highlights().validate());
        CHECK(snapshot.outline().size() == 3);
        CHECK_FALSE(snapshot.matches(document.snapshot()));
    }
}

TEST_CASE(
    "incremental semantic reuse matches independent full analysis across grammars and errors") {
    constexpr unsigned int seed = 0xA57A6A;
    INFO(seed);
    std::mt19937 random(seed);
    for (const auto language : {LanguageId::c,
                                LanguageId::cpp,
                                LanguageId::javascript,
                                LanguageId::typescript,
                                LanguageId::tsx,
                                LanguageId::json,
                                LanguageId::python}) {
        INFO(language_name(language));
        std::string source;
        for (int i = 0; i < 60; ++i) {
            const auto suffix = std::to_string(i);
            if (language == LanguageId::json)
                source += (i ? ",\n" : "[\n") + std::string("{\"name\":\"中🙂\",\"value\":") +
                          suffix + "}";
            else if (language == LanguageId::python)
                source += "def function" + suffix +
                          "(value):\n    text = '中🙂'\n    return value + " + suffix + "\n\n";
            else if (language == LanguageId::c || language == LanguageId::cpp)
                source += "int function" + suffix + "(int value) { return value + " + suffix +
                          "; } // 中🙂\n";
            else if (language == LanguageId::tsx)
                source +=
                    "const View" + suffix + " = () => <div data-id={" + suffix + "}>中🙂</div>;\n";
            else if (language == LanguageId::typescript)
                source += "function function" + suffix +
                          "(value: number): number { const text = '中🙂'; return value + " +
                          suffix + "; }\n";
            else
                source +=
                    "function function" + suffix +
                    "(value) { const object = {name: '中🙂'}; return object.name + value; }\n";
        }
        if (language == LanguageId::json)
            source += "\n]";
        astra::Document document({1}, source);
        SyntaxParser parser(language), reference(language);
        auto options = relaxed(), full_options = options;
        full_options.incremental_analysis = false;
        auto syntax = parser.parse(document.snapshot(), options);
        check_same_analysis(syntax, reference.parse(document.snapshot(), full_options));
        for (int iteration = 0; iteration < 24; ++iteration) {
            INFO(iteration);
            const auto& rope = document.text();
            auto begin = static_cast<std::size_t>(random() % (rope.size_bytes() + 1));
            while (!rope.is_boundary(begin))
                --begin;
            auto end = std::min(rope.size_bytes(), begin + random() % 7);
            while (!rope.is_boundary(end))
                ++end;
            const std::array<std::string, 6> replacements{"中🙂", "'", "}\n", "/*", "", "new_name"};
            const auto previous_full = reference.parse(document.snapshot(), full_options);
            const auto previous_semantic = syntax;
            auto edits = transaction(
                document, {edit(begin, end, replacements[random() % replacements.size()])});
            if (!document.apply(edits))
                continue;
            syntax = parser.update(
                syntax, edits, document.revision(), document.content_identity(), options);
            const auto full = reference.parse(document.snapshot(), full_options);
            {
                INFO("actual incremental tree full traversal");
                check_same_analysis(reference.update(previous_semantic,
                                                     edits,
                                                     document.revision(),
                                                     document.content_identity(),
                                                     full_options),
                                    full);
            }
            {
                INFO("incremental full traversal");
                check_same_analysis(reference.update(previous_full,
                                                     edits,
                                                     document.revision(),
                                                     document.content_identity(),
                                                     full_options),
                                    full);
            }
            {
                INFO("fresh partition");
                check_same_analysis(reference.parse(document.snapshot(), options), full);
            }
            {
                INFO("incremental reuse");
                check_same_analysis(syntax, full);
            }
            for (const auto kind : {TextObjectKind::function,
                                    TextObjectKind::class_,
                                    TextObjectKind::string,
                                    TextObjectKind::comment,
                                    TextObjectKind::block})
                CHECK(syntax.enclosing_object(bytes(begin, begin), kind) ==
                      full.enclosing_object(bytes(begin, begin), kind));
        }
    }
}

TEST_CASE("Markdown semantic units retain unchanged inline parses and measured ranges") {
    std::string source;
    for (std::size_t i = 0; i < 400; ++i)
        source += "# Section " + std::to_string(i) + "\n\nParagraph `code" + std::to_string(i) +
                  "` with **bold**, [link](https://example.com), 中🙂 and é.\n\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::markdown), reference(LanguageId::markdown);
    auto options = relaxed(), full_options = options;
    full_options.incremental_analysis = false;
    const auto old = parser.parse(document.snapshot(), options);
    const auto original = reference.parse(document.snapshot(), full_options);
    check_same_analysis(old, original);
    const auto begin = source.find("code200") + 4;
    auto edits = transaction(document, {edit(begin, begin + 3, "999")});
    REQUIRE(document.apply(edits));
    const auto updated =
        parser.update(old, edits, document.revision(), document.content_identity(), options);
    const auto full = reference.parse(document.snapshot(), full_options);
    check_same_analysis(updated, full);
    const auto stats = updated.analysis_statistics();
    INFO("visited ",
         stats.visited_nodes,
         ", inline parses ",
         stats.inline_parses,
         ", reused units ",
         stats.reused_units,
         ", reused bytes ",
         stats.reused_bytes);
    MESSAGE("Markdown semantic visits=",
            stats.visited_nodes,
            ", context visits=",
            stats.context_nodes,
            ", inline parses=",
            stats.inline_parses,
            ", reused units=",
            stats.reused_units,
            ", reused bytes=",
            stats.reused_bytes,
            ", full semantic visits=",
            full.analysis_statistics().visited_nodes);
    CHECK(stats.reused_units > 300);
    CHECK(stats.reused_bytes > source.size() * 3 / 4);
    CHECK(stats.visited_nodes < full.analysis_statistics().visited_nodes / 4);
    CHECK(stats.context_nodes > 0);
    CHECK(stats.visited_nodes + stats.context_nodes < full.analysis_statistics().visited_nodes / 2);
    CHECK(stats.inline_parses <= 4);
    CHECK(full.analysis_statistics().inline_parses == 800);
    CHECK(updated.highlights().shared_prefix_bytes(old.highlights()) > source.size() / 3);
    CHECK(updated.highlights().shared_suffix_bytes(old.highlights()) > source.size() / 3);
    REQUIRE(updated.outline_size() == 400);
    CHECK(updated.outline_range(390, 400) ==
          std::vector<OutlineItem>(full.outline().begin() + 390, full.outline().end()));
    for (const auto caret :
         {source.find("Section 1\n"), source.find("code200"), source.find("code399")})
        for (const auto kind : {TextObjectKind::heading, TextObjectKind::string})
            CHECK(updated.enclosing_object(bytes(caret, caret), kind) ==
                  full.enclosing_object(bytes(caret, caret), kind));
    check_same_analysis(old, original);
}

TEST_CASE("Markdown partitions preserve independent inline contexts in large blocks") {
    std::string source = "# Title\n\n> A long `quoted\n";
    for (int i = 0; i < 100; ++i)
        source += "> continuation 中🙂 " + std::to_string(i) + "\n";
    source += "> code` and **bold**.\n\n";
    for (int i = 0; i < 40; ++i)
        source += "| `cell" + std::to_string(i) + " | cell` |\n";
    source += "\n- **A list\n  continuation** with [link](target).\n\n";
    source += "Unclosed `delimiter in one paragraph.\n\nA closing delimiter` in another.\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::markdown), reference(LanguageId::markdown);
    auto options = relaxed(), full_options = options;
    full_options.incremental_analysis = false;
    auto syntax = parser.parse(document.snapshot(), options);
    check_same_analysis(syntax, reference.parse(document.snapshot(), full_options));
    for (const auto& replacement : {std::string("中🙂\n> new `span`"),
                                    std::string("\n\n"),
                                    std::string("**"),
                                    std::string("# Heading\n\n")}) {
        const auto previous = syntax;
        const auto text = document.text().str();
        const auto begin = text.find("continuation");
        auto edits = transaction(document, {edit(begin, begin + 12, replacement)});
        REQUIRE(document.apply(edits));
        syntax =
            parser.update(syntax, edits, document.revision(), document.content_identity(), options);
        const auto full = reference.parse(document.snapshot(), full_options);
        check_same_analysis(syntax, full);
        check_same_analysis(reference.parse(document.snapshot(), options), full);
        CHECK(previous.highlights().validate());
    }
}

TEST_CASE("Markdown incremental semantics match independent traversal after Unicode edits") {
    constexpr unsigned int seed = 0xA57A4D;
    INFO(seed);
    std::mt19937 random(seed);
    std::string source;
    for (int i = 0; i < 40; ++i)
        source += "## Heading " + std::to_string(i) +
                  "\n\nA `code` and **bold 中🙂** with é and [link](target).\n\n"
                  "> quoted *emphasis*\n> continued 中🙂\n\n"
                  "| left `one` | right *two* |\n| --- | --- |\n| data | cell |\n\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::markdown), reference(LanguageId::markdown);
    auto options = relaxed(), full_options = options;
    full_options.incremental_analysis = false;
    auto syntax = parser.parse(document.snapshot(), options);
    check_same_analysis(syntax, reference.parse(document.snapshot(), full_options));
    std::vector<std::pair<SyntaxSnapshot, SyntaxSnapshot>> retained;
    const std::array<std::string, 10> replacements{
        "中🙂", "`", "**", "~~", "\n\n", "# ", "|", "é", "", "```\n"};
    for (int iteration = 0; iteration < 64; ++iteration) {
        INFO(iteration);
        const auto& rope = document.text();
        auto begin = static_cast<std::size_t>(random() % (rope.size_bytes() + 1));
        while (!rope.is_boundary(begin))
            --begin;
        auto end = std::min(rope.size_bytes(), begin + random() % 9);
        while (!rope.is_boundary(end))
            ++end;
        if (iteration % 16 == 0)
            retained.emplace_back(syntax, reference.parse(document.snapshot(), full_options));
        auto edits =
            transaction(document, {edit(begin, end, replacements[random() % replacements.size()])});
        if (!document.apply(edits))
            continue;
        const auto previous = syntax;
        syntax =
            parser.update(syntax, edits, document.revision(), document.content_identity(), options);
        const auto full = reference.parse(document.snapshot(), full_options);
        check_same_analysis(syntax, full);
        check_same_analysis(reference.parse(document.snapshot(), options), full);
        check_same_analysis(
            reference.update(
                previous, edits, document.revision(), document.content_identity(), full_options),
            full);
        for (const auto kind :
             {TextObjectKind::heading, TextObjectKind::string, TextObjectKind::block})
            CHECK(syntax.enclosing_object(bytes(begin, begin), kind) ==
                  full.enclosing_object(bytes(begin, begin), kind));
    }
    for (const auto& [snapshot, full] : retained)
        check_same_analysis(snapshot, full);
}

TEST_CASE("Markdown cached inline captures still enforce limits and cancellation") {
    std::string source;
    for (int i = 0; i < 80; ++i)
        source += "# Heading\n\nA `code` and **bold** with [link](target).\n\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::markdown);
    const auto original = parser.parse(document.snapshot(), relaxed());
    const auto update = [&](ParseOptions options) {
        return parser.update(original,
                             {original.revision(), {}, std::nullopt},
                             {original.revision().value + 1},
                             document.content_identity(),
                             options);
    };
    for (int limit = 0; limit < 4; ++limit) {
        auto options = relaxed();
        if (limit == 0)
            options.max_nodes = 1;
        else if (limit == 1)
            options.max_spans = 1;
        else if (limit == 2)
            options.max_outline_items = 1;
        else
            options.max_text_objects = 1;
        CHECK_THROWS_AS((void)update(options), std::length_error);
    }
    auto options = relaxed();
    std::stop_source stop;
    stop.request_stop();
    options.stop = stop.get_token();
    CHECK_THROWS_AS((void)update(options), ParseCancelled);
    options = relaxed();
    options.timeout = std::chrono::milliseconds::zero();
    CHECK_THROWS_AS((void)update(options), std::invalid_argument);
    const auto cached = update(relaxed());
    CHECK(cached.analysis_statistics().inline_parses == 0);
    check_same_analysis(cached, original);
    CHECK(original.matches(document.snapshot()));
}

TEST_CASE("independent Markdown workers preserve shared snapshots and analysis mode transitions") {
    std::string source;
    for (int i = 0; i < 80; ++i)
        source += "# Heading " + std::to_string(i) + "\n\nText `old` and **bold 中🙂**.\n\n";
    astra::Document document({1}, source);
    SyntaxParser parser(LanguageId::markdown), reference(LanguageId::markdown);
    auto options = relaxed(), full_options = options;
    full_options.incremental_analysis = false;
    const auto cached = parser.parse(document.snapshot(), options);
    const auto full = reference.parse(document.snapshot(), full_options);
    const auto begin = source.find("old");
    const auto worker = [begin](SyntaxSnapshot previous, ParseOptions settings, std::string text) {
        SyntaxParser parser(LanguageId::markdown);
        return parser.update(previous,
                             {previous.revision(), {edit(begin, begin + 3, text)}, {}},
                             {previous.revision().value + 1},
                             {1},
                             settings);
    };
    auto first = std::async(std::launch::async, worker, cached, options, "one");
    auto second = std::async(std::launch::async, worker, cached, options, "two");
    const auto one = first.get(), two = second.get();
    astra::Document first_document({1}, one.text().str()), second_document({1}, two.text().str());
    check_same_analysis(one, reference.parse(first_document.snapshot(), full_options));
    check_same_analysis(two, reference.parse(second_document.snapshot(), full_options));
    check_same_analysis(worker(cached, full_options, "one"), one);
    check_same_analysis(worker(full, options, "one"), one);
    check_same_analysis(cached, full);
    CHECK(cached.text().str() == source);
}

TEST_CASE("randomized incremental syntax matches fresh parsing after every edit") {
    constexpr unsigned int seed = 0x53594e54U;
    INFO("seed = ", seed);
    std::mt19937 random(seed);
    astra::Document document({1}, "{\"value\": [0, 1, 2], \"label\": \"start\"}");
    SyntaxParser parser(LanguageId::json);
    auto syntax = parser.parse(document.snapshot(), relaxed());
    std::vector<SyntaxSnapshot> retained;
    for (std::size_t iteration = 0; iteration < 120; ++iteration) {
        INFO(iteration);
        const auto source = document.text().str();
        const auto begin = source.find('[');
        const auto end = source.find(']') + 1;
        const auto replacement = "[" + std::to_string(random() % 10000) + ", " +
                                 std::to_string(random() % 10000) + ", " +
                                 std::to_string(iteration) + "]";
        auto edits = transaction(document, {edit(begin, end, replacement)});
        REQUIRE(document.apply(edits));
        if (iteration % 20 == 0) {
            retained.push_back(syntax);
        }
        syntax = parser.update(
            syntax, edits, document.revision(), document.content_identity(), relaxed());
        check_same_analysis(syntax, parser.parse(document.snapshot(), relaxed()));
        CHECK(syntax.matches(document.snapshot()));
    }
    for (const auto& snapshot : retained) {
        CHECK(snapshot.highlights().validate());
        CHECK_FALSE(snapshot.has_errors());
        CHECK_FALSE(snapshot.matches(document.snapshot()));
    }
}
