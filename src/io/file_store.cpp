#include <astra/io/file_store.hpp>

#include <astra/core/utf8.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <random>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace astra::io {
namespace {

namespace fs = std::filesystem;

fs::path normalized_path(const fs::path& path) {
    if (path.empty() || path.filename().empty() ||
        path.native().find(fs::path::value_type{}) != fs::path::string_type::npos) {
        throw std::invalid_argument("file path is empty or contains NUL");
    }
    return fs::weakly_canonical(fs::absolute(path));
}

void append_utf8(std::string& output, std::uint32_t scalar) {
    if (scalar <= 0x7fU) {
        output.push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7ffU) {
        output.push_back(static_cast<char>(0xc0U | (scalar >> 6U)));
        output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else if (scalar <= 0xffffU) {
        output.push_back(static_cast<char>(0xe0U | (scalar >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (scalar >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
    }
}

void validate_text(std::string_view text) {
    utf8::validate(text);
    if (text.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("binary NUL is not supported in text files");
    }
}

std::pair<std::string, TextEncoding> decode_file(std::string_view raw) {
    if (raw.starts_with("\xef\xbb\xbf")) {
        raw.remove_prefix(3);
        validate_text(raw);
        return {std::string(raw), TextEncoding::utf8_bom};
    }
    const bool little = raw.starts_with("\xff\xfe");
    const bool big = raw.starts_with("\xfe\xff");
    if (!little && !big) {
        validate_text(raw);
        return {std::string(raw), TextEncoding::utf8};
    }
    raw.remove_prefix(2);
    if (raw.size() % 2 != 0) {
        throw std::invalid_argument("UTF-16 file has an incomplete code unit");
    }
    const auto unit = [raw, little](std::size_t offset) -> std::uint32_t {
        const auto first = static_cast<std::uint32_t>(static_cast<unsigned char>(raw[offset]));
        const auto second = static_cast<std::uint32_t>(static_cast<unsigned char>(raw[offset + 1]));
        return little ? first | (second << 8U) : (first << 8U) | second;
    };
    std::string text;
    text.reserve(raw.size());
    for (std::size_t offset = 0; offset < raw.size(); offset += 2) {
        auto scalar = unit(offset);
        if (scalar >= 0xd800U && scalar <= 0xdbffU) {
            if (raw.size() - offset < 4) {
                throw std::invalid_argument("UTF-16 file has an unmatched high surrogate");
            }
            const auto low = unit(offset + 2);
            if (low < 0xdc00U || low > 0xdfffU) {
                throw std::invalid_argument("UTF-16 file has an invalid surrogate pair");
            }
            scalar = 0x10000U + ((scalar - 0xd800U) << 10U) + low - 0xdc00U;
            offset += 2;
        } else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
            throw std::invalid_argument("UTF-16 file has an unmatched low surrogate");
        }
        if (scalar == 0) {
            throw std::invalid_argument("binary NUL is not supported in text files");
        }
        append_utf8(text, scalar);
    }
    return {std::move(text), little ? TextEncoding::utf16_le : TextEncoding::utf16_be};
}

std::string encode_file(std::string_view text, TextEncoding encoding) {
    validate_text(text);
    if (encoding == TextEncoding::utf8) { return std::string(text); }
    if (encoding == TextEncoding::utf8_bom) { return std::string("\xef\xbb\xbf") + std::string(text); }
    if (encoding == TextEncoding::latin1) {
        std::string raw;
        raw.reserve(text.size());
        for (std::size_t offset = 0; offset < text.size();) {
            const auto scalar = utf8::decode(text, offset);
            if (scalar.value > 0xff) { throw std::invalid_argument("text cannot be represented in Latin-1"); }
            raw.push_back(static_cast<char>(scalar.value));
            offset += scalar.bytes;
        }
        return raw;
    }
    if (encoding != TextEncoding::utf16_le && encoding != TextEncoding::utf16_be) {
        throw std::invalid_argument("unknown file encoding");
    }
    const bool little = encoding == TextEncoding::utf16_le;
    std::string raw = little ? "\xff\xfe" : "\xfe\xff";
    const auto append_unit = [&raw, little](std::uint32_t unit) {
        const auto low = static_cast<char>(unit & 0xffU);
        const auto high = static_cast<char>((unit >> 8U) & 0xffU);
        raw.push_back(little ? low : high);
        raw.push_back(little ? high : low);
    };
    for (std::size_t offset = 0; offset < text.size();) {
        const auto decoded = utf8::decode(text, offset);
        auto scalar = static_cast<std::uint32_t>(decoded.value);
        offset += decoded.bytes;
        if (scalar <= 0xffffU) {
            append_unit(scalar);
        } else {
            scalar -= 0x10000U;
            append_unit(0xd800U | (scalar >> 10U));
            append_unit(0xdc00U | (scalar & 0x3ffU));
        }
    }
    return raw;
}

std::string unique_component() {
    std::random_device random;
    constexpr char digits[] = "0123456789abcdef";
    std::string result = ".astra-";
    for (int block = 0; block < 4; ++block) {
        const auto value = static_cast<std::uint32_t>(random());
        for (unsigned int shift = 0; shift < 32; shift += 4) {
            result.push_back(digits[(value >> shift) & 0xfU]);
        }
    }
    return result + ".tmp";
}

void report_cleanup_failure(const char* operation) noexcept {
    // Destructors cannot throw. Emit a diagnostic instead of silently swallowing
    // cleanup failures; the original operation's error remains the primary result.
    std::fputs(operation, stderr);
    std::fputs("; temporary file may require cleanup\n", stderr);
}

#ifdef _WIN32

std::wstring native_path(const fs::path& path) {
    auto value = path.native();
    if (value.starts_with(L"\\\\?\\")) { return value; }
    if (value.starts_with(L"\\\\")) { return L"\\\\?\\UNC\\" + value.substr(2); }
    return L"\\\\?\\" + value;
}

[[noreturn]] void fail(const char* operation, const fs::path& path, DWORD error = GetLastError()) {
    throw fs::filesystem_error(operation, path,
        std::error_code(static_cast<int>(error), std::system_category()));
}

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE handle) : handle_(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            close_noexcept();
            handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~Handle() { close_noexcept(); }
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    void close(const fs::path& path) {
        if (handle_ != INVALID_HANDLE_VALUE) {
            const auto handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
            if (!CloseHandle(handle)) { fail("close file", path); }
        }
    }

private:
    void close_noexcept() noexcept {
        if (handle_ != INVALID_HANDLE_VALUE && !CloseHandle(handle_)) {
            report_cleanup_failure("CloseHandle failed");
        }
        handle_ = INVALID_HANDLE_VALUE;
    }
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

Handle open_read(const fs::path& path, bool for_save = false) {
    const auto handle = CreateFileW(native_path(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (for_save && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            throw FileConflict("file was removed since it was read");
        }
        fail("open file for reading", path, error);
    }
    Handle result(handle);
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) { fail("inspect file", path); }
    if (GetFileType(handle) != FILE_TYPE_DISK ||
        (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        throw std::invalid_argument("path does not refer to a regular file");
    }
    if (for_save && (information.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        fail("save read-only file", path, ERROR_ACCESS_DENIED);
    }
    return result;
}

std::size_t file_size(const Handle& handle, const fs::path& path, std::size_t limit) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size)) { fail("read file size", path); }
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit) {
        throw std::length_error("file exceeds the configured byte limit");
    }
    return static_cast<std::size_t>(size.QuadPart);
}

std::size_t read_some(const Handle& handle, const fs::path& path, char* output, std::size_t size) {
    DWORD read = 0;
    if (!ReadFile(handle.get(), output, static_cast<DWORD>(size), &read, nullptr)) {
        fail("read file", path);
    }
    return static_cast<std::size_t>(read);
}

class TemporaryFile {
public:
    explicit TemporaryFile(const fs::path& parent) {
        for (int attempt = 0; attempt < 32; ++attempt) {
            path_ = parent / unique_component();
            native_path_ = native_path(path_);
            const auto raw = CreateFileW(native_path_.c_str(), GENERIC_WRITE,
                0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (raw != INVALID_HANDLE_VALUE) {
                handle_ = Handle(raw);
                owned_ = true;
                return;
            }
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                fail("create temporary file", path_, error);
            }
        }
        throw std::runtime_error("unable to allocate a unique temporary file name");
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    ~TemporaryFile() {
        handle_ = Handle{};
        if (owned_ && !DeleteFileW(native_path_.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            report_cleanup_failure("DeleteFileW failed");
        }
    }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    void write(std::string_view raw) {
        std::size_t offset = 0;
        while (offset < raw.size()) {
            const auto count = static_cast<DWORD>(std::min<std::size_t>(raw.size() - offset, 64U * 1024U));
            DWORD written = 0;
            if (!WriteFile(handle_.get(), raw.data() + offset, count, &written, nullptr)) {
                fail("write temporary file", path_);
            }
            if (written == 0) { fail("write temporary file made no progress", path_, ERROR_WRITE_FAULT); }
            offset += written;
        }
        if (!FlushFileBuffers(handle_.get())) { fail("flush temporary file", path_); }
        handle_.close(path_);
    }
    void close() { handle_.close(path_); }
    void release() noexcept { owned_ = false; }

private:
    fs::path path_;
    std::wstring native_path_;
    Handle handle_;
    bool owned_{};
};

class ReplacementBackup {
public:
    explicit ReplacementBackup(const fs::path& parent) {
        for (int attempt = 0; attempt < 32; ++attempt) {
            directory_ = parent / unique_component();
            path_ = directory_ / "original";
            native_directory_ = native_path(directory_);
            native_path_ = native_path(path_);
            // Reserve the namespace atomically, but leave the backup file absent.
            // ReplaceFileW cannot overwrite an open backup placeholder, even
            // when all of its readers grant FILE_SHARE_DELETE (error 1175).
            if (CreateDirectoryW(native_directory_.c_str(), nullptr)) { owned_ = true; return; }
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
                fail("reserve replacement backup directory", directory_, error);
            }
        }
        throw std::runtime_error("unable to reserve a replacement backup directory");
    }
    ReplacementBackup(const ReplacementBackup&) = delete;
    ReplacementBackup& operator=(const ReplacementBackup&) = delete;
    ~ReplacementBackup() {
        if (!owned_) { return; }
        if (!DeleteFileW(native_path_.c_str())) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                report_cleanup_failure("DeleteFileW replacement backup failed");
                return; // Preserve its directory when the original is still present.
            }
        }
        if (!RemoveDirectoryW(native_directory_.c_str())) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                report_cleanup_failure("RemoveDirectoryW replacement backup failed");
            }
        }
    }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    void release() noexcept { owned_ = false; }
    // A successful restoration moved original back out; clean the empty reserve.
    void reclaim_empty() noexcept { owned_ = true; }

private:
    fs::path directory_;
    fs::path path_;
    std::wstring native_directory_;
    std::wstring native_path_;
    bool owned_{};
};

