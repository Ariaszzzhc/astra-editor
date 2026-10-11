#pragma once

#include "astra/ui/editor_channel.hpp"
#include "astra/ui/workbench_layout.hpp"
#include "astra/ui/inline_form.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;

namespace astra::ui {

struct EditorWindowOptions {
    std::string title{"Astra"};
    std::string workspace_name{"Workspace"};
    std::vector<std::string> open_paths;
    // Logical UI units, scaled and clamped to the current display's usable area.
    int width{1440};
    int height{900};
    float font_size{16};
    std::filesystem::path resource_directory;
    std::string user_script;
};

struct WorkbenchViewState {
    WorkbenchLayout layout;
    TabStrip tab_strip;
    std::vector<std::string> tabs;
    std::size_t visible_file_rows{};
    std::size_t outline_first{}, outline_total{}, visible_outline_rows{};
    std::size_t file_first{};
    std::optional<InlineFormLayout> inline_form_layout;
    protocol::Json inline_form;
    protocol::Json sidebar;
    std::optional<InlineFormLayout> sidebar_form_layout;
    protocol::Json sidebar_form;
    protocol::Json view_retirement;
    protocol::Json document_viewport;
    protocol::Json document;
    protocol::Json text_surface;
    protocol::Json input_layout;
    protocol::Json text_labels;
    protocol::Json tab_layout;
    protocol::Json file_icons;
    protocol::Json style;
    protocol::Json window_frame;
    protocol::Json outline;
};

// Main-thread editor front end. Every document change goes through EditorChannel.
class EditorWindow final {
  public:
    explicit EditorWindow(std::shared_ptr<EditorChannel> channel, EditorWindowOptions options = {});
    ~EditorWindow();
    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    // Polls input, advances ready responses without waiting, and paints one frame.
    // Returns false after a confirmed close. run adds a bounded idle delay.
    [[nodiscard]] bool pump();
    int run();
    [[nodiscard]] SDL_Window* window() const noexcept;
    void request_screenshot(std::filesystem::path destination);
    // These use exactly the same action path as menus, keybindings, and palette.
    void execute_ui_command(std::string_view command);
    void open_path(std::string path);
    [[nodiscard]] std::string active_document_id() const;
    [[nodiscard]] bool idle() const noexcept;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] std::string status_text() const;
    [[nodiscard]] std::string workspace_notice() const;
    [[nodiscard]] WorkbenchLayout layout() const;
    [[nodiscard]] WorkbenchViewState view_state() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace astra::ui
