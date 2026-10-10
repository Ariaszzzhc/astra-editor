#include <astra/ui/view_store.hpp>
#include <astra/core/utf8.hpp>
#include <algorithm>
#include <limits>
#include <utility>

namespace astra::ui {
namespace {
using Json = protocol::Json;

void text(std::string_view value, std::size_t limit) {
    if (value.size() > limit || value.find_first_of("\r\n") != value.npos ||
        value.find('\0') != value.npos)
        throw protocol::RpcError(-32602, "Invalid view text");
    utf8::validate(value);
}

std::future<Json> ready(bool value) {
    std::promise<Json> completion;
    completion.set_value(value);
    return completion.get_future();
}
} // namespace

ViewStore::ViewStore(std::size_t retirement_capacity, std::size_t byte_capacity)
    : retirement_(retirement_capacity, byte_capacity) {
}

ViewStore::~ViewStore() {
    shutdown();
}

void ViewStore::register_spec(const Json& spec) {
    if (!spec.is_object())
        throw protocol::RpcError(-32602, "View spec must be an object");
    const auto id = spec.at("id").get<std::string>(), title = spec.at("title").get<std::string>();
    text(id, 128);
    text(title, 128);
    const auto outline_title = spec.value("outlineTitle", std::string("Outline"));
    text(outline_title, 128);
    if (id.empty() || title.empty())
        throw protocol::RpcError(-32602, "View identity and title required");
    const auto icon = spec.value("icon", std::string("files"));
    if (icon != "files" && icon != "search" && icon != "branch" && icon != "braces" &&
        icon != "symbol")
        throw protocol::RpcError(-32602, "Unknown activity icon");
    const auto order = spec.value("order", 0);
    if (order < 0 || order > 31)
        throw protocol::RpcError(-32602, "View order must be 0..31");
    if (!views_.contains(id) && views_.size() >= 16)
        throw protocol::RpcError(-32004, "View capacity exceeded");
    Json prepared{{"id", id},
                  {"title", title},
                  {"icon", icon},
                  {"order", order},
                  {"outline", spec.value("outline", false)},
                  {"outlineTitle", outline_title}};
    if (auto found = views_.find(id); found != views_.end())
        found->second.spec = std::move(prepared);
    else {
        RetainedView view;
        view.spec = std::move(prepared);
        views_.emplace(id, std::move(view));
    }
}

std::string ViewStore::begin(const Json& spec) {
    if (stopped_)
        throw protocol::RpcError(-32004, "View store is shutting down");
    if (sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw protocol::RpcError(-32004, "View refresh identity exhausted");
    auto replacement = std::make_unique<Pending>(retirement_.reserve());
    replacement->version = std::to_string(++sequence_);
    const auto version = replacement->version;
    register_spec(spec);
    const auto id = spec.at("id").get<std::string>();
    if (auto found = pending_.find(id); found != pending_.end()) {
        if (found->second->committing)
            found->second->completion.set_value(false);
        retire(*found->second);
        found->second = std::move(replacement);
    } else
        pending_.emplace(id, std::move(replacement));
    return version;
}

ViewStore::Pending* ViewStore::pending(std::string_view id, std::string_view version) {
    const auto found = pending_.find(id);
    return found != pending_.end() && found->second->version == version ? found->second.get()
                                                                        : nullptr;
}

bool ViewStore::append(std::string_view id, std::string_view version, const Json& rows) {
    auto* stage = pending(id, version);
    if (!stage)
        return false;
    if (stage->committing)
        throw protocol::RpcError(-32004, "View refresh already committing");
    const auto bytes = rows.dump().size();
    stage->ownership.charge(bytes);
    try {
        stage->builder.append(rows);
    } catch (...) {
        stage->ownership.undo_charge(bytes);
        throw;
    }
    return true;
}

std::future<Json> ViewStore::commit(std::string_view id,
                                    std::string_view version,
                                    std::string subtitle,
                                    std::string empty) {
    auto* stage = pending(id, version);
    if (!stage)
        return ready(false);
    if (stage->committing)
        throw protocol::RpcError(-32004, "View refresh already committing");
    text(subtitle, 1024);
    text(empty, 1024);
    stage->source = std::move(stage->builder).finish();
    auto& view = at(id);
    stage->projection.emplace(stage->source, view.index.collapsed());
    stage->fold_generation = view.fold_generation;
    stage->subtitle = std::move(subtitle);
    stage->empty = std::move(empty);
    stage->committing = true;
    return stage->completion.get_future();
}

bool ViewStore::abort(std::string_view id, std::string_view version) {
    if (auto* stage = pending(id, version)) {
        if (stage->committing)
            stage->completion.set_value(false);
        retire(*stage);
        pending_.erase(std::string(id));
        return true;
    }
    return false;
}

void ViewStore::cancel_all() {
    for (auto& [id, stage] : pending_) {
        static_cast<void>(id);
        if (stage->committing)
            stage->completion.set_value(false);
        retire(*stage);
    }
    pending_.clear();
}

void ViewStore::retire(Pending& stage) noexcept {
    ViewRetirement::Payload payload;
    payload.builder.emplace(std::move(stage.builder));
    payload.source = std::move(stage.source);
    payload.projection = std::move(stage.projection);
    retirement_.retire(std::move(stage.ownership), std::move(payload));
}

void ViewStore::shutdown() {
    if (stopped_)
        return;
    stopped_ = true;
    cancel_all();
    for (auto& [id, ownership] : published_ownership_) {
        ViewRetirement::Payload payload;
        payload.projection.emplace(std::move(at(id).index));
        retirement_.retire(std::move(ownership), std::move(payload));
    }
    published_ownership_.clear();
}

void ViewStore::poll() {
    auto iterator = pending_.upper_bound(poll_after_);
    for (std::size_t visited = 0; visited < pending_.size(); ++visited, ++iterator) {
        if (iterator == pending_.end())
            iterator = pending_.begin();
        auto& stage = *iterator->second;
        if (!stage.committing)
            continue;
        poll_after_ = iterator->first;
        auto& view = at(iterator->first);
        try {
            if (stage.fold_generation != view.fold_generation) {
                auto ownership = retirement_.try_reserve();
                if (!ownership)
                    return; // Backpressure: keep the stage and old view live.
                ownership->charge(stage.source->bytes);
                ViewIndex replacement(stage.source, view.index.collapsed());
                ViewRetirement::Payload payload;
                payload.projection = std::move(stage.projection);
                stage.projection.emplace(std::move(replacement));
                retirement_.retire(std::move(*ownership), std::move(payload));
                stage.fold_generation = view.fold_generation;
            }
            if (stage.projection->filter_step(128)) {
                std::optional<std::string> next_cursor;
                if (view.cursor && stage.projection->size()) {
                    const auto position =
                        stage.projection->position(*view.cursor, true)
                            .value_or(std::min(cursor(iterator->first).value_or(0),
                                               stage.projection->size() - 1));
                    next_cursor = stage.projection->at(position).value->at("id").get<std::string>();
                }
                auto& ownership = published_ownership_.try_emplace(iterator->first).first->second;
                if (ownership) {
                    ViewRetirement::Payload payload;
                    payload.projection.emplace(std::move(view.index));
                    retirement_.retire(std::move(ownership), std::move(payload));
                }
                ownership = std::move(stage.ownership);
                view.index = std::move(*stage.projection);
                view.cursor = std::move(next_cursor);
                view.version = std::move(stage.version);
                view.subtitle = std::move(stage.subtitle);
                view.empty = std::move(stage.empty);
                view.scroll = std::min(view.scroll, view.index.size() ? view.index.size() - 1 : 0);
                stage.completion.set_value(true);
                pending_.erase(iterator);
            }
        } catch (...) {
            stage.completion.set_exception(std::current_exception());
            retire(stage);
            pending_.erase(iterator);
        }
        return; // At most one bounded projection page per owner pump.
    }
}

RetainedView& ViewStore::at(std::string_view id) {
    const auto found = views_.find(id);
    if (found == views_.end())
        throw protocol::RpcError(-32602, "Unknown view");
    return found->second;
}

const RetainedView& ViewStore::at(std::string_view id) const {
    const auto found = views_.find(id);
    if (found == views_.end())
        throw protocol::RpcError(-32602, "Unknown view");
    return found->second;
}

std::vector<std::string> ViewStore::ordered() const {
    std::vector<std::string> result;
    for (const auto& [id, view] : views_) {
        static_cast<void>(view);
        result.push_back(id);
    }
    std::ranges::sort(result, [&](const auto& left, const auto& right) {
        const auto a = at(left).spec.at("order").get<int>(),
                   b = at(right).spec.at("order").get<int>();
        return a == b ? left < right : a < b;
    });
    return result;
}

void ViewStore::toggle(std::string_view id, std::size_t row) {
    auto& view = at(id);
    if (view.fold_generation == std::numeric_limits<std::uint64_t>::max())
        throw protocol::RpcError(-32004, "View fold identity exhausted");
    auto ownership = retirement_.reserve();
    // A retired projection can outlive its source's own reservation. Charge its
    // shared JSON conservatively until this final owning edge is released too.
    ownership.charge(view.index.source_bytes());
    ViewRetirement::Payload payload;
    payload.rows = view.index.toggle(row);
    retirement_.retire(std::move(ownership), std::move(payload));
    ++view.fold_generation;
    if (view.cursor) {
        const auto position = view.index.position(*view.cursor, true);
        view.cursor =
            position ? std::optional(view.index.at(*position).value->at("id").get<std::string>())
                     : std::nullopt;
    }
}

std::optional<std::size_t> ViewStore::cursor(std::string_view id) const {
    const auto& view = at(id);
    return view.cursor ? view.index.position(*view.cursor) : std::nullopt;
}

Json ViewStore::state(std::string_view id) const {
    const auto& view = at(id);
    Json result{{"id", id},
                {"title", view.spec.at("title")},
                {"outlineTitle", view.spec.at("outlineTitle")},
                {"expanded", view.expanded},
                {"version", view.version},
                {"total", view.index.size()},
                {"scroll", view.scroll},
                {"row", nullptr},
                {"index", nullptr}};
    if (const auto position = cursor(id)) {
        const auto& row = view.index.at(*position);
        result["index"] = *position;
        result["row"] = *row.value;
        result["collapsed"] = row.group && view.index.collapsed(*view.cursor);
    }
    return result;
}

void ViewStore::reveal(std::string_view id, std::size_t page) {
    if (page == 0 || page > 256)
        throw protocol::RpcError(-32602, "View page must be 1..256 rows");
    auto& view = at(id);
    if (const auto position = cursor(id)) {
        if (*position < view.scroll)
            view.scroll = *position;
        else if (*position - view.scroll >= page)
            view.scroll = *position - page + 1;
    }
    view.scroll = std::min(view.scroll, view.index.size() ? view.index.size() - 1 : 0);
}

bool ViewStore::select(std::string_view id, std::string_view row, std::size_t page) {
    if (page == 0 || page > 256)
        throw protocol::RpcError(-32602, "View page must be 1..256 rows");
    auto& view = at(id);
    const auto position = view.index.position(row);
    if (!position)
        return false;
    view.cursor = std::string(row);
    reveal(id, page);
    return true;
}

bool ViewStore::reveal(std::string_view id, std::string_view row, std::size_t page) {
    if (page == 0 || page > 256)
        throw protocol::RpcError(-32602, "View page must be 1..256 rows");
    auto& view = at(id);
    auto next_cursor = std::optional(std::string(row));
    if (!view.index.position(row)) {
        if (!view.index.position(row, true))
            return false;
        if (view.fold_generation == std::numeric_limits<std::uint64_t>::max())
            throw protocol::RpcError(-32004, "View fold identity exhausted");
        auto ownership = retirement_.reserve();
        ownership.charge(view.index.source_bytes());
        auto prepared = view.index.revealing(row);
        ViewRetirement::Payload payload;
        payload.projection.emplace(std::move(view.index));
        view.index = std::move(*prepared);
        retirement_.retire(std::move(ownership), std::move(payload));
        ++view.fold_generation;
    }
    view.cursor = std::move(next_cursor);
    view.expanded = true;
    reveal(id, page);
    return true;
}

Json ViewStore::navigate(std::string_view id, std::string_view action, std::size_t page) {
    if (page == 0 || page > 256)
        throw protocol::RpcError(-32602, "View page must be 1..256 rows");
    if (action != "next" && action != "previous" && action != "first" && action != "last" &&
        action != "pageUp" && action != "pageDown" && action != "left" && action != "right" &&
        action != "toggle" && action != "activate" && action != "retain")
        throw protocol::RpcError(-32602, "Unknown view navigation action");
    auto& view = at(id);
    bool activate = false;
    if (view.index.size()) {
        auto position = cursor(id).value_or(0);
        const auto row = view.index.at(position);
        const auto row_id = row.value->at("id").get<std::string>();
        if (action == "next")
            position = std::min(position + 1, view.index.size() - 1);
        else if (action == "previous")
            position = position ? position - 1 : 0;
        else if (action == "first")
            position = 0;
        else if (action == "last")
            position = view.index.size() - 1;
        else if (action == "pageUp")
            position -= std::min(position, page);
        else if (action == "pageDown")
            position = std::min(position + page, view.index.size() - 1);
        else if (action == "left") {
            if (row.group && !view.index.collapsed(row_id))
                toggle(id, position);
            else
                position = view.index.parent(position).value_or(position);
        } else if (action == "right") {
            if (row.group && view.index.collapsed(row_id))
                toggle(id, position);
            else if (row.group && position + 1 < view.index.size() &&
                     view.index.at(position + 1).depth > row.depth)
                ++position;
        } else if (action == "toggle" || action == "activate") {
            if (row.group)
                toggle(id, position);
            else
                activate = action == "activate";
        }
        view.cursor = view.index.at(position).value->at("id").get<std::string>();
        view.expanded = true;
    }
    reveal(id, page);
    auto result = state(id);
    result["activate"] = activate;
    return result;
}
} // namespace astra::ui