#else

[[noreturn]] void fail(const char* operation, const fs::path& path, int error = errno) {
    throw fs::filesystem_error(operation, path, std::error_code(error, std::generic_category()));
}

class Handle {
public:
    Handle() = default;
    explicit Handle(int descriptor) : descriptor_(descriptor) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            close_noexcept();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    ~Handle() { close_noexcept(); }
    [[nodiscard]] int get() const noexcept { return descriptor_; }
    void close(const fs::path& path) {
        if (descriptor_ != -1 && ::close(std::exchange(descriptor_, -1)) != 0) { fail("close file", path); }
    }
private:
    void close_noexcept() noexcept {
        if (descriptor_ != -1 && ::close(descriptor_) != 0) { report_cleanup_failure("close failed"); }
        descriptor_ = -1;
    }
    int descriptor_{-1};
};

Handle open_read(const fs::path& path, bool for_save = false) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor == -1) {
        if (for_save && errno == ENOENT) { throw FileConflict("file was removed since it was read"); }
        fail("open file for reading", path);
    }
    Handle result(descriptor);
    struct stat information {};
    if (::fstat(descriptor, &information) != 0) { fail("inspect file", path); }
    if (!S_ISREG(information.st_mode)) { throw std::invalid_argument("path does not refer to a regular file"); }
    if (for_save && (information.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) {
        fail("save read-only file", path, EACCES);
    }
    return result;
}

