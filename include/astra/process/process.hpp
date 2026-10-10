#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace astra::process {

struct ProcessOptions {
    // Includes executable as argv[0]. UTF-8 argv is passed directly to uv_spawn;
    // no shell is involved. Windows windows_verbatim_arguments is never enabled.
    std::vector<std::string> argv;
    std::string cwd;
    // Complete KEY=value environment, or nullopt to inherit the parent environment.
    std::optional<std::vector<std::string>> env;
    std::size_t max_queued_stdin_bytes{8 * 1024 * 1024};
    std::size_t max_queued_tasks{4096};
    // Output streams are delivered without accumulating them in this adapter.
    // Zero means no cumulative limit; framing/consumer limits still apply.
    std::size_t max_stdout_bytes{};
    std::size_t max_stderr_bytes{8 * 1024 * 1024};
    std::chrono::milliseconds shutdown_timeout{500};
    std::chrono::milliseconds tick_interval{10};
};

struct ProcessExit {
    std::int64_t exit_status{};
    int term_signal{};
    bool terminated{};
    std::string error;
};

struct ProcessCallbacks {
    std::function<void()> on_started;
    std::function<void(std::string_view)> on_stdout;
    std::function<void(std::string_view)> on_stderr;
    std::function<void(const ProcessExit&)> on_exit;
    std::function<void(std::exception_ptr)> on_error;
    std::function<void()> on_tick;
};

// One private libuv loop thread owns every handle. All callbacks run on that
// thread and must not block. post cancellation can run on the submitting thread.
// start is called once; requests before start are rejected. close/terminate may
// be requested from callbacks. Destroy the Process from its external owner, not
// from inside a callback. Destruction terminates and joins the worker safely.
// Windows requires astra-process-host.exe beside the calling executable. The
// host joins a private Job Object before starting the target; completion also
// waits for every owned descendant to exit, including inherited pipe holders.
class Process {
  public:
    Process(ProcessOptions options, ProcessCallbacks callbacks = {});
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    Process(Process&&) = delete;
    Process& operator=(Process&&) = delete;

    [[nodiscard]] std::future<void> start();
    [[nodiscard]] std::future<void> write(std::string bytes);
    // Same loop owner only. Throws immediately for queue capacity, closure or an
    // invalid pipe. Async write failures are reported through on_error.
    void write_now(std::string bytes);
    [[nodiscard]] std::future<void> post(std::function<void()> task,
                                         std::function<void(std::exception_ptr)> on_cancel = {});
    // Gracefully close stdin, then kill after shutdown_timeout. Already queued
    // writes finish; queued arbitrary tasks are cancelled instead of hanging.
    void close() noexcept;
    void terminate() noexcept;
    [[nodiscard]] std::shared_future<ProcessExit> completion() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// UTF-8 environment snapshot for constructing an explicit sanitized child env.
[[nodiscard]] std::vector<std::string> current_environment();

} // namespace astra::process
