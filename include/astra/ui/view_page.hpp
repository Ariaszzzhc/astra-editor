#pragma once

#include <astra/ui/view_index.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace astra::ui {
// A bounded, immutable viewport publication. Providers own content/actions;
// this primitive fences identity, revision, visibility and the requested range.
class ViewPage final {
  public:
    struct Request {
        std::string id, revision, activity;
        std::size_t offset{}, limit{};
        bool visible{};
        friend bool operator==(const Request&, const Request&) = default;
    };

    void sync(Request);
    void refresh();
    [[nodiscard]] std::optional<protocol::Json> take_request();
    [[nodiscard]] bool publish(const protocol::Json&);
    [[nodiscard]] protocol::Json state() const;
    [[nodiscard]] protocol::Json activate(std::string_view version, std::string_view row) const;

    [[nodiscard]] const ViewIndex& rows() const noexcept {
        return rows_;
    }

    [[nodiscard]] std::size_t total() const noexcept {
        return total_;
    }

    [[nodiscard]] std::size_t offset() const noexcept {
        return request_.offset;
    }

    [[nodiscard]] bool pending() const noexcept {
        return pending_;
    }

    [[nodiscard]] bool needs_request() const noexcept {
        return dirty_;
    }

    [[nodiscard]] const std::string& message() const noexcept {
        return message_;
    }

  private:
    [[nodiscard]] protocol::Json identity() const;
    void invalidate(Request);
    Request request_;
    ViewIndex rows_;
    std::string message_;
    std::size_t total_{};
    std::uint64_t version_{};
    bool dirty_{}, pending_{};
};
} // namespace astra::ui
