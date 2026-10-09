#include <astra/editing/commands.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using astra::editing::Command;
using astra::editing::Options;
using astra::editing::execute;

astra::Selection selection(std::size_t anchor, std::size_t head) {
    return {astra::ByteOffset{anchor}, astra::ByteOffset{head}};
}

std::size_t caret(const astra::Document& document) {
    REQUIRE(document.selections().size() == 1);
    CHECK(document.selections()[0].empty());
    return document.selections()[0].head.value;
}

const std::vector<std::string> clusters{
    "A", "e\xcc\x81", "\xf0\x9f\x87\xad\xf0\x9f\x87\xb0", // Hong Kong RI pair
    "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb", // woman technologist
    "\xf0\x9f\x91\x8d\xf0\x9f\x8f\xbd", // emoji + skin tone
    "\r\n", "\xe4\xb8\xad"
};

} // namespace

TEST_CASE("editing arrow movement respects combining emoji ZWJ RI and CRLF graphemes") {
    std::string text;
    std::vector<std::size_t> boundaries{0};
    for (const auto& cluster : clusters) { text += cluster; boundaries.push_back(text.size()); }
    astra::Document document(astra::BufferId{1}, text);
    for (std::size_t index = 1; index < boundaries.size(); ++index) {
        REQUIRE(execute(document, Command::move_right));
        CHECK(caret(document) == boundaries[index]);
    }
    CHECK_FALSE(execute(document, Command::move_right));
    for (auto index = boundaries.size() - 1; index > 0; --index) {
        REQUIRE(execute(document, Command::move_left));
        CHECK(caret(document) == boundaries[index - 1]);
    }
    CHECK_FALSE(execute(document, Command::move_left));
    CHECK(document.revision() == astra::Revision{0});
    CHECK_FALSE(document.can_undo());
}

TEST_CASE("editing next occurrence selects words wraps and edits all matches in one undo step") {
    astra::Document document(astra::BufferId{1}, "one two one one");
    document.set_selections({selection(9, 9)});
    REQUIRE(execute(document, Command::select_next_occurrence));
    CHECK(document.selections() == std::vector{selection(8, 11)});
    REQUIRE(execute(document, Command::select_next_occurrence));
    REQUIRE(execute(document, Command::select_next_occurrence));
    CHECK(document.selections() == std::vector{selection(0, 3), selection(8, 11), selection(12, 15)});
    CHECK_FALSE(execute(document, Command::select_next_occurrence));
    CHECK_FALSE(document.can_undo());
    CHECK(document.revision() == astra::Revision{0});
    REQUIRE(document.replace_selections("X"));
    CHECK(document.text().str() == "X two X X");
    REQUIRE(document.undo());
    CHECK(document.text().str() == "one two one one");
    CHECK(document.selections().size() == 3);
}

TEST_CASE("editing occurrence search crosses windows and never selects part of a grapheme") {
    const std::string cluster = "e\xcc\x81";
    astra::Document document(astra::BufferId{2}, cluster + " " + std::string(65533, '.') + cluster + " end");
    document.set_selections({selection(0, 3)});
    REQUIRE(execute(document, Command::select_next_occurrence));
    CHECK(document.selections().back() == selection(65537, 65540));
    astra::Document combining(astra::BufferId{3}, "e e\xcc\x81 e");
    combining.set_selections({selection(0, 1)});
    REQUIRE(execute(combining, Command::select_next_occurrence));
    CHECK(combining.selections().back() == selection(6, 7));
    astra::Document overlapping(astra::BufferId{4}, "banana ana");
    overlapping.set_selections({selection(1, 4)});
    REQUIRE(execute(overlapping, Command::select_next_occurrence));
    CHECK(overlapping.selections().back() == selection(7, 10));
}

TEST_CASE("editing occurrence limit rejects without changing current selections") {
    std::string text;
    std::vector<astra::Selection> selections;
    for (std::size_t i = 0; i < 4097; ++i) {
        text += "x ";
        if (i < 4096) { selections.push_back(selection(i * 2, i * 2 + 1)); }
    }
    astra::Document document(astra::BufferId{5}, text);
    document.set_selections(selections);
    CHECK_THROWS_AS(static_cast<void>(execute(document, Command::select_next_occurrence)), std::length_error);
    CHECK(document.selections() == selections);
    CHECK_FALSE(document.dirty());
}

