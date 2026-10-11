#include <astra/io/file_store.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using astra::io::FileConflict;
using astra::io::FileVersion;
using astra::io::read_file;
using astra::io::save_file;
using astra::io::TextEncoding;

bool strictly_within(const fs::path& child, const fs::path& parent) {
    const auto relative = child.lexically_relative(parent);
    return !relative.empty() && relative != "." && !relative.is_absolute() &&
           std::none_of(relative.begin(), relative.end(), [](const fs::path& part) {
               return part == "..";
           });
}

class TestDirectory {
  public:
    TestDirectory() {
        const auto workspace = fs::canonical(fs::current_path());
        root_ = fs::weakly_canonical(workspace / "build");
        if (!strictly_within(root_, workspace)) {
            throw std::runtime_error("test build directory escaped the current workspace");
        }
        (void)fs::create_directories(root_);
        std::random_device random;
        for (int attempt = 0; attempt < 32; ++attempt) {
            const auto candidate = root_ / ("file-store-test-" + std::to_string(random()) + "-" +
                                            std::to_string(random()));
            if (fs::create_directory(candidate)) {
                path_ = fs::canonical(candidate);
                if (!strictly_within(path_, root_)) {
                    throw std::runtime_error("test directory escaped build root");
                }
                return;
            }
        }
        throw std::runtime_error("cannot create unique test directory");
    }

    TestDirectory(const TestDirectory&) = delete;
    TestDirectory& operator=(const TestDirectory&) = delete;

    ~TestDirectory() {
        // Resolve immediately before recursive deletion, and never delete a root,
        // another test's directory or a link redirected outside this build tree.
        std::error_code error;
        const auto resolved = fs::weakly_canonical(path_, error);
        if (error || resolved != path_ || !strictly_within(resolved, root_)) {
            std::fputs("refusing unsafe file-store test cleanup\n", stderr);
            return;
        }
        (void)fs::remove_all(resolved, error);
        if (error) {
            std::fputs("file-store test cleanup failed\n", stderr);
        }
    }

    [[nodiscard]] const fs::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] fs::path file(std::string_view name = "test.txt") const {
        return path_ / name;
    }

    [[nodiscard]] std::size_t file_count() const {
        return static_cast<std::size_t>(
            std::distance(fs::directory_iterator(path_), fs::directory_iterator{}));
    }

  private:
    fs::path root_;
    fs::path path_;
};

std::string bytes(std::initializer_list<unsigned int> values) {
    std::string result;
    for (const auto value : values) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

void write_raw(const fs::path& path, std::string_view raw) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.exceptions(std::ios::failbit | std::ios::badbit);
    stream.write(raw.data(), static_cast<std::streamsize>(raw.size()));
    stream.close();
}

std::string raw_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    stream.exceptions(std::ios::badbit);
    if (!stream) {
        throw std::runtime_error("cannot read test file");
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>{}};
}

} // namespace

TEST_CASE("pure file decoding preserves strict encoding and raw newline semantics") {
    using astra::io::decode_bytes;
    const auto utf8 = decode_bytes("\xef\xbb\xbf"
                                   "hello\r\n");
    CHECK(utf8.encoding == TextEncoding::utf8_bom);
    CHECK(utf8.text == "hello\r\n");
    const auto utf16 = decode_bytes(std::string("\xff\xfe"
                                                "A\0\x3d\xd8\x00\xde",
                                                8));
    CHECK(utf16.encoding == TextEncoding::utf16_le);
    CHECK(utf16.text == "A\xf0\x9f\x98\x80");
    const auto latin = decode_bytes("caf\xe9", TextEncoding::latin1);
    CHECK(latin.encoding == TextEncoding::latin1);
    CHECK(latin.text == "caf\xc3\xa9");
    CHECK_THROWS_AS((void)decode_bytes("caf\xe9"), std::invalid_argument);
    CHECK_THROWS_AS((void)decode_bytes(std::string("x\0y", 3)), std::invalid_argument);
    CHECK_THROWS_AS((void)decode_bytes("text", TextEncoding::utf16_le), std::invalid_argument);
}

