#include <astra/editing/commands.hpp>

#include <astra/core/utf8.hpp>

#include <utf8proc.h>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace astra::editing {
namespace {

struct Line {
    std::size_t start{};
    std::size_t content_end{};
    std::size_t end{};
    std::string text;
    std::vector<std::size_t> boundaries;
};

enum class CharacterClass { whitespace, word, punctuation };

CharacterClass classify(char32_t scalar) {
    const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(scalar));
    if ((category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_PC) || scalar == 0x200c || scalar == 0x200d) {
        return CharacterClass::word;
    }
    if (category == UTF8PROC_CATEGORY_ZS || category == UTF8PROC_CATEGORY_ZL || category == UTF8PROC_CATEGORY_ZP ||
        scalar == U'\t' || scalar == U'\r' || scalar == U'\n' || scalar == U'\v' || scalar == U'\f' || scalar == 0x85) {
        return CharacterClass::whitespace;
    }
    return CharacterClass::punctuation;
}

class Lines {
public:
    explicit Lines(const Rope& rope) : rope_(rope) {}

    const Line& get(std::size_t number) {
        if (const auto found = cache_.find(number); found != cache_.end()) { return found->second; }
        Line line;
        line.start = rope_.line_start(number);
        line.content_end = rope_.line_end(number);
        line.end = number + 1 < rope_.line_count() ? rope_.line_start(number + 1) : rope_.size_bytes();
        line.text = rope_.slice(line.start, line.end);
        line.boundaries.push_back(line.start);
        utf8proc_int32_t state = 0;
        std::optional<char32_t> previous;
        for (std::size_t offset = 0; offset < line.text.size();) {
            const auto scalar = utf8::decode(line.text, offset);
            if (previous && utf8proc_grapheme_break_stateful(static_cast<utf8proc_int32_t>(*previous),
                static_cast<utf8proc_int32_t>(scalar.value), &state)) {
                line.boundaries.push_back(line.start + offset);
            }
            previous = scalar.value;
            offset += scalar.bytes;
        }
        if (line.boundaries.back() != line.end) { line.boundaries.push_back(line.end); }
        return cache_.emplace(number, std::move(line)).first->second;
    }

    std::size_t left(std::size_t offset) {
        if (offset == 0) { return 0; }
        auto number = rope_.byte_to_position(offset).line;
        if (offset == rope_.line_start(number) && number != 0) { --number; }
        const auto& line = get(number);
        auto found = std::lower_bound(line.boundaries.begin(), line.boundaries.end(), offset);
        if (found == line.boundaries.begin()) { return line.start; }
        return *--found;
    }

    std::size_t right(std::size_t offset) {
        if (offset == rope_.size_bytes()) { return offset; }
        const auto& line = get(rope_.byte_to_position(offset).line);
        const auto found = std::upper_bound(line.boundaries.begin(), line.boundaries.end(), offset);
        return found == line.boundaries.end() ? line.end : *found;
    }

    std::size_t floor(std::size_t offset) {
        const auto& line = get(rope_.byte_to_position(offset).line);
        auto found = std::upper_bound(line.boundaries.begin(), line.boundaries.end(), offset);
        return found == line.boundaries.begin() ? line.start : *--found;
    }

    CharacterClass character_class(std::size_t offset) {
        const auto& line = get(rope_.byte_to_position(offset).line);
        return classify(utf8::decode(line.text, offset - line.start).value);
    }

    std::size_t word_left(std::size_t offset) {
        while (offset != 0) {
            const auto previous = left(offset);
            if (character_class(previous) != CharacterClass::whitespace) { break; }
            offset = previous;
        }
        if (offset == 0) { return 0; }
        const auto type = character_class(left(offset));
        while (offset != 0) {
            const auto previous = left(offset);
            if (character_class(previous) != type) { break; }
            offset = previous;
        }
        return offset;
    }

    std::size_t word_right(std::size_t offset) {
        offset = floor(offset);
        while (offset < rope_.size_bytes() && character_class(offset) == CharacterClass::whitespace) { offset = right(offset); }
        if (offset == rope_.size_bytes()) { return offset; }
        const auto type = character_class(offset);
        while (offset < rope_.size_bytes() && character_class(offset) == type) { offset = right(offset); }
        return offset;
    }

