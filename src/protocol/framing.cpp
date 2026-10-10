#include <astra/protocol/framing.hpp>

#include <algorithm>
#include <charconv>
#include <system_error>

namespace astra::protocol {
namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

bool equal_ascii_insensitive(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto lower = [](char c) {
            return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
        };
        if (lower(left[i]) != lower(right[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

FrameDecoder::FrameDecoder(std::size_t body_limit, std::size_t header_limit)
    : body_limit_(body_limit), header_limit_(header_limit) {
    if (body_limit == 0 || header_limit < 4) {
        throw std::invalid_argument("invalid protocol frame limits");
    }
}

[[noreturn]] void FrameDecoder::fail(const char* message) {
    failed_ = true;
    header_.clear();
    body_.clear();
    expected_.reset();
    throw FramingError(message);
}

std::size_t FrameDecoder::parse_header() {
    std::optional<std::size_t> length;
    std::string_view remaining(header_);
    while (remaining != "\r\n") {
        const auto end = remaining.find("\r\n");
        if (end == std::string_view::npos || end == 0) {
            fail("invalid frame header line");
        }
        const auto line = remaining.substr(0, end);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            fail("invalid frame header field");
        }
        const auto name = line.substr(0, colon);
        if (!std::ranges::all_of(name, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       c == '-';
            })) {
            fail("invalid frame header field name");
        }
        const auto value = trim(line.substr(colon + 1));
        if (equal_ascii_insensitive(name, "Content-Length")) {
            if (length) {
                fail("duplicate Content-Length header");
            }
            std::size_t parsed{};
            const auto [last, error] =
                std::from_chars(value.data(), value.data() + value.size(), parsed);
            if (value.empty() || error != std::errc{} || last != value.data() + value.size()) {
                fail("invalid Content-Length value");
            }
            if (parsed > body_limit_) {
                fail("frame body exceeds configured limit");
            }
            length = parsed;
        }
        remaining.remove_prefix(end + 2);
    }
    if (!length) {
        fail("missing Content-Length header");
    }
    return *length;
}

std::vector<std::string> FrameDecoder::feed(std::string_view bytes) {
    if (failed_) {
        throw FramingError("frame decoder is in a failed state");
    }
    std::vector<std::string> messages;
    while (!bytes.empty()) {
        if (!expected_) {
            const auto next = static_cast<unsigned char>(bytes.front());
            if ((next < 32 && next != '\r' && next != '\n' && next != '\t') || next > 126) {
                fail("frame headers must be ASCII");
            }
            if (header_.size() == header_limit_) {
                fail("frame header exceeds configured limit");
            }
            header_.push_back(bytes.front());
            bytes.remove_prefix(1);
            if (!header_.ends_with("\r\n\r\n")) {
                continue;
            }
            expected_ = parse_header();
            header_.clear();
            if (*expected_ == 0) {
                messages.emplace_back();
                expected_.reset();
                continue;
            }
        }
        const auto count = std::min(bytes.size(), *expected_ - body_.size());
        body_.append(bytes.substr(0, count));
        bytes.remove_prefix(count);
        if (body_.size() == *expected_) {
            messages.push_back(std::move(body_));
            body_.clear();
            expected_.reset();
        }
    }
    return messages;
}

void FrameDecoder::finish() {
    if (failed_) {
        throw FramingError("frame decoder is in a failed state");
    }
    if (!idle()) {
        fail("stream ended in a partial frame");
    }
}

bool FrameDecoder::idle() const noexcept {
    return !expected_ && header_.empty() && body_.empty();
}

std::size_t FrameDecoder::buffered_bytes() const noexcept {
    return header_.size() + body_.size();
}

std::string encode_frame(std::string_view body, std::size_t body_limit) {
    if (body.size() > body_limit) {
        throw FramingError("frame body exceeds configured limit");
    }
    auto result = std::string("Content-Length: ") + std::to_string(body.size()) + "\r\n\r\n";
    result.append(body);
    return result;
}

} // namespace astra::protocol