TEST_CASE("file store creates and replaces real files using explicit versions") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto created = save_file(path, "first\r\nsecond\n");
    CHECK(*created.raw_content == "first\r\nsecond\n");
    CHECK(created.path == fs::canonical(path));
    const auto loaded = read_file(path);
    CHECK(loaded.text == "first\r\nsecond\n");
    CHECK(loaded.encoding == TextEncoding::utf8);
    const auto changed = save_file(path, "replacement\r\n", loaded.encoding, loaded.version);
    CHECK(read_file(path).text == "replacement\r\n");
    CHECK(*changed.raw_content == raw_file(path));
    CHECK(*loaded.version.raw_content == "first\r\nsecond\n");
    CHECK(directory.file_count() == 1);
}

TEST_CASE("file store preserves UTF encodings and mixed line endings byte for byte") {
    TestDirectory directory;
    const std::string text = "A\xf0\x9f\x98\x80\r\n\xe4\xb8\xad"
                             "e\xcc\x81\n";
    const std::vector<std::pair<TextEncoding, std::string>> fixtures{
        {TextEncoding::utf8, text},
        {TextEncoding::utf8_bom, std::string("\xef\xbb\xbf") + text},
        {TextEncoding::utf16_le, bytes({0xff, 0xfe, 0x41, 0,    0x3d, 0xd8, 0, 0xde, 0x0d, 0,
                                        0x0a, 0,    0x2d, 0x4e, 0x65, 0,    1, 3,    0x0a, 0})},
        {TextEncoding::utf16_be, bytes({0xfe, 0xff, 0,    0x41, 0xd8, 0x3d, 0xde, 0, 0, 0x0d,
                                        0,    0x0a, 0x4e, 0x2d, 0,    0x65, 3,    1, 0, 0x0a})},
    };
    for (std::size_t index = 0; index < fixtures.size(); ++index) {
        CAPTURE(index);
        const auto path = directory.file(std::to_string(index));
        const auto& [encoding, raw] = fixtures[index];
        write_raw(path, raw);
        const auto loaded = read_file(path);
        CHECK(loaded.encoding == encoding);
        CHECK(loaded.text == text);
        REQUIRE_NOTHROW((void)save_file(path, loaded.text, loaded.encoding, loaded.version));
        CHECK(raw_file(path) == raw);
        const auto copy = directory.file("copy-" + std::to_string(index));
        REQUIRE_NOTHROW((void)save_file(copy, text, encoding));
        CHECK(raw_file(copy) == raw);
    }
    CHECK(directory.file_count() == 8);
}

TEST_CASE("file store handles empty files and BOM-only files") {
    TestDirectory directory;
    const std::array encodings{
        TextEncoding::utf8, TextEncoding::utf8_bom, TextEncoding::utf16_le, TextEncoding::utf16_be};
    const std::array expected{std::string{},
                              std::string("\xef\xbb\xbf"),
                              std::string("\xff\xfe"),
                              std::string("\xfe\xff")};
    for (std::size_t index = 0; index < encodings.size(); ++index) {
        const auto path = directory.file(std::to_string(index));
        REQUIRE_NOTHROW((void)save_file(path, "", encodings[index]));
        CHECK(raw_file(path) == expected[index]);
        const auto loaded = read_file(path);
        CHECK(loaded.text.empty());
        CHECK(loaded.encoding == encodings[index]);
    }
}

TEST_CASE("file store rejects malformed encoding and binary without replacement characters") {
    TestDirectory directory;
    const std::vector<std::string> invalid{
        bytes({0xc0, 0xaf}),                   // overlong UTF-8
        bytes({0xf0, 0x9f, 0x98}),             // truncated supplementary scalar
        bytes({0xed, 0xa0, 0x80}),             // UTF-8 encoded surrogate
        bytes({0x61, 0, 0x62}),                // binary NUL
        bytes({0xef, 0xbb, 0xbf, 0}),          // NUL after UTF-8 BOM
        bytes({0xff, 0xfe, 0x41}),             // odd UTF-16 byte count
        bytes({0xff, 0xfe, 0, 0xd8}),          // high surrogate without low
        bytes({0xfe, 0xff, 0xdc, 0}),          // unmatched low surrogate
        bytes({0xff, 0xfe, 0, 0xd8, 0x41, 0}), // high surrogate followed by A
        bytes({0xff, 0xfe, 0, 0}),             // UTF-16 NUL
        bytes({0xfe, 0xff, 0, 0}),             // big-endian UTF-16 NUL
        bytes({0, 0, 0xfe, 0xff}),             // unsupported UTF-32
    };
    for (std::size_t index = 0; index < invalid.size(); ++index) {
        CAPTURE(index);
        const auto path = directory.file(std::to_string(index));
        write_raw(path, invalid[index]);
        CHECK_THROWS_AS((void)read_file(path), std::invalid_argument);
        CHECK(raw_file(path) == invalid[index]);
    }
}

