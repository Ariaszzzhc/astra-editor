#pragma once

#include <astra/core/content_hash.hpp>
#include <include/core/SkTypeface.h>
#include <cstddef>
#include <optional>
#include <unordered_map>

namespace astra::ui {
// One engine's owner-thread memo. Only content identities survive publication;
// no native font objects are retained or shared with another worker.
class FontSignatures final {
  public:
    [[nodiscard]] std::optional<ContentDigest> read(const SkTypeface&);
    [[nodiscard]] std::size_t bytes_hashed() const noexcept;

  private:
    std::unordered_map<SkTypefaceID, std::optional<ContentDigest>> entries_;
    std::size_t bytes_{};
    [[nodiscard]] std::optional<ContentDigest> calculate(const SkTypeface&);
};
} // namespace astra::ui
