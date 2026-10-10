#include <astra/ui/tab_index.hpp>
#include <astra/core/utf8.hpp>

#include <cmath>
#include <stdexcept>

namespace astra::ui {
namespace {
void validate_label(std::string_view label) {
    if (label.size() > 4096) {
        throw std::length_error("Tab label exceeds its byte bound");
    }
    if (!utf8::is_valid(label)) {
        throw std::invalid_argument("Tab label requires valid UTF-8");
    }
}
} // namespace

std::optional<std::size_t> TabIndex::position(std::string_view id) const {
    const auto found = orders_.find(id);
    if (found == orders_.end()) {
        return std::nullopt;
    }
    const auto result = entries_.seek_prefix([&](const Summary& summary) {
        return summary.last_order >= found->second;
    });
    if (!result.item || result.item->id != id) {
        throw std::logic_error("Tab identity index disagrees with its ordered tree");
    }
    return result.index;
}

std::uint64_t TabIndex::order_at(const Snapshot& tree, std::size_t insertion) {
    const auto before = insertion == 0 ? 0 : tree.at(insertion - 1).order;
    if (insertion == tree.size() &&
        before <= std::numeric_limits<std::uint64_t>::max() - order_step) {
        return before + order_step;
    }
    const auto after = insertion == tree.size() ? std::numeric_limits<std::uint64_t>::max()
                                                : tree.at(insertion).order;
    return after - before > 1 ? before + (after - before) / 2 : 0;
}

TabIndex::Snapshot TabIndex::relabelled(const Snapshot& tree) {
    std::vector<Entry> entries;
    entries.reserve(tree.size());
    std::uint64_t order{};
    tree.for_each([&](const Entry& entry) {
        auto value = entry;
        value.order = order += order_step;
        entries.push_back(std::move(value));
    });
    return Snapshot(std::move(entries));
}

std::map<std::string, std::uint64_t, std::less<>> TabIndex::identities(const Snapshot& tree) {
    std::map<std::string, std::uint64_t, std::less<>> result;
    tree.for_each([&](const Entry& entry) {
        result.emplace(entry.id, entry.order);
    });
    return result;
}

void TabIndex::append(std::string id, std::string label) {
    if (id.empty() || id.size() > 128 || orders_.contains(id)) {
        throw std::invalid_argument("Tab identity must be bounded, nonempty and unique");
    }
    if (size() == max_tabs) {
        throw std::length_error("Tab count exceeds its bound");
    }
    validate_label(label);
    auto tree = entries_;
    auto order = order_at(tree, tree.size());
    const auto relabel = order == 0;
    if (relabel) {
        tree = relabelled(tree);
        order = order_at(tree, tree.size());
    }
    auto replacement = tree.insert(tree.size(), Entry{std::move(id), std::move(label), order});
    if (relabel) {
        auto index = identities(replacement);
        orders_.swap(index);
        ++relabels_;
    } else {
        const auto& entry = replacement.at(replacement.size() - 1);
        orders_.emplace(entry.id, entry.order);
    }
    entries_ = std::move(replacement);
}

void TabIndex::erase(std::string_view id) {
    const auto index = position(id);
    if (!index) {
        return;
    }
    const auto prepared = entries_.at(*index).generation == generation_;
    auto replacement = entries_.erase(*index, *index + 1);
    orders_.erase(orders_.find(id));
    entries_ = std::move(replacement);
    prepared_ -= prepared ? 1 : 0;
}

void TabIndex::move(std::string_view id, std::size_t destination) {
    const auto source = position(id);
    if (!source || destination >= size()) {
        throw std::out_of_range("Tab reorder requires existing source and destination");
    }
    if (*source == destination) {
        return;
    }
    auto entry = entries_.at(*source);
    auto tree = entries_.erase(*source, *source + 1);
    entry.order = order_at(tree, destination);
    const auto relabel = entry.order == 0;
    if (relabel) {
        tree = relabelled(tree);
        entry.order = order_at(tree, destination);
    }
    auto replacement = tree.insert(destination, std::move(entry));
    if (relabel) {
        auto index = identities(replacement);
        orders_.swap(index);
        ++relabels_;
    } else {
        orders_.find(id)->second = replacement.at(destination).order;
    }
    entries_ = std::move(replacement);
}

void TabIndex::rename(std::string_view id, std::string label) {
    validate_label(label);
    const auto index = position(id);
    if (!index) {
        throw std::out_of_range("Tab rename requires an existing identity");
    }
    auto entry = entries_.at(*index);
    if (entry.label == label) {
        return;
    }
    const auto prepared = entry.generation == generation_;
    entry.label = std::move(label);
    entry.generation = 0;
    auto replacement = entries_.erase(*index, *index + 1).insert(*index, std::move(entry));
    entries_ = std::move(replacement);
    prepared_ -= prepared ? 1 : 0;
}

void TabIndex::invalidate() {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Tab font generation exhausted");
    }
    ++generation_;
    prepared_ = 0;
}

void TabIndex::measured(std::size_t index, float physical_label_width, float scale) {
    if (!std::isfinite(physical_label_width) || physical_label_width < 0 || !std::isfinite(scale) ||
        scale <= 0) {
        throw std::invalid_argument("Tab measurement requires finite nonnegative pixels and scale");
    }
    auto entry = entries_.at(index);
    const auto prepared = entry.generation == generation_;
    entry.width = static_cast<float>(
        std::clamp(static_cast<double>(physical_label_width) / scale + 64, 110.0, 260.0));
    entry.generation = generation_;
    auto replacement = entries_.erase(index, index + 1).insert(index, std::move(entry));
    entries_ = std::move(replacement);
    prepared_ += prepared ? 0 : 1;
    ++measurements_;
}

std::optional<std::size_t> TabIndex::next_pending() const {
    const auto result = entries_.seek_prefix([&](const Summary& summary) {
        return summary.oldest_generation < generation_;
    });
    return result.item ? std::optional(result.index) : std::nullopt;
}

std::vector<std::string> TabIndex::ids() const {
    std::vector<std::string> result;
    result.reserve(size());
    entries_.for_each([&](const Entry& entry) {
        result.push_back(entry.id);
    });
    return result;
}
} // namespace astra::ui
