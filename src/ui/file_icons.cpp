#include <astra/ui/file_icons.hpp>
#include <astra/core/utf8.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace astra::ui {
namespace {
std::string key(std::string_view path, bool directory) {
    if (path.size() > 32768 || std::ranges::any_of(path, [](unsigned char c) {
            return c < 32;
        })) {
        throw protocol::RpcError(-32602, "Invalid file icon path");
    }
    utf8::validate(path);
    auto result = std::string(1, directory ? 'd' : 'f');
    result.append(path);
    return result;
}
} // namespace

FileIcon FileIcon::parse(const protocol::Json& value) {
    if (value.is_object() && value.contains("asset")) {
        FileIcon result;
        if (value.size() > 5) {
            throw protocol::RpcError(-32602, "Invalid resource icon descriptor");
        }
        for (const auto& [key, item] : value.items()) {
            if (!item.is_string()) {
                throw protocol::RpcError(-32602, "Icon descriptor fields require strings");
            }
            if (key == "tone") {
                result.tone = item.get<std::string>();
                continue;
            }
            const auto id = icon_asset(item.get_ref<const std::string&>());
            if (!id) {
                throw protocol::RpcError(-32602, "Unknown icon resource");
            }
            if (key == "asset") {
                result.asset = id;
            } else if (key == "lightAsset") {
                result.light_asset = id;
            } else if (key == "openAsset") {
                result.open_asset = id;
            } else if (key == "openLightAsset") {
                result.open_light_asset = id;
            } else {
                throw protocol::RpcError(-32602, "Invalid icon descriptor field");
            }
        }
        static_cast<void>(result.color({}));
        return result;
    }
    if (!value.is_object() || value.size() != 2 || !value.contains("glyph") ||
        !value.contains("tone")) {
        throw protocol::RpcError(-32602, "Invalid file icon descriptor");
    }
    const auto name = value.at("glyph").get<std::string>();
    const std::pair<std::string_view, Icon> glyphs[] = {{"files", Icon::files},
                                                        {"search", Icon::search},
                                                        {"branch", Icon::branch},
                                                        {"settings", Icon::settings},
                                                        {"folder", Icon::folder},
                                                        {"document", Icon::document},
                                                        {"code", Icon::code},
                                                        {"braces", Icon::braces},
                                                        {"symbol", Icon::symbol},
                                                        {"function", Icon::function},
                                                        {"refresh", Icon::refresh},
                                                        {"warning", Icon::warning}};
    for (const auto& [id, glyph] : glyphs) {
        if (name == id) {
            FileIcon result{glyph, value.at("tone").get<std::string>()};
            static_cast<void>(result.color({}));
            return result;
        }
    }
    throw protocol::RpcError(-32602, "Unknown file icon glyph");
}

IconAsset FileIcon::drawing_asset(const WorkbenchStyle::Colors& palette, bool expanded) const {
    const auto background = palette.background;
    const bool light = 299 * ((background >> 16) & 255) + 587 * ((background >> 8) & 255) +
                           114 * (background & 255) >=
                       128000;
    if (expanded) {
        if (light && open_light_asset) {
            return *open_light_asset;
        }
        if (open_asset) {
            return *open_asset;
        }
    }
    if (light && light_asset) {
        return *light_asset;
    }
    return asset ? *asset : icon_asset(glyph);
}

std::uint32_t FileIcon::color(const WorkbenchStyle::Colors& palette) const {
    const std::pair<std::string_view, std::uint32_t> roles[] = {{"muted", palette.muted},
                                                                {"foreground", palette.foreground},
                                                                {"folder", palette.folder},
                                                                {"codeIcon", palette.code_icon},
                                                                {"scriptIcon", palette.script_icon},
                                                                {"accent", palette.accent},
                                                                {"added", palette.added},
                                                                {"changed", palette.changed},
                                                                {"error", palette.error},
                                                                {"warning", palette.warning}};
    for (const auto& [name, color] : roles) {
        if (tone == name)
            return color;
    }
    if (tone.size() == 7 && tone.front() == '#') {
        std::uint32_t rgb{};
        const auto parsed = std::from_chars(tone.data() + 1, tone.data() + 7, rgb, 16);
        if (parsed.ec == std::errc{} && parsed.ptr == tone.data() + 7)
            return 0xff000000U | rgb;
    }
    throw protocol::RpcError(-32602, "Unknown file icon tone");
}

