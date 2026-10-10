#pragma once
#include <astra/core/sum_tree.hpp>
#include <string>
#include <string_view>
#include <cstddef>

namespace astra::ui {
struct IndexedTextLine {
    std::string text;
    std::size_t extent{};
};

struct TextLineSummary {
    std::size_t rows{}, bytes{};
    friend TextLineSummary operator+(TextLineSummary left, TextLineSummary right);
    bool operator==(const TextLineSummary&) const = default;
};

struct MeasureTextLine {
    TextLineSummary operator()(const IndexedTextLine& line) const noexcept {
        return {1, line.extent};
    }
};

// Logical CR/LF/CRLF lines retain byte extents and immutable snapshots. Native
// text panels seek/visit only their visible rows; line shaping stays separate.
class TextLineIndex final {
  public:
    void reset(std::string_view text);

    [[nodiscard]] std::size_t size() const noexcept {
        return lines_.size();
    }

    [[nodiscard]] std::size_t bytes() const noexcept {
        return lines_.summary().bytes;
    }

    [[nodiscard]] const std::string& at(std::size_t index) const {
        return lines_.at(index).text;
    }

    [[nodiscard]] std::size_t offset(std::size_t index) const {
        return lines_.prefix_summary(index).bytes;
    }

    template <class Visit>
    void for_each_range(std::size_t begin, std::size_t end, Visit&& visit) const {
        auto row = begin;
        lines_.for_each_range(begin, end, [&](const IndexedTextLine& line) {
            visit(row++, line.text);
        });
    }

  private:
    SumTree<IndexedTextLine, TextLineSummary, MeasureTextLine> lines_;
};
} // namespace astra::ui
