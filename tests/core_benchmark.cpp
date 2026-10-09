#include <astra/core/document.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
double microseconds(Clock::time_point start) {
    return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
}
double percentile(std::vector<double> values, std::size_t percent) {
    std::ranges::sort(values);
    return values[(values.size() - 1) * percent / 100];
}
void require(bool condition, const char* message) {
    if (!condition) { throw std::runtime_error(message); }
}
void benchmark(std::size_t mib) {
    constexpr std::size_t rounds = 1000;
    std::string input;
    const std::string line = "int value = 42; // Unicode: \xe4\xb8\xad\xf0\x9f\x98\x80 e\xcc\x81\n";
    input.reserve(mib * 1024 * 1024 + line.size());
    while (input.size() < mib * 1024 * 1024) { input += line; }
    auto start = Clock::now();
    astra::Document document({1}, input, rounds);
    const auto load_ms = microseconds(start) / 1000;
    const auto original = document.snapshot();
    std::mt19937_64 random(0x4153545241504552ULL);
    std::vector<double> edits, seeks, differences;
    edits.reserve(rounds); seeks.reserve(rounds); differences.reserve(rounds);
    for (std::size_t i = 0; i < rounds; ++i) {
        const auto before = document.snapshot();
        auto offset = static_cast<std::size_t>(random() % document.text().size_bytes());
        while (!document.text().is_boundary(offset)) { --offset; }
        start = Clock::now();
        const auto changed = document.apply({document.revision(), {{{offset}, {offset}, "\xf0\x9f\x98\x80"}}, std::nullopt});
        edits.push_back(microseconds(start));
        require(changed, "An insertion did not change the document");
        start = Clock::now();
        const auto difference = before.text.changed_extent(document.text());
        differences.push_back(microseconds(start));
        // Equal neighboring text can move the minimal change past the requested
        // insertion point; validate the resulting extent, not its provenance.
        require(difference.begin <= difference.old_end && difference.old_end <= before.text.size_bytes() &&
                difference.new_end <= document.text().size_bytes() && difference.new_end == difference.old_end + 4,
                "Invalid incremental difference");
        start = Clock::now();
        const auto position = document.text().byte_to_position(offset);
        const auto round_trip = document.text().position_to_byte(position);
        seeks.push_back(microseconds(start));
        require(round_trip == offset, "UTF-16 position round trip changed the byte offset");
    }
    require(original.text.str() == input, "Retained snapshot was mutated");
    require(document.text().validate(), "Persistent tree invariants failed");
    start = Clock::now();
    for (std::size_t i = 0; i < rounds; ++i) { require(document.undo(), "Missing undo entry"); }
    const auto undo_us = microseconds(start) / rounds;
    require(document.text().str() == input && !document.dirty(), "Undo did not restore the original saved state");
    std::cout << mib << ',' << load_ms << ',' << percentile(edits, 50) << ',' << percentile(edits, 95)
              << ',' << percentile(edits, 99) << ',' << percentile(differences, 95) << ','
              << percentile(seeks, 95) << ',' << undo_us << ',' << original.text.tree_height() << '\n';
}
}

int main() {
    try {
        std::cout << "MiB,load_ms,edit_p50_us,edit_p95_us,edit_p99_us,diff_p95_us,UTF16_seek_p95_us,undo_mean_us,tree_height\n";
        for (const auto size : {std::size_t{1}, std::size_t{8}, std::size_t{64}}) { benchmark(size); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Core benchmark failed: " << error.what() << '\n'; return 1;
    }
}
