#include <astra/process/process.hpp>
#include "support/temporary_directory.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <uv.h>
#endif

namespace {

using astra::process::Process;
using astra::process::ProcessCallbacks;
using astra::process::ProcessExit;
using astra::process::ProcessOptions;
using namespace std::chrono_literals;

[[nodiscard]] std::string fixture_path() {
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const auto count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (count == 0 || count >= path.size()) {
        throw std::runtime_error("cannot locate process test fixture");
    }
    path.resize(count);
    const auto fixture = std::filesystem::path(path).parent_path() / "astra-process-fixture.exe";
    const auto utf8 = fixture.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
#else
    std::vector<char> path(1024);
    for (;;) {
        auto size = path.size();
        const auto status = uv_exepath(path.data(), &size);
        if (status == 0 && size < path.size() - 1) {
            return (std::filesystem::path(std::string(path.data(), size)).parent_path() /
                    "astra-process-fixture")
                .string();
        }
        if (path.size() >= 1024 * 1024) {
            throw std::length_error("process test executable path exceeds one MiB");
        }
        if (status != 0 && status != UV_ENOBUFS) {
            throw std::runtime_error(std::string("cannot locate process test fixture: ") +
                                     uv_strerror(status));
        }
        path.resize(path.size() * 2);
    }
#endif
}

[[nodiscard]] ProcessOptions options(std::string mode) {
    ProcessOptions result;
    result.argv = {fixture_path(), std::move(mode)};
    result.shutdown_timeout = 150ms;
    return result;
}

[[nodiscard]] ProcessExit finish(Process& process) {
    const auto future = process.completion();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    return future.get();
}

} // namespace

TEST_CASE("process argv preserves Unicode spaces quotes metacharacters and empty arguments") {
    auto settings = options("arguments");
    const std::vector<std::string> values{
        "two words", "a\"b", "trailing\\", "", "$(literal)&|;", "\xe6\x98\x9f\xf0\x9f\x98\x80"};
    settings.argv.insert(settings.argv.end(), values.begin(), values.end());
    std::string output;
    ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) {
        output += bytes;
    };
    Process process(std::move(settings), std::move(callbacks));
    process.start().get();
    const auto result = finish(process);
    CHECK(result.exit_status == 0);
    CHECK(result.error.empty());
    std::string expected;
    for (const auto& value : values) {
        expected += std::to_string(value.size()) + ":" + value + "\n";
    }
    CHECK(output == expected);
}

