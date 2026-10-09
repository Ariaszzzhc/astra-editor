#include <astra/process/process.hpp>
#include <astra/core/utf8.hpp>

#include <uv.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace astra::process {
namespace {

using Clock = std::chrono::steady_clock;

void check_uv(int result, std::string_view operation) {
    if (result < 0) { throw std::runtime_error(std::string(operation) + ": " + uv_strerror(result)); }
}

void validate_string(std::string_view text) {
    if (text.find('\0') != text.npos) { throw std::invalid_argument("process arguments contain NUL"); }
    utf8::validate(text);
}

std::exception_ptr failure(std::string_view message) {
    return std::make_exception_ptr(std::runtime_error(std::string(message)));
}

std::string describe(std::exception_ptr error) {
    try { std::rethrow_exception(error); }
    catch (const std::exception& exception) { return exception.what(); }
    catch (...) { return "non-standard process callback exception"; }
}

#ifdef _WIN32
class WindowsHandle {
public:
    explicit WindowsHandle(HANDLE value = nullptr) noexcept : value_(value) {}
    ~WindowsHandle() { reset(); }
    WindowsHandle(const WindowsHandle&) = delete;
    WindowsHandle& operator=(const WindowsHandle&) = delete;
    WindowsHandle(WindowsHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    WindowsHandle& operator=(WindowsHandle&& other) noexcept {
        if (this != &other) { reset(); value_ = std::exchange(other.value_, nullptr); }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    void reset(HANDLE value = nullptr) noexcept {
        if (value_ && value_ != INVALID_HANDLE_VALUE && !CloseHandle(value_)) {
            std::fputs("Astra process Windows handle close failed\n", stderr);
        }
        value_ = value;
    }
private:
    HANDLE value_{};
};

class Job {
public:
    Job() {
        std::random_device random;
        constexpr char digits[] = "0123456789abcdef";
        name_ = "Local\\AstraProcess-";
        for (int index = 0; index < 32; ++index) { name_ += digits[random() & 15U]; }
        const std::wstring name(name_.begin(), name_.end());
        handle_.reset(CreateJobObjectW(nullptr, name.c_str()));
        if (!handle_.get()) { fail("create process job"); }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            throw std::runtime_error("process job name already exists");
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(handle_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            fail("configure process job");
        }
        port_.reset(CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1));
        if (!port_.get()) { fail("create process job completion port"); }
        JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
        association.CompletionKey = this;
        association.CompletionPort = port_.get();
        if (!SetInformationJobObject(handle_.get(), JobObjectAssociateCompletionPortInformation,
            &association, sizeof(association))) { fail("associate process job completion port"); }
    }
    ~Job() = default;
    Job(const Job&) = delete;
    Job& operator=(const Job&) = delete;
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] bool terminate() const noexcept { return TerminateJobObject(handle_.get(), 1) != FALSE; }
    [[nodiscard]] std::optional<DWORD> active() const noexcept {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION information{};
        if (!QueryInformationJobObject(handle_.get(), JobObjectBasicAccountingInformation,
            &information, sizeof(information), nullptr)) { return std::nullopt; }
        return information.ActiveProcesses;
    }

    // Retain real process handles, including descendants, before terminating the
    // job. A termination request/Job accounting change is not a wait operation.
    // NEW_PROCESS notifications also retain short-lived processes between polls;
    // the ID-list query covers notifications that Windows is permitted to omit.
    void observe() {
        for (std::size_t count = 0; count < 4096; ++count) {
            DWORD message = 0;
            ULONG_PTR key = 0;
            LPOVERLAPPED value = nullptr;
            if (!GetQueuedCompletionStatus(port_.get(), &message, &key, &value, 0)) {
                if (GetLastError() == WAIT_TIMEOUT) { break; }
                fail("read process job completion port");
            }
            if (key != reinterpret_cast<ULONG_PTR>(this)) { throw std::runtime_error("unexpected process job completion key"); }
            if (message == JOB_OBJECT_MSG_NEW_PROCESS || message == JOB_OBJECT_MSG_EXIT_PROCESS ||
                message == JOB_OBJECT_MSG_ABNORMAL_EXIT_PROCESS) {
                remember(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(value)));
            }
        }
        std::size_t capacity = 16;
        for (;;) {
            if (capacity > 65'536) { throw std::length_error("process job member limit exceeded"); }
            // ULONG_PTR storage satisfies the variable-sized API structure's
            // alignment; two DWORD counters precede its process-ID array.
            std::vector<ULONG_PTR> storage(capacity + 2);
            auto* ids = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
            const auto bytes = static_cast<DWORD>(storage.size() * sizeof(ULONG_PTR));
            if (QueryInformationJobObject(handle_.get(), JobObjectBasicProcessIdList, ids, bytes, nullptr)) {
                for (DWORD index = 0; index < ids->NumberOfProcessIdsInList; ++index) {
                    remember(static_cast<DWORD>(ids->ProcessIdList[index]));
                }
                break;
            }
            if (GetLastError() != ERROR_MORE_DATA) { fail("enumerate process job members"); }
            capacity = std::max(capacity * 2, static_cast<std::size_t>(ids->NumberOfAssignedProcesses));
        }
        std::erase_if(members_, [](const Member& member) {
            const auto state = WaitForSingleObject(member.handle.get(), 0);
            if (state == WAIT_FAILED) { fail("wait for process job member"); }
            return state == WAIT_OBJECT_0;
        });
    }