std::size_t file_size(const Handle& handle, const fs::path& path, std::size_t limit) {
    struct stat information {};
    if (::fstat(handle.get(), &information) != 0) { fail("read file size", path); }
    if (information.st_size < 0 || static_cast<std::uintmax_t>(information.st_size) > limit) {
        throw std::length_error("file exceeds the configured byte limit");
    }
    return static_cast<std::size_t>(information.st_size);
}

std::size_t read_some(const Handle& handle, const fs::path& path, char* output, std::size_t size) {
    ssize_t count = 0;
    do { count = ::read(handle.get(), output, size); } while (count < 0 && errno == EINTR);
    if (count < 0) { fail("read file", path); }
    return static_cast<std::size_t>(count);
}

class TemporaryFile {
public:
    explicit TemporaryFile(const fs::path& parent) {
        for (int attempt = 0; attempt < 32; ++attempt) {
            path_ = parent / unique_component();
            const auto descriptor = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (descriptor >= 0) { handle_ = Handle(descriptor); owned_ = true; return; }
            if (errno != EEXIST) { fail("create temporary file", path_); }
        }
        throw std::runtime_error("unable to allocate a unique temporary file name");
    }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    ~TemporaryFile() {
        handle_ = Handle{};
        if (owned_ && ::unlink(path_.c_str()) != 0 && errno != ENOENT) {
            report_cleanup_failure("unlink failed");
        }
    }
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }
    void write(std::string_view raw) {
        std::size_t offset = 0;
        while (offset < raw.size()) {
            const auto count = ::write(handle_.get(), raw.data() + offset,
                std::min<std::size_t>(raw.size() - offset, 64U * 1024U));
            if (count < 0 && errno == EINTR) { continue; }
            if (count <= 0) { fail("write temporary file", path_, count == 0 ? EIO : errno); }
            offset += static_cast<std::size_t>(count);
        }
        if (::fsync(handle_.get()) != 0) { fail("flush temporary file", path_); }
        handle_.close(path_);
    }
    void close() { handle_.close(path_); }
    void release() noexcept { owned_ = false; }