TEST_CASE("process stdin writes drain before graceful close and preserve binary bytes") {
    std::string output;
    ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) {
        output += bytes;
    };
    Process process(options("echo"), std::move(callbacks));
    process.start().get();
    const std::string binary("a\0b\r\n", 5);
    auto first = process.write(binary);
    auto second = process.write(std::string(200'000, 'x'));
    process.close();
    first.get();
    second.get();
    const auto result = finish(process);
    CHECK(result.exit_status == 0);
    CHECK_FALSE(result.terminated);
    CHECK(result.error.empty());
    CHECK(output == binary + std::string(200'000, 'x'));
}

TEST_CASE("process completion and exit callback occur after all trailing output") {
    std::string output, errors;
    std::size_t stdout_at_exit = 0;
    std::string stderr_at_exit;
    ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) {
        output += bytes;
    };
    callbacks.on_stderr = [&](std::string_view bytes) {
        errors += bytes;
    };
    callbacks.on_exit = [&](const ProcessExit&) {
        stdout_at_exit = output.size();
        stderr_at_exit = errors;
    };
    Process process(options("tail"), std::move(callbacks));
    process.start().get();
    const auto result = finish(process);
    CHECK(result.exit_status == 7);
    CHECK(result.error.empty());
    CHECK(output == std::string(128 * 1024, 't') + "TAIL");
    CHECK(errors == "error-tail");
    CHECK(stdout_at_exit == output.size());
    CHECK(stderr_at_exit == errors);
}

TEST_CASE("process explicit environment replaces inherited environment") {
    auto settings = options("environment");
    settings.env = astra::process::current_environment();
    std::erase_if(*settings.env, [](const std::string& item) {
        return item.starts_with("ASTRA_PROCESS_TEST=");
    });
    settings.env->push_back("ASTRA_PROCESS_TEST=isolated-child");
    std::string output;
    ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) {
        output += bytes;
    };
    Process process(std::move(settings), std::move(callbacks));
    process.start().get();
    CHECK(finish(process).error.empty());
    CHECK(output == "isolated-child");
}

TEST_CASE("process failed spawn resolves start completion and queued cancellation") {
    auto settings = options("echo");
    settings.argv[0] += ".does-not-exist";
    std::atomic<int> errors{};
    ProcessCallbacks callbacks;
    callbacks.on_error = [&](std::exception_ptr) {
        ++errors;
    };
    Process process(std::move(settings), std::move(callbacks));
    auto starting = process.start();
    std::atomic<int> cancellations{};
    auto queued = process.post(
        [] {
        },
        [&](std::exception_ptr) {
            ++cancellations;
        });
    CHECK_THROWS_AS(starting.get(), std::runtime_error);
    const auto result = finish(process);
    CHECK_FALSE(result.error.empty());
    CHECK(errors == 1);
    CHECK_THROWS_AS(queued.get(), std::runtime_error);
    CHECK(cancellations == 1);
}

TEST_CASE("process callbacks posts writes and ticks execute on one owner thread") {
    std::vector<std::thread::id> threads;
    std::string output;
    std::atomic<int> ticks{};
    Process* borrowed = nullptr;
    ProcessCallbacks callbacks;
    callbacks.on_started = [&] {
        threads.push_back(std::this_thread::get_id());
        borrowed->write_now("owned");
    };
    callbacks.on_stdout = [&](std::string_view bytes) {
        threads.push_back(std::this_thread::get_id());
        output += bytes;
    };
    callbacks.on_tick = [&] {
        threads.push_back(std::this_thread::get_id());
        if (++ticks == 2) {
            borrowed->close();
        }
    };
    callbacks.on_exit = [&](const ProcessExit&) {
        threads.push_back(std::this_thread::get_id());
    };
    Process process(options("echo"), std::move(callbacks));
    borrowed = &process;
    process.start().get();
    auto posted = process.post([&] {
        threads.push_back(std::this_thread::get_id());
    });
    posted.get();
    CHECK(finish(process).error.empty());
    REQUIRE_FALSE(threads.empty());
    CHECK(std::ranges::all_of(threads, [&](auto thread) {
        return thread == threads.front();
    }));
    CHECK(threads.front() != std::this_thread::get_id());
    CHECK(output == "owned");
    CHECK_THROWS_AS(process.write_now("wrong-thread"), std::logic_error);
}

TEST_CASE("process pending post is explicitly cancelled when close wins") {
    Process* borrowed = nullptr;
    std::future<void> posted;
    std::atomic<bool> ran{};
    std::atomic<int> cancelled{};
    ProcessCallbacks callbacks;
    callbacks.on_started = [&] {
        posted = borrowed->post(
            [&] {
                ran = true;
            },
            [&](std::exception_ptr) {
                ++cancelled;
            });
        borrowed->close();
    };
    Process process(options("echo"), std::move(callbacks));
    borrowed = &process;
    process.start().get();
    CHECK(finish(process).error.empty());
    CHECK_THROWS_AS(posted.get(), std::runtime_error);
    CHECK_FALSE(ran);
    CHECK(cancelled == 1);
    auto late = process.post(
        [] {
        },
        [&](std::exception_ptr) {
            ++cancelled;
        });
    CHECK_THROWS_AS(late.get(), std::runtime_error);
    CHECK(cancelled == 2);
}

TEST_CASE("process stdin bounds reject excessive writes without terminating useful work") {
    auto settings = options("echo");
    settings.max_queued_stdin_bytes = 4;
    std::string output;
    ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) {
        output += bytes;
    };
    Process process(std::move(settings), std::move(callbacks));
    process.start().get();
    auto rejected = process.write("12345");
    CHECK_THROWS_AS(rejected.get(), std::runtime_error);
    process.write("1234").get();
    process.close();
    CHECK(finish(process).error.empty());
    CHECK(output == "1234");
}

TEST_CASE("process output limits terminate excessive children with a reported error") {
    auto settings = options("emit");
    settings.max_stdout_bytes = 1024;
    std::size_t delivered = 0;
    ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) {
        delivered += bytes.size();
    };
    Process process(std::move(settings), std::move(callbacks));
    process.start().get();
    const auto result = finish(process);
    CHECK_FALSE(result.error.empty());
    CHECK(delivered <= 1024);
}

TEST_CASE("process zero stderr limit supports streaming long-lived protocol servers") {
    auto settings = options("stderr");
    settings.max_stderr_bytes = 0;
    std::size_t delivered = 0;
    ProcessCallbacks callbacks;
    callbacks.on_stderr = [&](std::string_view bytes) {
        delivered += bytes.size();
    };
    Process process(std::move(settings), std::move(callbacks));
    process.start().get();
    CHECK(finish(process).error.empty());
    CHECK(delivered == 128 * 4096);
}

TEST_CASE("process close kills an unresponsive child after its grace period") {
    auto settings = options("wait");
    settings.shutdown_timeout = 50ms;
    Process process(std::move(settings));
    process.start().get();
    const auto start = std::chrono::steady_clock::now();
    process.close();
    const auto result = finish(process);
    CHECK(result.terminated);
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}