    [[nodiscard]] bool finished() {
        observe();
        const auto count = active();
        if (!count) { fail("query process job accounting"); }
        return *count == 0 && members_.empty();
    }
private:
    struct Member { DWORD id; WindowsHandle handle; };
    WindowsHandle port_;
    WindowsHandle handle_;
    std::vector<Member> members_;
    std::string name_;

    [[noreturn]] static void fail(const char* operation) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
    }

    void remember(DWORD id) {
        if (std::ranges::any_of(members_, [id](const Member& member) { return member.id == id; })) { return; }
        WindowsHandle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, id));
        if (!process.get()) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) { return; } // Already fully destroyed.
            fail("open process job member");
        }
        BOOL member = FALSE;
        if (!IsProcessInJob(process.get(), handle_.get(), &member)) { fail("verify process job membership"); }
        if (!member) { return; } // PID was recycled; never wait for an unrelated process.
        if (members_.size() >= 65'536) { throw std::length_error("process job member limit exceeded"); }
        members_.push_back({id, std::move(process)});
    }
};

std::string host_path() {
    std::wstring module(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (length == 0 || length >= module.size()) { throw std::runtime_error("cannot locate Astra process host"); }
    module.resize(length);
    const auto separator = module.find_last_of(L"\\/");
    module.resize(separator == module.npos ? 0 : separator + 1);
    module += L"astra-process-host.exe";
    const auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, module.data(), static_cast<int>(module.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) { throw std::runtime_error("Astra process host path is not valid UTF-16"); }
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, module.data(), static_cast<int>(module.size()),
        result.data(), bytes, nullptr, nullptr) == 0) { throw std::runtime_error("encode Astra process host path"); }
    return result;
}
#endif

} // namespace

struct Process::Impl {
    enum class State { idle, starting, running, closing, finished };
    struct Pipe {
        Impl* owner{};
        uv_pipe_t handle{};
        std::array<char, 64 * 1024> buffer{};
        std::string pending_startup;
        std::size_t received{};
        bool stderr_stream{};
        bool startup_stream{};
        bool initialized{};
        bool closed{};
    };
    struct Task {
        std::function<void()> function;
        std::function<void(std::exception_ptr)> on_cancel;
        std::shared_ptr<std::promise<void>> promise;
        std::string bytes;
        bool write{};
    };
    struct Write {
        Impl* owner{};
        uv_write_t request{};
        std::string bytes;
        std::shared_ptr<std::promise<void>> promise;
    };

    ProcessOptions options;
    ProcessCallbacks callbacks;
    std::mutex mutex;
    State state{State::idle};
    std::deque<Task> tasks;
    std::size_t reserved_bytes{};
    bool async_ready{};
    bool request_close{};
    bool request_kill{};
    std::thread worker;
    std::thread::id loop_thread;
    std::promise<void> started;
    std::promise<ProcessExit> completed;
    std::shared_future<ProcessExit> completion{completed.get_future().share()};

    uv_loop_t loop{};
    uv_async_t control{};
    uv_timer_t timer{};
    uv_process_t child{};
    uv_shutdown_t shutdown{};
    Pipe input;
    Pipe output;
    Pipe errors;
    Pipe startup;
    std::string startup_reply;
#ifdef _WIN32
    std::unique_ptr<Job> job;
    bool job_terminated{};
#else
    // An unreaped direct child pins its PID, and therefore this private PGID.
    // Only this loop may wait for it. Never signal the stored number after reap.
    pid_t owned_pid{};
#endif
    std::unordered_map<uv_write_t*, std::unique_ptr<Write>> writes;
    bool loop_initialized{};
    bool control_initialized{};
    bool timer_initialized{};
    bool child_started{};
    bool child_initialized{};
    bool child_exited{};
    bool child_closed{};
    bool shutdown_started{};
    bool closing{};
    bool killed{};
    bool error_reported{};
    bool started_set{};
    Clock::time_point shutdown_deadline{};
    ProcessExit result;

