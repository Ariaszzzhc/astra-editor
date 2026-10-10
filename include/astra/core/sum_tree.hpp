#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace astra {

// Persistent measured AVL sequence. Summary{} is the identity, operator+
// composes summaries associatively in sequence order, and Measure{}(item)
// measures one immutable item. Measure must be a stateless function object.
// Each mutation copies O(log n) branch nodes; untouched leaves and branches
// remain shared by all snapshots. Items themselves are never mutated.
template <typename Item, typename Summary, typename Measure> class SumTree {
    struct Node;
    using NodePtr = std::shared_ptr<const Node>;

    struct Node {
        NodePtr left;
        NodePtr right;
        std::optional<Item> item;
        Summary summary;
        std::size_t count;
        std::size_t height;

        explicit Node(Item value)
            : item(std::move(value)), summary(Measure{}(*item)), count(1), height(1) {
        }

        Node(NodePtr before, NodePtr after)
            : left(std::move(before)), right(std::move(after)),
              summary(left->summary + right->summary),
              count(joined_count(left->count, right->count)),
              height(1 + std::max(left->height, right->height)) {
        }

        [[nodiscard]] bool is_leaf() const noexcept {
            return item.has_value();
        }

        [[nodiscard]] static std::size_t joined_count(std::size_t before, std::size_t after) {
            if (after > std::numeric_limits<std::size_t>::max() - before) {
                throw std::length_error("SumTree item count overflow");
            }
            return before + after;
        }
    };

  public:
    struct SeekResult {
        std::size_t index{};
        Summary prefix{};
        const Item* item{};
    };

    SumTree() = default;

    explicit SumTree(std::vector<Item> items) : root_(build(items, 0, items.size())) {
    }

    [[nodiscard]] static SumTree singleton(Item item) {
        return SumTree(std::make_shared<Node>(std::move(item)));
    }

    [[nodiscard]] bool empty() const noexcept {
        return !root_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return count(root_);
    }

    [[nodiscard]] std::size_t height() const noexcept {
        return height(root_);
    }

    [[nodiscard]] const Summary& summary() const noexcept {
        static const Summary identity{};
        return root_ ? root_->summary : identity;
    }

    // Proves equal edges by immutable node identity, skipping whole shared
    // subtrees. Stops at the first distinct leaf even if its value is equal.
    // No hashing, item comparison, flattening, or traversal of shared interiors.
    [[nodiscard]] Summary shared_prefix_summary(const SumTree& other) const {
        return shared_edge(other, false);
    }

    [[nodiscard]] Summary shared_suffix_summary(const SumTree& other) const {
        return shared_edge(other, true);
    }

    [[nodiscard]] const Item& at(std::size_t index) const {
        if (index >= size()) {
            throw std::out_of_range("SumTree item index is outside the tree");
        }
        auto node = root_.get();
        while (!node->is_leaf()) {
            if (index < node->left->count) {
                node = node->left.get();
            } else {
                index -= node->left->count;
                node = node->right.get();
            }
        }
        return *node->item;
    }

    [[nodiscard]] Summary prefix_summary(std::size_t item_count) const {
        if (item_count > size()) {
            throw std::out_of_range("SumTree prefix is outside the tree");
        }
        Summary prefix{};
        auto node = root_.get();
        while (node && item_count != 0) {
            if (item_count == node->count) {
                return prefix + node->summary;
            }
            if (item_count < node->left->count) {
                node = node->left.get();
            } else {
                item_count -= node->left->count;
                prefix = prefix + node->left->summary;
                node = node->right.get();
            }
        }
        return prefix;
    }

    // projection must be an additive, nonnegative dimension of Summary. Seek
    // returns the leaf containing offset in the half-open measured range;
    // offset == total returns {size(), summary(), nullptr}. Zero-width leaves
    // are skipped. The returned item pointer lives as long as this snapshot.
    template <typename Projection>
    [[nodiscard]] SeekResult seek(std::size_t offset, Projection projection) const {
        const auto total = static_cast<std::size_t>(std::invoke(projection, summary()));
        if (offset > total) {
            throw std::out_of_range("SumTree measured offset is outside the tree");
        }
        if (offset == total) {
            return {size(), summary(), nullptr};
        }
        SeekResult result;
        auto node = root_.get();
        while (!node->is_leaf()) {
            const auto left_size =
                static_cast<std::size_t>(std::invoke(projection, node->left->summary));
            if (offset < left_size) {
                node = node->left.get();
            } else {
                offset -= left_size;
                result.index += node->left->count;
                result.prefix = result.prefix + node->left->summary;
                node = node->right.get();
            }
        }
        result.item = std::addressof(*node->item);
        return result;
    }

    [[nodiscard]] std::pair<SumTree, SumTree> split(std::size_t index) const {
        if (index > size()) {
            throw std::out_of_range("SumTree split index is outside the tree");
        }
        auto [left, right] = split_node(root_, index);
        return {SumTree(std::move(left)), SumTree(std::move(right))};
    }

    // First item whose inclusive prefix satisfies a monotone predicate.
    // Supports ordered summaries (for example the largest source ordinal),
    // whose projection is not an additive distance. O(log n), no flattening.
    template <typename Predicate> [[nodiscard]] SeekResult seek_prefix(Predicate predicate) const {
        if (!root_ || !std::invoke(predicate, summary())) {
            return {size(), summary(), nullptr};
        }
        SeekResult result;
        auto node = root_.get();
        while (!node->is_leaf()) {
            const auto before = result.prefix + node->left->summary;
            if (std::invoke(predicate, before)) {
                node = node->left.get();
            } else {
                result.index += node->left->count;
                result.prefix = before;
                node = node->right.get();
            }
        }
        result.item = std::addressof(*node->item);
        return result;
    }

    [[nodiscard]] SumTree concat(const SumTree& other) const {
        return SumTree(join(root_, other.root_));
    }

    [[nodiscard]] SumTree insert(std::size_t index, Item item) const {
        auto [before, after] = split(index);
        return before.concat(singleton(std::move(item))).concat(after);
    }

    [[nodiscard]] SumTree erase(std::size_t begin, std::size_t end) const {
        if (begin > end || end > size()) {
            throw std::out_of_range("Invalid SumTree erase range");
        }
        auto [before, remainder] = split(begin);
        auto [removed, after] = remainder.split(end - begin);
        return before.concat(after);
    }

    template <typename Function> void for_each(Function&& function) const {
        visit(root_, function);
    }

    // Visits only intersecting branches: O(log n + number of visited items).
    template <typename Function>
    void for_each_range(std::size_t begin, std::size_t end, Function&& function) const {
        if (begin > end || end > size()) {
            throw std::out_of_range("Invalid SumTree visit range");
        }
        visit_range(root_, begin, end, function);
    }

    // Predicate examines a subtree's cached summary. False must prove that no
    // item in that subtree is wanted; true descends in source order. The visitor
    // receives the original item ordinal, including skipped subtrees.
    template <typename Predicate, typename Function>
    void for_each_pruned(Predicate&& predicate, Function&& function) const {
        const auto snapshot = root_;
        visit_pruned(snapshot, 0, predicate, function);
    }

    // Intended for tests and diagnostics; deliberately O(n).
    [[nodiscard]] bool validate() const {
        return validate_node(root_).valid;
    }

  private:
    NodePtr root_;

    template <typename Predicate, typename Function>
    static void visit_pruned(const NodePtr& node,
                             std::size_t ordinal,
                             Predicate& predicate,
                             Function& function) {
        if (!node || !std::invoke(predicate, node->summary)) {
            return;
        }
        if (node->is_leaf()) {
            std::invoke(function, ordinal, *node->item);
            return;
        }
        visit_pruned(node->left, ordinal, predicate, function);
        visit_pruned(node->right, ordinal + node->left->count, predicate, function);
    }

    [[nodiscard]] Summary shared_edge(const SumTree& other, bool reverse) const {
        Summary result{};
        if (!root_ || !other.root_) {
            return result;
        }
        std::vector<const Node*> left{root_.get()};
        std::vector<const Node*> right{other.root_.get()};
        const auto expand = [reverse](std::vector<const Node*>& stack) {
            const auto* node = stack.back();
            stack.pop_back();
            stack.push_back(reverse ? node->left.get() : node->right.get());
            stack.push_back(reverse ? node->right.get() : node->left.get());
        };
        while (!left.empty() && !right.empty()) {
            const auto* a = left.back();
            const auto* b = right.back();
            if (a == b) {
                result = reverse ? a->summary + result : result + a->summary;
                left.pop_back();
                right.pop_back();
            } else if (a->is_leaf() && b->is_leaf()) {
                break;
            } else if (!a->is_leaf() && (b->is_leaf() || a->height >= b->height)) {
                expand(left);
            } else {
                expand(right);
            }
        }
        return result;
    }

    explicit SumTree(NodePtr root) : root_(std::move(root)) {
    }

    [[nodiscard]] static std::size_t height(const NodePtr& node) noexcept {
        return node ? node->height : 0;
    }

    [[nodiscard]] static std::size_t count(const NodePtr& node) noexcept {
        return node ? node->count : 0;
    }

    [[nodiscard]] static NodePtr branch(NodePtr left, NodePtr right) {
        if (!left) {
            return right;
        }
        if (!right) {
            return left;
        }
        return std::make_shared<Node>(std::move(left), std::move(right));
    }

    [[nodiscard]] static NodePtr balance(NodePtr left, NodePtr right) {
        if (height(left) > height(right) + 1) {
            if (height(left->left) >= height(left->right)) {
                return branch(left->left, branch(left->right, std::move(right)));
            }
            const auto& pivot = left->right;
            return branch(branch(left->left, pivot->left), branch(pivot->right, std::move(right)));
        }
        if (height(right) > height(left) + 1) {
            if (height(right->right) >= height(right->left)) {
                return branch(branch(std::move(left), right->left), right->right);
            }
            const auto& pivot = right->left;
            return branch(branch(std::move(left), pivot->left), branch(pivot->right, right->right));
        }
        return branch(std::move(left), std::move(right));
    }

    [[nodiscard]] static NodePtr join(const NodePtr& left, const NodePtr& right) {
        if (!left) {
            return right;
        }
        if (!right) {
            return left;
        }
        if (height(left) > height(right) + 1) {
            return balance(left->left, join(left->right, right));
        }
        if (height(right) > height(left) + 1) {
            return balance(join(left, right->left), right->right);
        }
        return branch(left, right);
    }

    [[nodiscard]] static std::pair<NodePtr, NodePtr> split_node(const NodePtr& node,
                                                                std::size_t index) {
        if (index == 0) {
            return {{}, node};
        }
        if (index == count(node)) {
            return {node, {}};
        }
        if (index < node->left->count) {
            auto [before, after] = split_node(node->left, index);
            return {std::move(before), join(after, node->right)};
        }
        auto [before, after] = split_node(node->right, index - node->left->count);
        return {join(node->left, before), std::move(after)};
    }

    [[nodiscard]] static NodePtr
    build(std::vector<Item>& items, std::size_t begin, std::size_t end) {
        if (begin == end) {
            return {};
        }
        if (end - begin == 1) {
            return std::make_shared<Node>(std::move(items[begin]));
        }
        const auto middle = begin + (end - begin) / 2;
        return branch(build(items, begin, middle), build(items, middle, end));
    }

    template <typename Function> static void visit(const NodePtr& node, Function& function) {
        if (!node) {
            return;
        }
        if (node->is_leaf()) {
            std::invoke(function, *node->item);
            return;
        }
        visit(node->left, function);
        visit(node->right, function);
    }

    template <typename Function>
    static void
    visit_range(const NodePtr& node, std::size_t begin, std::size_t end, Function& function) {
        if (!node || begin == end) {
            return;
        }
        if (node->is_leaf()) {
            std::invoke(function, *node->item);
            return;
        }
        const auto left_count = node->left->count;
        if (begin < left_count) {
            visit_range(node->left, begin, std::min(end, left_count), function);
        }
        if (end > left_count) {
            visit_range(node->right,
                        begin > left_count ? begin - left_count : 0,
                        end - left_count,
                        function);
        }
    }

    struct Validation {
        bool valid{true};
        Summary summary{};
        std::size_t count{};
        std::size_t height{};
    };

    [[nodiscard]] static Validation validate_node(const NodePtr& node) {
        if (!node) {
            return {};
        }
        if (node->is_leaf()) {
            const auto expected = Measure{}(*node->item);
            return {!node->left && !node->right && node->count == 1 && node->height == 1 &&
                        node->summary == expected,
                    expected,
                    1,
                    1};
        }
        if (!node->left || !node->right) {
            return {false, {}, 0, 0};
        }
        const auto left = validate_node(node->left);
        const auto right = validate_node(node->right);
        const auto expected = left.summary + right.summary;
        const auto expected_height = 1 + std::max(left.height, right.height);
        const auto expected_count = left.count + right.count;
        return {left.valid && right.valid && left.height <= right.height + 1 &&
                    right.height <= left.height + 1 && node->summary == expected &&
                    node->height == expected_height && node->count == expected_count,
                expected,
                expected_count,
                expected_height};
    }
};

} // namespace astra
