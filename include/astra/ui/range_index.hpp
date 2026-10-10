#pragma once

#include <astra/core/sum_tree.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace astra::ui {
struct Utf8ByteUnit {};

struct DocumentLineUnit {};

struct RangeExtentSummary {
    std::size_t minimum{std::numeric_limits<std::size_t>::max()}, maximum{}, count{};
    friend bool operator==(const RangeExtentSummary&, const RangeExtentSummary&) = default;

    friend RangeExtentSummary operator+(const RangeExtentSummary& left,
                                        const RangeExtentSummary& right) {
        if (right.count > std::numeric_limits<std::size_t>::max() - left.count) {
            throw std::length_error("Range count overflow");
        }
        return {std::min(left.minimum, right.minimum),
                std::max(left.maximum, right.maximum),
                left.count + right.count};
    }
};

// Half-open ranges in one named coordinate unit. Summaries compose min(begin),
// max(end) and count, allowing even overlapping ranges to prune whole subtrees.
template <typename Payload, typename Unit> class RangeIndex {
  public:
    struct Entry {
        std::size_t begin{}, end{};
        Payload value;
        friend bool operator==(const Entry&, const Entry&) = default;
    };

    struct QueryStatistics {
        std::size_t nodes{}, matches{};
    };

    void reset(std::vector<Entry> entries) {
        if (entries.size() > 100000) {
            throw std::length_error("Presentation range index exceeds 100000 entries");
        }
        for (const auto& entry : entries) {
            if (entry.begin >= entry.end) {
                throw std::invalid_argument("Presentation ranges must be nonempty and half-open");
            }
        }
        const auto ordered = [](const Entry& left, const Entry& right) {
            return left.begin < right.begin;
        };
        if (!std::is_sorted(entries.begin(), entries.end(), ordered)) {
            std::stable_sort(entries.begin(), entries.end(), ordered);
        }
        if (entries.empty()) {
            clear();
            return;
        }
        std::size_t ordinal{}, prefix{}, suffix{};
        const auto old_size = tree_.size();
        tree_.for_each([&](const Entry& entry) {
            if (ordinal == prefix && ordinal < entries.size() && entry == entries[ordinal]) {
                ++prefix;
            }
            const auto remaining = old_size - ordinal;
            if (remaining <= entries.size() && entry == entries[entries.size() - remaining]) {
                ++suffix;
            } else {
                suffix = 0;
            }
            ++ordinal;
        });
        suffix = std::min(suffix, std::min(old_size, entries.size()) - prefix);
        if (prefix == old_size && prefix == entries.size()) {
            return;
        }
        auto before = tree_.split(prefix).first;
        auto after = tree_.split(old_size - suffix).second;
        std::vector<Entry> changed;
        changed.reserve(entries.size() - prefix - suffix);
        for (auto index = prefix; index < entries.size() - suffix; ++index) {
            changed.push_back(std::move(entries[index]));
        }
        auto next = before.concat(Tree(std::move(changed))).concat(after);
        tree_ = std::move(next);
    }

    void clear() {
        tree_ = {};
    }

    [[nodiscard]] std::size_t size() const {
        return tree_.size();
    }

    [[nodiscard]] bool validate() const {
        return tree_.validate();
    }

    [[nodiscard]] std::size_t shared_prefix_items(const RangeIndex& other) const {
        return tree_.shared_prefix_summary(other.tree_).count;
    }

    [[nodiscard]] std::size_t shared_suffix_items(const RangeIndex& other) const {
        return tree_.shared_suffix_summary(other.tree_).count;
    }

    template <typename Function>
    QueryStatistics intersect(std::size_t begin, std::size_t end, Function&& function) const {
        if (begin > end) {
            throw std::invalid_argument("Invalid presentation query range");
        }
        QueryStatistics statistics;
        if (begin == end) {
            return statistics;
        }
        tree_.for_each_pruned(
            [&](const RangeExtentSummary& summary) {
                ++statistics.nodes;
                return summary.minimum < end && summary.maximum > begin;
            },
            [&](std::size_t ordinal, const Entry& entry) {
                static_cast<void>(ordinal);
                ++statistics.matches;
                std::invoke(function, entry);
            });
        return statistics;
    }

  private:
    struct Measure {
        RangeExtentSummary operator()(const Entry& entry) const {
            return {entry.begin, entry.end, 1};
        }
    };

    using Tree = astra::SumTree<Entry, RangeExtentSummary, Measure>;
    Tree tree_;
};
} // namespace astra::ui
