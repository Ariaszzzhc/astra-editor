#include <astra/ui/input_queue.hpp>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace astra::ui {
InputQueue::Identity
InputQueue::accept(std::string document, std::string command, protocol::Json parameters) {
    if (document.empty() || document.size() > 128 || command.empty() || command.size() > 256 ||
        !parameters.is_object())
        throw protocol::RpcError(-32602, "Invalid platform editing input");
    if (inputs_.size() >= 256 || sequence_ == std::numeric_limits<Identity>::max())
        throw protocol::RpcError(-32004, "Platform input queue capacity exceeded");
    parameters["id"] = document;
    const auto bytes = parameters.dump().size() + command.size() + document.size();
    if (bytes > protocol::Dispatcher::max_body_bytes - bytes_)
        throw protocol::RpcError(-32004, "Platform input byte capacity exceeded");
    const auto identity = sequence_ + 1;
    inputs_.emplace(
        identity, Input{std::move(document), std::move(command), std::move(parameters), {}, bytes});
    bytes_ += bytes;
    sequence_ = identity;
    return identity;
}

std::vector<InputQueue::Identity> InputQueue::ready() const {
    std::set<std::string_view> seen;
    std::vector<Identity> result;
    for (const auto& [identity, input] : inputs_)
        if (seen.insert(input.document).second && !input.task)
            result.push_back(identity);
    return result;
}

const InputQueue::Input& InputQueue::at(Identity identity) const {
    return inputs_.at(identity);
}

void InputQueue::started(Identity identity, Identity task) {
    auto& input = inputs_.at(identity);
    if (!task || input.task)
        throw std::logic_error("Platform input already started or missing task identity");
    for (const auto& [other, value] : inputs_)
        if ((value.task == task) || (other < identity && value.document == input.document))
            throw std::logic_error("Platform input task must preserve per-document order");
    input.task = task;
}

void InputQueue::retire(Identity identity) {
    const auto found = inputs_.find(identity);
    if (found != inputs_.end()) {
        bytes_ -= found->second.bytes;
        inputs_.erase(found);
    }
}

void InputQueue::clear() {
    inputs_.clear();
    bytes_ = 0;
}

bool InputQueue::empty() const noexcept {
    return inputs_.empty();
}

InputQueue::Identity InputQueue::fence(std::string_view document, Identity caller) const {
    if (caller)
        for (const auto& [identity, input] : inputs_)
            if (input.document == document && input.task == caller)
                return identity - 1;
    return sequence_;
}

bool InputQueue::pending(std::string_view document, Identity fence) const {
    for (const auto& [identity, input] : inputs_) {
        if (identity > fence)
            break;
        if (input.document == document)
            return true;
    }
    return false;
}
} // namespace astra::ui
