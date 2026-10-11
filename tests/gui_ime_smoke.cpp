#include <astra/ui/editor_window.hpp>
#include <astra/service/service_host.hpp>
#include <astra/io/file_store.hpp>
#include "support/temporary_directory.hpp"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/encode/SkPngEncoder.h"
#include <SDL3/SDL.h>
#include <windows.h>
#include <commctrl.h>
#include <imm.h>
#include <msctf.h>
#include <winrt/base.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
using Window = astra::ui::EditorWindow;
using Json = astra::protocol::Json;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

void checked(HRESULT result, const char* operation) {
    require(result == S_OK,
            std::string(operation) +
                " failed: " + std::to_string(static_cast<std::uint32_t>(result)));
}

struct Apartment {
    Apartment() {
        const auto result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        require(SUCCEEDED(result), "Cannot initialize installed-IME test apartment");
    }

    ~Apartment() {
        CoUninitialize();
    }

    Apartment(const Apartment&) = delete;
    Apartment& operator=(const Apartment&) = delete;
};

struct BstrDeleter {
    void operator()(wchar_t* value) const noexcept {
        SysFreeString(value);
    }
};

void environment() {
    BOOL in_job{};
    require(IsProcessInJob(GetCurrentProcess(), nullptr, &in_job) != 0,
            "Cannot read own IME test job state");
    std::cout << "IME test process: in job=" << in_job;
    if (in_job) {
        JOBOBJECT_BASIC_UI_RESTRICTIONS restrictions{};
        if (QueryInformationJobObject(nullptr,
                                      JobObjectBasicUIRestrictions,
                                      &restrictions,
                                      sizeof(restrictions),
                                      nullptr))
            std::cout << " UI restrictions=" << restrictions.UIRestrictionsClass;
        else
            std::cout << " UI restriction query error=" << GetLastError();
    }
    winrt::handle token;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) != 0,
            "Cannot read own IME test token");
    std::cout << " restricted token=" << IsTokenRestricted(token.get());
    DWORD size{};
    require(!GetTokenInformation(token.get(), TokenIntegrityLevel, nullptr, 0, &size) &&
                GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0 && size <= 64 * 1024,
            "Invalid own token integrity information bound");
    std::vector<std::byte> storage(size);
    require(GetTokenInformation(token.get(), TokenIntegrityLevel, storage.data(), size, &size) != 0,
            "Cannot read own IME test integrity level");
    const auto label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(storage.data());
    require(IsValidSid(label->Label.Sid) != 0, "Invalid own integrity SID");
    const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
    require(count > 0, "Own integrity SID has no level");
    std::cout << " integrity=" << *GetSidSubAuthority(label->Label.Sid, count - 1) << '\n';
}

class Profiles {
  public:
    Profiles() {
        checked(CoCreateInstance(CLSID_TF_InputProcessorProfiles,
                                 nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 __uuidof(ITfInputProcessorProfileMgr),
                                 manager_.put_void()),
                "Create TSF profile manager");
        winrt::com_ptr<IEnumTfInputProcessorProfiles> enumeration;
        checked(manager_->EnumProfiles(0x0804, enumeration.put()),
                "Enumerate installed Chinese profiles");
        for (std::size_t count = 0; count < 256; ++count) {
            TF_INPUTPROCESSORPROFILE profile{};
            ULONG fetched{};
            const auto result = enumeration->Next(1, &profile, &fetched);
            require(SUCCEEDED(result), "Installed profile enumeration failed");
            if (!fetched)
                break;
            std::cout << "Installed Chinese profile: type=" << profile.dwProfileType
                      << " flags=" << profile.dwFlags << " capabilities=" << profile.dwCaps;
            std::array<wchar_t, 40> guid{};
            require(StringFromGUID2(profile.clsid, guid.data(), static_cast<int>(guid.size())) > 0,
                    "Cannot describe installed IME identity");
            std::cout << " clsid=";
            for (const auto character : guid) {
                if (!character)
                    break;
                std::cout << static_cast<char>(character);
            }
            std::cout << '\n';
            if (profile.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR &&
                IsEqualGUID(profile.catid, GUID_TFCAT_TIP_KEYBOARD) &&
                (profile.dwFlags & TF_IPP_FLAG_ENABLED)) {
                candidates_.push_back(profile);
                const auto names = manager_.as<ITfInputProcessorProfiles>();
                std::unique_ptr<wchar_t, BstrDeleter> name;
                checked(names->GetLanguageProfileDescription(
                            profile.clsid, profile.langid, profile.guidProfile, std::out_ptr(name)),
                        "Describe selected installed IME");
                const auto length = SysStringLen(name.get());
                require(length > 0 && length <= 4096,
                        "Installed IME description exceeds its bound");
                const auto bytes = WideCharToMultiByte(CP_UTF8,
                                                       WC_ERR_INVALID_CHARS,
                                                       name.get(),
                                                       static_cast<int>(length),
                                                       nullptr,
                                                       0,
                                                       nullptr,
                                                       nullptr);
                require(bytes > 0, "Installed IME description is not valid Unicode");
                std::string caption(static_cast<std::size_t>(bytes), '\0');
                require(WideCharToMultiByte(CP_UTF8,
                                            WC_ERR_INVALID_CHARS,
                                            name.get(),
                                            static_cast<int>(length),
                                            caption.data(),
                                            bytes,
                                            nullptr,
                                            nullptr) == bytes,
                        "Cannot encode installed IME description");
                std::cout << "Enabled installed IME name: " << caption << '\n';
            }
            require(count < 255, "Installed profile enumeration exceeded its bound");
        }
        require(!candidates_.empty(), "No enabled installed Chinese keyboard IME profile");
    }

    void activate() {
        require(!original_, "IME fixture profile already activated");
        TF_INPUTPROCESSORPROFILE original{};
        checked(manager_->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &original),
                "Read original input profile");
        original_ = original;
        const auto& profile = candidates_.front();
        activate_profile(profile);
        TF_INPUTPROCESSORPROFILE active{};
        checked(manager_->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active),
                "Read activated IME identity");
        require(active.langid == profile.langid && IsEqualGUID(active.clsid, profile.clsid) &&
                    IsEqualGUID(active.guidProfile, profile.guidProfile),
                "Installed profile activation did not select the requested IME");
    }

    void restore() {
        if (original_) {
            activate_profile(*original_);
            TF_INPUTPROCESSORPROFILE active{};
            checked(manager_->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active),
                    "Read restored IME identity");
            const auto& saved = *original_;
            require(active.dwProfileType == saved.dwProfileType && active.langid == saved.langid &&
                        IsEqualGUID(active.clsid, saved.clsid) &&
                        IsEqualGUID(active.guidProfile, saved.guidProfile) &&
                        active.hkl == saved.hkl,
                    "Original process-local IME profile was not restored");
            original_.reset();
        }
    }

    ~Profiles() {
        if (original_) {
            const auto& p = *original_;
            const auto result = manager_->ActivateProfile(
                p.dwProfileType, p.langid, p.clsid, p.guidProfile, p.hkl, TF_IPPMF_FORPROCESS);
            if (result != S_OK)
                std::cerr << "IME failure cleanup could not restore profile: " << result << '\n';
        }
    }

    Profiles(const Profiles&) = delete;
    Profiles& operator=(const Profiles&) = delete;

  private:
    winrt::com_ptr<ITfInputProcessorProfileMgr> manager_;
    std::vector<TF_INPUTPROCESSORPROFILE> candidates_;
    std::optional<TF_INPUTPROCESSORPROFILE> original_;

    void activate_profile(const TF_INPUTPROCESSORPROFILE& p) {
        checked(
            manager_->ActivateProfile(p.dwProfileType,
                                      p.langid,
                                      p.clsid,
                                      p.guidProfile,
                                      p.hkl,
                                      TF_IPPMF_FORPROCESS | TF_IPPMF_DONTCARECURRENTINPUTLANGUAGE),
            "Activate process-local input profile");
    }
};

struct AwaitTimeout : std::runtime_error {
    using std::runtime_error::runtime_error;
};

template <class Predicate>
void await(Window& window, Predicate predicate, const std::string& message) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    do {
        require(window.pump(), "Installed-IME fixture window closed");
        require(window.last_error().empty(), window.last_error());
        if (predicate())
            return;
        SDL_Delay(2);
    } while (Clock::now() < deadline);
    throw AwaitTimeout(message);
}