TEST_CASE("editing grapheme deletion preserves the complete unit and selection history") {
    for (const auto& cluster : clusters) {
        CAPTURE(cluster.size());
        astra::Document document(astra::BufferId{1}, "x" + cluster + "y");
        document.set_selections({selection(1 + cluster.size(), 1 + cluster.size())});
        const auto before = document.snapshot();
        REQUIRE(execute(document, Command::backspace));
        CHECK(document.text().str() == "xy");
        CHECK(caret(document) == 1);
        REQUIRE(document.undo());
        CHECK(document.text().str() == before.text.str());
        CHECK(document.selections() == before.selections);
        document.set_selections({selection(1, 1)});
        REQUIRE(execute(document, Command::delete_forward));
        CHECK(document.text().str() == "xy");
        CHECK(caret(document) == 1);
    }
}

TEST_CASE("editing scalar-boundary cursors inside graphemes snap safely") {
    astra::Document document(astra::BufferId{1}, "e\xcc\x81!");
    document.set_selections({selection(1, 1)});
    REQUIRE(execute(document, Command::move_right));
    CHECK(caret(document) == 3);
    document.set_selections({selection(1, 1)});
    REQUIRE(execute(document, Command::move_left));
    CHECK(caret(document) == 0);
    document.set_selections({selection(1, 1)});
    REQUIRE(execute(document, Command::backspace));
    CHECK(document.text().str() == "!");
    REQUIRE(document.undo());
    REQUIRE(execute(document, Command::delete_forward));
    CHECK(document.text().str() == "!");
}

TEST_CASE("editing regional-indicator parity and CRLF crossing rope chunks stay atomic") {
    astra::Document flags(astra::BufferId{1}, "\xf0\x9f\x87\xba\xf0\x9f\x87\xb8\xf0\x9f\x87\xa6");
    REQUIRE(execute(flags, Command::move_right));
    CHECK(caret(flags) == 8);
    REQUIRE(execute(flags, Command::move_right));
    CHECK(caret(flags) == 12);
    REQUIRE(execute(flags, Command::backspace));
    CHECK(flags.text().size_bytes() == 8);
    const std::string prefix(1023, 'a');
    astra::Document crlf(astra::BufferId{2}, prefix + "\r\nb");
    crlf.set_selections({selection(1024, 1024)}); // Between CR and LF is a scalar boundary.
    REQUIRE(execute(crlf, Command::backspace));
    CHECK(crlf.text().str() == prefix + "b");
    REQUIRE(crlf.undo());
    REQUIRE(execute(crlf, Command::delete_forward));
    CHECK(crlf.text().str() == prefix + "b");
}

TEST_CASE("editing selection extension preserves anchors and ordinary arrows collapse") {
    astra::Document document(astra::BufferId{1}, "abc");
    document.set_selections({selection(1, 1)});
    Options extend;
    extend.extend = true;
    REQUIRE(execute(document, Command::move_right, extend));
    CHECK(document.selections()[0] == selection(1, 2));
    REQUIRE(execute(document, Command::move_left));
    CHECK(caret(document) == 1);
    document.set_selections({selection(3, 1)});
    REQUIRE(execute(document, Command::move_right));
    CHECK(caret(document) == 3);
    REQUIRE(execute(document, Command::move_document_start, extend));
    CHECK(document.selections()[0] == selection(3, 0));
}

TEST_CASE("editing home toggles indentation and end excludes the complete CRLF") {
    astra::Document document(astra::BufferId{1}, "  abc\r\n\tx");
    document.set_selections({selection(4, 4)});
    REQUIRE(execute(document, Command::move_home));
    CHECK(caret(document) == 2);
    REQUIRE(execute(document, Command::move_home));
    CHECK(caret(document) == 0);
    REQUIRE(execute(document, Command::move_end));
    CHECK(caret(document) == 5);
    REQUIRE(execute(document, Command::move_right));
    CHECK(caret(document) == 7);
    REQUIRE(execute(document, Command::move_document_end));
    CHECK(caret(document) == 9);
    REQUIRE(execute(document, Command::move_document_start));
    CHECK(caret(document) == 0);
}

