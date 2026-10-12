#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
class SkCanvas;

namespace astra::ui {
enum class Icon {
    files,
    search,
    branch,
    settings,
    close,
    chevron_right,
    chevron_down,
    folder,
    document,
    code,
    braces,
    symbol,
    function,
    refresh,
    warning
};

struct IconAsset {
    std::uint16_t value{};
    bool operator==(const IconAsset&) const = default;
};

struct IconResource {
    std::string_view id, svg;
    bool monochrome{};
};

struct IconModule {
    std::string_view id, source;
};

[[nodiscard]] std::span<const IconResource> icon_resources();
[[nodiscard]] std::span<const IconModule> icon_modules();
[[nodiscard]] std::optional<IconAsset> icon_asset(std::string_view id);
[[nodiscard]] std::string_view icon_asset_name(IconAsset asset);
[[nodiscard]] IconAsset icon_asset(Icon icon);
[[nodiscard]] Icon view_icon(std::string_view name);

// One owned cache per window. SVG parsing/recording happens before startup or
// on a cancellable worker; painting only replays immutable vector pictures.
class IconRenderer final {
  public:
    IconRenderer();
    ~IconRenderer();
    IconRenderer(const IconRenderer&) = delete;
    IconRenderer& operator=(const IconRenderer&) = delete;
    void poll();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] std::size_t bytes() const;
    void draw(SkCanvas*, Icon, float x, float y, float size, std::uint32_t color) const;
    void draw(SkCanvas*, IconAsset, float x, float y, float size, std::uint32_t color) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace astra::ui
