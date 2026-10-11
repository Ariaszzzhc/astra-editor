// Windows production process launcher. The target joins Astra's Job Object
// before it can execute any instruction or spawn descendants. libuv supervises
// this host and its original stdin/stdout/stderr; fd 3 reports startup separately.
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <string>
#include <string_view>

namespace {

class Handle {
  public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {
    }

    ~Handle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE) {
            (void)CloseHandle(value_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    void reset() noexcept {
        if (value_) {
            (void)CloseHandle(value_);
            value_ = nullptr;
        }
    }

  private:
    HANDLE value_;
};

void notify(std::string_view message) noexcept {
    (void)_write(3, message.data(), static_cast<unsigned int>(message.size()));
    (void)_close(3);
}

int fail(DWORD error) {
    notify("ERROR " + std::to_string(error) + "\n");
    return 127;
}

std::wstring quote(std::wstring_view argument) {
    // This is Windows CRT argv quoting for CreateProcessW, never shell syntax.
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(character);
        } else {
            result.append(slashes, L'\\');
            result.push_back(character);
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

} // namespace

int wmain(int count, wchar_t** arguments) {
    (void)_setmode(3, _O_BINARY);
    if (count < 4) {
        return fail(ERROR_INVALID_PARAMETER);
    }
    const auto control = reinterpret_cast<HANDLE>(_get_osfhandle(3));
    if (control == INVALID_HANDLE_VALUE || !SetHandleInformation(control, HANDLE_FLAG_INHERIT, 0)) {
        return fail(GetLastError());
    }
    Handle job(OpenJobObjectW(JOB_OBJECT_ASSIGN_PROCESS | JOB_OBJECT_QUERY, FALSE, arguments[1]));
    if (!job.get()) {
        return fail(GetLastError());
    }
    if (!AssignProcessToJobObject(job.get(), GetCurrentProcess())) {
        return fail(GetLastError());
    }

    std::wstring command;
    for (int index = 3; index < count; ++index) {
        if (!command.empty()) {
            command.push_back(L' ');
        }
        command += quote(arguments[index]);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = arguments[2][0] ? arguments[2] : nullptr;
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION created{};
    if (!CreateProcessW(nullptr,
                        command.data(),
                        nullptr,
                        nullptr,
                        TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        nullptr,
                        nullptr,
                        &startup,
                        &created)) {
        return fail(GetLastError());
    }
    const Handle process(created.hProcess);
    const Handle thread(created.hThread);
    // Membership is inherited from this host. Verify it before releasing the
    // initial thread instead of silently falling back to direct-child cleanup.
    BOOL member = FALSE;
    if (!IsProcessInJob(process.get(), job.get(), &member) || !member) {
        const auto error = GetLastError();
        (void)TerminateProcess(process.get(), 127);
        return fail(error == ERROR_SUCCESS ? ERROR_ACCESS_DENIED : error);
    }
    // Only Astra retains a job handle during target execution. If Astra crashes,
    // KILL_ON_JOB_CLOSE must not be kept alive by this launcher process.
    job.reset();
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const auto error = GetLastError();
        (void)TerminateProcess(process.get(), 127);
        return fail(error);
    }
    notify("OK\n");
    if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0) {
        const auto error = GetLastError();
        (void)TerminateProcess(process.get(), 127);
        return static_cast<int>(error);
    }
    DWORD result = 127;
    if (!GetExitCodeProcess(process.get(), &result)) {
        return 127;
    }
    return static_cast<int>(result);
}
#else
#error astra-process-host is Windows-only
#endif
