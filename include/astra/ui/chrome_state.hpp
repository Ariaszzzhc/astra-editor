#pragma once

#include <astra/ui/view_index.hpp>
#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace astra::ui {
enum class ChromeBank {
    command,
    activity,
    manage,
    breadcrumb,
    status_left,
    status_right,
    welcome_title,
    welcome_detail,
    welcome_actions,
    notices
};
inline constexpr std::array<std::string_view, 10> chrome_banks{"command",
                                                               "activity",
                                                               "manage",
                                                               "breadcrumb",
                                                               "statusLeft",
                                                               "statusRight",
                                                               "welcomeTitle",
                                                               "welcomeDetail",
                                                               "welcomeActions",
                                                               "notices"};

// Owner-local, bounded presentation publication. All captions and actions are
// supplied by JS; retained old pixels cannot activate after context invalidation.
class ChromeState final {
  public:
    ChromeState();
    void sync(protocol::Json context);
    void refresh();
    [[nodiscard]] std::optional<protocol::Json> take_request();
    [[nodiscard]] bool publish(const protocol::Json&);
    [[nodiscard]] bool reject(std::string_view version);
    [[nodiscard]] protocol::Json
    activate(std::string_view version, std::string_view bank, std::string_view row) const;
    [[nodiscard]] protocol::Json state() const;

    [[nodiscard]] const ViewIndex& rows(ChromeBank bank) const {
        return rows_.at(static_cast<std::size_t>(bank));
    }

    [[nodiscard]] const protocol::Json& model() const noexcept {
        return model_;
    }

    [[nodiscard]] const protocol::Json& context() const noexcept {
        return context_;
    }

    [[nodiscard]] bool needs_request() const noexcept {
        return dirty_;
    }

    [[nodiscard]] bool pending() const noexcept {
        return pending_;
    }

  private:
    void invalidate(protocol::Json);
    protocol::Json context_, model_;
    std::array<ViewIndex, chrome_banks.size()> rows_;
    std::uint64_t version_{}, published_{};
    bool dirty_{}, pending_{};
};
} // namespace astra::ui
