#include <astra/ui/clipboard_client.hpp>
#include <astra/core/utf8.hpp>
#include <astra/process/process.hpp>
#include <astra/protocol/framing.hpp>
#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#endif

namespace astra::ui {
namespace {
using Json = protocol::Json;
using Clock = std::chrono::steady_clock;

std::string wire(const Json& value) {
    return protocol::encode_frame(value.dump(), ClipboardClient::max_wire_bytes);
}

std::exception_ptr failure(std::string message) {
    return std::make_exception_ptr(protocol::RpcError(-32004, std::move(message)));
}

#ifdef _WIN32
std::string current_desktop() {
    const auto name = [](HANDLE object) {
        std::array<wchar_t, 512> buffer{};
        DWORD needed{};
        if (!GetUserObjectInformationW(object, UOI_NAME, buffer.data(), sizeof(buffer), &needed) ||
            needed < sizeof(wchar_t) || needed > sizeof(buffer) ||
            buffer[needed / sizeof(wchar_t) - 1] != L'\0')
            throw std::runtime_error("Cannot determine clipboard helper desktop: " +
                                     std::to_string(GetLastError()));
        return std::wstring(buffer.data());
    };
    const auto wide =
        name(GetProcessWindowStation()) + L"\\" + name(GetThreadDesktop(GetCurrentThreadId()));
    const auto size = WideCharToMultiByte(CP_UTF8,
                                          WC_ERR_INVALID_CHARS,
                                          wide.data(),
                                          static_cast<int>(wide.size()),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (size <= 0)
        throw std::runtime_error("Cannot encode clipboard helper desktop");
    std::string encoded(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            wide.data(),
                            static_cast<int>(wide.size()),
                            encoded.data(),
                            size,
                            nullptr,
                            nullptr) != size)
        throw std::runtime_error("Cannot encode clipboard helper desktop");
    return encoded;
}
#endif
} // namespace

struct ClipboardClient::Impl {
    struct Job {
        std::string id;
        bool write{}, settled{}, stopping{}, launched{};
        std::string bytes;
        std::size_t limit{};
        std::size_t reserved{};
        Clock::time_point deadline;
        std::promise<Json> promise;
        std::future<void> started, sent;
        std::mutex mutex;
        std::optional<Json> reply;
        std::exception_ptr error;
        protocol::FrameDecoder decoder{max_wire_bytes};
        bool received{};         // Process loop only.
        std::string stderr_text; // Process loop only, bounded by ProcessOptions.
        // Destroy the process first: all callbacks borrow this Job.
        std::unique_ptr<process::Process> child;

        void report(std::exception_ptr incoming) {
            const std::lock_guard lock(mutex);
            if (!error)
                error = std::move(incoming);
        }
    };

    Options options;
    std::map<std::string, std::unique_ptr<Job>> jobs;
    std::string owner;
    std::string active;
    std::deque<std::string> queue;
    std::size_t reserved{};
    bool closing{};
    std::vector<std::string> errors;

    explicit Impl(Options configuration) : options(std::move(configuration)) {
        if (options.executable.empty() || !options.executable.is_absolute() ||
            options.timeout <= std::chrono::milliseconds::zero() ||
            options.timeout > std::chrono::seconds(30))
            throw std::invalid_argument("Invalid clipboard process options");
    }

    ~Impl() {
        for (auto& [id, job] : jobs) {
            static_cast<void>(id);
            job->child->terminate();
        }
        // Process destruction kills and joins owned helpers. Normal GUI shutdown
        // first uses close/poll/drained, so destruction has no external wait.
    }

