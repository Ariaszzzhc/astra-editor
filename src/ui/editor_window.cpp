#include "astra/ui/editor_window.hpp"
#include "astra/ui/renderer.hpp"
#include "astra/ui/text_layout.hpp"
#include "astra/ui/document_viewport.hpp"
#include "astra/ui/workbench.hpp"
#include "astra/ui/workbench_style.hpp"
#include "astra/ui/view_store.hpp"
#include "astra/ui/icons.hpp"
#include "astra/ui/file_icons.hpp"
#include "astra/ui/text_line_index.hpp"
#include "astra/ui/text_panel.hpp"
#include "astra/ui/text_surface.hpp"
#include "astra/ui/text_field.hpp"
#include "astra/ui/text_input_layout.hpp"
#include "astra/ui/text_labels.hpp"
#include "astra/ui/tab_bar.hpp"
#include "astra/ui/view_rows.hpp"
#include "astra/ui/workbench_chrome.hpp"
#include "astra/ui/document_metadata.hpp"
#include "astra/ui/picker_index.hpp"
#include "astra/ui/decoration_indexes.hpp"
#include "astra/ui/inline_form.hpp"
#include "astra/core/utf8.hpp"
#include "astra/ui/input_queue.hpp"
#include "astra/ui/input_events.hpp"
#include "astra/ui/form_registry.hpp"
#include "astra/io/file_store.hpp"

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
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
    for (auto& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c + ('a' - 'A'));
        }
    }
    return text;
}

std::string basename(std::string_view path) {
    const auto slash = path.find_last_of("/\\");
    return std::string(path.substr(slash == std::string_view::npos ? 0 : slash + 1));
}

std::string form_key(std::string_view id, std::string_view field) {
    auto result = std::string(id);
    result.push_back('\0');
    result.append(field);
    return result;
}

std::string normalized_binding(std::string_view binding) {
    const auto key = binding.substr(
        binding.find_last_of('+') == std::string_view::npos ? 0 : binding.find_last_of('+') + 1);
    return std::string(binding.find("Ctrl+") != std::string_view::npos ? "Ctrl+" : "") +
           (binding.find("Shift+") != std::string_view::npos ? "Shift+" : "") +
           (binding.find("Alt+") != std::string_view::npos ? "Alt+" : "") + std::string(key);
}

struct SdlStringDeleter {
    void operator()(char* value) const noexcept {
        SDL_free(value);
    }
};

} // namespace

struct EditorWindow::Impl {
    using Line = ViewportLine;

    struct Document {
        explicit Document(const TextLayoutEngine& fonts, std::shared_ptr<LayoutJobs> jobs)
            : viewport(fonts, {}, std::move(jobs)) {
        }

        Json metadata;
        std::string path;
        Json editor_config = Json::object();
        std::string encoding{"UTF-8"};
        std::string view_revision;
        DocumentViewport viewport;
        std::size_t first_line{};
        float horizontal{};
        bool needs_lines{true};
        bool loading{};
        std::string syntax_revision;
        std::string syntax_state{"pending"};
        DecorationIndexes decorations;
        Json outline = Json::array();
        std::size_t outline_scroll{}, outline_begin{}, outline_total{};
        bool needs_syntax{true};
        bool syntax_loading{};
        bool reveal_horizontal{};
        bool diagnostics_loading{};
        bool language_active{};
        Clock::time_point diagnostics_after{};
        std::uint64_t diagnostics_generation{};
    };

    struct PendingEdit {
        std::promise<Json> completion;
        bool settled{}, cancelled{};
        std::string operation;
        InputQueue::Identity input_fence{};
    };

    struct PendingRead {
        std::promise<Json> completion;
        bool settled{};
        InputQueue::Identity input_fence{};
    };

    struct PendingFence {
        script::Runtime::TaskId owner{};
        std::string method;
        std::promise<Json> completion;
    };

    struct Request {
        std::string method;
        Json params;
        std::string document;
        bool inject_revision{};
        std::function<void(const Json&)> complete;
        std::function<Json()> build_params;
        std::function<void()> failure{};
        std::shared_ptr<PendingEdit> edit{};
        std::shared_ptr<PendingRead> read{};
        std::function<bool()> ready{};
    };

    struct ActiveRequest {
        Request request;
        std::string id;
        std::future<std::optional<std::string>> future;
    };

    struct Operation {
        std::string id;
        std::function<void(const Json&)> complete;
        std::function<void()> failure;
        bool polling{};
        Clock::time_point next_poll{};
        Json accumulated;
        std::size_t offset{};
        std::shared_ptr<PendingEdit> edit;
        std::string document;
    };

    struct ViewState {
        std::string id;
        std::string revision;
        std::size_t first{};
        std::size_t limit{};
        std::map<std::size_t, Line> lines;
    };
    enum class Prompt {
        none,
        policy_input,
        policy_confirm,
        policy_choice,
        policy_pick,
        policy_text,
        policy_progress
    };

    struct Dialog {
        std::string id;
        script::Runtime::TaskId owner{};
        Json spec;
        std::promise<Json> completion;
        std::unique_ptr<TextSurface> detail;
        TextPanelPresentation text_panel;
        TextLineIndex items;
        PickerIndex picker;
        float horizontal{};
        std::unique_ptr<InlineForm> input;
        std::unique_ptr<TextField> input_layout;
    };

    std::shared_ptr<EditorChannel> channel;
    EditorWindowOptions options;
    Renderer renderer;
    TextLayoutEngine fonts;
    std::shared_ptr<LayoutJobs> layout_jobs = std::make_shared<LayoutJobs>();
    TextInputLayout document_composition{fonts, layout_jobs};
    bool document_composition_drawn{};
    std::uint32_t document_composition_color{};
    std::map<std::string, Document> documents;
    ClosedDocumentIds closed_documents;
    TabIndex tabs;
    std::string active_id;
    std::deque<Request> requests;
    std::optional<ActiveRequest> active_request;
    std::vector<std::shared_ptr<Operation>> operations;
    std::map<std::string, std::shared_ptr<Operation>> ordered_inputs;
    std::map<std::string, std::shared_ptr<PendingEdit>> pending_edits;
    std::map<std::string, std::shared_ptr<PendingRead>> pending_reads;
    std::map<std::string, PendingFence> pending_fences;

    struct PendingStyle {
        std::string id;
        Json patch;
        std::promise<Json> completion;
        Clock::time_point deadline;
    };

    std::deque<PendingStyle> pending_styles;
    std::uint64_t next_read{1};
    std::uint64_t next_request{1};
    std::map<std::string, std::string> keybindings;
    std::uint64_t keybinding_generation{};
    Json service_commands = Json::array();
    std::unique_ptr<Workbench> workbench;
    InputQueue policy_inputs;
    InputQueue view_inputs;
    std::set<std::string> service_ui_commands;
    Json busy_views = Json::object();
    Json settings = Json::object();
    Json editor_settings = Json::object();
    WorkbenchStyle ui_style;
    FileIcons file_icons;
    bool file_icons_override{};
    ViewStore sidebar_views;
    std::string active_view{"explorer"};
    std::optional<std::string> focused_view;
    std::uint64_t view_focus_sequence{};
    std::uint64_t document_activity{};
    bool pending_document_activity{};
    std::string focused_view_version;
    std::map<std::string, std::string> workspace_notices;
    std::string watch_error;
    std::string replace_notice;
    std::string workspace_branch{"Astra"};
    TextLabels labels{fonts};
    TabBar tab_bar{labels};
    ViewRows view_rows{labels};
    WorkbenchChrome chrome{labels};
    std::size_t painted_view_rows{};
    float scale{1};
    float row_height{26};
    float left{};
    float right{};
    float content_top{};
    float content_bottom{};
    float gutter{};
    bool show_sidebar{true};
    bool show_outline{};
    float sidebar_width{248};

    WorkbenchLayout geometry;
    bool dragging_sidebar{};
    std::string dragging_tab;
    float tab_drag_start{};
    bool running{true};
    bool stopping_workbench{};
    Clock::time_point workbench_shutdown_deadline{};
    bool quit_requested{};
    std::set<std::string> quitting_fences;
    bool recovery_available{};
    std::set<std::string> closing_pending;
    bool dragging{};
    bool dragging_scroll{};
    Clock::time_point drag_scroll_after{};
    std::size_t drag_anchor{};
    float mouse_x{};
    float mouse_y{};
    Prompt prompt{Prompt::none};
    std::optional<Dialog> dialog;
    std::string prompt_text;
    bool dragging_dialog_input{};
    std::size_t prompt_choice{};
    std::string preedit;
    bool consumed_modal_text{};
    std::bitset<128> consumed_modal_keys;
    std::string error;
    std::string status{"Starting workspace..."};
    std::optional<std::filesystem::path> screenshot;
    std::optional<float> vertical_column;
    FormRegistry forms;
    std::optional<std::size_t> dragging_form_field;
    std::string dragging_form_id;
    std::deque<std::pair<std::string, Json>> form_events;

    using FieldLayout = TextField;

    std::map<std::string, FieldLayout> form_fields;
    std::uint64_t form_sequence{};

    struct InputPoint {
        std::string dialog;
        std::string form;
        std::string epoch;
        std::size_t field{};
        Revision revision;
        Selection selection;
        TextLayoutOptions style;
        float x{}, y{};
        bool extend{};
        Clock::time_point deadline;
    };

    std::optional<InputPoint> input_point;
    InputEvents input_events;

    struct InputContext {
        enum class Kind { document, view, form, dialog };
        Kind kind{Kind::document};
        std::string id, epoch;
        std::optional<std::size_t> field;
        Revision revision;
        Selection selection;
        std::uint64_t focus{};
        friend bool operator==(const InputContext&, const InputContext&) = default;
    };

    std::optional<InputContext> input_event_context;
    bool replaying_input{};
    std::optional<SDL_Keymod> replay_input_modifiers;

    Impl(std::shared_ptr<EditorChannel> input, EditorWindowOptions configuration)
        : channel(std::move(input)), options(std::move(configuration)),
          renderer(options.title, options.width, options.height) {
        if (!channel) {
            throw std::invalid_argument("EditorWindow requires a document channel");
        }
        if (options.resource_directory.empty()) {
            const auto* base = SDL_GetBasePath();
            if (!base) {
                throw std::runtime_error(std::string("Locate workbench resources: ") +
                                         SDL_GetError());
            }
            options.resource_directory = std::filesystem::path(base);
        }
        std::map<std::string, std::string> modules;
        modules.emplace(
            "astra/defaults",
            io::read_file(options.resource_directory / "data" / "defaults.js", 1024 * 1024).text);
        for (const auto& [name, file] :
             std::map<std::string, std::string>{{"astra/ui", "api.js"},
                                                {"astra/icons", "icons.js"},
                                                {"astra/files", "files.js"},
                                                {"astra/git", "git.js"},
                                                {"astra/search", "search.js"},
                                                {"astra/workspace-search", "workspace-search.js"},
                                                {"astra/clipboard", "clipboard.js"},
                                                {"astra/language", "language.js"},
                                                {"astra/recovery", "recovery.js"},
                                                {"astra/lifecycle", "lifecycle.js"},
                                                {"astra/commands", "commands.js"},
                                                {"astra/workbench", "workbench.js"}}) {
            modules.emplace(
                name,
                io::read_file(options.resource_directory / "data" / "workbench" / file, 1024 * 1024)
                    .text);
        }
        workbench = std::make_unique<Workbench>(
            channel,
            [this](std::string_view method, const Json& params) {
                if (method == "_ui.dialog") {
                    return begin_dialog(params);
                }
                if (method == "_ui.read") {
                    return begin_read(params);
                }
                if (method == "_ui.barrier" || method == "_ui.window.close") {
                    return begin_fence(method, params);
                }
                if (method == "_ui.style.update") {
                    return begin_style(params);
                }
                if (method == "_ui.view.commit") {
                    return sidebar_views.commit(params.at("id").get<std::string>(),
                                                params.at("version").get<std::string>(),
                                                params.value("subtitle", std::string{}),
                                                params.value("empty", std::string("No items")));
                }
                if (method == "_ui.action" &&
                    params.at("command").get<std::string>().starts_with("editor.")) {
                    return begin_edit(params);
                }
                std::promise<Json> completed;
                try {
                    completed.set_value(native_ui_request(method, params));
                } catch (...) {
                    completed.set_exception(std::current_exception());
                }
                return completed.get_future();
            },
            modules,
            options.user_script);
        auto initial_settings = workbench->default_settings();
        initial_settings.merge_patch(workbench->settings());
        apply_settings(std::move(initial_settings));
        for (const auto& spec : workbench->settings().at("workbench").at("views"))
            sidebar_views.register_spec(spec);
        scale = renderer.dpi_scale();
        SDL_Rect usable{};
        if (SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(renderer.window()), &usable)) {
            const auto requested_width =
                static_cast<int>(std::round(static_cast<float>(options.width) * scale));
            const auto requested_height =
                static_cast<int>(std::round(static_cast<float>(options.height) * scale));
            if (!SDL_SetWindowSize(renderer.window(),
                                   std::min(requested_width, usable.w * 9 / 10),
                                   std::min(requested_height, usable.h * 9 / 10))) {
                throw std::runtime_error(std::string("Set DPI-scaled editor dimensions: ") +
                                         SDL_GetError());
            }
        }
        for (const auto& [key, name] : workbench->default_keybindings()) {
            keybindings[normalized_binding(key)] = name;
        }
        refresh_bindings();
        enqueue({"script.settings",
                 Json::object(),
                 {},
                 false,
                 [this](const Json& result) {
                     auto candidate = workbench->default_settings();
                     candidate.merge_patch(result.value("settings", Json::object()));
                     candidate.merge_patch(workbench->settings());
                     apply_settings(std::move(candidate));
                     if (result.contains("keybindings") && result["keybindings"].is_object()) {
                         for (const auto& [key, command] : result["keybindings"].items()) {
                             if (command.is_string()) {
                                 keybindings[normalized_binding(key)] = command.get<std::string>();
                             }
                         }
                     } else if (result.contains("keybindings") &&
                                result["keybindings"].is_array()) {
                         for (const auto& binding : result["keybindings"]) {
                             keybindings[normalized_binding(binding.at("key").get<std::string>())] =
                                 binding.at("command").get<std::string>();
                         }
                     }
                     service_commands = result.value("commands", Json::array());
                     for (const auto& command : service_commands) {
                         const auto name = command.get<std::string>();
                         if (name.starts_with("ui.")) {
                             service_ui_commands.insert(name);
                         }
                     }
                     keybinding_generation = 0;
                     refresh_bindings();
                 },
                 {}});
        enqueue({"workspace.documents",
                 Json::object(),
                 {},
                 false,
                 [this](const Json& result) {
                     for (const auto& metadata : result) {
                         accept_metadata(metadata);
                     }
                     if (!tabs.empty()) {
                         activate(tabs.id_at(0));
                     } else if (options.open_paths.empty()) {
                         new_document();
                     }
                     for (const auto& path : options.open_paths) {
                         open_path(path);
                     }
                 },
                 {}});
        refresh_tree();
        execute_policy("workbench.recoveryScan", Json::object());
    }

    Document* current() {
        const auto found = documents.find(active_id);
        return found == documents.end() ? nullptr : &found->second;
    }

    const Document* current() const {
        const auto found = documents.find(active_id);
        return found == documents.end() ? nullptr : &found->second;
    }

    void refresh_bindings() {
        const auto generation = workbench->keybinding_generation();
        if (generation == keybinding_generation) {
            return;
        }
        auto replacement = keybindings;
        for (const auto& [key, name] : workbench->keybindings()) {
            replacement[normalized_binding(key)] = name;
        }
        keybindings.swap(replacement);
        keybinding_generation = generation;
    }

    void enqueue(Request request) {
        if (requests.size() >= 4096) {
            throw std::runtime_error("Editor command queue is full; wait for the service");
        }
        requests.push_back(std::move(request));
    }

    bool accept_metadata(const Json& metadata) {
        const auto incoming = DocumentMetadataStamp::read(metadata);
        const auto id = metadata.at("id").get<std::string>();
        if (closed_documents.contains(document_id(id)))
            return false;
        if (const auto found = documents.find(id);
            found != documents.end() && !found->second.metadata.empty() &&
            !incoming.follows(DocumentMetadataStamp::read(found->second.metadata)))
            return false;
        auto [found, inserted] = documents.try_emplace(id, fonts, layout_jobs);
        auto& document = found->second;
        if (inserted) {
            tabs.append(id, "Untitled");
        }
        const auto revision = metadata.at("revision").get<std::string>();
        if (metadata.contains("path")) {
            const auto path = metadata["path"].get<std::string>();
            if (path != document.path && active_id == id)
                change_document_activity();
            if (path != document.path && !path.empty()) {
                document.language_active = true;
                document.diagnostics_after = {};
            }
            document.path = path;
        }
        if (metadata.contains("editorConfig")) {
            document.editor_config = metadata["editorConfig"];
        }
        if (metadata.contains("encoding")) {
            document.encoding = metadata.at("encoding").get<std::string>();
        }
        if (document.view_revision != revision) {
            document.needs_lines = true;
            document.needs_syntax = true;
            document.decorations.diagnostics.clear();
        }
        document.metadata = metadata;
        tabs.rename(id, document.path.empty() ? "Untitled" : basename(document.path));
        if (metadata.value("dirty", false)) {
            document.decorations.gutter.clear();
        }
        const auto line_count = metadata.value("lineCount", std::size_t{1});
        document.first_line = std::min(document.first_line, line_count - 1);
        return true;
    }

    void activate(const std::string& id) {
        if (!documents.contains(id)) {
            return;
        }
        if (active_id != id) {
            change_document_activity();
            static_cast<void>(close_inline_form());
        }
        active_id = id;
        tab_bar.reveal();
        documents.at(id).needs_lines = true;
        clear_preedit();
        vertical_column.reset();
    }

    void remove_document(const std::string& id) {
        const auto numeric_id = document_id(id);
        if (active_id == id && document_activity == std::numeric_limits<std::uint64_t>::max())
            throw protocol::RpcError(-32004, "Document activity identity exhausted");
        // Allocate the persistent closed-ID update before dropping live state.
        static_cast<void>(closed_documents.retire(numeric_id));
        if (active_id == id)
            change_document_activity();
        tabs.erase(id);
        documents.erase(id);
        workspace_notices.erase(id);
        if (active_id == id) {
            clear_preedit();
            static_cast<void>(close_inline_form());
            active_id = tabs.empty() ? std::string{} : tabs.id_at(tabs.size() - 1);
        }
    }

    void new_document() {
        execute_policy("ui.new");
    }

    void change_document_activity() {
        if (document_activity == std::numeric_limits<std::uint64_t>::max())
            throw protocol::RpcError(-32004, "Document activity identity exhausted");
        ++document_activity;
        pending_document_activity = true;
    }

    void start_operation(std::string method,
                         Json params,
                         std::function<void(const Json&)> complete,
                         std::string document = {},
                         bool revision = false,
                         std::function<void()> failure = {}) {
        enqueue({std::move(method),
                 std::move(params),
                 std::move(document),
                 revision,
                 [this, complete = std::move(complete), failure](const Json& result) {
                     auto operation = std::make_shared<Operation>();
                     operation->id = result.at("operationId").get<std::string>();
                     operation->complete = complete;
                     operation->failure = failure;
                     operations.push_back(std::move(operation));
                 },
                 {},
                 std::move(failure)});
    }

    void open_path(std::string path) {
        if (path.empty()) {
            return;
        }
        status = "Opening " + path;
        execute_policy("workbench.open", {{"path", path}});
    }

    void refresh_tree(bool announce = true) {
        static_cast<void>(announce);
        command("ui.refreshTree");
    }

    std::string workspace_notice() const {
        if (!watch_error.empty()) {
            return watch_error;
        }
        if (const auto found = workspace_notices.find(active_id);
            found != workspace_notices.end()) {
            return found->second;
        }
        if (!replace_notice.empty()) {
            return replace_notice;
        }
        return workspace_notices.empty() ? std::string{} : workspace_notices.begin()->second;
    }

    void service_responses();

    void retire_input(const std::shared_ptr<Operation>& operation) {
        const auto found = ordered_inputs.find(operation->document);
        if (found != ordered_inputs.end() && found->second == operation)
            ordered_inputs.erase(found);
    }

    bool queued_for(const std::string& id) const {
        return (active_request && active_request->request.document == id) ||
               std::ranges::any_of(requests, [&](const Request& request) {
                   return request.document == id;
               });
    }

    void fetch_lines();
    void fetch_lines_page(std::shared_ptr<ViewState> view,
                          std::size_t first,
                          std::optional<std::size_t> begin = {});
    void fetch_syntax();
    void sync_dialog_input();
    bool point_dialog_input(float, float, bool);
    void point_input(
        InlineForm&, std::size_t, TextField&, float, float, bool, std::string dialog_id = {});
    bool complete_input_point();
    void drain_input_events();
    void clear_input_events();
    InputContext input_context() const;
    void input_area(float, float, float);
    bool
    text_input_key(InlineForm&, const SDL_KeyboardEvent&, bool, bool, const std::function<void()>&);
    TextLayoutOptions input_style(float) const;
    TextLayoutOptions composition_style() const;
    void clear_preedit(bool cancel_native = true);
    void set_preedit(const char* text);
    void draw_field(
        SkCanvas*, const InlineForm::Field&, Box, bool, bool, TextField&, std::string_view, float);
    const TextLayout* prepare_line(Document&, std::size_t);
    void fetch_diagnostics(const std::string& id);
    void editor_command(std::string command,
                        Json extra = Json::object(),
                        std::shared_ptr<PendingEdit> edit = {});
    std::future<Json> begin_edit(const Json& params);
    std::future<Json> begin_read(Json params);
    void settle_read(const std::shared_ptr<PendingRead>& read,
                     const Json& value,
                     std::exception_ptr failure = {});
    void settle_edit(const std::shared_ptr<PendingEdit>& edit,
                     const Json& result,
                     std::exception_ptr failure = {});
    void cancel_edit(const std::shared_ptr<PendingEdit>& edit);
    void set_selections(Json selections, std::string revision = {});
    void close_document(const std::string& id, bool discard = false);
    std::future<Json> begin_fence(std::string_view method, const Json& params);
    bool finish_fences();
    bool native_idle(bool waiting_inputs = false) const;
    void command(std::string_view name);
    void execute_policy(std::string_view name, const Json& params = Json::object());
    void editor_policy(std::string name, Json parameters = Json::object());
    void advance_policy_inputs();
    void view_policy(std::string name, Json parameters);
    void advance_view_inputs();
    void native_command(std::string_view name);
    Json native_ui_request(std::string_view method, const Json& params);
    void apply_settings(Json candidate);
    [[nodiscard]] Json style_model() const;
    std::future<Json> begin_style(const Json& params);
    void validate_style_patch(const Json& params) const;
    void finish_styles();
    std::future<Json> begin_dialog(const Json& spec);
    void finish_dialog(Json value);
    void emit_picker_query();

    [[nodiscard]] std::string newline(const Document* document) const;
    void event(const SDL_Event& event);
    void key(const SDL_KeyboardEvent& key);
    void point(float x, float y, bool extend, bool add);
    void scroll_to(float y);
    std::optional<Json>
    vertical(std::string_view id, bool down, bool extend, std::optional<std::string> revision = {});
    void draw();
    void measure_layout(bool prepare_tabs = false);
    void draw_prompt(SkCanvas* canvas, float width, float height);
    void draw_inline_form(SkCanvas* canvas);
    void draw_form(SkCanvas* canvas, const InlineForm& form, const InlineFormLayout& boxes);
    InlineFormLayout form_layout(const InlineForm& form) const;
    void clear_form_cache(const InlineForm& form);
    bool inline_key(const SDL_KeyboardEvent& input, bool control, bool shift, bool alt);
    void focus_view(std::optional<std::string> id);
    std::size_t view_page() const;
    ViewRowsLayout view_rows_layout() const;
    void
    emit_form(std::string_view event, std::string_view action = {}, InlineForm* target = nullptr);
    bool close_inline_form(std::string_view id = {});
    void
    label(SkCanvas* canvas, std::string_view text, float x, float y, float size, SkColor color = 0);
    TextLayoutOptions label_style(float size, SkColor color) const;
    std::shared_ptr<const TextLayout>
    label_layout(std::string_view text, float size, SkColor color);
    void elided_label(SkCanvas* canvas,
                      std::string_view text,
                      float x,
                      float y,
                      float size,
                      float available,
                      SkColor color);
    void rectangle(SkCanvas* canvas, float x, float y, float width, float height, SkColor color);
    void reveal_caret();
    void accept_prompt();
};

