#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>

namespace astra::ui {
struct TextSourceStatistics {
    std::size_t bytes{}, utf16_units{}, chunks{};
    std::size_t shared_prefix_bytes{}, shared_suffix_bytes{};
};

// Immutable UTF-8 text and persistent measured coordinates, without native
// paragraphs/fonts. Copies can cross worker threads while older layouts paint.
class TextSource final {
  public:
    TextSource() = default;
    explicit TextSource(std::string);
    [[nodiscard]] TextSource updated(std::string) const;
    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] std::size_t byte_to_utf16(std::size_t) const;
    [[nodiscard]] std::size_t utf16_to_byte(std::size_t) const;
    [[nodiscard]] TextSourceStatistics statistics() const noexcept;

  private:
    struct Data;
    std::shared_ptr<const Data> data_;
};
} // namespace astra::ui
