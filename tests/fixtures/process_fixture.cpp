#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

namespace {

#ifdef _WIN32
class ChildHandle {
  public:
    explicit ChildHandle(HANDLE value) : value_(value) {
    }

    ~ChildHandle() {
        if (value_) {
            (void)CloseHandle(value_);
        }
    }

    ChildHandle(const ChildHandle&) = delete;
    ChildHandle& operator=(const ChildHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

  private:
    HANDLE value_;
};

int spawn_descendant(std::wstring_view mode) {
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return 75;
    }
    path.resize(length);
    // Fixed test modes and an OS-provided executable path; CreateProcessW argv,
    // no shell. The parent fixture exits while its descendants retain stdio.
    std::wstring command = L"\"" + path + L"\" " + std::wstring(mode);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(path.c_str(),
                        command.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        CREATE_NO_WINDOW,
                        nullptr,
                        nullptr,
                        &startup,
                        &child)) {
        return 76;
    }
    const ChildHandle process(child.hProcess);
    const ChildHandle thread(child.hThread);
    std::cout << "PID " << child.dwProcessId << '\n' << std::flush;
    return 0;
}
#endif

int run(const std::vector<std::string>& arguments) {
    if (arguments.size() < 2) {
        return 70;
    }
    const auto& mode = arguments[1];
    if (mode == "echo") {
        std::array<char, 4096> buffer{};
        while (std::cin) {
            std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            std::cout.write(buffer.data(), std::cin.gcount());
            std::cout.flush();
        }
        return 0;
    }
    if (mode == "arguments") {
        for (std::size_t index = 2; index < arguments.size(); ++index) {
            std::cout << arguments[index].size() << ':' << arguments[index] << '\n';
        }
        return 0;
    }
    if (mode == "environment") {
#ifdef _WIN32
        const auto length = GetEnvironmentVariableA("ASTRA_PROCESS_TEST", nullptr, 0);
        if (length == 0) {
            std::cout << "missing";
        } else {
            std::string value(length, '\0');
            const auto written =
                GetEnvironmentVariableA("ASTRA_PROCESS_TEST", value.data(), length);
            if (written == 0 || written >= length) {
                return 74;
            }
            value.resize(written);
            std::cout << value;
        }
#else
        const auto* value = std::getenv("ASTRA_PROCESS_TEST");
        std::cout << (value ? value : "missing");
#endif
        return 0;
    }
    if (mode == "tail") {
        std::cout << std::string(128 * 1024, 't') << "TAIL";
        std::cerr << "error-tail";
        return 7;
    }
    if (mode == "emit" || mode == "stderr") {
        auto& output = mode == "emit" ? std::cout : std::cerr;
        for (int index = 0; index < 128; ++index) {
            output << std::string(4096, 'x');
            output.flush();
        }
        return 0;
    }
    if (mode == "wait") {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
#ifdef _WIN32
    if (mode == "tree") {
        const auto event_name =
            L"Local\\AstraProcessFixture-" + std::to_wstring(GetCurrentProcessId());
        const ChildHandle ready(CreateEventW(nullptr, TRUE, FALSE, event_name.c_str()));
        if (!ready.get()) {
            return 77;
        }
        const auto spawned = spawn_descendant(L"grandchild " + event_name);
        if (spawned != 0) {
            return spawned;
        }
        return WaitForSingleObject(ready.get(), 3000) == WAIT_OBJECT_0 ? 0 : 78;
    }
    if (mode == "grandchild") {
        const auto result = spawn_descendant(L"wait");
        if (result != 0) {
            return result;
        }
        if (arguments.size() < 3) {
            return 79;
        }
        const std::wstring event_name(arguments[2].begin(), arguments[2].end());
        const ChildHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, event_name.c_str()));
        if (!ready.get() || !SetEvent(ready.get())) {
            return 80;
        }
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
#endif
    return 71;
}

} // namespace

#ifdef _WIN32
int wmain(int count, wchar_t** values) {
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
    (void)_setmode(_fileno(stderr), _O_BINARY);
    std::vector<std::string> arguments;
    for (int index = 0; index < count; ++index) {
        const auto length = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, values[index], -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0) {
            return 72;
        }
        std::string value(static_cast<std::size_t>(length), '\0');
        if (WideCharToMultiByte(CP_UTF8,
                                WC_ERR_INVALID_CHARS,
                                values[index],
                                -1,
                                value.data(),
                                length,
                                nullptr,
                                nullptr) == 0) {
            return 73;
        }
        value.pop_back();
        arguments.push_back(std::move(value));
    }
    return run(arguments);
}
#else
int main(int count, char** values) {
    std::vector<std::string> arguments;
    for (int index = 0; index < count; ++index) {
        arguments.emplace_back(values[index]);
    }
    return run(arguments);
}
#endif
