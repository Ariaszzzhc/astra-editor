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

ViewStore::~ViewStore() {
    cancel_all();
}

void ViewStore::register_spec(const Json& spec) {
    if (!spec.is_object())
        throw protocol::RpcError(-32602, "View spec must be an object");
    const auto id = spec.at("id").get<std::string>(), title = spec.at("title").get<std::string>();
    text(id, 128);
    text(title, 128);
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
                  {"outline", spec.value("outline", false)}};
    if (auto found = views_.find(id); found != views_.end())
        found->second.spec = std::move(prepared);
    else {
        RetainedView view;
        view.spec = std::move(prepared);
        views_.emplace(id, std::move(view));
    }
}

std::string ViewStore::begin(const Json& spec) {
    if (sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw protocol::RpcError(-32004, "View refresh identity exhausted");
    auto replacement = std::make_unique<Pending>();
    replacement->version = std::to_string(++sequence_);
    const auto version = replacement->version;
    register_spec(spec);
    const auto id = spec.at("id").get<std::string>();
    if (auto found = pending_.find(id); found != pending_.end()) {
        if (found->second->committing)
            found->second->completion.set_value(false);
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
    stage->builder.append(rows);
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
    auto source = std::move(stage->builder).finish();
    auto& view = at(id);
    ViewIndex projection(source, view.index.collapsed());
    stage->source = std::move(source);
    stage->projection.emplace(std::move(projection));
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
    }
    pending_.clear();
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
                stage.projection.emplace(stage.source, view.index.collapsed());
                stage.fold_generation = view.fold_generation;
            }
            if (stage.projection->filter_step(128)) {
                view.index = std::move(*stage.projection);
                view.version = std::move(stage.version);
                view.subtitle = std::move(stage.subtitle);
                view.empty = std::move(stage.empty);
                view.scroll = std::min(view.scroll, view.index.size() ? view.index.size() - 1 : 0);
                stage.completion.set_value(true);
                pending_.erase(iterator);
            }
        } catch (...) {
            stage.completion.set_exception(std::current_exception());
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
    view.index.toggle(row);
    ++view.fold_generation;
}
} // namespace astra::ui
