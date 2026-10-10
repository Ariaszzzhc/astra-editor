#pragma once

#include <astra/protocol/rpc.hpp>
#include <astra/ui/icons.hpp>
#include <astra/ui/workbench_style.hpp>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace astra::ui {
struct FileIcon {
    Icon glyph{Icon::document};
    std::string tone{"muted"};
    [[nodiscard]] static FileIcon parse(const protocol::Json& value);
    [[nodiscard]] std::uint32_t color(const WorkbenchStyle::Colors& palette) const;
};

// Bounded presentation cache. Association policy runs asynchronously in JS.
// Old descriptors remain drawable while a new theme generation is resolving.
class FileIcons final {
  public:
    using Json = protocol::Json;
    static constexpr std::size_t max_entries = 2048, max_bytes = 4 * 1024 * 1024, max_pending = 256,
                                 batch_size = 32, max_batch_bytes = 64 * 1024;
    FileIcons() = default;
    FileIcons(const FileIcons&) = delete;
    FileIcons& operator=(const FileIcons&) = delete;
    [[nodiscard]] FileIcon lookup(std::string_view path, bool directory);
    [[nodiscard]] std::optional<FileIcon> peek(std::string_view path, bool directory) const;
    [[nodiscard]] std::optional<Json> begin();
    void finish(const Json& batch, const Json* result);
    void invalidate();
    [[nodiscard]] std::string generation() const;

    [[nodiscard]] bool idle() const {
        return pending_.empty() && !active_;
    }

    [[nodiscard]] Json state() const;

  private:
    struct Entry {
        FileIcon icon;
        std::uint64_t generation{}, queued{};
        std::list<std::string>::iterator recent;
    };

    std::map<std::string, Entry, std::less<>> entries_;
    std::list<std::string> recent_;
    std::deque<std::string> pending_;
    std::size_t bytes_{};
    std::uint64_t generation_{1}, resolutions_{};
    bool active_{};
};
} // namespace astra::ui
