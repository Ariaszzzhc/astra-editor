#include <astra/ui/decoration_indexes.hpp>
#include <astra/core/utf8.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <map>
#include <vector>

namespace astra::ui {
namespace {
std::size_t coordinate(const protocol::Json& value) {
    if (!value.is_number_integer() ||
        (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        throw std::invalid_argument("Decoration coordinates must be nonnegative integers");
    }
    return value.get<std::size_t>();
}

std::size_t end_after(std::size_t begin, std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() - begin) {
        throw std::length_error("Decoration extent overflow");
    }
    return begin + count;
}

void check_array(const protocol::Json& values) {
    if (!values.is_array() || values.size() > 100000) {
        throw std::length_error("Decoration list must contain at most 100000 entries");
    }
}
} // namespace

void DecorationIndexes::set_highlights(const protocol::Json& values) {
    check_array(values);
    std::vector<decltype(highlights)::Entry> entries;
    entries.reserve(values.size());
    for (const auto& value : values) {
        entries.push_back({coordinate(value.at("begin")),
                           coordinate(value.at("end")),
                           value.at("kind").get<std::string>()});
    }
    highlights.reset(std::move(entries));
}

void DecorationIndexes::remap_highlights(const ViewportIndex& before,
                                         const std::vector<ViewportLine>& after) {
    std::map<std::string_view, const ViewportLine*, std::less<>> matching;
    before.for_each([&](const auto& line) {
        matching.try_emplace(line.text, &line);
    });
    std::vector<decltype(highlights)::Entry> entries;
    for (const auto& line : after) {
        const auto* old = before.find(line.number);
        if (!old || old->text != line.text) {
            if (const auto found = matching.find(line.text); found != matching.end()) {
                old = found->second;
            }
        }
        if (!old) {
            continue;
        }
        std::size_t prefix{};
        const auto common = std::min(old->text.size(), line.text.size());
        while (prefix < common && old->text[prefix] == line.text[prefix]) {
            ++prefix;
        }
        while (!utf8::is_boundary(old->text, prefix) || !utf8::is_boundary(line.text, prefix)) {
            --prefix;
        }
        std::size_t suffix{};
        while (suffix < common - prefix && old->text[old->text.size() - suffix - 1] ==
                                               line.text[line.text.size() - suffix - 1]) {
            ++suffix;
        }
        while (!utf8::is_boundary(old->text, old->text.size() - suffix) ||
               !utf8::is_boundary(line.text, line.text.size() - suffix)) {
            --suffix;
        }
        const auto old_end = old->text.size() - suffix;
        const auto new_end = line.text.size() - suffix;
        highlights.intersect(old->begin, old->end, [&](const auto& span) {
            const auto start = std::max(span.begin, old->begin) - old->begin;
            const auto end = std::min(span.end, old->end) - old->begin;
            if (start >= prefix && end <= old_end && old_end != prefix) {
                return;
            }
            const auto mapped_start =
                start >= old_end ? new_end + start - old_end : std::min(start, prefix);
            const auto mapped_end =
                end <= prefix ? end : new_end + (end > old_end ? end - old_end : 0);
            if (mapped_start < mapped_end && entries.size() < 100000) {
                entries.push_back({line.begin + mapped_start, line.begin + mapped_end, span.value});
            }
        });
    }
    highlights.reset(std::move(entries));
}

void DecorationIndexes::set_diagnostics(const protocol::Json& values) {
    check_array(values);
    std::vector<decltype(diagnostics)::Entry> entries;
    entries.reserve(values.size());
    for (const auto& value : values) {
        const auto& range = value.at("range");
        const auto first = coordinate(range.at("start").at("line"));
        const auto last = coordinate(range.at("end").at("line"));
        const auto start = coordinate(range.at("start").at("character"));
        const auto end = coordinate(range.at("end").at("character"));
        if (first > last || (first == last && start > end)) {
            throw std::invalid_argument("Reversed diagnostic range");
        }
        entries.push_back({first, end_after(last, 1), {start, end, value.value("severity", 1)}});
    }
    diagnostics.reset(std::move(entries));
}

void DecorationIndexes::set_gutter(const protocol::Json& values) {
    check_array(values);
    std::vector<decltype(gutter)::Entry> entries;
    entries.reserve(values.size());
    for (const auto& value : values) {
        const auto begin = coordinate(value.at("line"));
        entries.push_back(
            {begin,
             end_after(begin, std::max(std::size_t{1}, coordinate(value.at("count")))),
             value.at("kind").get<std::string>()});
    }
    gutter.reset(std::move(entries));
}
} // namespace astra::ui
