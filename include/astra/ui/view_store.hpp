#pragma once

#include <astra/ui/view_index.hpp>
#include <astra/ui/view_retirement.hpp>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace astra::ui {
struct RetainedView {
    protocol::Json spec;
    ViewIndex index;
    std::string version;
    std::string subtitle;
    std::string empty{"No items"};
    std::size_t scroll{};
    bool expanded{true};
    std::uint64_t fold_generation{};
    std::optional<std::string> cursor;
};

// Owner-thread publication fence. The published snapshot survives failed,
// superseded, cancelled and partially received replacements.
class ViewStore final {
  public:
    explicit ViewStore(std::size_t retirement_capacity = 48,
                       std::size_t byte_capacity = 256 * 1024 * 1024);
    ~ViewStore();
    void register_spec(const protocol::Json& spec);
    [[nodiscard]] std::string begin(const protocol::Json& spec);
    [[nodiscard]] bool
    append(std::string_view id, std::string_view version, const protocol::Json& rows);
    [[nodiscard]] std::future<protocol::Json>
    commit(std::string_view id, std::string_view version, std::string subtitle, std::string empty);
    [[nodiscard]] bool abort(std::string_view id, std::string_view version);
    void cancel_all();
    void shutdown();
    void poll();

    [[nodiscard]] bool idle() const noexcept {
        return pending_.empty();
    }

    [[nodiscard]] bool quiescent() const {
        return idle() && retirement_.idle();
    }

    [[nodiscard]] ViewRetirement::Stats retirement_stats() const {
        return retirement_.stats();
    }

    [[nodiscard]] RetainedView& at(std::string_view id);
    [[nodiscard]] const RetainedView& at(std::string_view id) const;
    [[nodiscard]] std::vector<std::string> ordered() const;
    void toggle(std::string_view id, std::size_t row);
    [[nodiscard]] std::optional<std::size_t> cursor(std::string_view id) const;
    [[nodiscard]] protocol::Json state(std::string_view id) const;
    [[nodiscard]] bool select(std::string_view id, std::string_view row, std::size_t page);
    [[nodiscard]] protocol::Json
    navigate(std::string_view id, std::string_view action, std::size_t page);
    void reveal(std::string_view id, std::size_t page);
    [[nodiscard]] bool reveal(std::string_view id, std::string_view row, std::size_t page);

  private:
    struct Pending {
        explicit Pending(ViewRetirement::Reservation reservation)
            : ownership(std::move(reservation)) {
        }

        ViewRetirement::Reservation ownership;
        std::string version;
        ViewBuilder builder;
        std::shared_ptr<const ViewSource> source;
        std::optional<ViewIndex> projection;
        std::promise<protocol::Json> completion;
        bool committing{};
        std::uint64_t fold_generation{};
        std::string subtitle, empty;
    };

    [[nodiscard]] Pending* pending(std::string_view id, std::string_view version);
    void retire(Pending& pending) noexcept;
    // Declared before its reservations so it outlives every owning view/stage.
    ViewRetirement retirement_;
    std::map<std::string, ViewRetirement::Reservation, std::less<>> published_ownership_;
    std::map<std::string, RetainedView, std::less<>> views_;
    std::map<std::string, std::unique_ptr<Pending>, std::less<>> pending_;
    std::uint64_t sequence_{};
    std::string poll_after_;
    bool stopped_{};
};
} // namespace astra::ui