TEST_CASE("file store refuses implicit overwrites and detects same-size same-time conflicts") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto version = save_file(path, "before");
    CHECK_THROWS_AS((void)save_file(path, "wrong"), FileConflict);
    CHECK(raw_file(path) == "before");
    const auto timestamp = fs::last_write_time(path);
    write_raw(path, "change");
    fs::last_write_time(path, timestamp);
    CHECK_THROWS_AS((void)save_file(path, "my edit", TextEncoding::utf8, version), FileConflict);
    CHECK(raw_file(path) == "change");
    CHECK(directory.file_count() == 1);
}

TEST_CASE("file store detects missing and changed-length expected files") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto version = save_file(path, "initial");
    SUBCASE("external deletion") {
        REQUIRE(fs::remove(path));
        CHECK_THROWS_AS((void)save_file(path, "new", TextEncoding::utf8, version), FileConflict);
        CHECK_FALSE(fs::exists(path));
        CHECK(directory.file_count() == 0);
    }
    SUBCASE("external truncation") {
        write_raw(path, "x");
        CHECK_THROWS_AS((void)save_file(path, "new", TextEncoding::utf8, version), FileConflict);
        CHECK(raw_file(path) == "x");
        CHECK(directory.file_count() == 1);
    }
    SUBCASE("external growth") {
        write_raw(path, "initial changed externally");
        CHECK_THROWS_AS((void)save_file(path, "new", TextEncoding::utf8, version), FileConflict);
        CHECK(raw_file(path) == "initial changed externally");
        CHECK(directory.file_count() == 1);
    }
}

TEST_CASE("file store rejects tokens from other paths and invalid tokens") {
    TestDirectory directory;
    const auto first = directory.file("first");
    const auto second = directory.file("second");
    const auto first_version = save_file(first, "same");
    REQUIRE_NOTHROW((void)save_file(second, "same"));
    CHECK_THROWS_AS((void)save_file(second, "new", TextEncoding::utf8, first_version),
                    std::invalid_argument);
    const FileVersion empty_token{fs::canonical(first), {}};
    CHECK_THROWS_AS((void)save_file(first, "new", TextEncoding::utf8, empty_token),
                    std::invalid_argument);
    CHECK(raw_file(first) == "same");
    CHECK(raw_file(second) == "same");
    CHECK(directory.file_count() == 2);
}

TEST_CASE("file store rejects invalid save text before changing disk") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto version = save_file(path, "safe");
    CHECK_THROWS_AS((void)save_file(path, bytes({0x61, 0, 0x62}), TextEncoding::utf8, version),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        (void)save_file(path, bytes({0xf5, 0x80, 0x80, 0x80}), TextEncoding::utf16_le, version),
        std::invalid_argument);
    CHECK_THROWS_AS((void)save_file(path, "valid", static_cast<TextEncoding>(42), version),
                    std::invalid_argument);
    CHECK(raw_file(path) == "safe");
    CHECK(directory.file_count() == 1);
}

TEST_CASE("file store enforces byte limits and refuses directories") {
    TestDirectory directory;
    const auto path = directory.file();
    REQUIRE_NOTHROW((void)save_file(path, "1234"));
    CHECK_THROWS_AS((void)read_file(path, 3), std::length_error);
    CHECK(read_file(path, 4).text == "1234");
    CHECK_THROWS_AS((void)read_file(path, 0), std::length_error);
    CHECK_THROWS_AS((void)read_file(directory.path()), std::invalid_argument);
    CHECK_THROWS((void)save_file(directory.path(), "do not replace a directory"));
    CHECK(fs::is_directory(directory.path()));
    CHECK(raw_file(path) == "1234");
}

