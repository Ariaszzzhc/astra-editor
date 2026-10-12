#include <astra/ui/icons.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace astra::ui {
std::optional<IconAsset> icon_asset(std::string_view id) {
    const auto resources = icon_resources();
    const auto found = std::ranges::lower_bound(resources, id, {}, &IconResource::id);
    if (found == resources.end() || found->id != id) {
        return std::nullopt;
    }
    return IconAsset{static_cast<std::uint16_t>(found - resources.begin())};
}

std::string_view icon_asset_name(IconAsset asset) {
    if (asset.value >= icon_resources().size()) {
        throw std::invalid_argument("Invalid icon asset");
    }
    return icon_resources()[asset.value].id;
}

IconAsset icon_asset(Icon icon) {
    constexpr std::array<std::string_view, 15> ids{"codicons/files",
                                                   "codicons/search",
                                                   "codicons/git-branch",
                                                   "codicons/settings-gear",
                                                   "codicons/close",
                                                   "codicons/chevron-right",
                                                   "codicons/chevron-down",
                                                   "codicons/folder",
                                                   "codicons/file",
                                                   "codicons/code",
                                                   "codicons/json",
                                                   "codicons/symbol-misc",
                                                   "codicons/symbol-method",
                                                   "codicons/refresh",
                                                   "codicons/warning"};
    const auto index = static_cast<std::size_t>(icon);
    if (index >= ids.size()) {
        throw std::invalid_argument("Invalid UI icon");
    }
    return *icon_asset(ids[index]);
}

Icon view_icon(std::string_view name) {
    constexpr std::pair<std::string_view, Icon> icons[] = {{"files", Icon::files},
                                                           {"search", Icon::search},
                                                           {"branch", Icon::branch},
                                                           {"braces", Icon::braces},
                                                           {"symbol", Icon::symbol},
                                                           {"folder", Icon::folder},
                                                           {"code", Icon::code},
                                                           {"function", Icon::function},
                                                           {"warning", Icon::warning},
                                                           {"settings", Icon::settings},
                                                           {"refresh", Icon::refresh}};
    for (const auto& [id, icon] : icons) {
        if (id == name) {
            return icon;
        }
    }
    return Icon::document;
}
} // namespace astra::ui
