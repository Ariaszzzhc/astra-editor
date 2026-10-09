#include "astra/ui/editor_window.hpp"
#include "astra/ui/renderer.hpp"
#include "astra/ui/text_layout.hpp"
#include "astra/ui/workbench.hpp"
#include "astra/ui/file_tree_index.hpp"
#include "astra/ui/icons.hpp"
#include "astra/io/file_store.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

namespace astra::ui {
namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::string lower_ascii(std::string text) {
    for (auto& c : text) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a' - 'A')); } }
    return text;
}

std::string basename(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return std::string(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
}

std::string normalized_binding(std::string_view binding) {
    const auto key = binding.substr(binding.find_last_of('+') == std::string_view::npos ? 0 : binding.find_last_of('+') + 1);
    return std::string(binding.find("Ctrl+") != std::string_view::npos ? "Ctrl+" : "") +
        (binding.find("Shift+") != std::string_view::npos ? "Shift+" : "") +
        (binding.find("Alt+") != std::string_view::npos ? "Alt+" : "") + std::string(key);
}

struct SdlStringDeleter { void operator()(char* value) const noexcept { SDL_free(value); } };

struct MenuCommand { std::string id; std::string label; std::string shortcut; };
} // namespace

struct EditorWindow::Impl {
    struct Line {
        std::size_t number{};
        std::size_t begin{};
        std::size_t end{};
        std::size_t next{};
        bool complete{};
        std::string text;
        std::optional<TextLayout> layout;
    };
    struct Document {
        Json metadata;
        std::string path;
        Json editor_config = Json::object();
        std::string encoding{"UTF-8"};
        std::string view_revision;
        std::map<std::size_t, Line> lines;
        std::size_t first_line{};
        float horizontal{};
        bool needs_lines{true};
        bool loading{};
        std::string syntax_revision;
        std::string syntax_state{"pending"};
        Json highlights = Json::array();
        Json outline = Json::array();
        bool needs_syntax{true};
        bool syntax_loading{};
        bool reveal_horizontal{};
        Json diagnostics = Json::array();
        Json git_gutter = Json::array();
        bool diagnostics_loading{};
        bool language_active{};
        Clock::time_point diagnostics_after{};
        std::uint64_t diagnostics_generation{};
    };
    struct Request {
        std::string method;
        Json params;
        std::string document;
        bool inject_revision{};
        std::function<void(const Json&)> complete;
        std::function<Json()> build_params;
        std::function<void()> failure{};
    };
    struct ActiveRequest { Request request; std::string id; std::future<std::optional<std::string>> future; };
    struct Operation { std::string id; std::function<void(const Json&)> complete; std::function<void()> failure; bool polling{}; Clock::time_point next_poll{}; Json accumulated; std::size_t offset{}; };
    struct FindState { std::string id; std::string revision; std::string query; std::string replacement; std::string text; std::size_t cursor{}; std::size_t size{}; bool replace{}; };
    struct ViewState { std::string id; std::string revision; std::size_t first{}; std::size_t limit{}; std::map<std::size_t, Line> lines; };
    enum class Prompt { none, policy_input, policy_confirm, palette, open, save_as, close, find, replace_query, replace_value, search, workspace_replace_query, workspace_replace_value, replace_confirm, completion, locations, hover, rename, rename_confirm, git_diff, git_discard, git_commit, recovery, recovery_confirm, recovery_flushing, recovery_exit_failed, reload_confirm };
    struct Dialog {
        std::string id; Json spec; std::promise<Json> completion;
        std::optional<TextLayout> detail_layout; float detail_width{};
        std::optional<TextLayout> input_layout; std::string input_text; float input_scale{};
    };

    std::shared_ptr<EditorChannel> channel;
    EditorWindowOptions options;
    Renderer renderer;
    TextLayoutEngine fonts;
    std::map<std::string, Document> documents;
    std::vector<std::string> tabs;
    std::size_t first_tab{};
    std::string active_id;
    std::deque<Request> requests;
    std::optional<ActiveRequest> active_request;
    std::vector<std::shared_ptr<Operation>> operations;
    std::uint64_t next_request{1};
    std::map<std::string, std::string> keybindings;
    std::vector<MenuCommand> script_commands;
    std::vector<MenuCommand> menu_commands;
    std::unique_ptr<Workbench> workbench;
    std::set<std::string> service_ui_commands;
    Json busy_views = Json::object();
    Json settings = Json::object();
    Json editor_settings = Json::object();
    SkColor background{0xFF191D27};
    SkColor sidebar_background{0xFF161A23};
    SkColor panel_background{0xFF1E2330};
    SkColor border_color{0xFF2B3140};
    SkColor foreground{0xFFDDE3EF};
    SkColor muted{0xFF8893A8};
    SkColor accent{0xFF7FA9F9};
    SkColor selection_color{0xFF304A73};
    SkColor status_color{0xFF263B5E};
    SkColor activity_background{0xFF141821};
    SkColor active_tab_background{0xFF191D27};
    SkColor error_color{0xFFF14C4C};
    SkColor warning_color{0xFFD7BA7D};
    SkColor hover_color{0xFF2A2D2E};
    SkColor current_line_color{0xFF272727};
    SkColor input_color{0xFF313131};
    SkColor cursor_color{0xFFAEAFAD};
    SkColor scrollbar_color{0xFF424242};
    SkColor folder_color{0xFFD7BA7D};
    SkColor code_icon_color{0xFF75BEFF};
    SkColor script_icon_color{0xFFE5C07B};
    SkColor added_color{0xFF73C991};
    SkColor changed_color{0xFF75BEFF};
    SkColor notice_background{0xFF332D18};
    SkColor recovery_background{0xFF263222};
    SkColor error_background{0xFF3A1D1D};
    SkColor status_foreground{0xFFCCCCCC};
    std::map<std::string, SkColor> syntax_colors{
        {"comment", 0xFF6A9955}, {"string", 0xFFCE9178}, {"number", 0xFFB5CEA8}, {"keyword", 0xFFC586C0},
        {"type", 0xFF4EC9B0}, {"function", 0xFFDCDCAA}, {"property", 0xFF9CDCFE}, {"variable", 0xFF9CDCFE},
        {"heading", 0xFF569CD6}, {"error", 0xFFF44747}};
    FileTreeIndex file_tree;
    Json search_results = Json::array();
    std::string search_query;
    std::string search_operation;
    std::string replacement_text;
    bool search_view{};
    bool replacing_workspace{};
    bool search_case_sensitive{true};
    bool search_regex{};
    bool search_whole_word{};
    std::uint64_t search_generation{};
    std::map<std::string, std::string> workspace_notices;
    std::string watch_error;
    std::string replace_notice;
    Json language_items = Json::array();
    std::string language_document;
    std::string language_revision;
    std::size_t language_offset{};
    std::uint64_t language_generation{};
    std::string hover_text;
    Json rename_plan;
    bool git_view{};
    Json git_status = Json::object();
    std::string git_path;
    std::string git_preview;
    bool git_staged{};
    std::size_t tree_scroll{};
    std::map<std::string, TextLayout> labels;
    float scale{1};
    float row_height{26};
    float left{};
    float right{};
    float content_top{};
    float content_bottom{};
    float gutter{};
    bool show_sidebar{true};
    bool show_outline{};
    bool explorer_expanded{true};
    float sidebar_width{248};
    float ui_font_size{13};
    WorkbenchLayout geometry;
    TabStrip tab_strip;
    bool reveal_tab{true};
    bool dragging_sidebar{};
    std::string dragging_tab;
    float tab_drag_start{};
    bool running{true};
    bool stopping_workbench{};
    Clock::time_point workbench_shutdown_deadline{};
    bool closing_window{};
    bool recovery_enabled{true};
    bool recovery_ready{};
    bool recovery_status_loading{};
    bool recovery_list_loading{};
    bool recovery_available{};
    bool recovery_flushing{};
    std::uint64_t recovery_close_generation{};
    Clock::time_point recovery_status_after{};
    Json recovery_sessions = Json::array();
    Json recovery_selected;
    std::set<std::string> closing_pending;
    std::set<std::string> close_after_save;
    bool dragging{};
    bool dragging_scroll{};
    Clock::time_point drag_scroll_after{};
    std::size_t drag_anchor{};
    float mouse_x{};
    float mouse_y{};
    Prompt prompt{Prompt::none};
    std::optional<Dialog> dialog;
    std::string prompt_text;
    std::string prompt_document;
    std::string prompt_revision;
    std::size_t prompt_choice{};
    bool prompt_select_all{};
    std::string preedit;
    std::string error;
    std::string status{"Starting workspace..."};
    std::optional<std::filesystem::path> screenshot;
    std::optional<float> vertical_column;

    Impl(std::shared_ptr<EditorChannel> input, EditorWindowOptions configuration)
        : channel(std::move(input)), options(std::move(configuration)), renderer(options.title, options.width, options.height) {
        if (!channel) { throw std::invalid_argument("EditorWindow requires a document channel"); }
        if (options.resource_directory.empty()) {
            const auto* base = SDL_GetBasePath();
            if (!base) { throw std::runtime_error(std::string("Locate workbench resources: ") + SDL_GetError()); }
            options.resource_directory = std::filesystem::path(base);
        }
        std::map<std::string, std::string> modules;
        for (const auto& [name, file] : std::map<std::string, std::string>{
            {"astra/ui", "api.js"}, {"astra/files", "files.js"}, {"astra/git", "git.js"},
            {"astra/search", "search.js"}, {"astra/clipboard", "clipboard.js"}, {"astra/commands", "commands.js"}, {"astra/workbench", "workbench.js"}}) {
            modules.emplace(name, io::read_file(options.resource_directory / "data" / "workbench" / file, 1024 * 1024).text);
        }
        workbench = std::make_unique<Workbench>(channel, [this](std::string_view method, const Json& params) {
            if (method == "_ui.dialog") { return begin_dialog(params); }
            std::promise<Json> completed;
            try { completed.set_value(native_ui_request(method, params)); }
            catch (...) { completed.set_exception(std::current_exception()); }
            return completed.get_future();
        }, modules, options.user_script);
        const auto policy = workbench->settings().at("workbench").at("commands");
        for (const auto& [id, spec] : policy.items()) {
            menu_commands.push_back({id, spec.at("title").get<std::string>(), spec.value("shortcut", std::string{})});
        }
        scale = renderer.dpi_scale();
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(renderer.window()), &usable)) {
            const auto requested_width = static_cast<int>(std::round(static_cast<float>(options.width) * scale));
            const auto requested_height = static_cast<int>(std::round(static_cast<float>(options.height) * scale));
            if (!SDL_SetWindowSize(renderer.window(), std::min(requested_width, usable.w * 9 / 10), std::min(requested_height, usable.h * 9 / 10))) {
                throw std::runtime_error(std::string("Set DPI-scaled editor dimensions: ") + SDL_GetError());
            }
        }
        keybindings = {
            {"Ctrl+N", "ui.new"}, {"Ctrl+P", "ui.quickOpen"}, {"Ctrl+Shift+P", "ui.commandPalette"},
            {"Ctrl+S", "ui.save"}, {"Ctrl+Shift+S", "ui.saveAs"}, {"Ctrl+W", "ui.closeTab"},
            {"Ctrl+Z", "editor.undo"}, {"Ctrl+Y", "editor.redo"}, {"Ctrl+Shift+Z", "editor.redo"},
            {"Ctrl+A", "editor.select_all"}, {"Ctrl+C", "ui.copy"}, {"Ctrl+X", "ui.cut"}, {"Ctrl+V", "ui.paste"},
            {"Ctrl+/", "editor.toggle_line_comment"}, {"Shift+Alt+Down", "editor.duplicate_line"},
            {"Ctrl+Alt+Up", "editor.add_cursor_above"}, {"Ctrl+Alt+Down", "editor.add_cursor_below"},
            {"Ctrl+B", "ui.toggleSidebar"}, {"Ctrl+Tab", "ui.nextTab"},
            {"Ctrl+F", "ui.find"}, {"Ctrl+H", "ui.replace"}, {"Ctrl+Shift+F", "ui.search"},
            {"Ctrl+Shift+H", "ui.workspaceReplace"}, {"Ctrl+Space", "ui.completion"},
            {"Ctrl+K", "ui.hover"}, {"F12", "ui.definition"}, {"Shift+F12", "ui.references"},
            {"F2", "ui.rename"}, {"Shift+Alt+F", "ui.format"}, {"Ctrl+Shift+G", "ui.gitStatus"},
            {"Ctrl+Shift+E", "ui.explorer"}, {"Ctrl+Shift+M", "ui.diagnostics"}
        };
        enqueue({"script.settings", Json::object(), {}, false, [this](const Json& result) {
            settings = result.value("settings", Json::object());
            settings.merge_patch(workbench->settings());
            editor_settings = settings.value("editor", Json::object());
            options.font_size = editor_settings.value("fontSize", options.font_size);
            if (!std::isfinite(options.font_size) || options.font_size < 6 || options.font_size > 128) {
                throw std::runtime_error("editor.fontSize must be between 6 and 128");
            }
            const auto theme = settings.value("theme", Json::object());
            const auto color = [&](std::string_view name, SkColor& destination) {
                const auto field = theme.find(std::string(name));
                if (field == theme.end()) { return; }
                const auto hex = field->get<std::string>();
                if (hex.size() != 7 || hex.front() != '#') { throw std::runtime_error("Theme colors require #RRGGBB"); }
                std::size_t parsed{};
                const auto rgb = std::stoul(hex.substr(1), &parsed, 16);
                if (parsed != 6) { throw std::runtime_error("Theme color contains non-hexadecimal digits"); }
                destination = 0xFF000000U | static_cast<SkColor>(rgb);
            };
            color("background", background); color("foreground", foreground); color("sidebar", sidebar_background);
            color("tabs", panel_background); color("border", border_color); color("muted", muted);
            color("accent", accent); color("selection", selection_color); color("status", status_color);
            color("activityBar", activity_background); color("activeTab", active_tab_background);
            color("error", error_color); color("warning", warning_color);
            color("hover", hover_color); color("currentLine", current_line_color); color("input", input_color);
            color("cursor", cursor_color); color("scrollbar", scrollbar_color); color("folder", folder_color);
            color("codeIcon", code_icon_color); color("scriptIcon", script_icon_color);
            color("added", added_color); color("changed", changed_color); color("noticeBackground", notice_background);
            color("recoveryBackground", recovery_background); color("errorBackground", error_background);
            color("statusForeground", status_foreground);
            const auto desired_sidebar=settings.value("workbench",Json::object()).value("sidebarWidth",248.0F);
            const auto desired_ui_font=settings.value("workbench",Json::object()).value("uiFontSize",13.0F);
            if (!std::isfinite(desired_sidebar) || !std::isfinite(desired_ui_font) || desired_ui_font<9 || desired_ui_font>16) { throw std::runtime_error("Invalid workbench sidebarWidth or uiFontSize (9 to 16)"); }
            sidebar_width=std::clamp(desired_sidebar,180.0F,480.0F);ui_font_size=desired_ui_font;
            for (auto& [name, value] : syntax_colors) { color(name, value); }
            if (result.contains("keybindings") && result["keybindings"].is_object()) {
                for (const auto& [key, command] : result["keybindings"].items()) {
                    if (command.is_string()) { keybindings[normalized_binding(key)] = command.get<std::string>(); }
                }
            } else if (result.contains("keybindings") && result["keybindings"].is_array()) {
                for (const auto& binding : result["keybindings"]) {
                    keybindings[normalized_binding(binding.at("key").get<std::string>())] = binding.at("command").get<std::string>();
                }
            }
            for (const auto& command : result.value("commands", Json::array())) {
                const auto name = command.get<std::string>();
                if (name.starts_with("ui.")) { service_ui_commands.insert(name); }
                if (std::ranges::none_of(menu_commands, [&](const auto& item) { return item.id == name; })) {
                    script_commands.push_back({name, name, "JavaScript"});
                }
            }
            const auto client_settings = workbench->settings().value("workbench", Json::object());
            const auto client_commands = client_settings.value("commands", Json::object());
            for (const auto& [name, spec] : client_commands.items()) {
                if (std::ranges::none_of(menu_commands, [&](const auto& item) { return item.id == name; })) {
                    script_commands.push_back({name, spec.at("title").get<std::string>(), spec.value("shortcut", std::string{})});
                }
            }
            for (const auto& [key, name] : workbench->keybindings()) { keybindings[normalized_binding(key)] = name; }
        }, {}});
        enqueue({"workspace.documents", Json::object(), {}, false, [this](const Json& result) {
            for (const auto& metadata : result) { accept_metadata(metadata); }
            if (!tabs.empty()) { activate(tabs.front()); }
            else if (options.open_paths.empty()) { new_document(); }
            for (const auto& path : options.open_paths) { open_path(path); }
        }, {}});
        refresh_tree();
        fetch_recovery_status();
    }

    Document* current() {
        const auto found = documents.find(active_id);
        return found == documents.end() ? nullptr : &found->second;
    }
    const Document* current() const {
        const auto found = documents.find(active_id);
        return found == documents.end() ? nullptr : &found->second;
    }
    void enqueue(Request request) {
        if (requests.size() >= 4096) { throw std::runtime_error("Editor command queue is full; wait for the service"); }
        requests.push_back(std::move(request));
    }
    void accept_metadata(const Json& metadata) {
        const auto id = metadata.at("id").get<std::string>();
        auto [found, inserted] = documents.try_emplace(id);
        auto& document = found->second;
        if (inserted) { tabs.push_back(id); }
        const auto revision = metadata.at("revision").get<std::string>();
        if (!document.metadata.empty() && std::stoull(revision) < std::stoull(document.metadata.at("revision").get<std::string>())) { return; }
        if (metadata.contains("path")) {
            const auto path = metadata["path"].get<std::string>();
            if (path != document.path && !path.empty()) { document.language_active = true; document.diagnostics_after = {}; }
            document.path = path;
        }
        if (metadata.contains("editorConfig")) { document.editor_config = metadata["editorConfig"]; }
        if (metadata.contains("encoding")) { document.encoding = metadata.at("encoding").get<std::string>(); }
        if (document.view_revision != revision) { document.needs_lines = true; document.needs_syntax = true; document.diagnostics.clear(); }
        document.metadata = metadata;
        if (metadata.value("dirty", false)) { document.git_gutter.clear(); }
        const auto line_count = metadata.value("lineCount", std::size_t{1});
        document.first_line = std::min(document.first_line, line_count - 1);
    }
    void activate(const std::string& id) {
        if (!documents.contains(id)) { return; }
        active_id = id;
        reveal_tab = true;
        documents.at(id).needs_lines = true;
        preedit.clear();
        vertical_column.reset();
    }
    void remove_document(const std::string& id) {
        documents.erase(id);
        workspace_notices.erase(id);
        std::erase(tabs, id);
        if (active_id == id) { active_id = tabs.empty() ? std::string{} : tabs.back(); }
        if (closing_window) { continue_close_window(); }
    }
    void new_document() {
        enqueue({"document.create", {{"text", ""}}, {}, false, [this](const Json& result) {
            accept_metadata(result); activate(result.at("id").get<std::string>()); status = "New file";
        }, {}});
    }
    void start_operation(std::string method, Json params, std::function<void(const Json&)> complete,
                         std::string document = {}, bool revision = false, std::function<void()> failure = {}) {
        const bool search = method == "workspace.search";
        const auto generation = search_generation;
        enqueue({std::move(method), std::move(params), std::move(document), revision,
            [this, complete = std::move(complete), failure, search, generation](const Json& result) {
                auto operation = std::make_shared<Operation>();
                operation->id = result.at("operationId").get<std::string>();
                if (search && generation == search_generation) { search_operation = operation->id; }
                operation->complete = complete;
                operation->failure = failure;
                operations.push_back(std::move(operation));
            }, {}, std::move(failure)});
    }
    void open_path(std::string path) {
        if (path.empty()) { return; }
        status = "Opening " + path;
        execute_policy("workbench.open", {{"path", path}});
    }
    void refresh_tree(bool announce = true) {
        static_cast<void>(announce);
        command("ui.refreshTree");
    }
    std::string workspace_notice() const {
        if (!watch_error.empty()) { return watch_error; }
        if (const auto found = workspace_notices.find(active_id); found != workspace_notices.end()) { return found->second; }
        if (!replace_notice.empty()) { return replace_notice; }
        return workspace_notices.empty() ? std::string{} : workspace_notices.begin()->second;
    }
    bool search_prompt() const {
        return prompt == Prompt::search || prompt == Prompt::workspace_replace_query || prompt == Prompt::workspace_replace_value;
    }

    void service_responses();
    void fetch_lines();
    void fetch_lines_page(std::shared_ptr<ViewState> view, std::size_t first, std::optional<std::size_t> begin = {});
    void fetch_syntax();
    void language_request(std::string method, std::string new_name = {});
    void accept_language(const Json& result, std::size_t offset);
    void fetch_diagnostics(const std::string& id, bool show = false);
    std::vector<std::size_t> filtered_language_items() const;
    void fetch_git_diff(std::string path, bool staged = false);
    void editor_command(std::string command, Json extra = Json::object());
    void set_selections(Json selections, std::string revision = {});
    void save_document(const std::string& id, bool save_as = false);
    void close_document(const std::string& id, bool discard = false);
    void continue_close_window();
    void fetch_recovery_status();
    void refresh_recovery(bool show);
    void flush_recovery(bool close);
    std::vector<std::size_t> filtered_recovery_sessions() const;
    void command(std::string_view name);
    void execute_policy(std::string_view name, const Json& params = Json::object());
    void native_command(std::string_view name);
    Json native_ui_request(std::string_view method, const Json& params);
    std::future<Json> begin_dialog(const Json& spec);
    void finish_dialog(Json value);
    void find_next(std::shared_ptr<FindState> state);
    void find_document(bool replace);
    void search_workspace(bool replace);
    void open_search_result(const Json& match);
    [[nodiscard]] std::string newline() const;
    void event(const SDL_Event& event);
    void key(const SDL_KeyboardEvent& key);
    void point(float x, float y, bool extend, bool add);
    void scroll_to(float y);
    void vertical(bool down, bool extend);
    void draw();
    void measure_layout();
    void draw_prompt(SkCanvas* canvas, float width, float height);
    void label(SkCanvas* canvas, std::string_view text, float x, float y, float size, SkColor color = 0);
    const TextLayout& label_layout(std::string_view text, float size, SkColor color);
    void elided_label(SkCanvas* canvas,std::string_view text,float x,float y,float size,float available,SkColor color);
    void rectangle(SkCanvas* canvas, float x, float y, float width, float height, SkColor color);
    void reveal_caret();
    std::vector<const MenuCommand*> filtered_commands() const;
    void accept_prompt();
};

