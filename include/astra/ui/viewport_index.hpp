#pragma once

#include <astra/core/sum_tree.hpp>

#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace astra::ui {

struct ViewportLine {
    std::size_t number{};
    std::size_t begin{};
    std::size_t end{};
    std::size_t next{};
    bool complete{};
    std::string text;
    friend bool operator==(const ViewportLine&, const ViewportLine&) = default;
};

struct ViewportSummary {
    std::size_t rows{};
    std::size_t utf8_bytes{};
    std::size_t first_line{std::numeric_limits<std::size_t>::max()};
    std::size_t last_line{};
    std::size_t begin{};
    std::size_t end{};
    friend bool operator==(const ViewportSummary&, const ViewportSummary&) = default;
    friend ViewportSummary operator+(const ViewportSummary&, const ViewportSummary&);
};

struct MeasureViewportLine {
    ViewportSummary operator()(const ViewportLine&) const noexcept;
};

// One complete, contiguous, bounded page in document-line and UTF-8 byte units.
// Copies retain immutable measured rows; publication preserves equal tree edges.
class ViewportIndex {
  public:
    void reset(std::vector<ViewportLine> lines);
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const ViewportSummary& summary() const noexcept;
    [[nodiscard]] const ViewportLine* find(std::size_t number) const;
    [[nodiscard]] bool validate() const;
    // Half-open ordinal row range; document-line numbers remain unchanged.
    [[nodiscard]] ViewportIndex row_range(std::size_t begin, std::size_t end) const;
    [[nodiscard]] std::size_t shared_prefix_rows(const ViewportIndex&) const;
    [[nodiscard]] std::size_t shared_suffix_rows(const ViewportIndex&) const;

    template <typename Function> void for_each(Function&& function) const {
        tree_.for_each(std::forward<Function>(function));
    }

  private:
    using Tree = SumTree<ViewportLine, ViewportSummary, MeasureViewportLine>;
    Tree tree_;
};

} // namespace astra::ui
