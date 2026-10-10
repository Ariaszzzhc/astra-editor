#include <astra/ui/input_events.hpp>

#include <stdexcept>
#include <utility>

namespace astra::ui {
SDL_Event StoredInputEvent::get() const noexcept {
    auto result = event;
    if (result.type == SDL_EVENT_TEXT_INPUT) {
        result.text.text = has_text ? text.c_str() : nullptr;
    } else if (result.type == SDL_EVENT_TEXT_EDITING) {
        result.edit.text = has_text ? text.c_str() : nullptr;
    }
    return result;
}

bool InputEvents::accepts(Uint32 type) noexcept {
    switch (type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        return true;
    default:
        return false;
    }
}

void InputEvents::push(const SDL_Event& event, SDL_Keymod modifiers) {
    if (!accepts(event.type)) {
        throw std::invalid_argument("Unsupported deferred input event");
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && !events_.empty() &&
        events_.back().event.type == SDL_EVENT_MOUSE_MOTION) {
        events_.back().event = event;
        events_.back().modifiers = modifiers;
        return;
    }
    if (events_.size() == max_events) {
        throw std::length_error("Input event queue capacity exceeded");
    }
    const char* payload = nullptr;
    if (event.type == SDL_EVENT_TEXT_INPUT) {
        payload = event.text.text;
    } else if (event.type == SDL_EVENT_TEXT_EDITING) {
        payload = event.edit.text;
    }
    std::size_t length{};
    if (payload) {
        while (length <= max_text_bytes && payload[length] != '\0') {
            ++length;
        }
    }
    if (length > max_text_bytes || length > max_bytes - bytes_) {
        throw std::length_error("Input event text capacity exceeded");
    }
    StoredInputEvent stored{event, modifiers, {}, payload != nullptr};
    if (payload) {
        stored.text.assign(payload, length);
    }
    if (stored.event.type == SDL_EVENT_TEXT_INPUT) {
        stored.event.text.text = nullptr;
    } else if (stored.event.type == SDL_EVENT_TEXT_EDITING) {
        stored.event.edit.text = nullptr;
    }
    events_.push_back(std::move(stored));
    bytes_ += length;
}

StoredInputEvent InputEvents::take() {
    if (events_.empty()) {
        throw std::out_of_range("Cannot take input from an empty queue");
    }
    auto result = std::move(events_.front());
    bytes_ -= result.text.size();
    events_.pop_front();
    return result;
}

void InputEvents::clear() noexcept {
    events_.clear();
    bytes_ = 0;
}

std::size_t InputEvents::size() const noexcept {
    return events_.size();
}

std::size_t InputEvents::bytes() const noexcept {
    return bytes_;
}
} // namespace astra::ui
