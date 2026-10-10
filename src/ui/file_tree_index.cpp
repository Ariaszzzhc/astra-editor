#include <astra/ui/file_tree_index.hpp>

#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
bool descendant(std::string_view path, std::string_view parent) {
    return path.size() > parent.size() && path.starts_with(parent) &&
           (path[parent.size()] == '/' || path[parent.size()] == '\\');
}
} // namespace

void FileTreeIndex::reset(const protocol::Json& entries) {
    if (!entries.is_array()) {
        throw std::invalid_argument("File tree entries must be an array");
    }
    std::vector<Row> rows;
    std::vector<std::pair<std::string, std::size_t>> ancestors;
    std::map<std::string, std::size_t> positions;
    rows.reserve(entries.size());
    for (const auto& value : entries) {
        const auto path = value.at("path").get<std::string>();
        if (path.empty() || !positions.emplace(path, rows.size()).second) {
            throw std::invalid_argument("Invalid or duplicate file tree path");
        }
        while (!ancestors.empty() && !descendant(path, ancestors.back().first)) {
            rows[ancestors.back().second].subtree_end = rows.size();
            ancestors.pop_back();
        }
        rows.push_back(
            {std::make_shared<const protocol::Json>(value), rows.size(), rows.size() + 1});
        if (value.at("directory").get<bool>()) {
            ancestors.emplace_back(path, rows.size() - 1);
        }
    }
    for (const auto& ancestor : ancestors) {
        rows[ancestor.second].subtree_end = rows.size();
    }
    Snapshot source(std::move(rows));
    auto collapsed = collapsed_;
    std::erase_if(collapsed, [&](const auto& path) {
        const auto found = positions.find(path);
        return found == positions.end() ||
               !source.at(found->second).value->at("directory").template get<bool>();
    });
    FileTreeIndex replacement;
    replacement.source_ = std::move(source);
    replacement.positions_ = std::move(positions);
    replacement.collapsed_ = std::move(collapsed);
    replacement.visible_ = replacement.filtered_range(0, replacement.source_.size());
    *this = std::move(replacement);
}

FileTreeIndex::Snapshot
FileTreeIndex::filtered_range(std::size_t begin, std::size_t end, std::string_view exclude) const {
    auto [before, remainder] = source_.split(begin);
    static_cast<void>(before);
    auto [result, after] = remainder.split(end - begin);
    static_cast<void>(after);
    std::vector<std::pair<std::size_t, std::size_t>> hidden;
    for (const auto& path : collapsed_) {
        if (path == exclude)
            continue;
        const auto& row = source_.at(positions_.at(path));
        const auto first = std::max(begin, row.ordinal + 1);
        const auto last = std::min(end, row.subtree_end);
        if (first < last) {
            hidden.emplace_back(first, last);
        }
    }
    std::ranges::sort(hidden);
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (const auto& range : hidden) {
        if (!ranges.empty() && range.first <= ranges.back().second) {
            ranges.back().second = std::max(ranges.back().second, range.second);
        } else {
            ranges.push_back(range);
        }
    }
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        result = result.erase(it->first - begin, it->second - begin);
    }
    return result;
}

void FileTreeIndex::toggle(std::size_t index) {
    const auto row = visible_.at(index);
    if (!row.value->at("directory").get<bool>()) {
        throw std::invalid_argument("Only a directory can be collapsed");
    }
    const auto path = row.value->at("path").get<std::string>();
    if (collapsed_.contains(path)) {
        const auto restored = filtered_range(row.ordinal + 1, row.subtree_end, path);
        auto [before, after] = visible_.split(index + 1);
        auto expanded = before.concat(restored).concat(after);
        collapsed_.erase(path);
        visible_ = std::move(expanded);
    } else {
        const auto end = visible_
                             .seek_prefix([&](const Summary& prefix) {
                                 return prefix.last_source > row.subtree_end;
                             })
                             .index;
        auto hidden = visible_.erase(index + 1, end);
        collapsed_.insert(path);
        visible_ = std::move(hidden);
    }
}
} // namespace astra::ui
