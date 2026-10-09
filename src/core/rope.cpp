#include "astra/core/rope.hpp"

#include "astra/core/utf8.hpp"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace astra {

TextSummary MeasureText::operator()(std::string_view text) const {
    TextSummary result;
    result.bytes = text.size();
    for (std::size_t offset = 0; offset < text.size();) {
        const auto scalar = utf8::decode(text, offset);
        const auto units = utf8::utf16_width(scalar.value);
        result.utf16_units += units;
        if (scalar.value == U'\n') {
            ++result.line_breaks;
            result.trailing_bytes = 0;
            result.trailing_utf16 = 0;
        } else {
            result.trailing_bytes += scalar.bytes;
            result.trailing_utf16 += units;
        }
        offset += scalar.bytes;
    }
    return result;
}

Rope::Rope(std::string_view text) {
    // Validate before chunking: a malformed leading byte must not be mistaken
    // for a boundary or cause the chunking loop to stop making progress.
    utf8::validate(text);
    std::vector<std::string> chunks;
    chunks.reserve(text.size() / max_chunk_bytes + (text.size() % max_chunk_bytes != 0));
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = begin + std::min(max_chunk_bytes, text.size() - begin);
        while (!utf8::is_boundary(text, end)) {
            --end;
        }
        chunks.emplace_back(text.substr(begin, end - begin));
        begin = end;
    }
    tree_ = Tree(std::move(chunks));
}

Rope::Rope(Tree tree) : tree_(std::move(tree)) {}

bool Rope::empty() const noexcept { return tree_.empty(); }

std::size_t Rope::size_bytes() const noexcept { return summary().bytes; }

std::size_t Rope::size_utf16() const noexcept { return summary().utf16_units; }

std::size_t Rope::line_count() const noexcept { return summary().line_breaks + 1; }

const TextSummary& Rope::summary() const noexcept { return tree_.summary(); }

std::string Rope::str() const {
    std::string result;
    result.reserve(size_bytes());
    tree_.for_each([&result](const std::string& chunk) { result.append(chunk); });
    return result;
}

bool Rope::is_boundary(std::size_t byte_offset) const {
    if (byte_offset > size_bytes()) {
        return false;
    }
    const auto found = tree_.seek(byte_offset, &TextSummary::bytes);
    return !found.item || utf8::is_boundary(*found.item, byte_offset - found.prefix.bytes);
}

void Rope::require_boundary(std::size_t byte_offset) const {
    if (byte_offset > size_bytes()) {
        throw std::out_of_range("Rope byte offset is outside the text");
    }
    if (!is_boundary(byte_offset)) {
        throw std::invalid_argument("Rope byte offset splits a UTF-8 scalar");
    }
}

void Rope::require_range(std::size_t begin, std::size_t end) const {
    if (begin > end || end > size_bytes()) {
        throw std::out_of_range("Invalid Rope byte range");
    }
    require_boundary(begin);
    require_boundary(end);
}

std::string Rope::slice(std::size_t begin, std::size_t end) const {
    require_range(begin, end);
    std::string result;
    result.reserve(end - begin);
    if (begin == end) {
        return result;
    }
    const auto first = tree_.seek(begin, &TextSummary::bytes);
    const auto last = tree_.seek(end, &TextSummary::bytes);
    auto current_byte = first.prefix.bytes;
    const auto last_index = last.index + (end > last.prefix.bytes ? 1U : 0U);
    tree_.for_each_range(first.index, last_index, [&](const std::string& chunk) {
        const auto local_begin = std::max(begin, current_byte) - current_byte;
        const auto local_end = std::min(end - current_byte, chunk.size());
        result.append(chunk, local_begin, local_end - local_begin);
        current_byte += chunk.size();
    });
    return result;
}

std::pair<Rope, Rope> Rope::split(std::size_t byte_offset) const {
    require_boundary(byte_offset);
    const auto found = tree_.seek(byte_offset, &TextSummary::bytes);
    auto [before, after] = tree_.split(found.index);
    const auto local_offset = byte_offset - found.prefix.bytes;
    if (local_offset == 0) {
        return {Rope(std::move(before)), Rope(std::move(after))};
    }
    auto [first, remainder] = after.split(1);
    const auto& chunk = first.at(0);
    before = before.concat(Tree::singleton(chunk.substr(0, local_offset)));
    after = Tree::singleton(chunk.substr(local_offset)).concat(remainder);
    return {Rope(std::move(before)), Rope(std::move(after))};
}

