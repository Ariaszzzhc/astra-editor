#include <astra/ui/viewport_index.hpp>
#include <astra/core/utf8.hpp>

#include <stdexcept>
#include <utility>

namespace astra::ui {

ViewportSummary operator+(const ViewportSummary& left, const ViewportSummary& right) {
    if (!left.rows)
        return right;
    if (!right.rows)
        return left;
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (right.rows > maximum - left.rows || right.utf8_bytes > maximum - left.utf8_bytes)
        throw std::length_error("Viewport summary overflow");
    return {left.rows + right.rows,
            left.utf8_bytes + right.utf8_bytes,
            left.first_line,
            right.last_line,
            left.begin,
            right.end};
}

ViewportSummary MeasureViewportLine::operator()(const ViewportLine& line) const noexcept {
    return {1, line.text.size(), line.number, line.number, line.begin, line.end};
}

void ViewportIndex::reset(std::vector<ViewportLine> lines) {
    if (lines.size() > 256)
        throw std::length_error("Document viewport exceeds 256 rows");
    const ViewportLine* previous = nullptr;
    for (const auto& line : lines) {
        if (!line.complete || line.begin > line.end || line.end > line.next ||
            line.text.size() != line.end - line.begin)
            throw std::invalid_argument("Viewport requires complete UTF-8 line extents");
        utf8::validate(line.text);
        if (previous && (previous->number == std::numeric_limits<std::size_t>::max() ||
                         line.number != previous->number + 1 || line.begin != previous->next))
            throw std::invalid_argument("Viewport lines must be contiguous and ordered");
        previous = &line;
    }
    const auto same = [&](std::size_t index) {
        const auto* retained = find(lines[index].number);
        return retained && *retained == lines[index];
    };
    Tree next;
    for (std::size_t begin = 0; begin < lines.size();) {
        const auto retained = same(begin);
        auto end = begin + 1;
        while (end < lines.size() && same(end) == retained)
            ++end;
        if (retained) {
            const auto ordinal = lines[begin].number - summary().first_line;
            next = next.concat(tree_.split(ordinal).second.split(end - begin).first);
        } else {
            std::vector<ViewportLine> changed;
            changed.reserve(end - begin);
            for (auto index = begin; index < end; ++index)
                changed.push_back(std::move(lines[index]));
            next = next.concat(Tree(std::move(changed)));
        }
        begin = end;
    }
    tree_ = std::move(next);
}

bool ViewportIndex::empty() const noexcept {
    return tree_.empty();
}

std::size_t ViewportIndex::size() const noexcept {
    return tree_.size();
}

const ViewportSummary& ViewportIndex::summary() const noexcept {
    return tree_.summary();
}

const ViewportLine* ViewportIndex::find(std::size_t number) const {
    if (empty() || number < summary().first_line || number - summary().first_line >= size())
        return nullptr;
    return &tree_.at(number - summary().first_line);
}

bool ViewportIndex::validate() const {
    return tree_.validate();
}

ViewportIndex ViewportIndex::row_range(std::size_t begin, std::size_t end) const {
    if (begin > end || end > size())
        throw std::out_of_range("Viewport row range exceeds the published page");
    ViewportIndex result;
    result.tree_ = tree_.split(begin).second.split(end - begin).first;
    return result;
}

std::size_t ViewportIndex::shared_prefix_rows(const ViewportIndex& other) const {
    return tree_.shared_prefix_summary(other.tree_).rows;
}

std::size_t ViewportIndex::shared_suffix_rows(const ViewportIndex& other) const {
    return tree_.shared_suffix_summary(other.tree_).rows;
}

} // namespace astra::ui