    std::size_t vertical(std::size_t offset, bool down) {
        const auto position = rope_.byte_to_position(offset);
        if ((!down && position.line == 0) || (down && position.line + 1 == rope_.line_count())) { return offset; }
        const auto& target = get(down ? position.line + 1 : position.line - 1);
        std::size_t column = 0;
        std::size_t local = 0;
        while (target.start + local < target.content_end) {
            const auto scalar = utf8::decode(target.text, local);
            const auto width = utf8::utf16_width(scalar.value);
            if (width > position.utf16_column - column) { break; }
            column += width;
            local += scalar.bytes;
        }
        auto found = std::upper_bound(target.boundaries.begin(), target.boundaries.end(), target.start + local);
        return found == target.boundaries.begin() ? target.start : *--found;
    }

    std::size_t indentation_end(std::size_t number) {
        const auto& line = get(number);
        auto offset = line.start;
        while (offset < line.content_end && (line.text[offset - line.start] == ' ' || line.text[offset - line.start] == '\t')) { ++offset; }
        return offset;
    }

private:
    const Rope& rope_;
    std::map<std::size_t, Line> cache_;
};

std::set<std::size_t> selected_lines(const Document& document) {
    std::set<std::size_t> result;
    for (const auto& selection : document.selections()) {
        const auto first = document.text().byte_to_position(selection.begin().value).line;
        auto last = document.text().byte_to_position(selection.end().value).line;
        if (!selection.empty() && last > first && selection.end().value == document.text().line_start(last)) { --last; }
        for (auto number = first; number <= last; ++number) { result.insert(number); }
    }
    return result;
}

bool set_selections(Document& document, std::vector<Selection> selections) {
    const auto before = document.selections();
    document.set_selections(std::move(selections));
    return before != document.selections();
}

bool apply(Document& document, Transaction transaction) {
    const auto before = document.selections();
    const bool changed = document.apply(std::move(transaction));
    return changed || before != document.selections();
}

bool move(Document& document, Command command, const Options& options, Lines& lines) {
    std::vector<Selection> selections;
    selections.reserve(document.selections().size());
    for (const auto& selection : document.selections()) {
        auto position = selection.head.value;
        if (!options.extend && !selection.empty() && (command == Command::move_left || command == Command::move_right)) {
            position = command == Command::move_left ? selection.begin().value : selection.end().value;
        } else {
            switch (command) {
            case Command::move_left: position = lines.left(position); break;
            case Command::move_right: position = lines.right(position); break;
            case Command::move_word_left: position = lines.word_left(position); break;
            case Command::move_word_right: position = lines.word_right(position); break;
            case Command::move_up: position = lines.vertical(position, false); break;
            case Command::move_down: position = lines.vertical(position, true); break;
            case Command::move_home: {
                const auto number = document.text().byte_to_position(position).line;
                const auto indent = lines.indentation_end(number);
                position = position == indent ? lines.get(number).start : lines.floor(indent);
                break;
            }
            case Command::move_end: position = lines.get(document.text().byte_to_position(position).line).content_end; break;
            case Command::move_document_start: position = 0; break;
            case Command::move_document_end: position = document.text().size_bytes(); break;
            default: throw std::invalid_argument("command is not a movement command");
            }
        }
        const ByteOffset target{position};
        selections.push_back({options.extend ? selection.anchor : target, target});
    }
    return set_selections(document, std::move(selections));
}