struct CaptionVisibility {
    HWND window;
    bool restore;

    explicit CaptionVisibility(HWND owned)
        : window(owned), restore((GetWindowLongPtrW(owned, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0) {
        require(SetWindowPos(
                    window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) !=
                    0,
                "Cannot expose owned IME fixture caption");
    }

    ~CaptionVisibility() {
        if (restore &&
            !SetWindowPos(
                window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE))
            std::cerr << "IME fixture caption stacking restoration failed\n";
    }

    CaptionVisibility(const CaptionVisibility&) = delete;
    CaptionVisibility& operator=(const CaptionVisibility&) = delete;
};

std::string focus_description(HWND expected, HWND target) {
    const auto foreground = GetForegroundWindow();
    DWORD foreground_process{};
    GetWindowThreadProcessId(foreground, &foreground_process);
    std::string description =
        " foreground=" + std::to_string(foreground == expected) +
        " foregroundOwned=" + std::to_string(foreground_process == GetCurrentProcessId()) +
        " focus=" + std::to_string(GetFocus() == target);
    // Inspect only this fixture's window metadata, never another app's text.
    if (foreground_process == GetCurrentProcessId()) {
        std::array<wchar_t, 256> name{};
        const auto count = GetClassNameW(foreground, name.data(), static_cast<int>(name.size()));
        description += " ownedClass=";
        for (int index = 0; index < count; ++index)
            description += name[index] < 128 ? static_cast<char>(name[index]) : '?';
    }
    return description;
}

struct Focus {
    std::function<void()> pump;
    HWND window;
    HWND previous{GetForegroundWindow()};

    Focus(HWND owned, std::function<void()> message_pump)
        : pump(std::move(message_pump)), window(owned) {
        if (!SetForegroundWindow(window) && GetForegroundWindow() != window) {
            CaptionVisibility visible(window);
            // Actual focus input targets a verified point on this fixture's
            // caption, without changing desktop foreground-lock settings.
            require(SetWindowPos(
                        window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE) !=
                        0,
                    "Cannot raise owned IME window for focus");
            RECT bounds{};
            POINT cursor{};
            require(GetWindowRect(window, &bounds) && GetCursorPos(&cursor),
                    "Cannot read owned IME window focus coordinates");
            const auto dpi = GetDpiForWindow(window);
            require(dpi != 0, "Cannot measure owned native caption DPI");
            const auto border = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                                GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const POINT caption{bounds.left + (bounds.right - bounds.left) / 3,
                                bounds.top + border +
                                    GetSystemMetricsForDpi(SM_CYCAPTION, dpi) / 2};
            require(caption.x >= std::numeric_limits<short>::min() &&
                        caption.x <= std::numeric_limits<short>::max() &&
                        caption.y >= std::numeric_limits<short>::min() &&
                        caption.y <= std::numeric_limits<short>::max(),
                    "Owned caption is outside native hit-test coordinates");
            require(SendMessageW(window,
                                 WM_NCHITTEST,
                                 0,
                                 MAKELPARAM(static_cast<WORD>(caption.x),
                                            static_cast<WORD>(caption.y))) == HTCAPTION,
                    "Owned IME focus point is not inside the actual native caption");
            const auto target = GetAncestor(WindowFromPoint(caption), GA_ROOT);
            require(target == window,
                    "Owned IME window focus point is obscured: point=" + std::to_string(caption.x) +
                        "," + std::to_string(caption.y) + " bounds=" + std::to_string(bounds.left) +
                        "," + std::to_string(bounds.top) +
                        " owned=" + std::to_string(reinterpret_cast<std::uintptr_t>(window)) +
                        " target=" + std::to_string(reinterpret_cast<std::uintptr_t>(target)));
            const auto left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const auto top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const auto width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const auto height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            require(width > 1 && height > 1, "Invalid native desktop dimensions");
            std::array<INPUT, 3> inputs{};
            for (auto& input : inputs)
                input.type = INPUT_MOUSE;
            inputs[0].mi.dx = static_cast<LONG>((caption.x - left) * 65535LL / (width - 1));
            inputs[0].mi.dy = static_cast<LONG>((caption.y - top) * 65535LL / (height - 1));
            inputs[0].mi.dwFlags =
                MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            inputs[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            require((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0,
                    "A user-held pointer prevents isolated IME focus");
            const auto delivered = SendInput(3, inputs.data(), sizeof(INPUT));
            if (delivered == 2 && SendInput(1, &inputs[2], sizeof(INPUT)) != 1)
                std::cerr << "IME focus button-up recovery failed\n";
            require(SetCursorPos(cursor.x, cursor.y) != 0,
                    "Cannot restore pointer after IME focus");
            require(delivered == 3, "Cannot deliver owned IME caption focus click");
        }
        require(SetFocus(window) || GetFocus() == window, "Cannot set owned IME keyboard focus");
    }

    ~Focus() {
        if (previous && IsWindow(previous) && !SetForegroundWindow(previous))
            std::cerr << "IME fixture could not restore previous foreground window\n";
    }

    void key(WORD code, HWND target = nullptr) const {
        if (!target)
            target = window;
        require(GetAncestor(target, GA_ROOT) == window && GetForegroundWindow() == window &&
                    GetFocus() == target,
                "Owned IME fixture lost focus before OS keyboard injection" +
                    focus_description(window, target));
        for (const auto modifier : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN})
            require((GetAsyncKeyState(modifier) & 0x8000) == 0,
                    "A user-held modifier prevents isolated IME input");
        std::array<INPUT, 2> inputs{};
        const auto scan = MapVirtualKeyExW(code, MAPVK_VK_TO_VSC_EX, GetKeyboardLayout(0));
        require(scan != 0, "Installed layout cannot map the OS test key");
        for (auto& input : inputs) {
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = static_cast<WORD>(scan & 0xFF);
            input.ki.dwFlags = KEYEVENTF_SCANCODE | ((scan & 0xFF00) ? KEYEVENTF_EXTENDEDKEY : 0);
        }
        inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        require((GetAsyncKeyState(code) & 0x8000) == 0,
                "A user-held test key prevents isolated input");

        struct Release {
            INPUT input;
            bool pending{false};

            void release() {
                require(SendInput(1, &input, sizeof(INPUT)) == 1,
                        "OS IME key-up was not delivered");
                pending = false;
            }

            ~Release() {
                if (pending && SendInput(1, &input, sizeof(INPUT)) != 1)
                    std::cerr << "IME fixture key-up recovery failed\n";
            }
        } up{inputs[1]};

        require(SendInput(1, inputs.data(), sizeof(INPUT)) == 1,
                "OS IME key-down was not delivered");
        up.pending = true;
        const auto until = Clock::now() + std::chrono::milliseconds(35);
        do {
            require(GetForegroundWindow() == window && GetFocus() == target,
                    "Owned IME focus changed while delivering OS key" +
                        focus_description(window, target));
            pump();
            Sleep(1);
        } while (Clock::now() < until);
        up.release();
    }

    Focus(const Focus&) = delete;
    Focus& operator=(const Focus&) = delete;
};

struct InputContext {
    HWND window;
    HIMC context;

    explicit InputContext(HWND owned) : window(owned), context(ImmGetContext(window)) {
        require(context != nullptr, "Installed IME has no window input context");
    }

    ~InputContext() {
        if (!ImmReleaseContext(window, context))
            std::cerr << "IME fixture input context release failed\n";
    }

    InputContext(const InputContext&) = delete;
    InputContext& operator=(const InputContext&) = delete;
};

struct Context {
    HWND window;
    BOOL open;
    DWORD conversion{}, sentence{};
    bool pending{true};

    explicit Context(HWND owned) : window(owned), open(FALSE) {
        InputContext current(window);
        open = ImmGetOpenStatus(current.context);
        require(ImmGetConversionStatus(current.context, &conversion, &sentence) != 0,
                "Cannot read native IME conversion state");
        std::cout << "Original fixture IME mode: open=" << open << " conversion=" << conversion
                  << " sentence=" << sentence << '\n';
    }

    void enable() {
        InputContext current(window);
        require(ImmSetOpenStatus(current.context, TRUE) != 0, "Cannot open installed Chinese IME");
        const auto native_conversion = (conversion | IME_CMODE_NATIVE) & ~IME_CMODE_NOCONVERSION;
        require(ImmSetConversionStatus(current.context, native_conversion, sentence) != 0,
                "Cannot select native Chinese conversion");
    }

    void restore() {
        InputContext current(window);
        require(ImmNotifyIME(current.context, NI_COMPOSITIONSTR, CPS_CANCEL, 0) != 0,
                "IME fixture cleanup could not cancel composition");
        require(ImmSetConversionStatus(current.context, conversion, sentence) != 0 &&
                    ImmSetOpenStatus(current.context, open) != 0,
                "IME fixture cleanup could not restore conversion/open state");
        DWORD restored_conversion{}, restored_sentence{};
        require(ImmGetConversionStatus(current.context, &restored_conversion, &restored_sentence) &&
                    restored_conversion == conversion && restored_sentence == sentence &&
                    ImmGetOpenStatus(current.context) == open,
                "Original IME conversion/open state was not restored");
        pending = false;
    }

    ~Context() {
        if (pending) {
            try {
                restore();
            } catch (const std::exception& error) {
                std::cerr << error.what() << '\n';
            }
        }
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
};

std::optional<std::string> selected_native_candidate(HWND window) {
    InputContext current(window);
    const auto size = ImmGetCandidateListW(current.context, 0, nullptr, 0);
    if (size == 0)
        return std::nullopt;
    require(size >= sizeof(CANDIDATELIST) && size <= 64 * 1024,
            "Native candidate list exceeds fixture bounds");
    std::vector<DWORD> storage((size + sizeof(DWORD) - 1) / sizeof(DWORD));
    require(ImmGetCandidateListW(
                current.context, 0, reinterpret_cast<CANDIDATELIST*>(storage.data()), size) == size,
            "Native candidate list changed while reading");
    CANDIDATELIST header{};
    std::memcpy(&header, storage.data(), sizeof(header));
    const auto offsets = offsetof(CANDIDATELIST, dwOffset);
    require(header.dwSize <= size && header.dwCount > 0 && header.dwCount <= 4096 &&
                header.dwSelection < header.dwCount &&
                offsets + header.dwCount * sizeof(DWORD) <= header.dwSize,
            "Native candidate list has invalid selection/offset metadata");
    const auto* bytes = reinterpret_cast<const std::byte*>(storage.data());
    DWORD offset{};
    std::memcpy(&offset, bytes + offsets + header.dwSelection * sizeof(DWORD), sizeof(offset));
    require(offset >= offsets + header.dwCount * sizeof(DWORD) && offset < header.dwSize &&
                offset % sizeof(wchar_t) == 0,
            "Native selected candidate has an invalid text offset");
    std::wstring text((header.dwSize - offset) / sizeof(wchar_t), L'\0');
    std::memcpy(text.data(), bytes + offset, text.size() * sizeof(wchar_t));
    const auto end = text.find(L'\0');
    require(end != std::wstring::npos && end > 0, "Native selected candidate is not terminated");
    text.resize(end);
    const auto length = static_cast<int>(text.size());
    const auto encoded_size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
    require(encoded_size > 0 && encoded_size <= 64 * 1024, "Invalid native candidate Unicode");
    std::string result(static_cast<std::size_t>(encoded_size), '\0');
    require(WideCharToMultiByte(CP_UTF8,
                                WC_ERR_INVALID_CHARS,
                                text.data(),
                                length,
                                result.data(),
                                encoded_size,
                                nullptr,
                                nullptr) == encoded_size,
            "Cannot encode native selected candidate");
    std::cout << "Native selected candidate: style=" << header.dwStyle
              << " count=" << header.dwCount << " selection=" << header.dwSelection
              << " text=" << result.substr(0, 128) << '\n';
    return result;
}

class NativeImeTrace {
  public:
    explicit NativeImeTrace(HWND window) : window_(window) {
        require(GetWindowThreadProcessId(window_, nullptr) == GetCurrentThreadId(),
                "IME observation requires a test-owned window on this thread");
        require(IsWindowUnicode(window_) != 0,
                "Native character observation needs a Unicode window");
        require(SetWindowSubclass(window_, observe, 1, reinterpret_cast<DWORD_PTR>(this)) != 0,
                "Cannot observe synchronous native IME messages");
        attached_ = true;
    }

    ~NativeImeTrace() {
        if (attached_ && !RemoveWindowSubclass(window_, observe, 1))
            std::cerr << "Native IME observation cleanup failed\n";
    }

    void report(const char* label) const {
        if (error_)
            std::rethrow_exception(error_);
        std::cout << label << ": start=" << starts_ << " composition=" << compositions_
                  << " end=" << ends_ << " flags=" << flags_ << " comp bytes=" << comp_bytes_
                  << " reading bytes=" << reading_bytes_ << " result bytes=" << result_bytes_
                  << " candidate lists=" << candidate_lists_ << " chars=" << characters_
                  << " char units=" << character_text_.size() << '\n';
    }

    std::string result_text() const {
        if (error_)
            std::rethrow_exception(error_);
        if (!result_text_.empty() || character_text_.empty())
            return result_text_;
        // Some native text services deliver Unicode WM_CHAR without an IMM
        // result packet. Preserve that independent payload, including surrogate
        // pairs; never infer committed text from a candidate's display label.
        const auto length = static_cast<int>(character_text_.size());
        const auto bytes = WideCharToMultiByte(CP_UTF8,
                                               WC_ERR_INVALID_CHARS,
                                               character_text_.data(),
                                               length,
                                               nullptr,
                                               0,
                                               nullptr,
                                               nullptr);
        require(bytes > 0 && bytes <= 64 * 1024,
                "Native character result has invalid Unicode or size");
        std::string result(static_cast<std::size_t>(bytes), '\0');
        require(WideCharToMultiByte(CP_UTF8,
                                    WC_ERR_INVALID_CHARS,
                                    character_text_.data(),
                                    length,
                                    result.data(),
                                    bytes,
                                    nullptr,
                                    nullptr) == bytes,
                "Cannot encode native character result");
        return result;
    }

    NativeImeTrace(const NativeImeTrace&) = delete;
    NativeImeTrace& operator=(const NativeImeTrace&) = delete;

  private:
    HWND window_;
    bool attached_{};
    std::size_t starts_{}, compositions_{}, ends_{}, characters_{};
    LPARAM flags_{};
    LONG comp_bytes_{}, reading_bytes_{}, result_bytes_{};
    std::string result_text_;
    std::wstring character_text_;
    DWORD candidate_lists_{};
    std::exception_ptr error_;

    static LRESULT CALLBACK observe(HWND window,
                                    UINT message,
                                    WPARAM parameter,
                                    LPARAM flags,
                                    UINT_PTR,
                                    DWORD_PTR borrowed) noexcept {
        auto& self = *reinterpret_cast<NativeImeTrace*>(borrowed);
        try {
            if (message == WM_IME_STARTCOMPOSITION)
                ++self.starts_;
            if (message == WM_IME_ENDCOMPOSITION)
                ++self.ends_;
            if (message == WM_CHAR || message == WM_IME_CHAR)
                ++self.characters_;
            if (message == WM_CHAR) {
                require(parameter <= 0xFFFF && self.character_text_.size() < 32 * 1024,
                        "Native Unicode character observation exceeds fixture bounds");
                self.character_text_.push_back(static_cast<wchar_t>(parameter));
            }
            if (message == WM_IME_COMPOSITION) {
                require(++self.compositions_ <= 4096, "Native IME observation exceeded bound");
                self.flags_ |= flags;
                InputContext current(window);
                self.comp_bytes_ =
                    std::max(self.comp_bytes_,
                             ImmGetCompositionStringW(current.context, GCS_COMPSTR, nullptr, 0));
                self.reading_bytes_ = std::max(
                    self.reading_bytes_,
                    ImmGetCompositionStringW(current.context, GCS_COMPREADSTR, nullptr, 0));
                self.result_bytes_ =
                    std::max(self.result_bytes_,
                             ImmGetCompositionStringW(current.context, GCS_RESULTSTR, nullptr, 0));
                if ((flags & GCS_RESULTSTR) != 0) {
                    const auto bytes =
                        ImmGetCompositionStringW(current.context, GCS_RESULTSTR, nullptr, 0);
                    require(bytes >= 0 && bytes <= 64 * 1024 && bytes % sizeof(wchar_t) == 0,
                            "Native IME result exceeds fixture Unicode bounds");
                    std::wstring text(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
                    require(ImmGetCompositionStringW(current.context,
                                                     GCS_RESULTSTR,
                                                     text.data(),
                                                     static_cast<DWORD>(bytes)) == bytes,
                            "Native IME result changed while reading");
                    if (!text.empty()) {
                        const auto length = static_cast<int>(text.size());
                        const auto size = WideCharToMultiByte(CP_UTF8,
                                                              WC_ERR_INVALID_CHARS,
                                                              text.data(),
                                                              length,
                                                              nullptr,
                                                              0,
                                                              nullptr,
                                                              nullptr);
                        require(size > 0 && static_cast<std::size_t>(size) <=
                                                64 * 1024 - self.result_text_.size(),
                                "Native IME result has invalid Unicode or exceeds buffer bound");
                        std::string result(static_cast<std::size_t>(size), '\0');
                        require(WideCharToMultiByte(CP_UTF8,
                                                    WC_ERR_INVALID_CHARS,
                                                    text.data(),
                                                    length,
                                                    result.data(),
                                                    size,
                                                    nullptr,
                                                    nullptr) == size,
                                "Cannot encode native IME result");
                        self.result_text_ += result;
                    }
                }
                DWORD lists{};
                ImmGetCandidateListCountW(current.context, &lists);
                self.candidate_lists_ = std::max(self.candidate_lists_, lists);
            }
        } catch (...) {
            self.error_ = std::current_exception();
        }
        if (message == WM_NCDESTROY) {
            if (!RemoveWindowSubclass(window, observe, 1))
                std::cerr << "Native IME destruction observation cleanup failed\n";
            self.attached_ = false;
        }
        return DefSubclassProc(window, message, parameter, flags);
    }
};

struct VisibleNativeWindow {
    HWND window;
    DWORD thread;
    RECT bounds;
    std::string name;
};

std::vector<VisibleNativeWindow> owned_native_windows() {
    struct Observation {
        std::vector<VisibleNativeWindow> windows;
        std::exception_ptr error;
    } observation;

    const auto observe = [](HWND window, LPARAM borrowed) noexcept -> BOOL {
        auto& state = *reinterpret_cast<Observation*>(borrowed);
        DWORD process{};
        const auto thread = GetWindowThreadProcessId(window, &process);
        if (process != GetCurrentProcessId() || !IsWindowVisible(window))
            return TRUE;
        try {
            require(state.windows.size() < 256, "Owned visible-window observation exceeded bound");
            std::array<char, 256> name{};
            RECT bounds{};
            require(GetClassNameA(window, name.data(), static_cast<int>(name.size())) > 0 &&
                        GetWindowRect(window, &bounds),
                    "Cannot inspect test-owned visible native window");
            state.windows.push_back({window, thread, bounds, name.data()});
        } catch (...) {
            state.error = std::current_exception();
            return FALSE;
        }
        return TRUE;
    };
    const auto result = EnumWindows(observe, reinterpret_cast<LPARAM>(&observation));
    if (observation.error)
        std::rethrow_exception(observation.error);
    require(result != 0, "Cannot enumerate test-owned visible native windows");
    return observation.windows;
}

void report_owned_native_windows(const char* label,
                                 HWND editor,
                                 const std::vector<VisibleNativeWindow>& windows) {
    for (const auto& window : windows)
        std::cout << label << ": class=" << window.name << " editor=" << (window.window == editor)
                  << " thread=" << window.thread << " rect=" << window.bounds.left << ','
                  << window.bounds.top << ',' << window.bounds.right << ',' << window.bounds.bottom
                  << '\n';
}

void capture_owned_native_window(const VisibleNativeWindow& window, const fs::path& destination) {
    DWORD process{};
    GetWindowThreadProcessId(window.window, &process);
    require(process == GetCurrentProcessId() && IsWindowVisible(window.window),
            "Native IME capture requires a visible test-owned window");
    const auto width = window.bounds.right - window.bounds.left;
    const auto height = window.bounds.bottom - window.bounds.top;
    require(width > 0 && width <= 2048 && height > 0 && height <= 1024,
            "Native IME capture exceeds image bounds");

    struct BitmapCloser {
        void operator()(HBITMAP value) const noexcept {
            if (!DeleteObject(value))
                std::cerr << "Native IME capture bitmap cleanup failed\n";
        }
    };

    struct DcCloser {
        void operator()(HDC value) const noexcept {
            if (!DeleteDC(value))
                std::cerr << "Native IME capture DC cleanup failed\n";
        }
    };

    BITMAPINFO definition{};
    definition.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    definition.bmiHeader.biWidth = width;
    definition.bmiHeader.biHeight = -height;
    definition.bmiHeader.biPlanes = 1;
    definition.bmiHeader.biBitCount = 32;
    definition.bmiHeader.biCompression = BI_RGB;
    void* borrowed_pixels{};
    std::unique_ptr<std::remove_pointer_t<HBITMAP>, BitmapCloser> bitmap(
        CreateDIBSection(nullptr, &definition, DIB_RGB_COLORS, &borrowed_pixels, nullptr, 0));
    require(bitmap && borrowed_pixels, "Cannot allocate native IME capture bitmap");
    std::unique_ptr<std::remove_pointer_t<HDC>, DcCloser> dc(CreateCompatibleDC(nullptr));
    require(dc != nullptr, "Cannot allocate native IME capture DC");

    struct Selection {
        HDC dc;
        HGDIOBJ previous;

        Selection(HDC target, HBITMAP selected_bitmap)
            : dc(target), previous(SelectObject(dc, selected_bitmap)) {
            require(previous && previous != HGDI_ERROR, "Cannot select native IME capture bitmap");
        }

        ~Selection() {
            const auto restored = SelectObject(dc, previous);
            if (!restored || restored == HGDI_ERROR)
                std::cerr << "Native IME capture selection cleanup failed\n";
        }

        Selection(const Selection&) = delete;
        Selection& operator=(const Selection&) = delete;
    } selected(dc.get(), bitmap.get());

    require(PrintWindow(window.window, dc.get(), PW_RENDERFULLCONTENT) != 0,
            "Cannot capture the actual owned native IME window");
    require(GdiFlush() != 0, "Cannot flush native IME capture pixels");
    // GDI RGB bitmaps do not define the alpha byte. The captured window is opaque.
    auto* pixels = static_cast<std::uint32_t*>(borrowed_pixels);
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t index = 0; index < count; ++index)
        pixels[index] |= 0xFF000000;
    const auto info = SkImageInfo::Make(width, height, kBGRA_8888_SkColorType, kOpaque_SkAlphaType);
    const SkPixmap frame(info, pixels, static_cast<std::size_t>(width) * 4);
    SkDynamicMemoryWStream encoded;
    require(SkPngEncoder::Encode(&encoded, frame, {}), "Cannot encode native IME capture");
    const auto image = encoded.detachAsData();
    std::ofstream output(destination, std::ios::binary);
    output.write(static_cast<const char*>(image->data()),
                 static_cast<std::streamsize>(image->size()));
    output.close();
    require(static_cast<bool>(output), "Cannot save native IME capture");
}

struct Observations {
    SDL_WindowID window;
    HWND native;
    std::string preedit, committed;
    std::size_t candidates{};
    std::size_t key_downs{};
    std::size_t native_keys{}, native_ime_messages{};
    WPARAM last_native_key{};
    std::exception_ptr error;

    static bool SDLCALL observe_native(void* borrowed, MSG* message) noexcept {
        auto& self = *static_cast<Observations*>(borrowed);
        if (message->hwnd == self.native) {
            if (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN) {
                ++self.native_keys;
                self.last_native_key = message->wParam;
            }
            if (message->message == WM_IME_STARTCOMPOSITION ||
                message->message == WM_IME_COMPOSITION)
                ++self.native_ime_messages;
        }
        return true;
    }

    static bool SDLCALL observe(void* borrowed, SDL_Event* event) noexcept {
        auto& self = *static_cast<Observations*>(borrowed);
        try {
            if (event->type == SDL_EVENT_KEY_DOWN && event->key.windowID == self.window)
                ++self.key_downs;
            if (event->type == SDL_EVENT_TEXT_EDITING && event->edit.windowID == self.window &&
                event->edit.text && *event->edit.text) {
                require(std::string_view(event->edit.text).size() <= 64 * 1024,
                        "Native IME preedit exceeds fixture buffer bound");
                self.preedit = event->edit.text;
            }
            if (event->type == SDL_EVENT_TEXT_INPUT && event->text.windowID == self.window &&
                event->text.text) {
                require(std::string_view(event->text.text).size() <=
                            64 * 1024 - self.committed.size(),
                        "Native IME commit exceeds fixture buffer bound");
                self.committed += event->text.text;
            }
            if (event->type == SDL_EVENT_TEXT_EDITING_CANDIDATES &&
                event->edit_candidates.windowID == self.window)
                self.candidates = std::max(
                    self.candidates,
                    static_cast<std::size_t>(std::max(0, event->edit_candidates.num_candidates)));
        } catch (...) {
            self.error = std::current_exception();
        }
        return true;
    }

    explicit Observations(SDL_Window* owned)
        : window(SDL_GetWindowID(owned)),
          native(static_cast<HWND>(SDL_GetPointerProperty(
              SDL_GetWindowProperties(owned), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))) {
        require(SDL_AddEventWatch(observe, this), "Cannot observe real SDL IME events");
        SDL_SetWindowsMessageHook(observe_native, this);
    }

    ~Observations() {
        SDL_SetWindowsMessageHook(nullptr, nullptr);
        SDL_RemoveEventWatch(observe, this);
    }

    Observations(const Observations&) = delete;
    Observations& operator=(const Observations&) = delete;
};

Json document(const std::shared_ptr<astra::service::ServiceHost>& host, const std::string& id) {
    auto response = host->submit(Json{
        {"jsonrpc", "2.0"},
        {"id", "ime-acceptance"},
        {"method", "document.get"},
        {"params", {{"id", id}}}}.dump());
    require(response.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
            "IME document verification timed out");
    const auto body = response.get();
    require(body.has_value(), "IME document verification has no response");
    const auto parsed = Json::parse(*body);
    require(!parsed.contains("error"), "IME document verification failed");
    return parsed.at("result");
}

struct WindowCloser {
    void operator()(HWND window) const noexcept {
        if (!DestroyWindow(window))
            std::cerr << "Owned native IME reference destruction failed\n";
    }
};

using NativeWindow = std::unique_ptr<std::remove_pointer_t<HWND>, WindowCloser>;

struct ThreadServices {
    winrt::com_ptr<ITfThreadMgr> manager;
    winrt::com_ptr<ITfMessagePump> messages;

    ThreadServices() {
        checked(CoCreateInstance(CLSID_TF_ThreadMgr,
                                 nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 __uuidof(ITfThreadMgr),
                                 manager.put_void()),
                "Create reference TSF thread manager");
        messages = manager.as<ITfMessagePump>();
        TfClientId client{};
        checked(manager->Activate(&client), "Activate reference TSF thread manager");
    }

    ~ThreadServices() {
        const auto result = manager->Deactivate();
        if (result != S_OK)
            std::cerr << "Reference TSF thread deactivation failed: " << result << '\n';
    }

    ThreadServices(const ThreadServices&) = delete;
    ThreadServices& operator=(const ThreadServices&) = delete;
};

struct ReferenceClass {
    static constexpr const wchar_t* name = L"AstraInstalledImeReference";
    HINSTANCE instance{GetModuleHandleW(nullptr)};

    ReferenceClass() {
        WNDCLASSEXW definition{};
        definition.cbSize = sizeof(definition);
        definition.lpfnWndProc = DefWindowProcW;
        definition.hInstance = instance;
        definition.hCursor = LoadCursor(nullptr, IDC_ARROW);
        definition.lpszClassName = name;
        require(RegisterClassExW(&definition) != 0, "Cannot register native IME reference class");
    }

    ~ReferenceClass() {
        if (!UnregisterClassW(name, instance))
            std::cerr << "Owned native IME reference class cleanup failed\n";
    }

    ReferenceClass(const ReferenceClass&) = delete;
    ReferenceClass& operator=(const ReferenceClass&) = delete;
};

class ReferenceEdit {
  public:
    explicit ReferenceEdit(HWND parent)
        : native_(CreateWindowExW(WS_EX_CLIENTEDGE,
                                  L"EDIT",
                                  L"",
                                  WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL,
                                  20,
                                  70,
                                  600,
                                  140,
                                  parent,
                                  nullptr,
                                  GetModuleHandleW(nullptr),
                                  nullptr)) {
        require(native_ != nullptr, "Cannot create owned Windows EDIT IME reference");
        require(SetFocus(native_.get()) || GetFocus() == native_.get(),
                "Cannot focus owned EDIT reference");
    }

    HWND native() const noexcept {
        return native_.get();
    }

    std::wstring text() const {
        const auto length = GetWindowTextLengthW(native_.get());
        require(length >= 0 && length <= 4096, "Native EDIT text exceeds reference bound");
        std::wstring result(static_cast<std::size_t>(length) + 1, L'\0');
        require(GetWindowTextW(native_.get(), result.data(), length + 1) == length,
                "Cannot read owned EDIT text");
        result.resize(static_cast<std::size_t>(length));
        return result;
    }

    ReferenceEdit(const ReferenceEdit&) = delete;
    ReferenceEdit& operator=(const ReferenceEdit&) = delete;

  private:
    NativeWindow native_;
};

void reference(bool activate_thread_services) {
    // Test-only Win32 baseline: no SDL initialization, Astra services or rendering.
    std::optional<ThreadServices> thread_services;
    if (activate_thread_services)
        thread_services.emplace();
    ReferenceClass registered;
    NativeWindow parent(CreateWindowExW(0,
                                        ReferenceClass::name,
                                        L"Astra — independent installed IME reference",
                                        WS_OVERLAPPEDWINDOW,
                                        CW_USEDEFAULT,
                                        CW_USEDEFAULT,
                                        680,
                                        280,
                                        nullptr,
                                        nullptr,
                                        registered.instance,
                                        nullptr));
    require(parent != nullptr, "Cannot create independent Win32 IME reference");
    ShowWindow(parent.get(), SW_SHOWNOACTIVATE);
    std::size_t native_keys{};
    WPARAM last_key{};
    HWND observed_target{};
    const auto next_message = [&](MSG& message) {
        BOOL received{};
        if (thread_services)
            checked(thread_services->messages->PeekMessageW(
                        &message, nullptr, 0, 0, PM_REMOVE, &received),
                    "Read reference TSF message queue");
        else
            received = PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE);
        return received != FALSE;
    };
    const auto pump = [&] {
        require(IsWindow(parent.get()), "Independent IME reference was closed");
        MSG message{};
        std::size_t messages{};
        while (next_message(message)) {
            require(++messages <= 4096, "Native IME reference message pump exceeded bound");
            require(message.message != WM_QUIT, "Native IME reference received quit");
            if (message.hwnd == observed_target &&
                (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN)) {
                ++native_keys;
                last_key = message.wParam;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    };
    Focus focus(parent.get(), pump);
    Profiles profiles;
    profiles.activate();
    pump();
    ReferenceEdit edit(parent.get());
    observed_target = edit.native();

    const auto await_native = [&](auto predicate, const char* failure) {
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        do {
            pump();
            if (predicate())
                return;
            Sleep(2);
        } while (Clock::now() < deadline);
        throw AwaitTimeout(failure);
    };
    Context context(edit.native());
    NativeImeTrace trace(edit.native());
    {
        InputContext current(edit.native());
        require(ImmSetOpenStatus(current.context, FALSE) != 0,
                "Cannot close IME for independent Latin-input control");
    }
    focus.key('A', edit.native());
    await_native(
        [&] {
            const auto text = edit.text();
            return text == L"a" || text == L"A";
        },
        "Independent EDIT reference did not accept real OS Latin input");
    std::cout << "Independent Win32 EDIT real OS Latin-input control: PASS\n";
    require(SetWindowTextW(edit.native(), L"") != 0, "Cannot clear Latin control fixture");
    context.enable();
    const auto settle_until = Clock::now() + std::chrono::seconds(2);
    do {
        pump();
        require(GetForegroundWindow() == parent.get() && GetFocus() == edit.native(),
                "Independent IME reference lost focus during activation settling");
        Sleep(2);
    } while (Clock::now() < settle_until);
    {
        InputContext current(edit.native());
        DWORD conversion{}, sentence{};
        require(ImmGetConversionStatus(current.context, &conversion, &sentence) != 0,
                "Cannot read settled native IME conversion state");
        std::cout << "Independent EDIT settled IME: open=" << ImmGetOpenStatus(current.context)
                  << " conversion=" << conversion << " sentence=" << sentence << '\n';
        require(ImmGetOpenStatus(current.context) && (conversion & IME_CMODE_NATIVE),
                "Native Chinese IME mode did not persist after activation");
    }
    if (thread_services) {
        winrt::com_ptr<ITfDocumentMgr> focused;
        checked(thread_services->manager->GetFocus(focused.put()), "Read reference TSF focus");
        std::cout << "Reference explicit TSF active, focused document=" << (focused != nullptr)
                  << '\n';
    }
    focus.key('N', edit.native());
    await_native(
        [&] {
            return native_keys > 0;
        },
        "Native EDIT did not receive first OS IME key");
    focus.key('I', edit.native());
    LONG composition{};
    bool composition_observed{true};
    try {
        await_native(
            [&] {
                InputContext current(edit.native());
                composition = ImmGetCompositionStringW(current.context, GCS_COMPSTR, nullptr, 0);
                return composition > 0;
            },
            "Installed IME produced no composition in standard Windows EDIT reference");
    } catch (const AwaitTimeout&) {
        composition_observed = false;
        std::cerr << "Independent EDIT IME diagnostic: native keys=" << native_keys
                  << " last key=" << last_key << " composition=" << composition
                  << " text units=" << edit.text().size()
                  << " native focus=" << (GetFocus() == edit.native()) << '\n';
        trace.report("Independent native preedit messages");
    }
    focus.key(VK_SPACE, edit.native());
    try {
        await_native(
            [&] {
                const auto text = edit.text();
                return std::any_of(text.begin(), text.end(), [](wchar_t c) {
                    return c >= 0x80;
                });
            },
            "Standard Windows EDIT reference did not commit a Chinese candidate");
    } catch (const AwaitTimeout&) {
        const auto text = edit.text();
        std::cerr << "Independent EDIT commit diagnostic: text units=" << text.size()
                  << " first unit=" << (text.empty() ? 0 : static_cast<unsigned>(text.front()))
                  << " native keys=" << native_keys << " last key=" << last_key << '\n';
        throw;
    }
    trace.report("Independent native committed messages");
    std::cout << "Independent Win32 EDIT real OS Chinese candidate commit: PASS\n";
    context.restore();
    profiles.restore();
    require(composition_observed,
            "Native Chinese commit succeeded but required composition observation is missing");
    std::cout
        << "Independent Win32 EDIT installed-IME reference: composition and Chinese commit: PASS\n";
}

void await_input_context(Window& window, HWND native, const std::string& role) {
    // Window-idle describes Astra's work, not the installed input service's
    // context readiness after changing its process-local profile.
    std::cout << role << " context readiness: foreground=" << (GetForegroundWindow() == native)
              << " focus=" << (GetFocus() == native)
              << " SDL focus=" << (SDL_GetKeyboardFocus() == window.window())
              << " text input=" << SDL_TextInputActive(window.window()) << '\n';
    await(
        window,
        [&] {
            const auto context = ImmGetContext(native);
            if (!context)
                return false;
            require(ImmReleaseContext(native, context) != 0,
                    role + " input context readiness lease could not be released");
            return SDL_TextInputActive(window.window());
        },
        role + " installed input context did not become available");
}

std::string field_candidate(Window& window,
                            const Focus& focus,
                            HWND native,
                            const fs::path& workspace,
                            const std::string& role,
                            astra::ui::Box field_box,
                            const std::function<Json()>& field_state,
                            const std::function<void()>& check_document) {
    std::cout << "Starting real IME field: " << role << '\n';
    await_input_context(window, native, role);
    Context context(native);
    context.enable();
    Observations observed(window.window());
    NativeImeTrace trace(native);
    const auto before = field_state();
    const auto before_windows = owned_native_windows();
    focus.key('N');
    focus.key('I');
    await(
        window,
        [&] {
            InputContext current(native);
            DWORD count{};
            return !observed.preedit.empty() ||
                   (ImmGetCandidateListCountW(current.context, &count) > 0 && count > 0);
        },
        role + " produced no real composition/candidates");
    require(field_state() == before, role + " preedit changed field content");
    check_document();
    SDL_Rect area{};
    int cursor{};
    require(SDL_GetTextInputArea(window.window(), &area, &cursor) && area.h > 0,
            role + " has no native text input area");
    int logical_width{}, logical_height{}, pixel_width{}, pixel_height{};
    require(SDL_GetWindowSize(window.window(), &logical_width, &logical_height) &&
                SDL_GetWindowSizeInPixels(window.window(), &pixel_width, &pixel_height) &&
                logical_width > 0 && logical_height > 0,
            role + " cannot convert its native caret coordinates");
    const auto x_scale = static_cast<float>(pixel_width) / logical_width;
    const auto y_scale = static_cast<float>(pixel_height) / logical_height;
    require(field_box.contains(area.x * x_scale, area.y * y_scale),
            role + " native caret remained outside actual field geometry");
    POINT caret{area.x, area.y};
    require(ClientToScreen(native, &caret) != 0, role + " has no native caret position");
    std::optional<VisibleNativeWindow> native_popup;
    const auto find_popup = [&] {
        for (const auto& popup : owned_native_windows()) {
            const bool newly_visible =
                std::none_of(before_windows.begin(), before_windows.end(), [&](const auto& old) {
                    return old.window == popup.window;
                });
            if (newly_visible && popup.thread == GetCurrentThreadId() &&
                std::abs(popup.bounds.left - caret.x) <= std::max(32L, area.h * 2L) &&
                popup.bounds.top >= caret.y - area.h && popup.bounds.top <= caret.y + area.h * 3L) {
                native_popup = popup;
                return true;
            }
        }
        return false;
    };
    if (observed.preedit.empty()) {
        await(window, find_popup, role + " native composition UI is not near field caret");
        const auto capture = workspace.parent_path() / (workspace.filename().string() + "-" + role +
                                                        "-native-composition.png");
        capture_owned_native_window(*native_popup, capture);
        std::cout << role << " native composition capture: " << capture << '\n';
        require(selected_native_candidate(native).has_value(),
                role + " has no native candidate metadata");
    }
    window.request_screenshot(workspace.parent_path() /
                              (workspace.filename().string() + "-" + role + "-preedit.png"));
    await(
        window,
        [&] {
            return window.idle();
        },
        role + " preedit presentation did not settle");
    focus.key(VK_SPACE);
    await(
        window,
        [&] {
            return !observed.committed.empty() && window.idle();
        },
        role + " did not commit Chinese candidate");
    if (observed.error)
        std::rethrow_exception(observed.error);
    trace.report((role + " native commit").c_str());
    require(observed.committed == trace.result_text(),
            role + " SDL commit differs from native result");
    require(std::any_of(observed.committed.begin(),
                        observed.committed.end(),
                        [](unsigned char value) {
                            return value >= 0x80;
                        }),
            role + " did not commit Chinese text");
    check_document();
    const auto committed_field = field_state();
    context.restore();
    window.request_screenshot(workspace.parent_path() /
                              (workspace.filename().string() + "-" + role + "-committed.png"));
    await(
        window,
        [&] {
            return window.idle();
        },
        role + " restored input mode did not settle");
    if (observed.error)
        std::rethrow_exception(observed.error);
    trace.report((role + " restored commit").c_str());
    require(observed.committed == trace.result_text() && field_state() == committed_field,
            role + " restoring IME mode changed committed field content");
    check_document();
    return observed.committed;
}

void cancel_field_candidate(Window& window,
                            const Focus& focus,
                            HWND native,
                            const std::string& role,
                            const std::function<Json()>& field_state,
                            const std::function<void()>& check_document) {
    await_input_context(window, native, role);
    Context context(native);
    context.enable();
    Observations observed(window.window());
    NativeImeTrace trace(native);
    const auto before = field_state();
    focus.key('N');
    focus.key('I');
    await(
        window,
        [&] {
            InputContext current(native);
            DWORD count{};
            return !observed.preedit.empty() ||
                   (ImmGetCandidateListCountW(current.context, &count) > 0 && count > 0);
        },
        role + " cancellation composition did not start");
    require(field_state() == before, role + " cancelled preedit changed content before Escape");
    focus.key(VK_ESCAPE);
    await(
        window,
        [&] {
            InputContext current(native);
            DWORD count{};
            const auto bytes = ImmGetCandidateListCountW(current.context, &count);
            return window.idle() && !bytes && !count;
        },
        role + " Escape did not dismiss native candidates");
    if (observed.error)
        std::rethrow_exception(observed.error);
    trace.report((role + " native cancellation").c_str());
    require(observed.committed.empty() && trace.result_text().empty(),
            role + " Escape committed cancelled composition");
    require(field_state() == before, role + " Escape changed content or dismissed its owner");
    check_document();
    context.restore();
    await(
        window,
        [&] {
            return window.idle();
        },
        role + " cancelled mode restoration did not settle");
    if (observed.error)
        std::rethrow_exception(observed.error);
    trace.report((role + " restored cancellation").c_str());
    require(observed.committed.empty() && trace.result_text().empty() && field_state() == before,
            role + " cancelled input leaked after mode restoration");
    check_document();
    std::cout << role << " real candidate commit/cancel and document isolation: PASS\n";
}

void run(const fs::path& workspace) {
    astra::service::ServiceHostOptions services;
    services.workspace = workspace;
    services.recovery_directory = workspace / "recovery";
    services.resource_directory = fs::current_path();
    services.defaults_script = astra::io::read_file("data/defaults.js", 1024 * 1024).text;
    auto host = std::make_shared<astra::service::ServiceHost>(std::move(services));
    {
        Window window(host,
                      {.title = "Astra — installed IME acceptance",
                       .open_paths = {"ime.txt"},
                       .user_script = R"JS(
            for (const view of ['', 'search']) {
                const id = view ? 'ime-sidebar' : 'ime-editor';
                astra.ui.registerCommand({ id: 'acceptance.' + id, title: id }, async () => {
                    if (view) await astra.ui.showView(view);
                    await astra.ui.forms.open({
                        id, title: 'Installed Chinese IME', view,
                        fields: [{ id: 'value', value: '', submit: 'accept' }],
                        actions: [{ id: 'accept', label: 'Accept' }]
                    });
                });
                astra.ui.registerCommand({ id: 'acceptance.close-' + id, title: 'Close ' + id },
                    () => astra.ui.forms.close(id));
            }
            astra.ui.registerCommand({ id: 'acceptance.ime-modal', title: 'Installed IME modal' },
                async () => {
                    const value = await astra.ui.input({ title: 'Installed Chinese IME', value: '' });
                    await astra.ui.update({ status: value === null ? 'IME modal cancelled' : 'IME modal ' + value });
                });
        )JS"});
        await(
            window,
            [&] {
                return window.idle();
            },
            "IME fixture startup did not settle");
        Profiles profiles;
        const auto native = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(window.window()), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        require(native != nullptr, "IME fixture needs the actual SDL Win32 window");
        Focus focus(native, [&] {
            require(window.pump(), "IME window closed while delivering OS key");
            require(window.last_error().empty(), window.last_error());
        });
        profiles.activate();
        await(
            window,
            [&] {
                return window.idle();
            },
            "Installed profile activation did not settle");
        {
            Context context(native);
            context.enable();
            Observations observed(window.window());
            NativeImeTrace trace(native);
            const auto before = document(host, window.active_document_id());
            const auto before_windows = owned_native_windows();
            report_owned_native_windows(
                "Astra owned windows before IME input", native, before_windows);
            focus.key('N');
            await(
                window,
                [&] {
                    return observed.native_keys > 0;
                },
                "First OS IME key did not reach owned window");
            focus.key('I');
            try {
                await(
                    window,
                    [&] {
                        if (!observed.preedit.empty())
                            return true;
                        InputContext current(native);
                        DWORD count{};
                        return ImmGetCandidateListCountW(current.context, &count) > 0 && count > 0;
                    },
                    "Installed IME produced no app preedit or native candidates");
            } catch (const AwaitTimeout&) {
                InputContext current(native);
                std::cerr << "IME preedit diagnostic: committed=" << observed.committed
                          << " native composition bytes="
                          << ImmGetCompositionStringW(current.context, GCS_COMPSTR, nullptr, 0)
                          << " open=" << ImmGetOpenStatus(current.context)
                          << " SDL keydowns=" << observed.key_downs
                          << " native keys=" << observed.native_keys
                          << " last native key=" << observed.last_native_key
                          << " native IME messages=" << observed.native_ime_messages
                          << " native foreground=" << (GetForegroundWindow() == native)
                          << " native focus=" << (GetFocus() == native)
                          << " SDL focus=" << (SDL_GetKeyboardFocus() == window.window())
                          << " text input active=" << SDL_TextInputActive(window.window())
                          << " keyboard=" << reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(0))
                          << '\n';
                trace.report("Astra native preedit messages");
                throw;
            }
            const bool preedit_observed = !observed.preedit.empty();
            if (observed.error)
                std::rethrow_exception(observed.error);
            const auto composing = document(host, window.active_document_id());
            require(composing.at("revision") == before.at("revision") &&
                        composing.at("text") == before.at("text") &&
                        composing.at("selections") == before.at("selections"),
                    "Installed IME preedit mutated the document");
            DWORD count{};
            DWORD bytes{};
            {
                InputContext current(native);
                bytes = ImmGetCandidateListCountW(current.context, &count);
            }
            std::cout << "Installed candidate state: native lists=" << count << " bytes=" << bytes
                      << " SDL candidates=" << observed.candidates << '\n';
            auto candidate_windows = owned_native_windows();
            const bool candidates_observed = count > 0 || observed.candidates > 0;
            const bool preedit_painted =
                window.view_state().input_layout.at("documentCompositionDrawn") == true;
            SDL_Rect area{};
            int cursor{};
            require(SDL_GetTextInputArea(window.window(), &area, &cursor) && area.h > 0,
                    "Installed IME has no native caret/candidate area");
            POINT caret{area.x, area.y};
            require(ClientToScreen(native, &caret) != 0,
                    "Cannot locate native IME caret on screen");
            std::cout << "Actual native IME caret: " << caret.x << ',' << caret.y
                      << " size=" << area.w << ',' << area.h << '\n';
            const auto is_composition_popup = [&](const VisibleNativeWindow& popup) {
                const bool new_window = std::none_of(
                    before_windows.begin(), before_windows.end(), [&](const auto& before) {
                        return before.window == popup.window;
                    });
                const bool near_caret =
                    std::abs(popup.bounds.left - caret.x) <= std::max(32L, area.h * 2L) &&
                    popup.bounds.top >= caret.y - area.h &&
                    popup.bounds.top <= caret.y + area.h * 3L;
                return new_window && popup.thread == GetCurrentThreadId() && near_caret;
            };
            if (!preedit_observed)
                await(
                    window,
                    [&] {
                        candidate_windows = owned_native_windows();
                        return std::any_of(candidate_windows.begin(),
                                           candidate_windows.end(),
                                           is_composition_popup);
                    },
                    "Native IME composition popup was not presented near the actual caret");
            report_owned_native_windows(
                "Astra owned windows with IME candidates", native, candidate_windows);
            bool native_preedit_presented{};
            for (const auto& popup : candidate_windows) {
                if (is_composition_popup(popup)) {
                    const auto capture = workspace.parent_path() / (workspace.filename().string() +
                                                                    "-native-composition.png");
                    capture_owned_native_window(popup, capture);
                    native_preedit_presented = true;
                    std::cout << "Captured test-owned IME popup near actual caret: " << popup.name
                              << " image=" << capture << '\n';
                }
            }
            window.request_screenshot("build/graphics/gui-installed-ime-preedit.png");
            await(
                window,
                [&] {
                    return window.idle();
                },
                "Installed preedit capture did not settle");
            const auto selected_candidate = selected_native_candidate(native);
            focus.key(VK_SPACE);
            await(
                window,
                [&] {
                    return !observed.committed.empty() && window.idle();
                },
                "Installed IME did not commit candidate");
            const auto committed = document(host, window.active_document_id());
            require(observed.committed.find_first_of("niNI ") != 0 &&
                        std::any_of(observed.committed.begin(),
                                    observed.committed.end(),
                                    [](unsigned char c) {
                                        return c >= 0x80;
                                    }),
                    "Installed IME committed Latin input instead of a Chinese candidate");
            require(committed.at("text") ==
                        observed.committed + before.at("text").get<std::string>(),
                    "OS candidate commit differs from the actual shared document");
            trace.report("Astra native committed messages");
            require(observed.committed == trace.result_text(),
                    "SDL Chinese commit differs from the exact native IME result string");
            require(std::stoull(committed.at("revision").get<std::string>()) ==
                        std::stoull(before.at("revision").get<std::string>()) + 1,
                    "One OS Chinese candidate commit was not one document transaction");
            std::cout << "Actual Astra OS Chinese commit: shared document, one transaction: PASS\n";
            window.request_screenshot("build/graphics/gui-installed-ime-committed.png");
            await(
                window,
                [&] {
                    return window.idle();
                },
                "Installed commit capture did not settle");
            context.restore();
            window.execute_ui_command("editor.undo");
            await(
                window,
                [&] {
                    return window.idle();
                },
                "OS Chinese commit undo did not settle");
            const auto undone = document(host, window.active_document_id());
            require(undone.at("text") == before.at("text") &&
                        undone.at("selections") == before.at("selections"),
                    "One undo did not restore the complete OS Chinese candidate commit");
            window.execute_ui_command("editor.redo");
            await(
                window,
                [&] {
                    return window.idle();
                },
                "OS Chinese commit redo did not settle");
            const auto redone = document(host, window.active_document_id());
            require(redone.at("text") == committed.at("text") &&
                        redone.at("selections") == committed.at("selections"),
                    "One redo did not restore the complete OS Chinese candidate commit");
            window.execute_ui_command("ui.save");
            await(
                window,
                [&] {
                    return window.idle();
                },
                "OS Chinese commit save did not settle");
            require(astra::io::read_file(workspace / "ime.txt", 4096).text ==
                        committed.at("text").get<std::string>(),
                    "Durable file content differs from the OS Chinese candidate commit");
            std::cout << "Actual Astra OS Chinese commit: undo, redo and durable save: PASS\n";
            await(
                window,
                [&] {
                    return window.idle();
                },
                "Document IME mode restoration did not settle");
            if (observed.error)
                std::rethrow_exception(observed.error);
            trace.report("Astra native restored messages");
            require(observed.committed == trace.result_text() &&
                        document(host, window.active_document_id()).at("text") ==
                            committed.at("text"),
                    "IME restoration changed the verified Chinese commit");
            require(
                candidates_observed,
                "Chinese commit/history/save passed; required candidate observation is missing");
            if (preedit_observed)
                require(preedit_painted, "Offered app IME preedit was not painted");
            else
                require(native_preedit_presented && selected_candidate.has_value(),
                        "Native-only IME has no observed composition UI near the actual caret");
            std::cout << "Installed composition route: "
                      << (preedit_observed ? "app preedit" : "owned native composition UI")
                      << ": PASS\n";
        }
        // Keep one selected profile for this continuous editing session; each
        // route restores its mode, and the original profile is verified on exit.
        const auto preserved_document = document(host, window.active_document_id());
        const auto check_document = [&] {
            const auto current = document(host, window.active_document_id());
            require(current.at("revision") == preserved_document.at("revision") &&
                        current.at("text") == preserved_document.at("text") &&
                        current.at("selections") == preserved_document.at("selections"),
                    "Installed field IME mutated document text/revision/selections");
        };
        for (const bool sidebar : {false, true}) {
            const std::string role = sidebar ? "ime-sidebar" : "ime-editor";
            window.execute_ui_command("acceptance." + role);
            const auto field_state = [&]() -> Json {
                const auto state = window.view_state();
                const auto& form = sidebar ? state.sidebar_form : state.inline_form;
                return form.is_object() && form.value("id", "") == role
                           ? form.at("values").at("value")
                           : Json(nullptr);
            };
            await(
                window,
                [&] {
                    return window.idle() && field_state() == "";
                },
                role + " not ready");
            const auto state = window.view_state();
            const auto& geometry = sidebar ? state.sidebar_form_layout : state.inline_form_layout;
            require(geometry && geometry->fields.size() == 1, role + " field geometry missing");
            Focus field_focus(native, focus.pump);
            const auto text = field_candidate(window,
                                              field_focus,
                                              native,
                                              workspace,
                                              role,
                                              geometry->fields.front(),
                                              field_state,
                                              check_document);
            require(field_state() == text, role + " native commit differs from actual form value");
            cancel_field_candidate(window, field_focus, native, role, field_state, check_document);
            window.execute_ui_command("acceptance.close-" + role);
            await(
                window,
                [&] {
                    return window.idle() && field_state().is_null();
                },
                role + " close did not settle");
        }
        window.execute_ui_command("acceptance.ime-modal");
        const auto modal_state = [&]() -> Json {
            const auto input = window.view_state().input_layout;
            return input.contains("bytes") ? input.at("bytes") : Json(nullptr);
        };
        await(
            window,
            [&] {
                return window.idle() && modal_state() == 0;
            },
            "IME modal not ready");
        const auto modal_view = window.view_state();
        int width{}, height{};
        require(SDL_GetWindowSizeInPixels(window.window(), &width, &height),
                "Cannot measure actual modal field owner");
        const auto modal_box = astra::ui::PickerLayout::measure(static_cast<float>(width),
                                                                static_cast<float>(height),
                                                                modal_view.layout.scale,
                                                                0,
                                                                modal_view.dialog_metrics)
                                   .input;
        Focus modal_focus(native, focus.pump);
        const auto modal_text = field_candidate(window,
                                                modal_focus,
                                                native,
                                                workspace,
                                                "ime-modal",
                                                modal_box,
                                                modal_state,
                                                check_document);
        require(modal_state() == modal_text.size(), "IME modal commit did not enter actual input");
        cancel_field_candidate(
            window, modal_focus, native, "ime-modal", modal_state, check_document);
        modal_focus.key(VK_RETURN);
        await(
            window,
            [&] {
                return window.idle() && modal_state().is_null();
            },
            "IME modal accept did not settle");
        require(window.status_text() == "IME modal " + modal_text,
                "Accepted modal value differs from exact native Chinese result");
        check_document();
        profiles.restore();
        await(
            window,
            [&] {
                return window.idle();
            },
            "Final field IME profile restoration did not settle");
        check_document();
    }
    host->shutdown();
}
} // namespace

int main(int argc, char* argv[]) {
    fs::path workspace;
    try {
        Apartment apartment;
        environment();
        require(argc == 1 || (argc == 2 && (std::string_view(argv[1]) == "--probe" ||
                                            std::string_view(argv[1]) == "--reference" ||
                                            std::string_view(argv[1]) == "--reference-tsf")),
                "Usage: gui-ime-smoke [--probe|--reference|--reference-tsf]");
        if (argc == 2 && std::string_view(argv[1]) == "--probe") {
            Profiles profiles;
            std::cout << "Enabled installed Chinese keyboard profile prerequisite: PASS\n";
            return 0;
        }
        const auto parent = fs::weakly_canonical(fs::absolute("build/graphics"));
        fs::create_directories(parent);
        workspace = parent / ("gui-ime-" + std::to_string(Clock::now().time_since_epoch().count()));
        require(fs::create_directory(workspace), "Cannot create installed IME fixture");
        {
            std::ofstream file(workspace / "ime.txt", std::ios::binary);
            file << "IME fixture\n";
            file.close();
            require(static_cast<bool>(file), "Cannot write installed IME fixture");
        }
        const bool system_reference = argc == 2;
        if (system_reference)
            reference(std::string_view(argv[1]) == "--reference-tsf");
        else
            run(workspace);
        const auto issues = astra::test::cleanup_owned_directory(workspace, parent, "gui-ime-");
        require(issues.empty(), "Installed IME fixture cleanup failed");
        if (!system_reference)
            std::cout
                << "Windows installed Chinese IME: real OS composition/candidates -> SDL commit -> "
                   "atomic shared document/history/save: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Installed IME acceptance failed: " << error.what()
                  << "\nPreserved fixture: " << workspace << '\n';
        return 1;
    }
}
