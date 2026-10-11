#include <astra/ui/view_store.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
using Json = astra::protocol::Json;

double microseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}

template <typename Append> void fill(std::size_t count, Append append) {
    for (std::size_t first = 0; first < count; first += 128) {
        Json page = Json::array();
        for (auto index = first; index < std::min(first + 128, count); ++index)
            page.push_back({{"id", std::to_string(index)},
                            {"label", "Source row 中🙂"},
                            {"data", {{"index", index}}}});
        append(page);
    }
}

double percentile(std::vector<double> values, std::size_t percent) {
    std::ranges::sort(values);
    return values[std::min(values.size() - 1, (values.size() * percent + 99) / 100 - 1)];
}

void drain(astra::ui::ViewStore& store) {
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    while (!store.quiescent()) {
        if (Clock::now() >= deadline)
            throw std::runtime_error("View retirement did not drain");
        store.poll();
        std::this_thread::yield();
    }
}

void benchmark(std::size_t count) {
    std::vector<double> immediate, cancellation, publication;
    const Json spec{{"id", "benchmark"}, {"title", "Benchmark"}};
    for (int sample = 0; sample < 7; ++sample) {
        astra::ui::ViewBuilder builder;
        fill(count, [&](const Json& page) {
            builder.append(page);
        });
        auto source = std::move(builder).finish();
        auto started = Clock::now();
        source.reset();
        immediate.push_back(microseconds(started));

        astra::ui::ViewStore store;
        const auto first = store.begin(spec);
        fill(count, [&](const Json& page) {
            if (!store.append("benchmark", first, page))
                throw std::runtime_error("Benchmark builder was superseded");
        });
        started = Clock::now();
        if (!store.abort("benchmark", first))
            throw std::runtime_error("Benchmark cancellation failed");
        cancellation.push_back(microseconds(started));
        drain(store);

        const auto published = store.begin(spec);
        fill(count, [&](const Json& page) {
            if (!store.append("benchmark", published, page))
                throw std::runtime_error("Benchmark publication was superseded");
        });
        auto initial = store.commit("benchmark", published, "", "Empty");
        drain(store);
        if (!initial.get().get<bool>())
            throw std::runtime_error("Benchmark publication failed");
        const auto replacement = store.begin(spec);
        if (!store.append(
                "benchmark", replacement, Json::array({{{"id", "new"}, {"label", "New"}}})))
            throw std::runtime_error("Benchmark replacement was superseded");
        auto completion = store.commit("benchmark", replacement, "", "Empty");
        started = Clock::now();
        store.poll();
        publication.push_back(microseconds(started));
        if (!completion.get().get<bool>())
            throw std::runtime_error("Benchmark replacement failed");
        drain(store);
        store.shutdown();
        drain(store);
        if (store.retirement_stats().bytes || store.retirement_stats().reserved)
            throw std::runtime_error("Benchmark retirement retained ownership");
    }
    std::cout << "rows=" << count << " samples=7 inline_free_p50_us=" << percentile(immediate, 50)
              << " inline_free_p95_us=" << percentile(immediate, 95)
              << " abort_owner_p50_us=" << percentile(cancellation, 50)
              << " abort_owner_p95_us=" << percentile(cancellation, 95)
              << " publish_owner_p95_us=" << percentile(publication, 95) << '\n';
}
} // namespace

int main() {
    try {
        for (const auto count : {std::size_t{1000}, std::size_t{10000}, std::size_t{100000}})
            benchmark(count);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