void EditorWindow::Impl::service_responses() {
    for (const auto& body : channel->take_notifications()) {
        const auto notification = Json::parse(body);
        const auto method = notification.value("method", std::string{});
        const auto metadata_event = method == "document.changed" || method == "document.created";
        if (metadata_event && !accept_metadata(notification.at("params")))
            continue;
        workbench->emit(method, notification.value("params", Json::object()));
        if (method == "document.closed") {
            remove_document(notification.at("params").at("id").get<std::string>());
        } else if (method == "language.diagnosticsChanged") {
            const auto id = notification.at("params").at("id").get<std::string>();
            if (const auto found = documents.find(id); found != documents.end()) {
                found->second.language_active = true;
                found->second.diagnostics_after = {};
                ++found->second.diagnostics_generation;
            }
        } else if (method == "workspace.watchError") {
            watch_error = "File monitoring: " +
                          notification.at("params").value(
                              "message", std::string("Monitoring failed; reload files explicitly"));
        } else if (method == "workspace.fileChanged") {
            const auto& value = notification.at("params");
            const auto id = value.at("id").get<std::string>();
            const auto path = value.at("path").get<std::string>();
            const auto change = value.at("status").get<std::string>();
            if (change == "reloaded") {
                workspace_notices[id] = "External changes reloaded: " + path;
            } else if (change == "deleted") {
                workspace_notices[id] = "File deleted outside Astra; buffer preserved: " + path;
            } else if (change == "conflict") {
                workspace_notices[id] =
                    "File changed outside Astra; unsaved buffer preserved: " + path;
            } else {
                workspace_notices[id] = "Cannot read " + path + ": " +
                                        value.value("message", std::string("External file error"));
            }
            refresh_tree(false);
        }
    }
    for (int processed = 0; processed < 32; ++processed) {
        if (active_request) {
            if (active_request->future.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready) {
                break;
            }
            auto completed = std::move(*active_request);
            active_request.reset();
            try {
                const auto body = completed.future.get();
                if (!body) {
                    throw std::runtime_error(
                        "The service returned no response for an editor request");
                }
                const auto response = Json::parse(*body);
                if (!response.contains("id") || response["id"] != completed.id) {
                    throw std::runtime_error("The service response ID does not match its request");
                }
                if (response.contains("error")) {
                    const auto& failure = response.at("error");
                    throw protocol::RpcError(
                        failure.value("code", -32603),
                        failure.value("message", std::string("Service request failed")),
                        failure.contains("data") ? std::optional<Json>(failure.at("data"))
                                                 : std::nullopt);
                }
                if (completed.request.method == "script.command" &&
                    response.at("result").is_object() &&
                    response.at("result").contains("operationId")) {
                    auto operation = std::make_shared<Operation>();
                    operation->id = response.at("result").at("operationId").get<std::string>();
                    operation->complete = completed.request.complete;
                    operation->failure = completed.request.failure;
                    operation->edit = completed.request.edit;
                    operation->document = completed.request.document;
                    if (!operation->document.empty())
                        ordered_inputs[operation->document] = operation;
                    operations.push_back(operation);
                    if (operation->edit) {
                        operation->edit->operation = operation->id;
                        if (operation->edit->cancelled)
                            enqueue(
                                {"operation.cancel", {{"id", operation->id}}, {}, false, {}, {}});
                    }
                } else {
                    if (completed.request.complete) {
                        completed.request.complete(response.at("result"));
                    }
                    settle_edit(completed.request.edit, response.at("result"));
                }
                if (completed.request.method == "script.command" ||
                    completed.request.method == "document.edit") {
                    error.clear();
                }
            } catch (const std::exception& failure) {
                bool script_handles_failure = static_cast<bool>(completed.request.edit);
                const auto* rpc_failure = dynamic_cast<const protocol::RpcError*>(&failure);
                const bool obsolete_syntax = completed.request.method == "document.syntax" &&
                                             rpc_failure && rpc_failure->code() == -32001;
                const bool presentation_read = completed.request.method == "document.lines" ||
                                               completed.request.method == "document.syntax" ||
                                               completed.request.method == "language.diagnostics";
                const bool missing_document =
                    presentation_read && !completed.request.document.empty() && rpc_failure &&
                    rpc_failure->code() == -32002 && rpc_failure->data() &&
                    rpc_failure->data()->is_object() &&
                    rpc_failure->data()->value("id", Json{}) == completed.request.document;
                // A real missing-buffer reply can beat the close notification on
                // another connection. It confirms this background target retired.
                if (missing_document)
                    remove_document(completed.request.document);
                settle_edit(completed.request.edit, nullptr, std::current_exception());
                if (completed.request.failure) {
                    completed.request.failure();
                }
                if (completed.request.method == "operation.get") {
                    const auto id = completed.request.params.value("id", std::string{});
                    for (const auto& operation : operations) {
                        if (operation->id == id) {
                            retire_input(operation);
                            script_handles_failure =
                                script_handles_failure || static_cast<bool>(operation->edit);
                            settle_edit(operation->edit, nullptr, std::current_exception());
                            if (operation->failure)
                                operation->failure();
                        }
                    }
                    std::erase_if(operations, [&](const auto& operation) {
                        return operation->id == id;
                    });
                }
                if (!script_handles_failure && !obsolete_syntax && !missing_document) {
                    error = failure.what();
                    status = "Action failed";
                }
                if (completed.request.method == "document.syntax" &&
                    documents.contains(completed.request.document)) {
                    auto& document = documents.at(completed.request.document);
                    document.syntax_loading = false;
                    document.needs_syntax = obsolete_syntax;
                    document.syntax_state = obsolete_syntax ? "pending" : "failed";
                }
                if (completed.request.method == "language.diagnostics" &&
                    documents.contains(completed.request.document)) {
                    auto& document = documents.at(completed.request.document);
                    document.diagnostics_loading = false;
                    document.language_active = false;
                }
                if (completed.request.method == "document.lines" &&
                    documents.contains(completed.request.document)) {
                    auto& document = documents.at(completed.request.document);
                    document.loading = false;
                    document.needs_lines = false;
                } else if (completed.request.method == "script.command" ||
                           completed.request.method == "document.edit" ||
                           completed.request.method == "document.setSelections") {
                    if (auto* document = current()) {
                        document->needs_lines = true;
                    }
                }
            }
        }
        if (requests.empty()) {
            break;
        }
        std::set<std::string> geometry_waiting;
        const auto next = std::find_if(
            requests.begin(), requests.end(), [this, &geometry_waiting](const Request& request) {
                if (request.edit && request.edit->cancelled) {
                    return true;
                }
                const bool presentation_read = request.method == "document.lines" ||
                                               request.method == "document.syntax" ||
                                               request.method == "language.diagnostics";
                if (!presentation_read && geometry_waiting.contains(request.document)) {
                    return false;
                }
                bool ready = !request.ready || request.ready();
                if (ready && request.method == "_ui.vertical") {
                    const auto found = documents.find(request.document);
                    if (found != documents.end() && request.document == active_id &&
                        found->second.view_revision ==
                            found->second.metadata.at("revision").get<std::string>()) {
                        auto& document = found->second;
                        const auto& positions = document.metadata.contains("selectionPositions")
                                                    ? document.metadata.at("selectionPositions")
                                                    : document.metadata.at("selections");
                        for (const auto& position : positions) {
                            const auto number = position.value("line", std::size_t{0});
                            const auto target =
                                request.params.at("name") == "editor.move_down"
                                    ? std::min(
                                          number + 1,
                                          document.metadata.at("lineCount").get<std::size_t>() - 1)
                                    : (number ? number - 1 : 0);
                            for (const auto row : {number, target}) {
                                if (document.viewport.lines().find(row) &&
                                    !prepare_line(document, row)) {
                                    ready = false;
                                }
                            }
                        }
                    }
                }
                if (!ready) {
                    geometry_waiting.insert(request.document);
                    return false;
                }
                if (request.edit && request.edit->cancelled)
                    return true;
                if (request.read &&
                    policy_inputs.pending(request.document, request.read->input_fence))
                    return false;
                if (request.edit &&
                    policy_inputs.pending(request.document, request.edit->input_fence))
                    return false;
                return request.document.empty() || !ordered_inputs.contains(request.document) ||
                       request.method == "document.lines" || request.method == "document.syntax" ||
                       request.method == "language.diagnostics";
            });
        if (next == requests.end())
            break;
        auto request = std::move(*next);
        requests.erase(next);
        if (request.read) {
            if (!request.read->settled) {
                try {
                    settle_read(request.read, native_ui_request("_ui.read", request.params));
                } catch (...) {
                    settle_read(request.read, nullptr, std::current_exception());
                }
            }
            continue;
        }
        if (request.edit && request.edit->cancelled) {
            continue;
        }
        if (!request.document.empty() && !documents.contains(request.document)) {
            settle_edit(request.edit,
                        nullptr,
                        std::make_exception_ptr(protocol::RpcError(
                            -32001, "Document closed before the edit was dispatched")));
            continue;
        }
        if (request.method == "document.syntax" &&
            request.params.at("baseRevision") !=
                documents.at(request.document).metadata.at("revision")) {
            auto& document = documents.at(request.document);
            document.syntax_loading = false;
            document.needs_syntax = true;
            document.syntax_state = "pending";
            continue;
        }
        try {
            if (request.method == "_ui.vertical") {
                const auto& parameters = request.params.at("params");
                const auto revision = parameters.contains("baseRevision")
                                          ? std::optional<std::string>(
                                                parameters.at("baseRevision").get<std::string>())
                                          : std::nullopt;
                auto selections = vertical(request.document,
                                           request.params.at("name") == "editor.move_down",
                                           parameters.value("extend", false),
                                           revision);
                if (selections) {
                    request.method = "document.setSelections";
                    request.params = std::move(*selections);
                    request.inject_revision = false;
                } else
                    request.method = "script.command";
            }
            if (request.build_params) {
                request.params = request.build_params();
            }
            if (request.inject_revision) {
                const auto found = documents.find(request.document);
                if (found == documents.end()) {
                    continue;
                }
                auto& params =
                    request.method == "script.command" ? request.params["params"] : request.params;
                params["id"] = request.document;
                params["baseRevision"] = found->second.metadata.at("revision");
            }
            const auto id = std::to_string(next_request++);
            auto future = channel->submit(Json{
                {"jsonrpc", "2.0"},
                {"id", id},
                {"method", request.method},
                {"params", request.params}}.dump());
            active_request.emplace(ActiveRequest{std::move(request), id, std::move(future)});
        } catch (const std::exception& failure) {
            settle_edit(request.edit, nullptr, std::current_exception());
            if (request.failure)
                request.failure();
            error = failure.what();
            status = "Action failed";
        }
    }
    const auto now = Clock::now();
    for (const auto& operation : operations) {
        if (operation->polling || now < operation->next_poll) {
            continue;
        }
        operation->polling = true;
        enqueue({"operation.get",
                 {{"id", operation->id}, {"offset", operation->offset}, {"limit", 256}},
                 {},
                 false,
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
                                     if (operation->accumulated.empty()) {
                                         operation->accumulated = value;
                                     } else {
                                         for (const auto& entry : value[field]) {
                                             operation->accumulated[field].push_back(entry);
                                         }
                                     }
                                 }
                             }
                             if (page.contains("nextOffset") && !page["nextOffset"].is_null() &&
                                 page["nextOffset"].get<std::size_t>() <
                                     page.at("total").get<std::size_t>()) {
                                 operation->offset = page["nextOffset"].get<std::size_t>();
                                 return;
                             }
                             if (!operation->accumulated.empty()) {
                                 value = operation->accumulated;
                             }
                         }
                         if (operation->complete) {
                             operation->complete(value);
                         }
                         settle_edit(operation->edit, value);
                         retire_input(operation);
                         std::erase(operations, operation);
                     } else if (state == "failed" || state == "cancelled") {
                         const auto failure = result.value("error", Json::object());
                         const auto exception = std::make_exception_ptr(protocol::RpcError(
                             failure.value("code", -32800),
                             failure.value("message", std::string("Workspace operation cancelled")),
                             failure.contains("data") ? std::optional<Json>(failure.at("data"))
                                                      : std::nullopt));
                         settle_edit(operation->edit, nullptr, exception);
                         std::rethrow_exception(exception);
                     }
                 },
                 {}});
    }
    fetch_lines();
    fetch_syntax();
    if (const auto* document = current();
        document && document->language_active && !document->diagnostics_loading &&
        Clock::now() >= document->diagnostics_after && requests.empty() && !active_request) {
        fetch_diagnostics(active_id);
    }
}

void EditorWindow::Impl::fetch_lines() {
    auto* document = current();
    if (!document || !document->needs_lines || document->loading || queued_for(active_id)) {
        return;
    }
    const auto id = active_id;
    const auto first = document->first_line > 2 ? document->first_line - 2 : 0;
    const auto visible =
        static_cast<std::size_t>(std::max(1.0F, (content_bottom - content_top) / row_height)) + 8;
    document->loading = true;
    auto view = std::make_shared<ViewState>();
    view->id = id;
    view->revision = document->metadata.at("revision").get<std::string>();
    view->first = first;
    view->limit = std::min(first + std::min(visible, std::size_t{256}),
                           document->metadata.at("lineCount").get<std::size_t>());
    fetch_lines_page(std::move(view), first);
}

void EditorWindow::Impl::fetch_lines_page(std::shared_ptr<ViewState> view,
                                          std::size_t first,
                                          std::optional<std::size_t> begin) {
    Json params{{"id", view->id}, {"firstLine", first}, {"count", view->limit - first}};
    if (begin) {
        params["begin"] = *begin;
    }
    enqueue(
        {"document.lines",
         std::move(params),
         view->id,
         false,
         [this, view](const Json& result) {
             if (!documents.contains(view->id)) {
                 return;
             }
             auto& document = documents.at(view->id);
             if (result.at("revision") != view->revision ||
                 document.metadata.at("revision") != view->revision) {
                 document.loading = false;
                 document.needs_lines = true;
                 return;
             }
             accept_metadata(result);
             for (const auto& record : result.at("lines")) {
                 const auto number = record.at("line").get<std::size_t>();
                 auto [found, inserted] = view->lines.try_emplace(number);
                 auto& line = found->second;
                 line.number = number;
                 if (inserted) {
                     line.begin = record.at("begin").get<std::size_t>();
                 }
                 line.end = record.at("end").get<std::size_t>();
                 line.next = record.at("nextBegin").get<std::size_t>();
                 line.complete = record.at("complete").get<bool>();
                 line.text += record.at("text").get<std::string>();
             }
             if (result.contains("continuation")) {
                 const auto& continuation = result.at("continuation");
                 fetch_lines_page(view,
                                  continuation.at("firstLine").get<std::size_t>(),
                                  continuation.at("begin").get<std::size_t>());
                 return;
             }
             const auto next = result.at("nextLine").get<std::size_t>();
             if (next < view->limit && !result.at("lines").empty()) {
                 fetch_lines_page(view, next);
                 return;
             }
             std::vector<Line> lines;
             lines.reserve(view->lines.size());
             for (auto& [number, line] : view->lines) {
                 static_cast<void>(number);
                 lines.push_back(std::move(line));
             }
             document.viewport.publish(std::move(lines));
             document.view_revision = view->revision;
             document.loading = false;
             const auto rows = static_cast<std::size_t>(
                                   std::max(1.0F, (content_bottom - content_top) / row_height)) +
                               1;
             const auto required_end = std::min(
                 document.first_line + rows, document.metadata.at("lineCount").get<std::size_t>());
             document.needs_lines = document.first_line < view->first || required_end > view->limit;
             document.needs_syntax = true;
         },
         {}});
}

void EditorWindow::Impl::fetch_syntax() {
    auto* document = current();
    if (!document || document->loading || document->needs_lines || !document->needs_syntax ||
        document->syntax_loading || queued_for(active_id) || document->viewport.lines().empty()) {
        return;
    }
    const auto id = active_id;
    const auto revision = document->view_revision;
    const auto outline_begin = document->outline_scroll;
    const auto outline_limit = static_cast<std::size_t>(std::ceil(geometry.outline_rows.height /
                                                                  geometry.sidebar_row_height)) +
                               1;
    const auto include_outline = show_outline && geometry.outline_rows.height > 0;
    document->syntax_loading = true;
    enqueue({"document.syntax",
             {{"id", id},
              {"baseRevision", revision},
              {"begin", document->viewport.lines().summary().begin},
              {"end", document->viewport.lines().summary().end},
              {"outlineBegin", outline_begin},
              {"outlineLimit", outline_limit},
              {"includeOutline", include_outline}},
             id,
             false,
             [this, id, revision, outline_limit, include_outline](const Json& result) {
                 if (!documents.contains(id)) {
                     return;
                 }
                 auto& document = documents.at(id);
                 document.syntax_loading = false;
                 if (document.metadata.at("revision") != revision) {
                     document.needs_syntax = true;
                     return;
                 }
                 const auto state = result.at("state").get<std::string>();
                 document.syntax_state = state;
                 if (state == "pending") {
                     document.syntax_loading = true;
                     auto operation = std::make_shared<Operation>();
                     operation->id = result.at("operationId").get<std::string>();
                     operation->complete = [this, id](const Json&) {
                         if (documents.contains(id)) {
                             documents.at(id).syntax_loading = false;
                             documents.at(id).needs_syntax = true;
                         }
                     };
                     operation->failure = [this, id] {
                         if (documents.contains(id)) {
                             auto& doc = documents.at(id);
                             doc.syntax_loading = false;
                             doc.needs_syntax = false;
                             doc.syntax_state = "failed";
                         }
                     };
                     operations.push_back(std::move(operation));
                     return;
                 }
                 document.syntax_revision = revision;
                 document.needs_syntax = false;
                 document.decorations.set_highlights(result.value("highlights", Json::array()));
                 document.outline = result.value("outline", Json::array());
                 const auto page = result.value("outlinePage", Json::object());
                 document.outline_begin = page.value("offset", std::size_t{0});
                 document.outline_total = page.value("total", document.outline.size());
                 if (document.outline_scroll >= document.outline_total)
                     document.outline_scroll = document.outline_total > outline_limit
                                                   ? document.outline_total - outline_limit
                                                   : 0;
                 const auto current_limit =
                     static_cast<std::size_t>(
                         std::ceil(geometry.outline_rows.height / geometry.sidebar_row_height)) +
                     1;
                 const auto want_outline = show_outline && geometry.outline_rows.height > 0;
                 document.needs_syntax =
                     want_outline != include_outline ||
                     (want_outline && (document.outline_scroll != document.outline_begin ||
                                       current_limit > outline_limit));
                 document.viewport.invalidate_layouts();
             },
             {}});
}

void EditorWindow::Impl::settle_edit(const std::shared_ptr<PendingEdit>& edit,
                                     const Json& result,
                                     std::exception_ptr failure) {
    if (!edit || edit->settled)
        return;
    edit->settled = true;
    if (failure)
        edit->completion.set_exception(std::move(failure));
    else
        edit->completion.set_value(result);
    std::erase_if(pending_edits, [&](const auto& entry) {
        return entry.second == edit;
    });
}

void EditorWindow::Impl::cancel_edit(const std::shared_ptr<PendingEdit>& edit) {
    if (edit->cancelled || edit->settled)
        return;
    edit->cancelled = true;
    // Runtime cancellation rejects the caller and drains this abandoned future.
    // Resolve the drain value, as for dismissed native dialogs.
    settle_edit(edit, nullptr);
    if (!edit->operation.empty())
        enqueue({"operation.cancel", {{"id", edit->operation}}, {}, false, {}, {}});
}

void EditorWindow::Impl::settle_read(const std::shared_ptr<PendingRead>& read,
                                     const Json& value,
                                     std::exception_ptr failure) {
    if (read->settled)
        return;
    read->settled = true;
    if (failure)
        read->completion.set_exception(std::move(failure));
    else
        read->completion.set_value(value);
    std::erase_if(pending_reads, [&](const auto& entry) {
        return entry.second == read;
    });
    std::erase_if(requests, [&](const Request& request) {
        return request.read == read;
    });
}

std::future<Json> EditorWindow::Impl::begin_read(Json params) {
    const auto id = params.value("id", active_id);
    params["id"] = id;
    auto read = std::make_shared<PendingRead>();
    read->input_fence = policy_inputs.fence(id, workbench ? workbench->request_owner() : 0);
    auto future = read->completion.get_future();
    if (params.value("immediate", false) || id.empty() ||
        (!queued_for(id) && !ordered_inputs.contains(id) &&
         !policy_inputs.pending(id, read->input_fence))) {
        try {
            settle_read(read, native_ui_request("_ui.read", params));
        } catch (...) {
            settle_read(read, nullptr, std::current_exception());
        }
        return future;
    }
    if (!params.contains("readId") && next_read == std::numeric_limits<std::uint64_t>::max())
        throw protocol::RpcError(-32004, "UI read identity capacity exceeded");
    const auto token = params.contains("readId") ? params.at("readId").get<std::string>()
                                                 : "native-read-" + std::to_string(next_read++);
    if (token.empty() || token.size() > 128 || pending_reads.contains(token))
        throw protocol::RpcError(-32602, "Invalid UI read identity");
    if (pending_reads.size() >= 256)
        throw protocol::RpcError(-32004, "UI read queue is full");
    pending_reads.emplace(token, read);
    try {
        enqueue({"_ui.read", std::move(params), id, false, {}, {}, {}, {}, read});
    } catch (...) {
        settle_read(read, nullptr, std::current_exception());
    }
    return future;
}

std::future<Json> EditorWindow::Impl::begin_edit(const Json& params) {
    if (dialog)
        throw protocol::RpcError(-32004, "Dismiss the active dialog before editing");
    const auto token = params.at("actionId").get<std::string>();
    if (token.empty() || token.size() > 128 || pending_edits.contains(token))
        throw protocol::RpcError(-32602, "Invalid editor action identity");
    if (pending_edits.size() >= 256)
        throw protocol::RpcError(-32004, "Editor action queue is full");
    const auto extra = params.value("parameters", Json::object());
    if (!extra.is_object())
        throw protocol::RpcError(-32602, "Editor action parameters must be an object");
    const auto id = extra.value("id", active_id);
    if (!documents.contains(id))
        throw protocol::RpcError(-32001, "Editor action requires an open document");
    auto edit = std::make_shared<PendingEdit>();
    edit->input_fence = policy_inputs.fence(id, workbench->request_owner());
    auto future = edit->completion.get_future();
    pending_edits.emplace(token, edit);
    try {
        editor_command(params.at("command").get<std::string>(), extra, edit);
    } catch (...) {
        settle_edit(edit, nullptr, std::current_exception());
    }
    return future;
}

void EditorWindow::Impl::editor_command(std::string name,
                                        Json extra,
                                        std::shared_ptr<PendingEdit> edit) {
    const auto id = extra.value("id", active_id);
    const auto found = documents.find(id);
    if (found == documents.end()) {
        return;
    }
    if (!name.starts_with("editor.move_up") && !name.starts_with("editor.move_down")) {
        vertical_column.reset();
    }
    if (name != "editor.undo" && name != "editor.redo") {
        auto tab_size = editor_settings.value("tabSize", 4U);
        auto insert_spaces = editor_settings.value("insertSpaces", true);
        const auto& config = found->second.editor_config;
        if (config.contains("tab_width")) {
            tab_size = static_cast<unsigned>(std::stoul(config.at("tab_width").get<std::string>()));
        } else if (config.contains("indent_size") && config.at("indent_size") != "tab") {
            tab_size =
                static_cast<unsigned>(std::stoul(config.at("indent_size").get<std::string>()));
        }
        if (config.contains("indent_style")) {
            insert_spaces = config.at("indent_style") == "space";
        }
        if (!extra.contains("tabSize"))
            extra["tabSize"] = tab_size;
        if (!extra.contains("insertSpaces"))
            extra["insertSpaces"] = insert_spaces;
        if (!extra.contains("newline")) {
            auto ending = std::string("\n");
            if (config.contains("end_of_line")) {
                if (config.at("end_of_line") == "crlf")
                    ending = "\r\n";
            } else
                found->second.viewport.lines().for_each([&](const Line& line) {
                    if (line.complete && line.next - line.end == 2) {
                        ending = "\r\n";
                    }
                });
            extra["newline"] = std::move(ending);
        }
    }
    const bool inject_revision = !extra.contains("baseRevision");
    extra["id"] = id;
    const auto method =
        name == "editor.move_up" || name == "editor.move_down" ? "_ui.vertical" : "script.command";
    enqueue({method,
             {{"name", name}, {"params", std::move(extra)}},
             id,
             inject_revision,
             [this, id](const Json& result) {
                 if (result.is_object() && result.contains("id") && result.contains("revision")) {
                     accept_metadata(result);
                     if (active_id == id)
                         reveal_caret();
                 }
                 status = "Ready";
             },
             {},
             {},
             std::move(edit)});
}

