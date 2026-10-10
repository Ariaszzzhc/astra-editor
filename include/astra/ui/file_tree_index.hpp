#pragma once

#include <astra/core/sum_tree.hpp>
#include <astra/protocol/rpc.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace astra::ui {

class FileTreeIndex final {
  public:
    struct Row {
        std::shared_ptr<const protocol::Json> value;
        std::size_t ordinal{};
        std::size_t subtree_end{};
    };

    struct Summary {
        std::size_t rows{};
        std::size_t last_source{};
        friend bool operator==(const Summary&, const Summary&) = default;

        friend Summary operator+(Summary left, Summary right) {
            return {left.rows + right.rows, std::max(left.last_source, right.last_source)};
        }
    };

    struct Measure {
        Summary operator()(const Row& row) const {
            return {1, row.ordinal + 1};
        }
    };

    using Snapshot = SumTree<Row, Summary, Measure>;
    void reset(const protocol::Json& entries);
    void toggle(std::size_t visible_row);

    [[nodiscard]] std::size_t size() const noexcept {
        return visible_.size();
    }

    [[nodiscard]] const protocol::Json& at(std::size_t row) const {
        return *visible_.at(row).value;
    }

    [[nodiscard]] bool collapsed(std::string_view path) const {
        return collapsed_.contains(std::string(path));
    }

    [[nodiscard]] Snapshot snapshot() const {
        return visible_;
    }

    template <typename Function>
    void for_each_range(std::size_t begin, std::size_t end, Function&& function) const {
        auto index = begin;
        visible_.for_each_range(begin, end, [&](const Row& row) {
            function(index++, *row.value);
        });
    }

  private:
    [[nodiscard]] Snapshot
    filtered_range(std::size_t begin, std::size_t end, std::string_view exclude = {}) const;
    Snapshot source_;
    Snapshot visible_;
    std::map<std::string, std::size_t> positions_;
    std::set<std::string> collapsed_;
};
} // namespace astra::ui