    Impl(ProcessOptions settings, ProcessCallbacks handlers)
        : options(std::move(settings)), callbacks(std::move(handlers)) {
        if (options.argv.empty() || options.argv.front().empty()) {
            throw std::invalid_argument("process executable is required");
        }
        for (const auto& argument : options.argv) { validate_string(argument); }
        validate_string(options.cwd);
        if (options.env) {
            for (const auto& variable : *options.env) {
                validate_string(variable);
                if (variable.find('=') == variable.npos) { throw std::invalid_argument("process environment entry has no equals sign"); }
            }
        }
        if (options.max_queued_stdin_bytes == 0 || options.max_queued_tasks == 0 ||
            options.shutdown_timeout <= std::chrono::milliseconds::zero() ||
            options.shutdown_timeout > std::chrono::hours(24) ||
            options.tick_interval <= std::chrono::milliseconds::zero() ||
            options.tick_interval > std::chrono::seconds(1)) {
            throw std::invalid_argument("invalid process queue or timeout limits");
        }
        input.owner = this;
        output.owner = this;
        errors.owner = this;
        errors.stderr_stream = true;
        startup.owner = this;
        startup.startup_stream = true;
    }

    ~Impl() {
        request_stop(true);
        if (worker.joinable()) {
            // The public ownership contract excludes destruction in callbacks;
            // joining a worker from that worker is never a valid lifetime.
            if (worker.get_id() == std::this_thread::get_id()) { std::terminate(); }
            worker.join();
        }
    }

    void release_bytes(std::size_t bytes) noexcept {
        const std::lock_guard lock(mutex);
        reserved_bytes -= bytes;
    }

    static void cancel(Task& task, std::exception_ptr error) noexcept {
        if (task.on_cancel) {
            try { task.on_cancel(error); }
            catch (...) { error = std::current_exception(); }
        }
        task.promise->set_exception(error);
    }

    void report(std::exception_ptr error) noexcept {
        if (error_reported) { return; }
        error_reported = true;
        try {
            result.error = describe(error);
            if (callbacks.on_error) { callbacks.on_error(error); }
        } catch (...) {
            // Surface a throwing error handler in the completion result too.
            try { result.error = describe(std::current_exception()); }
            catch (...) { std::fputs("Astra process error handler failed\n", stderr); }
        }
    }

    template <class Function>
    void invoke(Function&& function) noexcept {
        try { function(); }
        catch (...) { report(std::current_exception()); begin_close(true); }
    }

    static void close_pipe_callback(uv_handle_t* handle) noexcept {
        auto& pipe = *static_cast<Pipe*>(handle->data);
        pipe.closed = true;
        pipe.owner->maybe_finish();
    }

    void close_pipe(Pipe& pipe) noexcept {
        if (pipe.initialized && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&pipe.handle))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&pipe.handle), close_pipe_callback);
        }
    }

    void maybe_finish() noexcept {
        if (!child_exited || !child_closed ||
            (input.initialized && !input.closed) ||
            (output.initialized && !output.closed) ||
            (errors.initialized && !errors.closed) ||
            (startup.initialized && !startup.closed) || !writes.empty()) { return; }
#ifdef _WIN32
        if (job) {
            bool finished = false;
            try { finished = job->finished(); }
            catch (...) { report(std::current_exception()); }
            if (!finished) {
                if (!job_terminated) {
                    if (job->terminate()) { job_terminated = true; }
                    else { report(failure("cannot terminate remaining process job members")); }
                }
                return; // Timer keeps polling until every owned process exited.
            }
        }
#endif
        {
            const std::lock_guard lock(mutex);
            async_ready = false;
        }
        if (timer_initialized && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&timer))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&timer), nullptr);
        }
        if (control_initialized && !uv_is_closing(reinterpret_cast<uv_handle_t*>(&control))) {
            uv_close(reinterpret_cast<uv_handle_t*>(&control), nullptr);
        }
    }

    static void allocate(uv_handle_t* handle, std::size_t, uv_buf_t* buffer) noexcept {
        auto& pipe = *static_cast<Pipe*>(handle->data);
        *buffer = uv_buf_init(pipe.buffer.data(), static_cast<unsigned int>(pipe.buffer.size()));
    }

    static void read(uv_stream_t* stream, std::ptrdiff_t count, const uv_buf_t* buffer) noexcept {
        auto& pipe = *static_cast<Pipe*>(stream->data);
        auto& self = *pipe.owner;
        if (count > 0) {
            self.invoke([&] {
                const auto bytes = static_cast<std::size_t>(count);
                if (pipe.startup_stream) {
                    if (bytes > 1024 || self.startup_reply.size() > 1024 - bytes) {
                        throw std::runtime_error("process host startup reply is too large");
                    }
                    self.startup_reply.append(buffer->base, bytes);
                    if (self.startup_reply.find('\n') != std::string::npos) {
                        if (self.startup_reply != "OK\n") { throw std::runtime_error("process host launch failed: " + self.startup_reply); }
                        self.close_pipe(self.startup);
                        self.notify_started();
                    }
                    return;
                }
                if (!self.started_set) {
                    // The target can write immediately after ResumeThread,
                    // before libuv delivers the separate host startup message.
                    // Pause each stream after one bounded chunk so callbacks
                    // cannot close/fail a successfully launched process before
                    // on_started. No unbounded pre-start buffering is allowed.
                    check_uv(uv_read_stop(stream), "pause output before process startup");
                    pipe.pending_startup.assign(buffer->base, bytes);
                    return;
                }
                const auto limit = pipe.stderr_stream ? self.options.max_stderr_bytes : self.options.max_stdout_bytes;
                if ((limit != 0 && (bytes > limit || pipe.received > limit - bytes)) ||
                    bytes > std::numeric_limits<std::size_t>::max() - pipe.received) {
                    throw std::length_error(pipe.stderr_stream ? "process stderr limit exceeded" : "process stdout limit exceeded");
                }
                pipe.received += bytes;
                auto& callback = pipe.stderr_stream ? self.callbacks.on_stderr : self.callbacks.on_stdout;
                if (callback) { callback(std::string_view(buffer->base, bytes)); }
            });
        } else if (count < 0) {
            if (pipe.startup_stream && !self.started_set) {
                const auto error = failure("process host closed before confirming startup");
                self.started_set = true;
                self.started.set_exception(error);
                self.report(error);
                self.begin_close(true);
            }
            if (count != UV_EOF && count != UV_ECANCELED) {
                self.report(failure(std::string("read process output: ") + uv_strerror(static_cast<int>(count))));
                self.begin_close(true);
            }
            self.close_pipe(pipe);
        }
    }

    static void child_closed_callback(uv_handle_t* handle) noexcept {
        auto& self = *static_cast<Impl*>(handle->data);
        self.child_closed = true;
        self.maybe_finish();
    }

