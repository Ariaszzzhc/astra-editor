#pragma once

#include <astra/core/document.hpp>
#include <astra/editing/commands.hpp>
#include <astra/protocol/rpc.hpp>
#include <astra/ui/workbench_layout.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace astra::ui {
// Bounded ephemeral widget state. Product actions and result policy live in JS.
// Text inputs reuse Unicode editing primitives, independently of service buffers.
class InlineForm final {
  public:
    using Json = protocol::Json;

    struct Field {
        std::string id, placeholder, submit, shift_submit;
        astra::Document text;
    };

    struct Toggle {
        std::string id, label, shortcut;
        bool value{};
    };

    struct Action {
        std::string id, label;
    };

    static constexpr std::size_t max_input_bytes = 16384;
    explicit InlineForm(const Json& spec, std::string epoch = {});

    [[nodiscard]] const std::string& id() const noexcept {
        return id_;
    }

    [[nodiscard]] const std::string& title() const noexcept {
        return title_;
    }

    [[nodiscard]] const std::string& epoch() const noexcept {
        return epoch_;
    }

    [[nodiscard]] const std::string& view() const noexcept {
        return view_;
    }

    [[nodiscard]] const std::string& message() const noexcept {
        return message_;
    }

    [[nodiscard]] bool busy() const noexcept {
        return busy_;
    }

    [[nodiscard]] bool error() const noexcept {
        return error_;
    }

    [[nodiscard]] std::string version() const;
    [[nodiscard]] Json state() const;

    [[nodiscard]] const std::vector<Field>& fields() const noexcept {
        return fields_;
    }

    [[nodiscard]] const std::vector<Toggle>& toggles() const noexcept {
        return toggles_;
    }

    [[nodiscard]] const std::vector<Action>& actions() const noexcept {
        return actions_;
    }

    [[nodiscard]] std::optional<std::size_t> focus() const noexcept {
        return focus_;
    }

    void focus(std::optional<std::size_t> index, bool select_all = false);
    void cycle_focus(bool backwards);
    [[nodiscard]] bool edit(editing::Command command, const editing::Options& options = {});
    [[nodiscard]] bool history(bool redo);
    void select(std::size_t field, ByteOffset offset, bool extend);
    void toggle(std::size_t index);
    // Late results cannot change a newer input generation or another widget.
    [[nodiscard]] bool update(const Json& params);
    [[nodiscard]] bool set_values(const Json& params);

  private:
    void require_generation() const;
    void changed();
    std::string id_, title_, view_, message_, epoch_;
    bool busy_{}, error_{};
    std::uint64_t version_{1};
    std::optional<std::size_t> focus_;
    std::vector<Field> fields_;
    std::vector<Toggle> toggles_;
    std::vector<Action> actions_;
};

struct InlineFormLayout {
    Box panel, title, close, message;
    std::vector<Box> fields, toggles, actions;
    static InlineFormLayout measure(Box editor,
                                    float scale,
                                    std::size_t fields,
                                    std::size_t toggles,
                                    std::size_t actions,
                                    DialogMetrics metrics = {20, 15, 18, 12, 14});
    static InlineFormLayout measure_sidebar(Box available,
                                            float scale,
                                            std::size_t fields,
                                            std::size_t toggles,
                                            std::size_t actions,
                                            DialogMetrics metrics = {20, 15, 18, 12, 14});
};
} // namespace astra::ui
