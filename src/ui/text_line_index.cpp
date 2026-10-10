#include <astra/ui/text_line_index.hpp>
#include <limits>
#include <stdexcept>
#include <vector>
#include <utility>

namespace astra::ui {
TextLineSummary operator+(TextLineSummary left, TextLineSummary right) {
    if (right.rows > std::numeric_limits<std::size_t>::max() - left.rows ||
        right.bytes > std::numeric_limits<std::size_t>::max() - left.bytes)
        throw std::length_error("Text line summary overflow");
    return {left.rows + right.rows, left.bytes + right.bytes};
}

void TextLineIndex::reset(std::string_view text) {
    if (text.size() > 8 * 1024 * 1024)
        throw std::length_error("Text panel exceeds its 8 MiB limit");
    std::vector<IndexedTextLine> rows;
    std::size_t begin{};
    while (true) {
        if (rows.size() >= 100000)
            throw std::length_error("Text panel exceeds its 100,000 line limit");
        const auto ending = text.find_first_of("\r\n", begin);
        if (ending == text.npos) {
            rows.push_back({std::string(text.substr(begin)), text.size() - begin});
            break;
        }
        auto next = ending + 1;
        if (text[ending] == '\r' && next < text.size() && text[next] == '\n')
            ++next;
        rows.push_back({std::string(text.substr(begin, ending - begin)), next - begin});
        begin = next;
    }
    decltype(lines_) replacement(std::move(rows));
    lines_ = std::move(replacement);
}
} // namespace astra::ui
