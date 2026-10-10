#pragma once

#include <astra/core/sum_tree.hpp>
#include <astra/protocol/rpc.hpp>
#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace astra::ui {
struct ViewRow {
    std::shared_ptr<const protocol::Json> value;
    std::size_t ordinal{}, depth{};
    bool group{};
    std::optional<std::size_t> parent;
};

struct ViewSummary {
    std::size_t rows{}, last_source{}, minimum_depth{std::numeric_limits<std::size_t>::max()};
    friend bool operator==(const ViewSummary&, const ViewSummary&) = default;

    friend ViewSummary operator+(ViewSummary left, ViewSummary right) {
        return {left.rows + right.rows,
                std::max(left.last_source, right.last_source),
                std::min(left.minimum_depth, right.minimum_depth)};
    }
};

struct MeasureViewRow {
    ViewSummary operator()(const ViewRow& row) const {
        return {1, row.ordinal + 1, row.depth};
    }
};

using ViewSnapshot = SumTree<ViewRow, ViewSummary, MeasureViewRow>;

struct ViewSource {
    ViewSnapshot rows;
    std::map<std::string, std::size_t> positions;
    std::size_t bytes{};
};

// Immutable source plus an indexed visible projection. Refreshes build offscreen;
// publication filtering advances in bounded steps while the old view stays live.
class ViewIndex final {
  public:
    explicit ViewIndex(std::shared_ptr<const ViewSource> source = {},
                       std::set<std::string> collapsed = {});
    [[nodiscard]] bool filter_step(std::size_t budget = 128);

    [[nodiscard]] bool ready() const noexcept {
        return !source_ || cursor_ == source_->rows.size();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return visible_.size();
    }

    [[nodiscard]] std::size_t source_size() const noexcept {
        return source_ ? source_->rows.size() : 0;
    }

    [[nodiscard]] std::size_t source_bytes() const noexcept {
        return source_ ? source_->bytes : 0;
    }

    [[nodiscard]] const ViewRow& at(std::size_t index) const {
        return visible_.at(index);
    }

    [[nodiscard]] ViewSnapshot snapshot() const {
        return visible_;
    }

    [[nodiscard]] const std::set<std::string>& collapsed() const {
        static const std::set<std::string> empty;
        return collapsed_ ? *collapsed_ : empty;
    }

    [[nodiscard]] bool collapsed(std::string_view id) const {
        return collapsed_ && collapsed_->contains(std::string(id));
    }

    // Return the old projection so the owner can reclaim it off the input loop.
    [[nodiscard]] ViewSnapshot toggle(std::size_t index);
    // Prepare an ancestor-expanded projection without changing this snapshot.
    // Only the outermost hidden subtree is replaced; unrelated folds survive.
    [[nodiscard]] std::optional<ViewIndex> revealing(std::string_view id) const;
    [[nodiscard]] std::optional<std::size_t> position(std::string_view id,
                                                      bool visible_ancestor = false) const;
    [[nodiscard]] std::optional<std::size_t> parent(std::size_t index) const;

    template <typename Function>
    void for_each_range(std::size_t begin, std::size_t end, Function&& function) const {
        auto index = begin;
        visible_.for_each_range(begin, end, [&](const ViewRow& row) {
            std::invoke(function, index++, row);
        });
    }

  private:
    [[nodiscard]] std::size_t subtree_end(const ViewRow& row) const;
    [[nodiscard]] ViewSnapshot
    filtered_range(std::size_t begin, std::size_t end, std::string_view exclude) const;
    std::shared_ptr<const ViewSource> source_;
    ViewSnapshot visible_;
    // MSVC's set move can allocate its sentinel. Keep the set behind ownership
    // so publication/retirement moves remain allocation-free and noexcept.
    std::unique_ptr<std::set<std::string>> collapsed_;
    std::size_t cursor_{};
};

class ViewBuilder final {
  public:
    static constexpr std::size_t max_page_rows = 256, max_page_bytes = 65536, max_rows = 100000,
                                 max_bytes = 32 * 1024 * 1024;
    ViewBuilder();
    void append(const protocol::Json& rows);

    [[nodiscard]] std::size_t size() const noexcept {
        return source_ ? source_->rows.size() : 0;
    }

    [[nodiscard]] std::shared_ptr<const ViewSource> finish() &&;

  private:
    std::unique_ptr<ViewSource> source_;
};
} // namespace astra::ui
