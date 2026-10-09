#pragma once

#include <astra/core/document.hpp>

#include <cstddef>
#include <string>

namespace astra::editing {

enum class Command {
    move_left, move_right, move_word_left, move_word_right, move_up, move_down,
    move_home, move_end, move_document_start, move_document_end,
    select_all, select_word, backspace, delete_forward,
    delete_word_backward, delete_word_forward, newline, indent, outdent,
    toggle_line_comment, duplicate_line, add_cursor_above, add_cursor_below, insert_text
};

struct Options {
    bool extend{};
    std::size_t tab_size{4};
    bool insert_spaces{true};
    std::string comment_prefix{"//"};
    std::string newline{"\n"};
    std::string text;
};

// Executes one user command. Every text mutation, including all cursors, is a
// single Document transaction; movement only updates the current selections.
// Grapheme boundaries are computed once per touched logical line per command.
// Vertical navigation uses UTF-16 columns snapped to grapheme boundaries; the
// GUI should use shaped pixel hit-testing to maintain visual columns across tabs.
// Word motion skips whitespace and then consumes one Unicode word/punctuation run.
[[nodiscard]] bool execute(Document& document, Command command, const Options& options = {});

} // namespace astra::editing
