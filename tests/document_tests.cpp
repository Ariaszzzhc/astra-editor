#include <astra/core/document.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <utility>
#include <vector>
#include <type_traits>

namespace {

astra::ByteOffset byte(std::size_t value) { return astra::ByteOffset{value}; }

astra::Selection selection(std::size_t anchor, std::size_t head) {
    return {byte(anchor), byte(head)};
}

astra::Edit edit(std::size_t begin, std::size_t end, std::string text) {
    return {byte(begin), byte(end), std::move(text)};
}

astra::Transaction transaction(const astra::Document& document,
                               std::vector<astra::Edit> edits) {
    return {document.revision(), std::move(edits), std::nullopt};
}

void check_unchanged(const astra::Document& document, const astra::DocumentSnapshot& before) {
    CHECK(document.text().str() == before.text.str());
    CHECK(document.revision() == before.revision);
    CHECK(document.content_identity() == before.content_identity);
    CHECK(document.selections() == before.selections);
}

} // namespace

TEST_CASE("explicit document clones isolate history and publish without throwing") {
    static_assert(std::is_nothrow_move_assignable_v<astra::Document>);
    astra::Document original({501}, "baseline");
    REQUIRE(original.replace_selections("first "));
    original.mark_saved();
    REQUIRE(original.replace_selections("second "));
    REQUIRE(original.undo());
    auto prepared = original.clone();
    CHECK(prepared.snapshot().text.str() == original.text().str());
    CHECK(prepared.can_redo());
    CHECK_FALSE(prepared.dirty());
    REQUIRE(prepared.redo());
    REQUIRE(prepared.replace_selections("third "));
    CHECK(original.text().str() == "first baseline");
    CHECK(original.can_redo());
    CHECK_FALSE(original.dirty());
    const auto committed_text = prepared.text().str();
    original = std::move(prepared);
    CHECK(original.text().str() == committed_text);
    CHECK(original.dirty());
    REQUIRE(original.undo());
    REQUIRE(original.undo());
    CHECK(original.text().str() == "first baseline");
    CHECK_FALSE(original.dirty());
}

TEST_CASE("document starts with a clean persistent snapshot") {
    const astra::Document document(astra::BufferId{7}, "hello\nworld");
    const auto snapshot = document.snapshot();
    CHECK(snapshot.id == astra::BufferId{7});
    CHECK(snapshot.revision == astra::Revision{0});
    CHECK(snapshot.text.str() == "hello\nworld");
    REQUIRE(snapshot.selections.size() == 1);
    CHECK(snapshot.selections[0] == selection(0, 0));
    CHECK_FALSE(document.dirty());
    CHECK_FALSE(document.can_undo());
    CHECK_FALSE(document.can_redo());
}

TEST_CASE("multicursor edit is one transaction with selection history") {
    astra::Document document(astra::BufferId{1}, "one two three");
    document.set_selections({selection(13, 8), selection(0, 3)});
    const auto before = document.snapshot();
    REQUIRE(document.replace_selections("X"));
    CHECK(document.text().str() == "X two X");
    const std::vector expected{selection(1, 1), selection(7, 7)};
    CHECK(document.selections() == expected);
    CHECK(document.revision() == astra::Revision{1});
    CHECK(document.dirty());
    CHECK(before.text.str() == "one two three");
    REQUIRE(document.undo());
    CHECK(document.text().str() == "one two three");
    CHECK(document.selections() == before.selections);
    CHECK(document.revision() == astra::Revision{2});
    CHECK_FALSE(document.dirty());
    CHECK_FALSE(document.can_undo());
    REQUIRE(document.redo());
    CHECK(document.text().str() == "X two X");
    CHECK(document.selections() == expected);
    CHECK(document.revision() == astra::Revision{3});
}