    std::future<Json> submit(std::string id, bool write, std::size_t limit, std::string text) {
        if (closing || id.empty() || id.size() > 128 || id.find('\0') != id.npos ||
            jobs.contains(id))
            throw std::runtime_error("Clipboard request identity is unavailable");
        utf8::validate(id);
        if (limit == 0 || limit > max_text_bytes || text.size() > max_text_bytes ||
            text.find('\0') != text.npos)
            throw std::invalid_argument("Clipboard text exceeds its byte limit or contains NUL");
        utf8::validate(text);
        const auto bytes = wire(
            {{"id", id}, {"kind", write ? "write" : "read"}, {"limit", limit}, {"text", text}});
        if (jobs.size() >= max_jobs || bytes.size() > max_wire_bytes + 128 - reserved)
            throw std::runtime_error("Clipboard process capacity exceeded");
        auto job = std::make_unique<Job>();
        job->id = id;
        job->write = write;
        job->bytes = bytes;
        job->limit = limit;
        job->reserved = bytes.size();
        job->deadline = Clock::now() + options.timeout;
        auto future = job->promise.get_future();
        auto* incoming = job.get();
        process::ProcessOptions process_options;
        const auto path = options.executable.u8string();
        process_options.argv = {
            std::string(reinterpret_cast<const char*>(path.data()), path.size())};
#ifdef _WIN32
        process_options.windows_desktop = current_desktop();
#endif
        process_options.max_queued_stdin_bytes = max_wire_bytes + 128;
        process_options.max_stdout_bytes = max_wire_bytes + 128;
        process_options.max_stderr_bytes = 4096;
        process_options.shutdown_timeout = std::chrono::milliseconds(250);
        process::ProcessCallbacks callbacks;
        callbacks.on_stdout = [incoming](std::string_view chunk) {
            for (const auto& body : incoming->decoder.feed(chunk)) {
                if (incoming->received)
                    throw std::runtime_error("Clipboard helper sent multiple replies");
                incoming->received = true;
                auto reply = Json::parse(body);
                if (!reply.is_object() || reply.at("id") != incoming->id ||
                    (reply.contains("result") == reply.contains("error")))
                    throw std::runtime_error("Clipboard helper returned an invalid reply");
                if (reply.contains("result")) {
                    const auto& value = reply.at("result");
                    if (incoming->write ? (!value.is_boolean() || value != true)
                                        : !value.is_string())
                        throw std::runtime_error("Clipboard helper returned an invalid result");
                    if (!incoming->write) {
                        const auto& decoded = value.get_ref<const std::string&>();
                        if (decoded.size() > incoming->limit || decoded.find('\0') != decoded.npos)
                            throw std::runtime_error("Clipboard helper text exceeds its bound");
                        utf8::validate(decoded);
                    }
                }
                const std::lock_guard lock(incoming->mutex);
                incoming->reply = std::move(reply);
            }
        };
        callbacks.on_stderr = [incoming](std::string_view bytes) {
            incoming->stderr_text.append(bytes);
        };
        callbacks.on_error = [incoming](std::exception_ptr error) {
            incoming->report(std::move(error));
        };
        callbacks.on_exit = [incoming](const process::ProcessExit& exit) {
            try {
                incoming->decoder.finish();
                if (!exit.error.empty() || exit.exit_status != 0 ||
                    (!incoming->received && !exit.terminated))
                    throw std::runtime_error("Clipboard helper exited: " + exit.error + " " +
                                             incoming->stderr_text);
            } catch (...) {
                incoming->report(std::current_exception());
            }
        };
        job->child =
            std::make_unique<process::Process>(std::move(process_options), std::move(callbacks));
        auto [entry, inserted] = jobs.emplace(id, std::move(job));
        static_cast<void>(inserted);
        reserved += bytes.size();
        try {
            queue.push_back(id);
        } catch (...) {
            reserved -= entry->second->reserved;
            jobs.erase(entry);
            throw;
        }
        return future;
    }

    void settle(Job& job, std::exception_ptr error, Json value = nullptr) {
        if (job.settled)
            return;
        job.settled = true;
        reserved -= std::exchange(job.reserved, 0);
        if (error)
            job.promise.set_exception(std::move(error));
        else
            job.promise.set_value(std::move(value));
    }

    void stop(Job& job, std::exception_ptr error) {
        settle(job, std::move(error));
        job.stopping = true;
        job.child->terminate();
    }

