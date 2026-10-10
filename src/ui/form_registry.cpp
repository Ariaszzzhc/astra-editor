#include <astra/ui/form_registry.hpp>
#include <utility>

namespace astra::ui {
InlineForm* FormRegistry::find(std::string_view id) const noexcept {
    const auto found = forms_.find(id);
    return found == forms_.end() ? nullptr : found->second.get();
}

InlineForm* FormRegistry::mounted(std::string_view view) const noexcept {
    for (const auto& [id, form] : forms_) {
        static_cast<void>(id);
        if (form->view() == view)
            return form.get();
    }
    return nullptr;
}

InlineForm* FormRegistry::focused() const noexcept {
    for (const auto& [id, form] : forms_) {
        static_cast<void>(id);
        if (form->focus())
            return form.get();
    }
    return nullptr;
}

std::array<InlineForm*, 2> FormRegistry::visible(std::string_view view,
                                                 bool sidebar) const noexcept {
    return {mounted({}), sidebar && !view.empty() ? mounted(view) : nullptr};
}

std::unique_ptr<InlineForm> FormRegistry::open(std::unique_ptr<InlineForm> prepared) {
    if (!prepared)
        throw protocol::RpcError(-32602, "Missing form");
    if (const auto* duplicate = find(prepared->id());
        duplicate && duplicate->view() != prepared->view())
        throw protocol::RpcError(-32602, "Form identity already mounted elsewhere");
    const auto* previous = mounted(prepared->view());
    const auto old_id = previous ? previous->id() : std::string{};
    if (!previous && forms_.size() >= 17)
        throw protocol::RpcError(-32004, "Form capacity exceeded");
    const auto id = prepared->id();
    std::unique_ptr<InlineForm> retired;
    if (auto found = forms_.find(id); found != forms_.end()) {
        retired = std::move(found->second);
        found->second = std::move(prepared);
    } else {
        forms_.emplace(id, std::move(prepared));
        if (!old_id.empty())
            retired = close(old_id);
    }
    for (auto& [key, form] : forms_)
        if (key != id)
            form->focus(std::nullopt);
    return retired;
}

std::unique_ptr<InlineForm> FormRegistry::close(std::string_view id) {
    auto found = forms_.find(id);
    if (found == forms_.end())
        return {};
    auto result = std::move(found->second);
    forms_.erase(found);
    return result;
}

void FormRegistry::focus(std::string_view id, std::optional<std::size_t> field, bool select_all) {
    auto* target = find(id);
    if (!target)
        throw protocol::RpcError(-32602, "Unknown form");
    target->focus(field, select_all);
    for (auto& [key, form] : forms_)
        if (key != id)
            form->focus(std::nullopt);
}

void FormRegistry::blur() {
    for (auto& [id, form] : forms_) {
        static_cast<void>(id);
        form->focus(std::nullopt);
    }
}
} // namespace astra::ui
