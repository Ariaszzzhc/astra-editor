#pragma once

#include <astra/ui/inline_form.hpp>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace astra::ui {
// One editor overlay and one mounted control panel per registered view.
// Returned pointers borrow registry-owned widgets; only one widget owns focus.
class FormRegistry final {
  public:
    [[nodiscard]] InlineForm* find(std::string_view id) const noexcept;
    [[nodiscard]] InlineForm* mounted(std::string_view view) const noexcept;
    [[nodiscard]] InlineForm* focused() const noexcept;
    [[nodiscard]] std::array<InlineForm*, 2> visible(std::string_view view,
                                                     bool sidebar) const noexcept;
    [[nodiscard]] std::unique_ptr<InlineForm> open(std::unique_ptr<InlineForm> prepared);
    [[nodiscard]] std::unique_ptr<InlineForm> close(std::string_view id);
    void focus(std::string_view id, std::optional<std::size_t> field, bool select_all = false);
    void blur();

  private:
    std::map<std::string, std::unique_ptr<InlineForm>, std::less<>> forms_;
};
} // namespace astra::ui