Rope Rope::concat(const Rope& other) const {
    if (empty()) {
        return other;
    }
    if (other.empty()) {
        return *this;
    }
    const auto& left_chunk = tree_.at(tree_.size() - 1);
    const auto& right_chunk = other.tree_.at(0);
    if (left_chunk.size() + right_chunk.size() <= max_chunk_bytes) {
        auto [before, last] = tree_.split(tree_.size() - 1);
        auto [first, after] = other.tree_.split(1);
        auto middle = Tree::singleton(left_chunk + right_chunk);
        return Rope(before.concat(middle).concat(after));
    }
    return Rope(tree_.concat(other.tree_));
}

Rope Rope::replace(std::size_t begin, std::size_t end, std::string_view replacement) const {
    require_range(begin, end);
    Rope inserted(replacement);
    if (begin == end && inserted.empty()) {
        return *this;
    }
    auto [before, remainder] = split(begin);
    auto [removed, after] = remainder.split(end - begin);
    return before.concat(inserted).concat(after);
}

std::size_t Rope::byte_to_utf16(std::size_t byte_offset) const {
    require_boundary(byte_offset);
    const auto found = tree_.seek(byte_offset, &TextSummary::bytes);
    if (!found.item) {
        return found.prefix.utf16_units;
    }
    return found.prefix.utf16_units +
           utf8::utf16_length(std::string_view(*found.item).substr(
               0, byte_offset - found.prefix.bytes));
}

std::size_t Rope::utf16_to_byte(std::size_t utf16_offset) const {
    const auto found = tree_.seek(utf16_offset, &TextSummary::utf16_units);
    if (!found.item) {
        return found.prefix.bytes;
    }
    return found.prefix.bytes + utf8::byte_offset_from_utf16(
                                    *found.item, utf16_offset - found.prefix.utf16_units);
}

TextPosition Rope::byte_to_position(std::size_t byte_offset) const {
    require_boundary(byte_offset);
    const auto found = tree_.seek(byte_offset, &TextSummary::bytes);
    auto prefix = found.prefix;
    if (found.item) {
        prefix = prefix + MeasureText{}(std::string_view(*found.item).substr(
                              0, byte_offset - found.prefix.bytes));
    }
    return {prefix.line_breaks, prefix.trailing_utf16};
}

std::size_t Rope::position_to_byte(TextPosition position) const {
    const auto start = line_start(position.line);
    const auto physical_end = position.line + 1 < line_count()
                                  ? line_start(position.line + 1) - 1
                                  : size_bytes();
    const auto first_unit = byte_to_utf16(start);
    const auto line_units = byte_to_utf16(physical_end) - first_unit;
    if (position.utf16_column > line_units) {
        throw std::out_of_range("UTF-16 column is outside the line");
    }
    return utf16_to_byte(first_unit + position.utf16_column);
}

std::size_t Rope::line_start(std::size_t line) const {
    if (line >= line_count()) {
        throw std::out_of_range("Rope line is outside the text");
    }
    if (line == 0) {
        return 0;
    }
    const auto found = tree_.seek(line - 1, &TextSummary::line_breaks);
    auto remaining = line - 1 - found.prefix.line_breaks;
    for (std::size_t index = 0; index < found.item->size(); ++index) {
        if ((*found.item)[index] == '\n') {
            if (remaining == 0) {
                return found.prefix.bytes + index + 1;
            }
            --remaining;
        }
    }
    throw std::logic_error("Rope line summary does not match its text");
}

char Rope::byte_at(std::size_t byte_offset) const {
    if (byte_offset >= size_bytes()) {
        throw std::out_of_range("Rope byte offset is outside the text");
    }
    const auto found = tree_.seek(byte_offset, &TextSummary::bytes);
    return (*found.item)[byte_offset - found.prefix.bytes];
}

std::size_t Rope::line_end(std::size_t line) const {
    const auto start = line_start(line);
    if (line + 1 == line_count()) {
        return size_bytes();
    }
    auto end = line_start(line + 1) - 1;
    if (end > start && byte_at(end - 1) == '\r') {
        --end;
    }
    return end;
}

std::size_t Rope::chunk_count() const noexcept { return tree_.size(); }

std::size_t Rope::tree_height() const noexcept { return tree_.height(); }

bool Rope::validate() const {
    if (!tree_.validate()) {
        return false;
    }
    bool valid = true;
    tree_.for_each([&valid](const std::string& chunk) {
        valid = valid && !chunk.empty() && chunk.size() <= max_chunk_bytes &&
                utf8::is_valid(chunk);
    });
    return valid;
}

} // namespace astra