TEST_CASE("stale transactions reject every field before mutation") {
    astra::Document document(astra::BufferId{1}, "base");
    auto stale = transaction(document, {edit(0, 4, "stale")});
    REQUIRE(document.apply(transaction(document, {edit(0, 0, "new ")})));
    const auto before = document.snapshot();
    stale.selections_after = std::vector{selection(0, 0)};
    CHECK_THROWS_AS((void)document.apply(std::move(stale)), astra::StaleRevision);
    check_unchanged(document, before);
    REQUIRE(document.undo());
    CHECK(document.text().str() == "base");
    CHECK_THROWS_AS((void)document.apply({astra::Revision{0}, {}, std::nullopt}), astra::StaleRevision);
}

TEST_CASE("invalid edits and post selections have an all or nothing effect") {
    astra::Document document(astra::BufferId{1}, "abcdef");
    const auto before = document.snapshot();
    SUBCASE("out of bounds") {
        CHECK_THROWS_AS((void)document.apply(transaction(document,
            {edit(0, 1, "valid"), edit(4, 8, "bad")})), std::out_of_range);
    }
    SUBCASE("reversed range") {
        CHECK_THROWS_AS((void)document.apply(transaction(document,
            {edit(0, 1, "valid"), edit(5, 2, "bad")})), std::invalid_argument);
    }
    SUBCASE("malformed UTF-8 replacement") {
        CHECK_THROWS_AS((void)document.apply(transaction(document,
            {edit(0, 1, "valid"), edit(4, 5, "\xc0\xaf")})), std::invalid_argument);
    }
    SUBCASE("invalid post transaction selection") {
        auto request = transaction(document, {edit(0, 6, "x")});
        request.selections_after = std::vector{selection(4, 4)};
        CHECK_THROWS_AS((void)document.apply(std::move(request)), std::out_of_range);
    }
    SUBCASE("empty post transaction selection") {
        auto request = transaction(document, {edit(0, 6, "x")});
        request.selections_after = std::vector<astra::Selection>{};
        CHECK_THROWS_AS((void)document.apply(std::move(request)), std::invalid_argument);
    }
    check_unchanged(document, before);
    CHECK_FALSE(document.dirty());
    CHECK_FALSE(document.can_undo());
}

TEST_CASE("transactions use original coordinates and allow adjacent replacements") {
    astra::Document document(astra::BufferId{1}, "abcdefgh");
    REQUIRE(document.apply(transaction(document,
        {edit(6, 8, "!"), edit(2, 4, "x"), edit(0, 2, "long")})));
    CHECK(document.text().str() == "longxef!");
    REQUIRE(document.undo());
    CHECK(document.text().str() == "abcdefgh");
}

TEST_CASE("overlapping and ambiguously ordered edits are rejected") {
    astra::Document document(astra::BufferId{1}, "abcdef");
    const auto before = document.snapshot();
    SUBCASE("intersecting replacements") {
        CHECK_THROWS_AS((void)document.apply(transaction(document,
            {edit(0, 3, "x"), edit(2, 4, "y")})), std::invalid_argument);
    }
    SUBCASE("duplicate insertions") {
        CHECK_THROWS_AS((void)document.apply(transaction(document,
            {edit(2, 2, "x"), edit(2, 2, "y")})), std::invalid_argument);
    }
    SUBCASE("insertion at replacement start") {
        CHECK_THROWS_AS((void)document.apply(transaction(document,
            {edit(2, 4, "x"), edit(2, 2, "y")})), std::invalid_argument);
    }
    SUBCASE("insertion at replacement end") {
        CHECK_THROWS_AS((void)document.apply(transaction(document,
            {edit(2, 4, "x"), edit(4, 4, "y")})), std::invalid_argument);
    }
    SUBCASE("no-op is validated before filtering") {
        CHECK_THROWS_AS((void)document.apply(transaction(document,
            {edit(2, 4, "cd"), edit(3, 3, "x")})), std::invalid_argument);
    }
    check_unchanged(document, before);
}