FileIcon FileIcons::lookup(std::string_view path, bool directory) {
    auto identity = key(path, directory);
    auto found = entries_.find(identity);
    if (found == entries_.end()) {
        while (!recent_.empty() &&
               (entries_.size() >= max_entries || bytes_ + identity.size() > max_bytes)) {
            bytes_ -= recent_.front().size();
            std::erase(pending_, recent_.front());
            entries_.erase(recent_.front());
            recent_.pop_front();
        }
        recent_.push_back(identity);
        try {
            found = entries_.emplace(identity, Entry{{}, 0, 0, std::prev(recent_.end())}).first;
        } catch (...) {
            recent_.pop_back();
            throw;
        }
        bytes_ += identity.size();
    } else {
        recent_.splice(recent_.end(), recent_, found->second.recent);
    }
    auto& entry = found->second;
    if (entry.generation != generation_ && entry.queued != generation_ &&
        pending_.size() < max_pending) {
        pending_.push_back(std::move(identity));
        entry.queued = generation_;
    }
    return entry.icon;
}

std::optional<FileIcons::Json> FileIcons::begin() {
    if (active_ || pending_.empty())
        return std::nullopt;
    auto items = Json::array();
    std::size_t bytes{}, consumed{};
    while (consumed < pending_.size() && items.size() < batch_size) {
        const auto& identity = pending_.at(consumed);
        if (!items.empty() && bytes + identity.size() > max_batch_bytes)
            break;
        ++consumed;
        const auto found = entries_.find(identity);
        if (found == entries_.end() || found->second.generation == generation_)
            continue;
        bytes += identity.size();
        items.push_back({{"path", identity.substr(1)}, {"directory", identity.front() == 'd'}});
    }
    std::optional<Json> batch;
    if (!items.empty())
        batch.emplace(Json{{"generation", generation()}, {"items", std::move(items)}});
    // Queue ownership changes only after the complete request is allocated.
    for (std::size_t i = 0; i < consumed; ++i)
        pending_.pop_front();
    active_ = batch.has_value();
    return batch;
}

std::optional<FileIcon> FileIcons::peek(std::string_view path, bool directory) const {
    const auto found = entries_.find(key(path, directory));
    return found == entries_.end() ? std::nullopt : std::optional<FileIcon>(found->second.icon);
}

void FileIcons::finish(const Json& batch, const Json* result) {
    active_ = false;
    if (batch.at("generation") != generation())
        return;
    std::vector<FileIcon> prepared;
    try {
        if (result) {
            if (result->at("generation") != batch.at("generation") ||
                !result->at("items").is_array() ||
                result->at("items").size() != batch.at("items").size()) {
                throw protocol::RpcError(-32602, "Invalid file icon batch result");
            }
            for (std::size_t i = 0; i < batch.at("items").size(); ++i) {
                const auto& item = result->at("items").at(i);
                if (item.at("path") != batch.at("items").at(i).at("path") ||
                    item.at("directory") != batch.at("items").at(i).at("directory")) {
                    throw protocol::RpcError(-32602, "File icon batch identity mismatch");
                }
                prepared.push_back(FileIcon::parse(item.at("icon")));
            }
        }
    } catch (...) {
        finish(batch, nullptr);
        throw;
    }
    for (std::size_t i = 0; i < batch.at("items").size(); ++i) {
        const auto& item = batch.at("items").at(i);
        auto found = entries_.find(
            key(item.at("path").get<std::string>(), item.at("directory").get<bool>()));
        if (found == entries_.end())
            continue;
        // Failed batches retain old drawing data and suppress per-frame retries.
        found->second.generation = generation_;
        if (result) {
            found->second.icon = std::move(prepared.at(i));
            ++resolutions_;
        }
    }
}

void FileIcons::invalidate() {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw protocol::RpcError(-32004, "File icon generation exhausted");
    }
    ++generation_;
    pending_.clear();
}

std::string FileIcons::generation() const {
    return std::to_string(generation_);
}

FileIcons::Json FileIcons::state() const {
    return {{"generation", generation()},
            {"entries", entries_.size()},
            {"bytes", bytes_},
            {"pending", pending_.size()},
            {"active", active_},
            {"resolutions", resolutions_}};
}
} // namespace astra::ui