TEST_CASE("file store reports missing parents and invalid paths without creating files") {
    TestDirectory directory;
    CHECK_THROWS_AS((void)read_file(directory.file("missing")), fs::filesystem_error);
    CHECK_THROWS_AS((void)save_file(directory.path() / "missing" / "file", "text"),
                    fs::filesystem_error);
    CHECK_FALSE(fs::exists(directory.path() / "missing"));
    CHECK_THROWS_AS((void)save_file({}, "text"), std::invalid_argument);
    const auto embedded_nul = directory.path() / fs::path(std::string("a\0b", 3));
    CHECK_THROWS_AS((void)save_file(embedded_nul, "text"), std::invalid_argument);
    CHECK(directory.file_count() == 0);
}

TEST_CASE("file store handles Unicode filenames and substantial bounded chunks") {
    TestDirectory directory;
    const auto path = directory.path() / fs::path(u8"中文-😀-é.txt");
    std::string content;
    for (std::size_t index = 0; index < 40000; ++index) {
        content += "abc\xf0\x9f\x98\x80\r\n";
    }
    const auto version = save_file(path, content, TextEncoding::utf16_le);
    const auto loaded = read_file(path);
    CHECK(loaded.text == content);
    CHECK(loaded.encoding == TextEncoding::utf16_le);
    REQUIRE_NOTHROW((void)save_file(path, "smaller\n", loaded.encoding, version));
    CHECK(read_file(path).text == "smaller\n");
    CHECK(directory.file_count() == 1);
}

TEST_CASE("file store can change encoding only when explicitly requested") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto first = save_file(path, "text\r\n", TextEncoding::utf16_be);
    const auto second = save_file(path, "text\r\n", TextEncoding::utf8_bom, first);
    CHECK(read_file(path).encoding == TextEncoding::utf8_bom);
    CHECK(*second.raw_content == "\xef\xbb\xbftext\r\n");
    const auto third = save_file(path, "text\r\n", TextEncoding::utf8, second);
    CHECK(*third.raw_content == "text\r\n");
    CHECK(directory.file_count() == 1);
}

TEST_CASE("file store uses explicit Latin-1 and refuses unrepresentable saves") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto version = save_file(path, "caf\xc3\xa9\r\n\xc2\xa3", TextEncoding::latin1);
    CHECK(raw_file(path) == "caf\xe9\r\n\xa3");
    CHECK_THROWS_AS((void)read_file(path), std::invalid_argument);
    const auto loaded = read_file(path, astra::io::default_max_file_bytes, TextEncoding::latin1);
    CHECK(loaded.text == "caf\xc3\xa9\r\n\xc2\xa3");
    CHECK(loaded.encoding == TextEncoding::latin1);
    CHECK_THROWS_AS((void)save_file(path, "\xe4\xb8\xad", TextEncoding::latin1, version),
                    std::invalid_argument);
    CHECK(*version.raw_content == raw_file(path));
    CHECK(directory.file_count() == 1);
    write_raw(path, bytes({0xc3, 0xa9}));
    CHECK(read_file(path).text == "\xc3\xa9");
    CHECK(read_file(path, 10, TextEncoding::latin1).text == "\xc3\x83\xc2\xa9");
    write_raw(path, bytes({0}));
    CHECK_THROWS_AS((void)read_file(path, 10, TextEncoding::latin1), std::invalid_argument);
}

#ifdef _WIN32
namespace {

class ReadOnlyGuard {
  public:
    explicit ReadOnlyGuard(fs::path path)
        : path_(std::move(path)), attributes_(GetFileAttributesW(path_.c_str())) {
        if (attributes_ == INVALID_FILE_ATTRIBUTES ||
            !SetFileAttributesW(path_.c_str(), attributes_ | FILE_ATTRIBUTE_READONLY)) {
            throw std::runtime_error("cannot set read-only test attribute");
        }
    }

    ReadOnlyGuard(const ReadOnlyGuard&) = delete;
    ReadOnlyGuard& operator=(const ReadOnlyGuard&) = delete;

    ~ReadOnlyGuard() {
        if (!SetFileAttributesW(path_.c_str(), attributes_)) {
            std::fputs("failed to restore test attributes\n", stderr);
        }
    }