TEST_CASE("selections normalize deterministically while retaining direction") {
    astra::Document document(astra::BufferId{1}, "abcdefghij");
    document.set_selections({selection(8, 8), selection(4, 1),
        selection(2, 6), selection(8, 8), selection(9, 10)});
    const std::vector expected{selection(6, 1), selection(8, 8), selection(9, 10)};
    CHECK(document.selections() == expected);
    CHECK(document.revision() == astra::Revision{0});
    CHECK_FALSE(document.dirty());
    CHECK_FALSE(document.can_undo());
    const auto before = document.snapshot();
    CHECK_THROWS_AS((void)document.set_selections({}), std::invalid_argument);
    CHECK_THROWS_AS((void)document.set_selections({selection(12, 12)}), std::out_of_range);
    check_unchanged(document, before);
}

TEST_CASE("adjacent selections remain separate but duplicate boundary carets merge") {
    astra::Document document(astra::BufferId{1}, "abcd");
    document.set_selections({selection(2, 4), selection(0, 2), selection(2, 2)});
    REQUIRE(document.selections().size() == 2);
    REQUIRE(document.replace_selections("X"));
    CHECK(document.text().str() == "XX");
    const std::vector expected{selection(1, 1), selection(2, 2)};
    CHECK(document.selections() == expected);
}

TEST_CASE("Unicode document edits require scalar boundaries") {
    // A, supplementary-plane emoji, e + combining acute accent, CRLF, CJK.
    const std::string original = "A\xf0\x9f\x98\x80" "e\xcc\x81\r\n\xe4\xb8\xad";
    astra::Document document(astra::BufferId{1}, original);
    const auto before = document.snapshot();
    CHECK_THROWS_AS((void)document.set_selections({selection(2, 2)}), std::invalid_argument);
    CHECK_THROWS_AS((void)document.apply(transaction(document, {edit(2, 5, "!")})), std::invalid_argument);
    check_unchanged(document, before);
    document.set_selections({selection(1, 5), selection(10, 13)});
    REQUIRE(document.replace_selections("\xf0\x9f\x8c\x9f"));
    CHECK(document.text().str() == "A\xf0\x9f\x8c\x9f" "e\xcc\x81\r\n\xf0\x9f\x8c\x9f");
    const std::vector expected{selection(5, 5), selection(14, 14)};
    CHECK(document.selections() == expected);
    REQUIRE(document.undo());
    CHECK(document.text().str() == original);
}

TEST_CASE("default selection mapping tracks insertions and preserves reversed ranges") {
    astra::Document document(astra::BufferId{1}, "abcdef");
    document.set_selections({selection(1, 1), selection(6, 4)});
    REQUIRE(document.apply(transaction(document, {edit(1, 1, "XX"), edit(4, 6, "Y")})));
    CHECK(document.text().str() == "aXXbcdY");
    const std::vector expected{selection(3, 3), selection(7, 6)};
    CHECK(document.selections() == expected);
}

TEST_CASE("anchor bias defines positions at and inside replacements") {
    const std::vector edits{edit(2, 5, "XY"), edit(7, 7, "!")};
    using astra::AnchorBias;
    CHECK(astra::transform_offset(byte(1), edits, AnchorBias::after) == byte(1));
    CHECK(astra::transform_offset(byte(2), edits, AnchorBias::before) == byte(2));
    CHECK(astra::transform_offset(byte(2), edits, AnchorBias::after) == byte(4));
    CHECK(astra::transform_offset(byte(3), edits, AnchorBias::before) == byte(2));
    CHECK(astra::transform_offset(byte(3), edits, AnchorBias::after) == byte(4));
    CHECK(astra::transform_offset(byte(5), edits, AnchorBias::before) == byte(4));
    CHECK(astra::transform_offset(byte(7), edits, AnchorBias::before) == byte(6));
    CHECK(astra::transform_offset(byte(7), edits, AnchorBias::after) == byte(7));
    CHECK(astra::transform_offset(byte(9), edits, AnchorBias::after) == byte(9));
}

