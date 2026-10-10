#pragma once

#include <astra/protocol/rpc.hpp>
#include <cstdint>
#include <cstddef>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace astra::ui {
// Accepted platform commands retain their target identity across JS awaits.
// The document field names the scope (a document or a view). One handler per
// scope runs at a time; independent scopes can run concurrently. Document read
// fences include only inputs accepted before that read.
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
    // Successors pause while running handlers await their own fence or modal input.
    [[nodiscard]] bool waiting_only(std::span<const Identity> tasks) const noexcept;

  private:
    Identity sequence_{};
    std::size_t bytes_{};
    std::map<Identity, Input> inputs_;
};
} // namespace astra::ui
