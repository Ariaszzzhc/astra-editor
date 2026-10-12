#pragma once

#include <astra/protocol/rpc.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace astra::ui {
// Owner-thread publication fence for a nonmodal editor text tooltip. Geometry
// and document coordinates come from the native viewport; contents come from JS.
class EditorTooltip final {
  public:
    void sync(protocol::Json target);
    [[nodiscard]] std::optional<protocol::Json> take_request();
    [[nodiscard]] bool publish(const protocol::Json&);
    [[nodiscard]] protocol::Json state() const;
    [[nodiscard]] const protocol::Json& target() const noexcept;
    [[nodiscard]] const std::string& text() const noexcept;
    [[nodiscard]] bool pending() const noexcept;

  private:
    protocol::Json target_;
    std::string text_;
    std::uint64_t version_{};
    bool changed_{}, pending_{};
};
} // namespace astra::ui