void EditorWindow::Impl::fetch_diagnostics(const std::string& id) {
    if (!documents.contains(id) || documents.at(id).diagnostics_loading) {
        return;
    }
    documents.at(id).diagnostics_loading = true;
    const auto generation = documents.at(id).diagnostics_generation;
    enqueue({"language.diagnostics",
             {{"id", id}},
             id,
             false,
             [this, id, generation](const Json& result) {
                 if (!documents.contains(id)) {
                     return;
                 }
                 auto& document = documents.at(id);
                 document.diagnostics_loading = false;
                 document.diagnostics_after = generation == document.diagnostics_generation
                                                  ? Clock::now() + std::chrono::milliseconds(750)
                                                  : Clock::time_point{};
                 document.language_active = result.value("available", true);
                 if (result.at("revision") == document.metadata.at("revision")) {
                     document.decorations.set_diagnostics(
                         result.value("diagnostics", Json::array()));
                 }
             },
             {}});
}

void EditorWindow::Impl::set_selections(Json selections, std::string revision) {
    auto* document = current();
    if (!document) {
        return;
    }
    Json params{{"id", active_id}, {"selections", std::move(selections)}};
    if (!revision.empty()) {
        params["baseRevision"] = revision;
    }
    enqueue({"document.setSelections",
             std::move(params),
             active_id,
             revision.empty(),
             [this](const Json& result) {
                 accept_metadata(result);
                 reveal_caret();
                 status = "Ready";
             },
             {}});
}

void EditorWindow::Impl::reveal_caret() {
    auto* document = current();
    if (!document || document->metadata.at("selections").empty()) {
        return;
    }
    const auto& positions = document->metadata.contains("selectionPositions")
                                ? document->metadata["selectionPositions"]
                                : document->metadata["selections"];
    const auto line = positions.front().value("line", std::size_t{0});
    document->reveal_horizontal = true;
    const auto rows =
        static_cast<std::size_t>(std::max(1.0F, (content_bottom - content_top) / row_height));
    if (line < document->first_line) {
        document->first_line = line;
        document->needs_lines = true;
    } else if (line >= document->first_line + rows) {
        document->first_line = line - rows + 1;
        document->needs_lines = true;
    }
}

void EditorWindow::Impl::close_document(const std::string& id, bool discard) {
    const auto found = documents.find(id);
    if (found == documents.end() || closing_pending.contains(id)) {
        return;
    }
    closing_pending.insert(id);
    try {
        workbench->execute(
            "workbench.close",
            {{"id", id}, {"discardChanges", discard}},
            [this, id](const script::Runtime::Outcome& outcome) {
                closing_pending.erase(id);
                if (outcome.error) {
                    if (outcome.error->value("name", std::string{}) != "AbortError") {
                        error = outcome.error->value("message", std::string("Close failed"));
                        status = "Action failed";
                    }
                }
            });
    } catch (...) {
        closing_pending.erase(id);
        throw;
    }
}

bool EditorWindow::Impl::native_idle(bool waiting_inputs) const {
    bool inputs_idle = policy_inputs.empty() && view_inputs.empty();
    if (!inputs_idle && waiting_inputs) {
        std::array<InputQueue::Identity, 257> owners;
        std::size_t count{};
        for (const auto& [id, fence] : pending_fences) {
            static_cast<void>(id);
            owners[count++] = fence.owner;
        }
        if (dialog)
            owners[count++] = dialog->owner;
        const auto waiting = std::span(owners.data(), count);
        inputs_idle = policy_inputs.waiting_only(waiting) && view_inputs.waiting_only(waiting);
    }
    return requests.empty() && !active_request && operations.empty() && pending_edits.empty() &&
           pending_reads.empty() && closing_pending.empty() && form_events.empty() &&
           !pending_document_activity && sidebar_views.quiescent() && inputs_idle &&
           layout_jobs->idle() && !input_point && input_events.size() == 0 &&
           pending_styles.empty() && !document_composition.pending();
}

void EditorWindow::Impl::emit_form(std::string_view event,
                                   std::string_view action,
                                   InlineForm* target) {
    auto* inline_form = target ? target : forms.focused();
    if (!inline_form)
        return;
    auto state = inline_form->state();
    if (!action.empty())
        state["action"] = action;
    if (event == "ui.form.changed" && !form_events.empty() && form_events.back().first == event &&
        form_events.back().second.at("id") == inline_form->id()) {
        form_events.back().second = std::move(state);
        return;
    }
    if (form_events.size() >= 256)
        throw protocol::RpcError(-32004, "Inline event queue capacity exceeded");
    form_events.emplace_back(event, std::move(state));
}

bool EditorWindow::Impl::close_inline_form(std::string_view id) {
    auto* inline_form = id.empty() ? forms.mounted({}) : forms.find(id);
    if (!inline_form)
        return false;
    const bool queued_form = input_event_context &&
                             input_event_context->kind == InputContext::Kind::form &&
                             input_event_context->id == inline_form->id() &&
                             input_event_context->epoch == inline_form->epoch();
    if (queued_form ||
        (input_point && input_point->dialog.empty() && input_point->form == inline_form->id())) {
        clear_input_events();
    }
    emit_form("ui.form.closed", {}, inline_form);
    if (inline_form->focus())
        clear_preedit();
    clear_form_cache(*inline_form);
    static_cast<void>(forms.close(inline_form->id()));
    return true;
}

void EditorWindow::Impl::clear_form_cache(const InlineForm& form) {
    for (const auto& field : form.fields())
        form_fields.erase(form_key(form.id(), field.id));
    if (dragging_form_id == form.id()) {
        dragging_form_field.reset();
        dragging_form_id.clear();
    }
}

InlineFormLayout EditorWindow::Impl::form_layout(const InlineForm& form) const {
    if (form.view().empty())
        return InlineFormLayout::measure(geometry.editor,
                                         scale,
                                         form.fields().size(),
                                         form.toggles().size(),
                                         form.actions().size());
    if (form.view() != active_view || !show_sidebar || geometry.sidebar.width <= 0)
        return {};
    const auto top = geometry.sidebar.y + 32 * scale;
    return InlineFormLayout::measure_sidebar({geometry.sidebar.x,
                                              top,
                                              geometry.sidebar.width,
                                              std::max(0.0F, geometry.sidebar_rows.bottom() - top)},
                                             scale,
                                             form.fields().size(),
                                             form.toggles().size(),
                                             form.actions().size());
}

std::future<Json> EditorWindow::Impl::begin_fence(std::string_view method, const Json& params) {
    const auto id = params.at("waitId").get<std::string>();
    if (id.empty() || id.size() > 128 || pending_fences.contains(id) ||
        quitting_fences.contains(id))
        throw protocol::RpcError(-32602, "Invalid UI wait identity");
    if (pending_fences.size() >= 256)
        throw protocol::RpcError(-32004, "UI wait queue is full");
    PendingFence fence;
    fence.owner = workbench->request_owner();
    fence.method = method;
    auto result = fence.completion.get_future();
    pending_fences.emplace(id, std::move(fence));
    return result;
}

bool EditorWindow::Impl::finish_fences() {
    if (!native_idle(true) || dialog || pending_fences.empty())
        return false;
    constexpr std::string_view methods[]{"_ui.barrier", "_ui.window.close"};
    if (!workbench->awaiting(methods))
        return false;
    for (const auto method : {"_ui.barrier", "_ui.window.close"}) {
        // The waiting command itself owns one fence. All other script jobs and
        // transport responses must have settled before teardown is granted.
        const bool closing = std::string_view(method) == "_ui.window.close";
        const bool accepted = !closing || tabs.empty();
        for (auto it = pending_fences.begin(); it != pending_fences.end();) {
            if (it->second.method != method) {
                ++it;
                continue;
            }
            if (closing && accepted) {
                quitting_fences.insert(it->first);
                quit_requested = true;
            }
            auto pending = std::move(it->second);
            it = pending_fences.erase(it);
            pending.completion.set_value(accepted);
        }
    }
    return true;
}

std::future<Json> EditorWindow::Impl::begin_dialog(const Json& spec) {
    if (dialog || prompt != Prompt::none) {
        throw protocol::RpcError(-32004, "Another modal input is active");
    }
    const auto id = spec.at("id").get<std::string>();
    const auto kind = spec.at("kind").get<std::string>();
    const auto dynamic = spec.value("dynamic", false);
    if (dynamic && kind != "pick") {
        throw protocol::RpcError(-32602, "Dynamic providers require a picker");
    }
    if (kind != "input" && kind != "confirm" && kind != "choice" && kind != "pick" &&
        kind != "text" && kind != "progress") {
        throw protocol::RpcError(-32602, "Unknown dialog kind");
    }
    const auto title = spec.at("title").get<std::string>();
    const auto value = spec.value("value", std::string{});
    const auto detail = spec.value(kind == "text" ? "text" : "detail", std::string{});
    const auto accept = spec.value("accept", std::string("Confirm"));
    const auto placeholder = spec.value("placeholder", std::string{});
    if (spec.value("cancel", std::string{}).size() > 4096)
        throw protocol::RpcError(-32602, "Dialog cancel label exceeds its bounds");
    if (id.empty() || id.size() > 128 || title.empty() || title.size() > 4096 ||
        value.size() > 16384 || detail.size() > (kind == "text" ? 8 * 1024 * 1024U : 65536U) ||
        accept.size() > 4096 || placeholder.size() > 4096 ||
        value.find_first_of("\r\n") != value.npos || value.find('\0') != value.npos) {
        throw protocol::RpcError(-32602, "Dialog content exceeds its bounds");
    }
    Dialog pending;
    pending.id = id;
    pending.owner = workbench->request_owner();
    pending.spec = spec;
    pending.detail = std::make_unique<TextSurface>(fonts, layout_jobs);
    pending.detail->reset(detail);
    if (kind == "text")
        pending.text_panel.reset(spec, pending.detail->lines().size());
    else if (spec.contains("actions") || spec.contains("lineTones"))
        throw protocol::RpcError(-32602, "Text presentation is only supported by text panels");
    const auto items = spec.value("items", Json::array());
    if (!items.is_array() || items.size() > 4096)
        throw protocol::RpcError(-32602,
                                 "Confirmation items must be an array of at most 4096 strings");
    std::string item_text;
    for (const auto& item : items) {
        const auto label = item.get<std::string>();
        if (label.empty() || label.find_first_of("\r\n") != std::string::npos ||
            label.size() > 4096 || item_text.size() + label.size() + 1 > 65536)
            throw protocol::RpcError(-32602, "Confirmation item text exceeds its bounds");
        if (!item_text.empty())
            item_text += '\n';
        item_text += label;
    }
    if (kind == "choice") {
        if (items.size() < 2 || items.size() > 4)
            throw protocol::RpcError(-32602, "Choice dialogs require 2 to 4 choices");
        const auto selected = spec.value("defaultChoice", Json(0));
        if (!selected.is_number_integer() ||
            (selected.is_number_integer() && !selected.is_number_unsigned() &&
             selected.get<std::int64_t>() < 0) ||
            selected.get<std::uint64_t>() >= items.size())
            throw protocol::RpcError(-32602, "Invalid default choice");
        const auto keys = spec.value("keys", Json::array());
        if (!keys.is_array() || (!keys.empty() && keys.size() != items.size()))
            throw protocol::RpcError(-32602, "Choice keys must match the choices");
        std::set<std::string> used;
        for (const auto& key : keys) {
            const auto text = lower_ascii(key.get<std::string>());
            if (text.empty())
                continue;
            if (text.size() != 1 ||
                !((text[0] >= 'a' && text[0] <= 'z') || (text[0] >= '0' && text[0] <= '9')) ||
                !used.insert(text).second)
                throw protocol::RpcError(-32602,
                                         "Choice keys must be unique ASCII letters or digits");
        }
    }
    if (kind == "pick") {
        pending.picker.reset(
            items.get<std::vector<std::string>>(),
            dynamic,
            spec.value("iconPaths", Json::array()).get<std::vector<std::string>>());
        pending.picker.filter(value);
    } else if (!items.empty())
        pending.items.reset(item_text);
    if (kind == "input" || kind == "pick") {
        pending.input = std::make_unique<InlineForm>(
            Json{{"id", "native.input"},
                 {"title", "Input"},
                 {"fields", Json::array({{{"id", "value"}, {"value", value}}})},
                 {"actions", Json::array({{{"id", "accept"}, {"label", "Accept"}}})}});
        pending.input->focus(0);
        static_cast<void>(pending.input->edit(editing::Command::move_end));
        pending.input_layout = std::make_unique<TextField>(fonts, layout_jobs);
    }
    auto result = pending.completion.get_future();
    focus_view(std::nullopt);
    forms.blur();
    dialog.emplace(std::move(pending));
    prompt = kind == "input"
                 ? Prompt::policy_input
                 : (kind == "pick"
                        ? Prompt::policy_pick
                        : (kind == "text" ? Prompt::policy_text
                                          : (kind == "choice"
                                                 ? Prompt::policy_choice
                                                 : (kind == "progress" ? Prompt::policy_progress
                                                                       : Prompt::policy_confirm))));
    prompt_text = value;
    prompt_choice = kind == "choice" ? spec.value("defaultChoice", std::size_t{0}) : 0;
    clear_preedit();
    consumed_modal_text = false;
    if (dynamic)
        emit_picker_query();
    return result;
}

void EditorWindow::Impl::emit_picker_query() {
    if (!dialog || !dialog->picker.dynamic())
        return;
    std::erase_if(form_events, [&](const auto& event) {
        return event.first == "ui.picker.query" && event.second.at("id") == dialog->id;
    });
    if (form_events.size() >= 256) {
        throw protocol::RpcError(-32004, "Picker event capacity exceeded");
    }
    form_events.emplace_back("ui.picker.query",
                             Json{{"id", dialog->id},
                                  {"generation", dialog->picker.generation()},
                                  {"query", dialog->picker.query()}});
}

void EditorWindow::Impl::finish_dialog(Json value) {
    if (!dialog) {
        return;
    }
    auto pending = std::move(*dialog);
    std::erase_if(form_events, [&](const auto& event) {
        return event.first == "ui.picker.query" && event.second.at("id") == pending.id;
    });
    clear_input_events();
    dialog.reset();
    prompt = Prompt::none;
    prompt_text.clear();
    dragging_dialog_input = false;
    clear_preedit();
    pending.completion.set_value(std::move(value));
}

void EditorWindow::Impl::sync_dialog_input() {
    const auto text = dialog->input->fields().front().text.text().str();
    if (prompt_text != text) {
        prompt_choice = 0;
        if (dialog->picker.dynamic()) {
            dialog->picker.filter(text);
            emit_picker_query();
        }
    }
    prompt_text = text;
}

void EditorWindow::Impl::clear_input_events() {
    input_point.reset();
    input_events.clear();
    input_event_context.reset();
    dragging_dialog_input = false;
    dragging_form_field.reset();
    dragging_form_id.clear();
}

EditorWindow::Impl::InputContext EditorWindow::Impl::input_context() const {
    const auto field_context = [](const InlineForm& form) {
        const auto field = form.focus();
        InputContext result;
        result.kind = InputContext::Kind::form;
        result.id = form.id();
        result.epoch = form.epoch();
        result.field = field;
        if (!field) {
            return result;
        }
        if (*field < form.fields().size()) {
            const auto& text = form.fields()[*field].text;
            result.revision = text.revision();
            result.selection = text.selections().front();
        }
        return result;
    };
    if (dialog) {
        auto result = dialog->input ? field_context(*dialog->input) : InputContext{};
        result.kind = InputContext::Kind::dialog;
        result.id = dialog->id;
        return result;
    }
    if (const auto* form = forms.focused()) {
        return field_context(*form);
    }
    InputContext result;
    if (focused_view) {
        result.kind = InputContext::Kind::view;
        result.id = *focused_view;
        result.focus = view_focus_sequence;
    } else {
        result.id = active_id;
    }
    return result;
}

void EditorWindow::Impl::point_input(InlineForm& form,
                                     std::size_t field,
                                     TextField& cached,
                                     float x,
                                     float y,
                                     bool extend,
                                     std::string dialog_id) {
    const auto& document = form.fields().at(field).text;
    const auto style = input_style(dialog_id.empty() ? 13.0F : 14.0F);
    const auto* paragraph = cached.text().prepare_async(document.text().str(), style);
    if (paragraph) {
        const auto hit = paragraph->hit_test(x, y);
        form.select(field, ByteOffset{hit.byte_offset}, extend);
        if (!dialog_id.empty()) {
            sync_dialog_input();
        }
        return;
    }
    input_point = InputPoint{std::move(dialog_id),
                             form.id(),
                             form.epoch(),
                             field,
                             document.revision(),
                             document.selections().front(),
                             style,
                             x,
                             y,
                             extend,
                             Clock::now() + std::chrono::seconds(30)};
    input_event_context = input_context();
}

bool EditorWindow::Impl::complete_input_point() {
    const auto& pending = *input_point;
    InlineForm* form = nullptr;
    TextField* cached = nullptr;
    if (!pending.dialog.empty()) {
        if (dialog && dialog->id == pending.dialog) {
            form = dialog->input.get();
            cached = dialog->input_layout.get();
        }
    } else {
        form = forms.find(pending.form);
        if (form && form->epoch() == pending.epoch && pending.field < form->fields().size()) {
            const auto found =
                form_fields.find(form_key(form->id(), form->fields()[pending.field].id));
            if (found != form_fields.end()) {
                cached = &found->second;
            }
        }
    }
    if (!form || !cached || pending.field >= form->fields().size() ||
        form->focus() != pending.field) {
        throw std::runtime_error("Input focus changed before pointer selection completed");
    }
    const auto& document = form->fields()[pending.field].text;
    const auto style = input_style(pending.dialog.empty() ? 13.0F : 14.0F);
    if (document.revision() != pending.revision ||
        document.selections().front() != pending.selection || style != pending.style) {
        throw std::runtime_error(
            "Input content or style changed before pointer selection completed");
    }
    if (Clock::now() >= pending.deadline) {
        throw std::runtime_error("Input pointer selection timed out");
    }
    const auto* paragraph = cached->text().prepare_async(document.text().str(), style);
    if (!paragraph) {
        return false;
    }
    form->select(pending.field,
                 ByteOffset{paragraph->hit_test(pending.x, pending.y).byte_offset},
                 pending.extend);
    if (!pending.dialog.empty()) {
        sync_dialog_input();
    }
    input_point.reset();
    return true;
}

void EditorWindow::Impl::drain_input_events() {
    const auto deadline = Clock::now() + std::chrono::milliseconds(4);
    try {
        for (int count = 0; count < 16 && Clock::now() < deadline; ++count) {
            if (input_event_context && *input_event_context != input_context()) {
                throw std::runtime_error("Input target changed before queued input completed");
            }
            if (input_point && !complete_input_point()) {
                break;
            }
            if (input_event_context) {
                input_event_context = input_context();
            }
            if (input_events.size() == 0) {
                break;
            }
            auto stored = input_events.take();
            replaying_input = true;
            replay_input_modifiers = stored.modifiers;
            event(stored.get());
            replaying_input = false;
            replay_input_modifiers.reset();
            input_event_context = input_context();
        }
        if (!input_point && input_events.size() == 0) {
            input_event_context.reset();
        }
    } catch (const std::exception& failure) {
        replaying_input = false;
        replay_input_modifiers.reset();
        clear_input_events();
        error = failure.what();
        status = "Input could not be applied";
    }
}

bool EditorWindow::Impl::point_dialog_input(float x, float y, bool extend) {
    if (!dialog || !dialog->input) {
        return false;
    }
    const auto pixels = renderer.pixel_size();
    const auto layout = PickerLayout::measure(
        static_cast<float>(pixels.width),
        static_cast<float>(pixels.height),
        scale,
        prompt == Prompt::policy_pick
            ? std::max(dialog->picker.size(), dialog->picker.dynamic() ? std::size_t{1} : 0)
            : 0);
    if (!dragging_dialog_input && !layout.input.contains(x, y)) {
        return false;
    }
    auto& input_geometry = *dialog->input_layout;
    const auto [text_x, text_y] = input_geometry.to_text(layout.input, scale, x, y);
    point_input(*dialog->input, 0, input_geometry, text_x, text_y, extend, dialog->id);
    clear_preedit();
    return true;
}

void EditorWindow::Impl::validate_style_patch(const Json& params) const {
    if (!params.is_object()) {
        throw std::invalid_argument("Style patch requires an object");
    }
    for (const auto& [key, value] : params.items()) {
        if (key != "theme" && key != "workbench") {
            throw std::invalid_argument("Unknown style patch field: " + key);
        }
        if (!value.is_object()) {
            throw std::invalid_argument("Style theme/workbench patch requires an object");
        }
        if (key == "workbench") {
            for (const auto& [field, entry] : value.items()) {
                if (field != "uiFontSize" && field != "uiFontFamilies" &&
                    field != "uiMonospaceFontFamilies" && field != "sidebarWidth") {
                    throw std::invalid_argument("Unknown workbench style field: " + field);
                }
                if (field == "uiFontFamilies" || field == "uiMonospaceFontFamilies") {
                    if (!entry.is_array() || entry.empty() || entry.size() > 8) {
                        throw std::invalid_argument("Style font families require 1 to 8 names");
                    }
                    for (const auto& name : entry) {
                        if (!name.is_string() || name.get_ref<const std::string&>().size() > 128) {
                            throw std::invalid_argument("Style font family exceeds its limit");
                        }
                    }
                } else if (!entry.is_number()) {
                    throw std::invalid_argument("Style size and width must be numbers");
                }
            }
        } else {
            for (const auto& [field, entry] : value.items()) {
                if (!settings.at("theme").contains(field)) {
                    throw std::invalid_argument("Unknown theme style field: " + field);
                }
                if (!entry.is_string() ||
                    entry.get_ref<const std::string&>().size() > (field == "name" ? 128U : 7U)) {
                    throw std::invalid_argument("Theme field exceeds its string limit: " + field);
                }
            }
        }
    }
}

std::future<Json> EditorWindow::Impl::begin_style(const Json& params) {
    // Reject oversized/unknown data before copying it into the bounded queue.
    validate_style_patch(params.at("patch"));
    auto id = params.at("id").get<std::string>();
    if (id.empty() || id.size() > 128 ||
        std::ranges::any_of(pending_styles, [&](const auto& pending) {
            return pending.id == id;
        })) {
        throw std::invalid_argument("Invalid style update identity");
    }
    if (pending_styles.size() >= 256) {
        throw protocol::RpcError(-32004, "Style update queue is full");
    }
    PendingStyle pending{
        std::move(id), params.at("patch"), {}, Clock::now() + std::chrono::seconds(30)};
    auto result = pending.completion.get_future();
    pending_styles.push_back(std::move(pending));
    return result;
}

void EditorWindow::Impl::finish_styles() {
    // Accepted pointer/text input finishes with the font geometry it captured.
    // Modal input may remain open; a style update does not wait for its answer.
    if (input_point || input_events.size() != 0) {
        return;
    }
    const auto until = Clock::now() + std::chrono::milliseconds(4);
    for (int count = 0; count < 8 && !pending_styles.empty() && Clock::now() < until; ++count) {
        auto pending = std::move(pending_styles.front());
        pending_styles.pop_front();
        try {
            if (Clock::now() >= pending.deadline) {
                throw protocol::RpcError(-32004, "Style update timed out");
            }
            pending.completion.set_value(native_ui_request("_ui.style.update", pending.patch));
        } catch (...) {
            pending.completion.set_exception(std::current_exception());
        }
    }
}

void EditorWindow::Impl::apply_settings(Json candidate) {
    if (file_icons_override) {
        candidate["fileIcons"] = settings.at("fileIcons");
    }
    auto prepared = WorkbenchStyle::compile(candidate);
    auto editor = candidate.at("editor");
    const auto font_size = editor.at("fontSize").get<float>();
    if (!std::isfinite(font_size) || font_size < 6 || font_size > 128) {
        throw std::invalid_argument("editor.fontSize must be between 6 and 128");
    }
    const auto editor_geometry_changed = editor_settings != editor;
    const auto document_style_changed = editor_geometry_changed ||
                                        ui_style.colors.foreground != prepared.colors.foreground ||
                                        ui_style.syntax != prepared.syntax;
    candidate["workbench"]["sidebarWidth"] = prepared.sidebar_width;
    if (settings.value("fileIcons", Json::object()) != candidate.at("fileIcons")) {
        file_icons.invalidate();
    }
    settings.swap(candidate);
    editor_settings.swap(editor);
    std::swap(ui_style, prepared);
    options.font_size = font_size;
    sidebar_width = ui_style.sidebar_width;
    tab_bar.reveal();
    if (document_style_changed) {
        for (auto& [id, document] : documents) {
            static_cast<void>(id);
            document.viewport.invalidate_layouts();
            document.needs_lines = document.needs_lines || editor_geometry_changed;
        }
    }
}