bool erase(Document& document, Command command, Lines& lines) {
    std::vector<Edit> edits;
    std::vector<ByteOffset> carets;
    for (const auto& selection : document.selections()) {
        auto begin = selection.begin().value;
        auto end = selection.end().value;
        if (selection.empty()) {
            const bool interior = lines.floor(begin) != begin;
            if (command == Command::backspace) { begin = lines.left(begin); }
            else if (command == Command::delete_forward) { end = lines.right(end); }
            else if (command == Command::delete_word_backward) { begin = lines.word_left(begin); }
            else { end = lines.word_right(end); }
            if (interior) {
                begin = std::min(begin, lines.floor(selection.head.value));
                end = std::max(end, lines.right(selection.head.value));
            }
        }
        carets.push_back(ByteOffset{begin});
        if (begin != end) { edits.push_back({ByteOffset{begin}, ByteOffset{end}, {}}); }
    }
    std::ranges::sort(edits, {}, &Edit::begin);
    std::vector<Edit> merged;
    for (const auto& edit : edits) {
        if (!merged.empty() && edit.begin <= merged.back().end) { merged.back().end = std::max(merged.back().end, edit.end); }
        else { merged.push_back(edit); }
    }
    std::vector<Selection> selections;
    for (const auto caret : carets) {
        const auto target = transform_offset(caret, merged, AnchorBias::before);
        selections.push_back({target, target});
    }
    return apply(document, Transaction{document.revision(), std::move(merged), std::move(selections)});
}

bool newline(Document& document, const Options& options, Lines& lines) {
    Transaction transaction{document.revision(), {}, std::vector<Selection>{}};
    std::size_t source = 0;
    std::size_t destination = 0;
    for (const auto& selection : document.selections()) {
        const auto number = document.text().byte_to_position(selection.begin().value).line;
        const auto& line = lines.get(number);
        const auto indent_end = std::min(selection.begin().value, lines.indentation_end(number));
        const auto text = options.newline + line.text.substr(0, indent_end - line.start);
        destination += selection.begin().value - source + text.size();
        source = selection.end().value;
        transaction.edits.push_back({selection.begin(), selection.end(), text});
        transaction.selections_after->push_back({ByteOffset{destination}, ByteOffset{destination}});
    }
    return apply(document, std::move(transaction));
}

bool indentation(Document& document, Command command, const Options& options, Lines& lines) {
    Transaction transaction{document.revision(), {}, std::nullopt};
    for (const auto number : selected_lines(document)) {
        const auto& line = lines.get(number);
        if (command == Command::indent) {
            transaction.edits.push_back({ByteOffset{line.start}, ByteOffset{line.start},
                options.insert_spaces ? std::string(options.tab_size, ' ') : "\t"});
        } else {
            std::size_t bytes = 0;
            std::size_t columns = 0;
            while (line.start + bytes < line.content_end && columns < options.tab_size) {
                if (line.text[bytes] == ' ') { ++columns; }
                else if (line.text[bytes] == '\t') { columns += options.tab_size - columns % options.tab_size; }
                else { break; }
                ++bytes;
            }
            if (bytes != 0) { transaction.edits.push_back({ByteOffset{line.start}, ByteOffset{line.start + bytes}, {}}); }
        }
    }
    return apply(document, std::move(transaction));
}

bool comment(Document& document, const Options& options, Lines& lines) {
    const auto numbers = selected_lines(document);
    bool remove = true;
    for (const auto number : numbers) {
        const auto& line = lines.get(number);
        const auto local = lines.indentation_end(number) - line.start;
        if (!std::string_view(line.text).substr(local, line.content_end - line.start - local).starts_with(options.comment_prefix)) { remove = false; }
    }
    Transaction transaction{document.revision(), {}, std::nullopt};
    for (const auto number : numbers) {
        const auto& line = lines.get(number);
        const auto begin = lines.indentation_end(number);
        auto end = begin;
        if (remove) {
            end += options.comment_prefix.size();
            if (end < line.content_end && line.text[end - line.start] == ' ') { ++end; }
        }
        transaction.edits.push_back({ByteOffset{begin}, ByteOffset{end}, remove ? "" : options.comment_prefix + " "});
    }
    return apply(document, std::move(transaction));
}