#ifdef _WIN32
    static void exited(uv_process_t* process, std::int64_t status, int signal) noexcept {
        auto& self = *static_cast<Impl*>(process->data);
        self.result.exit_status = status;
        self.result.term_signal = signal;
        self.child_exited = true;
        uv_close(reinterpret_cast<uv_handle_t*>(process), child_closed_callback);
        self.begin_close(false);
        self.close_pipe(self.input);
        // stdout/stderr remain readable until EOF, including buffered tail data.
    }
#else
    void report_errno(int error, const char* operation) noexcept {
        try { throw std::system_error(error, std::generic_category(), operation); }
        catch (...) { report(std::current_exception()); }
    }

    bool inspect_owned_child(siginfo_t& information) noexcept {
        if (owned_pid <= 0) { return false; }
        int status;
        do {
            status = waitid(P_PID, static_cast<id_t>(owned_pid), &information,
                WEXITED | WNOHANG | WNOWAIT);
        } while (status < 0 && errno == EINTR);
        if (status == 0) { return true; }
        const auto error = errno;
        if (error == ECHILD) {
            // Another reaper or SIGCHLD disposition violated our ownership.
            // Revoke the number before reporting; never signal a recycled PID.
            owned_pid = 0;
            child_exited = true;
        }
        report_errno(error, "observe owned process without reaping");
        return false;
    }

    bool kill_owned_group() noexcept {
        siginfo_t information{};
        if (!inspect_owned_child(information)) { return false; }
        // WNOWAIT retains even an already-exited leader until group cleanup.
        // With exclusive wait ownership there can be no PID reuse here.
        int status;
        do { status = ::kill(-owned_pid, SIGKILL); } while (status < 0 && errno == EINTR);
        if (status == 0 || errno == ESRCH) { return true; }
        report_errno(errno, "terminate owned process group");
        return false;
    }

    void poll_owned_child() noexcept {
        if (owned_pid <= 0) { return; }
        siginfo_t information{};
        if (!inspect_owned_child(information)) {
            begin_close(true);
            return;
        }
        if (information.si_pid == 0) { return; }

        // A normal leader exit also terminates same-group descendants, even if
        // they closed their inherited pipes already. Repeat a previous force
        // signal now, while the zombie leader still pins the group identifier.
        begin_close(false);
        if (!kill_owned_group() && owned_pid > 0 && Clock::now() < shutdown_deadline) {
            return; // Timer retries group cleanup with ownership still retained.
        }
        if (owned_pid <= 0) { close_pipe(input); maybe_finish(); return; }
        int status = 0;
        pid_t waited;
        do { waited = waitpid(owned_pid, &status, WNOHANG); } while (waited < 0 && errno == EINTR);
        if (waited == 0) { return; }
        if (waited < 0) {
            const auto error = errno;
            if (error != ECHILD) { report_errno(error, "reap owned process"); return; }
            owned_pid = 0;
            report_errno(error, "owned process was reaped by another owner");
        } else {
            owned_pid = 0; // No signal or PGID probe is legal beyond this point.
            result.exit_status = WIFEXITED(status) ? WEXITSTATUS(status) : 0;
            result.term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
        }
        child_exited = true;
        killed = true;
        shutdown_deadline = Clock::now() + options.shutdown_timeout;
        close_pipe(input);
        maybe_finish();
    }
