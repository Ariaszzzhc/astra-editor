#pragma once

#include <astra/core/sum_tree.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace astra::ui {

// Ordered identities and cached logical widths. Font generations invalidate
// measurements in O(1); stale widths remain provisional until bounded refresh.
class TabIndex final {
  public:
    static constexpr std::size_t max_tabs = 65536;

    struct Entry {
        std::string id, label;
        std::uint64_t order{};
        float width{110};
        std::uint64_t generation{};
        friend bool operator==(const Entry&, const Entry&) = default;
    };

    struct Summary {
        std::size_t count{};
        double width{};
        std::uint64_t last_order{};
        std::uint64_t oldest_generation{std::numeric_limits<std::uint64_t>::max()};
        friend bool operator==(const Summary&, const Summary&) = default;

        friend Summary operator+(Summary left, Summary right) {
            return {left.count + right.count,
                    left.width + right.width,
                    std::max(left.last_order, right.last_order),
                    std::min(left.oldest_generation, right.oldest_generation)};
        }
    };

    struct Measure {
        Summary operator()(const Entry& entry) const {
            return {1, entry.width, entry.order, entry.generation};
        }
    };

    using Snapshot = SumTree<Entry, Summary, Measure>;

    void append(std::string id, std::string label);
    void erase(std::string_view id);
    void move(std::string_view id, std::size_t destination);
    void rename(std::string_view id, std::string label);
    void invalidate();
    void measured(std::size_t index, float physical_label_width, float scale);
    [[nodiscard]] std::optional<std::size_t> position(std::string_view id) const;
    [[nodiscard]] std::optional<std::size_t> next_pending() const;

    [[nodiscard]] bool prepared(std::size_t index) const {
        return entries_.at(index).generation == generation_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        return entries_.empty();
    }

    [[nodiscard]] const Entry& at(std::size_t index) const {
        return entries_.at(index);
    }

    [[nodiscard]] const std::string& id_at(std::size_t index) const {
        return at(index).id;
    }

    [[nodiscard]] std::vector<std::string> ids() const;

    [[nodiscard]] Snapshot snapshot() const {
        return entries_;
    }

    [[nodiscard]] std::size_t pending() const noexcept {
        return size() - prepared_;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] std::size_t measurements() const noexcept {
        return measurements_;
    }

    [[nodiscard]] std::size_t relabels() const noexcept {
        return relabels_;
    }

  private:
    Snapshot entries_;
    std::map<std::string, std::uint64_t, std::less<>> orders_;
    std::uint64_t generation_{1};
    std::size_t prepared_{}, measurements_{}, relabels_{};
    static constexpr std::uint64_t order_step = std::uint64_t{1} << 32;
    [[nodiscard]] static std::uint64_t order_at(const Snapshot&, std::size_t insertion);
    // Exhausted gaps trigger a bounded O(n) relabel. Normal identity lookup,
    // insertion, removal, reordering and width updates use logarithmic paths.
    [[nodiscard]] static Snapshot relabelled(const Snapshot&);
    [[nodiscard]] static std::map<std::string, std::uint64_t, std::less<>>
    identities(const Snapshot&);
};
} // namespace astra::ui