void EditorWindow::Impl::service_responses() {
    for (const auto& body : channel->take_notifications()) {
        const auto notification = Json::parse(body);
        const auto method = notification.value("method", std::string{});
        workbench->emit(method, notification.value("params", Json::object()));
        if (method == "document.changed" || method == "document.created") { accept_metadata(notification.at("params")); }
        else if (method == "document.closed") { remove_document(notification.at("params").at("id").get<std::string>()); }
        else if (method == "recovery.available") { recovery_available = true; refresh_recovery(false); }
        else if (method == "recovery.error") {
            error = "Recovery checkpoint: " + notification.at("params").value("message", std::string("Storage failed"));
            status = "Recovery needs attention";
        }
        else if (method == "language.diagnosticsChanged") {
            const auto id = notification.at("params").at("id").get<std::string>();
            if (const auto found = documents.find(id); found != documents.end()) {
                found->second.language_active = true;
                found->second.diagnostics_after = {};
                ++found->second.diagnostics_generation;
            }
        }
        else if (method == "workspace.watchError") {
            watch_error = "File monitoring: " + notification.at("params").value("message", std::string("Monitoring failed; reload files explicitly"));
        }
        else if (method == "workspace.fileChanged") {
            const auto& value = notification.at("params");
            const auto id = value.at("id").get<std::string>();
            const auto path = value.at("path").get<std::string>();
            const auto change = value.at("status").get<std::string>();
            if (change == "reloaded") { workspace_notices[id] = "External changes reloaded: " + path; }
            else if (change == "deleted") { workspace_notices[id] = "File deleted outside Astra; buffer preserved: " + path; }
            else if (change == "conflict") { workspace_notices[id] = "File changed outside Astra; unsaved buffer preserved: " + path; }
            else { workspace_notices[id] = "Cannot read " + path + ": " + value.value("message", std::string("External file error")); }
            refresh_tree(false);
        }
    }
    for (int processed = 0; processed < 32; ++processed) {
        if (active_request) {
            if (active_request->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { break; }
            auto completed = std::move(*active_request);
            active_request.reset();
            try {
                const auto body = completed.future.get();
                if (!body) { throw std::runtime_error("The service returned no response for an editor request"); }
                const auto response = Json::parse(*body);
                if (!response.contains("id") || response["id"] != completed.id) { throw std::runtime_error("The service response ID does not match its request"); }
                if (response.contains("error")) { throw std::runtime_error(response["error"].value("message", std::string("Service request failed"))); }
                if (completed.request.method == "script.command" && response.at("result").is_object() && response.at("result").contains("operationId")) {
                    auto operation = std::make_shared<Operation>();
                    operation->id = response.at("result").at("operationId").get<std::string>();
                    operation->complete = completed.request.complete;
                    operation->failure = completed.request.failure;
                    operations.push_back(std::move(operation));
                } else if (completed.request.complete) { completed.request.complete(response.at("result")); }
                if (completed.request.method == "script.command" || completed.request.method == "document.edit") { error.clear(); }
            } catch (const std::exception& failure) {
                error = failure.what(); status = "Action failed";
                if (completed.request.failure) { completed.request.failure(); }
                if (completed.request.method == "operation.get") {
                    const auto id = completed.request.params.value("id", std::string{});
                    for (const auto& operation : operations) { if (operation->id == id && operation->failure) { operation->failure(); } }
                    std::erase_if(operations, [&](const auto& operation) { return operation->id == id; });
                }
                if (completed.request.method == "document.close") {
                    closing_pending.erase(completed.request.document); closing_window = false;
                }
                if (completed.request.method == "document.syntax" && documents.contains(completed.request.document)) {
                    auto& document = documents.at(completed.request.document);
                    document.syntax_loading = false; document.needs_syntax = false; document.syntax_state = "failed";
                }
                if (completed.request.method == "language.diagnostics" && documents.contains(completed.request.document)) {
                    auto& document = documents.at(completed.request.document);
                    document.diagnostics_loading = false; document.language_active = false;
                }
                if (completed.request.method == "document.lines" && documents.contains(completed.request.document)) {
                    auto& document = documents.at(completed.request.document);
                    document.loading = false; document.needs_lines = false;
                } else if (completed.request.method == "script.command" || completed.request.method == "document.edit" || completed.request.method == "document.setSelections") {
                    if (auto* document = current()) { document->needs_lines = true; }
                }
            }
        }
        if (requests.empty()) { break; }
        auto request = std::move(requests.front());
        requests.pop_front();
        if (!request.document.empty() && !documents.contains(request.document)) { continue; }
        if (request.build_params) { request.params = request.build_params(); }
        if (request.inject_revision) {
            const auto found = documents.find(request.document);
            if (found == documents.end()) { continue; }
            auto& params = request.method == "script.command" ? request.params["params"] : request.params;
            params["id"] = request.document;
            params["baseRevision"] = found->second.metadata.at("revision");
        }
        const auto id = std::to_string(next_request++);
        auto future = channel->submit(Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", request.method}, {"params", request.params}}.dump());
        active_request.emplace(ActiveRequest{std::move(request), id, std::move(future)});
    }
    const auto now = Clock::now();
    for (const auto& operation : operations) {
        if (operation->polling || now < operation->next_poll) { continue; }
        operation->polling = true;
        enqueue({"operation.get", {{"id", operation->id}, {"offset", operation->offset}, {"limit", 256}}, {}, false,
            [this, operation](const Json& result) {
                operation->polling = false;
                operation->next_poll = Clock::now() + std::chrono::milliseconds(35);
                const auto state = result.at("state").get<std::string>();
                if (state == "completed") {
                    auto value = result.at("result");
                    if (result.contains("page")) {
                        const auto& page = result["page"];
                        for (const auto field : {"entries", "matches"}) {
                            if (value.contains(field)) {
                                if (operation->accumulated.empty()) { operation->accumulated = value; }
                                else { for (const auto& entry : value[field]) { operation->accumulated[field].push_back(entry); } }
                            }
                        }
                        if (page.contains("nextOffset") && !page["nextOffset"].is_null() && page["nextOffset"].get<std::size_t>() < page.at("total").get<std::size_t>()) {
                            operation->offset = page["nextOffset"].get<std::size_t>();
                            return;
                        }
                        if (!operation->accumulated.empty()) { value = operation->accumulated; }
                    }
                    std::erase(operations, operation);
                    if (operation->complete) { operation->complete(value); }
                } else if (state == "failed" || state == "cancelled") {
                    if (operation->failure) { operation->failure(); }
                    std::erase(operations, operation);
                    throw std::runtime_error(result.contains("error") ? result["error"].value("message", std::string("Workspace operation failed")) : "Workspace operation cancelled");
                }
            }, {}});
    }
    fetch_lines();
    fetch_syntax();
    if (const auto* document = current(); document && document->language_active && !document->diagnostics_loading &&
        Clock::now() >= document->diagnostics_after && requests.empty() && !active_request) { fetch_diagnostics(active_id); }
    if (recovery_enabled && !recovery_ready && !recovery_status_loading && now >= recovery_status_after && requests.empty() && !active_request) {
        fetch_recovery_status();
    }
    if (closing_window && prompt == Prompt::none && requests.empty() && !active_request && operations.empty()) { continue_close_window(); }
}

void EditorWindow::Impl::fetch_lines() {
    auto* document = current();
    if (!document || !document->needs_lines || document->loading || !requests.empty() || active_request) { return; }
    const auto id = active_id;
    const auto first = document->first_line > 2 ? document->first_line - 2 : 0;
    const auto visible = static_cast<std::size_t>(std::max(1.0F, (content_bottom - content_top) / row_height)) + 8;
    document->loading = true;
    auto view = std::make_shared<ViewState>();
    view->id = id; view->revision = document->metadata.at("revision").get<std::string>();
    view->first = first; view->limit = std::min(first + std::min(visible, std::size_t{256}), document->metadata.at("lineCount").get<std::size_t>());
    fetch_lines_page(std::move(view), first);
}

void EditorWindow::Impl::fetch_lines_page(std::shared_ptr<ViewState> view, std::size_t first, std::optional<std::size_t> begin) {
    Json params{{"id", view->id}, {"firstLine", first}, {"count", view->limit - first}};
    if (begin) { params["begin"] = *begin; }
    enqueue({"document.lines", std::move(params), view->id, false,
        [this, view](const Json& result) {
            if (!documents.contains(view->id)) { return; }
            auto& document = documents.at(view->id);
            if (result.at("revision") != view->revision || document.metadata.at("revision") != view->revision) {
                document.loading = false; document.needs_lines = true; return;
            }
            accept_metadata(result);
            for (const auto& record : result.at("lines")) {
                const auto number = record.at("line").get<std::size_t>();
                auto [found, inserted] = view->lines.try_emplace(number);
                auto& line = found->second;
                line.number = number;
                if (inserted) { line.begin = record.at("begin").get<std::size_t>(); }
                line.end = record.at("end").get<std::size_t>();
                line.next = record.at("nextBegin").get<std::size_t>();
                line.complete = record.at("complete").get<bool>();
                line.text += record.at("text").get<std::string>();
            }
            if (result.contains("continuation")) {
                const auto& continuation = result.at("continuation");
                fetch_lines_page(view, continuation.at("firstLine").get<std::size_t>(), continuation.at("begin").get<std::size_t>()); return;
            }
            const auto next = result.at("nextLine").get<std::size_t>();
            if (next < view->limit && !result.at("lines").empty()) { fetch_lines_page(view, next); return; }
            document.lines = std::move(view->lines);
            document.view_revision = view->revision;
            document.loading = false;
            const auto rows = static_cast<std::size_t>(std::max(1.0F, (content_bottom - content_top) / row_height)) + 1;
            const auto required_end = std::min(document.first_line + rows, document.metadata.at("lineCount").get<std::size_t>());
            document.needs_lines = document.first_line < view->first || required_end > view->limit;
            document.needs_syntax = true;
        }, {}});
}

void EditorWindow::Impl::fetch_syntax() {
    auto* document = current();
    if (!document || document->loading || document->needs_lines || !document->needs_syntax || document->syntax_loading ||
        !requests.empty() || active_request || document->lines.empty()) { return; }
    const auto id = active_id;
    const auto revision = document->view_revision;
    document->syntax_loading = true;
    enqueue({"document.syntax", {{"id", id}, {"baseRevision", revision}, {"begin", document->lines.begin()->second.begin},
                                {"end", document->lines.rbegin()->second.end}}, id, false,
        [this, id, revision](const Json& result) {
            if (!documents.contains(id)) { return; }
            auto& document = documents.at(id);
            document.syntax_loading = false;
            if (document.metadata.at("revision") != revision) { document.needs_syntax = true; return; }
            const auto state = result.at("state").get<std::string>();
            document.syntax_state = state;
            if (state == "pending") {
                document.syntax_loading = true;
                auto operation = std::make_shared<Operation>();
                operation->id = result.at("operationId").get<std::string>();
                operation->complete = [this, id](const Json&) {
                    if (documents.contains(id)) { documents.at(id).syntax_loading = false; documents.at(id).needs_syntax = true; }
                };
                operation->failure = [this, id] {
                    if (documents.contains(id)) { auto& doc = documents.at(id); doc.syntax_loading = false; doc.needs_syntax = false; doc.syntax_state = "failed"; }
                };
                operations.push_back(std::move(operation));
                return;
            }
            document.syntax_revision = revision; document.needs_syntax = false;
            document.highlights = result.value("highlights", Json::array());
            document.outline = result.value("outline", Json::array());
            for (auto& [number, line] : document.lines) { static_cast<void>(number); line.layout.reset(); }
        }, {}});
}

void EditorWindow::Impl::editor_command(std::string name, Json extra) {
    if (!current()) { return; }
    if (!name.starts_with("editor.move_up") && !name.starts_with("editor.move_down")) { vertical_column.reset(); }
    if (name != "editor.undo" && name != "editor.redo") {
        auto tab_size = editor_settings.value("tabSize", 4U);
        auto insert_spaces = editor_settings.value("insertSpaces", true);
        const auto& config = current()->editor_config;
        if (config.contains("tab_width")) { tab_size = static_cast<unsigned>(std::stoul(config.at("tab_width").get<std::string>())); }
        else if (config.contains("indent_size") && config.at("indent_size") != "tab") { tab_size = static_cast<unsigned>(std::stoul(config.at("indent_size").get<std::string>())); }
        if (config.contains("indent_style")) { insert_spaces = config.at("indent_style") == "space"; }
        extra["tabSize"] = tab_size;
        extra["insertSpaces"] = insert_spaces;
        extra["newline"] = newline();
    }
    const auto id = active_id;
    enqueue({"script.command", {{"name", name}, {"params", std::move(extra)}}, id, true,
        [this](const Json& result) { accept_metadata(result); reveal_caret(); status = "Ready"; }, {}});
}

void EditorWindow::Impl::language_request(std::string method, std::string new_name) {
    const auto* document = current();
    if (!document) { return; }
    language_document = active_id;
    documents.at(active_id).language_active = true;
    language_revision = document->metadata.at("revision").get<std::string>();
    language_offset = document->metadata.at("selections").front().at("head").get<std::size_t>();
    Json params{{"id", language_document}, {"baseRevision", language_revision}, {"method", method}};
    if (method != "formatting") { params["byteOffset"] = language_offset; }
    if (method == "rename") { params["newName"] = std::move(new_name); }
    status = "Language server: " + method;
    const auto generation = ++language_generation;
    const auto offset = language_offset;
    start_operation("language.request", std::move(params), [this, generation, offset](const Json& result) {
        if (generation == language_generation) { accept_language(result, offset); }
    });
}

void EditorWindow::Impl::accept_language(const Json& result, std::size_t offset) {
    const auto id = result.at("id").get<std::string>();
    const auto revision = result.at("revision").get<std::string>();
    if (result.value("obsolete", false) || !documents.contains(id) || documents.at(id).metadata.at("revision") != revision) {
        status = "Discarded a language result for an older revision"; return;
    }
    language_document = id; language_revision = revision; language_offset = offset;
    const auto method = result.at("method").get<std::string>();
    const auto& value = result.at("result");
    fetch_diagnostics(id);
    prompt_text.clear(); prompt_choice = 0;
    if (method == "completion") {
        language_items = value.is_array() ? value : (value.is_object() ? value.value("items", Json::array()) : Json::array());
        if (language_items.empty()) { status = "No completion items"; return; }
        prompt = Prompt::completion;
    } else if (method == "hover") {
        hover_text.clear();
        if (!value.is_null()) {
            const auto& contents = value.at("contents");
            const auto append = [this](const Json& part) {
                if (!hover_text.empty()) { hover_text += '\n'; }
                hover_text += part.is_string() ? part.get<std::string>() : part.value("value", std::string{});
            };
            if (contents.is_array()) { for (const auto& part : contents) { append(part); } }
            else { append(contents); }
        }
        if (hover_text.empty()) { status = "No hover information"; return; }
        prompt = Prompt::hover;
    } else if (method == "definition" || method == "references") {
        language_items = result.value("resolvedLocations", Json::array());
        if (language_items.empty()) { status = "No locations found"; return; }
        prompt = Prompt::locations;
        if (language_items.size() == 1) { accept_prompt(); }
    } else if (method == "formatting") {
        if (value.is_null() || value.empty()) { status = "Document is already formatted"; return; }
        enqueue({"language.applyTextEdits", {{"id", id}, {"baseRevision", revision}, {"edits", value}}, {}, false,
            [this](const Json& metadata) { accept_metadata(metadata); reveal_caret(); status = "Document formatted"; }, {}});
    } else if (method == "rename") {
        rename_plan = result;
        if (!result.contains("planId") || result.value("documents", Json::array()).empty()) { status = "No rename edits"; return; }
        prompt = Prompt::rename_confirm;
    }
}

void EditorWindow::Impl::fetch_diagnostics(const std::string& id, bool show) {
    if (!documents.contains(id) || (documents.at(id).diagnostics_loading && !show)) { return; }
    documents.at(id).diagnostics_loading = true;
    const auto generation = documents.at(id).diagnostics_generation;
    enqueue({"language.diagnostics", {{"id", id}}, id, false, [this, id, show, generation](const Json& result) {
        if (!documents.contains(id)) { return; }
        auto& document = documents.at(id);
        document.diagnostics_loading = false;
        document.diagnostics_after = generation == document.diagnostics_generation
            ? Clock::now() + std::chrono::milliseconds(750) : Clock::time_point{};
        document.language_active = result.value("available", true);
        if (result.at("revision") == document.metadata.at("revision")) { document.diagnostics = result.value("diagnostics", Json::array()); }
        if (show) {
            hover_text.clear();
            for (const auto& diagnostic : document.diagnostics) {
                const auto& point = diagnostic.at("range").at("start");
                hover_text += std::to_string(point.at("line").get<std::size_t>() + 1) + ":" +
                    std::to_string(point.at("character").get<std::size_t>() + 1) + "  " + diagnostic.at("message").get<std::string>() + "\n";
            }
            if (hover_text.empty()) { hover_text = result.value("pending", false) ? "Waiting for language diagnostics..." : result.value("reason", std::string("No diagnostics")); }
            prompt = Prompt::hover; prompt_choice = 0;
        }
    }, {}});
}

std::vector<std::size_t> EditorWindow::Impl::filtered_language_items() const {
    std::vector<std::size_t> result;
    const auto query = lower_ascii(prompt_text);
    for (std::size_t i = 0; i < language_items.size(); ++i) {
        const auto& item = language_items[i];
        const auto text = prompt == Prompt::completion ? item.value("label", std::string{}) : item.value("path", item.value("uri", std::string{}));
        if (query.empty() || lower_ascii(text).find(query) != std::string::npos) { result.push_back(i); }
    }
    return result;
}

void EditorWindow::Impl::fetch_git_diff(std::string path, bool staged) {
    if (path.empty()) { return; }
    git_path = std::move(path); git_staged = staged;
    start_operation("git.diff", {{"path", git_path}, {"staged", staged}}, [this](const Json& result) {
        hover_text = result.value("binary", false) ? "Binary file — textual diff is unavailable." : result.value("patch", std::string{});
        if (hover_text.empty()) { hover_text = "No changes in this comparison."; }
        git_preview = result.value("previewId", std::string{});
        for (auto& [id, document] : documents) {
            static_cast<void>(id);
            if (document.path == git_path && !git_staged && !document.metadata.value("dirty", false)) {
                document.git_gutter = result.value("gutter", Json::array());
            }
        }
        prompt = Prompt::git_diff; prompt_choice = 0;
    });
}

void EditorWindow::Impl::set_selections(Json selections, std::string revision) {
    auto* document = current();
    if (!document) { return; }
    Json params{{"id", active_id}, {"selections", std::move(selections)}};
    if (!revision.empty()) { params["baseRevision"] = revision; }
    enqueue({"document.setSelections", std::move(params), active_id, revision.empty(),
        [this](const Json& result) { accept_metadata(result); reveal_caret(); status = "Ready"; }, {}});
}

void EditorWindow::Impl::reveal_caret() {
    auto* document = current();
    if (!document || document->metadata.at("selections").empty()) { return; }
    const auto& positions = document->metadata.contains("selectionPositions") ? document->metadata["selectionPositions"] : document->metadata["selections"];
    const auto line = positions.front().value("line", std::size_t{0});
    document->reveal_horizontal = true;
    const auto rows = static_cast<std::size_t>(std::max(1.0F, (content_bottom - content_top) / row_height));
    if (line < document->first_line) { document->first_line = line; document->needs_lines = true; }
    else if (line >= document->first_line + rows) { document->first_line = line - rows + 1; document->needs_lines = true; }
}

void EditorWindow::Impl::save_document(const std::string& id, bool save_as) {
    const auto found = documents.find(id);
    if (found == documents.end()) { return; }
    if (save_as || found->second.path.empty()) {
        prompt = Prompt::save_as; prompt_document = id; prompt_text = found->second.path;
        return;
    }
    status = "Saving " + found->second.path;
    start_operation("workspace.save", {{"id", id}}, [this, id](const Json& result) {
        accept_metadata(result); status = "Saved";
        if (close_after_save.erase(id) != 0) { close_document(id); }
        refresh_tree();
    }, id, true);
}

void EditorWindow::Impl::close_document(const std::string& id, bool discard) {
    const auto found = documents.find(id);
    if (found == documents.end() || closing_pending.contains(id)) { return; }
    if (found->second.metadata.value("dirty", false) && !discard) {
        prompt = Prompt::close; prompt_document = id; prompt_text.clear();
        return;
    }
    closing_pending.insert(id);
    enqueue({"document.close", {{"id", id}, {"discardChanges", discard}}, id, true,
        [this, id](const Json&) { closing_pending.erase(id); remove_document(id); }, {}});
}

void EditorWindow::Impl::continue_close_window() {
    if (!closing_window || !closing_pending.empty() || recovery_flushing) { return; }
    if (tabs.empty()) {
        if (requests.empty() && !active_request && operations.empty()) { flush_recovery(true); }
        return;
    }
    activate(tabs.back());
    close_document(tabs.back());
}

void EditorWindow::Impl::fetch_recovery_status() {
    if (recovery_status_loading) { return; }
    recovery_status_loading = true;
    enqueue({"recovery.status", Json::object(), {}, false, [this](const Json& result) {
        recovery_status_loading = false;
        recovery_enabled = result.value("enabled", false);
        recovery_ready = result.value("ready", false);
        recovery_status_after = Clock::now() + std::chrono::milliseconds(500);
        if (result.contains("error") && result["error"].is_string() && !result["error"].get<std::string>().empty()) {
            error = "Recovery: " + result["error"].get<std::string>();
            recovery_status_after = Clock::time_point::max();
        } else if (recovery_enabled && recovery_ready) { refresh_recovery(false); }
    }, {}, [this] { recovery_status_loading = false; recovery_status_after = Clock::time_point::max(); }});
}

void EditorWindow::Impl::refresh_recovery(bool show) {
    if (show) {
        prompt = Prompt::recovery; prompt_text.clear(); prompt_choice = 0; preedit.clear();
        status = "Looking for recoverable sessions...";
    }
    if (recovery_list_loading) { return; }
    recovery_list_loading = true;
    start_operation("recovery.list", Json::object(), [this](const Json& result) {
        recovery_list_loading = false;
        recovery_sessions = result.value("sessions", Json::array());
        recovery_available = std::ranges::any_of(recovery_sessions, [](const Json& session) {
            return !session.value("active", false) && (!session.value("buffers", Json::array()).empty() || !session.value("issues", Json::array()).empty());
        });
        if (prompt == Prompt::recovery) { status = recovery_available ? "Select a session to review recovery" : "No abandoned unsaved sessions found"; }
    }, {}, false, [this] { recovery_list_loading = false; });
}

void EditorWindow::Impl::flush_recovery(bool close) {
    if (recovery_flushing) { return; }
    recovery_flushing = true;
    const auto generation = ++recovery_close_generation;
    if (close) { prompt = Prompt::recovery_flushing; }
    status = "Writing recovery checkpoint...";
    start_operation("recovery.flush", Json::object(), [this, close, generation](const Json&) {
        recovery_flushing = false;
        status = "Recovery checkpoint saved"; error.clear();
        if (close && generation == recovery_close_generation && closing_window) {
            prompt = Prompt::none;
            if (tabs.empty()) { running = false; }
            else { continue_close_window(); }
        }
    }, {}, false, [this, close, generation] {
        recovery_flushing = false;
        if (close && generation == recovery_close_generation && closing_window) {
            closing_window = false; prompt = Prompt::recovery_exit_failed;
        }
    });
}

std::vector<std::size_t> EditorWindow::Impl::filtered_recovery_sessions() const {
    std::vector<std::size_t> result;
    const auto query = lower_ascii(prompt_text);
    for (std::size_t i = 0; i < recovery_sessions.size(); ++i) {
        const auto& session = recovery_sessions[i];
        if (session.value("active", false)) { continue; }
        if (session.value("buffers", Json::array()).empty() && session.value("issues", Json::array()).empty()) { continue; }
        auto searchable = session.at("id").get<std::string>();
        for (const auto& buffer : session.value("buffers", Json::array())) { searchable += " " + buffer.value("path", std::string("Untitled")); }
        if (query.empty() || lower_ascii(std::move(searchable)).find(query) != std::string::npos) { result.push_back(i); }
    }
    return result;
}

std::future<Json> EditorWindow::Impl::begin_dialog(const Json& spec) {
    if (dialog || prompt != Prompt::none) { throw protocol::RpcError(-32004, "Another modal input is active"); }
    const auto id = spec.at("id").get<std::string>();
    const auto kind = spec.at("kind").get<std::string>();
    if (kind != "input" && kind != "confirm") { throw protocol::RpcError(-32602, "Unknown dialog kind"); }
    const auto title = spec.at("title").get<std::string>();
    const auto value = spec.value("value", std::string{});
    const auto detail = spec.value("detail", std::string{});
    const auto accept = spec.value("accept", std::string("Confirm"));
    const auto placeholder = spec.value("placeholder", std::string{});
    if (id.empty() || id.size()>128 || title.empty() || title.size() > 4096 || value.size() > 16384 || detail.size() > 65536 || accept.size()>4096 || placeholder.size()>4096) { throw protocol::RpcError(-32602, "Dialog content exceeds its bounds"); }
    Dialog pending{id,spec,{},{},0,{}, {},0};
    auto result = pending.completion.get_future();
    dialog.emplace(std::move(pending));
    prompt = kind == "input" ? Prompt::policy_input : Prompt::policy_confirm;
    prompt_text = value; prompt_choice = 0; prompt_select_all = false; preedit.clear();
    return result;
}

void EditorWindow::Impl::finish_dialog(Json value) {
    if (!dialog) { return; }
    auto pending = std::move(*dialog); dialog.reset();
    prompt = Prompt::none; prompt_text.clear(); prompt_select_all = false; preedit.clear();
    pending.completion.set_value(std::move(value));
}

Json EditorWindow::Impl::native_ui_request(std::string_view method, const Json& params) {
    if (method == "_ui.dialog.cancel") {
        if (dialog && dialog->id == params.at("id").get<std::string>()) { finish_dialog(nullptr); }
        return nullptr;
    }
    if (method == "_ui.clipboard.read") {
        const std::unique_ptr<char, SdlStringDeleter> text(SDL_GetClipboardText());
        if (!text) { throw std::runtime_error(std::string("Clipboard read failed: ") + SDL_GetError()); }
        const std::string_view value(text.get());
        if (value.size() > 8 * 1024 * 1024) { throw std::runtime_error("Clipboard exceeds the 8 MiB scripted clipboard limit"); }
        return value;
    }
    if (method == "_ui.clipboard.write") {
        const auto text = params.at("text").get<std::string>();
        if (text.size() > 8 * 1024 * 1024 || text.find('\0') != std::string::npos) { throw std::runtime_error("Clipboard text exceeds its limit or contains NUL"); }
        if (!SDL_SetClipboardText(text.c_str())) { throw std::runtime_error(std::string("Clipboard write failed: ") + SDL_GetError()); }
        return nullptr;
    }
    if (method == "_ui.read") {
        Json result{{"sidebarVisible", show_sidebar}, {"outlineVisible", show_outline}, {"tabs", tabs}, {"gitPath", prompt == Prompt::git_diff ? git_path : std::string{}}};
        if (const auto* document = current()) {
            result["document"] = document->metadata; result["document"]["path"] = document->path;
            result["newline"] = newline(); result["selectionLines"] = Json::array();
            for (const auto& position : document->metadata.value("selectionPositions", Json::array())) {
                const auto line = document->lines.find(position.value("line", std::size_t{0}));
                if (line == document->lines.end() || document->view_revision != document->metadata.at("revision").get<std::string>()) { result["selectionLines"].push_back(nullptr); }
                else { result["selectionLines"].push_back({{"begin", line->second.begin}, {"next", line->second.next}}); }
            }
        }
        return result;
    }
    if (method == "_ui.action") { native_command(params.at("command").get<std::string>()); return nullptr; }
    if (method != "_ui.update") { throw protocol::RpcError(-32601, "Unknown native UI primitive"); }
    if (!params.is_object()) { throw protocol::RpcError(-32602, "UI updates require an object"); }
    // Establish document state before activation regardless of JSON key order.
    if (params.contains("document")) { accept_metadata(params.at("document")); }
    for (const auto& [name, value] : params.items()) {
        if (name == "status") { status = value.get<std::string>(); }
        else if (name == "sidebarVisible") { show_sidebar = value.get<bool>(); }
        else if (name == "outlineVisible") { show_outline = value.get<bool>(); }
        else if (name == "sidebar") {
            const auto view = value.get<std::string>();
            if (view != "explorer" && view != "search" && view != "git") { throw protocol::RpcError(-32602, "Unknown sidebar view"); }
            search_view = view == "search"; git_view = view == "git"; tree_scroll = 0;
        } else if (name == "document") { /* Applied before activation above. */ }
        else if (name == "activate") { activate(value.get<std::string>()); }
        else if (name == "revealCaret") { if (value.get<bool>()) { reveal_caret(); } }
        else if (name == "workspaceNotice") { workspace_notices[value.at("id").get<std::string>()] = value.at("text").get<std::string>(); }
        else if (name == "dismissPrompt") { if (value.get<bool>()) { prompt = Prompt::none; } }
        else if (name == "busy") { busy_views.merge_patch(value); }
        else if (name != "view" && name != "model") { throw protocol::RpcError(-32602, "Unknown UI update field: " + name); }
    }
    if (params.contains("view")) {
        const auto view = params.at("view").get<std::string>();
        const auto& model = params.at("model");
        if (view == "explorer") {
            file_tree.reset(model.at("entries"));
            status = model.value("truncated", false) ? "File tree reached its configured limit" : "Workspace ready";
        } else if (view == "git") { git_status = model; }
        else if (view == "search") {
            search_results = model.at("matches"); search_operation = model.at("operationId").get<std::string>();
            status = std::to_string(search_results.size()) + " workspace match(es)";
            if (model.value("truncated", false)) { error = "Search reached its limit; refine the query before replacing"; }
            else if (!model.value("issues", Json::array()).empty()) { error = "Search could not read some files; refine the scope before replacing"; }
            else if (replacing_workspace && !search_results.empty()) { prompt = Prompt::replace_confirm; }
        } else { throw protocol::RpcError(-32602, "Unknown view model"); }
    }
    if (auto* document = current(); document && (params.contains("sidebarVisible") || params.contains("outlineVisible"))) { document->needs_lines = true; }
    return nullptr;
}

void EditorWindow::Impl::execute_policy(std::string_view name, const Json& params) {
    workbench->execute(name, params, [this](const script::Runtime::Outcome& outcome) {
        if (outcome.error) {
            if (outcome.error->value("name", std::string{}) != "AbortError") { error = outcome.error->value("message", std::string("Workbench action failed")); status = "Action failed"; }
        } else if (outcome.value.is_object() && outcome.value.contains("id") && outcome.value.contains("revision")) { accept_metadata(outcome.value); reveal_caret(); }
    });
    // Settle ready native focus/layout primitives before the next SDL event.
    // Neither tick waits for a service future, parser or disk operation.
    workbench->poll(); workbench->poll();
}

void EditorWindow::Impl::command(std::string_view name) {
    prompt_select_all = false;
    if (recovery_flushing && prompt == Prompt::recovery_flushing && name != "ui.cancel") { return; }
    if (service_ui_commands.contains(std::string(name))) {
        enqueue({"script.command", {{"name", name}, {"params", Json::object()}}, active_id, !active_id.empty(),
            [this](const Json& result) { if (result.is_object() && result.contains("id") && result.contains("revision")) { accept_metadata(result); reveal_caret(); } }, {}});
    } else if (workbench->contains(name)) { execute_policy(name); }
    else { native_command(name); }
}

void EditorWindow::Impl::native_command(std::string_view name) {
    if (dialog && name != "ui.cancel" && name != "ui.closeWindow") { throw protocol::RpcError(-32004, "Dismiss the active dialog before changing its focus"); }
    prompt_select_all = false;
    if (recovery_flushing && prompt == Prompt::recovery_flushing && name != "ui.cancel") { return; }
    if (name.starts_with("editor.")) {
        editor_command(std::string(name));
    } else if (name == "ui.new") { new_document(); }
    else if (name == "ui.commandPalette") { prompt = Prompt::palette; prompt_text.clear(); prompt_choice = 0; }
    else if (name == "ui.save") { save_document(active_id); }
    else if (name == "ui.saveAs") { save_document(active_id, true); }
    else if (name == "ui.dismissWorkspaceNotice") {
        if (!watch_error.empty()) { watch_error.clear(); }
        else if (workspace_notices.contains(active_id)) { workspace_notices.erase(active_id); }
        else if (!replace_notice.empty()) { replace_notice.clear(); }
        else if (!workspace_notices.empty()) { workspace_notices.erase(workspace_notices.begin()); }
    }
    else if (name == "ui.toggleSearchCase") { search_case_sensitive = !search_case_sensitive; }
    else if (name == "ui.toggleSearchRegex") { search_regex = !search_regex; }
    else if (name == "ui.toggleSearchWholeWord") { search_whole_word = !search_whole_word; }
    else if (name == "ui.closeTab") { close_document(active_id); }
    else if (name == "ui.closeWindow") { finish_dialog(nullptr); closing_window = true; continue_close_window(); }
    else if (name == "ui.recovery") { refresh_recovery(true); }
    else if (name == "ui.checkpoint") { flush_recovery(false); }
    else if (name == "ui.confirmRecovery") {
        if (prompt != Prompt::recovery_confirm || recovery_selected.value("active", false)) { return; }
        if (recovery_selected.value("buffers", Json::array()).empty()) { status = "This session contains issues but no recoverable buffers"; return; }
        const auto session = recovery_selected.at("id").get<std::string>();
        prompt = Prompt::none;
        start_operation("recovery.restore", {{"sessionId", session}}, [this](const Json& result) {
            for (const auto& metadata : result.at("documents")) { accept_metadata(metadata); }
            if (!result.at("documents").empty()) { activate(result.at("documents").front().at("id").get<std::string>()); }
            status = "Recovered buffers are unsaved; review and save to persist";
            error.clear(); refresh_recovery(false);
        });
    }
    else if (name == "ui.retryExit") {
        if (prompt == Prompt::recovery_exit_failed) { prompt = Prompt::none; closing_window = true; continue_close_window(); }
    }
    else if (name == "ui.exitWithoutCheckpoint") {
        if (prompt == Prompt::recovery_exit_failed) { ++recovery_close_generation; running = false; }
    }
    else if (name == "ui.toggleSidebar") { show_sidebar = !show_sidebar; if (auto* doc = current()) { doc->needs_lines = true; } }
    else if (name == "ui.explorer") { show_sidebar = true; search_view = false; git_view = false; tree_scroll = 0; }
    else if (name == "ui.toggleOutline") { show_outline = !show_outline; }
    else if (name == "ui.refreshTree") { refresh_tree(); }
    else if (name == "ui.find") { prompt = Prompt::find; prompt_text = search_query; }
    else if (name == "ui.replace") { prompt = Prompt::replace_query; prompt_text = search_query; }
    else if (name == "ui.search") { prompt = Prompt::search; prompt_text = search_query; }
    else if (name == "ui.workspaceReplace") { prompt = Prompt::workspace_replace_query; prompt_text = search_query; }
    else if (name == "ui.completion") { language_request("completion"); }
    else if (name == "ui.hover") { language_request("hover"); }
    else if (name == "ui.diagnostics") { if (current()) { fetch_diagnostics(active_id, true); } }
    else if (name == "ui.definition") { language_request("definition"); }
    else if (name == "ui.references") { language_request("references"); }
    else if (name == "ui.format") { language_request("formatting"); }
    else if (name == "ui.rename") { prompt = Prompt::rename; prompt_text.clear(); }
    else if (name == "ui.gitStatus") {
        git_view = true; search_view = false; show_sidebar = true; tree_scroll = 0;
        start_operation("git.status", Json::object(), [this](const Json& result) { git_status = result; status = "Source control refreshed"; });
    }
    else if (name == "ui.gitDiff") { if (const auto* doc = current()) { fetch_git_diff(doc->path); } }
    else if (name == "ui.gitStage" || name == "ui.gitUnstage") {
        const auto path = prompt == Prompt::git_diff ? git_path : (current() ? current()->path : std::string{});
        if (path.empty()) { return; }
        prompt = Prompt::none;
        start_operation(name == "ui.gitStage" ? "git.stage" : "git.unstage", {{"paths", Json::array({path})}}, [this](const Json& result) {
            git_status = result; status = "Git index updated";
        });
    }
    else if (name == "ui.gitDiscard") {
        if (prompt == Prompt::git_diff && !git_staged && !git_preview.empty()) { prompt = Prompt::git_discard; }
    }
    else if (name == "ui.confirmDiscard") {
        if (prompt != Prompt::git_discard) { return; }
        prompt = Prompt::none;
        start_operation("git.discard", {{"previewId", git_preview}}, [this](const Json& result) {
            git_status = result; status = "Working tree change discarded"; refresh_tree();
        });
    }
    else if (name == "ui.gitCommit") {
        start_operation("git.status", Json::object(), [this](const Json& result) {
            git_status = result;
            const bool staged = std::ranges::any_of(result.at("entries"), [](const Json& entry) { return entry.at("staged") != "unchanged" && entry.at("staged") != "untracked"; });
            if (!staged) { status = "No staged changes to commit"; return; }
            prompt = Prompt::git_commit; prompt_text.clear();
        });
    }
    else if (name == "ui.confirmRename") {
        if (prompt != Prompt::rename_confirm) { return; }
        prompt = Prompt::none;
        start_operation("language.applyWorkspaceEdit", {{"planId", rename_plan.at("planId")}}, [this](const Json& result) {
            for (const auto& metadata : result.at("documents")) { accept_metadata(metadata); }
            status = "Rename applied to buffers; save files to persist";
        });
    }
    else if (name == "ui.selectFunction" || name == "ui.selectClass" || name == "ui.selectBlock" || name == "ui.selectString") {
        const auto* document = current();
        if (!document) { return; }
        if (document->syntax_state != "ready" || document->syntax_revision != document->metadata.at("revision").get<std::string>()) {
            status = "Wait for parsing before selecting a syntax object"; return;
        }
        const auto kind = name == "ui.selectFunction" ? "function" : (name == "ui.selectClass" ? "class" : (name == "ui.selectBlock" ? "block" : "string"));
        enqueue({"document.selectObject", {{"kind", kind}, {"inner", name == "ui.selectString"}}, active_id, true,
            [this](const Json& result) { accept_metadata(result); reveal_caret(); }, {}});
    }
    else if (name == "ui.confirmReplace") {
        if (prompt != Prompt::replace_confirm) { return; }
        prompt = Prompt::none;
        start_operation("workspace.replace", {{"searchId", search_operation}, {"replacement", replacement_text}}, [this](const Json& result) {
            const auto modified = result.value("modifiedBuffers", Json::array());
            for (const auto& metadata : modified) { accept_metadata(metadata); }
            replace_notice = "Replaced in " + std::to_string(result.at("applied").size()) + " disk file(s) and " +
                std::to_string(modified.size()) + " unsaved buffer(s)";
            const auto pending = result.at("pending").size() + result.value("pendingBuffers", Json::array()).size();
            if (pending != 0 || !result.at("issues").empty() || result.value("cancelled", false)) {
                replace_notice += "; " + std::to_string(pending) + " pending, " + std::to_string(result.at("issues").size()) + " issue(s). Search again before retrying";
            }
            status = replace_notice;
            search_results.clear();
            refresh_tree(false);
            if (auto* document = current()) { document->needs_lines = true; }
        });
    }
    else if (name == "ui.nextTab" && !tabs.empty()) {
        const auto current_tab = std::ranges::find(tabs, active_id);
        const auto index = current_tab == tabs.end() ? 0 : static_cast<std::size_t>(current_tab - tabs.begin());
        activate(tabs[(index + 1) % tabs.size()]);
    } else if (name == "ui.previousTab" && !tabs.empty()) {
        const auto position = std::ranges::find(tabs, active_id);
        const auto index = position == tabs.end() ? 0 : static_cast<std::size_t>(position - tabs.begin());
        activate(tabs[(index + tabs.size() - 1) % tabs.size()]);
    } else if (name == "ui.cancel") {
        finish_dialog(nullptr);
        prompt = Prompt::none; closing_window = false; ++recovery_close_generation;
        close_after_save.clear(); preedit.clear(); error.clear();
    } else if (name == "ui.discard") {
        if (prompt == Prompt::close) { const auto id = prompt_document; prompt = Prompt::none; close_document(id, true); }
    } else if (name == "ui.saveAndClose") {
        if (prompt == Prompt::close) { const auto id = prompt_document; prompt = Prompt::none; close_after_save.insert(id); save_document(id); }
    } else if (!name.starts_with("ui.")) {
        enqueue({"script.command", {{"name", name}, {"params", Json::object()}}, active_id, !active_id.empty(),
            [this](const Json& result) { if (result.is_object() && result.contains("id") && result.contains("revision")) { accept_metadata(result); reveal_caret(); } }, {}});
    } else { throw std::invalid_argument("Unknown UI command: " + std::string(name)); }
}

std::string EditorWindow::Impl::newline() const {
    const auto* document = current();
    if (!document) { return "\n"; }
    if (document->editor_config.contains("end_of_line")) {
        const auto configured = document->editor_config.at("end_of_line").get<std::string>();
        if (configured == "crlf") { return "\r\n"; }
        // Legacy CR files use LF internally; the workspace codec preserves CR on save.
        if (configured == "cr") { return "\n"; }
        return "\n";
    }
    for (const auto& [number, line] : document->lines) {
        static_cast<void>(number);
        if (line.complete && line.next - line.end == 2) { return "\r\n"; }
        if (line.complete && line.next > line.end) { return "\n"; }
    }
    return "\n";
}

std::vector<const MenuCommand*> EditorWindow::Impl::filtered_commands() const {
    std::vector<const MenuCommand*> result;
    const auto query = lower_ascii(prompt_text);
    for (const auto& item : menu_commands) {
        if (query.empty() || lower_ascii(item.label).find(query) != std::string::npos) { result.push_back(&item); }
    }
    for (const auto& item : script_commands) {
        if (query.empty() || lower_ascii(item.label).find(query) != std::string::npos) { result.push_back(&item); }
    }
    return result;
}

void EditorWindow::Impl::accept_prompt() {
    if (prompt == Prompt::policy_input) { finish_dialog(prompt_text); }
    else if (prompt == Prompt::policy_confirm) { finish_dialog(true); }
    else if (prompt == Prompt::recovery) {
        const auto matches = filtered_recovery_sessions();
        if (matches.empty()) { return; }
        recovery_selected = recovery_sessions[matches[std::min(prompt_choice, matches.size() - 1)]];
        prompt = Prompt::recovery_confirm; prompt_choice = 0; prompt_text.clear();
    } else if (prompt == Prompt::recovery_confirm) { command("ui.confirmRecovery"); }
    else if (prompt == Prompt::recovery_exit_failed) { command("ui.retryExit"); }
    else if (prompt == Prompt::completion || prompt == Prompt::locations) {
        const auto matches = filtered_language_items();
        if (matches.empty()) { return; }
        const auto item = language_items[matches[std::min(prompt_choice, matches.size() - 1)]];
        const auto mode = prompt; prompt = Prompt::none;
        if (mode == Prompt::completion) {
            const auto found = documents.find(language_document);
            if (found == documents.end() || found->second.metadata.at("revision") != language_revision ||
                found->second.metadata.at("selections").front().at("head") != language_offset) {
                status = "Completion became stale; request it again"; return;
            }
            enqueue({"language.applyCompletion", {{"id", language_document}, {"baseRevision", language_revision},
                     {"completionItem", item}, {"byteOffset", language_offset}}, {}, false,
                [this](const Json& result) { accept_metadata(result); reveal_caret(); status = "Completion applied"; }, {}});
        } else {
            if (!item.contains("path")) { status = "Location is outside the workspace: " + item.value("uri", std::string{}); return; }
            start_operation("workspace.open", {{"path", item.at("path")}}, [this, item](const Json& result) {
                accept_metadata(result); activate(result.at("id").get<std::string>());
                if (item.contains("targetRevision") && item.at("targetRevision") != result.at("revision")) {
                    status = "Navigation target changed; request the location again"; return;
                }
                if (item.contains("byteOffset")) {
                    set_selections(Json::array({{{"anchor", item.at("byteOffset")}, {"head", item.value("endByteOffset", item.at("byteOffset").get<std::size_t>())}}}));
                } else {
                    documents.at(active_id).first_line = item.value("line", std::size_t{0});
                    documents.at(active_id).needs_lines = true;
                }
            });
        }
    } else if (prompt == Prompt::hover) { prompt = Prompt::none; }
    else if (prompt == Prompt::git_diff) { prompt = Prompt::none; }
    else if (prompt == Prompt::git_discard) { command("ui.confirmDiscard"); }
    else if (prompt == Prompt::git_commit) {
        if (prompt_text.empty()) { return; }
        const auto message = prompt_text; prompt = Prompt::none;
        start_operation("git.commit", {{"message", message}}, [this](const Json&) { status = "Commit created"; command("ui.gitStatus"); });
    }
    else if (prompt == Prompt::rename) {
        if (prompt_text.empty()) { return; }
        const auto name = prompt_text; prompt = Prompt::none; language_request("rename", name);
    } else if (prompt == Prompt::palette) {
        const auto matches = filtered_commands();
        if (matches.empty()) { return; }
        const auto id = std::string(matches[std::min(prompt_choice, matches.size() - 1)]->id);
        prompt = Prompt::none; command(id);
    } else if (prompt == Prompt::open) {
        const auto path = prompt_text; prompt = Prompt::none; open_path(path);
    } else if (prompt == Prompt::save_as) {
        if (prompt_text.empty()) { return; }
        const auto id = prompt_document;
        const auto path = prompt_text;
        prompt = Prompt::none;
        start_operation("workspace.save", {{"id", id}, {"path", path}}, [this, id](const Json& result) {
            accept_metadata(result); status = "Saved"; refresh_tree();
            if (close_after_save.erase(id) != 0) { close_document(id); }
        }, id, true);
    } else if (prompt == Prompt::find) {
        if (prompt_text.empty()) { return; }
        search_query = prompt_text; prompt = Prompt::none; find_document(false);
    } else if (prompt == Prompt::replace_query || prompt == Prompt::workspace_replace_query) {
        if (prompt_text.empty()) { return; }
        search_query = prompt_text;
        prompt = prompt == Prompt::replace_query ? Prompt::replace_value : Prompt::workspace_replace_value;
        prompt_text.clear();
    } else if (prompt == Prompt::replace_value) {
        replacement_text = prompt_text; prompt = Prompt::none; find_document(true);
    } else if (prompt == Prompt::search || prompt == Prompt::workspace_replace_value) {
        const bool replace = prompt == Prompt::workspace_replace_value;
        if (!replace) { if (prompt_text.empty()) { return; } search_query = prompt_text; }
        else { replacement_text = prompt_text; }
        prompt = Prompt::none; search_workspace(replace);
    } else if (prompt == Prompt::replace_confirm) { command("ui.confirmReplace"); }
    else if (prompt == Prompt::rename_confirm) { command("ui.confirmRename"); }
    else if (prompt == Prompt::reload_confirm) { command("ui.confirmReload"); }
}

void EditorWindow::Impl::find_document(bool replace) {
    const auto* document = current();
    if (!document || search_query.empty()) { return; }
    auto state = std::make_shared<FindState>();
    state->id = active_id; state->revision = document->metadata.at("revision").get<std::string>();
    state->query = search_query; state->replacement = replacement_text; state->replace = replace;
    state->cursor = document->metadata.at("selections").front().at("head").get<std::size_t>();
    state->size = document->metadata.at("sizeBytes").get<std::size_t>();
    find_next(std::move(state));
}

void EditorWindow::Impl::find_next(std::shared_ptr<FindState> state) {
    if (state->text.size() < state->size) {
        enqueue({"document.get", {{"id", state->id}, {"begin", state->text.size()}, {"end", state->size}}, {}, false,
            [this, state](const Json& result) {
                if (result.at("revision") != state->revision) { throw std::runtime_error("Document changed while finding; try again"); }
                const auto part = result.at("text").get<std::string>();
                if (part.empty()) { throw std::runtime_error("Document search made no progress"); }
                state->text += part;
                if (state->text.size() > 64 * 1024 * 1024) { throw std::runtime_error("Document exceeds the 64 MiB find limit"); }
                find_next(state);
            }, {}});
        return;
    }
    if (state->replace) {
        auto edits = Json::array();
        std::size_t from{};
        while (from <= state->text.size()) {
            const auto match = state->text.find(state->query, from);
            if (match == std::string::npos) { break; }
            if (edits.size() >= 4096) { throw std::runtime_error("More than 4096 matches; refine the replacement query"); }
            edits.push_back({{"begin", match}, {"end", match + state->query.size()}, {"text", state->replacement}});
            from = match + state->query.size();
        }
        if (edits.empty()) { status = "No matches"; return; }
        const auto count = edits.size();
        enqueue({"document.edit", {{"id", state->id}, {"baseRevision", state->revision}, {"edits", std::move(edits)}}, {}, false,
            [this, count](const Json& result) { accept_metadata(result); reveal_caret(); status = "Replaced " + std::to_string(count) + " match(es)"; }, {}});
    } else {
        auto match = state->text.find(state->query, state->cursor);
        if (match == std::string::npos) { match = state->text.find(state->query); }
        if (match == std::string::npos) { status = "No matches"; return; }
        enqueue({"document.setSelections", {{"id", state->id}, {"baseRevision", state->revision},
                 {"selections", Json::array({{{"anchor", match}, {"head", match + state->query.size()}}})}}, {}, false,
            [this](const Json& result) { accept_metadata(result); reveal_caret(); status = "Match found"; }, {}});
    }
}

void EditorWindow::Impl::search_workspace(bool replace) {
    search_view = true; git_view = false; show_sidebar = true; tree_scroll = 0; replacing_workspace = replace;
    status = "Searching workspace and unsaved buffers...";
    search_results.clear(); search_operation.clear();
    ++search_generation;
    execute_policy("workbench.search", {{"query", search_query}, {"caseSensitive", search_case_sensitive},
        {"regex", search_regex}, {"wholeWord", search_whole_word}});
}

void EditorWindow::Impl::open_search_result(const Json& match) {
    if (match.value("source", std::string("disk")) == "buffer") {
        const auto id = match.at("documentId").get<std::string>();
        enqueue({"document.get", {{"id", id}, {"begin", 0}, {"end", 0}}, {}, false, [this, match, id](const Json& result) {
            accept_metadata(result); activate(id);
            if (result.at("revision") != match.at("revision")) { status = "Search result changed; search again to locate the match"; return; }
            set_selections(Json::array({{{"anchor", match.at("begin")}, {"head", match.at("end")}}}), result.at("revision").get<std::string>());
            documents.at(id).first_line = match.at("line").get<std::size_t>(); documents.at(id).needs_lines = true;
        }, {}});
        return;
    }
    start_operation("workspace.open", {{"path", match.at("path")}}, [this, match](const Json& result) {
        accept_metadata(result); activate(result.at("id").get<std::string>());
        if (result.value("dirty", false)) { status = "Disk result opened; buffer has unsaved changes"; return; }
        set_selections(Json::array({{{"anchor", match.at("begin")}, {"head", match.at("end")}}}));
        documents.at(active_id).first_line = match.at("line").get<std::size_t>();
        documents.at(active_id).needs_lines = true;
    });
}

void EditorWindow::Impl::vertical(bool down, bool extend) {
    auto* document = current();
    if (!document) { return; }
    if (document->view_revision != document->metadata.at("revision").get<std::string>()) {
        editor_command(down ? "editor.move_down" : "editor.move_up", {{"extend", extend}}); return;
    }
    auto selections = Json::array();
    const auto& positions = document->metadata.contains("selectionPositions") ? document->metadata["selectionPositions"] : document->metadata["selections"];
    std::size_t i{};
    for (const auto& selection : document->metadata.at("selections")) {
        const auto number = positions[i++].value("line", std::size_t{0});
        const auto next = down ? std::min(number + 1, document->metadata.at("lineCount").get<std::size_t>() - 1) : (number == 0 ? 0 : number - 1);
        const auto source = document->lines.find(number);
        const auto target = document->lines.find(next);
        if (source == document->lines.end() || target == document->lines.end() || !source->second.layout || !target->second.layout) {
            editor_command(down ? "editor.move_down" : "editor.move_up", {{"extend", extend}}); return;
        }
        const auto head = selection.at("head").get<std::size_t>();
        const auto x = source->second.layout->caret(std::min(head - source->second.begin, source->second.text.size())).x;
        if (!vertical_column && document->metadata.at("selections").size() == 1) { vertical_column = x; }
        const auto hit = target->second.layout->hit_test(vertical_column.value_or(x), target->second.layout->height() / 2);
        const auto offset = target->second.begin + hit.byte_offset;
        selections.push_back({{"anchor", extend ? selection.at("anchor").get<std::size_t>() : offset}, {"head", offset}});
    }
    set_selections(std::move(selections), document->view_revision);
}

void EditorWindow::Impl::key(const SDL_KeyboardEvent& input) {
#if defined(__APPLE__)
    const bool control = (input.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
#else
    const bool control = (input.mod & SDL_KMOD_CTRL) != 0;
#endif
    const bool shift = (input.mod & SDL_KMOD_SHIFT) != 0;
    const bool alt = (input.mod & SDL_KMOD_ALT) != 0;
    if (input.key == SDLK_ESCAPE) { command("ui.cancel"); return; }
    if (!preedit.empty()) { return; }
    if (prompt != Prompt::none) {
        if (prompt == Prompt::policy_confirm) {
            if (input.key == SDLK_RETURN || input.key == SDLK_KP_ENTER) { finish_dialog(true); }
            return;
        }
        if (search_prompt() && alt) {
            if (input.key == SDLK_C) { command("ui.toggleSearchCase"); return; }
            if (input.key == SDLK_R) { command("ui.toggleSearchRegex"); return; }
            if (input.key == SDLK_W) { command("ui.toggleSearchWholeWord"); return; }
        }
        if (prompt == Prompt::recovery_flushing) { return; }
        if (prompt == Prompt::recovery_exit_failed) {
            if (input.key == SDLK_RETURN) { command("ui.retryExit"); }
            else if (input.key == SDLK_E) { command("ui.exitWithoutCheckpoint"); }
            return;
        }
        if (prompt == Prompt::close) {
            if (input.key == SDLK_RETURN) { command("ui.saveAndClose"); }
            else if (input.key == SDLK_D) { command("ui.discard"); }
            return;
        }
        if (prompt == Prompt::hover || prompt == Prompt::git_diff) {
            if (prompt == Prompt::git_diff) {
                if (input.key == SDLK_S) { command("ui.gitStage"); return; }
                if (input.key == SDLK_U) { command("ui.gitUnstage"); return; }
                if (input.key == SDLK_D) { command("ui.gitDiscard"); return; }
            }
            if (input.key == SDLK_RETURN) { prompt = Prompt::none; }
            else if (input.key == SDLK_DOWN) { ++prompt_choice; }
            else if (input.key == SDLK_UP && prompt_choice != 0) { --prompt_choice; }
            return;
        }
        if (control && input.key == SDLK_A) { prompt_select_all = true; return; }
        if (input.key == SDLK_RETURN || input.key == SDLK_KP_ENTER) { accept_prompt(); }
        else if (input.key == SDLK_BACKSPACE && !prompt_text.empty()) {
            if (prompt_select_all) { prompt_text.clear(); prompt_select_all = false; }
            else {
                const auto layout = fonts.layout(prompt_text);
                const auto boundaries = layout.grapheme_boundaries();
                prompt_text.resize(boundaries[boundaries.size() - 2]);
            }
            prompt_choice = 0;
        } else if (input.key == SDLK_UP && prompt_choice != 0) { --prompt_choice; }
        else if (input.key == SDLK_DOWN) { ++prompt_choice; }
        else if (control && input.key == SDLK_V) {
            const std::unique_ptr<char, SdlStringDeleter> text(SDL_GetClipboardText());
            if (!text) { throw std::runtime_error(std::string("Clipboard read failed: ") + SDL_GetError()); }
            const std::string_view pasted(text.get());
            const auto existing=prompt_select_all ? std::size_t{0} : prompt_text.size();
            if (existing>16384 || pasted.size()>16384-existing) { error="Input exceeds its 16 KiB limit"; return; }
            if (prompt_select_all) { prompt_text.clear(); prompt_select_all = false; }
            prompt_text += pasted;
        }
        return;
    }

    std::string key_name;
    if (input.key >= SDLK_A && input.key <= SDLK_Z) { key_name.assign(1, static_cast<char>('A' + input.key - SDLK_A)); }
    else {
        switch (input.key) {
        case SDLK_UP: key_name = "Up"; break;
        case SDLK_DOWN: key_name = "Down"; break;
        case SDLK_LEFT: key_name = "Left"; break;
        case SDLK_RIGHT: key_name = "Right"; break;
        case SDLK_TAB: key_name = "Tab"; break;
        case SDLK_HOME: key_name = "Home"; break;
        case SDLK_END: key_name = "End"; break;
        case SDLK_BACKSPACE: key_name = "Backspace"; break;
        case SDLK_DELETE: key_name = "Delete"; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: key_name = "Enter"; break;
        case SDLK_F1: key_name = "F1"; break;
        case SDLK_SPACE: key_name = "Space"; break;
        case SDLK_SLASH: key_name = "/"; break;
        case SDLK_F4: key_name = "F4"; break;
        default: key_name = SDL_GetKeyName(input.key); break;
        }
    }
    const std::string token = (control ? "Ctrl+" : "") + std::string(shift ? "Shift+" : "") + (alt ? "Alt+" : "") + key_name;
    auto binding = keybindings.find(token);
    if (binding == keybindings.end() && shift) {
        const auto unextended = (control ? "Ctrl+" : "") + std::string(alt ? "Alt+" : "") + key_name;
        const auto candidate = keybindings.find(unextended);
        if (candidate != keybindings.end() && candidate->second.starts_with("editor.move_")) { binding = candidate; }
    }
    if (binding != keybindings.end()) {
        if (binding->second == "editor.move_up" || binding->second == "editor.move_down") { vertical(binding->second == "editor.move_down", shift); }
        else if (binding->second.starts_with("editor.move_")) { editor_command(binding->second, {{"extend", shift}}); }
        else { command(binding->second); }
        return;
    }
    if (alt && input.key == SDLK_F4) { command("ui.closeWindow"); return; }
    if (alt) { return; }
    std::string editing;
    switch (input.key) {
    case SDLK_LEFT: editing = control ? "move_word_left" : "move_left"; break;
    case SDLK_RIGHT: editing = control ? "move_word_right" : "move_right"; break;
    case SDLK_UP: vertical(false, shift); return;
    case SDLK_DOWN: vertical(true, shift); return;
    case SDLK_HOME: editing = control ? "move_document_start" : "move_home"; break;
    case SDLK_END: editing = control ? "move_document_end" : "move_end"; break;
    case SDLK_BACKSPACE: editing = control ? "delete_word_backward" : "backspace"; break;
    case SDLK_DELETE: editing = control ? "delete_word_forward" : "delete_forward"; break;
    case SDLK_RETURN: case SDLK_KP_ENTER: editing = "newline"; break;
    case SDLK_TAB: editing = shift ? "outdent" : "indent"; break;
    case SDLK_PAGEUP: case SDLK_PAGEDOWN: {
        auto* document = current();
        if (!document) { return; }
        const auto step = static_cast<std::size_t>(std::max(1.0F, (content_bottom - content_top) / row_height));
        document->first_line = input.key == SDLK_PAGEUP ? (document->first_line > step ? document->first_line - step : 0)
            : std::min(document->first_line + step, document->metadata.at("lineCount").get<std::size_t>() - 1);
        document->needs_lines = true;
        return;
    }
    default: return;
    }
    editor_command("editor." + editing, {{"extend", shift}});
}

void EditorWindow::Impl::point(float x, float y, bool extend, bool add) {
    auto* document = current();
    if (!document || x < left || x > right || y < content_top || y > content_bottom) { return; }
    const auto number = document->first_line + static_cast<std::size_t>((y - content_top) / row_height);
    const auto found = document->lines.find(number);
    if (found == document->lines.end() || !found->second.layout) { return; }
    const auto hit = found->second.layout->hit_test(x - left - gutter + document->horizontal,
        std::max(0.0F, y - content_top - static_cast<float>(number - document->first_line) * row_height));
    const auto offset = found->second.begin + hit.byte_offset;
    if (add) {
        const auto id = active_id;
        const auto revision = document->view_revision;
        enqueue({"document.setSelections", Json::object(), id, false,
            [this](const Json& result) { accept_metadata(result); reveal_caret(); },
            [this, id, revision, offset] {
                auto selections = documents.at(id).metadata.at("selections");
                selections.push_back({{"anchor", offset}, {"head", offset}});
                return Json{{"id", id}, {"baseRevision", revision}, {"selections", std::move(selections)}};
            }});
        vertical_column.reset(); return;
    }
    auto selections = add ? document->metadata.at("selections") : Json::array();
    const auto anchor = dragging ? drag_anchor : (extend ? document->metadata.at("selections").front().at("anchor").get<std::size_t>() : offset);
    selections.push_back({{"anchor", anchor}, {"head", offset}});
    if (!dragging) { drag_anchor = anchor; }
    vertical_column.reset();
    set_selections(std::move(selections), document->view_revision);
}

void EditorWindow::Impl::scroll_to(float y) {
    auto* document = current();
    if (!document) { return; }
    const auto count = document->metadata.at("lineCount").get<std::size_t>();
    const auto position = std::clamp((y - content_top) / std::max(1.0F, content_bottom - content_top), 0.0F, 1.0F);
    document->first_line = static_cast<std::size_t>(position * static_cast<float>(count - 1));
    document->needs_lines = true;
}

void EditorWindow::Impl::event(const SDL_Event& input) {
    int logical_width{}, logical_height{};
    if (!SDL_GetWindowSize(renderer.window(), &logical_width, &logical_height)) { throw std::runtime_error(SDL_GetError()); }
    const auto pixels = renderer.pixel_size();
    const auto ratio_x = logical_width > 0 ? static_cast<float>(pixels.width) / static_cast<float>(logical_width) : 1;
    const auto ratio_y = logical_height > 0 ? static_cast<float>(pixels.height) / static_cast<float>(logical_height) : 1;
    measure_layout();
    switch (input.type) {
    case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        command("ui.closeWindow"); break;
    case SDL_EVENT_KEY_DOWN: key(input.key); break;
    case SDL_EVENT_TEXT_EDITING:
        preedit = input.edit.text ? input.edit.text : "";
        break;
    case SDL_EVENT_TEXT_INPUT:
        preedit.clear();
        if (!input.text.text) { break; }
        if (prompt != Prompt::none && prompt != Prompt::close && prompt != Prompt::replace_confirm && prompt != Prompt::rename_confirm && prompt != Prompt::reload_confirm &&
            prompt != Prompt::hover && prompt != Prompt::git_diff && prompt != Prompt::git_discard &&
            prompt != Prompt::recovery_confirm && prompt != Prompt::recovery_flushing && prompt != Prompt::recovery_exit_failed && prompt != Prompt::policy_confirm) {
            if (prompt_select_all) { prompt_text.clear(); prompt_select_all = false; }
            if (prompt_text.size() + std::string_view(input.text.text).size() > 16384) { error = "Input exceeds its 16 KiB limit"; break; }
            prompt_text += input.text.text; prompt_choice = 0;
        } else if (prompt == Prompt::none) { editor_command("editor.insert_text", {{"text", input.text.text}}); }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouse_x = input.motion.x * ratio_x; mouse_y = input.motion.y * ratio_y;
        if (dragging_sidebar) { sidebar_width=std::clamp((mouse_x-geometry.activity.right())/scale,180.0F,480.0F); }
        else if (!dragging_tab.empty() && std::abs(mouse_x-tab_drag_start)>8*scale) {
            for (const auto& tab : tab_strip.visible) {
                if (!tab.bounds.contains(mouse_x,mouse_y)) { continue; }
                const auto found=std::ranges::find(tabs,dragging_tab);
                if (found!=tabs.end() && tabs[tab.index]!=dragging_tab) {
                    const auto id=*found; tabs.erase(found);
                    tabs.insert(tabs.begin()+static_cast<std::ptrdiff_t>(tab.index),id); reveal_tab=true;
                }
                break;
            }
        }
        else if (dragging_scroll) { scroll_to(mouse_y); }
        else if (dragging) { point(mouse_x, mouse_y, true, false); }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (input.button.button != SDL_BUTTON_LEFT) { break; }
        mouse_x = input.button.x * ratio_x; mouse_y = input.button.y * ratio_y;
        const auto width = static_cast<float>(pixels.width);
        if (prompt == Prompt::policy_confirm) {
            const auto bx = (width - std::min(600 * scale,width - 24 * scale)) / 2;
            const auto button_width = (std::min(600 * scale,width - 24 * scale) - 72 * scale) / 2;
            if (mouse_y >= 211 * scale && mouse_y < 245 * scale) {
                if (mouse_x >= bx + 24 * scale && mouse_x < bx + 24 * scale + button_width) { finish_dialog(true); }
                else if (mouse_x >= bx + 48 * scale + button_width && mouse_x < width - bx - 24 * scale) { finish_dialog(nullptr); }
            }
            break;
        }
        if (prompt == Prompt::recovery_flushing) { command("ui.cancel"); break; }
        if (prompt == Prompt::recovery_exit_failed) {
            const auto bx = (width - 640 * scale) / 2;
            if (mouse_y >= 245 * scale && mouse_y < 281 * scale) {
                if (mouse_x >= bx + 24 * scale && mouse_x < bx + 190 * scale) { command("ui.retryExit"); }
                else if (mouse_x >= bx + 218 * scale && mouse_x < bx + 400 * scale) { command("ui.cancel"); }
                else if (mouse_x >= bx + 426 * scale && mouse_x < bx + 616 * scale) { command("ui.exitWithoutCheckpoint"); }
            }
            break;
        }
        if (prompt == Prompt::recovery_confirm) {
            const auto bx = (width - 680 * scale) / 2;
            if (mouse_y >= 425 * scale && mouse_y < 461 * scale) {
                if (mouse_x >= bx + 24 * scale && mouse_x < bx + 308 * scale) { command("ui.confirmRecovery"); }
                else if (mouse_x >= bx + 370 * scale && mouse_x < bx + 654 * scale) { command("ui.cancel"); }
            }
            break;
        }
        if (prompt == Prompt::close) {
            const auto bx = (width - 520 * scale) / 2;
            const auto by = 190 * scale;
            if (mouse_y >= by && mouse_y < by + 34 * scale) {
                if (mouse_x >= bx + 24 * scale && mouse_x < bx + 160 * scale) { command("ui.saveAndClose"); }
                else if (mouse_x >= bx + 178 * scale && mouse_x < bx + 324 * scale) { command("ui.discard"); }
                else if (mouse_x >= bx + 342 * scale && mouse_x < bx + 496 * scale) { command("ui.cancel"); }
            }
            break;
        }
        if (prompt == Prompt::palette || prompt == Prompt::completion || prompt == Prompt::locations || prompt == Prompt::recovery) {
            const auto bx = (width - 600 * scale) / 2;
            if (mouse_x > bx && mouse_x < bx + 600 * scale && mouse_y >= 130 * scale) {
                const auto first = prompt_choice >= 12 ? prompt_choice - 11 : 0;
                const auto row = static_cast<std::size_t>((mouse_y - 130 * scale) / (34 * scale));
                if (row < 12) { prompt_choice = first + row; accept_prompt(); }
            }
            break;
        }
        if (prompt == Prompt::replace_confirm || prompt == Prompt::rename_confirm || prompt == Prompt::git_discard || prompt == Prompt::reload_confirm) {
            const auto bx = (width - 600 * scale) / 2;
            if (mouse_y >= 211 * scale && mouse_y < 245 * scale) {
                if (mouse_x >= bx + 24 * scale && mouse_x < bx + 260 * scale) { command(prompt == Prompt::reload_confirm ? "ui.confirmReload" : (prompt == Prompt::rename_confirm ? "ui.confirmRename" : (prompt == Prompt::git_discard ? "ui.confirmDiscard" : "ui.confirmReplace"))); }
                else if (mouse_x >= bx + 340 * scale && mouse_x < bx + 576 * scale) { command("ui.cancel"); }
            }
            break;
        }
        if (search_prompt()) {
            const auto bx = (width - 600 * scale) / 2;
            if (mouse_y >= 132 * scale && mouse_y < 158 * scale) {
                if (mouse_x >= bx + 12 * scale && mouse_x < bx + 196 * scale) { command("ui.toggleSearchCase"); }
                else if (mouse_x >= bx + 206 * scale && mouse_x < bx + 390 * scale) { command("ui.toggleSearchRegex"); }
                else if (mouse_x >= bx + 400 * scale && mouse_x < bx + 584 * scale) { command("ui.toggleSearchWholeWord"); }
            }
        }
        if (prompt != Prompt::none) { break; }
        if (!workspace_notice().empty() && mouse_x >= left + 12 * scale && mouse_x < right - 12 * scale &&
            mouse_y >= content_bottom - 142 * scale && mouse_y < content_bottom - 106 * scale) {
            command("ui.dismissWorkspaceNotice"); break;
        }
        if (recovery_available && mouse_x >= left + 12 * scale && mouse_x < right - 12 * scale &&
            mouse_y >= content_bottom - 100 * scale && mouse_y < content_bottom - 64 * scale) { command("ui.recovery"); break; }
        if (geometry.header.contains(mouse_x,mouse_y)) { command("ui.commandPalette"); break; }
        if (geometry.sidebar_handle.contains(mouse_x,mouse_y)) { dragging_sidebar=true; break; }
        if (geometry.activity.contains(mouse_x,mouse_y)) {
            const auto index=static_cast<int>((mouse_y-geometry.activity.y)/(48*scale));
            if (mouse_y>geometry.status.y-40*scale) command("ui.commandPalette");
            else if (index==1) { git_view=false; show_sidebar=true; command("ui.search"); }
            else if (index==2) command("ui.gitStatus");
            else if (index==3) command("ui.diagnostics");
            else command("ui.explorer");
            break;
        }
        if (geometry.outline_header.contains(mouse_x,mouse_y)) { command("ui.toggleOutline"); break; }
        if (geometry.sidebar.contains(mouse_x,mouse_y) && mouse_y<geometry.sidebar_rows.y) {
            const Box refresh{left-34*scale,geometry.sidebar.y+5*scale,28*scale,26*scale};
            if (refresh.contains(mouse_x,mouse_y)) {
                const auto view=git_view ? "git" : (search_view ? "search" : "explorer");
                if (busy_views.value(view,false)) execute_policy("workbench.cancelView",{{"id",view}});
                else if (git_view) command("ui.gitStatus");
                else if (search_view) search_workspace(false);
                else command("ui.refreshTree");
            } else if (!git_view && !search_view && mouse_y>=geometry.sidebar.y+32*scale) { explorer_expanded=!explorer_expanded; tree_scroll=0; }
            break;
        }
        if (geometry.outline_rows.contains(mouse_x,mouse_y)) {
            if (const auto* document=current();document && document->syntax_revision==document->view_revision) {
                const auto index=static_cast<std::size_t>((mouse_y-geometry.outline_rows.y)/geometry.sidebar_row_height);
                if (index<document->outline.size()) {
                    const auto& symbol=document->outline[index];
                    set_selections(Json::array({{{"anchor",symbol.at("selectionBegin")},{"head",symbol.at("selectionEnd")}}}),document->syntax_revision);
                }
            }
            break;
        }
        if (geometry.sidebar_rows.contains(mouse_x,mouse_y)) {
            if (!git_view && !search_view && !explorer_expanded) break;
            const auto row = tree_scroll + static_cast<std::size_t>((mouse_y-geometry.sidebar_rows.y)/geometry.sidebar_row_height);
            if (git_view) {
                const auto entries = git_status.value("entries", Json::array());
                if (row < entries.size()) {
                    const auto& entry = entries[row];
                    const auto staged = entry.at("unstaged") == "unchanged";
                    fetch_git_diff(entry.at("path").get<std::string>(), staged);
                }
            } else if (search_view) {
                if (row < search_results.size()) { open_search_result(search_results[row]); }
            } else if (row < file_tree.size()) {
                const auto path = file_tree.at(row).at("path").get<std::string>();
                if (file_tree.at(row).at("directory").get<bool>()) {
                    file_tree.toggle(row);
                } else { open_path(path); }
            }
            break;
        }
        if (geometry.tabs.contains(mouse_x,mouse_y)) {
            if (tab_strip.previous.contains(mouse_x,mouse_y)) { if (first_tab) --first_tab; reveal_tab=false; }
            else if (tab_strip.next.contains(mouse_x,mouse_y)) { if (first_tab+1<tabs.size()) ++first_tab; reveal_tab=false; }
            else for (const auto& tab : tab_strip.visible) {
                if (!tab.bounds.contains(mouse_x,mouse_y)) continue;
                const auto id=tabs[tab.index]; activate(id);
                if (tab.close.contains(mouse_x,mouse_y)) { close_document(id); }
                else { dragging_tab=id; tab_drag_start=mouse_x; }
                break;
            }
            break;
        }
        if (mouse_x >= right - 12 * scale && mouse_x < right && mouse_y >= content_top && mouse_y <= content_bottom) {
            dragging_scroll = true; scroll_to(mouse_y); break;
        }
        const auto modifiers = SDL_GetModState();
        point(mouse_x, mouse_y, (modifiers & SDL_KMOD_SHIFT) != 0, (modifiers & SDL_KMOD_ALT) != 0);
        if (mouse_x >= left && mouse_x <= right && mouse_y >= content_top && mouse_y <= content_bottom) {
            dragging = (modifiers & SDL_KMOD_ALT) == 0;
            if (input.button.clicks >= 2) { editor_command("editor.select_word"); dragging = false; }
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (input.button.button == SDL_BUTTON_LEFT) { dragging=false; dragging_scroll=false; dragging_sidebar=false; dragging_tab.clear(); }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        const auto direction = input.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
        if (prompt != Prompt::none) {
            const auto shift = static_cast<long long>(std::round(input.wheel.y * direction * 3));
            prompt_choice = static_cast<std::size_t>(std::max(0LL, static_cast<long long>(prompt_choice) - shift));
            break;
        }
        if (geometry.tabs.contains(mouse_x,mouse_y)) {
            const auto direction_step=input.wheel.y*direction<0 ? 1 : -1;
            first_tab=static_cast<std::size_t>(std::clamp(static_cast<long long>(first_tab)+direction_step,0LL,static_cast<long long>(tabs.empty() ? 0 : tabs.size()-1))); reveal_tab=false;
        } else if (geometry.sidebar.contains(mouse_x,mouse_y)) {
            const auto shift = static_cast<long long>(std::round(input.wheel.y * direction * 3));
            tree_scroll = static_cast<std::size_t>(std::max(0LL, static_cast<long long>(tree_scroll) - shift));
        } else if (auto* document = current()) {
            if ((SDL_GetModState() & SDL_KMOD_SHIFT) != 0) {
                document->horizontal = std::max(0.0F, document->horizontal - input.wheel.y * direction * 40 * scale);
            } else {
                const auto shift = static_cast<long long>(std::round(input.wheel.y * direction * 3));
                document->first_line = std::min(static_cast<std::size_t>(std::max(0LL, static_cast<long long>(document->first_line) - shift)),
                    document->metadata.at("lineCount").get<std::size_t>() - 1);
                document->needs_lines = true;
                document->horizontal = std::max(0.0F, document->horizontal - input.wheel.x * direction * 40 * scale);
            }
        }
        break;
    }
    case SDL_EVENT_DROP_FILE:
        if (input.drop.data) { open_path(input.drop.data); }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        labels.clear();
        for (auto& [id, document] : documents) {
            static_cast<void>(id); document.needs_lines = true;
            for (auto& [number, line] : document.lines) { static_cast<void>(number); line.layout.reset(); }
        }
        break;
    default: break;
    }
}

void EditorWindow::Impl::rectangle(SkCanvas* canvas, float x, float y, float width, float height, SkColor color) {
    SkPaint paint; paint.setColor(color);
    canvas->drawRect(SkRect::MakeXYWH(x, y, width, height), paint);
}

void EditorWindow::Impl::label(SkCanvas* canvas, std::string_view text, float x, float y, float size, SkColor color) {
    if (text.empty()) { return; }
    label_layout(text, size, color).draw(canvas, x, y);
}

const TextLayout& EditorWindow::Impl::label_layout(std::string_view text, float size, SkColor color) {
    size=size*ui_font_size/13.0F;
    if (color == 0) { color = foreground; }
    std::string display(text);
    std::ranges::replace(display, '\n', ' ');
    std::ranges::replace(display, '\r', ' ');
    const auto cache_key = display + '\0' + std::to_string(size) + ':' + std::to_string(color) + ':' + std::to_string(scale);
    if (labels.size() > 1024) { labels.clear(); }
    auto found = labels.find(cache_key);
    if (found == labels.end()) {
        TextLayoutOptions style; style.font_size = size; style.dpi_scale = scale; style.color = color; style.line_height = 1.15F;
        style.font_families = {"Segoe UI", "Microsoft YaHei UI", "Segoe UI Emoji"};
        found = labels.emplace(cache_key, fonts.layout(std::move(display), style)).first;
    }
    return found->second;
}

void EditorWindow::Impl::elided_label(SkCanvas* canvas,std::string_view text,float x,float y,float size,float available,SkColor color) {
    if (text.empty() || available<=0) return;
    const auto& full=label_layout(text,size,color);
    if (full.width()<=available) { full.draw(canvas,x,y); return; }
    const auto points=full.grapheme_boundaries();
    const std::vector<std::size_t> boundaries(points.begin(),points.end());
    if (label_layout("…",size,color).width()>available) return;
    std::size_t low{},high=boundaries.size()-1;
    while (low<high) {
        const auto middle=(low+high+1)/2;
        const auto candidate=std::string(text.substr(0,boundaries[middle]))+"…";
        if (label_layout(candidate,size,color).width()<=available) low=middle;
        else high=middle-1;
    }
    label(canvas,std::string(text.substr(0,boundaries[low]))+"…",x,y,size,color);
}

void EditorWindow::Impl::measure_layout() {
    const auto pixels = renderer.pixel_size();
    scale = renderer.dpi_scale();
    geometry = WorkbenchLayout::measure(static_cast<float>(pixels.width), static_cast<float>(pixels.height), scale,
        show_sidebar, sidebar_width, show_outline, !search_view && !git_view);
    left = geometry.editor.x; right = geometry.editor.right();
    content_top = geometry.editor.y; content_bottom = geometry.editor.bottom();
    std::vector<float> widths;
    for (const auto& id : tabs) {
        const auto& path = documents.at(id).path;
        widths.push_back(label_layout(path.empty() ? "Untitled" : basename(path), 13, foreground).width());
    }
    const auto active = std::ranges::find(tabs, active_id);
    const auto index = active == tabs.end() ? 0 : static_cast<std::size_t>(active-tabs.begin());
    tab_strip = TabStrip::measure(geometry.tabs, scale, widths, first_tab, index, reveal_tab);
    first_tab = tab_strip.first; reveal_tab = false;
}

void EditorWindow::Impl::draw_prompt(SkCanvas* canvas, float width, float height) {
    if (prompt == Prompt::none) { return; }
    rectangle(canvas, 0, 0, width, height, SkColorSetA(background, 160));
    if (prompt == Prompt::policy_confirm && dialog) {
        const auto panel_width = std::min(600 * scale,width - 24 * scale);
        const auto x = (width-panel_width)/2;
        rectangle(canvas,x,90*scale,panel_width,175*scale,panel_background);
        rectangle(canvas,x,90*scale,panel_width,2*scale,accent);
        elided_label(canvas,dialog->spec.at("title").get<std::string>(),x+24*scale,108*scale,16,panel_width-48*scale,foreground);
        TextLayoutOptions style; style.font_size=13; style.dpi_scale=scale; style.color=muted; style.wrap_width=panel_width-48*scale;
        if(!dialog->detail_layout || dialog->detail_width!=style.wrap_width){
            dialog->detail_layout=fonts.layout(dialog->spec.value("detail",std::string{}),style);dialog->detail_width=style.wrap_width;
        }
        canvas->save(); canvas->clipRect(SkRect::MakeXYWH(x+24*scale,145*scale,panel_width-48*scale,58*scale));
        dialog->detail_layout->draw(canvas,x+24*scale,145*scale);canvas->restore();
        const auto button_width=(panel_width-72*scale)/2;
        rectangle(canvas,x+24*scale,211*scale,button_width,34*scale,accent);
        rectangle(canvas,x+48*scale+button_width,211*scale,button_width,34*scale,border_color);
        elided_label(canvas,dialog->spec.value("accept",std::string("Confirm")),x+40*scale,219*scale,13,button_width-32*scale,foreground);
        label(canvas,"Cancel",x+64*scale+button_width,219*scale,13);
        return;
    }
    if (prompt == Prompt::policy_input && dialog) {
        const auto panel_width=std::min(600*scale,width-24*scale),x=(width-panel_width)/2;
        rectangle(canvas,x,64*scale,panel_width,76*scale,panel_background);
        rectangle(canvas,x,64*scale,panel_width,2*scale,accent);
        elided_label(canvas,dialog->spec.at("title").get<std::string>(),x+16*scale,73*scale,10,panel_width-32*scale,muted);
        rectangle(canvas,x+12*scale,94*scale,panel_width-24*scale,30*scale,input_color);
        if(prompt_select_all&&!prompt_text.empty())rectangle(canvas,x+20*scale,97*scale,panel_width-40*scale,25*scale,selection_color);
        TextLayoutOptions style;style.font_size=14;style.dpi_scale=scale;style.color=foreground;
        if(!dialog->input_layout || dialog->input_text!=prompt_text || dialog->input_scale!=scale){
            dialog->input_layout=fonts.layout(prompt_text,style);dialog->input_text=prompt_text;dialog->input_scale=scale;
        }
        const auto& input=*dialog->input_layout;
        const auto horizontal=std::max(0.0F,input.width()-(panel_width-64*scale));
        const auto input_x=x+22*scale+input.width()-horizontal;
        canvas->save();canvas->clipRect(SkRect::MakeXYWH(x+20*scale,97*scale,panel_width-40*scale,27*scale));
        if(prompt_text.empty())label(canvas,dialog->spec.value("placeholder",std::string("Type text, then press Enter")),x+22*scale,100*scale,14,muted);
        else input.draw(canvas,x+22*scale-horizontal,100*scale);
        if(!preedit.empty()){const auto composition=fonts.layout(preedit,style);composition.draw(canvas,input_x,100*scale);rectangle(canvas,input_x,120*scale,composition.width(),scale,accent);}
        else rectangle(canvas,input_x,100*scale,scale,20*scale,cursor_color);
        canvas->restore();
        int logical_width{},logical_height{};
        if(SDL_GetWindowSize(renderer.window(),&logical_width,&logical_height)&&logical_width>0&&logical_height>0){
            renderer.set_text_input_area({static_cast<int>(input_x*static_cast<float>(logical_width)/width),static_cast<int>(98*scale*static_cast<float>(logical_height)/height),2,static_cast<int>(26*scale*static_cast<float>(logical_height)/height)},0);
        }
        return;
    }
    if (prompt == Prompt::recovery_flushing) {
        const auto x = (width - 520 * scale) / 2;
        rectangle(canvas, x, 90 * scale, 520 * scale, 130 * scale, panel_background);
        label(canvas, "Saving recovery checkpoint before exit...", x + 24 * scale, 115 * scale, 17);
        label(canvas, "The window will close after durable storage succeeds.", x + 24 * scale, 153 * scale, 13, muted);
        label(canvas, "Esc or click to keep this window open", x + 24 * scale, 186 * scale, 12, accent);
        return;
    }
    if (prompt == Prompt::recovery_exit_failed) {
        const auto x = (width - 640 * scale) / 2;
        rectangle(canvas, x, 90 * scale, 640 * scale, 210 * scale, panel_background);
        label(canvas, "Recovery checkpoint failed — window kept open", x + 24 * scale, 110 * scale, 17, warning_color);
        TextLayoutOptions style; style.font_size = 13; style.dpi_scale = scale; style.color = foreground; style.wrap_width = 590 * scale;
        auto message = fonts.layout(error.empty() ? "The service could not persist the recovery state." : error, style);
        canvas->save(); canvas->clipRect(SkRect::MakeLTRB(x + 24 * scale, 147 * scale, x + 616 * scale, 205 * scale));
        message.draw(canvas, x + 24 * scale, 147 * scale); canvas->restore();
        label(canvas, "Exit anyway may leave recovery state incomplete.", x + 24 * scale, 216 * scale, 13, warning_color);
        rectangle(canvas, x + 24 * scale, 245 * scale, 166 * scale, 36 * scale, accent);
        rectangle(canvas, x + 218 * scale, 245 * scale, 182 * scale, 36 * scale, border_color);
        rectangle(canvas, x + 426 * scale, 245 * scale, 190 * scale, 36 * scale, error_background);
        label(canvas, "Retry (Enter)", x + 48 * scale, 253 * scale, 13);
        label(canvas, "Keep open (Esc)", x + 239 * scale, 253 * scale, 13);
        label(canvas, "Exit anyway (E)", x + 449 * scale, 253 * scale, 13);
        return;
    }
    if (prompt == Prompt::recovery_confirm) {
        const auto x = (width - 680 * scale) / 2;
        rectangle(canvas, x, 90 * scale, 680 * scale, 390 * scale, panel_background);
        label(canvas, "Restore unsaved buffers from this session?", x + 24 * scale, 110 * scale, 17);
        label(canvas, "Recovered buffers stay unsaved. Review and save them afterward.", x + 24 * scale, 146 * scale, 13, muted);
        std::vector<std::string> entries;
        for (const auto& buffer : recovery_selected.value("buffers", Json::array())) {
            entries.push_back(buffer.value("path", std::string("Untitled")) + "  (" + std::to_string(buffer.value("bytes", std::size_t{})) + " bytes)");
        }
        for (const auto& issue : recovery_selected.value("issues", Json::array())) {
            entries.push_back("Issue: " + issue.value("path", std::string{}) + " — " + issue.value("message", std::string{}));
        }
        const auto first = entries.empty() ? 0 : std::min(prompt_choice, entries.size() - 1);
        canvas->save(); canvas->clipRect(SkRect::MakeLTRB(x + 24 * scale, 182 * scale, x + 654 * scale, 409 * scale));
        for (std::size_t i = first; i < std::min(first + 8, entries.size()); ++i) {
            label(canvas, entries[i], x + 24 * scale, (185 + static_cast<float>(i - first) * 28) * scale, 13,
                  entries[i].starts_with("Issue:") ? warning_color : foreground);
        }
        if (entries.empty()) { label(canvas, "This session contains no recoverable buffers.", x + 24 * scale, 185 * scale, 13, muted); }
        canvas->restore();
        rectangle(canvas, x + 24 * scale, 425 * scale, 284 * scale, 36 * scale, accent);
        rectangle(canvas, x + 370 * scale, 425 * scale, 284 * scale, 36 * scale, border_color);
        label(canvas, "Restore session", x + 97 * scale, 433 * scale, 13);
        label(canvas, "Cancel", x + 481 * scale, 433 * scale, 13);
        return;
    }
    if (prompt == Prompt::close) {
        const auto x = (width - 520 * scale) / 2;
        rectangle(canvas, x, 90 * scale, 520 * scale, 154 * scale, panel_background);
        rectangle(canvas, x, 90 * scale, 520 * scale, 2 * scale, accent);
        label(canvas, "Save changes before closing?", x + 24 * scale, 110 * scale, 18);
        const auto found = documents.find(prompt_document);
        const auto name = found == documents.end() || found->second.path.empty() ? "Untitled" : basename(found->second.path);
        label(canvas, name + " has unsaved edits.", x + 24 * scale, 147 * scale, 13, muted);
        rectangle(canvas, x + 24 * scale, 190 * scale, 136 * scale, 34 * scale, accent);
        rectangle(canvas, x + 178 * scale, 190 * scale, 146 * scale, 34 * scale, border_color);
        rectangle(canvas, x + 342 * scale, 190 * scale, 154 * scale, 34 * scale, border_color);
        label(canvas, "Save", x + 72 * scale, 197 * scale, 13);
        label(canvas, "Don't save", x + 213 * scale, 197 * scale, 13);
        label(canvas, "Cancel", x + 398 * scale, 197 * scale, 13);
        return;
    }
    if (prompt == Prompt::hover || prompt == Prompt::git_diff) {
        const auto x = (width - 680 * scale) / 2;
        const auto panel_bottom = std::min(height - 70 * scale, 610 * scale);
        rectangle(canvas, x, 70 * scale, 680 * scale, panel_bottom - 70 * scale, panel_background);
        label(canvas, prompt == Prompt::git_diff ? "DIFF — " + git_path + (git_staged ? " [staged]" : " [working tree]") : "LANGUAGE INFORMATION — Esc to close · arrows to scroll", x + 18 * scale, 83 * scale, 11, muted);
        if (prompt == Prompt::git_diff) { label(canvas, "S: Stage  ·  U: Unstage  ·  D: Discard  ·  Esc: Close", x + 18 * scale, 100 * scale, 10, accent); }
        canvas->save(); canvas->clipRect(SkRect::MakeLTRB(x + 14 * scale, 115 * scale, x + 666 * scale, panel_bottom - 12 * scale));
        std::size_t begin{}; std::size_t line{}; float y = 115 * scale;
        while (begin <= hover_text.size()) {
            auto end = hover_text.find('\n', begin);
            if (end == std::string::npos) { end = hover_text.size(); }
            auto part = hover_text.substr(begin, end - begin);
            if (!part.empty() && part.back() == '\r') { part.pop_back(); }
            if (line++ >= prompt_choice) {
                TextLayoutOptions style; style.font_size = 14; style.dpi_scale = scale; style.color = foreground; style.wrap_width = 630 * scale;
                if (prompt == Prompt::git_diff) {
                    if (part.starts_with('+') && !part.starts_with("+++")) { style.color = added_color; }
                    else if (part.starts_with('-') && !part.starts_with("---")) { style.color = error_color; }
                    else if (part.starts_with("@@")) { style.color = accent; }
                }
                auto layout = fonts.layout(std::move(part), style);
                layout.draw(canvas, x + 22 * scale, y); y += layout.height() + 5 * scale;
            }
            if (end == hover_text.size() || y > panel_bottom) { break; }
            begin = end + 1;
        }
        canvas->restore(); return;
    }
    if (prompt == Prompt::replace_confirm || prompt == Prompt::rename_confirm || prompt == Prompt::git_discard || prompt == Prompt::reload_confirm) {
        const auto x = (width - 600 * scale) / 2;
        const bool rename = prompt == Prompt::rename_confirm;
        const bool discard = prompt == Prompt::git_discard;
        const bool reload = prompt == Prompt::reload_confirm;
        const auto extra = rename ? 220 * scale : 0;
        rectangle(canvas, x, 90 * scale, 600 * scale, 175 * scale + extra, panel_background);
        rectangle(canvas, x, 90 * scale, 600 * scale, 2 * scale, accent);
        label(canvas, reload ? "Reload from disk and replace unsaved edits?" : (rename ? "Apply symbol rename?" : (discard ? "Discard the previewed working tree change?" : "Replace " + std::to_string(search_results.size()) + " workspace matches?")), x + 24 * scale, 108 * scale, 17);
        label(canvas, reload ? "The disk version will replace this buffer's unsaved content." : (rename ? std::to_string(rename_plan.at("documents").size()) + " file(s) will become edited buffers" : (discard ? git_path : "Find: " + search_query)), x + 24 * scale, 146 * scale, 13, muted);
        label(canvas, reload ? "Cancel to preserve the current buffer." : (rename ? "Review files below. Save afterward to persist changes." : (discard ? "This removes changes from disk. It cannot be undone here." : "Replace with: " + replacement_text)), x + 24 * scale, 172 * scale, 13, muted);
        rectangle(canvas, x + 24 * scale, 211 * scale, 236 * scale, 34 * scale, accent);
        rectangle(canvas, x + 340 * scale, 211 * scale, 236 * scale, 34 * scale, border_color);
        label(canvas, reload ? "Reload file" : (rename ? "Apply rename" : (discard ? "Discard changes" : "Replace matches")), x + 85 * scale, 218 * scale, 13);
        label(canvas, "Cancel", x + 432 * scale, 218 * scale, 13);
        if (rename) {
            const auto& files = rename_plan.at("documents");
            const auto first = std::min(prompt_choice, files.size() - 1);
            canvas->save(); canvas->clipRect(SkRect::MakeLTRB(x + 20 * scale, 260 * scale, x + 580 * scale, 475 * scale));
            for (std::size_t i = first; i < std::min(first + 7, files.size()); ++i) {
                label(canvas, files[i].at("path").get<std::string>() + "  (" + std::to_string(files[i].at("editCount").get<std::size_t>()) + " edits)",
                      x + 24 * scale, (265 + static_cast<float>(i - first) * 28) * scale, 13);
            }
            canvas->restore();
        }
        return;
    }
    const auto x = (width - 600 * scale) / 2;
    const auto matches = filtered_commands();
    const auto language_matches = filtered_language_items();
    const auto recovery_matches = filtered_recovery_sessions();
    const bool language_list = prompt == Prompt::completion || prompt == Prompt::locations;
    const auto total = prompt == Prompt::palette ? matches.size() : (language_list ? language_matches.size() :
        (prompt == Prompt::recovery ? recovery_matches.size() : std::size_t{0}));
    if (total != 0) { prompt_choice = std::min(prompt_choice, total - 1); }
    const auto first = prompt_choice >= 12 ? prompt_choice - 11 : 0;
    const auto count = std::min(total, std::size_t{12});
    rectangle(canvas, x, 64 * scale, 600 * scale, (76 + static_cast<float>(count) * 34 + (search_prompt() ? 32 : 0)) * scale, panel_background);
    rectangle(canvas, x, 64 * scale, 600 * scale, 2 * scale, accent);
    const char* hint = "SAVE AS — WORKSPACE RELATIVE PATH";
    if (prompt == Prompt::palette) { hint = "COMMAND PALETTE"; }
    else if (prompt == Prompt::open) { hint = "OPEN WORKSPACE FILE"; }
    else if (prompt == Prompt::find) { hint = "FIND NEXT IN DOCUMENT — LITERAL TEXT"; }
    else if (prompt == Prompt::replace_query) { hint = "REPLACE ALL IN DOCUMENT — FIND TEXT"; }
    else if (prompt == Prompt::replace_value || prompt == Prompt::workspace_replace_value) { hint = "REPLACEMENT TEXT — ENTER TO CONTINUE"; }
    else if (prompt == Prompt::search) { hint = "SEARCH WORKSPACE AND UNSAVED BUFFERS"; }
    else if (prompt == Prompt::workspace_replace_query) { hint = "REPLACE ACROSS WORKSPACE — FIND TEXT"; }
    else if (prompt == Prompt::completion) { hint = "COMPLETION — TYPE TO FILTER · ENTER TO INSERT"; }
    else if (prompt == Prompt::locations) { hint = "LANGUAGE LOCATIONS — ENTER TO OPEN"; }
    else if (prompt == Prompt::rename) { hint = "RENAME SYMBOL — NEW NAME"; }
    else if (prompt == Prompt::git_commit) { hint = "COMMIT STAGED CHANGES — COMMIT MESSAGE"; }
    else if (prompt == Prompt::recovery) { hint = "RECOVERY — SELECT A SESSION TO REVIEW · ESC TO KEEP EDITING"; }
    label(canvas, prompt == Prompt::policy_input && dialog ? dialog->spec.at("title").get<std::string>() : hint, x + 16 * scale, 73 * scale, 10, muted);
    rectangle(canvas, x + 12 * scale, 94 * scale, 576 * scale, 30 * scale, background);
    if (prompt_select_all && !prompt_text.empty()) { rectangle(canvas, x + 20 * scale, 97 * scale, 560 * scale, 25 * scale, selection_color); }
    const auto placeholder = prompt == Prompt::palette ? "Type a command..." : (prompt == Prompt::recovery ?
        (recovery_list_loading ? "Looking for sessions..." : (total == 0 ? "No abandoned sessions found" : "Filter by session or file path")) : "Type text, then press Enter");
    label(canvas, prompt_text.empty() ? placeholder : prompt_text,
          x + 22 * scale, 100 * scale, 14, prompt_text.empty() ? muted : foreground);
    if (search_prompt()) {
        const bool selected[] = {search_case_sensitive, search_regex, search_whole_word};
        const char* titles[] = {"Match case (Alt+C)", "Regex (Alt+R)", "Whole word (Alt+W)"};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto bx = x + (12 + static_cast<float>(i) * 194) * scale;
            rectangle(canvas, bx, 132 * scale, 184 * scale, 26 * scale, selected[i] ? accent : border_color);
            label(canvas, titles[i], bx + 10 * scale, 138 * scale, 11, selected[i] ? foreground : muted);
        }
    }
    TextLayoutOptions input_style; input_style.font_size = 14; input_style.dpi_scale = scale; input_style.color = foreground;
    const auto input_layout = fonts.layout(prompt_text, input_style);
    const auto input_x = std::min(x + 570 * scale, x + 22 * scale + input_layout.width());
    if (!preedit.empty()) {
        const auto composition = fonts.layout(preedit, input_style);
        composition.draw(canvas, input_x, 100 * scale);
        rectangle(canvas, input_x, 120 * scale, composition.width(), scale, accent);
    }
    int logical_width{}, logical_height{};
    if (SDL_GetWindowSize(renderer.window(), &logical_width, &logical_height) && logical_width > 0 && logical_height > 0) {
        const auto rx = width / static_cast<float>(logical_width);
        const auto ry = height / static_cast<float>(logical_height);
        renderer.set_text_input_area({static_cast<int>(input_x / rx), static_cast<int>(98 * scale / ry), 2, static_cast<int>(26 * scale / ry)}, 0);
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto y = (130 + static_cast<float>(i) * 34) * scale;
        if (first + i == prompt_choice) { rectangle(canvas, x + 6 * scale, y, 588 * scale, 32 * scale, hover_color); }
        canvas->save(); canvas->clipRect(SkRect::MakeLTRB(x + 12 * scale, y, x + 588 * scale, y + 33 * scale));
        if (language_list) {
            const auto& item = language_items[language_matches[first + i]];
            const auto title = prompt == Prompt::completion ? item.value("label", std::string{})
                : item.value("path", item.value("uri", std::string{})) + ":" + std::to_string(item.value("line", 0U) + 1);
            label(canvas, title, x + 20 * scale, y + 7 * scale, 13);
        } else if (prompt == Prompt::recovery) {
            const auto& session = recovery_sessions[recovery_matches[first + i]];
            label(canvas, session.at("id").get<std::string>() + " — " + std::to_string(session.value("buffers", Json::array()).size()) + " unsaved buffers",
                  x + 20 * scale, y + 7 * scale, 13);
        } else {
            label(canvas, matches[first + i]->label, x + 20 * scale, y + 7 * scale, 13);
            label(canvas, matches[first + i]->shortcut, x + 442 * scale, y + 7 * scale, 11, muted);
        }
        canvas->restore();
    }
}

void EditorWindow::Impl::draw() {
    auto* canvas = renderer.begin_frame();
    if (!canvas) { return; }
    const auto pixels = renderer.pixel_size();
    const auto width = static_cast<float>(pixels.width);
    const auto height = static_cast<float>(pixels.height);
    scale = renderer.dpi_scale();
    row_height = std::ceil(options.font_size * scale * std::clamp(editor_settings.value("lineHeight", 1.4F), 1.0F, 4.0F) + 3 * scale);
    measure_layout();
    gutter = 60 * scale;
    canvas->clear(background);
    const auto fill = [&](const Box& box, SkColor color) { rectangle(canvas,box.x,box.y,box.width,box.height,color); };
    const auto file_icon = [&](std::string_view path, float x, float y, float size) {
        const auto dot = path.find_last_of('.');
        const auto extension = dot == std::string_view::npos ? std::string_view{} : path.substr(dot);
        const bool code = extension==".cpp" || extension==".hpp" || extension==".c" || extension==".h";
        const bool script = extension==".js" || extension==".ts" || extension==".json";
        draw_icon(canvas,code ? Icon::code : (script ? Icon::braces : Icon::document),x,y,size,
                  code ? code_icon_color : (script ? script_icon_color : muted));
    };
    fill(geometry.header, activity_background);
    rectangle(canvas,0,geometry.header.bottom()-scale,width,scale,border_color);
    label(canvas,"Astra",14*scale,8*scale,12,muted);
    const auto search_width = std::min(480*scale,width*0.44F);
    const auto search_x = (width-search_width)/2;
    SkPaint search_paint; search_paint.setColor(input_color); search_paint.setAntiAlias(true);
    canvas->drawRoundRect(SkRect::MakeXYWH(search_x,5*scale,search_width,22*scale),4*scale,4*scale,search_paint);
    draw_icon(canvas,Icon::search,search_x+9*scale,8*scale,15*scale,muted);
    canvas->save(); canvas->clipRect(SkRect::MakeXYWH(search_x+30*scale,5*scale,search_width-115*scale,22*scale));
    label(canvas,options.workspace_name,search_x+31*scale,8*scale,12,foreground);
    canvas->restore();
    if (search_width>260*scale) label(canvas,"Ctrl+Shift+P",search_x+search_width-85*scale,9*scale,10,muted);
    fill(geometry.activity,activity_background);
    const auto selected_activity = git_view ? 2 : (search_view ? 1 : 0);
    if (geometry.sidebar.width>0) rectangle(canvas,0,geometry.header.bottom()+static_cast<float>(selected_activity)*48*scale,2*scale,48*scale,accent);
    const Icon activity_icons[]={Icon::files,Icon::search,Icon::branch,Icon::braces};
    for (int index=0;index<4;++index) draw_icon(canvas,activity_icons[index],10*scale,geometry.header.bottom()+(13+static_cast<float>(index)*48)*scale,24*scale,
                  selected_activity==index ? foreground : muted);
    draw_icon(canvas,Icon::settings,10*scale,geometry.status.y-32*scale,22*scale,muted);
    rectangle(canvas,geometry.activity.right()-scale,geometry.activity.y,scale,geometry.activity.height,border_color);
    if (geometry.sidebar.width>0) {
        fill(geometry.sidebar,sidebar_background);
        const auto sx=geometry.sidebar.x;
        label(canvas,git_view ? "SOURCE CONTROL" : (search_view ? "SEARCH" : "EXPLORER"),sx+16*scale,geometry.sidebar.y+12*scale,11,foreground);
        const auto busy=busy_views.value(git_view ? "git" : (search_view ? "search" : "explorer"),false);
        const Box refresh{left-34*scale,geometry.sidebar.y+5*scale,28*scale,26*scale};
        draw_icon(canvas,busy && refresh.contains(mouse_x,mouse_y) ? Icon::close : Icon::refresh,left-29*scale,geometry.sidebar.y+9*scale,16*scale,busy ? accent : muted);
        rectangle(canvas,sx,geometry.sidebar.y+32*scale,geometry.sidebar.width,24*scale,panel_background);
        if (!git_view && !search_view) draw_icon(canvas,explorer_expanded ? Icon::chevron_down : Icon::chevron_right,sx+6*scale,geometry.sidebar.y+36*scale,16*scale,muted);
        canvas->save(); canvas->clipRect(SkRect::MakeXYWH(sx+22*scale,geometry.sidebar.y+32*scale,geometry.sidebar.width-30*scale,24*scale));
        label(canvas,git_view ? git_status.value("branch",std::string("Loading repository...")) :
              (search_view ? std::to_string(search_results.size())+" matches: "+search_query : options.workspace_name),
              sx+24*scale,geometry.sidebar.y+37*scale,12,foreground);
        canvas->restore();
        const Json empty_entries=Json::array();
        const auto& git_entries=git_status.contains("entries") ? git_status.at("entries") : empty_entries;
        const auto count=git_view ? git_entries.size() : (search_view ? search_results.size() : (explorer_expanded ? file_tree.size() : 0));
        if (tree_scroll>=count) tree_scroll=count ? count-1 : 0;
        const auto visible_count=static_cast<std::size_t>(std::ceil(geometry.sidebar_rows.height/geometry.sidebar_row_height))+1;
        const auto end=std::min(count,tree_scroll+visible_count);
        std::vector<const Json*> rows;
        if (!git_view && !search_view) file_tree.for_each_range(tree_scroll,end,[&](std::size_t,const Json& row){rows.push_back(&row);});
        canvas->save(); canvas->clipRect(SkRect::MakeXYWH(geometry.sidebar_rows.x,geometry.sidebar_rows.y,geometry.sidebar_rows.width,geometry.sidebar_rows.height));
        for (auto i=tree_scroll;i<end;++i) {
            const auto y=geometry.sidebar_rows.y+static_cast<float>(i-tree_scroll)*geometry.sidebar_row_height;
            const auto& item=git_view ? git_entries[i] : (search_view ? search_results[i] : *rows[i-tree_scroll]);
            const auto path=item.at("path").get<std::string>();
            if (geometry.sidebar_rows.contains(mouse_x,mouse_y) && mouse_y>=y && mouse_y<y+geometry.sidebar_row_height)
                rectangle(canvas,sx,y,geometry.sidebar.width,geometry.sidebar_row_height,hover_color);
            if (git_view) {
                const auto change=item.at("unstaged")!="unchanged" ? item.at("unstaged").get<std::string>() : item.at("staged").get<std::string>();
                file_icon(path,sx+13*scale,y+4*scale,16*scale);
                label(canvas,basename(path),sx+38*scale,y+4*scale,13,foreground);
                const auto mark=change=="untracked" ? "U" : (change.empty() ? " " : std::string(1,static_cast<char>(std::toupper(static_cast<unsigned char>(change.front())))));
                label(canvas,mark,left-25*scale,y+4*scale,12,item.value("conflict",false) ? error_color : added_color);
            } else if (search_view) {
                file_icon(path,sx+13*scale,y+4*scale,16*scale);
                label(canvas,basename(path)+":"+std::to_string(item.at("line").get<std::size_t>()+1),sx+38*scale,y+4*scale,13);
            } else {
                const auto depth=static_cast<float>(std::count(path.begin(),path.end(),'/')+std::count(path.begin(),path.end(),'\\'));
                const auto x=sx+(8+12*depth)*scale;
                if (const auto* document=current();document && document->path==path)
                    rectangle(canvas,sx,y,geometry.sidebar.width,geometry.sidebar_row_height,selection_color);
                if (item.at("directory").get<bool>()) {
                    draw_icon(canvas,file_tree.collapsed(path) ? Icon::chevron_right : Icon::chevron_down,x,y+4*scale,16*scale,muted);
                    draw_icon(canvas,Icon::folder,x+18*scale,y+4*scale,16*scale,folder_color);
                } else file_icon(path,x+18*scale,y+4*scale,16*scale);
                label(canvas,basename(path),x+42*scale,y+4*scale,13,foreground);
            }
        }
        if (!count && (git_view || search_view || explorer_expanded)) label(canvas,git_view ? "No changes" : (search_view ? "No search results" : "No files in this workspace"),sx+16*scale,geometry.sidebar_rows.y+12*scale,12,muted);
        canvas->restore();
        if (geometry.outline_header.height>0) {
            fill(geometry.outline_header,panel_background);
            draw_icon(canvas,show_outline ? Icon::chevron_down : Icon::chevron_right,sx+6*scale,geometry.outline_header.y+5*scale,16*scale,muted);
            label(canvas,"OUTLINE",sx+27*scale,geometry.outline_header.y+7*scale,11,foreground);
        }
        rectangle(canvas,left-scale,geometry.sidebar.y,scale,geometry.sidebar.height,border_color);
    }
    fill(geometry.tabs,panel_background);
    for (const auto& tab : tab_strip.visible) {
        const auto& document=documents.at(tabs[tab.index]);
        const bool active=tabs[tab.index]==active_id;
        if (active) {
            fill(tab.bounds,active_tab_background);
            rectangle(canvas,tab.bounds.x,tab.bounds.y,tab.bounds.width,2*scale,accent);
        }
        rectangle(canvas,tab.bounds.right()-scale,tab.bounds.y,scale,tab.bounds.height,border_color);
        file_icon(document.path,tab.bounds.x+12*scale,tab.bounds.y+9*scale,16*scale);
        canvas->save(); canvas->clipRect(SkRect::MakeXYWH(tab.bounds.x+34*scale,tab.bounds.y,tab.bounds.width-62*scale,tab.bounds.height));
        elided_label(canvas,document.path.empty() ? "Untitled" : basename(document.path),tab.bounds.x+34*scale,tab.bounds.y+10*scale,13,tab.bounds.width-62*scale,active ? foreground : muted);
        canvas->restore();
        if (document.metadata.value("dirty",false)) {
            SkPaint dirty; dirty.setColor(active ? foreground : muted); dirty.setAntiAlias(true);
            canvas->drawCircle(tab.close.x+13*scale,tab.close.y+17*scale,3*scale,dirty);
        } else draw_icon(canvas,Icon::close,tab.close.x+6*scale,tab.close.y+10*scale,14*scale,muted);
    }
    if (tab_strip.previous.width>0) {
        fill(tab_strip.previous,panel_background); fill(tab_strip.next,panel_background);
        canvas->save(); canvas->translate(tab_strip.previous.x+18*scale,tab_strip.previous.y+10*scale); canvas->scale(-1,1);
        draw_icon(canvas,Icon::chevron_right,0,0,14*scale,muted); canvas->restore();
        draw_icon(canvas,Icon::chevron_right,tab_strip.next.x+6*scale,tab_strip.next.y+10*scale,14*scale,muted);
    }
    fill(geometry.breadcrumb,background);
    if (const auto* document=current()) {
        canvas->save(); canvas->clipRect(SkRect::MakeXYWH(geometry.breadcrumb.x,geometry.breadcrumb.y,geometry.breadcrumb.width,geometry.breadcrumb.height));
        label(canvas,document->path.empty() ? "Untitled" : document->path,left+18*scale,geometry.breadcrumb.y+5*scale,12,muted);
        canvas->restore();
    }
    rectangle(canvas,left,geometry.breadcrumb.bottom()-scale,right-left,scale,border_color);

    if (auto* document = current()) {
        const auto& selections = document->metadata.at("selections");
        const auto& positions = document->metadata.contains("selectionPositions") ? document->metadata["selectionPositions"] : selections;
        const auto active_line = positions.empty() ? std::size_t{0} : positions.front().value("line", std::size_t{0});
        canvas->save(); canvas->clipRect(SkRect::MakeLTRB(left, content_top, right, content_bottom));
        for (auto& [number, line] : document->lines) {
            if (number < document->first_line) { continue; }
            const auto y = content_top + static_cast<float>(number - document->first_line) * row_height;
            if (y >= content_bottom) { break; }
            if (!line.layout) {
                TextLayoutOptions style; style.font_size = options.font_size; style.dpi_scale = scale;
                style.line_height = std::clamp(editor_settings.value("lineHeight", 1.4F), 1.0F, 4.0F);
                style.tab_size = editor_settings.value("tabSize", 4U); style.color = foreground;
                if (document->editor_config.contains("tab_width")) { style.tab_size = static_cast<unsigned>(std::stoul(document->editor_config.at("tab_width").get<std::string>())); }
                if (editor_settings.contains("fontFamilies")) { style.font_families = editor_settings.at("fontFamilies").get<std::vector<std::string>>(); }
                if (document->syntax_revision == document->view_revision) {
                    for (const auto& highlight : document->highlights) {
                        const auto begin = std::max(line.begin, highlight.at("begin").get<std::size_t>());
                        const auto end = std::min(line.end, highlight.at("end").get<std::size_t>());
                        if (begin >= end) { continue; }
                        const auto kind = highlight.at("kind").get<std::string>();
                        const auto found = syntax_colors.find(kind);
                        const auto color = found == syntax_colors.end() ? foreground : found->second;
                        style.color_spans.push_back({begin - line.begin, end - line.begin, color});
                    }
                }
                line.layout.emplace(fonts.layout(line.text, style));
            }
            if (document->reveal_horizontal && number == active_line && !selections.empty()) {
                const auto head = selections.front().at("head").get<std::size_t>();
                if (head >= line.begin && head <= line.end) {
                    const auto x = line.layout->caret(head - line.begin).x;
                    const auto available = std::max(20 * scale, right - left - gutter - 25 * scale);
                    if (x < document->horizontal) { document->horizontal = std::max(0.0F, x - 20 * scale); }
                    else if (x > document->horizontal + available) { document->horizontal = x - available; }
                    document->reveal_horizontal = false;
                }
            }
            if (number == active_line) { rectangle(canvas, left, y, right - left, row_height, current_line_color); }
            for (const auto& change : document->git_gutter) {
                const auto begin = change.at("line").get<std::size_t>();
                const auto count = std::max(std::size_t{1}, change.at("count").get<std::size_t>());
                if (number >= begin && number - begin < count) {
                    const auto kind = change.at("kind").get<std::string>();
                    rectangle(canvas, left + 3 * scale, y, 3 * scale, kind == "deleted" ? 4 * scale : row_height,
                              kind == "added" ? added_color : (kind == "deleted" ? error_color : changed_color));
                }
            }
            label(canvas, std::to_string(number + 1), left + 15 * scale, y + 3 * scale, 12, number == active_line ? foreground : muted);
            const auto text_x = left + gutter - document->horizontal;
            canvas->save(); canvas->clipRect(SkRect::MakeLTRB(left + gutter - 5 * scale, y, right, y + row_height));
            for (const auto& selection : selections) {
                const auto anchor = selection.at("anchor").get<std::size_t>();
                const auto head = selection.at("head").get<std::size_t>();
                const auto begin = std::max(std::min(anchor, head), line.begin);
                const auto end = std::min(std::max(anchor, head), line.end);
                if (begin < end) {
                    for (const auto& box : line.layout->selection_rects(begin - line.begin, end - line.begin)) {
                        rectangle(canvas, text_x + box.x, y + box.y, box.width, box.height, selection_color);
                    }
                }
                if (std::min(anchor, head) <= line.end && std::max(anchor, head) > line.end && line.complete) {
                    rectangle(canvas, text_x + line.layout->width(), y, 8 * scale, row_height, selection_color);
                }
            }
            line.layout->draw(canvas, text_x, y);
            for (const auto& diagnostic : document->diagnostics) {
                const auto& range = diagnostic.at("range");
                const auto first = range.at("start").at("line").get<std::size_t>();
                const auto last = range.at("end").at("line").get<std::size_t>();
                if (number < first || number > last) { continue; }
                const auto begin = number == first ? line.layout->byte_offset_from_utf16(range.at("start").at("character").get<std::size_t>()) : 0;
                const auto end = number == last ? line.layout->byte_offset_from_utf16(range.at("end").at("character").get<std::size_t>()) : line.text.size();
                const auto color = diagnostic.value("severity", 1) == 1 ? error_color : warning_color;
                for (const auto& box : line.layout->selection_rects(begin, end)) {
                    rectangle(canvas, text_x + box.x, y + box.y + box.height - 2 * scale, std::max(scale, box.width), scale, color);
                }
            }
            for (std::size_t index = 0; index < selections.size(); ++index) {
                const auto head = selections[index].at("head").get<std::size_t>();
                if (head < line.begin || head > line.end) { continue; }
                const auto caret = line.layout->caret(head - line.begin);
                const auto cursor_x = text_x + caret.x;
                const auto cursor_y = y + caret.y;
                if (!editor_settings.value("cursorBlink", true) || (SDL_GetTicks() / 550) % 2 == 0 || !requests.empty() || active_request) {
                    rectangle(canvas, cursor_x, cursor_y, std::max(1.0F, 1.5F * scale), caret.height, cursor_color);
                }
                if (index == 0) {
                    int logical_width{}, logical_height{};
                    if (SDL_GetWindowSize(renderer.window(), &logical_width, &logical_height) && logical_width > 0 && logical_height > 0) {
                        const auto rx = width / static_cast<float>(logical_width);
                        const auto ry = height / static_cast<float>(logical_height);
                        renderer.set_text_input_area(SDL_Rect{static_cast<int>(cursor_x / rx), static_cast<int>(cursor_y / ry),
                            2, std::max(1, static_cast<int>(caret.height / ry))}, 0);
                    }
                    if (!preedit.empty() && prompt == Prompt::none) {
                        auto composition = fonts.layout(preedit, TextLayoutOptions{options.font_size, scale, 1.5F});
                        rectangle(canvas, cursor_x, cursor_y, composition.width() + 4 * scale, composition.height(), background);
                        composition.draw(canvas, cursor_x, cursor_y);
                        rectangle(canvas, cursor_x, cursor_y + composition.height() - scale, composition.width(), scale, accent);
                    }
                }
            }
            if (!line.complete) { label(canvas, "  … loading long line", text_x + line.layout->width(), y + 3 * scale, 12, muted); }
            canvas->restore();
        }
        canvas->restore();
        const auto lines = document->metadata.at("lineCount").get<std::size_t>();
        const auto rows = std::max(1.0F, (content_bottom - content_top) / row_height);
        if (static_cast<float>(lines) > rows) {
            const auto track = content_bottom - content_top;
            const auto thumb = std::max(28 * scale, track * rows / static_cast<float>(lines));
            const auto y = content_top + (track - thumb) * static_cast<float>(document->first_line) / static_cast<float>(std::max(std::size_t{1}, lines - 1));
            rectangle(canvas, right - 7 * scale, y, 5 * scale, thumb, scrollbar_color);
        }
    } else {
        label(canvas, "ASTRA", left + 85 * scale, content_top + 96 * scale, 40, muted);
        label(canvas, "A focused place to write code.", left + 88 * scale, content_top + 159 * scale, 15, muted);
        label(canvas, "New file        Ctrl+N", left + 90 * scale, content_top + 211 * scale, 13, muted);
        label(canvas, "Open file       Ctrl+P", left + 90 * scale, content_top + 248 * scale, 13, muted);
        label(canvas, "Commands        Ctrl+Shift+P", left + 90 * scale, content_top + 285 * scale, 13, muted);
    }

    if (geometry.outline_rows.height>0 && geometry.sidebar.width>0) {
        if (const auto* document = current()) {
            const auto ox=geometry.outline_rows.x+16*scale, oy=geometry.outline_rows.y;
            canvas->save(); canvas->clipRect(SkRect::MakeXYWH(geometry.outline_rows.x,oy,geometry.outline_rows.width,geometry.outline_rows.height));
            if (document->syntax_revision != document->view_revision || document->syntax_state == "pending") {
                label(canvas, "Parsing document...", ox, oy+6*scale, 12, muted);
            } else if (document->outline.empty()) {
                label(canvas, document->syntax_state == "unsupported" ? "No parser for this file type" : "No symbols", ox, oy+6*scale, 12, muted);
            } else for (std::size_t i = 0; i < document->outline.size(); ++i) {
                const auto y = oy+static_cast<float>(i)*geometry.sidebar_row_height;
                if (y >= geometry.outline_rows.bottom()) { break; }
                const auto& symbol = document->outline[i];
                const auto depth = std::min(symbol.value("depth", 0U), 8U);
                const auto x = ox+static_cast<float>(depth)*10*scale;
                draw_icon(canvas,symbol.at("kind")=="function" ? Icon::function : Icon::symbol,x,y+4*scale,16*scale,accent);
                label(canvas, symbol.at("name").get<std::string>(), x + 22 * scale, y+4*scale, 13, foreground);
            }
            canvas->restore();
        }
    }
    fill(geometry.status,status_color);
    rectangle(canvas,0,geometry.status.y,width,scale,border_color);
    const auto status_y=geometry.status.y+6*scale;
    draw_icon(canvas,Icon::branch,12*scale,geometry.status.y+4*scale,16*scale,status_foreground);
    const auto branch=git_status.value("branch",std::string("Astra"));
    elided_label(canvas,branch,34*scale,status_y,11,90*scale,status_foreground);
    canvas->save(); canvas->clipRect(SkRect::MakeXYWH(130*scale,geometry.status.y,std::max(0.0F,width-600*scale),geometry.status.height));
    label(canvas,status,140*scale,status_y,11,status_foreground); canvas->restore();
    if (const auto* document = current()) {
        const auto& positions = document->metadata.contains("selectionPositions") ? document->metadata["selectionPositions"] : document->metadata["selections"];
        const auto line = positions.empty() ? std::size_t{0} : positions.front().value("line", std::size_t{0});
        auto column=positions.empty() ? std::size_t{0} : positions.front().value("utf16Column",std::size_t{0});
        std::string unit="UTF16 Col ";
        auto tab_size=editor_settings.value("tabSize",4U);
        if (document->editor_config.contains("tab_width")) tab_size=static_cast<unsigned>(std::stoul(document->editor_config.at("tab_width").get<std::string>()));
        const auto found=document->lines.find(line);
        if (found!=document->lines.end() && found->second.layout) {
            const auto head=document->metadata.at("selections").front().at("head").get<std::size_t>();
            if (head>=found->second.begin && head<=found->second.end) {
                const auto advance=found->second.layout->tab_stop_width()/static_cast<float>(tab_size);
                if (advance>0) { column=static_cast<std::size_t>(std::round(found->second.layout->caret(head-found->second.begin).x/advance)); unit="Col "; }
            }
        }
        const auto spaces=document->editor_config.value("indent_style",std::string(editor_settings.value("insertSpaces",true) ? "space" : "tab"))=="space";
        auto detail="Ln "+std::to_string(line+1)+", "+unit+std::to_string(column+1);
        if (width>500*scale) detail+="    "+document->encoding;
        if (width>820*scale) detail+="    "+std::string(spaces ? "Spaces: " : "Tab Size: ")+std::to_string(tab_size)+(document->language_active ? "    LSP" : "");
        const auto detail_width=label_layout(detail,11,status_foreground).width();
        label(canvas,detail,std::max(130*scale,width-detail_width-16*scale),status_y,11,status_foreground);
    }
    if (recovery_available) {
        rectangle(canvas, left + 12 * scale, content_bottom - 100 * scale, std::max(100 * scale, right - left - 24 * scale), 36 * scale, recovery_background);
        label(canvas, "Unsaved recovery sessions are available — click to review", left + 24 * scale, content_bottom - 91 * scale, 12, foreground);
    }
    if (const auto notice = workspace_notice(); !notice.empty()) {
        rectangle(canvas, left + 12 * scale, content_bottom - 142 * scale, std::max(100 * scale, right - left - 24 * scale), 36 * scale, notice_background);
        canvas->save(); canvas->clipRect(SkRect::MakeLTRB(left + 24 * scale, content_bottom - 142 * scale, right - 44 * scale, content_bottom - 106 * scale));
        label(canvas, notice, left + 24 * scale, content_bottom - 133 * scale, 12, warning_color); canvas->restore();
        label(canvas, "×", right - 34 * scale, content_bottom - 133 * scale, 13, warning_color);
    }
    if (!error.empty()) {
        rectangle(canvas, left + 12 * scale, content_bottom - 58 * scale, std::max(100 * scale, right - left - 24 * scale), 46 * scale, error_background);
        label(canvas, error, left + 24 * scale, content_bottom - 44 * scale, 12, foreground);
    }
    draw_prompt(canvas, width, height);
    if (screenshot) { renderer.capture_png(*screenshot); screenshot.reset(); }
    renderer.end_frame();
}

EditorWindow::EditorWindow(std::shared_ptr<EditorChannel> channel, EditorWindowOptions options)
    : impl_(std::make_unique<Impl>(std::move(channel), std::move(options))) {}
EditorWindow::~EditorWindow() = default;
SDL_Window* EditorWindow::window() const noexcept { return impl_->renderer.window(); }
void EditorWindow::request_screenshot(std::filesystem::path destination) { impl_->screenshot = std::move(destination); }
void EditorWindow::execute_ui_command(std::string_view command) { impl_->command(command); }
void EditorWindow::open_path(std::string path) { impl_->open_path(std::move(path)); }
std::string EditorWindow::active_document_id() const { return impl_->active_id; }
bool EditorWindow::idle() const noexcept {
    const auto* document = impl_->current();
    return impl_->requests.empty() && !impl_->active_request && impl_->operations.empty() &&
        (impl_->workbench->idle() || (impl_->dialog && impl_->workbench->awaiting_input())) &&
        (!document || (!document->needs_lines && !document->loading && !document->needs_syntax && !document->syntax_loading));
}
std::string EditorWindow::last_error() const { return impl_->error; }
std::string EditorWindow::status_text() const { return impl_->status; }
std::string EditorWindow::workspace_notice() const { return impl_->workspace_notice(); }
WorkbenchLayout EditorWindow::layout() const { return impl_->geometry; }
WorkbenchViewState EditorWindow::view_state() const { return {impl_->geometry,impl_->tab_strip,impl_->tabs,impl_->file_tree.size()}; }
bool EditorWindow::pump() {
    if (!impl_->running) {
        if (!impl_->stopping_workbench) {
            impl_->finish_dialog(nullptr);
            impl_->stopping_workbench = true;
            impl_->workbench_shutdown_deadline = Clock::now() + std::chrono::seconds(5);
            impl_->workbench->shutdown();
        }
        impl_->workbench->poll();
        if (impl_->workbench->idle()) { return false; }
        if (Clock::now() >= impl_->workbench_shutdown_deadline) { throw std::runtime_error("Workbench transport did not drain on shutdown"); }
        return true;
    }
    try {
        impl_->renderer.poll_events([this](const SDL_Event& event) {
            try { impl_->event(event); }
            catch (const std::exception& error) { impl_->error = error.what(); }
        });
        impl_->service_responses();
        impl_->workbench->poll();
        for (const auto& error : impl_->workbench->take_errors()) { impl_->error = error.value("message", std::string("Workbench event failed")); }
        if (impl_->dragging && (impl_->mouse_y < impl_->content_top || impl_->mouse_y > impl_->content_bottom) && Clock::now() >= impl_->drag_scroll_after) {
            impl_->drag_scroll_after = Clock::now() + std::chrono::milliseconds(50);
            if (auto* document = impl_->current(); document && !document->loading) {
                if (impl_->mouse_y < impl_->content_top && document->first_line > 0) { --document->first_line; }
                else if (impl_->mouse_y > impl_->content_bottom) { document->first_line = std::min(document->first_line + 1, document->metadata.at("lineCount").get<std::size_t>() - 1); }
                document->needs_lines = true;
                impl_->point(std::clamp(impl_->mouse_x, impl_->left, impl_->right), std::clamp(impl_->mouse_y, impl_->content_top + 1, impl_->content_bottom - 1), true, false);
            }
        }
        if (impl_->running) { impl_->draw(); }
    } catch (const std::exception& error) {
        impl_->error = error.what();
        // A renderer failure cannot be presented as a functional blank editor.
        throw;
    }
    return true;
}
int EditorWindow::run() {
    while (pump()) { SDL_Delay(4); }
    return 0;
}

} // namespace astra::ui
