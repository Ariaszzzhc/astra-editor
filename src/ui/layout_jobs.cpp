#include <astra/ui/layout_jobs.hpp>
#include <uv.h>

#include <atomic>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace astra::ui {
struct LayoutJobs::Impl {
    using Clock = std::chrono::steady_clock;
    enum class Phase { pending, running, retire_pending, retiring, complete };
    enum class Stop { none, cancelled, expired };

    struct Job {
        uv_work_t work{};
        std::string text;
        TextSource previous;
        GlyphSource glyphs;
        TextLayoutOptions options;
        std::size_t bytes{};
        Clock::time_point deadline;
        Phase phase{Phase::pending};
        Stop stop{Stop::none}; // Owner thread only.
        std::atomic<bool> stopped{};
        LayoutJobResult result; // Worker owns until the completion callback.
        Impl& owner;
        Ticket ticket{};

        explicit Job(Impl& owner) : owner(owner) {
            work.data = this;
        }
    };

    uv_loop_t loop{};
    LayoutJobLimits limits;
    LayoutJobStatistics stats;
    std::map<Ticket, std::unique_ptr<Job>> jobs;
    Ticket sequence{};
    bool active{}, closing{};
    std::exception_ptr timeout_error;

    explicit Impl(LayoutJobLimits limits) : limits(limits) {
        if (!limits.jobs || limits.jobs > 256 || !limits.bytes ||
            limits.bytes > 256 * 1024 * 1024 || !limits.text_bytes ||
            limits.text_bytes > limits.bytes || limits.timeout.count() <= 0 ||
            limits.timeout > std::chrono::minutes(5)) {
            throw std::invalid_argument("Invalid layout job bounds");
        }
        timeout_error = std::make_exception_ptr(std::runtime_error("Text layout timed out"));
        const auto status = uv_loop_init(&loop);
        if (status) {
            throw std::runtime_error(uv_strerror(status));
        }
    }

    ~Impl() {
        // Public close/poll provides responsive draining; destruction is the
        // final lifetime guarantee for an unpreemptible native shaping call.
        close();
        while (!jobs.empty()) {
            start();
            uv_run(&loop, UV_RUN_ONCE);
        }
        if (uv_loop_close(&loop)) {
            std::terminate();
        }
    }

    void erase(Ticket ticket) noexcept {
        const auto found = jobs.find(ticket);
        if (found == jobs.end()) {
            return;
        }
        stats.bytes -= found->second->bytes;
        jobs.erase(found);
    }

    void finish(Job& job) noexcept {
        if (job.result.paragraph && job.stop != Stop::none) {
            job.phase = Phase::retire_pending;
            return;
        }
        if (job.stop == Stop::cancelled) {
            erase(job.ticket);
            return;
        }
        if (job.stop == Stop::expired) {
            job.result.error = timeout_error;
        }
        job.phase = Phase::complete;
        ++stats.completed;
    }

    static void work(uv_work_t* request) noexcept {
        auto& job = *static_cast<Job*>(request->data);
        try {
            if (job.phase == Phase::retiring) {
                job.result = {};
                return;
            }
            if (!job.stopped.load(std::memory_order_acquire)) {
                TextLayoutEngine engine(TextLayoutEngine::FontIdentity::content);
                job.result.paragraph =
                    std::make_shared<const TextLayout>(engine.layout(std::move(job.text),
                                                                     job.options,
                                                                     std::move(job.previous),
                                                                     std::move(job.glyphs)));
            }
            if (job.stopped.load(std::memory_order_acquire)) {
                job.result = {};
            }
        } catch (...) {
            job.result.error = std::current_exception();
        }
    }

    static void after(uv_work_t* request, int status) noexcept {
        auto& job = *static_cast<Job*>(request->data);
        auto& owner = job.owner;
        owner.active = false;
        owner.stats.running = 0;
        if (status) {
            try {
                throw std::runtime_error(uv_strerror(status));
            } catch (...) {
                job.result.error = std::current_exception();
            }
        }
        // No queue allocation from the C callback. Scheduling scans the bounded
        // map, prioritizing retirement before the oldest pending job.
        owner.finish(job);
    }

    void start() {
        if (active) {
            return;
        }
        for (;;) {
            auto candidate = jobs.end();
            for (auto it = jobs.begin(); it != jobs.end(); ++it) {
                if (it->second->phase == Phase::retire_pending) {
                    candidate = it;
                    break;
                }
                if (it->second->phase == Phase::pending && candidate == jobs.end()) {
                    candidate = it;
                }
            }
            if (candidate == jobs.end()) {
                return;
            }
            auto& job = *candidate->second;
            job.phase = job.phase == Phase::retire_pending ? Phase::retiring : Phase::running;
            const auto status = uv_queue_work(&loop, &job.work, work, after);
            if (status) {
                // Submission failed before any worker acquired this job.
                job.result.paragraph.reset();
                try {
                    throw std::runtime_error(uv_strerror(status));
                } catch (...) {
                    job.result.error = std::current_exception();
                }
                finish(job);
                continue;
            }
            active = true;
            stats.running = 1;
            return;
        }
    }