TEST_CASE("editing word motion and deletion classify Unicode words and punctuation") {
    astra::Document document(astra::BufferId{1}, "one  \xe4\xb8\xad\xe6\x96\x87! e\xcc\x81");
    REQUIRE(execute(document, Command::move_word_right));
    CHECK(caret(document) == 3);
    REQUIRE(execute(document, Command::move_word_right));
    CHECK(caret(document) == 11);
    REQUIRE(execute(document, Command::move_word_right));
    CHECK(caret(document) == 12);
    REQUIRE(execute(document, Command::move_word_right));
    CHECK(caret(document) == 16);
    REQUIRE(execute(document, Command::move_word_left));
    CHECK(caret(document) == 13);
    REQUIRE(execute(document, Command::delete_word_backward));
    CHECK(document.text().str() == "one  \xe4\xb8\xad\xe6\x96\x87" "e\xcc\x81");
    REQUIRE(document.undo());
    document.set_selections({selection(3, 3)});
    REQUIRE(execute(document, Command::delete_word_forward));
    CHECK(document.text().str() == "one! e\xcc\x81");
}

TEST_CASE("editing select word and select all do not mutate text history") {
    astra::Document document(astra::BufferId{1}, "one two\n");
    document.set_selections({selection(5, 5)});
    REQUIRE(execute(document, Command::select_word));
    CHECK(document.selections()[0] == selection(4, 7));
    REQUIRE(execute(document, Command::select_all));
    CHECK(document.selections()[0] == selection(0, 8));
    CHECK_FALSE(document.can_undo());
    astra::Document empty(astra::BufferId{2});
    CHECK_FALSE(execute(empty, Command::select_all));
    CHECK_FALSE(execute(empty, Command::select_word));
}

TEST_CASE("editing vertical motion uses UTF16 columns without splitting supplementary characters") {
    astra::Document document(astra::BufferId{1}, "ab\nx\xf0\x9f\x98\x80" "z\ne\xcc\x81" "ab\n");
    document.set_selections({selection(2, 2)}); // UTF-16 col 2, interior of emoji on next line.
    REQUIRE(execute(document, Command::move_down));
    CHECK(caret(document) == 4); // Snaps to before emoji, UTF-16 col 1.
    REQUIRE(execute(document, Command::move_down));
    CHECK(caret(document) == 10); // col 1 is inside e + combining mark, snaps to its start.
    REQUIRE(execute(document, Command::move_down));
    CHECK(caret(document) == document.text().size_bytes());
    CHECK_FALSE(execute(document, Command::move_down));
}

TEST_CASE("editing add cursor commands preserve existing selections and normalize duplicates") {
    astra::Document document(astra::BufferId{1}, "abc\nx\n1234");
    document.set_selections({selection(2, 2)});
    REQUIRE(execute(document, Command::add_cursor_below));
    const std::vector first{selection(2, 2), selection(5, 5)};
    CHECK(document.selections() == first);
    REQUIRE(execute(document, Command::add_cursor_below));
    const std::vector second{selection(2, 2), selection(5, 5), selection(7, 7)};
    CHECK(document.selections() == second);
    Options insert;
    insert.text = "!";
    REQUIRE(execute(document, Command::insert_text, insert));
    CHECK(document.text().str() == "ab!c\nx!\n1!234");
    REQUIRE(document.undo());
    CHECK(document.selections() == second);
    CHECK(document.text().str() == "abc\nx\n1234");
}

TEST_CASE("editing overlapping multicursor word deletions form one atomic history entry") {
    astra::Document document(astra::BufferId{1}, "hello world");
    document.set_selections({selection(2, 2), selection(4, 4), selection(5, 5)});
    const auto before = document.snapshot();
    REQUIRE(execute(document, Command::delete_word_backward));
    CHECK(document.text().str() == " world");
    CHECK(caret(document) == 0);
    CHECK(document.revision() == astra::Revision{1});
    REQUIRE(document.undo());
    CHECK(document.text().str() == "hello world");
    CHECK(document.selections() == before.selections);
    CHECK_FALSE(document.can_undo());
}

