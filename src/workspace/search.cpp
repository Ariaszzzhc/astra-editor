#include <astra/workspace/workspace.hpp>
#include <astra/core/utf8.hpp>

#include <re2/re2.h>
#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>

namespace astra::workspace {
namespace {

bool word_character(char32_t scalar) {
    const auto category = utf8proc_category(static_cast<utf8proc_int32_t>(scalar));
    return (category >= UTF8PROC_CATEGORY_LU && category <= UTF8PROC_CATEGORY_PC) ||
        scalar == 0x200c || scalar == 0x200d;
}

bool word_boundary(std::string_view text, std::size_t begin, std::size_t end) {
    if (begin != 0) {
        auto previous = begin - 1;
        while (!utf8::is_boundary(text, previous)) { --previous; }
        if (word_character(utf8::decode(text, previous).value)) { return false; }
    }
    return end == text.size() || !word_character(utf8::decode(text, end).value);
}

void require_unicode_regex(std::string_view query) {
    bool quoted = false;
    for (std::size_t index = 0; index < query.size(); ++index) {
        if (query[index] != '\\' || index + 1 == query.size()) { continue; }
        const auto escaped = query[++index];
        if (quoted) { if (escaped == 'E') { quoted = false; } }
        else if (escaped == 'Q') { quoted = true; }
        else if (escaped == 'C') { throw std::invalid_argument("Regex byte matching \\C is not supported for Unicode text"); }
    }
}

RE2::Options compile_options(const SearchOptions& options) {
    if (options.query.empty() || options.query.size() > SearchPattern::max_query_bytes ||
        options.query.find('\0') != std::string::npos) {
        throw std::invalid_argument("Search query must contain 1..65536 UTF-8 bytes without NUL");
    }
    utf8::validate(options.query);
    if (options.max_file_bytes > SearchPattern::max_input_bytes || options.max_total_bytes > 512U * 1024U * 1024U ||
        options.max_files > 100000 || options.max_matches > 100000 || options.max_result_bytes > 64U * 1024U * 1024U ||
        options.excluded_paths.size() > 100000) {
        throw std::invalid_argument("Search limits exceed supported bounds");
    }
    if (options.regex) { require_unicode_regex(options.query); }
    RE2::Options result;
    result.set_encoding(RE2::Options::EncodingUTF8);
    result.set_literal(!options.regex);
    result.set_case_sensitive(options.case_sensitive);
    result.set_log_errors(false);
    result.set_max_mem(8U * 1024U * 1024U);
    return result;
}

absl::string_view re2_text(std::string_view text) {
    // A nonnull empty view distinguishes an empty capture from no capture.
    return {text.empty() ? "" : text.data(), text.size()};
}

struct ReplacementToken {
    std::string_view literal;
    std::optional<std::size_t> group;
};

void append_bounded(std::string& result, std::string_view value, std::size_t max_bytes) {
    if (value.size() > max_bytes - result.size()) { throw std::length_error("Replacement expansion exceeds output limit"); }
    result.append(value);
}

} // namespace

class SearchPattern::Impl {
public:
    explicit Impl(const SearchOptions& options)
        : options_(options), expression_(re2_text(options.query), compile_options(options)) {
        if (!expression_.ok()) { throw std::invalid_argument("Invalid RE2 search expression: " + expression_.error()); }
        if (expression_.NumberOfCapturingGroups() > static_cast<int>(max_capture_groups) ||
            (options.regex && expression_.ProgramSize() > 10000)) {
            throw std::invalid_argument("Search expression exceeds capture or compiled-program limit");
        }
        capture_count_ = static_cast<std::size_t>(expression_.NumberOfCapturingGroups()) + 1;
    }

    void require_input_size(std::string_view text) const {
        if (text.size() > options_.max_file_bytes || text.size() > max_input_bytes) {
            throw std::length_error("Search text exceeds input limit");
        }
    }

    std::optional<TextMatch> match(std::string_view text, std::size_t position, RE2::Anchor anchor) const {
        const auto input = re2_text(text);
        std::array<absl::string_view, max_capture_groups + 1> captured;
        if (!expression_.Match(input, position, input.size(), anchor, captured.data(), static_cast<int>(capture_count_))) {
            return std::nullopt;
        }
        TextMatch result;
        result.captures.reserve(capture_count_);
        for (std::size_t index = 0; index < capture_count_; ++index) {
            if (captured[index].data() == nullptr) { result.captures.push_back(std::nullopt); continue; }
            const auto begin = static_cast<std::size_t>(captured[index].data() - input.data());
            const auto end = begin + captured[index].size();
            if (!utf8::is_boundary(text, begin) || !utf8::is_boundary(text, end)) {
                throw std::invalid_argument("Search match splits a Unicode scalar");
            }
            result.captures.push_back(TextRange{ByteOffset{begin}, ByteOffset{end}});
        }
        if (!result.captures.front()) { throw std::runtime_error("RE2 returned no whole-match range"); }
        result.begin = result.captures.front()->begin;
        result.end = result.captures.front()->end;
        return result;
    }

