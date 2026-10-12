#include <astra/ui/editor_tooltip.hpp>
#include <astra/core/utf8.hpp>

#include <limits>
#include <stdexcept>
#include <utility>

namespace astra::ui {
void EditorTooltip::sync(protocol::Json target) {
    if (target == target_) {
        return;
    }
    if (version_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("Editor tooltip version exhausted");
    }
    target_ = std::move(target);
    text_.clear();
    ++version_;
    changed_ = true;
    pending_ = !target_.is_null();
}

std::optional<protocol::Json> EditorTooltip::take_request() {
    if (!std::exchange(changed_, false)) {
        return std::nullopt;
    }
    return protocol::Json{{"version", std::to_string(version_)}, {"target", target_}};
}

bool EditorTooltip::publish(const protocol::Json& value) {
    if (!value.is_object() || value.size() != 2 || !value.contains("version") ||
        !value.at("version").is_string() || !value.contains("text") ||
        !value.at("text").is_string()) {
        throw protocol::RpcError(-32602, "Tooltip publication requires version and text");
    }
    if (target_.is_null() || value.at("version") != std::to_string(version_)) {
        return false;
    }
    const auto& text = value.at("text").get_ref<const std::string&>();
    if (text.size() > 16 * 1024 || text.find('\0') != text.npos) {
        throw protocol::RpcError(-32602, "Tooltip text exceeds its 16 KiB bound");
    }
    utf8::validate(text);
    text_ = text;
    pending_ = false;
    return true;
}

protocol::Json EditorTooltip::state() const {
    return {{"version", std::to_string(version_)},
            {"target", target_},
            {"text", text_},
            {"pending", pending_}};
}

const protocol::Json& EditorTooltip::target() const noexcept {
    return target_;
}

const std::string& EditorTooltip::text() const noexcept {
    return text_;
}

bool EditorTooltip::pending() const noexcept {
    return pending_;
}
} // namespace astra::ui
