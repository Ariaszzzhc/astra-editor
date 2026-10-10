#include <astra/ui/text_source.hpp>
#include <astra/core/rope.hpp>
#include <astra/core/utf8.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace astra::ui {
struct TextSource::Data {
    std::string text;
    Rope coordinates;
    std::size_t shared_prefix{}, shared_suffix{};

    Data(std::string text, Rope coordinates, std::size_t prefix = 0, std::size_t suffix = 0)
        : text(std::move(text)), coordinates(std::move(coordinates)), shared_prefix(prefix),
          shared_suffix(suffix) {
    }
};

TextSource::TextSource(std::string text) {
    auto coordinates = Rope(text);
    data_ = std::make_shared<const Data>(std::move(text), std::move(coordinates));
}

TextSource TextSource::updated(std::string text) const {
    if (!data_)
        return TextSource(std::move(text));
    const auto before = this->text();
    const auto common = std::min(before.size(), text.size());
    auto prefix = static_cast<std::size_t>(
        std::mismatch(
            before.begin(), before.begin() + static_cast<std::ptrdiff_t>(common), text.begin())
            .first -
        before.begin());
    if (prefix == before.size() && prefix == text.size())
        return *this;
    // Shared bytes may end inside a scalar. Include its complete old/new
    // encoding in the replacement; only the changed span needs UTF-8 validation.
    while (!utf8::is_boundary(before, prefix))
        --prefix;
    auto old_end = before.size(), new_end = text.size();
    while (old_end > prefix && new_end > prefix && before[old_end - 1] == text[new_end - 1]) {
        --old_end;
        --new_end;
    }
    while (!utf8::is_boundary(before, old_end)) {
        ++old_end;
        ++new_end;
    }
    auto coordinates = data_->coordinates.replace(
        prefix, old_end, std::string_view(text).substr(prefix, new_end - prefix));
    const auto shared_prefix = data_->coordinates.shared_prefix_bytes(coordinates);
    const auto shared_suffix = data_->coordinates.shared_suffix_bytes(coordinates);
    TextSource result;
    result.data_ = std::make_shared<const Data>(
        std::move(text), std::move(coordinates), shared_prefix, shared_suffix);
    return result;
}

std::string_view TextSource::text() const noexcept {
    return data_ ? std::string_view(data_->text) : std::string_view{};
}

std::size_t TextSource::byte_to_utf16(std::size_t offset) const {
    if (data_)
        return data_->coordinates.byte_to_utf16(offset);
    if (offset)
        throw std::out_of_range("Text source offset exceeds empty text");
    return 0;
}

std::size_t TextSource::utf16_to_byte(std::size_t column) const {
    return data_ ? data_->coordinates.utf16_to_byte_floor(column) : 0;
}

TextSourceStatistics TextSource::statistics() const noexcept {
    return data_ ? TextSourceStatistics{data_->coordinates.size_bytes(),
                                        data_->coordinates.size_utf16(),
                                        data_->coordinates.chunk_count(),
                                        data_->shared_prefix,
                                        data_->shared_suffix}
                 : TextSourceStatistics{};
}
} // namespace astra::ui
