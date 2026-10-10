#pragma once

#include <astra/core/sum_tree.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace astra::ui {
class PickerIndex final {
  public:
    struct Row {
        std::string label, folded;
        std::size_t source{};
        std::string icon_path;
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
    void reset(const std::vector<std::string>& labels,
               bool dynamic = false,
               const std::vector<std::string>& icon_paths = {});
    void filter(std::string_view query);
    // Provider publication is atomic and only accepts the current query identity.
    bool publish(std::string_view generation,
                 const std::vector<std::string>& labels,
                 std::string message,
                 bool error = false,
                 const std::vector<std::string>& icon_paths = {});

    [[nodiscard]] bool dynamic() const noexcept {
        return dynamic_;
    }

    [[nodiscard]] bool ready() const noexcept {
        return !dynamic_ || published_ == generation_;
    }

    [[nodiscard]] std::string generation() const {
        return std::to_string(generation_);
    }

    [[nodiscard]] const std::string& query() const noexcept {
        return query_;
    }

    [[nodiscard]] const std::string& message() const noexcept {
        return message_;
    }

    [[nodiscard]] bool error() const noexcept {
        return error_;
    }

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
    std::string message_;
    std::uint64_t generation_{1}, published_{};
    bool dynamic_{}, error_{};
};
} // namespace astra::ui