TEST_CASE("editing adjacent multicursor grapheme deletions merge without losing pre selections") {
    astra::Document document(astra::BufferId{1}, "abcd");
    document.set_selections({selection(1, 1), selection(2, 2), selection(3, 3)});
    const auto before = document.selections();
    REQUIRE(execute(document, Command::backspace));
    CHECK(document.text().str() == "d");
    CHECK(caret(document) == 0);
    REQUIRE(document.undo());
    CHECK(document.selections() == before);
}

TEST_CASE("editing backspace and delete use nonempty selections as exact replacement ranges") {
    astra::Document document(astra::BufferId{1}, "one two three");
    document.set_selections({selection(3, 0), selection(8, 13)});
    const auto before = document.selections();
    REQUIRE(execute(document, Command::delete_forward));
    CHECK(document.text().str() == " two ");
    REQUIRE(document.undo());
    CHECK(document.selections() == before);
    document.set_selections({selection(0, 0)});
    CHECK_FALSE(execute(document, Command::backspace));
    document.set_selections({selection(13, 13)});
    CHECK_FALSE(execute(document, Command::delete_forward));
}

TEST_CASE("editing newline keeps indentation at every cursor with one undo") {
    astra::Document document(astra::BufferId{1}, "  alpha\r\n\tbeta");
    document.set_selections({selection(7, 7), selection(14, 14)});
    const auto before = document.snapshot();
    Options options;
    options.newline = "\r\n";
    REQUIRE(execute(document, Command::newline, options));
    CHECK(document.text().str() == "  alpha\r\n  \r\n\tbeta\r\n\t");
    const std::vector expected{selection(11, 11), selection(21, 21)};
    CHECK(document.selections() == expected);
    REQUIRE(document.undo());
    CHECK(document.selections() == before.selections);
    CHECK(document.text().str() == before.text.str());
}

TEST_CASE("editing newline before indentation only carries the prefix before the caret") {
    astra::Document document(astra::BufferId{1}, "    value");
    document.set_selections({selection(2, 2)});
    REQUIRE(execute(document, Command::newline));
    CHECK(document.text().str() == "  \n    value");
    CHECK(caret(document) == 5);
}

TEST_CASE("editing indent excludes the next line when selection ends at its start") {
    astra::Document document(astra::BufferId{1}, "one\ntwo\nthree");
    document.set_selections({selection(0, 8)});
    const auto before = document.snapshot();
    Options options;
    options.tab_size = 2;
    REQUIRE(execute(document, Command::indent, options));
    CHECK(document.text().str() == "  one\n  two\nthree");
    CHECK(document.selections()[0] == selection(0, 12));
    REQUIRE(document.undo());
    CHECK(document.selections() == before.selections);
    CHECK(document.text().str() == before.text.str());
    CHECK_FALSE(document.can_undo());
}

TEST_CASE("editing outdent handles spaces tabs and duplicate selected lines atomically") {
    astra::Document document(astra::BufferId{1}, "    abc\n \txyz\nnoindent");
    document.set_selections({selection(4, 4), selection(6, 6), selection(0, 22)});
    // Document normalization merges ranges; all three logical lines are affected once.
    const auto before = document.snapshot();
    REQUIRE(execute(document, Command::outdent));
    CHECK(document.text().str() == "abc\nxyz\nnoindent");
    REQUIRE(document.undo());
    CHECK(document.selections() == before.selections);
    CHECK(document.text().str() == before.text.str());
}

TEST_CASE("editing line comments toggle across selected lines in one transaction") {
    astra::Document document(astra::BufferId{1}, "  alpha\n\tbeta\ngamma");
    document.set_selections({selection(0, 14)});
    const auto before = document.snapshot();
    REQUIRE(execute(document, Command::toggle_line_comment));
    CHECK(document.text().str() == "  // alpha\n\t// beta\ngamma");
    REQUIRE(execute(document, Command::toggle_line_comment));
    CHECK(document.text().str() == before.text.str());
    REQUIRE(document.undo());
    CHECK(document.text().str() == "  // alpha\n\t// beta\ngamma");
    REQUIRE(document.undo());
    CHECK(document.selections() == before.selections);
}

