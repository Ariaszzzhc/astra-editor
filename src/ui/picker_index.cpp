#include <astra/ui/picker_index.hpp>
#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
std::string fold(std::string_view value) {
    std::string result(value);
    for (auto& byte : result) if (byte >= 'A' && byte <= 'Z') byte = static_cast<char>(byte - 'A' + 'a');
    return result;
}
}
void PickerIndex::reset(const std::vector<std::string>& labels) {
    if (labels.size() > 4096) throw std::length_error("Picker exceeds its 4096 item limit");
    std::vector<Row> rows;
    std::size_t bytes{};
    rows.reserve(labels.size());
    for (const auto& label : labels) {
        if (label.empty() || label.size() > 4096 || label.find_first_of("\r\n") != label.npos ||
            label.size() > 65536 - bytes) throw std::length_error("Picker label exceeds its bounds");
        bytes += label.size();
        rows.push_back({label, fold(label), rows.size()});
    }
    Snapshot replacement(std::move(rows));
    source_ = replacement; visible_ = std::move(replacement); query_.clear();
}
void PickerIndex::filter(std::string_view query) {
    auto folded = fold(query);
    if (folded == query_) return;
    Snapshot replacement;
    if (folded.empty()) replacement = source_;
    else {
        std::vector<Row> rows;
        source_.for_each_range(0, source_.size(), [&](const Row& row) {
            if (row.folded.find(folded) != row.folded.npos) rows.push_back(row);
        });
        replacement = Snapshot(std::move(rows));
    }
    visible_ = std::move(replacement); query_ = std::move(folded);
}
} // namespace astra::ui