    void poll() {
        while (active.empty() && !queue.empty() && !closing) {
            const auto id = std::move(queue.front());
            queue.pop_front();
            const auto found = jobs.find(id);
            if (found == jobs.end() || found->second->settled)
                continue;
            auto& job = *found->second;
            if (Clock::now() >= job.deadline) {
                stop(job, failure("Clipboard operation timed out before dispatch"));
                continue;
            }
            active = id;
            job.launched = true;
            try {
                job.started = job.child->start();
                job.sent = job.child->write(std::move(job.bytes));
            } catch (...) {
                stop(job, std::current_exception());
            }
        }
        for (auto entry = jobs.begin(); entry != jobs.end();) {
            auto& job = *entry->second;
            std::exception_ptr error;
            std::optional<Json> reply;
            {
                const std::lock_guard lock(job.mutex);
                error = std::exchange(job.error, {});
                reply = std::exchange(job.reply, {});
            }
            try {
                for (auto* ready : {&job.started, &job.sent}) {
                    if (ready->valid() &&
                        ready->wait_for(std::chrono::seconds::zero()) == std::future_status::ready)
                        ready->get();
                }
            } catch (...) {
                error = std::current_exception();
            }
            if (error) {
                if (job.settled && !job.stopping) {
                    try {
                        std::rethrow_exception(error);
                    } catch (const std::exception& reported) {
                        errors.push_back(reported.what());
                    }
                }
                stop(job, error);
            } else if (!job.settled && reply) {
                if (reply->contains("error")) {
                    stop(job, failure(reply->at("error").get<std::string>()));
                } else {
                    auto result = std::move(reply->at("result"));
                    if (job.write) {
                        if (!owner.empty()) {
                            auto& previous = *jobs.at(owner);
                            previous.stopping = true;
                            previous.child->close();
                        }
                        owner = job.id;
                    } else {
                        job.stopping = true;
                        job.child->close();
                    }
                    settle(job, {}, std::move(result));
                    active.clear();
                }
            }
            if (!job.settled && Clock::now() >= job.deadline)
                stop(job, failure("Clipboard operation timed out"));
            const auto complete = job.child->completion();
            if (complete.wait_for(std::chrono::seconds::zero()) == std::future_status::ready) {
                const auto exit = complete.get();
                if (job.launched &&
                    (!exit.error.empty() || (exit.exit_status != 0 && !exit.terminated)))
                    errors.push_back("Clipboard process cleanup failed: " + exit.error + " " +
                                     job.stderr_text);
                if (!job.settled)
                    settle(job,
                           failure("Clipboard helper exited before completion: " + exit.error));
                if (owner == job.id)
                    owner.clear();
                if (active == job.id)
                    active.clear();
                entry = jobs.erase(entry);
            } else {
                ++entry;
            }
        }
    }
};

ClipboardClient::ClipboardClient(Options options)
    : impl_(std::make_unique<Impl>(std::move(options))) {
}

ClipboardClient::~ClipboardClient() = default;

std::future<protocol::Json> ClipboardClient::read(std::string id, std::size_t limit) {
    return impl_->submit(std::move(id), false, limit, {});
}

std::future<protocol::Json> ClipboardClient::write(std::string id, std::string text) {
    return impl_->submit(std::move(id), true, max_text_bytes, std::move(text));
}

void ClipboardClient::cancel(const std::string& id) {
    if (const auto found = impl_->jobs.find(id);
        found != impl_->jobs.end() && !found->second->settled)
        impl_->stop(*found->second, failure("Clipboard operation cancelled"));
}

void ClipboardClient::poll() {
    impl_->poll();
}

void ClipboardClient::cancel_pending() {
    for (auto& [id, job] : impl_->jobs) {
        static_cast<void>(id);
        if (!job->settled)
            impl_->stop(*job, failure("Clipboard operation cancelled"));
    }
}

void ClipboardClient::close() {
    impl_->closing = true;
    for (auto& [id, job] : impl_->jobs) {
        static_cast<void>(id);
        if (!job->settled)
            impl_->stop(*job, failure("Clipboard client closed"));
        else if (!job->stopping) {
            job->stopping = true;
            job->child->close();
        }
    }
}

bool ClipboardClient::idle() const noexcept {
    return std::all_of(impl_->jobs.begin(), impl_->jobs.end(), [this](const auto& entry) {
        return entry.first == impl_->owner && entry.second->settled && !entry.second->stopping;
    });
}

bool ClipboardClient::drained() const noexcept {
    return impl_->jobs.empty();
}

std::vector<std::string> ClipboardClient::take_errors() {
    return std::exchange(impl_->errors, {});
}
} // namespace astra::ui