private:
    fs::path path_;
    Handle handle_;
    bool owned_{};
};

#endif

std::string read_raw(const fs::path& path, std::size_t limit) {
    const auto handle = open_read(path);
    const auto size = file_size(handle, path, limit);
    std::string raw(size, '\0');
    std::size_t offset = 0;
    while (offset < size) {
        const auto count = read_some(handle, path, raw.data() + offset,
            std::min<std::size_t>(size - offset, 64U * 1024U));
        if (count == 0) { throw FileConflict("file shrank while being read"); }
        offset += count;
    }
    if (file_size(handle, path, std::numeric_limits<std::size_t>::max()) != size) {
        throw FileConflict("file changed size while being read");
    }
    return raw;
}

Handle verify_expected(const fs::path& path, const FileVersion& expected) {
    auto handle = open_read(path, true);
    if (file_size(handle, path, std::numeric_limits<std::size_t>::max()) != expected.raw_content->size()) {
        throw FileConflict("file changed on disk since it was read");
    }
    std::array<char, 64U * 1024U> bytes{};
    std::size_t offset = 0;
    while (offset < expected.raw_content->size()) {
        const auto count = read_some(handle, path, bytes.data(),
            std::min(bytes.size(), expected.raw_content->size() - offset));
        if (count == 0 || std::string_view(bytes.data(), count) !=
            std::string_view(*expected.raw_content).substr(offset, count)) {
            throw FileConflict("file changed on disk since it was read");
        }
        offset += count;
    }
    return handle;
}