bool duplicate(Document& document, const Options& options, Lines& lines) {
    struct Block { std::size_t first; std::size_t last; std::size_t begin; std::size_t end; std::size_t separator; };
    std::vector<Block> blocks;
    for (const auto number : selected_lines(document)) {
        if (!blocks.empty() && number == blocks.back().last + 1) { blocks.back().last = number; }
        else { blocks.push_back({number, number, 0, 0, 0}); }
    }
    Transaction transaction{document.revision(), {}, std::vector<Selection>{}};
    for (auto& block : blocks) {
        block.begin = lines.get(block.first).start;
        const auto& last = lines.get(block.last);
        block.end = last.end;
        auto text = document.text().slice(block.begin, block.end);
        if (last.content_end == last.end) { block.separator = options.newline.size(); text.insert(0, options.newline); }
        transaction.edits.push_back({ByteOffset{block.end}, ByteOffset{block.end}, std::move(text)});
    }
    for (const auto& selection : document.selections()) {
        const auto number = document.text().byte_to_position(selection.begin().value).line;
        const auto found = std::ranges::find_if(blocks, [number](const Block& block) { return number >= block.first && number <= block.last; });
        if (found == blocks.end()) { throw std::logic_error("selection has no duplication block"); }
        const auto base = transform_offset(ByteOffset{found->end}, transaction.edits, AnchorBias::before).value + found->separator;
        transaction.selections_after->push_back({ByteOffset{base + selection.anchor.value - found->begin},
            ByteOffset{base + selection.head.value - found->begin}});
    }
    return apply(document, std::move(transaction));
}

} // namespace

bool execute(Document& document, Command command, const Options& options) {
    if (options.tab_size == 0 || options.tab_size > 256) { throw std::invalid_argument("tab size must be between 1 and 256"); }
    if (options.newline != "\n" && options.newline != "\r\n" && options.newline != "\r") { throw std::invalid_argument("invalid newline sequence"); }
    if (options.comment_prefix.empty() || options.comment_prefix.find_first_of("\r\n") != std::string::npos ||
        options.comment_prefix.find('\0') != std::string::npos) { throw std::invalid_argument("invalid line comment prefix"); }
    utf8::validate(options.comment_prefix);
    Lines lines(document.text());
    switch (command) {
    case Command::move_left: case Command::move_right: case Command::move_word_left: case Command::move_word_right:
    case Command::move_up: case Command::move_down: case Command::move_home: case Command::move_end:
    case Command::move_document_start: case Command::move_document_end:
        return move(document, command, options, lines);
    case Command::select_all:
        return set_selections(document, {{ByteOffset{0}, ByteOffset{document.text().size_bytes()}}});
    case Command::select_word: {
        std::vector<Selection> selections;
        for (const auto& selection : document.selections()) {
            auto begin = selection.head.value;
            if (begin == document.text().size_bytes() && begin != 0) { begin = lines.left(begin); }
            else { begin = lines.floor(begin); }
            auto end = begin;
            if (begin < document.text().size_bytes()) {
                const auto number = document.text().byte_to_position(begin).line;
                const auto& line = lines.get(number);
                const auto type = lines.character_class(begin);
                while (begin > line.start && lines.character_class(lines.left(begin)) == type) { begin = lines.left(begin); }
                while (end < line.content_end && lines.character_class(end) == type) { end = lines.right(end); }
            }
            selections.push_back({ByteOffset{begin}, ByteOffset{end}});
        }
        return set_selections(document, std::move(selections));
    }
    case Command::backspace: case Command::delete_forward: case Command::delete_word_backward: case Command::delete_word_forward:
        return erase(document, command, lines);
    case Command::newline: return newline(document, options, lines);
    case Command::indent: case Command::outdent: return indentation(document, command, options, lines);
    case Command::toggle_line_comment: return comment(document, options, lines);
    case Command::duplicate_line: return duplicate(document, options, lines);
    case Command::add_cursor_above: case Command::add_cursor_below: {
        auto selections = document.selections();
        for (const auto& selection : document.selections()) {
            const auto offset = lines.vertical(selection.head.value, command == Command::add_cursor_below);
            if (offset != selection.head.value) { selections.push_back({ByteOffset{offset}, ByteOffset{offset}}); }
        }
        return set_selections(document, std::move(selections));
    }
    case Command::insert_text: {
        const auto before = document.selections();
        const auto changed = document.replace_selections(options.text);
        return changed || before != document.selections();
    }
    }
    throw std::invalid_argument("unknown editing command");
}

} // namespace astra::editing
