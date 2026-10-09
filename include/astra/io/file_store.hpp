#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace astra::io {

enum class TextEncoding { utf8, utf8_bom, utf16_le, utf16_be, latin1 };

struct FileVersion {
    std::filesystem::path path;
    // Collision-free conflict detection retains one encoded copy of the file.
    // Copies of this token share its allocation; loading also owns decoded UTF-8.
    std::shared_ptr<const std::string> raw_content;
};

struct FileSnapshot {
    std::string text;
    TextEncoding encoding{TextEncoding::utf8};
    FileVersion version;
};

struct DecodedText {
    std::string text;
    TextEncoding encoding{TextEncoding::utf8};
};

// Pure strict decoding for worker outputs such as Git's working-tree filters.
// Applies exactly the same BOM, UTF-8, UTF-16, Latin-1 and NUL rules as read_file.
[[nodiscard]] DecodedText decode_bytes(std::string_view raw,
    std::optional<TextEncoding> encoding_override = std::nullopt);

class FileConflict final : public std::runtime_error {
public:
    explicit FileConflict(const std::string& message);
};

// A rare OS replacement failure can require manual recovery. The backup is kept,
// never deleted, when moving it back to the original path also fails.
class FileRecoveryRequired final : public std::runtime_error {
public:
    FileRecoveryRequired(std::filesystem::path original, std::filesystem::path backup);
    [[nodiscard]] const std::filesystem::path& original_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& backup_path() const noexcept;

private:
    std::filesystem::path original_;
    std::filesystem::path backup_;
};

inline constexpr std::size_t default_max_file_bytes = 64U * 1024U * 1024U;

// Synchronous filesystem adapters: invoke on libuv workers, never the input loop.
// The limit applies to encoded on-disk bytes. UTF-8/UTF-16 are decoded strictly;
// BOMs select encoding, NUL is rejected as binary, and line endings stay unchanged.
// Latin-1 requires an explicit override: guessing it would hide malformed UTF-8.
// Other explicit overrides must agree with the detected BOM/UTF-8 encoding.
[[nodiscard]] FileSnapshot read_file(const std::filesystem::path& path,
                                    std::size_t max_bytes = default_max_file_bytes,
                                    std::optional<TextEncoding> encoding_override = std::nullopt);

// No expected version means create-new only. Overwriting requires the version
// returned by read_file/save_file for the same path and a byte-for-byte match.
// Content is flushed before an atomic same-volume replacement. On Windows a held
// read handle denies competing writes during the final check and replacement;
// external rename operations are not a transactional compare-and-swap guarantee.
[[nodiscard]] FileVersion save_file(const std::filesystem::path& path,
                                    std::string_view utf8,
                                    TextEncoding encoding = TextEncoding::utf8,
                                    std::optional<FileVersion> expected = std::nullopt);

} // namespace astra::io