TEST_CASE("saved state follows history identity and asynchronous save snapshots") {
    astra::Document document(astra::BufferId{1}, "start");
    const auto initial = document.snapshot();
    REQUIRE(document.replace_selections("A"));
    const auto saving = document.snapshot();
    REQUIRE(document.replace_selections("B"));
    document.mark_saved(saving);
    CHECK(document.dirty());
    REQUIRE(document.undo());
    CHECK_FALSE(document.dirty());
    REQUIRE(document.undo());
    CHECK(document.dirty());
    REQUIRE(document.redo());
    CHECK_FALSE(document.dirty());
    REQUIRE(document.redo());
    CHECK(document.dirty());
    document.mark_saved();
    CHECK_FALSE(document.dirty());
    document.mark_saved(initial);
    CHECK(document.dirty());
    astra::Document other(astra::BufferId{2}, "other");
    CHECK_THROWS_AS((void)document.mark_saved(other.snapshot()), std::invalid_argument);
    CHECK(document.dirty());
}

TEST_CASE("invalidating saved identity keeps all historical states dirty") {
    astra::Document document(astra::BufferId{1}, "initial");
    const auto initial = document.snapshot();
    REQUIRE(document.replace_selections("A"));
    document.mark_saved();
    REQUIRE(document.replace_selections("B"));
    const auto before = document.snapshot();
    document.invalidate_saved_state();
    check_unchanged(document, before);
    CHECK(document.dirty());
    REQUIRE(document.undo());
    CHECK(document.dirty());
    REQUIRE(document.undo());
    CHECK(document.dirty());
    REQUIRE(document.redo());
    CHECK(document.dirty());
    REQUIRE(document.redo());
    CHECK(document.dirty());
    document.mark_saved(initial);
    CHECK(document.dirty());
    REQUIRE(document.undo());
    REQUIRE(document.undo());
    CHECK_FALSE(document.dirty());
    document.invalidate_saved_state();
    CHECK(document.dirty());
    document.mark_saved();
    CHECK_FALSE(document.dirty());
}

TEST_CASE("foreign saved snapshot cannot restore invalid saved identity") {
    astra::Document document(astra::BufferId{1}, "original");
    astra::Document foreign(astra::BufferId{2}, "foreign");
    document.invalidate_saved_state();
    const auto before = document.snapshot();
    CHECK_THROWS_AS((void)document.mark_saved(foreign.snapshot()), std::invalid_argument);
    check_unchanged(document, before);
    CHECK(document.dirty());
    REQUIRE(document.replace_selections("x"));
    REQUIRE(document.undo());
    CHECK(document.dirty());
    document.mark_saved(before);
    CHECK_FALSE(document.dirty());
    CHECK_THROWS_AS((void)document.mark_saved(foreign.snapshot()), std::invalid_argument);
    CHECK_FALSE(document.dirty());
}

TEST_CASE("branching edits invalidate redo without reusing saved identities") {
    astra::Document document(astra::BufferId{1});
    REQUIRE(document.replace_selections("a"));
    document.mark_saved();
    const auto saved_identity = document.content_identity();
    REQUIRE(document.undo());
    REQUIRE(document.replace_selections("b"));
    CHECK_FALSE(document.can_redo());
    CHECK_FALSE(document.redo());
    CHECK(document.dirty());
    CHECK(document.content_identity() != saved_identity);
    CHECK(document.revision() == astra::Revision{3});
}