    std::vector<ReplacementToken> replacement_tokens(std::string_view replacement) const {
        if (replacement.size() > max_replacement_bytes || replacement.find('\0') != std::string_view::npos) {
            throw std::invalid_argument("Replacement must contain at most 65536 UTF-8 bytes without NUL");
        }
        utf8::validate(replacement);
        if (!options_.regex) { return {{replacement, std::nullopt}}; }
        std::vector<ReplacementToken> result;
        for (std::size_t begin = 0; begin < replacement.size();) {
            const auto marker = replacement.find('$', begin);
            if (marker == std::string_view::npos) { result.push_back({replacement.substr(begin), std::nullopt}); break; }
            if (marker > begin) { result.push_back({replacement.substr(begin, marker - begin), std::nullopt}); }
            auto end = marker + 1;
            if (end == replacement.size()) { throw std::invalid_argument("Replacement ends with an unescaped dollar sign"); }
            if (replacement[end] == '$') {
                result.push_back({replacement.substr(end, 1), std::nullopt});
                begin = end + 1;
                continue;
            }
            std::size_t group{};
            if (replacement[end] == '{') {
                const auto close = replacement.find('}', end + 1);
                if (close == std::string_view::npos) { throw std::invalid_argument("Replacement has an unterminated group name"); }
                const auto name = std::string(replacement.substr(end + 1, close - end - 1));
                const auto named = expression_.NamedCapturingGroups().find(name);
                if (named == expression_.NamedCapturingGroups().end()) { throw std::invalid_argument("Replacement references an unknown named group"); }
                group = static_cast<std::size_t>(named->second);
                end = close + 1;
            } else {
                const auto first = end;
                while (end < replacement.size() && replacement[end] >= '0' && replacement[end] <= '9') {
                    if (end - first >= 2) { throw std::invalid_argument("Replacement group references must be $0..$99"); }
                    group = group * 10 + static_cast<std::size_t>(replacement[end++] - '0');
                }
                if (end == first) { throw std::invalid_argument("Replacement requires $$, $0..$99 or ${name}"); }
            }
            if (group >= capture_count_) { throw std::invalid_argument("Replacement references an unknown capture group"); }
            result.push_back({{}, group});
            begin = end;
        }
        return result;
    }

    SearchOptions options_;
    RE2 expression_;
    std::size_t capture_count_{};
};

SearchPattern::SearchPattern(const SearchOptions& options) : impl_(std::make_unique<Impl>(options)) {}
SearchPattern::~SearchPattern() = default;
SearchPattern::SearchPattern(SearchPattern&&) noexcept = default;
SearchPattern& SearchPattern::operator=(SearchPattern&&) noexcept = default;

TextSearchResult SearchPattern::find_all(std::string_view text, std::stop_token stop, std::size_t max_matches) const {
    impl_->require_input_size(text);
    TextSearchResult result;
    for (std::size_t index = 0; index < text.size();) {
        if (stop.stop_requested()) { result.cancelled = true; return result; }
        index += utf8::decode(text, index).bytes;
    }
    const auto limit = std::min(max_matches, impl_->options_.max_matches);
    const auto match_bytes = sizeof(TextMatch) + impl_->capture_count_ * sizeof(std::optional<TextRange>);
    const auto byte_limit = impl_->options_.max_result_bytes / match_bytes;
    std::size_t scan_work{};
    constexpr auto max_scan_work = std::size_t{256} * 1024U * 1024U;
    for (std::size_t offset = 0; offset <= text.size();) {
        if (stop.stop_requested()) { result.cancelled = true; break; }
        auto found = impl_->match(text, offset, RE2::UNANCHORED);
        if (stop.stop_requested()) { result.cancelled = true; break; }
        if (!found) { break; }
        const auto begin = found->begin.value;
        const auto end = found->end.value;
        const auto work = std::max(std::size_t{1}, end - offset);
        if (work > max_scan_work - scan_work) { result.truncated = true; break; }
        scan_work += work;
        const bool accepted = !impl_->options_.whole_word || word_boundary(text, begin, end);
        if (accepted) {
            if (result.matches.size() >= limit || result.matches.size() >= byte_limit) { result.truncated = true; break; }
            result.matches.push_back(std::move(*found));
        }
        if (accepted && end > begin) { offset = end; }
        else {
            if (begin == text.size()) { break; }
            offset = begin + utf8::decode(text, begin).bytes;
        }
    }
    return result;
}

void SearchPattern::validate_replacement(std::string_view replacement) const {
    (void)impl_->replacement_tokens(replacement);
}

std::string SearchPattern::expand_replacement(std::string_view text, const TextMatch& match,
    std::string_view replacement, std::size_t max_bytes) const {
    impl_->require_input_size(text);
    if (max_bytes > max_output_bytes) { throw std::invalid_argument("Replacement output limit exceeds supported bound"); }
    const auto tokens = impl_->replacement_tokens(replacement);
    if (match.begin > match.end || match.end.value > text.size() ||
        !utf8::is_boundary(text, match.begin.value) || !utf8::is_boundary(text, match.end.value)) {
        throw std::invalid_argument("Invalid replacement match range");
    }
    const auto actual = impl_->match(text, match.begin.value, RE2::ANCHOR_START);
    if (!actual || actual->begin != match.begin || actual->end != match.end || actual->captures != match.captures ||
        (impl_->options_.whole_word && !word_boundary(text, match.begin.value, match.end.value))) {
        throw std::invalid_argument("Replacement match or capture ranges no longer match the search pattern");
    }
    std::string result;
    for (const auto& token : tokens) {
        if (!token.group) { append_bounded(result, token.literal, max_bytes); }
        else if (const auto& capture = actual->captures[*token.group]) {
            append_bounded(result, text.substr(capture->begin.value, capture->end.value - capture->begin.value), max_bytes);
        }
    }
    return result;
}

} // namespace astra::workspace