  private:
    fs::path path_;
    DWORD attributes_{};
};

class ReadHandleGuard {
  public:
    explicit ReadHandleGuard(const fs::path& path, DWORD sharing = 0)
        : handle_(CreateFileW(path.c_str(),
                              GENERIC_READ,
                              sharing,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr)) {
        if (handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("cannot lock test file");
        }
    }

    ReadHandleGuard(const ReadHandleGuard&) = delete;
    ReadHandleGuard& operator=(const ReadHandleGuard&) = delete;

    ~ReadHandleGuard() {
        if (!CloseHandle(handle_)) {
            std::fputs("failed to close test handle\n", stderr);
        }
    }

    [[nodiscard]] std::string read() const {
        std::string result;
        std::array<char, 4096> buffer{};
        for (;;) {
            DWORD count{};
            if (!ReadFile(
                    handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
                throw std::runtime_error("cannot read held file snapshot");
            }
            if (count == 0) {
                return result;
            }
            result.append(buffer.data(), count);
        }
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

} // namespace

TEST_CASE("Windows file store preserves read-only originals on save failure") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto version = save_file(path, "protected");
    {
        const ReadOnlyGuard read_only(path);
        CHECK_THROWS_AS((void)save_file(path, "attempt", TextEncoding::utf8, version),
                        fs::filesystem_error);
        CHECK(read_file(path).text == "protected");
        CHECK(directory.file_count() == 1);
    }
    CHECK(raw_file(path) == "protected");
}

TEST_CASE("Windows file store reports sharing failures and cleans owned temporary files") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto version = save_file(path, "protected");
    {
        const ReadHandleGuard lock(path);
        CHECK_THROWS_AS((void)save_file(path, "attempt", TextEncoding::utf8, version),
                        fs::filesystem_error);
        CHECK_THROWS_AS((void)read_file(path), fs::filesystem_error);
        CHECK(directory.file_count() == 1);
    }
    CHECK(raw_file(path) == "protected");
    REQUIRE_NOTHROW((void)save_file(path, "recovered", TextEncoding::utf8, version));
    CHECK(read_file(path).text == "recovered");
}

TEST_CASE("Windows file store replaces beside shared readers without invalidating old handles") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto original = save_file(path, "old snapshot");
    const ReadHandleGuard first(path, FILE_SHARE_READ | FILE_SHARE_DELETE);
    const ReadHandleGuard second(path, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    const auto saved = save_file(path, "new snapshot", TextEncoding::utf8, original);
    CHECK(read_file(path).text == "new snapshot");
    CHECK(first.read() == "old snapshot");
    CHECK(second.read() == "old snapshot");
    CHECK(directory.file_count() == 1);
    CHECK_THROWS_AS((void)save_file(path, "stale overwrite", TextEncoding::utf8, original),
                    FileConflict);
    CHECK(read_file(path).text == "new snapshot");
    REQUIRE_NOTHROW((void)save_file(path, "latest", TextEncoding::utf8, saved));
    CHECK(read_file(path).text == "latest");
    CHECK(directory.file_count() == 1);
}

TEST_CASE("Windows replacement rejects an open backup placeholder even with delete sharing") {
    TestDirectory directory;
    const auto path = directory.file();
    const auto replacement = directory.file("replacement.tmp");
    const auto placeholder = directory.file("placeholder.tmp");
    const auto original = save_file(path, "original");
    write_raw(replacement, "replacement");
    write_raw(placeholder, "");
    {
        const ReadHandleGuard held(placeholder,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
        const auto replaced = ReplaceFileW(
            path.c_str(), replacement.c_str(), placeholder.c_str(), 0, nullptr, nullptr);
        const auto error = GetLastError();
        CHECK_FALSE(replaced);
        CHECK(error == ERROR_UNABLE_TO_REMOVE_REPLACED);
        CHECK(read_file(path).text == "original");
        CHECK(raw_file(replacement) == "replacement");
        CHECK(held.read().empty());
    }
    REQUIRE(fs::remove(replacement));
    REQUIRE(fs::remove(placeholder));
    REQUIRE_NOTHROW((void)save_file(path, "replacement", TextEncoding::utf8, original));
    CHECK(read_file(path).text == "replacement");
    CHECK(directory.file_count() == 1);
}
#endif