TEST_CASE("no-op transactions preserve revision history and redo") {
    astra::Document document(astra::BufferId{1}, "abc");
    REQUIRE(document.replace_selections("X"));
    REQUIRE(document.undo());
    const auto before = document.snapshot();
    CHECK_FALSE(document.apply(transaction(document, {})));
    CHECK_FALSE(document.apply(transaction(document, {edit(0, 3, "abc")})));
    CHECK_FALSE(document.apply(transaction(document, {edit(1, 1, "")})));
    check_unchanged(document, before);
    CHECK(document.can_redo());
    auto selection_only = transaction(document, {});
    selection_only.selections_after = std::vector{selection(3, 3)};
    CHECK_FALSE(document.apply(std::move(selection_only)));
    CHECK(document.revision() == before.revision);
    REQUIRE(document.redo());
    CHECK(document.text().str() == "Xabc");
}

TEST_CASE("bounded history discards oldest operations and supports zero capacity") {
    astra::Document document(astra::BufferId{1}, "", 2);
    REQUIRE(document.replace_selections("a"));
    REQUIRE(document.replace_selections("b"));
    REQUIRE(document.replace_selections("c"));
    REQUIRE(document.undo());
    REQUIRE(document.undo());
    CHECK(document.text().str() == "a");
    CHECK_FALSE(document.undo());
    document.set_history_limit(1);
    REQUIRE(document.redo());
    CHECK(document.text().str() == "ab");
    CHECK_FALSE(document.redo());
    document.set_history_limit(0);
    CHECK_FALSE(document.can_undo());
    REQUIRE(document.replace_selections("!"));
    CHECK(document.text().str() == "ab!");
    CHECK_FALSE(document.can_undo());
}

TEST_CASE("failed transaction preserves undo redo and saved identity") {
    astra::Document document(astra::BufferId{1}, "base");
    REQUIRE(document.replace_selections("A"));
    document.mark_saved();
    REQUIRE(document.replace_selections("B"));
    REQUIRE(document.undo());
    const auto before = document.snapshot();
    REQUIRE(document.can_undo());
    REQUIRE(document.can_redo());
    REQUIRE_FALSE(document.dirty());
    auto invalid = transaction(document, {edit(0, 1, "\xf0\x9f\x98\x80")});
    invalid.selections_after = std::vector{selection(2, 2)};
    CHECK_THROWS_AS((void)document.apply(std::move(invalid)), std::invalid_argument);
    check_unchanged(document, before);
    CHECK_FALSE(document.dirty());
    REQUIRE(document.redo());
    CHECK(document.text().str() == "ABbase");
    REQUIRE(document.undo());
    CHECK_FALSE(document.dirty());
    REQUIRE(document.undo());
    CHECK(document.text().str() == "base");
}

TEST_CASE("randomized Unicode multicursor transactions match a flat reference") {
    constexpr std::uint64_t seed = 0x4d554c5449435552ULL;
    INFO("seed = ", seed);
    std::mt19937_64 random(seed);
    const std::vector<std::string> tokens{
        "a", "\xf0\x9f\x98\x80", "\xe4\xb8\xad", "e\xcc\x81", "\r\n", " ", "xyz"};
    for (std::size_t iteration = 0; iteration < 400; ++iteration) {
        INFO("iteration = ", iteration);
        std::string original;
        std::vector<std::size_t> boundaries{0};
        for (std::size_t index = 0; index < 40; ++index) {
            original += tokens[static_cast<std::size_t>(random() % tokens.size())];
            boundaries.push_back(original.size());
        }
        astra::Document document(astra::BufferId{1}, original);
        std::vector<astra::Selection> selections;
        // Non-overlapping separated cells permit independent insertions, deletions,
        // forward ranges and reversed ranges without deriving a model from the tree.
        for (std::size_t index = 0; index < 40; index += 4) {
            const auto begin = boundaries[index];
            const auto end = boundaries[index + static_cast<std::size_t>(random() % 3)];
            selections.push_back(random() % 2 == 0 ? selection(begin, end) : selection(end, begin));
        }
        std::shuffle(selections.begin(), selections.end(), random);
        document.set_selections(selections);
        const auto before = document.snapshot();
        const auto replacement = random() % 4 == 0 ? std::string{} :
            tokens[static_cast<std::size_t>(random() % tokens.size())];
        std::string expected;
        std::size_t source = 0;
        std::vector<astra::Selection> expected_selections;
        for (const auto& selected : document.selections()) {
            expected.append(original, source, selected.begin().value - source);
            expected += replacement;
            source = selected.end().value;
            expected_selections.push_back(selection(expected.size(), expected.size()));
        }
        expected.append(original, source, original.size() - source);
        const auto changed = document.replace_selections(replacement);
        CHECK(document.text().str() == expected);
        CHECK(document.selections() == expected_selections);
        CHECK(before.text.str() == original);
        if (changed) {
            REQUIRE(document.undo());
            CHECK(document.text().str() == original);
            CHECK(document.selections() == before.selections);
            CHECK_FALSE(document.can_undo());
            REQUIRE(document.redo());
            CHECK(document.text().str() == expected);
            CHECK(document.selections() == expected_selections);
        }
    }
}

