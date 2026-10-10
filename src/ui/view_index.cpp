#include <astra/ui/view_index.hpp>
#include <astra/core/utf8.hpp>
#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace astra::ui {
namespace {
using Json = protocol::Json;

std::string field(const Json& row, const char* name, std::size_t limit) {
    const auto value = row.value(name, std::string{});
    if (value.size() > limit || value.find_first_of("\r\n") != value.npos ||
        value.find('\0') != value.npos)
        throw protocol::RpcError(-32602, "Invalid single-line view field");
    utf8::validate(value);
    return value;
}

ViewRow parse(const Json& value, std::size_t ordinal) {
    if (!value.is_object())
        throw protocol::RpcError(-32602, "View rows must be objects");
    for (const auto& [key, unused] : value.items()) {
        static_cast<void>(unused);
        if (key != "id" && key != "label" && key != "description" && key != "badge" &&
            key != "icon" && key != "tone" && key != "depth" && key != "group" &&
            key != "selected" && key != "data")
            throw protocol::RpcError(-32602, "Unknown view row field");
    }
    if (field(value, "id", 4096).empty() || field(value, "label", 4096).empty())
        throw protocol::RpcError(-32602, "View row identity and label required");
    static_cast<void>(field(value, "description", 1024));
    static_cast<void>(field(value, "badge", 64));
    const auto icon = field(value, "icon", 32), tone = field(value, "tone", 32);
    if (!icon.empty() && icon != "files" && icon != "search" && icon != "branch" &&
        icon != "folder" && icon != "document" && icon != "code" && icon != "braces" &&
        icon != "symbol" && icon != "function" && icon != "warning")
        throw protocol::RpcError(-32602, "Unknown view icon");
    if (!tone.empty() && tone != "normal" && tone != "muted" && tone != "added" &&
        tone != "changed" && tone != "error")
        throw protocol::RpcError(-32602, "Unknown view row tone");
    const auto depth = value.value("depth", Json(0));
    if (!depth.is_number_integer() ||
        (!depth.is_number_unsigned() && depth.get<std::int64_t>() < 0) ||
        depth.get<std::uint64_t>() > 64)
        throw protocol::RpcError(-32602, "View depth must be 0..64");
    if (value.contains("data") && value.at("data").dump().size() > 8192)
        throw protocol::RpcError(-32602, "View row data exceeds 8 KiB");
    static_cast<void>(value.value("selected", false));
    return {std::make_shared<const Json>(value),
            ordinal,
            depth.get<std::size_t>(),
            value.value("group", false)};
}
} // namespace

ViewBuilder::ViewBuilder() : source_(std::make_unique<ViewSource>()) {
}

void ViewBuilder::append(const Json& values) {
    if (!source_)
        throw protocol::RpcError(-32004, "View builder already finished");
    if (!values.is_array() || values.size() > max_page_rows)
        throw protocol::RpcError(-32602, "View page exceeds 256 rows");
    const auto bytes = values.dump().size();
    if (bytes > max_page_bytes || bytes > max_bytes - source_->bytes ||
        values.size() > max_rows - source_->rows.size())
        throw protocol::RpcError(-32004, "View data exceeds its bounded capacity");
    std::vector<ViewRow> rows;
    std::map<std::string, std::size_t> positions;
    rows.reserve(values.size());
    for (const auto& value : values) {
        auto row = parse(value, source_->rows.size() + rows.size());
        const auto id = value.at("id").get<std::string>();
        if (source_->positions.contains(id) || !positions.emplace(id, row.ordinal).second)
            throw protocol::RpcError(-32602, "Duplicate view row identity");
        if (row.ordinal == 0) {
            if (row.depth != 0)
                throw protocol::RpcError(-32602, "First view row must be a root");
        } else {
            const auto& previous = rows.empty() ? source_->rows.at(row.ordinal - 1) : rows.back();
            if (row.depth > previous.depth && (row.depth != previous.depth + 1 || !previous.group))
                throw protocol::RpcError(-32602, "View children require an adjacent parent group");
        }
        rows.push_back(std::move(row));
    }
    auto prepared = source_->rows.concat(ViewSnapshot(std::move(rows)));
    // All allocations/validation precede publication; merging map nodes allocates nothing.
    source_->positions.merge(positions);
    source_->rows = std::move(prepared);
    source_->bytes += bytes;
}