Json EditorWindow::Impl::style_model() const {
    const auto& bench = settings.at("workbench");
    return {{"theme", settings.at("theme")},
            {"workbench",
             {{"uiFontSize", bench.at("uiFontSize")},
              {"uiFontFamilies", bench.at("uiFontFamilies")},
              {"uiMonospaceFontFamilies", bench.at("uiMonospaceFontFamilies")},
              {"sidebarWidth", sidebar_width}}}};
}

Json EditorWindow::Impl::native_ui_request(std::string_view method, const Json& params) {
    if (method == "_ui.icons.read") {
        return {{"theme", settings.at("fileIcons")}, {"generation", file_icons.generation()}};
    }
    if (method == "_ui.icons.update") {
        if (params.at("generation") != file_icons.generation()) {
            throw protocol::RpcError(-32001, "File icon theme changed before publication");
        }
        const auto& theme = params.at("theme");
        if (!theme.is_object() || theme.dump().size() > 256 * 1024) {
            throw protocol::RpcError(-32602, "Invalid or oversized file icon preferences");
        }
        auto candidate = settings;
        candidate["fileIcons"] = theme;
        auto reply = Json{{"theme", theme}, {"generation", ""}};
        // Prepare allocations before changing the live generation/configuration.
        const auto next = std::stoull(file_icons.generation());
        if (next == std::numeric_limits<std::uint64_t>::max()) {
            throw protocol::RpcError(-32004, "File icon generation exhausted");
        }
        reply["generation"] = std::to_string(next + 1);
        file_icons.invalidate();
        settings.swap(candidate);
        file_icons_override = true;
        return reply;
    }
    if (method == "_ui.style.cancel") {
        const auto id = params.at("id").get<std::string>();
        const auto found = std::ranges::find(pending_styles, id, &PendingStyle::id);
        if (found != pending_styles.end()) {
            // The request runtime already owns the AbortError/TimeoutError.
            // Complete the abandoned native future without a second failure.
            found->completion.set_value(nullptr);
            pending_styles.erase(found);
        }
        return nullptr;
    }
    if (method == "_ui.style.read") {
        return style_model();
    }
    if (method == "_ui.style.update") {
        validate_style_patch(params);
        // Prepare the reply before publication as well. Allocation/validation
        // failures must leave the entire current style and document intact.
        auto candidate = settings;
        candidate["workbench"]["sidebarWidth"] = sidebar_width;
        candidate.merge_patch(params);
        const auto prepared = WorkbenchStyle::compile(candidate);
        candidate["workbench"]["sidebarWidth"] = prepared.sidebar_width;
        auto reply = Json{{"theme", candidate.at("theme")},
                          {"workbench",
                           {{"uiFontSize", prepared.font_size},
                            {"uiFontFamilies", prepared.font_families},
                            {"uiMonospaceFontFamilies", prepared.monospace_families},
                            {"sidebarWidth", prepared.sidebar_width}}}};
        apply_settings(std::move(candidate));
        return reply;
    }
    if (method == "_ui.dialog.present") {
        if (!dialog || dialog->id != params.at("id").get<std::string>())
            throw protocol::RpcError(-32004, "The progress component could not be opened");
        return true;
    }
    if (method == "_ui.picker.publish") {
        if (!dialog || dialog->id != params.at("id").get<std::string>() ||
            !dialog->picker.dynamic())
            return false;
        const auto accepted = dialog->picker.publish(
            params.at("generation").get<std::string>(),
            params.at("items").get<std::vector<std::string>>(),
            params.value("message", std::string{}),
            params.value("error", false),
            params.value("iconPaths", Json::array()).get<std::vector<std::string>>());
        if (accepted)
            prompt_choice = 0;
        return accepted;
    }
    if (method == "_ui.wait.cancel") {
        const auto id = params.at("waitId").get<std::string>();
        if (quitting_fences.contains(id)) {
            quit_requested = false;
            quitting_fences.clear();
        }
        const auto found = pending_fences.find(id);
        if (found != pending_fences.end()) {
            auto pending = std::move(found->second);
            pending_fences.erase(found);
            pending.completion.set_value(false);
        }
        return nullptr;
    }
    if (method == "_ui.dialog.answer") {
        if (!dialog || dialog->id != params.at("id").get<std::string>())
            return nullptr;
        if (prompt == Prompt::policy_confirm) {
            finish_dialog(params.at("accepted").get<bool>());
            return nullptr;
        }
        if (prompt != Prompt::policy_choice)
            return nullptr;
        const auto& index = params.at("index");
        if (!index.is_number_integer() ||
            (!index.is_number_unsigned() && index.get<std::int64_t>() < 0) ||
            index.get<std::uint64_t>() >= dialog->spec.at("items").size())
            throw protocol::RpcError(-32602, "Invalid choice index");
        finish_dialog(index);
        return nullptr;
    }
    if (method == "_ui.read.cancel") {
        const auto found = pending_reads.find(params.at("readId").get<std::string>());
        if (found != pending_reads.end()) {
            const auto read = found->second;
            settle_read(read, nullptr);
        }
        return nullptr;
    }
    if (method == "_ui.dialog.cancel") {
        if (dialog && dialog->id == params.at("id").get<std::string>()) {
            finish_dialog(nullptr);
        }
        return nullptr;
    }
    if (method == "_ui.action.cancel") {
        const auto found = pending_edits.find(params.at("actionId").get<std::string>());
        if (found != pending_edits.end()) {
            const auto edit = found->second;
            cancel_edit(edit);
        }
        return nullptr;
    }
    if (method == "_ui.clipboard.read") {
        const std::unique_ptr<char, SdlStringDeleter> text(SDL_GetClipboardText());
        if (!text) {
            throw std::runtime_error(std::string("Clipboard read failed: ") + SDL_GetError());
        }
        const std::string_view value(text.get());
        if (value.size() > 8 * 1024 * 1024) {
            throw std::runtime_error("Clipboard exceeds the 8 MiB scripted clipboard limit");
        }
        return value;
    }
    if (method == "_ui.clipboard.write") {
        const auto text = params.at("text").get<std::string>();
        if (text.size() > 8 * 1024 * 1024 || text.find('\0') != std::string::npos) {
            throw std::runtime_error("Clipboard text exceeds its limit or contains NUL");
        }
        if (!SDL_SetClipboardText(text.c_str())) {
            throw std::runtime_error(std::string("Clipboard write failed: ") + SDL_GetError());
        }
        return nullptr;
    }
    if (method == "_ui.form.open") {
        if (dialog || prompt != Prompt::none)
            throw protocol::RpcError(-32004, "Dismiss modal input before opening an inline form");
        if (form_sequence == std::numeric_limits<std::uint64_t>::max())
            throw protocol::RpcError(-32004, "Inline component identity exhausted");
        auto prepared = std::make_unique<InlineForm>(params, std::to_string(++form_sequence));
        const auto id = prepared->id();
        if (!prepared->view().empty()) {
            static_cast<void>(sidebar_views.at(prepared->view()));
            if (!show_sidebar || prepared->view() != active_view)
                throw protocol::RpcError(-32004, "Show a view before mounting its form");
        }
        if (form_events.size() >= 256)
            throw protocol::RpcError(-32004, "Form event capacity exceeded");
        auto retired = forms.open(std::move(prepared));
        focus_view(std::nullopt);
        if (retired) {
            emit_form("ui.form.closed", {}, retired.get());
            clear_form_cache(*retired);
        }
        dragging_form_field.reset();
        dragging_form_id.clear();
        clear_preedit();
        if (!forms.find(id)->view().empty())
            sidebar_views.at(forms.find(id)->view()).expanded = true;
        return forms.find(id)->state();
    }
    if (method == "_ui.form.read") {
        const auto* form = forms.find(params.at("id").get<std::string>());
        return form ? form->state() : Json(nullptr);
    }
    if (method == "_ui.form.close")
        return close_inline_form(params.at("id").get<std::string>());
    if (method == "_ui.form.update") {
        auto* form = forms.find(params.at("id").get<std::string>());
        return form && form->update(params);
    }
    if (method == "_ui.form.setValues") {
        auto* form = forms.find(params.at("id").get<std::string>());
        if (!form || params.at("version") != form->version())
            return nullptr;
        if (form->set_values(params))
            emit_form("ui.form.changed", {}, form);
        return form->state();
    }
    if (method == "_ui.form.focus") {
        auto* form = forms.find(params.at("id").get<std::string>());
        if (!form)
            return nullptr;
        if (!form->view().empty() && (form->view() != active_view || !show_sidebar))
            throw protocol::RpcError(-32004, "Cannot focus a hidden form");
        const auto field = params.value("field", form->fields().front().id);
        const auto found =
            std::find_if(form->fields().begin(), form->fields().end(), [&](const auto& item) {
                return item.id == field;
            });
        if (found == form->fields().end())
            throw protocol::RpcError(-32602, "Unknown form field");
        focus_view(std::nullopt);
        forms.focus(form->id(),
                    static_cast<std::size_t>(found - form->fields().begin()),
                    params.value("selectAll", true));
        dragging_form_field.reset();
        clear_preedit();
        return form->state();
    }
    if (method == "_ui.form.blur") {
        auto* form = forms.find(params.at("id").get<std::string>());
        if (!form)
            return false;
        if (form->focus())
            clear_preedit();
        form->focus(std::nullopt);
        dragging_form_field.reset();
        return true;
    }
    if (method == "_ui.view.begin")
        return sidebar_views.begin(params.at("spec"));
    if (method == "_ui.view.append")
        return sidebar_views.append(params.at("id").get<std::string>(),
                                    params.at("version").get<std::string>(),
                                    params.at("rows"));
    if (method == "_ui.view.abort")
        return sidebar_views.abort(params.at("id").get<std::string>(),
                                   params.at("version").get<std::string>());
    if (method == "_ui.view.focus") {
        if (params.contains("focus") &&
            (!focused_view || params.at("focus") != std::to_string(view_focus_sequence) ||
             (params.contains("id") && params.at("id") != *focused_view)))
            return false;
        if (dialog || prompt != Prompt::none)
            throw protocol::RpcError(-32004, "Dismiss modal input before focusing a view");
        if (params.value("editor", false)) {
            focus_view(std::nullopt);
            forms.blur();
            clear_preedit();
            return true;
        }
        const auto id = params.value("id", active_view);
        static_cast<void>(sidebar_views.at(id));
        if (!show_sidebar || id != active_view)
            throw protocol::RpcError(-32004, "Cannot focus a hidden view");
        forms.blur();
        focus_view(id);
        static_cast<void>(sidebar_views.navigate(id, "retain", view_page()));
        auto result = sidebar_views.state(id);
        result["focused"] = true;
        result["focus"] = std::to_string(view_focus_sequence);
        return result;
    }
    if (method == "_ui.view.read" || method == "_ui.view.navigate" || method == "_ui.view.select" ||
        method == "_ui.view.reveal") {
        const auto id = params.value("id", active_view);
        const auto& view = sidebar_views.at(id);
        if ((params.contains("activity") &&
             params.at("activity") != std::to_string(document_activity)) ||
            (params.contains("version") && params.at("version") != view.version) ||
            (params.contains("focus") &&
             (!focused_view || *focused_view != id ||
              params.at("focus") != std::to_string(view_focus_sequence))))
            return nullptr;
        if (method == "_ui.view.navigate")
            return sidebar_views.navigate(id, params.at("action").get<std::string>(), view_page());
        if (method == "_ui.view.select" &&
            !sidebar_views.select(id, params.at("row").get<std::string>(), view_page()))
            return nullptr;
        if (method == "_ui.view.reveal" &&
            !sidebar_views.reveal(id, params.at("row").get<std::string>(), view_page()))
            return nullptr;
        auto result = sidebar_views.state(id);
        result["focused"] = focused_view == id;
        result["focus"] = std::to_string(view_focus_sequence);
        return result;
    }
    if (method == "_ui.read") {
        Json result{{"sidebarVisible", show_sidebar},
                    {"activity", std::to_string(document_activity)},
                    {"sidebar", active_view},
                    {"outlineVisible", show_outline},
                    {"tabs", tabs.ids()}};
        result["fileIcons"] = file_icons.state();
        if (params.contains("preferences")) {
            const auto& sections = params.at("preferences");
            if (!sections.is_array() || sections.size() > 16)
                throw protocol::RpcError(-32602, "Preference read accepts at most 16 sections");
            result["preferences"] = Json::object();
            for (const auto& section : sections) {
                if (!section.is_string() || section.get_ref<const std::string&>().size() > 128)
                    throw protocol::RpcError(-32602, "Invalid preference section");
                const auto name = section.get<std::string>();
                result["preferences"][name] = settings.value(name, Json(nullptr));
            }
        }
        result["tabLayout"] = {{"count", tabs.size()},
                               {"pending", tabs.pending()},
                               {"measurements", tabs.measurements()},
                               {"generation", std::to_string(tabs.generation())}};
        if (params.contains("tabLimit")) {
            const auto& limit = params.at("tabLimit");
            const auto offset = params.value("tabOffset", Json(0));
            if (!limit.is_number_integer() || limit.get<std::int64_t>() < 1 ||
                limit.get<std::int64_t>() > 64 || !offset.is_number_integer() ||
                offset.get<std::int64_t>() < 0) {
                throw std::invalid_argument(
                    "Tab metadata page requires a nonnegative offset and limit 1..64");
            }
            const auto first = std::min(tabs.size(), offset.get<std::size_t>());
            const auto end = first + std::min(tabs.size() - first, limit.get<std::size_t>());
            result["tabDocuments"] = Json::array();
            tabs.snapshot().for_each_range(first, end, [&](const TabIndex::Entry& entry) {
                const auto& metadata = documents.at(entry.id).metadata;
                result["tabDocuments"].push_back({{"id", entry.id},
                                                  {"revision", metadata.at("revision")},
                                                  {"dirty", metadata.at("dirty")}});
            });
        }
        result["serviceCommands"] = service_commands;
        result["focusedView"] = focused_view ? Json(*focused_view) : Json(nullptr);
        const auto* focused = forms.focused();
        result["focusedForm"] =
            focused ? Json{{"id", focused->id()}, {"view", focused->view()}} : Json(nullptr);
        const auto requested = params.value("id", active_id);
        const auto found = documents.find(requested);
        if (const auto* document = found == documents.end() ? nullptr : &found->second) {
            result["document"] = document->metadata;
            result["document"]["path"] = document->path;
            result["viewport"] = {{"firstLine", document->first_line},
                                  {"pageLines",
                                   static_cast<std::size_t>(std::max(
                                       1.0F, (content_bottom - content_top) / row_height))}};
            result["newline"] = newline(document);
            result["selectionLines"] = Json::array();
            for (const auto& position :
                 document->metadata.value("selectionPositions", Json::array())) {
                const auto* line =
                    document->viewport.lines().find(position.value("line", std::size_t{0}));
                if (!line || document->view_revision !=
                                 document->metadata.at("revision").get<std::string>()) {
                    result["selectionLines"].push_back(nullptr);
                } else {
                    result["selectionLines"].push_back(
                        {{"begin", line->begin}, {"next", line->next}});
                }
            }
        }
        return result;
    }
    if (method == "_ui.action") {
        native_command(params.at("command").get<std::string>());
        return nullptr;
    }
    if (method != "_ui.update") {
        throw protocol::RpcError(-32601, "Unknown native UI primitive");
    }
    if (!params.is_object()) {
        throw protocol::RpcError(-32602, "UI updates require an object");
    }
    if (params.contains("viewVersion") &&
        sidebar_views.at(params.at("view").get<std::string>()).version !=
            params.at("viewVersion").get<std::string>())
        return false;
    // Establish document state before activation regardless of JSON key order.
    if (params.contains("document")) {
        accept_metadata(params.at("document"));
    }
    if (params.contains("documents"))
        for (const auto& metadata : params.at("documents"))
            accept_metadata(metadata);
    for (const auto& [name, value] : params.items()) {
        if (name == "closedDocument") {
            remove_document(value.get<std::string>());
        } else if (name == "status") {
            status = value.get<std::string>();
        } else if (name == "branch" || name == "notice") {
            auto text = value.get<std::string>();
            if (text.size() > 4096 || text.find_first_of("\r\n") != text.npos ||
                text.find('\0') != text.npos)
                throw protocol::RpcError(-32602, "UI label exceeds its single-line bound");
            utf8::validate(text);
            if (name == "branch")
                workspace_branch = std::move(text);
            else
                replace_notice = std::move(text);
        } else if (name == "recoveryAvailable") {
            recovery_available = value.get<bool>();
        } else if (name == "error") {
            const auto message = value.get<std::string>();
            if (message.size() > 64 * 1024)
                throw protocol::RpcError(-32602, "UI error exceeds its 64 KiB limit");
            error = message;
        } else if (name == "sidebarVisible") {
            show_sidebar = value.get<bool>();
            if (!show_sidebar)
                focus_view(std::nullopt);
            if (!show_sidebar)
                if (const auto* focused = forms.focused(); focused && !focused->view().empty()) {
                    forms.blur();
                    clear_preedit();
                }
        } else if (name == "outlineVisible") {
            show_outline = value.get<bool>();
        } else if (name == "sidebar") {
            const auto view = value.get<std::string>();
            static_cast<void>(sidebar_views.at(view));
            active_view = view;
            if (focused_view && *focused_view != active_view)
                focus_view(std::nullopt);
            if (const auto* focused = forms.focused();
                focused && !focused->view().empty() && focused->view() != active_view) {
                forms.blur();
                clear_preedit();
            }
        } else if (name == "document" ||
                   name == "documents") { /* Applied before activation above. */
        } else if (name == "activate") {
            activate(value.get<std::string>());
        } else if (name == "languageActive") {
            if (const auto found = documents.find(value.at("id").get<std::string>());
                found != documents.end())
                found->second.language_active = value.at("active").get<bool>();
        } else if (name == "scrollLine") {
            if (const auto found = documents.find(value.at("id").get<std::string>());
                found != documents.end()) {
                found->second.first_line =
                    std::min(value.at("line").get<std::size_t>(),
                             found->second.metadata.at("lineCount").get<std::size_t>() - 1);
                found->second.needs_lines = true;
            }
        } else if (name == "revealCaret") {
            if (value.get<bool>()) {
                reveal_caret();
            }
        } else if (name == "revealCaretFor") {
            if (active_id == value.get<std::string>()) {
                reveal_caret();
            }
        } else if (name == "workspaceNotice") {
            workspace_notices[value.at("id").get<std::string>()] =
                value.at("text").get<std::string>();
        } else if (name == "gutter") {
            const auto found = documents.find(value.at("id").get<std::string>());
            if (found != documents.end() &&
                found->second.metadata.at("revision") == value.at("revision"))
                found->second.decorations.set_gutter(value.at("marks"));
        } else if (name == "dismissPrompt") {
            if (value.get<bool>()) {
                finish_dialog(nullptr);
                prompt = Prompt::none;
            }
        } else if (name == "busy") {
            busy_views.merge_patch(value);
        } else if (name != "view" && name != "viewVersion") {
            throw protocol::RpcError(-32602, "Unknown UI update field: " + name);
        }
    }
    if (auto* document = current();
        document && (params.contains("sidebarVisible") || params.contains("outlineVisible"))) {
        document->needs_lines = true;
    }
    return nullptr;
}

void EditorWindow::Impl::execute_policy(std::string_view name, const Json& params) {
    workbench->execute(name, params, [this](const script::Runtime::Outcome& outcome) {
        if (outcome.error) {
            if (outcome.error->value("name", std::string{}) != "AbortError") {
                error = outcome.error->value("message", std::string("Workbench action failed"));
                status = "Action failed";
            }
        } else if (outcome.value.is_object() && outcome.value.contains("id") &&
                   outcome.value.contains("revision") && outcome.value.contains("sizeBytes") &&
                   outcome.value.contains("lineCount") && outcome.value.contains("selections") &&
                   outcome.value.contains("selectionPositions")) {
            accept_metadata(outcome.value);
            if (active_id == outcome.value.at("id").get<std::string>()) {
                reveal_caret();
            }
        }
    });
    // Settle ready native focus/layout primitives before the next SDL event.
    // Neither tick waits for a service future, parser or disk operation.
    const auto until = Clock::now() + std::chrono::milliseconds(4);
    for (int tick = 0; tick < 8 && Clock::now() < until; ++tick) {
        workbench->poll();
        if (workbench->idle() || workbench->awaiting_input())
            break;
    }
}

void EditorWindow::Impl::view_policy(std::string name, Json parameters) {
    if (!focused_view)
        return;
    static_cast<void>(view_inputs.accept(*focused_view, std::move(name), std::move(parameters)));
    advance_view_inputs();
}

void EditorWindow::Impl::advance_view_inputs() {
    const auto until = Clock::now() + std::chrono::milliseconds(4);
    for (const auto identity : view_inputs.ready()) {
        if (Clock::now() >= until)
            break;
        const auto& input = view_inputs.at(identity);
        if (focused_view != input.document ||
            input.parameters.at("focus") != std::to_string(view_focus_sequence) ||
            input.parameters.at("version") != sidebar_views.at(input.document).version) {
            view_inputs.retire(identity);
            continue;
        }
        try {
            const auto task = workbench->execute(
                input.command,
                input.parameters,
                [this, identity](const script::Runtime::Outcome& outcome) {
                    view_inputs.retire(identity);
                    if (outcome.error &&
                        outcome.error->value("name", std::string{}) != "AbortError") {
                        error = outcome.error->value("message", std::string("View policy failed"));
                        status = "Action failed";
                    }
                });
            view_inputs.started(identity, task);
        } catch (...) {
            view_inputs.retire(identity);
            throw;
        }
    }
}

void EditorWindow::Impl::editor_policy(std::string name, Json parameters) {
    if (active_id.empty())
        return;
    static_cast<void>(policy_inputs.accept(active_id, std::move(name), std::move(parameters)));
    advance_policy_inputs();
    const auto until = Clock::now() + std::chrono::milliseconds(4);
    for (int tick = 0; tick < 8 && Clock::now() < until; ++tick) {
        workbench->poll();
        if (workbench->idle() || workbench->awaiting_input())
            break;
    }
}

void EditorWindow::Impl::advance_policy_inputs() {
    const auto until = Clock::now() + std::chrono::milliseconds(4);
    const auto ready = policy_inputs.ready();
    for (const auto identity : ready) {
        if (Clock::now() >= until)
            break;
        const auto& input = policy_inputs.at(identity);
        if (!documents.contains(input.document)) {
            policy_inputs.retire(identity);
            error = "Document closed before the accepted input was dispatched";
            status = "Action failed";
            continue;
        }
        try {
            const auto task = workbench->execute(
                input.command,
                input.parameters,
                [this, identity, id = input.document](const script::Runtime::Outcome& outcome) {
                    policy_inputs.retire(identity);
                    if (outcome.error) {
                        if (outcome.error->value("name", std::string{}) != "AbortError") {
                            error = outcome.error->value("message",
                                                         std::string("Editing policy failed"));
                            status = "Action failed";
                        }
                    } else if (outcome.value.is_object() &&
                               outcome.value.value("id", std::string{}) == id &&
                               outcome.value.contains("revision") &&
                               outcome.value.contains("sizeBytes")) {
                        accept_metadata(outcome.value);
                        if (active_id == id)
                            reveal_caret();
                    }
                });
            policy_inputs.started(identity, task);
        } catch (...) {
            policy_inputs.retire(identity);
            throw;
        }
    }
}

void EditorWindow::Impl::command(std::string_view name) {
    if ((quit_requested || prompt == Prompt::policy_progress) && name != "ui.cancel") {
        return;
    }
    if (name.starts_with("editor.")) {
        editor_policy(std::string(name));
        return;
    }
    if (service_ui_commands.contains(std::string(name))) {
        enqueue({"script.command",
                 {{"name", name}, {"params", Json::object()}},
                 active_id,
                 !active_id.empty(),
                 [this](const Json& result) {
                     if (result.is_object() && result.contains("id") &&
                         result.contains("revision")) {
                         accept_metadata(result);
                         reveal_caret();
                     }
                 },
                 {}});
    } else if (workbench->contains(name)) {
        execute_policy(name);
    } else {
        native_command(name);
    }
}