TEST_CASE("randomized document edit history matches a flat reference") {
    constexpr std::uint64_t seed = 0x4153545241444f43ULL;
    INFO("seed = ", seed);
    std::mt19937_64 random(seed);
    constexpr std::size_t history_limit = 24;
    struct ReferenceState {
        std::string text;
        std::vector<astra::Selection> selections;
    };
    struct ReferenceEntry { ReferenceState before; ReferenceState after; };
    ReferenceState reference{"initial", {selection(0, 0)}};
    std::deque<ReferenceEntry> undo;
    std::deque<ReferenceEntry> redo;
    astra::Document document(astra::BufferId{1}, reference.text, history_limit);
    std::uint64_t revision = 0;
    std::vector<std::pair<astra::DocumentSnapshot, std::string>> retained;
    for (std::size_t iteration = 0; iteration < 1500; ++iteration) {
        INFO("iteration = ", iteration);
        const auto action = random() % 5;
        if (action == 0) {
            CHECK(document.undo() == !undo.empty());
            if (!undo.empty()) {
                reference = undo.back().before;
                redo.push_back(undo.back());
                undo.pop_back();
                ++revision;
            }
        } else if (action == 1) {
            CHECK(document.redo() == !redo.empty());
            if (!redo.empty()) {
                reference = redo.back().after;
                undo.push_back(redo.back());
                redo.pop_back();
                ++revision;
            }
        } else {
            auto begin = static_cast<std::size_t>(random() % (reference.text.size() + 1));
            auto end = static_cast<std::size_t>(random() % (reference.text.size() + 1));
            if (begin > end) { std::swap(begin, end); }
            const std::string replacement(static_cast<std::size_t>(random() % 9),
                static_cast<char>('a' + random() % 26));
            reference.selections = {selection(begin, end)};
            document.set_selections(reference.selections);
            const auto before = reference;
            reference.text.replace(begin, end - begin, replacement);
            reference.selections = {selection(begin + replacement.size(), begin + replacement.size())};
            const bool changed = before.text != reference.text;
            CHECK(document.replace_selections(replacement) == changed);
            if (changed) {
                undo.push_back({before, reference});
                redo.clear();
                ++revision;
            }
        }
        while (undo.size() > history_limit) { undo.pop_front(); }
        while (redo.size() > history_limit) { redo.pop_front(); }
        CHECK(document.text().str() == reference.text);
        CHECK(document.selections() == reference.selections);
        CHECK(document.revision() == astra::Revision{revision});
        CHECK(document.can_undo() == !undo.empty());
        CHECK(document.can_redo() == !redo.empty());
        if (iteration % 75 == 0) { retained.emplace_back(document.snapshot(), reference.text); }
    }
    for (const auto& [snapshot, text] : retained) {
        CHECK(snapshot.text.str() == text);
    }
}