std::shared_ptr<const ViewSource> ViewBuilder::finish() && {
    if (!source_)
        throw protocol::RpcError(-32004, "View builder already finished");
    return std::shared_ptr<const ViewSource>(std::move(source_));
}

ViewIndex::ViewIndex(std::shared_ptr<const ViewSource> source, std::set<std::string> collapsed)
    : source_(std::move(source)), collapsed_(std::move(collapsed)) {
    if (collapsed_.size() > 4096)
        throw protocol::RpcError(-32004, "View collapse state exceeds its bound");
    if (!source_) {
        collapsed_.clear();
        return;
    }
    std::erase_if(collapsed_, [&](const auto& id) {
        const auto found = source_->positions.find(id);
        return found == source_->positions.end() || !source_->rows.at(found->second).group;
    });
}

std::size_t ViewIndex::subtree_end(const ViewRow& row) const {
    const auto remainder = source_->rows.split(row.ordinal + 1).second;
    return row.ordinal + 1 +
           remainder
               .seek_prefix([&](const ViewSummary& prefix) {
                   return prefix.minimum_depth <= row.depth;
               })
               .index;
}

bool ViewIndex::filter_step(std::size_t budget) {
    if (budget == 0 || budget > 256)
        throw std::invalid_argument("View filter budget must be 1..256");
    if (ready())
        return true;
    if (collapsed_.empty()) {
        visible_ = source_->rows;
        cursor_ = source_->rows.size();
        return true;
    }
    std::vector<ViewRow> rows;
    auto cursor = cursor_;
    for (std::size_t work = 0; work < budget && cursor < source_->rows.size(); ++work) {
        const auto& row = source_->rows.at(cursor);
        rows.push_back(row);
        cursor = row.group && collapsed(row.value->at("id").get<std::string>()) ? subtree_end(row)
                                                                                : cursor + 1;
    }
    auto prepared = visible_.concat(ViewSnapshot(std::move(rows)));
    visible_ = std::move(prepared);
    cursor_ = cursor;
    return ready();
}

ViewSnapshot
ViewIndex::filtered_range(std::size_t begin, std::size_t end, std::string_view exclude) const {
    auto result = source_->rows.split(begin).second.split(end - begin).first;
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    for (const auto& id : collapsed_) {
        if (id == exclude)
            continue;
        const auto& row = source_->rows.at(source_->positions.at(id));
        const auto first = std::max(begin, row.ordinal + 1), last = std::min(end, subtree_end(row));
        if (first < last)
            ranges.emplace_back(first, last);
    }
    std::ranges::sort(ranges);
    std::vector<std::pair<std::size_t, std::size_t>> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, range.second);
        else
            merged.push_back(range);
    }
    for (auto item = merged.rbegin(); item != merged.rend(); ++item)
        result = result.erase(item->first - begin, item->second - begin);
    return result;
}

void ViewIndex::toggle(std::size_t index) {
    if (!ready())
        throw protocol::RpcError(-32004, "View projection is not ready");
    const auto row = visible_.at(index);
    if (!row.group)
        throw protocol::RpcError(-32602, "Only view groups can be collapsed");
    const auto id = row.value->at("id").get<std::string>();
    const auto end = subtree_end(row);
    if (collapsed(id)) {
        auto [before, after] = visible_.split(index + 1);
        auto expanded = before.concat(filtered_range(row.ordinal + 1, end, id)).concat(after);
        collapsed_.erase(id);
        visible_ = std::move(expanded);
    } else {
        if (collapsed_.size() >= 4096)
            throw protocol::RpcError(-32004, "View collapse state exceeds its bound");
        const auto last = visible_
                              .seek_prefix([&](const ViewSummary& prefix) {
                                  return prefix.last_source > end;
                              })
                              .index;
        auto hidden = visible_.erase(index + 1, last);
        collapsed_.insert(id);
        visible_ = std::move(hidden);
    }
}
} // namespace astra::ui
