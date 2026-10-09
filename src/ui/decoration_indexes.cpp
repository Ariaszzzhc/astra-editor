#include <astra/ui/decoration_indexes.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace astra::ui {
namespace {
std::size_t coordinate(const protocol::Json& value) {
    if (!value.is_number_integer() || (!value.is_number_unsigned() && value.get<std::int64_t>() < 0)) {
        throw std::invalid_argument("Decoration coordinates must be nonnegative integers");
    }
    return value.get<std::size_t>();
}
std::size_t end_after(std::size_t begin, std::size_t count) {
    if (count > std::numeric_limits<std::size_t>::max() - begin) { throw std::length_error("Decoration extent overflow"); }
    return begin + count;
}
void check_array(const protocol::Json& values) {
    if (!values.is_array() || values.size() > 100000) { throw std::length_error("Decoration list must contain at most 100000 entries"); }
}
} // namespace

void DecorationIndexes::set_highlights(const protocol::Json& values) {
    check_array(values);
    std::vector<decltype(highlights)::Entry> entries;
    entries.reserve(values.size());
    for (const auto& value : values) { entries.push_back({coordinate(value.at("begin")), coordinate(value.at("end")), value.at("kind").get<std::string>()}); }
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
        if (first > last || (first == last && start > end)) { throw std::invalid_argument("Reversed diagnostic range"); }
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
        entries.push_back({begin, end_after(begin, std::max(std::size_t{1}, coordinate(value.at("count")))), value.at("kind").get<std::string>()});
    }
    gutter.reset(std::move(entries));
}
} // namespace astra::ui
