#include <astra/ui/text_panel.hpp>
#include <astra/core/utf8.hpp>

#include <set>
#include <stdexcept>
#include <cstdint>
#include <limits>
#include <utility>

namespace astra::ui {
void TextPanelPresentation::reset(const protocol::Json& spec, std::size_t rows) {
    static_cast<void>(spec.value("monospace", false));
    const auto actions = spec.value("actions", protocol::Json::array());
    const auto tones = spec.value("lineTones", protocol::Json::array());
    if (!actions.is_array() || actions.size() > 8 || !tones.is_array() || tones.size() > 100000)
        throw std::invalid_argument("Text panel presentation exceeds its bounds");
    std::vector<TextPanelAction> next_actions;
    std::set<std::string> ids, keys;
    for (const auto& action : actions) {
        TextPanelAction next{action.at("id").get<std::string>(),
                             action.at("label").get<std::string>(),
                             action.value("key", std::string{}),
                             action.value("destructive", false)};
        if (next.id.empty() || next.id.size() > 128 || next.label.empty() ||
            next.label.size() > 256 || next.id.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
            next.label.find_first_of("\r\n\0", 0, 3) != std::string::npos ||
            !ids.insert(next.id).second)
            throw std::invalid_argument("Invalid text panel action");
        utf8::validate(next.id);
        utf8::validate(next.label);
        for (auto& key : next.key)
            if (key >= 'A' && key <= 'Z')
                key = static_cast<char>(key + ('a' - 'A'));
        if (!next.key.empty() && (next.key.size() != 1 ||
                                  !((next.key[0] >= 'a' && next.key[0] <= 'z') ||
                                    (next.key[0] >= '0' && next.key[0] <= '9')) ||
                                  !keys.insert(next.key).second))
            throw std::invalid_argument(
                "Text panel shortcuts must be unique ASCII letters or digits");
        next_actions.push_back(std::move(next));
    }
    using Entry = RangeIndex<TextPanelTone, DocumentLineUnit>::Entry;
    std::vector<Entry> next_tones;
    std::size_t previous_end{};
    for (const auto& entry : tones) {
        const auto& begin = entry.at("begin");
        const auto& end = entry.at("end");
        const auto valid_index = [](const protocol::Json& index) {
            return index.is_number_unsigned() ||
                   (index.is_number_integer() && index.get<std::int64_t>() >= 0);
        };
        if (!valid_index(begin) || !valid_index(end))
            throw std::invalid_argument("Text panel tones require nonnegative row indexes");
        const auto first = begin.get<std::size_t>(), last = end.get<std::size_t>();
        if (first < previous_end || first >= last || last > rows)
            throw std::invalid_argument("Text panel tones must be ordered disjoint row ranges");
        const auto name = entry.at("tone").get<std::string>();
        TextPanelTone tone;
        if (name == "normal")
            tone = TextPanelTone::normal;
        else if (name == "added")
            tone = TextPanelTone::added;
        else if (name == "removed")
            tone = TextPanelTone::removed;
        else if (name == "accent")
            tone = TextPanelTone::accent;
        else if (name == "muted")
            tone = TextPanelTone::muted;
        else
            throw std::invalid_argument("Unknown text panel tone");
        next_tones.push_back({first, last, tone});
        previous_end = last;
    }
    RangeIndex<TextPanelTone, DocumentLineUnit> next_index;
    next_index.reset(std::move(next_tones));
    actions_ = std::move(next_actions);
    tones_ = std::move(next_index);
}

const std::vector<TextPanelAction>& TextPanelPresentation::actions() const noexcept {
    return actions_;
}

TextPanelTone TextPanelPresentation::tone(std::size_t row) const {
    if (row == std::numeric_limits<std::size_t>::max())
        return TextPanelTone::normal;
    auto result = TextPanelTone::normal;
    tones_.intersect(row, row + 1, [&](const auto& entry) {
        result = entry.value;
    });
    return result;
}

const TextPanelAction* TextPanelPresentation::shortcut(char key) const noexcept {
    if (key >= 'A' && key <= 'Z')
        key = static_cast<char>(key + ('a' - 'A'));
    for (const auto& action : actions_)
        if (action.key.size() == 1 && action.key[0] == key)
            return &action;
    return nullptr;
}
} // namespace astra::ui
