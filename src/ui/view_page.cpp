#include <astra/ui/view_page.hpp>
#include <astra/core/utf8.hpp>
#include <limits>
#include <utility>

namespace astra::ui {
using Json = protocol::Json;

namespace {
std::size_t count(const Json& value, const char* field) {
    const auto& number = value.at(field);
    if (!number.is_number_integer() ||
        (!number.is_number_unsigned() && number.get<std::int64_t>() < 0) ||
        number.get<std::uint64_t>() > 9007199254740991ULL)
        throw protocol::RpcError(-32602, "View page counts must be safe nonnegative integers");
    return number.get<std::size_t>();
}
} // namespace

void ViewPage::sync(Request request) {
    if (request.limit > ViewBuilder::max_page_rows ||
        (request.visible && (request.id.empty() || request.revision.empty() || !request.limit)))
        throw protocol::RpcError(-32602, "Invalid view page viewport");
    if (request != request_)
        invalidate(std::move(request));
}

void ViewPage::invalidate(Request request) {
    if (version_ == std::numeric_limits<std::uint64_t>::max())
        throw protocol::RpcError(-32004, "View page generation exhausted");
    ViewIndex empty;
    const auto same_content = request.id == request_.id && request.revision == request_.revision;
    request_ = std::move(request);
    rows_ = std::move(empty);
    message_.clear();
    if (!same_content)
        total_ = 0;
    ++version_;
    dirty_ = true;
    pending_ = request_.visible;
}

void ViewPage::refresh() {
    invalidate(request_);
}

Json ViewPage::identity() const {
    return {{"version", std::to_string(version_)},
            {"id", request_.id},
            {"revision", request_.revision},
            {"activity", request_.activity},
            {"offset", request_.offset},
            {"limit", request_.limit},
            {"visible", request_.visible}};
}

std::optional<Json> ViewPage::take_request() {
    if (!dirty_)
        return std::nullopt;
    auto result = identity();
    dirty_ = false;
    return result;
}

bool ViewPage::publish(const Json& params) {
    if (!request_.visible || !pending_ || params.at("version") != std::to_string(version_) ||
        params.at("id") != request_.id || params.at("revision") != request_.revision ||
        params.at("activity") != request_.activity)
        return false;
    for (const auto& [field, unused] : params.items()) {
        static_cast<void>(unused);
        if (field != "version" && field != "id" && field != "revision" && field != "activity" &&
            field != "offset" && field != "total" && field != "rows" && field != "empty" &&
            field != "pending")
            throw protocol::RpcError(-32602, "Unknown view page publication field");
    }
    auto message = params.value("empty", std::string{});
    if (message.size() > 4096 || message.find_first_of("\r\n") != message.npos ||
        message.find('\0') != message.npos)
        throw protocol::RpcError(-32602, "Invalid view page message");
    utf8::validate(message);
    if (params.value("pending", false)) {
        if (!pending_ || params.contains("rows") || params.contains("offset") ||
            params.contains("total"))
            throw protocol::RpcError(-32602, "Pending view page cannot replace published content");
        message_.swap(message);
        return true;
    }
    const auto offset = count(params, "offset"), total = count(params, "total");
    const auto& values = params.at("rows");
    if (!values.is_array() || offset > total || offset > request_.offset ||
        (offset != request_.offset && request_.offset < total) ||
        values.size() != std::min(request_.limit, total - offset))
        throw protocol::RpcError(-32602, "View page does not cover its requested viewport");
    auto prepared = ViewIndex::page(values);
    rows_ = std::move(prepared);
    message_.swap(message);
    request_.offset = offset;
    total_ = total;
    pending_ = false;
    return true;
}

Json ViewPage::state() const {
    auto result = identity();
    result["total"] = total_;
    result["pending"] = pending_;
    result["empty"] = message_;
    result["rows"] = Json::array();
    rows_.for_each_range(0, rows_.size(), [&](std::size_t, const ViewRow& row) {
        result["rows"].push_back(*row.value);
    });
    return result;
}

Json ViewPage::activate(std::string_view version, std::string_view row) const {
    if (pending_ || !request_.visible || version != std::to_string(version_))
        return nullptr;
    const auto position = rows_.position(row);
    if (!position)
        return nullptr;
    return {{"request", identity()}, {"row", *rows_.at(*position).value}};
}
} // namespace astra::ui