TEST_CASE("process terminate and destructor reap children and resolve completion") {
    std::shared_future<ProcessExit> completion;
    {
        Process process(options("wait"));
        process.start().get();
        completion = process.completion();
    }
    REQUIRE(completion.wait_for(1s) == std::future_status::ready);
    CHECK(completion.get().terminated);
}

TEST_CASE("process callback errors surface without escaping the libuv callback boundary") {
    ProcessCallbacks callbacks;
    callbacks.on_stdout = [](std::string_view) {
        throw std::runtime_error("consumer failed");
    };
    Process process(options("emit"), std::move(callbacks));
    process.start().get();
    const auto result = finish(process);
    CHECK(result.error == "consumer failed");
}

TEST_CASE("process options and lifecycle reject invalid input") {
    CHECK_THROWS_AS((void)Process(ProcessOptions{}), std::invalid_argument);
    auto settings = options("echo");
    settings.argv.push_back(std::string("bad\0argument", 12));
    CHECK_THROWS_AS((void)Process(settings), std::invalid_argument);
    settings = options("echo");
    settings.env = std::vector<std::string>{"missing-equals"};
    CHECK_THROWS_AS((void)Process(settings), std::invalid_argument);
    settings = options("echo");
    settings.max_queued_tasks = 0;
    CHECK_THROWS_AS((void)Process(settings), std::invalid_argument);
    Process process(options("echo"));
    CHECK_THROWS_AS(process.write("before-start").get(), std::runtime_error);
    process.start().get();
    CHECK_THROWS_AS((void)process.start(), std::logic_error);
    process.close();
    CHECK(finish(process).error.empty());
    Process idle(options("echo"));
    idle.close();
    CHECK_FALSE(idle.completion().get().error.empty());
    CHECK_THROWS_AS((void)idle.start(), std::logic_error);
}

#ifdef _WIN32
TEST_CASE(
    "Windows process completion releases target and descendant working directories immediately") {
    namespace fs = std::filesystem;
    const auto base = fs::weakly_canonical(fs::current_path() / "build");
    for (const auto mode : {"arguments", "tree", "arguments", "tree"}) {
        const auto nonce =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(std::random_device{}());
        const auto directory = base / ("process-cwd-tests-" + nonce);
        const auto renamed = base / ("process-cwd-tests-" + nonce + "-renamed");
        REQUIRE(fs::create_directory(directory));

        struct Cleanup {
            fs::path base, directory, renamed;

            ~Cleanup() {
                for (const auto& path : {directory, renamed}) {
                    for (const auto& issue :
                         astra::test::cleanup_owned_directory(path, base, "process-cwd-tests-")) {
                        CHECK_MESSAGE(false, "Process cwd cleanup: ", issue.message);
                    }
                }
            }
        } cleanup{base, directory, renamed};

        auto settings = options(mode);
        const auto path = directory.u8string();
        settings.cwd.assign(reinterpret_cast<const char*>(path.data()), path.size());
        Process process(std::move(settings));
        process.start().get();
        const auto result = finish(process);
        CHECK(result.exit_status == 0);
        CHECK(result.error.empty());
        // The Process itself is deliberately still alive: completion, not its
        // destructor or a test-side delay, must release every inherited cwd.
        CHECK_NOTHROW(fs::rename(directory, renamed));
        CHECK(fs::remove(renamed));
    }
}

TEST_CASE("Windows process job reaps grandchildren retaining pipes after target exit") {
    auto settings = options("tree");
    settings.shutdown_timeout = 400ms;
    std::string output;
    ProcessCallbacks callbacks;
    callbacks.on_stdout = [&](std::string_view bytes) {
        output += bytes;
    };
    Process process(std::move(settings), std::move(callbacks));
    const auto began = std::chrono::steady_clock::now();
    process.start().get();
    const auto result = finish(process);
    CHECK(result.exit_status == 0);
    CHECK(result.error.empty());
    CHECK(std::chrono::steady_clock::now() - began < 3s);
    std::istringstream records(output);
    std::string prefix;
    DWORD pid = 0;
    std::size_t children = 0;
    while (records >> prefix >> pid) {
        REQUIRE(prefix == "PID");
        const auto handle =
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (handle) {
            CHECK(WaitForSingleObject(handle, 0) == WAIT_OBJECT_0);
            CHECK(CloseHandle(handle) != FALSE);
        } else {
            CHECK(GetLastError() == ERROR_INVALID_PARAMETER);
        }
        ++children;
    }
    CHECK(children == 2);
}

TEST_CASE("Windows process startup failure still closes the private job") {
    auto settings = options("echo");
    settings.argv[0] += ".missing-target";
    Process process(std::move(settings));
    CHECK_THROWS_AS(process.start().get(), std::runtime_error);
    const auto result = finish(process);
    CHECK_FALSE(result.error.empty());
}
#endif