void EditorWindow::Impl::native_command(std::string_view name) {
    if (dialog && name != "ui.cancel" && name != "ui.closeWindow") {
        throw protocol::RpcError(-32004, "Dismiss the active dialog before changing its focus");
    }
    if (name.starts_with("editor.")) {
        editor_command(std::string(name));
    } else if (name == "ui.dismissWorkspaceNotice") {
        if (!watch_error.empty()) {
            watch_error.clear();
        } else if (workspace_notices.contains(active_id)) {
            workspace_notices.erase(active_id);
        } else if (!replace_notice.empty()) {
            replace_notice.clear();
        } else if (!workspace_notices.empty()) {
            workspace_notices.erase(workspace_notices.begin());
        }
    } else if (name == "ui.cancel") {
        clear_input_events();
        finish_dialog(nullptr);
        static_cast<void>(close_inline_form());
        prompt = Prompt::none;
        quit_requested = false;
        quitting_fences.clear();
        clear_preedit();
        error.clear();
    } else if (!name.starts_with("ui.")) {
        enqueue({"script.command",
                 {{"name", name}, {"params", Json::object()}},
                 active_id,
                 !active_id.empty(),
                 [this](const Json& result) {
                     if (result.is_object() && result.contains("id") &&
                         result.contains("revision")) {
                         accept_metadata(result);
                         reveal_caret();
                     }
                 },
                 {}});
    } else {
        throw std::invalid_argument("Unknown UI command: " + std::string(name));
    }
}

std::string EditorWindow::Impl::newline(const Document* document) const {
    if (!document) {
        return "\n";
    }
    if (document->editor_config.contains("end_of_line")) {
        const auto configured = document->editor_config.at("end_of_line").get<std::string>();
        if (configured == "crlf") {
            return "\r\n";
        }
        // Legacy CR files use LF internally; the workspace codec preserves CR on save.
        if (configured == "cr") {
            return "\n";
        }
        return "\n";
    }
    std::string ending = "\n";
    bool found_ending{};
    document->viewport.lines().for_each([&](const Line& line) {
        if (!found_ending && line.complete && line.next > line.end) {
            ending = line.next - line.end == 2 ? "\r\n" : "\n";
            found_ending = true;
        }
    });
    return ending;
}

void EditorWindow::Impl::accept_prompt() {
    if (prompt == Prompt::policy_input) {
        finish_dialog(prompt_text);
    } else if (prompt == Prompt::policy_confirm) {
        finish_dialog(true);
    } else if (prompt == Prompt::policy_pick && dialog) {
        dialog->picker.filter(prompt_text);
        if (dialog->picker.ready() && dialog->picker.size()) {
            const auto source =
                dialog->picker.at(std::min(prompt_choice, dialog->picker.size() - 1)).source;
            finish_dialog(dialog->picker.dynamic()
                              ? Json{{"generation", dialog->picker.generation()}, {"index", source}}
                              : Json(source));
        }
    } else if (prompt == Prompt::policy_text) {
        finish_dialog(nullptr);
    }
}

const TextLayout* EditorWindow::Impl::prepare_line(Document& document, std::size_t number) {
    if (const auto* shaped = document.viewport.layout(number)) {
        return shaped;
    }
    if (document.viewport.pending(number)) {
        return nullptr;
    }
    const auto* line = document.viewport.lines().find(number);
    if (!line) {
        return nullptr;
    }
    TextLayoutOptions style;
    style.font_size = options.font_size;
    style.dpi_scale = scale;
    style.line_height = std::clamp(editor_settings.value("lineHeight", 1.4F), 1.0F, 4.0F);
    style.tab_size = editor_settings.value("tabSize", 4U);
    style.color = ui_style.colors.foreground;
    if (document.editor_config.contains("tab_width")) {
        style.tab_size = static_cast<unsigned>(
            std::stoul(document.editor_config.at("tab_width").get<std::string>()));
    }
    if (editor_settings.contains("fontFamilies")) {
        style.font_families = editor_settings.at("fontFamilies").get<std::vector<std::string>>();
    }
    if (document.syntax_revision == document.view_revision) {
        document.decorations.highlights.intersect(
            line->begin, line->end, [&](const auto& highlight) {
                const auto begin = std::max(line->begin, highlight.begin);
                const auto end = std::min(line->end, highlight.end);
                const auto found = ui_style.syntax.find(highlight.value);
                const auto color =
                    found == ui_style.syntax.end() ? ui_style.colors.foreground : found->second;
                style.color_spans.push_back({begin - line->begin, end - line->begin, color});
            });
    }
    return document.viewport.prepare(number, style);
}

std::optional<Json> EditorWindow::Impl::vertical(std::string_view id,
                                                 bool down,
                                                 bool extend,
                                                 std::optional<std::string> revision) {
    auto* document = current();
    if (!document || id != active_id ||
        document->view_revision != document->metadata.at("revision").get<std::string>() ||
        (revision && *revision != document->view_revision))
        return std::nullopt;
    auto selections = Json::array();
    const auto& positions = document->metadata.contains("selectionPositions")
                                ? document->metadata["selectionPositions"]
                                : document->metadata["selections"];
    std::size_t i{};
    for (const auto& selection : document->metadata.at("selections")) {
        const auto number = positions[i++].value("line", std::size_t{0});
        const auto next =
            down ? std::min(number + 1, document->metadata.at("lineCount").get<std::size_t>() - 1)
                 : (number == 0 ? 0 : number - 1);
        const auto* source = document->viewport.lines().find(number);
        const auto* target = document->viewport.lines().find(next);
        const auto* source_layout = document->viewport.layout(number);
        const auto* target_layout = document->viewport.layout(next);
        if (!source || !target || !source_layout || !target_layout) {
            return std::nullopt;
        }
        const auto head = selection.at("head").get<std::size_t>();
        const auto x = source_layout->caret(std::min(head - source->begin, source->text.size())).x;
        if (!vertical_column && document->metadata.at("selections").size() == 1) {
            vertical_column = x;
        }
        const auto hit =
            target_layout->hit_test(vertical_column.value_or(x), target_layout->height() / 2);
        const auto offset = target->begin + hit.byte_offset;
        selections.push_back(
            {{"anchor", extend ? selection.at("anchor").get<std::size_t>() : offset},
             {"head", offset}});
    }
    return Json{{"id", id},
                {"baseRevision", document->view_revision},
                {"selections", std::move(selections)}};
}

void EditorWindow::Impl::focus_view(std::optional<std::string> id) {
    if (focused_view == id)
        return;
    if (view_focus_sequence == std::numeric_limits<std::uint64_t>::max())
        throw protocol::RpcError(-32004, "View focus identity exhausted");
    focused_view = std::move(id);
    focused_view_version = focused_view ? sidebar_views.at(*focused_view).version : std::string{};
    ++view_focus_sequence;
    clear_preedit();
}

std::size_t EditorWindow::Impl::view_page() const {
    return view_rows_layout().page();
}

ViewRowsLayout EditorWindow::Impl::view_rows_layout() const {
    const auto& view = sidebar_views.at(active_view);
    return ViewRowsLayout::measure(geometry.sidebar_rows,
                                   geometry.scale,
                                   geometry.sidebar_row_height,
                                   view.expanded ? view.index.size() : 0,
                                   view.scroll);
}