void install_file(const fs::path& path, TemporaryFile& temporary,
                  const std::optional<FileVersion>& expected) {
#ifdef _WIN32
    if (!expected) {
        if (!MoveFileExW(native_path(temporary.path()).c_str(), native_path(path).c_str(), MOVEFILE_WRITE_THROUGH)) {
            const auto error = GetLastError();
            if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
                throw FileConflict("creating a file would overwrite an existing path");
            }
            fail("install new file", path, error);
        }
        temporary.release();
        return;
    }
    ReplacementBackup backup(path.parent_path());
    auto guard = verify_expected(path, *expected);
    if (!ReplaceFileW(native_path(path).c_str(), native_path(temporary.path()).c_str(),
        native_path(backup.path()).c_str(), 0, nullptr, nullptr)) {
        const auto error = GetLastError();
        if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
            // This path now owns the original. Preserve it before anything else
            // can throw, including allocation or a failed restoration attempt.
            backup.release();
            if (!MoveFileExW(native_path(backup.path()).c_str(), native_path(path).c_str(), MOVEFILE_WRITE_THROUGH)) {
                throw FileRecoveryRequired(path, backup.path());
            }
            backup.reclaim_empty();
        }
        fail("replace file", path, error);
    }
    temporary.release();
#else
    // Native POSIX implementation; it has not been exercised on this Windows host.
    Handle guard;
    if (expected) {
        guard = verify_expected(path, *expected);
        struct stat information {};
        if (::fstat(guard.get(), &information) != 0) { fail("inspect existing permissions", path); }
        if (::chmod(temporary.path().c_str(), information.st_mode & 07777) != 0) {
            fail("preserve file permissions", temporary.path());
        }
        if (::rename(temporary.path().c_str(), path.c_str()) != 0) { fail("replace file", path); }
        temporary.release();
    } else {
        // link() refuses existing destinations, avoiding a check/rename overwrite race.
        if (::link(temporary.path().c_str(), path.c_str()) != 0) {
            if (errno == EEXIST) { throw FileConflict("creating a file would overwrite an existing path"); }
            fail("install new file", path);
        }
    }
    Handle directory(::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.get() == -1 || ::fsync(directory.get()) != 0) {
        fail("file installed but directory synchronization failed", path);
    }
#endif
}

} // namespace

FileConflict::FileConflict(const std::string& message) : std::runtime_error(message) {}

FileRecoveryRequired::FileRecoveryRequired(fs::path original, fs::path backup)
    : std::runtime_error("file replacement failed; original data is retained at the backup path"),
      original_(std::move(original)), backup_(std::move(backup)) {}

const fs::path& FileRecoveryRequired::original_path() const noexcept { return original_; }
const fs::path& FileRecoveryRequired::backup_path() const noexcept { return backup_; }

DecodedText decode_bytes(std::string_view raw, std::optional<TextEncoding> encoding_override) {
    if (encoding_override == TextEncoding::latin1) {
        std::string text;
        text.reserve(raw.size());
        for (const auto character : raw) {
            if (character == '\0') { throw std::invalid_argument("binary NUL is not supported in text files"); }
            append_utf8(text, static_cast<unsigned char>(character));
        }
        return {std::move(text), TextEncoding::latin1};
    }
    auto [text, encoding] = decode_file(raw);
    if (encoding_override && *encoding_override != encoding) {
        throw std::invalid_argument("file encoding does not match the requested encoding");
    }
    return {std::move(text), encoding};
}

FileSnapshot read_file(const fs::path& input_path, std::size_t max_bytes,
                       std::optional<TextEncoding> encoding_override) {
    const auto path = normalized_path(input_path);
    auto raw = std::make_shared<const std::string>(read_raw(path, max_bytes));
    auto decoded = decode_bytes(*raw, encoding_override);
    return {std::move(decoded.text), decoded.encoding, FileVersion{path, std::move(raw)}};
}

FileVersion save_file(const fs::path& input_path, std::string_view text,
                      TextEncoding encoding, std::optional<FileVersion> expected) {
    const auto path = normalized_path(input_path);
    if (expected && (!expected->raw_content || expected->path != path)) {
        throw std::invalid_argument("expected file version does not belong to this path");
    }
    // Validate and allocate the final version before any persistent mutation.
    auto raw = std::make_shared<const std::string>(encode_file(text, encoding));
    FileVersion result{path, raw};
    TemporaryFile temporary(path.parent_path());
    temporary.write(*raw);
    install_file(path, temporary, expected);
    return result;
}

} // namespace astra::io