    void stop(Ticket ticket, Stop reason) noexcept {
        const auto found = jobs.find(ticket);
        if (found == jobs.end()) {
            return;
        }
        auto& job = *found->second;
        if (job.stop == Stop::cancelled || (job.stop == reason)) {
            return;
        }
        job.stop = reason;
        job.stopped.store(true, std::memory_order_release);
        if (reason == Stop::expired) {
            ++stats.expired;
        } else {
            ++stats.cancelled;
        }
        if (job.phase == Phase::pending || job.phase == Phase::complete) {
            finish(job);
        }
        // Running work observes only the atomic stop flag. Neither its result
        // nor its input storage is touched before its completion callback.
    }

    void close() noexcept {
        closing = true;
        for (auto it = jobs.begin(); it != jobs.end();) {
            const auto ticket = (it++)->first;
            stop(ticket, Stop::cancelled);
        }
    }
};

LayoutJobs::LayoutJobs(LayoutJobLimits limits) : impl_(std::make_unique<Impl>(limits)) {
}

LayoutJobs::~LayoutJobs() = default;

std::optional<LayoutJobs::Ticket> LayoutJobs::submit(std::string_view text,
                                                     const TextLayoutOptions& options,
                                                     TextSource previous,
                                                     GlyphSource glyphs) {
    auto& state = *impl_;
    if (state.closing) {
        throw std::logic_error("Layout queue is closing");
    }
    if (text.size() > state.limits.text_bytes) {
        throw std::length_error("Layout text exceeds its job limit");
    }
    std::size_t bytes = text.size();
    const auto add = [&](std::size_t amount) {
        if (amount > std::numeric_limits<std::size_t>::max() - bytes) {
            throw std::length_error("Layout job accounting overflow");
        }
        bytes += amount;
    };
    add(sizeof(Impl::Job));
    // Include the retained seed's flat text and Rope payload conservatively,
    // even when another cache/job also holds the same immutable snapshot.
    add(previous.statistics().bytes);
    add(previous.statistics().bytes);
    add(glyphs.statistics().bytes);
    add(options.locale.size());
    for (const auto& family : options.font_families) {
        add(sizeof(std::string));
        add(family.size());
    }
    if (options.color_spans.size() >
        std::numeric_limits<std::size_t>::max() / sizeof(TextColorSpan)) {
        throw std::length_error("Layout style accounting overflow");
    }
    add(options.color_spans.size() * sizeof(TextColorSpan));
    if (bytes > state.limits.bytes) {
        throw std::length_error("Layout job inputs exceed its byte limit");
    }
    if (state.jobs.size() >= state.limits.jobs || bytes > state.limits.bytes - state.stats.bytes) {
        return std::nullopt;
    }
    if (state.sequence == std::numeric_limits<Ticket>::max()) {
        throw std::length_error("Layout ticket capacity exceeded");
    }
    const auto ticket = state.sequence + 1;
    auto job = std::make_unique<Impl::Job>(state);
    job->ticket = ticket;
    job->text = text;
    job->previous = std::move(previous);
    job->glyphs = std::move(glyphs);
    job->options = options;
    job->bytes = bytes;
    job->deadline = Impl::Clock::now() + state.limits.timeout;
    state.jobs.emplace(ticket, std::move(job));
    state.sequence = ticket;
    state.stats.bytes += bytes;
    ++state.stats.submitted;
    state.start();
    return ticket;
}

void LayoutJobs::poll() {
    auto& state = *impl_;
    const auto now = Impl::Clock::now();
    for (auto it = state.jobs.begin(); it != state.jobs.end();) {
        const auto& job = *(it++)->second;
        if (job.stop == Impl::Stop::none && job.phase != Impl::Phase::complete &&
            now >= job.deadline) {
            state.stop(job.ticket, Impl::Stop::expired);
        }
    }
    uv_run(&state.loop, UV_RUN_NOWAIT);
    state.start();
}

std::optional<LayoutJobResult> LayoutJobs::take(Ticket ticket) {
    const auto found = impl_->jobs.find(ticket);
    if (found == impl_->jobs.end() || found->second->phase != Impl::Phase::complete) {
        return std::nullopt;
    }
    auto result = std::move(found->second->result);
    impl_->erase(ticket);
    return result;
}

void LayoutJobs::cancel(Ticket ticket) {
    impl_->stop(ticket, Impl::Stop::cancelled);
}

void LayoutJobs::close() {
    impl_->close();
}

bool LayoutJobs::idle() const noexcept {
    return impl_->jobs.empty();
}

LayoutJobStatistics LayoutJobs::statistics() const noexcept {
    auto result = impl_->stats;
    result.jobs = impl_->jobs.size();
    return result;
}
} // namespace astra::ui
