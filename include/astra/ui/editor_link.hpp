#pragma once

#include <astra/protocol/rpc.hpp>
#include <cstdint>
#include <optional>
#include <string>

namespace astra::ui {
// A definition affordance is published only for the current Ctrl-hover target.
class EditorLink final {
  public:
    void sync(protocol::Json target);
    [[nodiscard]] std::optional<protocol::Json> take_request();
    [[nodiscard]] bool publish(const protocol::Json&);
    [[nodiscard]] protocol::Json state() const;
    [[nodiscard]] const protocol::Json& target() const noexcept;
    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool pending() const noexcept;

  private:
    protocol::Json target_;
    std::uint64_t version_{};
    bool changed_{}, available_{}, pending_{};
    std::string error_;
};
} // namespace astra::ui