TEST_CASE("editing line comments support custom prefixes and empty lines") {
    astra::Document document(astra::BufferId{1}, "");
    Options options;
    options.comment_prefix = "#";
    REQUIRE(execute(document, Command::toggle_line_comment, options));
    CHECK(document.text().str() == "# ");
    REQUIRE(execute(document, Command::toggle_line_comment, options));
    CHECK(document.text().str().empty());
}

TEST_CASE("editing duplicate line moves cursors into copies and handles missing final newline") {
    astra::Document document(astra::BufferId{1}, "abc\ndef");
    document.set_selections({selection(5, 5)});
    const auto before = document.snapshot();
    REQUIRE(execute(document, Command::duplicate_line));
    CHECK(document.text().str() == "abc\ndef\ndef");
    CHECK(caret(document) == 9);
    REQUIRE(document.undo());
    CHECK(document.text().str() == before.text.str());
    CHECK(document.selections() == before.selections);
    document.set_selections({selection(1, 1)});
    REQUIRE(execute(document, Command::duplicate_line));
    CHECK(document.text().str() == "abc\nabc\ndef");
    CHECK(caret(document) == 5);
}

TEST_CASE("editing duplicate adjacent selected lines creates a single coherent copy") {
    astra::Document document(astra::BufferId{1}, "a\nb\nc\n");
    document.set_selections({selection(0, 4)});
    REQUIRE(execute(document, Command::duplicate_line));
    CHECK(document.text().str() == "a\nb\na\nb\nc\n");
    CHECK(document.selections()[0] == selection(4, 8));
    REQUIRE(document.undo());
    CHECK(document.text().str() == "a\nb\nc\n");
    CHECK(document.selections()[0] == selection(0, 4));
}

TEST_CASE("editing insertion is atomic for all cursors and rejects invalid options") {
    astra::Document document(astra::BufferId{1}, "ab");
    document.set_selections({selection(0, 0), selection(2, 2)});
    const auto before = document.snapshot();
    Options options;
    options.text = "\xf0\x9f\x98\x80";
    REQUIRE(execute(document, Command::insert_text, options));
    CHECK(document.text().str() == "\xf0\x9f\x98\x80" "ab\xf0\x9f\x98\x80");
    REQUIRE(document.undo());
    CHECK(document.selections() == before.selections);
    options.text = "\xc0\xaf";
    CHECK_THROWS_AS((void)execute(document, Command::insert_text, options), std::invalid_argument);
    options.tab_size = 0;
    CHECK_THROWS_AS((void)execute(document, Command::indent, options), std::invalid_argument);
    CHECK(document.text().str() == before.text.str());
    CHECK(document.selections() == before.selections);
}

TEST_CASE("editing randomized grapheme deletion matches a flat cluster model") {
    constexpr unsigned int seed = 0x45444954U;
    INFO("seed = ", seed);
    std::mt19937 random(seed);
    for (std::size_t iteration = 0; iteration < 400; ++iteration) {
        std::string text;
        std::vector<std::string> parts;
        for (std::size_t index = 0; index < 20; ++index) {
            // ASCII separators isolate RI pairs and combining sequences from
            // neighboring random clusters while preserving their internal rules.
            parts.push_back(clusters[random() % clusters.size()]);
            parts.push_back("|");
        }
        for (const auto& part : parts) { text += part; }
        astra::Document document(astra::BufferId{1}, text);
        const auto selected = static_cast<std::size_t>(random() % parts.size());
        std::size_t begin = 0;
        for (std::size_t index = 0; index < selected; ++index) { begin += parts[index].size(); }
        document.set_selections({selection(begin, begin)});
        REQUIRE(execute(document, Command::delete_forward));
        text.erase(begin, parts[selected].size());
        CHECK(document.text().str() == text);
        REQUIRE(document.undo());
        CHECK(caret(document) == begin);
    }
}
