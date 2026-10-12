#include <astra/ui/editor_link.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace astra::ui {
void EditorLink::sync(protocol::Json target) {
    if (target == target_) {
        return;
    }
    if (version_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("Editor link version exhausted");
    }
    target_ = std::move(target);
    ++version_;
    changed_ = true;
    available_ = false;
    error_.clear();
    pending_ = !target_.is_null();
}

std::optional<protocol::Json> EditorLink::take_request() {
    if (!std::exchange(changed_, false)) {
        return std::nullopt;
    }
    return protocol::Json{{"version", std::to_string(version_)}, {"target", target_}};
}

bool EditorLink::publish(const protocol::Json& value) {
    if (!value.is_object() || value.size() != 3 || !value.contains("version") ||
        !value.at("version").is_string() || !value.contains("available") ||
        !value.at("available").is_boolean() || !value.contains("error") ||
        !value.at("error").is_string()) {
        throw protocol::RpcError(-32602, "Link publication requires version, available and error");
    }
    if (target_.is_null() || value.at("version") != std::to_string(version_)) {
        return false;
    }
    const auto& error = value.at("error").get_ref<const std::string&>();
    if (error.size() > 4096) {
        throw protocol::RpcError(-32602, "Link error exceeds its bound");
    }
    error_ = error;
    available_ = value.at("available").get<bool>();
    pending_ = false;
    return true;
}

protocol::Json EditorLink::state() const {
    return {{"version", std::to_string(version_)},
            {"target", target_},
            {"available", available_},
            {"pending", pending_},
            {"error", error_}};
}

const protocol::Json& EditorLink::target() const noexcept {
    return target_;
}

bool EditorLink::available() const noexcept {
    return available_;
}

bool EditorLink::pending() const noexcept {
    return pending_;
}
} // namespace astra::ui
