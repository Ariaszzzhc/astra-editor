#pragma once

#include <astra/core/sum_tree.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace astra::ui {
class PickerIndex final {
  public:
    struct Row {
        std::string label, folded;
        std::size_t source{};
    };

    struct Summary {
        std::size_t rows{};
        friend bool operator==(const Summary&, const Summary&) = default;

        friend Summary operator+(Summary left, Summary right) {
            return {left.rows + right.rows};
        }
    };

    struct Measure {
        Summary operator()(const Row&) const {
            return {1};
        }
    };

    using Snapshot = SumTree<Row, Summary, Measure>;
    void reset(const std::vector<std::string>& labels);
    void filter(std::string_view query);

    [[nodiscard]] std::size_t size() const noexcept {
        return visible_.size();
    }

    [[nodiscard]] const Row& at(std::size_t row) const {
        return visible_.at(row);
    }

    [[nodiscard]] Snapshot snapshot() const {
        return visible_;
    }

    template <typename Function>
    void for_each_range(std::size_t begin, std::size_t end, Function&& function) const {
        auto index = begin;
        visible_.for_each_range(begin, end, [&](const Row& row) {
            function(index++, row);
        });
    }

  private:
    Snapshot source_, visible_;
    std::string query_;
};
} // namespace astra::ui