void EditorWindow::Impl::key(const SDL_KeyboardEvent& input) {
    refresh_bindings();
    if (input.key < consumed_modal_keys.size()) {
        if (input.repeat && consumed_modal_keys.test(input.key)) {
            consumed_modal_text = true;
            return;
        }
        if (!input.repeat)
            consumed_modal_keys.reset(input.key);
    }
    consumed_modal_text = false;
#if defined(__APPLE__)
    const bool control = (input.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0;
#else
    const bool control = (input.mod & SDL_KMOD_CTRL) != 0;
#endif
    const bool shift = (input.mod & SDL_KMOD_SHIFT) != 0;
    const bool alt = (input.mod & SDL_KMOD_ALT) != 0;
    if (prompt == Prompt::none && !control && !alt &&
        (input.key == SDLK_F6 ||
         (focused_view && (input.key == SDLK_TAB || input.key == SDLK_ESCAPE)))) {
        execute_policy(focused_view ? "ui.focusEditor" : "ui.focusSidebar");
        return;
    }
    if (input.key == SDLK_ESCAPE) {
        command("ui.cancel");
        return;
    }
    if (!preedit.empty()) {
        return;
    }
    if (prompt != Prompt::none) {
        if (prompt == Prompt::policy_choice && dialog) {
            const auto count = dialog->spec.at("items").size();
            if (input.key == SDLK_RETURN || input.key == SDLK_KP_ENTER) {
                finish_dialog(prompt_choice);
                return;
            }
            if (input.key == SDLK_RIGHT || input.key == SDLK_DOWN ||
                (input.key == SDLK_TAB && !shift))
                prompt_choice = (prompt_choice + 1) % count;
            else if (input.key == SDLK_LEFT || input.key == SDLK_UP ||
                     (input.key == SDLK_TAB && shift))
                prompt_choice = (prompt_choice + count - 1) % count;
            else if (!control && !alt) {
                const auto keys = dialog->spec.value("keys", Json::array());
                for (std::size_t i = 0; i < keys.size(); ++i) {
                    const auto key = lower_ascii(keys[i].get<std::string>());
                    if (!key.empty() && input.key == static_cast<SDL_Keycode>(key[0])) {
                        consumed_modal_text = true;
                        consumed_modal_keys.set(input.key);
                        finish_dialog(i);
                        return;
                    }
                }
            }
            return;
        }
        if (prompt == Prompt::policy_confirm) {
            if (input.key == SDLK_RETURN || input.key == SDLK_KP_ENTER) {
                finish_dialog(true);
            } else if (input.key == SDLK_DOWN)
                ++prompt_choice;
            else if (input.key == SDLK_UP && prompt_choice)
                --prompt_choice;
            return;
        }
        if (prompt == Prompt::policy_progress) {
            return;
        }
        if (prompt == Prompt::policy_text) {
            if (dialog && !control && !alt &&
                ((input.key >= SDLK_A && input.key <= SDLK_Z) ||
                 (input.key >= SDLK_0 && input.key <= SDLK_9))) {
                if (const auto* action =
                        dialog->text_panel.shortcut(static_cast<char>(input.key))) {
                    consumed_modal_text = true;
                    consumed_modal_keys.set(input.key);
                    finish_dialog(action->id);
                    return;
                }
            }
            if (input.key == SDLK_RETURN) {
                if (dialog)
                    finish_dialog(nullptr);
                else
                    prompt = Prompt::none;
            } else if (input.key == SDLK_DOWN) {
                ++prompt_choice;
            } else if (input.key == SDLK_UP && prompt_choice != 0) {
                --prompt_choice;
            } else if (dialog && input.key == SDLK_LEFT) {
                dialog->horizontal = std::max(0.0F, dialog->horizontal - 64 * scale);
            } else if (dialog && input.key == SDLK_RIGHT) {
                dialog->horizontal =
                    std::min(64 * 1024 * 1024 * scale, dialog->horizontal + 64 * scale);
            }
            return;
        }
        if (!dialog || !dialog->input) {
            throw std::logic_error("Editable dialog has no input model");
        }
        if (input.key == SDLK_RETURN || input.key == SDLK_KP_ENTER) {
            accept_prompt();
        } else if (input.key == SDLK_UP && prompt_choice != 0) {
            --prompt_choice;
        } else if (input.key == SDLK_DOWN) {
            ++prompt_choice;
        } else if (!alt) {
            text_input_key(*dialog->input, input, control, shift, [this] {
                sync_dialog_input();
            });
        }
        return;
    }

    if (inline_key(input, control, shift, alt))
        return;
    if (focused_view && !alt) {
        std::string action;
        switch (input.key) {
        case SDLK_UP:
            action = "previous";
            break;
        case SDLK_DOWN:
            action = "next";
            break;
        case SDLK_HOME:
            action = "first";
            break;
        case SDLK_END:
            action = "last";
            break;
        case SDLK_PAGEUP:
            action = "pageUp";
            break;
        case SDLK_PAGEDOWN:
            action = "pageDown";
            break;
        case SDLK_LEFT:
            if (!control)
                action = "left";
            break;
        case SDLK_RIGHT:
            if (!control)
                action = "right";
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (!control)
                action = "activate";
            break;
        default:
            break;
        }
        if (!action.empty()) {
            view_policy("ui.view." + action,
                        {{"id", *focused_view},
                         {"version", sidebar_views.at(*focused_view).version},
                         {"focus", std::to_string(view_focus_sequence)}});
            return;
        }
        if (!control)
            return;
    }
    std::string key_name;
    if (input.key >= SDLK_A && input.key <= SDLK_Z) {
        key_name.assign(1, static_cast<char>('A' + input.key - SDLK_A));
    } else {
        switch (input.key) {
        case SDLK_UP:
            key_name = "Up";
            break;
        case SDLK_DOWN:
            key_name = "Down";
            break;
        case SDLK_LEFT:
            key_name = "Left";
            break;
        case SDLK_RIGHT:
            key_name = "Right";
            break;
        case SDLK_TAB:
            key_name = "Tab";
            break;
        case SDLK_PAGEUP:
            key_name = "PageUp";
            break;
        case SDLK_PAGEDOWN:
            key_name = "PageDown";
            break;
        case SDLK_HOME:
            key_name = "Home";
            break;
        case SDLK_END:
            key_name = "End";
            break;
        case SDLK_BACKSPACE:
            key_name = "Backspace";
            break;
        case SDLK_DELETE:
            key_name = "Delete";
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            key_name = "Enter";
            break;
        case SDLK_F1:
            key_name = "F1";
            break;
        case SDLK_SPACE:
            key_name = "Space";
            break;
        case SDLK_SLASH:
            key_name = "/";
            break;
        case SDLK_F4:
            key_name = "F4";
            break;
        default:
            key_name = SDL_GetKeyName(input.key);
            break;
        }
    }
    const std::string token = (control ? "Ctrl+" : "") + std::string(shift ? "Shift+" : "") +
                              (alt ? "Alt+" : "") + key_name;
    auto binding = keybindings.find(token);
    if (binding == keybindings.end() && shift) {
        const auto unextended =
            (control ? "Ctrl+" : "") + std::string(alt ? "Alt+" : "") + key_name;
        const auto candidate = keybindings.find(unextended);
        if (candidate != keybindings.end() && candidate->second.starts_with("editor.move_")) {
            binding = candidate;
        }
    }
    if (binding != keybindings.end()) {
        if (focused_view &&
            (binding->second.starts_with("editor.") || binding->second == "ui.copy" ||
             binding->second == "ui.cut" || binding->second == "ui.paste"))
            return;
        if (binding->second.starts_with("editor.move_")) {
            editor_policy(binding->second, {{"extend", shift}});
        } else {
            command(binding->second);
        }
        return;
    }
}

bool EditorWindow::Impl::inline_key(const SDL_KeyboardEvent& input,
                                    bool control,
                                    bool shift,
                                    bool alt) {
    auto* inline_form = forms.focused();
    if (!inline_form || !inline_form->focus())
        return false;
    auto& form = *inline_form;
    if (alt && !control && input.key >= SDLK_A && input.key <= SDLK_Z) {
        const auto token = std::string("Alt+") + static_cast<char>('A' + input.key - SDLK_A);
        for (std::size_t i = 0; i < form.toggles().size(); ++i)
            if (form.toggles()[i].shortcut == token) {
                form.toggle(i);
                emit_form("ui.form.changed");
                return true;
            }
    }
    if (alt)
        return false;
    if (input.key == SDLK_TAB && !control) {
        form.cycle_focus(shift);
        clear_preedit();
        return true;
    }
    const auto focused = *form.focus();
    if (focused >= form.fields().size()) {
        if (input.key == SDLK_SPACE || input.key == SDLK_RETURN || input.key == SDLK_KP_ENTER) {
            const auto index = focused - form.fields().size();
            if (index < form.toggles().size()) {
                form.toggle(index);
                emit_form("ui.form.changed");
            } else
                emit_form("ui.form.action", form.actions()[index - form.toggles().size()].id);
            return true;
        }
        return (control && (input.key == SDLK_A || input.key == SDLK_C || input.key == SDLK_X ||
                            input.key == SDLK_V || input.key == SDLK_Z || input.key == SDLK_Y)) ||
               (!control &&
                ((input.key >= SDLK_SPACE && input.key <= SDLK_DELETE) || input.key == SDLK_UP ||
                 input.key == SDLK_DOWN || input.key == SDLK_LEFT || input.key == SDLK_RIGHT ||
                 input.key == SDLK_HOME || input.key == SDLK_END));
    }
    const auto& field = form.fields()[focused];
    if (input.key == SDLK_RETURN || input.key == SDLK_KP_ENTER) {
        const auto& action =
            shift && !field.shift_submit.empty() ? field.shift_submit : field.submit;
        if (!action.empty())
            emit_form("ui.form.action", action);
        return true;
    }
    return text_input_key(form, input, control, shift, [this] {
        emit_form("ui.form.changed");
    });
}

bool EditorWindow::Impl::text_input_key(InlineForm& form,
                                        const SDL_KeyboardEvent& input,
                                        bool control,
                                        bool shift,
                                        const std::function<void()>& changed) {
    const auto& field = form.fields()[*form.focus()];
    if (control && (input.key == SDLK_C || input.key == SDLK_X)) {
        const auto selection = field.text.selections().front();
        const auto text = field.text.text().slice(selection.begin().value, selection.end().value);
        if (!SDL_SetClipboardText(text.c_str()))
            throw std::runtime_error(SDL_GetError());
        if (input.key == SDLK_X && form.edit(editing::Command::insert_text))
            changed();
        return true;
    }
    if (control && input.key == SDLK_V) {
        const std::unique_ptr<char, SdlStringDeleter> pasted(SDL_GetClipboardText());
        if (!pasted)
            throw std::runtime_error(SDL_GetError());
        editing::Options input_options;
        input_options.text = pasted.get();
        if (form.edit(editing::Command::insert_text, input_options))
            changed();
        return true;
    }
    if (control && (input.key == SDLK_Z || input.key == SDLK_Y)) {
        if (form.history(input.key == SDLK_Y || shift))
            changed();
        return true;
    }
    std::optional<editing::Command> input_command;
    switch (input.key) {
    case SDLK_LEFT:
        input_command = control ? editing::Command::move_word_left : editing::Command::move_left;
        break;
    case SDLK_RIGHT:
        input_command = control ? editing::Command::move_word_right : editing::Command::move_right;
        break;
    case SDLK_HOME:
        input_command = editing::Command::move_home;
        break;
    case SDLK_END:
        input_command = editing::Command::move_end;
        break;
    case SDLK_BACKSPACE:
        input_command =
            control ? editing::Command::delete_word_backward : editing::Command::backspace;
        break;
    case SDLK_DELETE:
        input_command =
            control ? editing::Command::delete_word_forward : editing::Command::delete_forward;
        break;
    case SDLK_A:
        if (control)
            input_command = editing::Command::select_all;
        break;
    default:
        break;
    }
    if (!input_command)
        return !control &&
               ((input.key >= SDLK_SPACE && input.key <= SDLK_DELETE) || input.key == SDLK_UP ||
                input.key == SDLK_DOWN || input.key == SDLK_PAGEUP || input.key == SDLK_PAGEDOWN);
    editing::Options input_options;
    input_options.extend = shift;
    if (form.edit(*input_command, input_options))
        changed();
    return true;
}

void EditorWindow::Impl::point(float x, float y, bool extend, bool add) {
    auto* document = current();
    if (!document || x < left || x > right || y < content_top || y > content_bottom) {
        return;
    }
    const auto number =
        document->first_line + static_cast<std::size_t>((y - content_top) / row_height);
    const auto* found = document->viewport.lines().find(number);
    const auto* shaped = document->viewport.layout(number);
    if (!found) {
        return;
    }
    if (!shaped ||
        document->view_revision != document->metadata.at("revision").get<std::string>()) {
        const auto id = active_id;
        const auto revision = document->metadata.at("revision").get<std::string>();
        const auto click_scale = scale;
        const auto click_font_size = options.font_size;
        const auto local_x = x - left - gutter + document->horizontal;
        const auto local_y = std::max(
            0.0F, y - content_top - static_cast<float>(number - document->first_line) * row_height);
        const bool drag = dragging;
        Request request{
            "document.setSelections", Json::object(), id, false, [this](const Json& result) {
                accept_metadata(result);
                reveal_caret();
            }};
        request.ready = [this, id, revision, number, click_scale, click_font_size] {
            const auto found = documents.find(id);
            if (found == documents.end() || found->second.metadata.at("revision") != revision ||
                scale != click_scale || options.font_size != click_font_size) {
                return true;
            }
            if (found->second.view_revision != revision) {
                return false;
            }
            return !found->second.viewport.lines().find(number) ||
                   found->second.viewport.layout(number) != nullptr;
        };
        request.build_params = [this,
                                id,
                                revision,
                                number,
                                click_scale,
                                click_font_size,
                                local_x,
                                local_y,
                                extend,
                                add,
                                drag] {
            auto& document = documents.at(id);
            if (document.view_revision != revision ||
                document.metadata.at("revision") != revision || scale != click_scale ||
                options.font_size != click_font_size) {
                throw protocol::RpcError(-32002, "Pointer geometry changed before selection");
            }
            const auto* line = document.viewport.lines().find(number);
            const auto* layout = document.viewport.layout(number);
            if (!line || !layout) {
                throw protocol::RpcError(-32002, "Pointer row left the viewport");
            }
            const auto offset = line->begin + layout->hit_test(local_x, local_y).byte_offset;
            auto selections = add ? document.metadata.at("selections") : Json::array();
            const auto anchor = add ? offset
                                    : (drag ? drag_anchor
                                            : (extend ? document.metadata.at("selections")
                                                            .front()
                                                            .at("anchor")
                                                            .get<std::size_t>()
                                                      : offset));
            selections.push_back({{"anchor", anchor}, {"head", offset}});
            if (!drag && !add) {
                drag_anchor = anchor;
            }
            vertical_column.reset();
            return Json{
                {"id", id}, {"baseRevision", revision}, {"selections", std::move(selections)}};
        };
        enqueue(std::move(request));
        return;
    }
    const auto hit = shaped->hit_test(
        x - left - gutter + document->horizontal,
        std::max(0.0F,
                 y - content_top - static_cast<float>(number - document->first_line) * row_height));
    const auto offset = found->begin + hit.byte_offset;
    if (add) {
        const auto id = active_id;
        const auto revision = document->view_revision;
        enqueue({"document.setSelections",
                 Json::object(),
                 id,
                 false,
                 [this](const Json& result) {
                     accept_metadata(result);
                     reveal_caret();
                 },
                 [this, id, revision, offset] {
                     auto selections = documents.at(id).metadata.at("selections");
                     selections.push_back({{"anchor", offset}, {"head", offset}});
                     return Json{{"id", id},
                                 {"baseRevision", revision},
                                 {"selections", std::move(selections)}};
                 }});
        vertical_column.reset();
        return;
    }
    auto selections = add ? document->metadata.at("selections") : Json::array();
    const auto anchor =
        dragging
            ? drag_anchor
            : (extend ? document->metadata.at("selections").front().at("anchor").get<std::size_t>()
                      : offset);
    selections.push_back({{"anchor", anchor}, {"head", offset}});
    if (!dragging) {
        drag_anchor = anchor;
    }
    vertical_column.reset();
    set_selections(std::move(selections), document->view_revision);
}

void EditorWindow::Impl::scroll_to(float y) {
    auto* document = current();
    if (!document) {
        return;
    }
    const auto count = document->metadata.at("lineCount").get<std::size_t>();
    const auto position =
        std::clamp((y - content_top) / std::max(1.0F, content_bottom - content_top), 0.0F, 1.0F);
    document->first_line = static_cast<std::size_t>(position * static_cast<float>(count - 1));
    document->needs_lines = true;
}

void EditorWindow::Impl::event(const SDL_Event& input) {
    const auto modifiers = replay_input_modifiers.value_or(SDL_GetModState());
    int logical_width{}, logical_height{};
    if (!SDL_GetWindowSize(renderer.window(), &logical_width, &logical_height)) {
        throw std::runtime_error(SDL_GetError());
    }
    const auto pixels = renderer.pixel_size();
    const auto ratio_x = logical_width > 0
                             ? static_cast<float>(pixels.width) / static_cast<float>(logical_width)
                             : 1;
    const auto ratio_y =
        logical_height > 0 ? static_cast<float>(pixels.height) / static_cast<float>(logical_height)
                           : 1;
    if (!replaying_input && (input_point || input_events.size() != 0) &&
        InputEvents::accepts(input.type)) {
        bool cancellation = input.type == SDL_EVENT_KEY_DOWN && input.key.key == SDLK_ESCAPE;
        if (input.type == SDL_EVENT_MOUSE_BUTTON_DOWN && input.button.button == SDL_BUTTON_LEFT &&
            prompt == Prompt::none) {
            if (const auto* form = forms.focused()) {
                cancellation = form_layout(*form).close.contains(input.button.x * ratio_x,
                                                                 input.button.y * ratio_y);
            }
        }
        if (cancellation) {
            clear_input_events();
        } else {
            input_events.push(input, modifiers);
            return;
        }
    }
    if (input.type == SDL_EVENT_QUIT || input.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        clear_input_events();
    }
    auto* inline_form = forms.focused();
    if (quit_requested) {
        if (input.type == SDL_EVENT_KEY_DOWN && input.key.key == SDLK_ESCAPE)
            command("ui.cancel");
        return;
    }
    measure_layout();
    switch (input.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        command("ui.closeWindow");
        break;
    case SDL_EVENT_KEY_DOWN:
        key(input.key);
        break;
    case SDL_EVENT_KEY_UP:
        if (input.key.key < consumed_modal_keys.size())
            consumed_modal_keys.reset(input.key.key);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        consumed_modal_keys.reset();
        consumed_modal_text = false;
        clear_preedit();
        break;
    case SDL_EVENT_TEXT_EDITING:
        if (consumed_modal_text)
            break;
        set_preedit(!focused_view ? input.edit.text : nullptr);
        break;
    case SDL_EVENT_TEXT_INPUT:
        // The OS already committed this packet. Do not cancel a new composition
        // which the platform may have started after emitting it.
        clear_preedit(false);
        if (!input.text.text || focused_view || consumed_modal_text) {
            break;
        }
        if (prompt == Prompt::policy_input || prompt == Prompt::policy_pick) {
            if (!dialog || !dialog->input) {
                throw std::logic_error("Editable dialog has no input model");
            }
            editing::Options entered;
            entered.text = input.text.text;
            if (dialog->input->edit(editing::Command::insert_text, entered)) {
                sync_dialog_input();
            }
        } else if (prompt == Prompt::none) {
            if (inline_form && inline_form->focus()) {
                editing::Options input_options;
                input_options.text = input.text.text;
                if (inline_form->edit(editing::Command::insert_text, input_options))
                    emit_form("ui.form.changed");
            } else
                editor_policy("editor.insert_text", {{"text", input.text.text}});
        }
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouse_x = input.motion.x * ratio_x;
        mouse_y = input.motion.y * ratio_y;
        if (dragging_dialog_input && dialog && dialog->input) {
            static_cast<void>(point_dialog_input(mouse_x, mouse_y, true));
        } else if (dragging_sidebar) {
            sidebar_width =
                std::clamp((mouse_x - geometry.activity.right()) / scale, 180.0F, 480.0F);
        } else if (!dragging_tab.empty() && std::abs(mouse_x - tab_drag_start) > 8 * scale) {
            if (const auto hit = tab_bar.hit(mouse_x, mouse_y);
                hit &&
                (hit->target == TabBar::Target::caption || hit->target == TabBar::Target::close) &&
                tabs.position(dragging_tab) && tabs.id_at(hit->index) != dragging_tab) {
                tabs.move(dragging_tab, hit->index);
                tab_bar.reveal();
            }
        } else if (dragging_form_field && inline_form && dragging_form_id == inline_form->id()) {
            const auto index = *dragging_form_field;
            const auto boxes = form_layout(*inline_form);
            const auto cached =
                form_fields.find(form_key(inline_form->id(), inline_form->fields()[index].id));
            if (cached != form_fields.end()) {
                const auto& box = boxes.fields[index];
                const auto [text_x, text_y] = cached->second.to_text(box, scale, mouse_x, mouse_y);
                point_input(*inline_form, index, cached->second, text_x, text_y, true);
            }
        } else if (dragging_scroll) {
            scroll_to(mouse_y);
        } else if (dragging) {
            point(mouse_x, mouse_y, true, false);
        }
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (input.button.button != SDL_BUTTON_LEFT) {
            break;
        }
        consumed_modal_text = false;
        mouse_x = input.button.x * ratio_x;
        mouse_y = input.button.y * ratio_y;
        const auto width = static_cast<float>(pixels.width);
        if (point_dialog_input(mouse_x, mouse_y, (modifiers & SDL_KMOD_SHIFT) != 0)) {
            dragging_dialog_input = true;
            break;
        }
        if (prompt == Prompt::policy_text && dialog) {
            const auto layout = TextPanelLayout::measure(width,
                                                         static_cast<float>(pixels.height),
                                                         scale,
                                                         dialog->detail->lines().size(),
                                                         dialog->text_panel.actions().size());
            for (std::size_t index = 0; index < layout.actions.size(); ++index)
                if (layout.actions[index].contains(mouse_x, mouse_y)) {
                    finish_dialog(dialog->text_panel.actions()[index].id);
                    break;
                }
            break;
        }
        if (prompt == Prompt::policy_pick && dialog) {
            dialog->picker.filter(prompt_text);
            const auto layout = PickerLayout::measure(
                width,
                static_cast<float>(pixels.height),
                scale,
                std::max(dialog->picker.size(), dialog->picker.dynamic() ? std::size_t{1} : 0));
            if (layout.rows.contains(mouse_x, mouse_y) && layout.capacity &&
                dialog->picker.size()) {
                prompt_choice = std::min(prompt_choice, dialog->picker.size() - 1);
                const auto first =
                    prompt_choice >= layout.capacity ? prompt_choice - layout.capacity + 1 : 0;
                const auto row =
                    static_cast<std::size_t>((mouse_y - layout.rows.y) / layout.row_height);
                if (row < layout.capacity && first + row < dialog->picker.size()) {
                    prompt_choice = first + row;
                    accept_prompt();
                }
            }
            break;
        }
        if (prompt == Prompt::policy_choice && dialog) {
            const auto layout = ChoiceLayout::measure(
                width, static_cast<float>(pixels.height), scale, dialog->spec.at("items").size());
            for (std::size_t i = 0; i < layout.buttons.size(); ++i)
                if (layout.buttons[i].contains(mouse_x, mouse_y)) {
                    finish_dialog(i);
                    break;
                }
            break;
        }
        if (prompt == Prompt::policy_confirm || prompt == Prompt::policy_progress) {
            const auto layout = ConfirmationLayout::measure(
                width, static_cast<float>(pixels.height), scale, dialog ? dialog->items.size() : 0);
            if (prompt == Prompt::policy_confirm && layout.accept.contains(mouse_x, mouse_y))
                finish_dialog(true);
            else if (layout.cancel.contains(mouse_x, mouse_y))
                finish_dialog(nullptr);
            break;
        }
        if (prompt != Prompt::none) {
            break;
        }
        bool hit_form = false;
        for (auto* form : forms.visible(active_view, show_sidebar)) {
            if (!form)
                continue;
            const auto boxes = form_layout(*form);
            if (!boxes.panel.contains(mouse_x, mouse_y))
                continue;
            hit_form = true;
            focus_view(std::nullopt);
            if (boxes.close.contains(mouse_x, mouse_y)) {
                static_cast<void>(close_inline_form(form->id()));
                break;
            }
            clear_preedit();
            for (std::size_t i = 0; i < boxes.fields.size(); ++i)
                if (boxes.fields[i].contains(mouse_x, mouse_y)) {
                    forms.focus(form->id(), i);
                    auto& cached = form_fields
                                       .try_emplace(form_key(form->id(), form->fields()[i].id),
                                                    fonts,
                                                    layout_jobs)
                                       .first->second;
                    const auto [text_x, text_y] =
                        cached.to_text(boxes.fields[i], scale, mouse_x, mouse_y);
                    point_input(
                        *form, i, cached, text_x, text_y, (modifiers & SDL_KMOD_SHIFT) != 0);
                    dragging_form_field = i;
                    dragging_form_id = form->id();
                }
            for (std::size_t i = 0; i < boxes.toggles.size(); ++i)
                if (boxes.toggles[i].contains(mouse_x, mouse_y)) {
                    forms.focus(form->id(), form->fields().size() + i);
                    form->toggle(i);
                    emit_form("ui.form.changed", {}, form);
                }
            for (std::size_t i = 0; i < boxes.actions.size(); ++i)
                if (boxes.actions[i].contains(mouse_x, mouse_y)) {
                    forms.focus(form->id(), form->fields().size() + form->toggles().size() + i);
                    emit_form("ui.form.action", form->actions()[i].id, form);
                }
            break;
        }
        if (hit_form)
            break;
        focus_view(std::nullopt);
        forms.blur();
        clear_preedit();
        if (!workspace_notice().empty() && mouse_x >= left + 12 * scale &&
            mouse_x < right - 12 * scale && mouse_y >= content_bottom - 142 * scale &&
            mouse_y < content_bottom - 106 * scale) {
            command("ui.dismissWorkspaceNotice");
            break;
        }
        if (recovery_available && mouse_x >= left + 12 * scale && mouse_x < right - 12 * scale &&
            mouse_y >= content_bottom - 100 * scale && mouse_y < content_bottom - 64 * scale) {
            command("ui.recovery");
            break;
        }
        if (geometry.header.contains(mouse_x, mouse_y)) {
            if (geometry.command_center.contains(mouse_x, mouse_y)) {
                command("ui.commandPalette");
            }
            break;
        }
        if (geometry.sidebar_handle.contains(mouse_x, mouse_y)) {
            dragging_sidebar = true;
            break;
        }
        if (geometry.activity.contains(mouse_x, mouse_y)) {
            const auto index = static_cast<int>((mouse_y - geometry.activity.y) / (48 * scale));
            if (mouse_y > geometry.status.y - 40 * scale)
                command("ui.commandPalette");
            else {
                const auto views = sidebar_views.ordered();
                if (index >= 0 && static_cast<std::size_t>(index) < views.size())
                    execute_policy("workbench.showView",
                                   {{"id", views[static_cast<std::size_t>(index)]}});
            }
            break;
        }
        if (const auto target = WorkbenchChrome::hit_sidebar(geometry, mouse_x, mouse_y)) {
            if (*target == WorkbenchChrome::Target::outline) {
                command("ui.toggleOutline");
            } else if (*target == WorkbenchChrome::Target::refresh) {
                const auto& view = active_view;
                if (busy_views.value(view, false))
                    execute_policy("workbench.cancelView", {{"id", view}});
                else
                    execute_policy("workbench.refreshView", {{"id", view}});
            } else if (*target == WorkbenchChrome::Target::section) {
                auto& view = sidebar_views.at(active_view);
                view.expanded = !view.expanded;
                view.scroll = 0;
            }
            break;
        }
        if (geometry.outline_rows.contains(mouse_x, mouse_y)) {
            if (const auto* document = current();
                document && document->syntax_revision == document->view_revision &&
                document->outline_begin == document->outline_scroll) {
                const auto index = static_cast<std::size_t>((mouse_y - geometry.outline_rows.y) /
                                                            geometry.sidebar_row_height);
                if (index < document->outline.size()) {
                    const auto& symbol = document->outline[index];
                    set_selections(Json::array({{{"anchor", symbol.at("selectionBegin")},
                                                 {"head", symbol.at("selectionEnd")}}}),
                                   document->syntax_revision);
                }
            }
            break;
        }
        if (geometry.sidebar_rows.contains(mouse_x, mouse_y)) {
            auto& view = sidebar_views.at(active_view);
            if (!view.expanded)
                break;
            const auto row = view_rows_layout().hit(mouse_x, mouse_y);
            if (row) {
                const auto item = view.index.at(*row);
                focus_view(active_view);
                static_cast<void>(sidebar_views.select(
                    active_view, item.value->at("id").get<std::string>(), view_page()));
                if (item.group)
                    sidebar_views.toggle(active_view, *row);
                else {
                    if (form_events.size() >= 256)
                        throw protocol::RpcError(-32004, "Policy event queue capacity exceeded");
                    form_events.emplace_back(
                        "ui.view.action",
                        Json{{"id", active_view}, {"version", view.version}, {"row", *item.value}});
                }
            }
            break;
        }
        if (geometry.tabs.contains(mouse_x, mouse_y)) {
            if (const auto hit = tab_bar.hit(mouse_x, mouse_y)) {
                if (hit->target == TabBar::Target::previous ||
                    hit->target == TabBar::Target::next) {
                    tab_bar.scroll(hit->target == TabBar::Target::previous ? -1 : 1, tabs.size());
                } else {
                    const auto id = tabs.id_at(hit->index);
                    activate(id);
                    if (hit->target == TabBar::Target::close) {
                        close_document(id);
                    } else {
                        dragging_tab = id;
                        tab_drag_start = mouse_x;
                    }
                }
            }
            break;
        }
        if (mouse_x >= right - 12 * scale && mouse_x < right && mouse_y >= content_top &&
            mouse_y <= content_bottom) {
            dragging_scroll = true;
            scroll_to(mouse_y);
            break;
        }
        point(mouse_x, mouse_y, (modifiers & SDL_KMOD_SHIFT) != 0, (modifiers & SDL_KMOD_ALT) != 0);
        if (mouse_x >= left && mouse_x <= right && mouse_y >= content_top &&
            mouse_y <= content_bottom) {
            dragging = (modifiers & SDL_KMOD_ALT) == 0;
            if (input.button.clicks >= 2) {
                editor_policy("editor.select_word");
                dragging = false;
            }
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
        dragging_dialog_input = false;
        if (input.button.button == SDL_BUTTON_LEFT) {
            dragging = false;
            dragging_scroll = false;
            dragging_sidebar = false;
            dragging_form_field.reset();
            dragging_tab.clear();
        }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        mouse_x = input.wheel.mouse_x * ratio_x;
        mouse_y = input.wheel.mouse_y * ratio_y;
        const auto direction = input.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
        if (prompt != Prompt::none) {
            if (prompt == Prompt::policy_text && dialog && (modifiers & SDL_KMOD_SHIFT) != 0) {
                dialog->horizontal =
                    std::clamp(dialog->horizontal - input.wheel.y * direction * 40 * scale,
                               0.0F,
                               64 * 1024 * 1024 * scale);
                break;
            }
            const auto shift = static_cast<long long>(std::round(input.wheel.y * direction * 3));
            prompt_choice = static_cast<std::size_t>(
                std::max(0LL, static_cast<long long>(prompt_choice) - shift));
            break;
        }
        if (geometry.tabs.contains(mouse_x, mouse_y)) {
            const auto direction_step = input.wheel.y * direction < 0 ? 1 : -1;
            tab_bar.scroll(direction_step, tabs.size());
        } else if (geometry.outline_rows.contains(mouse_x, mouse_y)) {
            if (auto* document = current()) {
                const auto shift =
                    static_cast<long long>(std::round(input.wheel.y * direction * 3));
                const auto count = static_cast<std::size_t>(
                    std::ceil(geometry.outline_rows.height / geometry.sidebar_row_height));
                const auto maximum =
                    document->outline_total > count ? document->outline_total - count : 0;
                const auto next = static_cast<std::size_t>(
                    std::clamp(static_cast<long long>(document->outline_scroll) - shift,
                               0LL,
                               static_cast<long long>(maximum)));
                if (next != document->outline_scroll) {
                    document->outline_scroll = next;
                    document->needs_syntax = true;
                }
            }
        } else if (geometry.sidebar.contains(mouse_x, mouse_y)) {
            const auto shift = static_cast<long long>(std::round(input.wheel.y * direction * 3));
            auto& view = sidebar_views.at(active_view);
            view.scroll = static_cast<std::size_t>(
                std::max(0LL, static_cast<long long>(view.scroll) - shift));
        } else if (auto* document = current()) {
            if ((modifiers & SDL_KMOD_SHIFT) != 0) {
                document->horizontal =
                    std::max(0.0F, document->horizontal - input.wheel.y * direction * 40 * scale);
            } else {
                const auto shift =
                    static_cast<long long>(std::round(input.wheel.y * direction * 3));
                document->first_line =
                    std::min(static_cast<std::size_t>(std::max(
                                 0LL, static_cast<long long>(document->first_line) - shift)),
                             document->metadata.at("lineCount").get<std::size_t>() - 1);
                document->needs_lines = true;
                document->horizontal =
                    std::max(0.0F, document->horizontal - input.wheel.x * direction * 40 * scale);
            }
        }
        break;
    }
    case SDL_EVENT_DROP_FILE:
        if (input.drop.data) {
            open_path(input.drop.data);
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
        // Chrome paragraphs certify exact DPI/style keys; resizing only changes
        // their elision bounds and does not require clearing the entire LRU.
        for (auto& [id, document] : documents) {
            static_cast<void>(id);
            document.needs_lines = true;
            document.viewport.invalidate_layouts();
        }
        break;
    default:
        break;
    }
}

void EditorWindow::Impl::rectangle(
    SkCanvas* canvas, float x, float y, float width, float height, SkColor color) {
    SkPaint paint;
    paint.setColor(color);
    canvas->drawRect(SkRect::MakeXYWH(x, y, width, height), paint);
}

void EditorWindow::Impl::label(
    SkCanvas* canvas, std::string_view text, float x, float y, float size, SkColor color) {
    if (text.empty()) {
        return;
    }
    label_layout(text, size, color)->draw(canvas, x, y);
}

TextLayoutOptions EditorWindow::Impl::label_style(float size, SkColor color) const {
    return ui_style.text(size, scale, color == 0 ? ui_style.colors.foreground : color);
}

std::shared_ptr<const TextLayout>
EditorWindow::Impl::label_layout(std::string_view text, float size, SkColor color) {
    return labels.prepare(text, label_style(size, color));
}

void EditorWindow::Impl::elided_label(SkCanvas* canvas,
                                      std::string_view text,
                                      float x,
                                      float y,
                                      float size,
                                      float available,
                                      SkColor color) {
    if (const auto paragraph = labels.elide(text, label_style(size, color), available)) {
        canvas->save();
        canvas->clipRect(SkRect::MakeXYWH(x, y, available, paragraph->height()));
        paragraph->draw(canvas, x, y);
        canvas->restore();
    }
}

void EditorWindow::Impl::measure_layout(bool prepare_tabs) {
    const auto pixels = renderer.pixel_size();
    scale = renderer.dpi_scale();
    geometry = WorkbenchLayout::measure(static_cast<float>(pixels.width),
                                        static_cast<float>(pixels.height),
                                        scale,
                                        show_sidebar,
                                        sidebar_width,
                                        show_outline,
                                        sidebar_views.at(active_view).spec.value("outline", false),
                                        ui_style.font_size);
    if (const auto* form = forms.mounted(active_view); form && show_sidebar) {
        const auto panel = form_layout(*form).panel;
        const auto bottom = geometry.sidebar_rows.bottom();
        geometry.sidebar_rows.y = std::min(bottom, panel.bottom());
        geometry.sidebar_rows.height = std::max(0.0F, bottom - geometry.sidebar_rows.y);
    }
    left = geometry.editor.x;
    right = geometry.editor.right();
    content_top = geometry.editor.y;
    content_bottom = geometry.editor.bottom();
    tab_bar.measure(
        geometry.tabs, scale, tabs, tabs.position(active_id).value_or(0), ui_style, prepare_tabs);
}

void EditorWindow::Impl::draw_inline_form(SkCanvas* canvas) {
    for (const auto* form : forms.visible(active_view, show_sidebar))
        if (form)
            draw_form(canvas, *form, form_layout(*form));
}

TextLayoutOptions EditorWindow::Impl::input_style(float font_size) const {
    return ui_style.text(font_size, scale, ui_style.colors.foreground, 1.0F);
}

void EditorWindow::Impl::clear_preedit(bool cancel_native) {
    const auto had_text = !preedit.empty();
    preedit.clear();
    document_composition.reset();
    document_composition_drawn = false;
    document_composition_color = 0;
    if (cancel_native && had_text) {
        renderer.clear_composition();
    }
}

void EditorWindow::Impl::set_preedit(const char* text) {
    std::size_t size{};
    if (text) {
        while (size <= InlineForm::max_input_bytes && text[size] != '\0') {
            ++size;
        }
    }
    if (size > InlineForm::max_input_bytes) {
        throw std::length_error("IME composition exceeds its text bound");
    }
    const auto value = text ? std::string_view(text, size) : std::string_view{};
    if (value.find_first_of("\r\n") != value.npos) {
        throw std::invalid_argument("IME composition requires single-line UTF-8");
    }
    utf8::validate(value);
    if (value == preedit) {
        return;
    }
    std::string replacement(value);
    document_composition.reset();
    document_composition_drawn = false;
    document_composition_color = 0;
    preedit.swap(replacement);
}

TextLayoutOptions EditorWindow::Impl::composition_style() const {
    TextLayoutOptions style;
    style.font_size = options.font_size;
    style.dpi_scale = scale;
    style.line_height = std::clamp(editor_settings.value("lineHeight", 1.4F), 1.0F, 4.0F);
    style.color = ui_style.colors.foreground;
    style.direction = TextDirection::automatic;
    if (editor_settings.contains("fontFamilies")) {
        style.font_families = editor_settings.at("fontFamilies").get<std::vector<std::string>>();
    }
    return style;
}

void EditorWindow::Impl::input_area(float x, float y, float height) {
    int logical_width{}, logical_height{}, pixels_width{}, pixels_height{};
    if (!SDL_GetWindowSize(renderer.window(), &logical_width, &logical_height) ||
        !SDL_GetWindowSizeInPixels(renderer.window(), &pixels_width, &pixels_height) ||
        logical_width <= 0 || logical_height <= 0) {
        throw std::runtime_error(SDL_GetError());
    }
    const auto rx = static_cast<float>(pixels_width) / static_cast<float>(logical_width);
    const auto ry = static_cast<float>(pixels_height) / static_cast<float>(logical_height);
    renderer.set_text_input_area(SDL_Rect{static_cast<int>(x / rx),
                                          static_cast<int>(y / ry),
                                          2,
                                          std::max(1, static_cast<int>(height / ry))},
                                 0);
}

void EditorWindow::Impl::draw_field(SkCanvas* canvas,
                                    const InlineForm::Field& field,
                                    Box box,
                                    bool focused,
                                    bool invalid,
                                    TextField& cached,
                                    std::string_view placeholder,
                                    float font_size) {
    const auto text = field.text.text().str();
    const TextFieldPaint paint{box,
                               input_style(font_size),
                               label_style(13, ui_style.colors.muted),
                               {ui_style.colors.input,
                                ui_style.colors.border,
                                ui_style.colors.accent,
                                ui_style.colors.error,
                                ui_style.colors.selection,
                                ui_style.colors.hover},
                               text,
                               placeholder,
                               focused ? std::string_view(preedit) : std::string_view{},
                               field.text.selections().front(),
                               focused,
                               invalid};
    std::optional<Box> caret;
    try {
        caret = cached.draw(canvas, labels, paint);
    } catch (const std::exception& failure) {
        error = failure.what();
        status = "Input text unavailable";
        caret = cached.draw_unavailable(canvas, labels, paint, true);
    }
    if (caret) {
        input_area(caret->x, caret->y, caret->height);
    }
}

void EditorWindow::Impl::draw_form(SkCanvas* canvas,
                                   const InlineForm& form,
                                   const InlineFormLayout& boxes) {
    if (boxes.panel.width <= 0 || boxes.panel.height <= 0)
        return;
    rectangle(canvas,
              boxes.panel.x + 3 * scale,
              boxes.panel.y + 3 * scale,
              boxes.panel.width,
              boxes.panel.height,
              0x55000000);
    rectangle(canvas,
              boxes.panel.x,
              boxes.panel.y,
              boxes.panel.width,
              boxes.panel.height,
              ui_style.colors.border);
    rectangle(canvas,
              boxes.panel.x + scale,
              boxes.panel.y + scale,
              std::max(0.0F, boxes.panel.width - 2 * scale),
              std::max(0.0F, boxes.panel.height - 2 * scale),
              ui_style.colors.sidebar);
    canvas->save();
    canvas->clipRect(
        SkRect::MakeXYWH(boxes.panel.x, boxes.panel.y, boxes.panel.width, boxes.panel.height));
    elided_label(canvas,
                 form.title(),
                 boxes.title.x,
                 boxes.title.y,
                 12,
                 boxes.title.width,
                 ui_style.colors.foreground);
    if (boxes.close.contains(mouse_x, mouse_y))
        rectangle(canvas,
                  boxes.close.x,
                  boxes.close.y,
                  boxes.close.width,
                  boxes.close.height,
                  ui_style.colors.hover);
    draw_icon(canvas,
              Icon::close,
              boxes.close.x + 6 * scale,
              boxes.close.y + 5 * scale,
              14 * scale,
              ui_style.colors.muted);
    for (std::size_t i = 0; i < form.fields().size(); ++i) {
        const auto& field = form.fields()[i];
        const auto box = boxes.fields[i];
        const auto focused = form.focus() == i;
        auto& cached = form_fields.try_emplace(form_key(form.id(), field.id), fonts, layout_jobs)
                           .first->second;
        draw_field(canvas, field, box, focused, form.error(), cached, field.placeholder, 13);
    }
    for (std::size_t i = 0; i < form.toggles().size(); ++i) {
        const auto& toggle = form.toggles()[i];
        const auto box = boxes.toggles[i];
        const auto focused = form.focus() == form.fields().size() + i;
        rectangle(canvas,
                  box.x,
                  box.y,
                  box.width,
                  box.height,
                  toggle.value ? ui_style.colors.selection
                               : (box.contains(mouse_x, mouse_y) ? ui_style.colors.hover
                                                                 : ui_style.colors.sidebar));
        if (focused)
            rectangle(
                canvas, box.x, box.bottom() - scale, box.width, scale, ui_style.colors.accent);
        elided_label(canvas,
                     toggle.label,
                     box.x + 6 * scale,
                     box.y + 5 * scale,
                     11,
                     std::max(0.0F, box.width - 12 * scale),
                     toggle.value ? ui_style.colors.foreground : ui_style.colors.muted);
    }
    elided_label(canvas,
                 form.message(),
                 boxes.message.x,
                 boxes.message.y,
                 11,
                 boxes.message.width,
                 form.error() ? ui_style.colors.error : ui_style.colors.muted);
    for (std::size_t i = 0; i < form.actions().size(); ++i) {
        const auto box = boxes.actions[i];
        const auto focused = form.focus() == form.fields().size() + form.toggles().size() + i;
        rectangle(canvas,
                  box.x,
                  box.y,
                  box.width,
                  box.height,
                  focused ? ui_style.colors.selection
                          : (box.contains(mouse_x, mouse_y) ? ui_style.colors.hover
                                                            : ui_style.colors.input));
        elided_label(canvas,
                     form.actions()[i].label,
                     box.x + 6 * scale,
                     box.y + 7 * scale,
                     11,
                     std::max(0.0F, box.width - 12 * scale),
                     ui_style.colors.foreground);
    }
    canvas->restore();
}

void EditorWindow::Impl::draw_prompt(SkCanvas* canvas, float width, float height) {
    if (prompt == Prompt::none) {
        return;
    }
    rectangle(canvas, 0, 0, width, height, SkColorSetA(ui_style.colors.background, 160));
    if (prompt == Prompt::policy_choice && dialog) {
        const auto& choices = dialog->spec.at("items");
        const auto layout = ChoiceLayout::measure(width, height, scale, choices.size());
        rectangle(canvas,
                  layout.panel.x,
                  layout.panel.y,
                  layout.panel.width,
                  layout.panel.height,
                  ui_style.colors.panel);
        rectangle(canvas,
                  layout.panel.x,
                  layout.panel.y,
                  layout.panel.width,
                  2 * scale,
                  ui_style.colors.accent);
        elided_label(canvas,
                     dialog->spec.at("title").get<std::string>(),
                     layout.title.x,
                     layout.title.y,
                     16,
                     layout.title.width,
                     ui_style.colors.foreground);
        auto style =
            ui_style.text(13, scale, ui_style.colors.muted, 1.4F, false, layout.detail.width);
        canvas->save();
        canvas->clipRect(SkRect::MakeXYWH(
            layout.detail.x, layout.detail.y, layout.detail.width, layout.detail.height));
        float y = layout.detail.y;
        bool awaiting{};
        dialog->detail->show(0, std::min(dialog->detail->lines().size(), std::size_t{8}));
        dialog->detail->lines().for_each_range(
            0,
            std::min(dialog->detail->lines().size(), std::size_t{8}),
            [&](std::size_t row, const std::string&) {
                const auto* paragraph = dialog->detail->prepare(row, style);
                if (!paragraph) {
                    awaiting = true;
                    return;
                }
                if (awaiting || y >= layout.detail.bottom()) {
                    return;
                }
                paragraph->draw(canvas, layout.detail.x, y);
                y += paragraph->height();
            });
        canvas->restore();
        for (std::size_t i = 0; i < choices.size(); ++i) {
            const auto& box = layout.buttons[i];
            rectangle(canvas,
                      box.x,
                      box.y,
                      box.width,
                      box.height,
                      i == prompt_choice ? ui_style.colors.accent : ui_style.colors.border);
            elided_label(canvas,
                         choices[i].get<std::string>(),
                         box.x + 12 * scale,
                         box.y + 8 * scale,
                         13,
                         box.width - 24 * scale,
                         ui_style.colors.foreground);
        }
        return;
    }
    if (prompt == Prompt::policy_text && dialog) {
        const auto layout = TextPanelLayout::measure(width,
                                                     height,
                                                     scale,
                                                     dialog->detail->lines().size(),
                                                     dialog->text_panel.actions().size());
        rectangle(canvas,
                  layout.panel.x,
                  layout.panel.y,
                  layout.panel.width,
                  layout.panel.height,
                  ui_style.colors.panel);
        rectangle(canvas,
                  layout.panel.x,
                  layout.panel.y,
                  layout.panel.width,
                  2 * scale,
                  ui_style.colors.accent);
        elided_label(canvas,
                     dialog->spec.at("title").get<std::string>(),
                     layout.title.x,
                     layout.title.y,
                     11,
                     layout.title.width,
                     ui_style.colors.muted);
        auto style = ui_style.text(
            13, scale, ui_style.colors.foreground, 1.4F, dialog->spec.value("monospace", false));
        prompt_choice = std::min(prompt_choice, dialog->detail->lines().size() - 1);
        const auto count =
            static_cast<std::size_t>(std::ceil(layout.content.height / (18 * scale))) + 1;
        canvas->save();
        canvas->clipRect(SkRect::MakeXYWH(
            layout.content.x, layout.content.y, layout.content.width, layout.content.height));
        float y = layout.content.y;
        bool awaiting{};
        dialog->detail->show(prompt_choice,
                             std::min(dialog->detail->lines().size(), prompt_choice + count));
        dialog->detail->lines().for_each_range(
            prompt_choice,
            std::min(dialog->detail->lines().size(), prompt_choice + count),
            [&](std::size_t row, const std::string&) {
                switch (dialog->text_panel.tone(row)) {
                case TextPanelTone::added:
                    style.color = ui_style.colors.added;
                    break;
                case TextPanelTone::removed:
                    style.color = ui_style.colors.error;
                    break;
                case TextPanelTone::accent:
                    style.color = ui_style.colors.accent;
                    break;
                case TextPanelTone::muted:
                    style.color = ui_style.colors.muted;
                    break;
                case TextPanelTone::normal:
                    style.color = ui_style.colors.foreground;
                    break;
                }
                const auto* paragraph = dialog->detail->prepare(row, style);
                if (!paragraph) {
                    awaiting = true;
                    return;
                }
                if (awaiting || y >= layout.content.bottom()) {
                    return;
                }
                paragraph->draw(canvas, layout.content.x - dialog->horizontal, y);
                y += paragraph->height();
            });
        canvas->restore();
        for (std::size_t index = 0; index < layout.actions.size(); ++index) {
            const auto& action = dialog->text_panel.actions()[index];
            const auto& box = layout.actions[index];
            rectangle(canvas, box.x, box.y, box.width, box.height, ui_style.colors.border);
            elided_label(canvas,
                         action.label + (action.key.empty() ? "" : "  [" + action.key + "]"),
                         box.x + 8 * scale,
                         box.y + 7 * scale,
                         12,
                         std::max(0.0F, box.width - 16 * scale),
                         action.destructive ? ui_style.colors.error : ui_style.colors.foreground);
        }
        return;
    }
    if ((prompt == Prompt::policy_confirm || prompt == Prompt::policy_progress) && dialog) {
        const auto layout = ConfirmationLayout::measure(width, height, scale, dialog->items.size());
        const auto fill = [&](const Box& box, SkColor color) {
            rectangle(canvas, box.x, box.y, box.width, box.height, color);
        };
        fill(layout.panel, ui_style.colors.panel);
        rectangle(canvas,
                  layout.panel.x,
                  layout.panel.y,
                  layout.panel.width,
                  2 * scale,
                  ui_style.colors.accent);
        elided_label(canvas,
                     dialog->spec.at("title").get<std::string>(),
                     layout.title.x,
                     layout.title.y,
                     16,
                     layout.title.width,
                     ui_style.colors.foreground);
        auto style =
            ui_style.text(13, scale, ui_style.colors.muted, 1.4F, false, layout.detail.width);
        canvas->save();
        canvas->clipRect(SkRect::MakeXYWH(
            layout.detail.x, layout.detail.y, layout.detail.width, layout.detail.height));
        float detail_y = layout.detail.y;
        bool awaiting{};
        dialog->detail->show(0, std::min(dialog->detail->lines().size(), std::size_t{8}));
        dialog->detail->lines().for_each_range(
            0,
            std::min(dialog->detail->lines().size(), std::size_t{8}),
            [&](std::size_t row, const std::string&) {
                const auto* paragraph = dialog->detail->prepare(row, style);
                if (!paragraph) {
                    awaiting = true;
                    return;
                }
                if (awaiting || detail_y >= layout.detail.bottom()) {
                    return;
                }
                paragraph->draw(canvas, layout.detail.x, detail_y);
                detail_y += paragraph->height();
            });
        canvas->restore();
        if (dialog->items.size() > 0) {
            prompt_choice = std::min(prompt_choice, dialog->items.size() - 1);
            const auto count =
                static_cast<std::size_t>(std::ceil(layout.items.height / (28 * scale))) + 1;
            canvas->save();
            canvas->clipRect(SkRect::MakeXYWH(
                layout.items.x, layout.items.y, layout.items.width, layout.items.height));
            dialog->items.for_each_range(
                prompt_choice,
                std::min(dialog->items.size(), prompt_choice + count),
                [&](std::size_t row, const std::string& text) {
                    const auto y =
                        layout.items.y + static_cast<float>(row - prompt_choice) * 28 * scale;
                    elided_label(canvas,
                                 text,
                                 layout.items.x,
                                 y + 4 * scale,
                                 13,
                                 layout.items.width,
                                 ui_style.colors.foreground);
                });
            canvas->restore();
        }
        if (prompt == Prompt::policy_confirm) {
            fill(layout.accept, ui_style.colors.accent);
            elided_label(canvas,
                         dialog->spec.value("accept", std::string("Confirm")),
                         layout.accept.x + 16 * scale,
                         layout.accept.y + 8 * scale,
                         13,
                         layout.accept.width - 32 * scale,
                         ui_style.colors.foreground);
        }
        fill(layout.cancel, ui_style.colors.border);
        elided_label(canvas,
                     dialog->spec.value("cancel", std::string("Cancel")),
                     layout.cancel.x + 16 * scale,
                     layout.cancel.y + 8 * scale,
                     13,
                     layout.cancel.width - 32 * scale,
                     ui_style.colors.foreground);
        return;
    }
    if ((prompt == Prompt::policy_input || prompt == Prompt::policy_pick) && dialog) {
        const auto picking = prompt == Prompt::policy_pick;
        if (picking)
            dialog->picker.filter(prompt_text);
        const auto layout = PickerLayout::measure(
            width,
            height,
            scale,
            picking ? std::max(dialog->picker.size(), dialog->picker.dynamic() ? std::size_t{1} : 0)
                    : 0);
        rectangle(canvas,
                  layout.panel.x,
                  layout.panel.y,
                  layout.panel.width,
                  picking ? layout.panel.height
                          : layout.input.bottom() - layout.panel.y + 16 * scale,
                  ui_style.colors.panel);
        rectangle(canvas,
                  layout.panel.x,
                  layout.panel.y,
                  layout.panel.width,
                  2 * scale,
                  ui_style.colors.accent);
        const auto picker_title =
            dialog->spec.at("title").get<std::string>() +
            (picking && !dialog->picker.message().empty() ? " — " + dialog->picker.message() : "");
        elided_label(canvas,
                     picker_title,
                     layout.title.x + 4 * scale,
                     layout.title.y,
                     10,
                     layout.title.width - 8 * scale,
                     ui_style.colors.muted);
        rectangle(canvas,
                  layout.input.x,
                  layout.input.y,
                  layout.input.width,
                  layout.input.height,
                  ui_style.colors.input);
        draw_field(canvas,
                   dialog->input->fields().front(),
                   layout.input,
                   true,
                   false,
                   *dialog->input_layout,
                   dialog->spec.value("placeholder",
                                      std::string(picking ? "Type to filter; Enter to choose"
                                                          : "Type text, then press Enter")),
                   14);
        if (picking) {
            dialog->picker.filter(prompt_text);
            if (dialog->picker.size())
                prompt_choice = std::min(prompt_choice, dialog->picker.size() - 1);
            else
                prompt_choice = 0;
            const auto first = layout.capacity && prompt_choice >= layout.capacity
                                   ? prompt_choice - layout.capacity + 1
                                   : 0;
            canvas->save();
            canvas->clipRect(SkRect::MakeXYWH(
                layout.rows.x, layout.rows.y, layout.rows.width, layout.rows.height));
            dialog->picker.for_each_range(
                first,
                std::min(dialog->picker.size(), first + layout.capacity),
                [&](std::size_t row, const PickerIndex::Row& item) {
                    const auto y =
                        layout.rows.y + static_cast<float>(row - first) * layout.row_height;
                    if (row == prompt_choice)
                        rectangle(canvas,
                                  layout.rows.x,
                                  y,
                                  layout.rows.width,
                                  layout.row_height - 2 * scale,
                                  ui_style.colors.hover);
                    const auto icon_width = item.icon_path.empty() ? 0.0F : 24 * scale;
                    if (!item.icon_path.empty()) {
                        const auto icon = file_icons.lookup(item.icon_path, false);
                        draw_icon(canvas,
                                  icon.glyph,
                                  layout.rows.x + 8 * scale,
                                  y + 7 * scale,
                                  16 * scale,
                                  icon.color(ui_style.colors));
                    }
                    elided_label(canvas,
                                 item.label,
                                 layout.rows.x + 8 * scale + icon_width,
                                 y + 7 * scale,
                                 13,
                                 layout.rows.width - 16 * scale - icon_width,
                                 ui_style.colors.foreground);
                });
            if (!dialog->picker.size())
                label(canvas,
                      dialog->picker.dynamic() && !dialog->picker.message().empty()
                          ? dialog->picker.message()
                          : "No matching items",
                      layout.rows.x + 8 * scale,
                      layout.rows.y + 7 * scale,
                      13,
                      ui_style.colors.muted);
            canvas->restore();
        }
        return;
    }
    throw std::logic_error("Dialog has no matching native presentation");
}

void EditorWindow::Impl::draw() {
    document_composition_drawn = false;
    document_composition_color = 0;
    auto* canvas = renderer.begin_frame();
    if (!canvas) {
        return;
    }
    const auto pixels = renderer.pixel_size();
    const auto width = static_cast<float>(pixels.width);
    const auto height = static_cast<float>(pixels.height);
    scale = renderer.dpi_scale();
    row_height = std::ceil(options.font_size * scale *
                               std::clamp(editor_settings.value("lineHeight", 1.4F), 1.0F, 4.0F) +
                           3 * scale);
    measure_layout(true);
    gutter = 60 * scale;
    canvas->clear(ui_style.colors.background);
    const auto fill = [&](const Box& box, SkColor color) {
        rectangle(canvas, box.x, box.y, box.width, box.height, color);
    };
    chrome.paint_header(canvas,
                        geometry,
                        ui_style,
                        {"Astra", options.workspace_name, "Ctrl+Shift+P", mouse_x, mouse_y});
    fill(geometry.activity, ui_style.colors.activity);
    const auto activity_views = sidebar_views.ordered();
    canvas->save();
    canvas->clipRect(SkRect::MakeLTRB(geometry.activity.x,
                                      geometry.activity.y,
                                      geometry.activity.right(),
                                      geometry.status.y - 40 * scale));
    for (std::size_t index = 0; index < activity_views.size(); ++index) {
        const auto& view = sidebar_views.at(activity_views[index]);
        const auto y = geometry.header.bottom() + static_cast<float>(index) * 48 * scale;
        const auto selected = active_view == activity_views[index];
        if (selected && geometry.sidebar.width > 0)
            rectangle(canvas, 0, y, 2 * scale, 48 * scale, ui_style.colors.accent);
        draw_icon(canvas,
                  view_icon(view.spec.value("icon", std::string("files"))),
                  10 * scale,
                  y + 13 * scale,
                  24 * scale,
                  selected ? ui_style.colors.foreground : ui_style.colors.muted);
    }
    canvas->restore();
    draw_icon(canvas,
              Icon::settings,
              10 * scale,
              geometry.status.y - 32 * scale,
              22 * scale,
              ui_style.colors.muted);
    rectangle(canvas,
              geometry.activity.right() - scale,
              geometry.activity.y,
              scale,
              geometry.activity.height,
              ui_style.colors.border);
    painted_view_rows = 0;
    if (geometry.sidebar.width > 0) {
        fill(geometry.sidebar, ui_style.colors.sidebar);
        auto& view = sidebar_views.at(active_view);
        const auto busy = busy_views.value(active_view, false);
        const auto rows = view_rows_layout();
        view.scroll = rows.first();
        painted_view_rows = view_rows.paint(
            canvas,
            view.index,
            rows,
            ui_style,
            {view.cursor ? std::string_view(*view.cursor) : std::string_view{},
             view.empty,
             focused_view == active_view,
             view.expanded,
             mouse_x,
             mouse_y},
            [this](const ViewRow& row) -> std::optional<ViewRows::Decoration> {
                if (!row.value->contains("iconPath"))
                    return std::nullopt;
                const auto icon = file_icons.lookup(
                    row.value->at("iconPath").get_ref<const std::string&>(), row.group);
                return ViewRows::Decoration{icon.glyph, icon.color(ui_style.colors)};
            });
        chrome.paint_sidebar(canvas,
                             geometry,
                             ui_style,
                             {view.spec.at("title").get_ref<const std::string&>(),
                              view.subtitle.empty() ? options.workspace_name : view.subtitle,
                              view.spec.at("outlineTitle").get_ref<const std::string&>(),
                              view.expanded,
                              busy,
                              show_outline,
                              mouse_x,
                              mouse_y});
    }
    tab_bar.paint(
        canvas, tabs, tabs.position(active_id).value_or(0), ui_style, [&](std::size_t index) {
            const auto& document = documents.at(tabs.id_at(index));
            const auto icon = file_icons.lookup(document.path, false);
            return TabBar::Decoration{
                icon.glyph, icon.color(ui_style.colors), document.metadata.value("dirty", false)};
        });
    fill(geometry.breadcrumb, ui_style.colors.background);
    if (const auto* document = current()) {
        canvas->save();
        canvas->clipRect(SkRect::MakeXYWH(geometry.breadcrumb.x,
                                          geometry.breadcrumb.y,
                                          geometry.breadcrumb.width,
                                          geometry.breadcrumb.height));
        elided_label(canvas,
                     document->path.empty() ? "Untitled" : document->path,
                     left + 18 * scale,
                     geometry.breadcrumb.y + 5 * scale,
                     12,
                     geometry.breadcrumb.width - 36 * scale,
                     ui_style.colors.muted);
        canvas->restore();
    }
    rectangle(canvas,
              left,
              geometry.breadcrumb.bottom() - scale,
              right - left,
              scale,
              ui_style.colors.border);

    if (auto* document = current()) {
        const auto& selections = document->metadata.at("selections");
        const auto& positions = document->metadata.contains("selectionPositions")
                                    ? document->metadata["selectionPositions"]
                                    : selections;
        const auto active_line =
            positions.empty() ? std::size_t{0} : positions.front().value("line", std::size_t{0});
        canvas->save();
        canvas->clipRect(SkRect::MakeLTRB(left, content_top, right, content_bottom));
        document->viewport.lines().for_each([&](const Line& line) {
            const auto number = line.number;
            if (number < document->first_line) {
                return;
            }
            const auto y =
                content_top + static_cast<float>(number - document->first_line) * row_height;
            if (y >= content_bottom) {
                return;
            }
            const auto* shaped = prepare_line(*document, number);
            if (!shaped) {
                return;
            }
            if (document->reveal_horizontal && number == active_line && !selections.empty()) {
                const auto head = selections.front().at("head").get<std::size_t>();
                if (head >= line.begin && head <= line.end) {
                    const auto x = shaped->caret(head - line.begin).x;
                    const auto available = std::max(20 * scale, right - left - gutter - 25 * scale);
                    if (x < document->horizontal) {
                        document->horizontal = std::max(0.0F, x - 20 * scale);
                    } else if (x > document->horizontal + available) {
                        document->horizontal = x - available;
                    }
                    document->reveal_horizontal = false;
                }
            }
            if (number == active_line) {
                rectangle(canvas, left, y, right - left, row_height, ui_style.colors.current_line);
            }
            document->decorations.gutter.intersect(number, number + 1, [&](const auto& change) {
                const auto& kind = change.value;
                rectangle(canvas,
                          left + 3 * scale,
                          y,
                          3 * scale,
                          kind == "deleted" ? 4 * scale : row_height,
                          kind == "added" ? ui_style.colors.added
                                          : (kind == "deleted" ? ui_style.colors.error
                                                               : ui_style.colors.changed));
            });
            label(canvas,
                  std::to_string(number + 1),
                  left + 15 * scale,
                  y + 3 * scale,
                  12,
                  number == active_line ? ui_style.colors.foreground : ui_style.colors.muted);
            const auto text_x = left + gutter - document->horizontal;
            canvas->save();
            canvas->clipRect(SkRect::MakeLTRB(left + gutter - 5 * scale, y, right, y + row_height));
            for (const auto& selection : selections) {
                const auto anchor = selection.at("anchor").get<std::size_t>();
                const auto head = selection.at("head").get<std::size_t>();
                const auto begin = std::max(std::min(anchor, head), line.begin);
                const auto end = std::min(std::max(anchor, head), line.end);
                if (begin < end) {
                    for (const auto& box :
                         shaped->selection_rects(begin - line.begin, end - line.begin)) {
                        rectangle(canvas,
                                  text_x + box.x,
                                  y + box.y,
                                  box.width,
                                  box.height,
                                  ui_style.colors.selection);
                    }
                }
                if (std::min(anchor, head) <= line.end && std::max(anchor, head) > line.end &&
                    line.complete) {
                    rectangle(canvas,
                              text_x + shaped->width(),
                              y,
                              8 * scale,
                              row_height,
                              ui_style.colors.selection);
                }
            }
            shaped->draw(canvas, text_x, y);
            document->decorations.diagnostics.intersect(
                number, number + 1, [&](const auto& diagnostic) {
                    const auto begin =
                        number == diagnostic.begin
                            ? shaped->byte_offset_from_utf16(diagnostic.value.start_utf16)
                            : 0;
                    const auto end =
                        number == diagnostic.end - 1
                            ? shaped->byte_offset_from_utf16(diagnostic.value.end_utf16)
                            : line.text.size();
                    const auto color = diagnostic.value.severity == 1 ? ui_style.colors.error
                                                                      : ui_style.colors.warning;
                    for (const auto& box : shaped->selection_rects(begin, end)) {
                        rectangle(canvas,
                                  text_x + box.x,
                                  y + box.y + box.height - 2 * scale,
                                  std::max(scale, box.width),
                                  scale,
                                  color);
                    }
                });
            for (std::size_t index = 0; index < selections.size(); ++index) {
                const auto head = selections[index].at("head").get<std::size_t>();
                if (head < line.begin || head > line.end) {
                    continue;
                }
                const auto caret = shaped->caret(head - line.begin);
                const auto cursor_x = text_x + caret.x;
                const auto cursor_y = y + caret.y;
                if (!editor_settings.value("cursorBlink", true) ||
                    (SDL_GetTicks() / 550) % 2 == 0 || !requests.empty() || active_request) {
                    rectangle(canvas,
                              cursor_x,
                              cursor_y,
                              std::max(1.0F, 1.5F * scale),
                              caret.height,
                              ui_style.colors.cursor);
                }
                if (index == 0) {
                    int logical_width{}, logical_height{};
                    if (SDL_GetWindowSize(renderer.window(), &logical_width, &logical_height) &&
                        logical_width > 0 && logical_height > 0) {
                        const auto rx = width / static_cast<float>(logical_width);
                        const auto ry = height / static_cast<float>(logical_height);
                        renderer.set_text_input_area(
                            SDL_Rect{static_cast<int>(cursor_x / rx),
                                     static_cast<int>(cursor_y / ry),
                                     2,
                                     std::max(1, static_cast<int>(caret.height / ry))},
                            0);
                    }
                    if (!preedit.empty() && prompt == Prompt::none && !forms.focused()) {
                        const TextLayout* composition{};
                        std::uint32_t composition_color{};
                        bool composition_failed{};
                        try {
                            const auto style = composition_style();
                            composition = document_composition.prepare_async(preedit, style);
                            composition_color = style.color;
                        } catch (const std::exception& failure) {
                            error = std::string("IME preview failed: ") + failure.what();
                            composition_failed = true;
                        }
                        if (!composition) {
                            rectangle(canvas,
                                      cursor_x,
                                      cursor_y + caret.height - scale,
                                      4 * scale,
                                      scale,
                                      composition_failed ? ui_style.colors.error
                                                         : ui_style.colors.accent);
                            continue;
                        }
                        document_composition_drawn = true;
                        document_composition_color = composition_color;
                        rectangle(canvas,
                                  cursor_x,
                                  cursor_y,
                                  composition->width() + 4 * scale,
                                  composition->height(),
                                  ui_style.colors.background);
                        composition->draw(canvas, cursor_x, cursor_y);
                        rectangle(canvas,
                                  cursor_x,
                                  cursor_y + composition->height() - scale,
                                  composition->width(),
                                  scale,
                                  ui_style.colors.accent);
                    }
                }
            }
            if (!line.complete) {
                label(canvas,
                      "  … loading long line",
                      text_x + shaped->width(),
                      y + 3 * scale,
                      12,
                      ui_style.colors.muted);
            }
            canvas->restore();
        });
        canvas->restore();
        const auto lines = document->metadata.at("lineCount").get<std::size_t>();
        const auto rows = std::max(1.0F, (content_bottom - content_top) / row_height);
        if (static_cast<float>(lines) > rows) {
            const auto track = content_bottom - content_top;
            const auto thumb = std::max(28 * scale, track * rows / static_cast<float>(lines));
            const auto y =
                content_top + (track - thumb) * static_cast<float>(document->first_line) /
                                  static_cast<float>(std::max(std::size_t{1}, lines - 1));
            rectangle(canvas, right - 7 * scale, y, 5 * scale, thumb, ui_style.colors.scrollbar);
        }
    } else {
        label(canvas,
              "ASTRA",
              left + 85 * scale,
              content_top + 96 * scale,
              40,
              ui_style.colors.muted);
        label(canvas,
              "A focused place to write code.",
              left + 88 * scale,
              content_top + 159 * scale,
              15,
              ui_style.colors.muted);
        label(canvas,
              "New file        Ctrl+N",
              left + 90 * scale,
              content_top + 211 * scale,
              13,
              ui_style.colors.muted);
        label(canvas,
              "Open file       Ctrl+P",
              left + 90 * scale,
              content_top + 248 * scale,
              13,
              ui_style.colors.muted);
        label(canvas,
              "Commands        Ctrl+Shift+P",
              left + 90 * scale,
              content_top + 285 * scale,
              13,
              ui_style.colors.muted);
    }

    if (geometry.outline_rows.height > 0 && geometry.sidebar.width > 0) {
        if (const auto* document = current()) {
            const auto ox = geometry.outline_rows.x + 16 * scale, oy = geometry.outline_rows.y;
            canvas->save();
            canvas->clipRect(SkRect::MakeXYWH(geometry.outline_rows.x,
                                              oy,
                                              geometry.outline_rows.width,
                                              geometry.outline_rows.height));
            if (document->syntax_revision != document->view_revision ||
                document->syntax_state == "pending" ||
                document->outline_begin != document->outline_scroll) {
                label(canvas, "Parsing document...", ox, oy + 6 * scale, 12, ui_style.colors.muted);
            } else if (document->outline.empty()) {
                label(canvas,
                      document->syntax_state == "unsupported" ? "No parser for this file type"
                                                              : "No symbols",
                      ox,
                      oy + 6 * scale,
                      12,
                      ui_style.colors.muted);
            } else
                for (std::size_t i = 0; i < document->outline.size(); ++i) {
                    const auto y = oy + static_cast<float>(i) * geometry.sidebar_row_height;
                    if (y >= geometry.outline_rows.bottom()) {
                        break;
                    }
                    const auto& symbol = document->outline[i];
                    const auto depth = std::min(symbol.value("depth", 0U), 8U);
                    const auto x = ox + static_cast<float>(depth) * 10 * scale;
                    draw_icon(canvas,
                              symbol.at("kind") == "function" ? Icon::function : Icon::symbol,
                              x,
                              y + 4 * scale,
                              16 * scale,
                              ui_style.colors.accent);
                    label(canvas,
                          symbol.at("name").get<std::string>(),
                          x + 22 * scale,
                          y + 4 * scale,
                          13,
                          ui_style.colors.foreground);
                }
            canvas->restore();
        }
    }
    fill(geometry.status, ui_style.colors.status);
    rectangle(canvas, 0, geometry.status.y, width, scale, ui_style.colors.border);
    const auto status_y = geometry.status.y + 6 * scale;
    draw_icon(canvas,
              Icon::branch,
              12 * scale,
              geometry.status.y + 4 * scale,
              16 * scale,
              ui_style.colors.status_foreground);
    const auto branch = workspace_branch;
    elided_label(
        canvas, branch, 34 * scale, status_y, 11, 90 * scale, ui_style.colors.status_foreground);
    canvas->save();
    canvas->clipRect(SkRect::MakeXYWH(130 * scale,
                                      geometry.status.y,
                                      std::max(0.0F, width - 600 * scale),
                                      geometry.status.height));
    label(canvas, status, 140 * scale, status_y, 11, ui_style.colors.status_foreground);
    canvas->restore();
    if (const auto* document = current()) {
        const auto& positions = document->metadata.contains("selectionPositions")
                                    ? document->metadata["selectionPositions"]
                                    : document->metadata["selections"];
        const auto line =
            positions.empty() ? std::size_t{0} : positions.front().value("line", std::size_t{0});
        auto column = positions.empty() ? std::size_t{0}
                                        : positions.front().value("utf16Column", std::size_t{0});
        std::string unit = "UTF16 Col ";
        auto tab_size = editor_settings.value("tabSize", 4U);
        if (document->editor_config.contains("tab_width"))
            tab_size = static_cast<unsigned>(
                std::stoul(document->editor_config.at("tab_width").get<std::string>()));
        const auto* found = document->viewport.lines().find(line);
        const auto* shaped = document->viewport.layout(line);
        if (found && shaped) {
            const auto head =
                document->metadata.at("selections").front().at("head").get<std::size_t>();
            if (head >= found->begin && head <= found->end) {
                const auto advance = shaped->tab_stop_width() / static_cast<float>(tab_size);
                if (advance > 0) {
                    column = static_cast<std::size_t>(
                        std::round(shaped->caret(head - found->begin).x / advance));
                    unit = "Col ";
                }
            }
        }
        const auto spaces =
            document->editor_config.value(
                "indent_style",
                std::string(editor_settings.value("insertSpaces", true) ? "space" : "tab")) ==
            "space";
        auto detail = "Ln " + std::to_string(line + 1) + ", " + unit + std::to_string(column + 1);
        if (width > 500 * scale)
            detail += "    " + document->encoding;
        if (width > 820 * scale)
            detail += "    " + std::string(spaces ? "Spaces: " : "Tab Size: ") +
                      std::to_string(tab_size) + (document->language_active ? "    LSP" : "");
        const auto detail_width =
            label_layout(detail, 11, ui_style.colors.status_foreground)->width();
        label(canvas,
              detail,
              std::max(130 * scale, width - detail_width - 16 * scale),
              status_y,
              11,
              ui_style.colors.status_foreground);
    }
    if (recovery_available) {
        rectangle(canvas,
                  left + 12 * scale,
                  content_bottom - 100 * scale,
                  std::max(100 * scale, right - left - 24 * scale),
                  36 * scale,
                  ui_style.colors.recovery_background);
        label(canvas,
              "Unsaved recovery sessions are available — click to review",
              left + 24 * scale,
              content_bottom - 91 * scale,
              12,
              ui_style.colors.foreground);
    }
    if (const auto notice = workspace_notice(); !notice.empty()) {
        rectangle(canvas,
                  left + 12 * scale,
                  content_bottom - 142 * scale,
                  std::max(100 * scale, right - left - 24 * scale),
                  36 * scale,
                  ui_style.colors.notice_background);
        canvas->save();
        canvas->clipRect(SkRect::MakeLTRB(left + 24 * scale,
                                          content_bottom - 142 * scale,
                                          right - 44 * scale,
                                          content_bottom - 106 * scale));
        label(canvas,
              notice,
              left + 24 * scale,
              content_bottom - 133 * scale,
              12,
              ui_style.colors.warning);
        canvas->restore();
        label(canvas,
              "×",
              right - 34 * scale,
              content_bottom - 133 * scale,
              13,
              ui_style.colors.warning);
    }
    if (!error.empty()) {
        rectangle(canvas,
                  left + 12 * scale,
                  content_bottom - 58 * scale,
                  std::max(100 * scale, right - left - 24 * scale),
                  46 * scale,
                  ui_style.colors.error_background);
        label(canvas,
              error,
              left + 24 * scale,
              content_bottom - 44 * scale,
              12,
              ui_style.colors.foreground);
    }
    draw_inline_form(canvas);
    draw_prompt(canvas, width, height);
    if (screenshot) {
        renderer.capture_png(*screenshot);
        screenshot.reset();
    }
    renderer.end_frame();
}

EditorWindow::EditorWindow(std::shared_ptr<EditorChannel> channel, EditorWindowOptions options)
    : impl_(std::make_unique<Impl>(std::move(channel), std::move(options))) {
}

EditorWindow::~EditorWindow() = default;

SDL_Window* EditorWindow::window() const noexcept {
    return impl_->renderer.window();
}

void EditorWindow::request_screenshot(std::filesystem::path destination) {
    impl_->screenshot = std::move(destination);
}

void EditorWindow::execute_ui_command(std::string_view command) {
    impl_->command(command);
}

void EditorWindow::open_path(std::string path) {
    impl_->open_path(std::move(path));
}

std::string EditorWindow::active_document_id() const {
    return impl_->active_id;
}

bool EditorWindow::idle() const noexcept {
    const auto* document = impl_->current();
    return !impl_->quit_requested && impl_->tabs.pending() == 0 && impl_->file_icons.idle() &&
           impl_->pending_fences.empty() && impl_->native_idle(impl_->dialog.has_value()) &&
           (!impl_->dialog || !impl_->dialog->picker.dynamic() || impl_->dialog->picker.ready()) &&
           (impl_->workbench->idle() || (impl_->dialog && impl_->workbench->awaiting_input())) &&
           (!document || (!document->needs_lines && !document->loading && !document->needs_syntax &&
                          !document->syntax_loading));
}

std::string EditorWindow::last_error() const {
    return impl_->error;
}

std::string EditorWindow::status_text() const {
    return impl_->status;
}

std::string EditorWindow::workspace_notice() const {
    return impl_->workspace_notice();
}

WorkbenchLayout EditorWindow::layout() const {
    return impl_->geometry;
}

WorkbenchViewState EditorWindow::view_state() const {
    const auto retired = impl_->sidebar_views.retirement_stats();
    const auto label_stats = impl_->labels.statistics();
    WorkbenchViewState result{impl_->geometry,
                              impl_->tab_bar.geometry(),
                              impl_->tabs.ids(),
                              impl_->sidebar_views.at("explorer").index.size()};
    const auto& sidebar = impl_->sidebar_views.at(impl_->active_view);
    result.style = impl_->style_model();
    result.file_icons = impl_->file_icons.state();
    auto visible_icons = Json::array();
    const auto rows = impl_->view_rows_layout();
    sidebar.index.for_each_range(rows.first(), rows.end(), [&](std::size_t, const ViewRow& row) {
        if (!row.value->contains("iconPath"))
            return;
        const auto path = row.value->at("iconPath").get<std::string>();
        if (const auto icon = impl_->file_icons.peek(path, row.group)) {
            visible_icons.push_back(
                {{"path", path}, {"glyph", static_cast<int>(icon->glyph)}, {"tone", icon->tone}});
        }
    });
    result.file_icons["visibleRows"] = std::move(visible_icons);
    result.tab_layout = {{"count", impl_->tabs.size()},
                         {"pending", impl_->tabs.pending()},
                         {"measurements", impl_->tabs.measurements()},
                         {"generation", std::to_string(impl_->tabs.generation())},
                         {"relabels", impl_->tabs.relabels()},
                         {"maxMeasuredPass", impl_->tab_bar.max_measured_pass()},
                         {"height", impl_->tabs.snapshot().height()}};
    result.file_first = sidebar.scroll;
    result.text_labels = {{"entries", label_stats.entries},
                          {"bytes", label_stats.accounted_bytes},
                          {"shaped", label_stats.shaped},
                          {"reused", label_stats.reused},
                          {"evicted", label_stats.evicted}};
    result.sidebar = impl_->sidebar_views.state(impl_->active_view);
    result.sidebar["viewport"] = {{"first", rows.first()},
                                  {"end", rows.end()},
                                  {"page", rows.page()},
                                  {"painted", impl_->painted_view_rows}};
    result.view_retirement = {{"queued", retired.queued},
                              {"completed", retired.completed},
                              {"reserved", retired.reserved},
                              {"bytes", retired.bytes}};
    result.input_layout = {{"pointerPending", impl_->input_point.has_value()},
                           {"documentCompositionPending", impl_->document_composition.pending()},
                           {"documentCompositionDrawn", impl_->document_composition_drawn},
                           {"documentCompositionColor", impl_->document_composition_color},
                           {"documentCompositionBytes",
                            impl_->prompt == Impl::Prompt::none && !impl_->forms.focused() &&
                                    !impl_->focused_view && impl_->current()
                                ? impl_->preedit.size()
                                : 0},
                           {"queuedEvents", impl_->input_events.size()},
                           {"queuedBytes", impl_->input_events.bytes()},
                           {"workerSubmitted", impl_->layout_jobs->statistics().submitted},
                           {"workerJobs", impl_->layout_jobs->statistics().jobs}};
    std::size_t pending_fields{};
    std::size_t composing_fields{};
    for (const auto& [id, cached] : impl_->form_fields) {
        static_cast<void>(id);
        pending_fields += cached.pending() ? 1 : 0;
        composing_fields += cached.composition_pending() ? 1 : 0;
    }
    result.input_layout["pendingFields"] = pending_fields;
    result.input_layout["pendingStyles"] = impl_->pending_styles.size();
    result.input_layout["compositionPendingFields"] = composing_fields;
    if (impl_->prompt == Impl::Prompt::policy_pick && impl_->dialog) {
        const auto& picker = impl_->dialog->picker;
        result.input_layout["pickerPending"] = !picker.ready();
        result.input_layout["pickerRows"] = picker.size();
        result.input_layout["pickerGeneration"] = picker.generation();
        result.input_layout["pickerQuery"] = impl_->prompt_text;
        result.input_layout["pickerMessage"] = picker.message();
        result.input_layout["pickerError"] = picker.error();
    }
    if (impl_->dialog) {
        if (impl_->dialog->input_layout) {
            result.input_layout["pending"] = impl_->dialog->input_layout->pending();
            result.input_layout["compositionPending"] =
                impl_->dialog->input_layout->composition_pending();
            result.input_layout["bytes"] = impl_->prompt_text.size();
        }
        const auto& detail = *impl_->dialog->detail;
        const auto visible = detail.visible();
        const auto stats = detail.statistics();
        result.text_surface = {{"rows", detail.lines().size()},
                               {"pending", detail.pending()},
                               {"cached", stats.entries},
                               {"accountedBytes", stats.accounted_bytes},
                               {"shaped", stats.shaped},
                               {"first", visible ? Json(visible->first) : Json(nullptr)},
                               {"end", visible ? Json(visible->second) : Json(nullptr)}};
    }
    result.sidebar["focused"] = impl_->focused_view == impl_->active_view;
    result.sidebar["focus"] = std::to_string(impl_->view_focus_sequence);
    result.sidebar["rows"] = Json::array();
    const auto end = std::min(sidebar.index.size(), sidebar.scroll + 128);
    sidebar.index.for_each_range(
        std::min(sidebar.scroll, sidebar.index.size()), end, [&](std::size_t, const ViewRow& row) {
            result.sidebar["rows"].push_back(*row.value);
        });
    if (const auto* editor_form = impl_->forms.mounted({})) {
        const auto& form = *editor_form;
        result.inline_form_layout = InlineFormLayout::measure(impl_->geometry.editor,
                                                              impl_->scale,
                                                              form.fields().size(),
                                                              form.toggles().size(),
                                                              form.actions().size());
        result.inline_form = form.state();
        result.inline_form["busy"] = form.busy();
        result.inline_form["error"] = form.error();
        result.inline_form["message"] = form.message();
    }
    if (const auto* form = impl_->forms.mounted(impl_->active_view); form && impl_->show_sidebar) {
        result.sidebar_form_layout = impl_->form_layout(*form);
        result.sidebar_form = form->state();
        result.sidebar_form["busy"] = form->busy();
        result.sidebar_form["error"] = form->error();
        result.sidebar_form["message"] = form->message();
    }
    if (const auto* document = impl_->current()) {
        result.document = document->metadata;
        const auto cache = document->viewport.statistics();
        const auto& summary = document->viewport.lines().summary();
        result.document_viewport = {
            {"rows", summary.rows},
            {"bytes", summary.utf8_bytes},
            {"firstLine", summary.rows ? Json(summary.first_line) : Json(nullptr)},
            {"lastLine", summary.rows ? Json(summary.last_line) : Json(nullptr)},
            {"cached", cache.entries},
            {"accountedBytes", cache.accounted_bytes},
            {"shaped", cache.shaped},
            {"reused", cache.reused},
            {"evicted", cache.evicted},
            {"layoutsPending", document->viewport.layouts_pending()},
            {"workerSubmitted", impl_->layout_jobs->statistics().submitted},
            {"workerJobs", impl_->layout_jobs->statistics().jobs}};
        result.outline_first = document->outline_begin;
        result.outline_total = document->outline_total;
        result.visible_outline_rows = document->outline.size();
    }
    return result;
}

bool EditorWindow::pump() {
    if (!impl_->running) {
        impl_->layout_jobs->poll();
        if (!impl_->stopping_workbench) {
            impl_->finish_dialog(nullptr);
            while (!impl_->pending_edits.empty()) {
                const auto edit = impl_->pending_edits.begin()->second;
                impl_->cancel_edit(edit);
            }
            while (!impl_->pending_reads.empty()) {
                const auto read = impl_->pending_reads.begin()->second;
                impl_->settle_read(read, nullptr);
            }
            while (!impl_->pending_fences.empty()) {
                auto found = impl_->pending_fences.begin();
                auto pending = std::move(found->second);
                impl_->pending_fences.erase(found);
                pending.completion.set_value(false);
            }
            impl_->stopping_workbench = true;
            impl_->clear_preedit();
            impl_->clear_input_events();
            for (auto& pending : impl_->pending_styles) {
                pending.completion.set_value(nullptr);
            }
            impl_->pending_styles.clear();
            impl_->sidebar_views.shutdown();
            impl_->workbench_shutdown_deadline = Clock::now() + std::chrono::seconds(5);
            impl_->workbench->shutdown();
            impl_->policy_inputs.clear();
            impl_->view_inputs.clear();
            impl_->layout_jobs->close();
        }
        impl_->workbench->poll();
        if (impl_->workbench->idle() && impl_->sidebar_views.quiescent() &&
            impl_->layout_jobs->idle()) {
            return false;
        }
        if (Clock::now() >= impl_->workbench_shutdown_deadline) {
            throw std::runtime_error(
                "Workbench transport or view retirement did not drain on shutdown");
        }
        return true;
    }
    try {
        impl_->renderer.poll_events([this](const SDL_Event& event) {
            try {
                impl_->event(event);
            } catch (const std::exception& error) {
                impl_->error = error.what();
            }
        });
        impl_->layout_jobs->poll();
        impl_->document_composition.poll();
        for (auto& [id, document] : impl_->documents) {
            static_cast<void>(id);
            document.viewport.poll_layouts();
        }
        if (impl_->dialog) {
            impl_->dialog->detail->poll();
            if (impl_->dialog->input_layout) {
                impl_->dialog->input_layout->poll();
            }
        }
        for (auto& [id, cached] : impl_->form_fields) {
            static_cast<void>(id);
            cached.poll();
        }
        impl_->drain_input_events();
        // Read-only modal shortcuts retain their keystroke through release,
        // including auto-repeat and its text/IME packets after the panel closes.
        // Other keydowns, pointer input and new dialogs start a fresh text sequence.
        if (impl_->consumed_modal_keys.none())
            impl_->consumed_modal_text = false;
        impl_->service_responses();
        impl_->sidebar_views.poll();
        if (impl_->focused_view) {
            const auto& version = impl_->sidebar_views.at(*impl_->focused_view).version;
            if (version != impl_->focused_view_version) {
                static_cast<void>(impl_->sidebar_views.navigate(
                    *impl_->focused_view, "retain", impl_->view_page()));
                impl_->focused_view_version = version;
            }
        }
        const auto form_deadline = Clock::now() + std::chrono::milliseconds(4);
        if (impl_->pending_document_activity) {
            impl_->pending_document_activity = false;
            impl_->workbench->emit("ui.document.activated",
                                   {{"activity", std::to_string(impl_->document_activity)}});
        }
        for (int count = 0;
             count < 8 && !impl_->form_events.empty() && Clock::now() < form_deadline;
             ++count) {
            auto event = std::move(impl_->form_events.front());
            impl_->form_events.pop_front();
            impl_->workbench->emit(event.first, event.second);
        }
        impl_->workbench->poll();
        impl_->finish_styles();
        if (const auto batch = impl_->file_icons.begin()) {
            try {
                impl_->workbench->execute(
                    "workbench.resolveIcons",
                    *batch,
                    [this, batch = *batch](const script::Runtime::Outcome& outcome) {
                        try {
                            impl_->file_icons.finish(batch,
                                                     outcome.error ? nullptr : &outcome.value);
                            if (outcome.error) {
                                impl_->error = outcome.error->value(
                                    "message", std::string("File icon resolution failed"));
                            }
                        } catch (const std::exception& error) {
                            impl_->error = error.what();
                        }
                    });
            } catch (...) {
                impl_->file_icons.finish(*batch, nullptr);
                throw;
            }
        }
        impl_->advance_policy_inputs();
        impl_->advance_view_inputs();
        if (impl_->finish_fences())
            impl_->workbench->poll();
        if (impl_->quit_requested && impl_->workbench->idle() && impl_->native_idle()) {
            if (impl_->tabs.empty())
                impl_->running = false;
            else {
                impl_->quit_requested = false;
                impl_->quitting_fences.clear();
                impl_->error = "Documents opened during teardown; window kept open";
            }
        }
        for (const auto& error : impl_->workbench->take_errors()) {
            impl_->error = error.value("message", std::string("Workbench event failed"));
        }
        if (impl_->dragging &&
            (impl_->mouse_y < impl_->content_top || impl_->mouse_y > impl_->content_bottom) &&
            Clock::now() >= impl_->drag_scroll_after) {
            impl_->drag_scroll_after = Clock::now() + std::chrono::milliseconds(50);
            if (auto* document = impl_->current(); document && !document->loading) {
                if (impl_->mouse_y < impl_->content_top && document->first_line > 0) {
                    --document->first_line;
                } else if (impl_->mouse_y > impl_->content_bottom) {
                    document->first_line =
                        std::min(document->first_line + 1,
                                 document->metadata.at("lineCount").get<std::size_t>() - 1);
                }
                document->needs_lines = true;
                impl_->point(
                    std::clamp(impl_->mouse_x, impl_->left, impl_->right),
                    std::clamp(impl_->mouse_y, impl_->content_top + 1, impl_->content_bottom - 1),
                    true,
                    false);
            }
        }
        if (impl_->running) {
            impl_->draw();
        }
    } catch (const std::exception& error) {
        impl_->error = error.what();
        // A renderer failure cannot be presented as a functional blank editor.
        throw;
    }
    return true;
}

int EditorWindow::run() {
    while (pump()) {
        SDL_Delay(4);
    }
    return 0;
}

} // namespace astra::ui