#endif

    static void shutdown_done(uv_shutdown_t* request, int status) noexcept {
        auto& self = *static_cast<Impl*>(request->data);
        if (status < 0 && status != UV_ECANCELED && status != UV_EPIPE && !self.child_exited) {
            self.report(failure(std::string("close process stdin: ") + uv_strerror(status)));
        }
        self.close_pipe(self.input);
    }

    void cancel_queued(bool keep_writes) noexcept {
        std::deque<Task> pending;
        {
            const std::lock_guard lock(mutex);
            pending.swap(tasks);
        }
        for (auto& task : pending) {
            if (task.write && keep_writes && !child_exited) {
                try { submit_write(std::move(task.bytes), task.promise, true); }
                catch (...) { task.promise->set_exception(std::current_exception()); report(std::current_exception()); }
            } else {
                if (task.write) { release_bytes(task.bytes.size()); }
                cancel(task, failure("process closed before queued task ran"));
            }
        }
    }

    void begin_close(bool force) noexcept {
        if (!closing) {
            closing = true;
            shutdown_deadline = Clock::now() + options.shutdown_timeout;
            {
                const std::lock_guard lock(mutex);
                state = State::closing;
            }
            cancel_queued(!force);
            if (input.initialized && !shutdown_started &&
                !uv_is_closing(reinterpret_cast<uv_handle_t*>(&input.handle))) {
                shutdown_started = true;
                shutdown.data = this;
                const auto status = uv_shutdown(&shutdown, reinterpret_cast<uv_stream_t*>(&input.handle), shutdown_done);
                if (status < 0) {
                    if (status != UV_ENOTCONN && status != UV_EPIPE && !child_exited) {
                        report(failure(std::string("shutdown process stdin: ") + uv_strerror(status)));
                    }
                    close_pipe(input);
                }
            }
        }
        if (force && child_started && (!killed
#ifdef _WIN32
            || (job && !job_terminated)
#endif
            )) {
            result.terminated = result.terminated || !child_exited;
#ifdef _WIN32
            if (job) {
                try { job->observe(); }
                catch (...) { report(std::current_exception()); }
            }
            const auto active = job ? job->active() : std::optional<DWORD>{0};
            if (job && (!active || *active > 0)) {
                if (job->terminate()) { killed = true; job_terminated = true; }
                else { report(failure("terminate process job failed")); }
            } else if (!child_exited) {
                // Startup may fail before the host joins its job.
                const auto status = uv_process_kill(&child, SIGKILL);
                if (status >= 0 || status == UV_ESRCH) { killed = true; }
                else { report(failure(std::string("kill process host: ") + uv_strerror(status))); }
            } else { killed = true; job_terminated = true; }
#else
            if (owned_pid > 0) { killed = kill_owned_group(); }
            else { killed = true; }
#endif
            if (killed) { shutdown_deadline = Clock::now() + options.shutdown_timeout; }
            close_pipe(input);
        }
        maybe_finish();
    }

    static void write_done(uv_write_t* request, int status) noexcept {
        auto* write = static_cast<Write*>(request->data);
        auto& self = *write->owner;
        const auto size = write->bytes.size();
        const auto promise = write->promise;
        self.writes.erase(request);
        self.release_bytes(size);
        if (status < 0) {
            const auto error = failure(std::string("write process stdin: ") + uv_strerror(status));
            if (promise) { promise->set_exception(error); }
            if (!self.closing && !self.child_exited) { self.report(error); self.begin_close(true); }
        } else if (promise) { promise->set_value(); }
        self.maybe_finish();
    }

    void submit_write(std::string bytes, std::shared_ptr<std::promise<void>> promise,
                       bool reserved) {
        const auto byte_count = bytes.size();
        if (!reserved) {
            const std::lock_guard lock(mutex);
            if (state != State::running || byte_count > options.max_queued_stdin_bytes ||
                reserved_bytes > options.max_queued_stdin_bytes - byte_count) {
                throw std::runtime_error("process stdin is closed or queue capacity exceeded");
            }
            reserved_bytes += byte_count;
        }
        try {
            if (!child_started || child_exited || !input.initialized ||
                uv_is_closing(reinterpret_cast<uv_handle_t*>(&input.handle)) ||
                byte_count > std::numeric_limits<unsigned int>::max()) {
                throw std::runtime_error("process stdin pipe is not writable");
            }
            if (bytes.empty()) {
                release_bytes(byte_count);
                if (promise) { promise->set_value(); }
                return;
            }
            auto write = std::make_unique<Write>();
            write->owner = this;
            write->bytes = std::move(bytes);
            write->promise = std::move(promise);
            write->request.data = write.get();
            const auto request = &write->request;
            const auto buffer = uv_buf_init(write->bytes.data(), static_cast<unsigned int>(byte_count));
            writes.emplace(request, std::move(write));
            const auto status = uv_write(request, reinterpret_cast<uv_stream_t*>(&input.handle), &buffer, 1, write_done);
            if (status < 0) { writes.erase(request); check_uv(status, "write process stdin"); }
        } catch (...) { release_bytes(byte_count); throw; }
    }

    void drain_tasks() noexcept {
        bool stop = false;
        bool force = false;
        {
            const std::lock_guard lock(mutex);
            stop = request_close;
            force = request_kill;
        }
        if (stop || force) { begin_close(force); return; }
        if (!started_set) { return; }
        // Bound each dispatch turn so flood producers cannot starve I/O/timers.
        for (std::size_t count = 0; count < 64; ++count) {
            Task task;
            {
                const std::lock_guard lock(mutex);
                if (tasks.empty()) { break; }
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            try {
                if (task.write) { submit_write(std::move(task.bytes), task.promise, true); }
                else { task.function(); task.promise->set_value(); }
            } catch (...) {
                const auto error = std::current_exception();
                task.promise->set_exception(error);
                report(error);
                begin_close(true);
                break;
            }
            if (closing) { break; }
        }
        const std::lock_guard lock(mutex);
        if (!tasks.empty() && async_ready) { (void)uv_async_send(&control); }
    }

    static void control_ready(uv_async_t* handle) noexcept {
        static_cast<Impl*>(handle->data)->drain_tasks();
    }

    static void tick(uv_timer_t* handle) noexcept {
        auto& self = *static_cast<Impl*>(handle->data);
#ifdef _WIN32
        if (self.job) { self.invoke([&] { self.job->observe(); }); }
#else
        self.poll_owned_child();
#endif
        self.drain_tasks();
        if (self.started_set && !self.closing && self.callbacks.on_tick) { self.invoke([&] { self.callbacks.on_tick(); }); }
        if (self.closing && Clock::now() >= self.shutdown_deadline) {
            self.begin_close(true);
            if (Clock::now() >= self.shutdown_deadline && self.child_exited && ((!self.output.closed && self.output.initialized) ||
                (!self.errors.closed && self.errors.initialized))) {
                self.report(failure("process exited but output pipes did not close before deadline"));
                self.close_pipe(self.output);
                self.close_pipe(self.errors);
            }
        }
        self.maybe_finish();
    }

    void initialize_pipe(Pipe& pipe) {
        check_uv(uv_pipe_init(&loop, &pipe.handle, 0), "initialize process pipe");
        pipe.initialized = true;
        pipe.handle.data = &pipe;
    }

    void notify_started() {
        if (started_set) { return; }
        {
            const std::lock_guard lock(mutex);
            if (request_close || request_kill || error_reported) { throw std::runtime_error("process closed during startup"); }
            state = child_exited ? State::closing : State::running;
        }
        if (callbacks.on_started) { callbacks.on_started(); }
        started_set = true;
        started.set_value();
        for (auto* pipe : {&output, &errors}) {
            if (pipe->pending_startup.empty()) { continue; }
            auto pending = std::move(pipe->pending_startup);
            const auto stream = reinterpret_cast<uv_stream_t*>(&pipe->handle);
            check_uv(uv_read_start(stream, allocate, read), "resume output after process startup");
            const auto buffer = uv_buf_init(pending.data(), static_cast<unsigned int>(pending.size()));
            read(stream, static_cast<std::ptrdiff_t>(pending.size()), &buffer);
        }
        drain_tasks();
    }

    void run() noexcept {
        {
            const std::lock_guard lock(mutex);
            loop_thread = std::this_thread::get_id();
        }
        try {
            check_uv(uv_loop_init(&loop), "initialize process loop");
            loop_initialized = true;
            check_uv(uv_async_init(&loop, &control, control_ready), "initialize process control");
            control_initialized = true;
            control.data = this;
            check_uv(uv_timer_init(&loop, &timer), "initialize process timer");
            timer_initialized = true;
            timer.data = this;
            initialize_pipe(input);
            initialize_pipe(output);
            initialize_pipe(errors);
#ifdef _WIN32
            initialize_pipe(startup);
            job = std::make_unique<Job>();
            options.argv.insert(options.argv.begin(), job->name());
            options.argv.insert(options.argv.begin(), host_path());
#endif
            std::vector<char*> argv;
            argv.reserve(options.argv.size() + 1);
            for (auto& argument : options.argv) { argv.push_back(argument.data()); }
            argv.push_back(nullptr);
            std::vector<char*> environment;
            if (options.env) {
                for (auto& variable : *options.env) { environment.push_back(variable.data()); }
                environment.push_back(nullptr);
            }
#ifdef _WIN32
            std::array<uv_stdio_container_t, 4> stdio{};
#else
            std::array<uv_stdio_container_t, 3> stdio{};
#endif
            stdio[0].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_READABLE_PIPE);
            stdio[0].data.stream = reinterpret_cast<uv_stream_t*>(&input.handle);
            stdio[1].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[1].data.stream = reinterpret_cast<uv_stream_t*>(&output.handle);
            stdio[2].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[2].data.stream = reinterpret_cast<uv_stream_t*>(&errors.handle);
#ifdef _WIN32
            stdio[3].flags = static_cast<uv_stdio_flags>(UV_CREATE_PIPE | UV_WRITABLE_PIPE);
            stdio[3].data.stream = reinterpret_cast<uv_stream_t*>(&startup.handle);
#endif
            uv_process_options_t spawn{};
#ifdef _WIN32
            spawn.exit_cb = exited;
#endif
            spawn.file = argv.front();
            spawn.args = argv.data();
            spawn.cwd = options.cwd.empty() ? nullptr : options.cwd.c_str();
            spawn.env = options.env ? environment.data() : nullptr;
            spawn.stdio_count = static_cast<int>(stdio.size());
            spawn.stdio = stdio.data();
#ifdef _WIN32
            // DETACHED skips libuv's global SILENT_BREAKAWAY job. The private
            // Astra job owns the host and descendants instead, avoiding a race
            // between uv_spawn's job assignment and the host's own assignment.
            // The uv_process handle remains referenced and is always waited.
            spawn.flags = UV_PROCESS_WINDOWS_HIDE | UV_PROCESS_DETACHED;
#else
            // libuv performs setsid before exec. Reject automatic reaping: this
            // adapter must retain the leader until it has signalled the group.
            struct sigaction disposition{};
            if (sigaction(SIGCHLD, nullptr, &disposition) != 0) {
                throw std::system_error(errno, std::generic_category(), "inspect SIGCHLD disposition");
            }
            if (disposition.sa_handler == SIG_IGN || (disposition.sa_flags & SA_NOCLDWAIT) != 0) {
                throw std::runtime_error("process ownership requires waitable children (SIGCHLD is ignored)");
            }
            spawn.flags = UV_PROCESS_DETACHED;
            // Start before spawning: failures after exec must still have a
            // polling owner to kill/reap, including failed stdio initialization.
            check_uv(uv_timer_start(&timer, tick, static_cast<std::uint64_t>(options.tick_interval.count()),
                static_cast<std::uint64_t>(options.tick_interval.count())), "start process timer");
#endif
            child.data = this;
            // uv_spawn initializes its handle even when spawning fails; it must
            // be closed on either path, unlike ordinary libuv init functions.
            child_initialized = true;
#ifdef _WIN32
            check_uv(uv_spawn(&loop, &child, &spawn), "spawn process");
            child_started = true;
#else
            const auto spawn_status = uv_spawn(&loop, &child, &spawn);
            // uv_spawn may return a stdio error after successfully executing the
            // child. Its active handle identifies that separately from status.
            if (uv_is_active(reinterpret_cast<uv_handle_t*>(&child))) {
                child_started = true;
                owned_pid = child.pid;
                // Closing before uv_run removes libuv's waitpid ownership.
                // Our timer uses waitid(WNOWAIT), then cleans the group before
                // the single waitpid. No libuv internals are accessed.
                uv_close(reinterpret_cast<uv_handle_t*>(&child), child_closed_callback);
            }
            check_uv(spawn_status, "spawn process");
#endif
            check_uv(uv_read_start(reinterpret_cast<uv_stream_t*>(&output.handle), allocate, read), "read process stdout");
            check_uv(uv_read_start(reinterpret_cast<uv_stream_t*>(&errors.handle), allocate, read), "read process stderr");
#ifdef _WIN32
            check_uv(uv_read_start(reinterpret_cast<uv_stream_t*>(&startup.handle), allocate, read), "read process startup");
            check_uv(uv_timer_start(&timer, tick, static_cast<std::uint64_t>(options.tick_interval.count()),
                static_cast<std::uint64_t>(options.tick_interval.count())), "start process timer");
#endif
            {
                const std::lock_guard lock(mutex);
                async_ready = true;
            }
#ifndef _WIN32
            notify_started();
#endif
            drain_tasks();
        } catch (...) {
            const auto error = std::current_exception();
            report(error);
            if (!started_set) { started_set = true; started.set_exception(error); }
            if (child_started) {
                begin_close(true);
                // Windows still receives its exit callback; Unix's timer was
                // started before spawn and retains manual kill/reap ownership.
                close_pipe(output);
                close_pipe(errors);
                close_pipe(startup);
            } else {
                child_exited = true;
                if (child_initialized) {
                    uv_close(reinterpret_cast<uv_handle_t*>(&child), child_closed_callback);
                } else { child_closed = true; }
                begin_close(true);
                close_pipe(input);
                close_pipe(output);
                close_pipe(errors);
                close_pipe(startup);
                maybe_finish();
            }
        }
        if (loop_initialized) {
            (void)uv_run(&loop, UV_RUN_DEFAULT);
            const auto status = uv_loop_close(&loop);
            if (status < 0) { report(failure(std::string("close process loop: ") + uv_strerror(status))); }
        }
        cancel_queued(false);
        if (!started_set) { started_set = true; started.set_exception(failure(result.error.empty() ? "process failed during startup" : result.error)); }
        if (callbacks.on_exit) {
            try { callbacks.on_exit(result); }
            catch (...) { report(std::current_exception()); }
        }
        {
            const std::lock_guard lock(mutex);
            async_ready = false;
            state = State::finished;
        }
        completed.set_value(std::move(result));
    }

    std::future<void> start() {
        const std::lock_guard lock(mutex);
        if (state != State::idle) { throw std::logic_error("process can only be started once"); }
        auto future = started.get_future();
        state = State::starting;
        try { worker = std::thread([this] { run(); }); }
        catch (...) {
            state = State::finished;
            started.set_exception(std::current_exception());
            completed.set_value({0, 0, false, "could not create process worker"});
            throw;
        }
        return future;
    }

    std::future<void> enqueue(Task task) {
        auto future = task.promise->get_future();
        std::exception_ptr rejected;
        {
            const std::lock_guard lock(mutex);
            if (state != State::starting && state != State::running) {
                rejected = failure("process is not accepting tasks");
            } else if (tasks.size() >= options.max_queued_tasks ||
                (task.write && (task.bytes.size() > options.max_queued_stdin_bytes ||
                reserved_bytes > options.max_queued_stdin_bytes - task.bytes.size()))) {
                rejected = failure("process task queue capacity exceeded");
            } else {
                const auto size = task.write ? task.bytes.size() : 0;
                try {
                    tasks.push_back(std::move(task));
                    reserved_bytes += size;
                    if (async_ready) { (void)uv_async_send(&control); }
                } catch (...) { rejected = std::current_exception(); }
            }
        }
        if (rejected) { cancel(task, rejected); }
        return future;
    }

    void request_stop(bool force) noexcept {
        const std::lock_guard lock(mutex);
        if (state == State::finished) { return; }
        if (state == State::idle) {
            state = State::finished;
            completed.set_value({0, 0, force, "process was closed before start"});
            return;
        }
        request_close = true;
        request_kill = request_kill || force;
        state = State::closing;
        if (async_ready) { (void)uv_async_send(&control); }
    }
};

