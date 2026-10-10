#pragma once

#include <astra/protocol/rpc.hpp>
#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace astra::ui {
// Accepted platform edits retain their visible document identity across JS
// awaits. One handler per document runs at a time; unrelated documents can run
// concurrently. Read fences include only inputs accepted before that read.
class InputQueue final {
  public:
    using Identity = std::uint64_t;

    struct Input {
        std::string document;
        std::string command;
        protocol::Json parameters;
        std::optional<Identity> task;
        std::size_t bytes{};
    };

    [[nodiscard]] Identity
    accept(std::string document, std::string command, protocol::Json parameters);
    [[nodiscard]] std::vector<Identity> ready() const;
    [[nodiscard]] const Input& at(Identity identity) const;
    void started(Identity identity, Identity task);
    void retire(Identity identity);
    void clear();
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] Identity fence(std::string_view document, Identity caller) const;
    [[nodiscard]] bool pending(std::string_view document, Identity fence) const;

  private:
    Identity sequence_{};
    std::size_t bytes_{};
    std::map<Identity, Input> inputs_;
};
} // namespace astra::ui
