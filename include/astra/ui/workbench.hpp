#pragma once

#include <astra/script/runtime.hpp>
#include <astra/ui/editor_channel.hpp>

#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace astra::ui {

// Client policy host. Documents remain behind EditorChannel; UI primitives are
// supplied by the native view. Futures are advanced only on the UI owner loop.
class Workbench final {
  public:
    using Json = protocol::Json;
    using NativeRequest = script::Runtime::AsyncRequest;
    using Completion = std::function<void(const script::Runtime::Outcome&)>;
    Workbench(std::shared_ptr<EditorChannel> channel,
              NativeRequest native,
              const std::map<std::string, std::string>& modules,
              std::string_view user_source = {});
    ~Workbench();
    Workbench(const Workbench&) = delete;
    Workbench& operator=(const Workbench&) = delete;
    [[nodiscard]] bool contains(std::string_view command) const;
    void execute(std::string_view command, const Json& params, Completion complete);
    void poll();
    void emit(std::string_view event, const Json& params);
    [[nodiscard]] bool idle() const;
    [[nodiscard]] bool awaiting_input() const;
    [[nodiscard]] bool awaiting(std::string_view method) const;
    [[nodiscard]] bool awaiting(std::span<const std::string_view> methods) const;
    [[nodiscard]] Json settings() const;
    [[nodiscard]] std::map<std::string, std::string> keybindings() const;
    [[nodiscard]] std::vector<Json> take_errors();
    void shutdown();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::ui
