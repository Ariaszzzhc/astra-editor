#include <astra/ui/picker_index.hpp>
#include <astra/core/utf8.hpp>
#include <limits>
#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
std::string fold(std::string_view value) {
    std::string result(value);
    for (auto& byte : result)
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
    return result;
}
} // namespace

namespace {
PickerIndex::Snapshot rows(const std::vector<std::string>& labels,
                           const std::vector<std::string>& icon_paths,
                           const std::vector<std::string>& badges) {
    if (!icon_paths.empty() && icon_paths.size() != labels.size()) {
        throw std::invalid_argument("Picker icon paths must match its labels");
    }
    if (!badges.empty() && badges.size() != labels.size()) {
        throw std::invalid_argument("Picker badges must match its labels");
    }
    if (labels.size() > 4096)
        throw std::length_error("Picker exceeds its 4096 item limit");
    std::vector<PickerIndex::Row> rows;
    std::size_t bytes{};
    std::size_t path_bytes{};
    std::size_t badge_bytes{};
    rows.reserve(labels.size());
    for (const auto& label : labels) {
        if (label.empty() || label.size() > 4096 || label.find_first_of("\r\n") != label.npos ||
            label.find('\0') != label.npos || label.size() > 65536 - bytes)
            throw std::length_error("Picker label exceeds its bounds");
        utf8::validate(label);
        bytes += label.size();
        const auto path = icon_paths.empty() ? std::string{} : icon_paths.at(rows.size());
        if (path.size() > 32768 || path.size() > 65536 - path_bytes ||
            path.find('\0') != path.npos) {
            throw std::length_error("Picker icon paths exceed their bounds");
        }
        utf8::validate(path);
        path_bytes += path.size();
        const auto badge = badges.empty() ? std::string{} : badges.at(rows.size());
        if (badge.size() > 128 || badge.size() > 65536 - badge_bytes ||
            badge.find_first_of("\r\n") != badge.npos || badge.find('\0') != badge.npos) {
            throw std::length_error("Picker badges exceed their bounds");
        }
        utf8::validate(badge);
        badge_bytes += badge.size();
        rows.push_back({label, fold(label), rows.size(), path, badge});
    }
    return PickerIndex::Snapshot(std::move(rows));
}
} // namespace

void PickerIndex::reset(const std::vector<std::string>& labels,
                        bool dynamic,
                        const std::vector<std::string>& icon_paths,
                        const std::vector<std::string>& badges) {
    auto replacement = rows(labels, icon_paths, badges);
    std::string message = dynamic ? "Searching…" : "";
    source_ = replacement;
    visible_ = std::move(replacement);
    query_.clear();
    message_ = std::move(message);
    dynamic_ = dynamic;
    generation_ = 1;
    published_ = dynamic ? 0 : 1;
    error_ = false;
}

void PickerIndex::filter(std::string_view query) {
    if (query.size() > 16384 || query.find_first_of("\r\n") != query.npos ||
        query.find('\0') != query.npos) {
        throw std::length_error("Picker query exceeds its bounds");
    }
    utf8::validate(query);
    if (dynamic_) {
        if (query == query_)
            return;
        if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::length_error("Picker query identity exhausted");
        }
        std::string replacement(query), message("Searching…");
        query_ = std::move(replacement);
        message_ = std::move(message);
        ++generation_;
        error_ = false;
        return;
    }
    auto folded = fold(query);
    if (folded == query_)
        return;
    Snapshot replacement;
    if (folded.empty())
        replacement = source_;
    else {
        std::vector<Row> rows;
        source_.for_each_range(0, source_.size(), [&](const Row& row) {
            if (row.folded.find(folded) != row.folded.npos)
                rows.push_back(row);
        });
        replacement = Snapshot(std::move(rows));
    }
    visible_ = std::move(replacement);
    query_ = std::move(folded);
}

bool PickerIndex::publish(std::string_view generation,
                          const std::vector<std::string>& labels,
                          std::string message,
                          bool error,
                          const std::vector<std::string>& icon_paths,
                          const std::vector<std::string>& badges) {
    if (!dynamic_ || generation != this->generation())
        return false;
    if (message.size() > 4096 || message.find_first_of("\r\n") != message.npos ||
        message.find('\0') != message.npos) {
        throw std::length_error("Picker message exceeds its bounds");
    }
    utf8::validate(message);
    auto replacement = rows(labels, icon_paths, badges);
    source_ = replacement;
    visible_ = std::move(replacement);
    message_ = std::move(message);
    published_ = generation_;
    error_ = error;
    return true;
}
} // namespace astra::ui
