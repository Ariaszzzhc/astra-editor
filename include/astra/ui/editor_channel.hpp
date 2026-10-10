#pragma once

#include <future>
#include <optional>
#include <string>
#include <vector>

namespace astra::ui {

// The GUI does not own documents. Both local and TCP channels deliver the same
// JSON-RPC dispatcher responses and notifications. submit must return promptly;
// a future becomes ready when the service has finished that individual request.
class EditorChannel {
  public:
    virtual ~EditorChannel() = default;
    [[nodiscard]] virtual std::future<std::optional<std::string>> submit(std::string body) = 0;
    [[nodiscard]] virtual std::vector<std::string> take_notifications() = 0;
};

} // namespace astra::ui