Process::Process(ProcessOptions options, ProcessCallbacks callbacks)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(callbacks))) {}
Process::~Process() = default;
std::future<void> Process::start() { return impl_->start(); }
std::future<void> Process::write(std::string bytes) {
    return impl_->enqueue({{}, {}, std::make_shared<std::promise<void>>(), std::move(bytes), true});
}
void Process::write_now(std::string bytes) {
    {
        const std::lock_guard lock(impl_->mutex);
        if (std::this_thread::get_id() != impl_->loop_thread) { throw std::logic_error("write_now requires process loop owner"); }
    }
    impl_->submit_write(std::move(bytes), {}, false);
}
std::future<void> Process::post(std::function<void()> task,
    std::function<void(std::exception_ptr)> on_cancel) {
    if (!task) { throw std::invalid_argument("process post task is empty"); }
    return impl_->enqueue({std::move(task), std::move(on_cancel),
        std::make_shared<std::promise<void>>(), {}, false});
}
void Process::close() noexcept { impl_->request_stop(false); }
void Process::terminate() noexcept { impl_->request_stop(true); }
std::shared_future<ProcessExit> Process::completion() const { return impl_->completion; }

std::vector<std::string> current_environment() {
    uv_env_item_t* environment = nullptr;
    int count = 0;
    check_uv(uv_os_environ(&environment, &count), "read process environment");
    struct Cleanup {
        uv_env_item_t* data;
        int count;
        ~Cleanup() { uv_os_free_environ(data, count); }
    } cleanup{environment, count};
    std::vector<std::string> result;
    result.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        result.push_back(std::string(environment[index].name) + "=" + environment[index].value);
    }
    return result;
}

} // namespace astra::process
