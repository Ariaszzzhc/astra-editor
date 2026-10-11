#include <astra/ui/clipboard_text.hpp>
#include <SDL3/SDL.h>
#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::ui {
namespace {
#ifdef _WIN32
[[noreturn]] void fail(std::string_view operation) {
    const auto code = GetLastError();
    throw std::runtime_error("Clipboard read failed: " + std::string(operation) +
                             " (Windows error " + std::to_string(code) + ")");
}

class ClipboardOpen final {
  public:
    ClipboardOpen() {
        if (!OpenClipboard(nullptr))
            fail("OpenClipboard");
    }

    ~ClipboardOpen() {
        if (open_ && !CloseClipboard())
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Clipboard lease cleanup failed: %lu",
                         GetLastError());
    }

    ClipboardOpen(const ClipboardOpen&) = delete;
    ClipboardOpen& operator=(const ClipboardOpen&) = delete;

    void close() {
        if (!CloseClipboard())
            fail("CloseClipboard");
        open_ = false;
    }

  private:
    bool open_{true};
};

class ClipboardMemory final {
  public:
    explicit ClipboardMemory(HGLOBAL borrowed)
        : handle_(borrowed), text_(static_cast<const wchar_t*>(GlobalLock(borrowed))) {
        if (!text_)
            fail("GlobalLock");
    }

    ~ClipboardMemory() {
        if (locked_ && !unlock())
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Clipboard memory cleanup failed: %lu",
                         GetLastError());
    }

    ClipboardMemory(const ClipboardMemory&) = delete;
    ClipboardMemory& operator=(const ClipboardMemory&) = delete;

    const wchar_t* text() const noexcept {
        return text_;
    }

    void close() {
        if (!unlock())
            fail("GlobalUnlock");
        locked_ = false;
    }

  private:
    bool unlock() noexcept {
        SetLastError(ERROR_SUCCESS);
        return GlobalUnlock(handle_) != FALSE || GetLastError() == ERROR_SUCCESS;
    }

    HGLOBAL handle_;      // Borrowed OS clipboard data; never freed by this reader.
    const wchar_t* text_; // Borrowed only while this object holds the memory lock.
    bool locked_{true};
};
#else
struct SdlStringDeleter {
    void operator()(char* value) const noexcept {
        SDL_free(value);
    }
};
#endif
} // namespace

std::string read_clipboard_text(std::size_t max_bytes) {
    if (max_bytes == 0 || max_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("Invalid clipboard byte limit");
#ifdef _WIN32
    // SDL 3.4.12's Win32 reader can return empty text without an SDL error when
    // OpenClipboard fails. Acquire explicitly so contention cannot become data.
    ClipboardOpen clipboard;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        clipboard.close();
        return {};
    }
    const auto data = GetClipboardData(CF_UNICODETEXT);
    if (!data)
        fail("GetClipboardData");
    const auto storage = GlobalSize(data);
    if (storage < sizeof(wchar_t) || storage % sizeof(wchar_t) != 0)
        throw std::runtime_error("Clipboard read failed: invalid Unicode storage");
    std::string result;
    {
        ClipboardMemory memory(data);
        const auto extent = std::min(storage / sizeof(wchar_t), max_bytes + 1);
        std::size_t units{};
        while (units < extent && memory.text()[units])
            ++units;
        if (units == extent)
            throw std::runtime_error(
                "Clipboard read failed: unterminated or oversized Unicode text");
        if (units != 0) {
            const auto count = static_cast<int>(units);
            const auto bytes = WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, memory.text(), count, nullptr, 0, nullptr, nullptr);
            if (!bytes)
                fail("Unicode conversion");
            if (static_cast<std::size_t>(bytes) > max_bytes)
                throw std::runtime_error("Clipboard exceeds the input byte limit");
            result.resize(static_cast<std::size_t>(bytes));
            if (WideCharToMultiByte(CP_UTF8,
                                    WC_ERR_INVALID_CHARS,
                                    memory.text(),
                                    count,
                                    result.data(),
                                    bytes,
                                    nullptr,
                                    nullptr) != bytes)
                fail("Unicode conversion");
        }
        memory.close();
    }
    clipboard.close();
    return result;
#else
    SDL_ClearError();
    const std::unique_ptr<char, SdlStringDeleter> text(SDL_GetClipboardText());
    const auto* failure = SDL_GetError();
    if (!text || (text.get()[0] == '\0' && *failure))
        throw std::runtime_error(std::string("Clipboard read failed: ") +
                                 (*failure ? failure : "SDL returned no data"));
    std::size_t bytes{};
    while (bytes < max_bytes && text.get()[bytes])
        ++bytes;
    if (text.get()[bytes])
        throw std::runtime_error("Clipboard exceeds the input byte limit");
    return {text.get(), bytes};
#endif
}
} // namespace astra::ui
