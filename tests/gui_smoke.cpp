#include "astra/ui/editor_window.hpp"
#include <astra/ui/text_layout.hpp>
#include "astra/service/service_host.hpp"
#include "astra/io/file_store.hpp"
#include <astra/transport/editor_channel.hpp>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void settle(astra::ui::EditorWindow& window) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    do {
        require(window.pump(), "Window closed before acceptance completed");
        require(window.last_error().empty(), "GUI action failed: " + window.last_error());
        if (window.idle()) {
            return;
        }
        SDL_Delay(1);
    } while (Clock::now() < deadline);
    throw std::runtime_error("GUI did not become idle within 15 seconds");
}

void key(astra::ui::EditorWindow& window,
         SDL_Keycode keycode,
         SDL_Keymod modifiers = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = keycode;
    event.key.mod = modifiers;
    event.key.down = true;
    event.key.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Push production keyboard event failed");
    settle(window);
}

void text(astra::ui::EditorWindow& window, const std::string& input) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = input.c_str();
    event.text.windowID = SDL_GetWindowID(window.window());
    require(SDL_PushEvent(&event), "Push production text input event failed");
    settle(window);
}

void pointer(astra::ui::EditorWindow& window, Uint32 type, float x, float y) {
    int width{}, height{}, pixels_x{}, pixels_y{};
    require(SDL_GetWindowSize(window.window(), &width, &height) &&
                SDL_GetWindowSizeInPixels(window.window(), &pixels_x, &pixels_y),
            "Cannot read pointer scale");
    SDL_Event event{};
    event.type = type;
    if (type == SDL_EVENT_MOUSE_MOTION) {
        event.motion.windowID = SDL_GetWindowID(window.window());
        event.motion.state = SDL_BUTTON_LMASK;
        event.motion.x = x * static_cast<float>(width) / static_cast<float>(pixels_x);
        event.motion.y = y * static_cast<float>(height) / static_cast<float>(pixels_y);
    } else {
        event.button.windowID = SDL_GetWindowID(window.window());
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.x = x * static_cast<float>(width) / static_cast<float>(pixels_x);
        event.button.y = y * static_cast<float>(height) / static_cast<float>(pixels_y);
    }
    require(SDL_PushEvent(&event), "Cannot enqueue pointer input");
}

void click(astra::ui::EditorWindow& window, astra::ui::Box box) {
    pointer(window, SDL_EVENT_MOUSE_BUTTON_DOWN, box.x + box.width / 2, box.y + box.height / 2);
    pointer(window, SDL_EVENT_MOUSE_BUTTON_UP, box.x + box.width / 2, box.y + box.height / 2);
    settle(window);
}

Json call(const std::shared_ptr<astra::service::ServiceHost>& host,
          const std::string& method,
          const Json& params) {
    const auto response =
        host
            ->submit(Json{
                {"jsonrpc", "2.0"}, {"id", "acceptance"}, {"method", method}, {"params", params}}
                         .dump())
            .get();
    require(response.has_value(), "Document verification returned no response");
    const auto parsed = Json::parse(*response);
    require(!parsed.contains("error"), "Document verification returned an error");
    return parsed.at("result");
}

Json read_document(const std::shared_ptr<astra::service::ServiceHost>& host,
                   const std::string& id) {
    return call(host, "document.get", {{"id", id}});
}

std::string read_all_text(const std::shared_ptr<astra::service::ServiceHost>& host,
                          const std::string& id) {
    std::string text;
    std::optional<std::string> revision;
    for (;;) {
        const auto page = call(host, "document.get", {{"id", id}, {"begin", text.size()}});
        const auto current_revision = page.at("revision").get<std::string>();
        require(!revision || *revision == current_revision,
                "Document changed during paginated verification");
        revision = current_revision;
        const auto end = page.at("range").at("end").get<std::size_t>();
        text += page.at("text").get<std::string>();
        require(end == text.size(), "Document page has inconsistent byte extent");
        if (end == page.at("sizeBytes").get<std::size_t>()) {
            return text;
        }
        require(!page.at("text").get_ref<const std::string&>().empty(),
                "Document pagination did not advance");
    }
}

class ConcurrentSyntaxChannel final : public astra::ui::EditorChannel {
  public:
    ConcurrentSyntaxChannel(std::shared_ptr<astra::ui::EditorChannel> channel,
                            std::shared_ptr<astra::service::ServiceHost> host)
        : channel_(std::move(channel)), host_(std::move(host)) {
    }

    std::future<std::optional<std::string>> submit(std::string body) override {
        const auto request = Json::parse(body);
        const auto method = request.at("method").get<std::string>();
        if (method == "document.get" && hold_next_metadata) {
            require(!held_metadata_, "A metadata response is already withheld");
            hold_next_metadata = false;
            held_metadata_.emplace();
            held_metadata_->future = channel_->submit(std::move(body));
            return held_metadata_->reply.get_future();
        }
        if (method == "document.find") {
            if (request.at("params").value("select", false))
                ++find_requests;
            else
                ++preview_requests;
        }
        if (method == "document.replaceAll")
            ++replace_requests;
        if (method == "document.get" && request.at("params").value("end", std::size_t{0}) >
                                            request.at("params").value("begin", std::size_t{0}))
            ++text_downloads;
        if (method == "document.find" && hold_next_find) {
            hold_next_find = false;
            held_find.emplace();
            held_find->body = std::move(body);
            return held_find->reply.get_future();
        }
        if (!changed && request.at("method") == "document.syntax") {
            changed = true;
            const auto id = request.at("params").at("id").get<std::string>();
            const auto original = read_document(host_, id);
            const auto edited = call(host_,
                                     "document.command",
                                     {{"id", id},
                                      {"baseRevision", original.at("revision")},
                                      {"command", "insert_text"},
                                      {"text", "other client"}});
            static_cast<void>(call(
                host_, "document.undo", {{"id", id}, {"baseRevision", edited.at("revision")}}));
        }
        return channel_->submit(std::move(body));
    }

    std::vector<std::string> take_notifications() override {
        if (held_metadata_ && held_metadata_->future &&
            held_metadata_->future->wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            held_metadata_->body = held_metadata_->future->get();
            held_metadata_->future.reset();
            require(held_metadata_->body.has_value(), "Real metadata request returned no body");
        }
        if (held_find && held_find->future &&
            held_find->future->wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            held_find->reply.set_value(held_find->future->get());
            held_find.reset();
        }
        if (defer_notifications_) {
            defer_notifications_ = false;
            ++notification_deferrals;
            return {};
        }
        auto notifications = channel_->take_notifications();
        if (released_notification_) {
            notifications.push_back(std::move(*released_notification_));
            released_notification_.reset();
        }
        std::vector<std::string> delivered;
        for (const auto& body : notifications) {
            const auto event = Json::parse(body);
            const auto method = event.value("method", std::string{});
            if (method == "document.changed" || method == "document.created") {
                const auto& metadata = event.at("params");
                if (withheld_selection_ && metadata.at("id") == withheld_selection_->at("id") &&
                    metadata.at("selections") == withheld_selection_->at("selections")) {
                    require(!held_notification_, "A selection notification is already withheld");
                    held_notification_ = body;
                    withheld_selection_.reset();
                    continue;
                }
                observed_[metadata.at("id").get<std::string>()] = metadata;
            } else if (method == "document.closed") {
                observed_.erase(event.at("params").at("id").get<std::string>());
            }
            delivered.push_back(body);
        }
        return delivered;
    }

    bool metadata_ready() const {
        return held_metadata_ && held_metadata_->body.has_value();
    }

    void release_metadata() {
        require(metadata_ready(), "No completed metadata response to release");
        held_metadata_->reply.set_value(std::move(held_metadata_->body));
        held_metadata_.reset();
    }

    void hold_selection(Json expected) {
        require(!held_notification_ && !withheld_selection_, "A notification is already held");
        withheld_selection_ = std::move(expected);
    }

    bool selection_held() const {
        return held_notification_.has_value();
    }

    void release_selection() {
        require(held_notification_.has_value(), "No actual selection notification to release");
        released_notification_ = std::move(held_notification_);
        held_notification_.reset();
    }

    bool observed_selection(const Json& expected) const {
        const auto found = observed_.find(expected.at("id").get<std::string>());
        return found != observed_.end() &&
               found->second.at("revision") == expected.at("revision") &&
               found->second.at("selections") == expected.at("selections");
    }

    void defer_notifications_once() {
        defer_notifications_ = true;
    }

    void release_find() {
        require(held_find.has_value(), "No withheld search request to release");
        held_find->future.emplace(channel_->submit(std::move(held_find->body)));
    }

    bool changed{};
    bool hold_next_find{};
    bool hold_next_metadata{};
    unsigned find_requests{}, preview_requests{}, replace_requests{}, text_downloads{};
    unsigned notification_deferrals{};

  private:
    struct HeldFind {
        std::string body;
        std::promise<std::optional<std::string>> reply;
        std::optional<std::future<std::optional<std::string>>> future;
    };

    std::optional<HeldFind> held_find;

    struct HeldMetadata {
        std::promise<std::optional<std::string>> reply;
        std::optional<std::future<std::optional<std::string>>> future;
        std::optional<std::string> body;
    };

    std::optional<HeldMetadata> held_metadata_;
    std::optional<Json> withheld_selection_;
    std::optional<std::string> held_notification_, released_notification_;
    std::map<std::string, Json> observed_;
    bool defer_notifications_{};
    std::shared_ptr<astra::ui::EditorChannel> channel_;
    std::shared_ptr<astra::service::ServiceHost> host_;
};

template <class Predicate>
void pump_until(astra::ui::EditorWindow& window, Predicate ready, const std::string& failure) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    do {
        require(window.pump(), "Window closed during deferred editing acceptance");
        require(window.last_error().empty(), "GUI action failed: " + window.last_error());
        if (ready())
            return;
        SDL_Delay(1);
    } while (Clock::now() < deadline);
    throw std::runtime_error(failure);
}
} // namespace

int main(int argc, char* argv[]) {
    try {
        const bool remote = argc == 2 && std::string_view(argv[1]) == "--tcp";
        require(argc == 1 || remote, "Usage: gui-smoke [--tcp]");
        std::cerr << std::unitbuf << "GUI acceptance: starting service\n";
        const auto name =
            "gui-workspace-" + std::to_string(Clock::now().time_since_epoch().count());
        const auto workspace =
            std::filesystem::absolute(std::filesystem::path("build/graphics") / name);
        std::filesystem::create_directories(workspace / "src");
        {
            std::ofstream file(workspace / "src/main.cpp", std::ios::binary);
            file << "// GUI acceptance\nint main() { return 0; }\n";
        }
        {
            std::ofstream file(workspace / "README.md", std::ios::binary);
            file << "# Astra GUI acceptance\n";
        }
        {
            std::ofstream file(workspace / "rename.txt", std::ios::binary);
            file << "needle and needle\n";
        }
        {
            std::ofstream file(workspace / "closing.txt", std::ios::binary);
            file << "baseline";
        }
        const auto long_line = std::string(70'000, 'a') + "\t中文🙂";
        const std::string navigation_source = "\t中文🙂";
        const auto navigation_target = std::string(70'000, 'b');
        {
            std::ofstream file(workspace / "long-navigation.txt", std::ios::binary);
            file << navigation_source << '\n' << navigation_target;
        }
        {
            std::ofstream file(workspace / "cache.cpp", std::ios::binary);
            for (int i = 0; i < 80; ++i)
                file << "int value" << i << " = " << i << "; // 中🙂é\n";
        }
        {
            std::ofstream file(workspace / "long.txt", std::ios::binary);
            file << long_line;
        }
        {
            std::ofstream file(workspace / ".editorconfig", std::ios::binary);
            file << "root = true\n[*]\nindent_style = space\nindent_size = 4\nend_of_line = "
                    "lf\n[README.md]\nend_of_line = crlf\n";
        }
        {
            std::ofstream file(workspace / ".clangd", std::ios::binary);
            file << "CompileFlags:\n  Add: [-nostdinc, -nostdinc++]\n";
        }
        astra::service::ServiceHostOptions service_options;
        service_options.workspace = workspace;
        service_options.recovery_directory = workspace.parent_path() / (name + "-recovery");
        service_options.resource_directory = std::filesystem::current_path();
        service_options.defaults_script =
            astra::io::read_file("data/defaults.js", 1024 * 1024).text;
        service_options.user_script = R"js(let releaseInput,observedRead=null;
        astra.registerCommand('acceptance.inputState',()=>({waiting:!!releaseInput,observedRead}));
        astra.registerCommand('acceptance.observeRead',parameters=>{observedRead=parameters;return true;});
        astra.registerCommand('acceptance.releaseInput',()=>{releaseInput();releaseInput=undefined;return true;});
        astra.registerCommand('editor.insert_text',async parameters=>{
            if(parameters.text==='A')await new Promise(resolve=>{releaseInput=resolve;});
            await astra.sleep(80);
            return await astra.request('document.command',{...parameters,command:'insert_text'});
        });)js";
        auto host = std::make_shared<astra::service::ServiceHost>(std::move(service_options));
        std::cerr << "GUI acceptance: creating window\n";
        astra::ui::EditorWindowOptions options;
        options.title = "Astra — GUI acceptance";
        options.workspace_name = "GUI ACCEPTANCE";
        options.open_paths = {"src/main.cpp"};
        options.width = 1400;
        options.height = 860;
        options.user_script = R"js(if(astra.context==='client') {
            let latestSelectionEvent;
            astra.on('document.changed', value => {
                if (value.reason === 'selections') latestSelectionEvent = value;
            });
            astra.registerCommand('acceptance.delayedMetadata', async () => {
                const {document} = await astra.ui.read({immediate:true});
                const old = await astra.request('document.get', {id:document.id});
                await astra.ui.update({document:old,status:'Delayed metadata response completed'});
            });
            astra.registerCommand('acceptance.metadataEvent', async () => {
                await astra.ui.update({status:'Selection event '+latestSelectionEvent.selections[0].head});
            });
            let releaseClientInput;
            astra.registerCommand('editor.insert_text', async parameters => {
                if (parameters.text === 'A') {
                    await astra.ui.update({ status: 'Client input suspended' });
                    await new Promise(resolve => { releaseClientInput = resolve; });
                } else await astra.sleep(20);
                // The handler must read its captured document without waiting
                // for itself or for later inputs already accepted by SDL.
                const { document } = await astra.ui.read({ id: parameters.id });
                if (document.id !== parameters.id)
                    throw new Error('Client input changed documents across await');
                return await astra.ui.native('editor.insert_text', parameters);
            });
            astra.registerCommand('acceptance.releaseClientInput', () => {
                if (!releaseClientInput) throw new Error('Client input gate was not active');
                releaseClientInput();
                releaseClientInput = undefined;
            });
            astra.ui.registerCommand({ id: 'acceptance.nativeUndo', title: 'Direct native undo', palette: false },
                () => astra.ui.native('editor.undo'));
            astra.bind('Ctrl+Alt+U', 'acceptance.nativeUndo');
            astra.registerCommand('editor.move_down', async parameters => {
                await astra.sleep(0);
                const result = await astra.ui.native('editor.move_down', parameters);
                await astra.ui.update({ status: 'Client shaped navigation' });
                return result;
            });
            let unsavedReplacementId;
            astra.registerCommand('acceptance.prepareUnsavedReplacement', async () => {
                const { document: original } = await astra.ui.read();
                await astra.ui.execute('ui.new');
                const { document } = await astra.ui.read();
                unsavedReplacementId = document.id;
                await astra.ui.execute('editor.insert_text', {
                    id: document.id, baseRevision: document.revision, text: 'needle in unsaved'
                });
                await astra.ui.update({ activate: original.id });
            });
            astra.registerCommand('acceptance.undoUnsavedReplacement', async () => {
                const { document } = await astra.ui.read({ id: unsavedReplacementId });
                await astra.ui.execute('editor.undo', {
                    id: document.id, baseRevision: document.revision
                });
            });
            astra.registerCommand('acceptance.closeUnsavedReplacement', () =>
                astra.ui.execute('workbench.close', {
                    id: unsavedReplacementId, discardChanges: true
                })
            );
            astra.registerCommand('acceptance.findWithoutInput',()=>astra.ui.execute('ui.find',{query:'beta'}));
            astra.registerCommand('acceptance.readInput',async()=>{
                const state=await astra.ui.read();
                await astra.request('script.command',{name:'acceptance.observeRead',params:state});
            });
            astra.registerCommand('acceptance.cancelRead',async()=>{
                const control=new AbortController();
                const pending=astra.ui.read({}, {signal:control.signal});
                const observed=pending.then(()=>false,error=>error.name==='AbortError');
                await astra.sleep(0);control.abort();
                if(!await observed)throw new Error('Read cancellation did not reject');
                await astra.ui.update({status:'Read fence cancelled'});
            });
            astra.ui.registerCommand({id:'ui.toggleSidebar',title:'User sidebar policy'},async()=>{
                const state=await astra.ui.read({immediate:true});await astra.ui.update({sidebarVisible:!state.sidebarVisible,status:'User sidebar policy'});
            });
            astra.ui.registerCommand({id:'acceptance.awaitEdits',title:'Await actual editing'},async()=>{
                const {document}=await astra.ui.read();
                const undone=await astra.ui.execute('editor.undo',{id:document.id,baseRevision:document.revision});
                if(undone.id!==document.id || undone.revision===document.revision)throw new Error('Undo resolved before commit');
                const before=await astra.request('document.get',{id:document.id});
                if(before.text!=='alpha\nbeta')throw new Error('Awaited Undo did not observe the edit');
                const redone=await astra.ui.execute('editor.redo',{id:document.id,baseRevision:undone.revision});
                const after=await astra.request('document.get',{id:document.id});
                if(after.text!=='Xalpha\nXbeta' || after.revision!==redone.revision)throw new Error('Awaited Redo did not observe the edit');
                let rejected=false;
                try { await astra.ui.execute('editor.undo',{id:document.id,baseRevision:document.revision}); }
                catch(error) { rejected=error.code===-32001; }
                if(!rejected)throw new Error('Editor action lost the stale revision error');
                const retained=await astra.request('document.get',{id:document.id});
                if(retained.text!==after.text || retained.revision!==after.revision)throw new Error('Rejected edit changed the document');
                await astra.ui.update({status:'Awaited editing passed'});
            });
            astra.ui.registerCommand({id:'acceptance.cancelEdit',title:'Cancel queued editing'},async()=>{
                const control=new AbortController();
                const pending=astra.ui.native('editor.insert_text',{text:'cancelled edit'},{signal:control.signal});
                const observed=pending.then(()=>false,error=>error.name==='AbortError');
                await astra.sleep(0);control.abort();
                if(!await observed)throw new Error('Editor action cancellation did not reject');
                await astra.ui.update({status:'Queued edit cancelled'});
            });
            let compositionStyle;
            astra.ui.registerCommand({
                id: 'acceptance.ime.recolor', title: 'Recolor IME preview'
            }, async () => {
                compositionStyle = await astra.ui.style.read();
                await astra.ui.style.update({theme: {foreground: '#F0B040'}});
            });
            astra.ui.registerCommand({
                id: 'acceptance.ime.restore', title: 'Restore IME preview style'
            }, async () => {
                await astra.ui.style.update(compositionStyle);
            });
        })js";
        std::shared_ptr<astra::ui::EditorChannel> channel = host;
        if (remote) {
            channel =
                std::make_shared<astra::transport::TcpEditorChannel>("127.0.0.1", host->port());
        }
        const auto concurrent = std::make_shared<ConcurrentSyntaxChannel>(channel, host);
        channel = concurrent;
        astra::ui::EditorWindow window(channel, std::move(options));
        settle(window);
        std::cerr << "GUI acceptance: initial viewport and syntax ready\n";
        require(concurrent->changed, "Concurrent syntax revision acceptance did not run");
        const auto id = window.active_document_id();
        require(!id.empty(), "Workspace file did not open in the editor");
        require(read_document(host, id).at("text") ==
                    "// GUI acceptance\nint main() { return 0; }\n",
                "Opened wrong file");
        const auto original_metadata = window.view_state().document;
        concurrent->hold_next_metadata = true;
        window.execute_ui_command("acceptance.delayedMetadata");
        pump_until(
            window,
            [&] {
                return concurrent->metadata_ready();
            },
            "Actual document.get response was not retained by the channel proxy");
        const auto move = [&](std::size_t head) {
            return call(host,
                        "document.setSelections",
                        {{"id", id},
                         {"baseRevision", original_metadata.at("revision")},
                         {"selections", Json::array({{{"anchor", head}, {"head", head}}})}});
        };
        concurrent->hold_selection(
            {{"id", id}, {"selections", Json::array({{{"anchor", 1}, {"head", 1}}})}});
        static_cast<void>(move(1));
        pump_until(
            window,
            [&] {
                return concurrent->selection_held();
            },
            "Actual selection notification was not delayed");
        const auto newest = move(2);
        pump_until(
            window,
            [&] {
                return window.view_state().document.at("selections") == newest.at("selections");
            },
            "GUI did not accept the newer same-text selection");
        const auto accepted_metadata = window.view_state().document;
        require(accepted_metadata.at("revision") == original_metadata.at("revision"),
                "Selection-only change advanced the text revision");
        concurrent->release_metadata();
        pump_until(
            window,
            [&] {
                return window.status_text() == "Delayed metadata response completed" &&
                       window.idle();
            },
            "Old metadata RPC continuation did not complete");
        require(window.view_state().document == accepted_metadata,
                "Old same-revision RPC response replaced current GUI metadata");
        concurrent->release_selection();
        settle(window);
        require(window.view_state().document == accepted_metadata,
                "Old same-revision notification replaced current GUI metadata");
        window.execute_ui_command("acceptance.metadataEvent");
        settle(window);
        require(window.status_text() == "Selection event 2",
                "Obsolete selection notification reached the user JS listener");
        // A legitimate notification can arrive after a read of that exact
        // state. Both must retain the same identity, and JS still needs its event.
        concurrent->hold_selection(
            {{"id", id}, {"selections", Json::array({{{"anchor", 3}, {"head", 3}}})}});
        const auto third = move(3);
        pump_until(
            window,
            [&] {
                return concurrent->selection_held();
            },
            "Could not retain the latest legitimate selection notification");
        window.execute_ui_command("acceptance.delayedMetadata");
        pump_until(
            window,
            [&] {
                return window.view_state().document.at("selections") == third.at("selections") &&
                       window.status_text() == "Delayed metadata response completed" &&
                       window.idle();
            },
            "Read did not observe the latest state before its notification");
        const auto observed_sequence = window.view_state().document.at("metadataSequence");
        concurrent->release_selection();
        settle(window);
        require(window.view_state().document.at("selections") == third.at("selections") &&
                    window.view_state().document.at("metadataSequence") == observed_sequence,
                "Same-state read and delayed legitimate event used different identities");
        window.execute_ui_command("acceptance.metadataEvent");
        settle(window);
        require(window.status_text() == "Selection event 3",
                "Latest legitimate selection event was suppressed after a read");
        const auto restored = call(host,
                                   "document.setSelections",
                                   {{"id", id},
                                    {"baseRevision", original_metadata.at("revision")},
                                    {"selections", original_metadata.at("selections")}});
        pump_until(
            window,
            [&] {
                return window.view_state().document.at("selections") == restored.at("selections");
            },
            "Could not restore the selection after metadata ordering checks");
        settle(window);
        std::cerr << "GUI metadata order: delayed real RPC and notification, same-text selection, "
                     "JS stale filtering, legitimate event delivery and original revision "
                     "preserved: PASS\n";
        key(window, SDLK_B, SDL_KMOD_CTRL);
        require(window.status_text() == "User sidebar policy",
                "Native UI command bypassed the user JS override");
        key(window, SDLK_B, SDL_KMOD_CTRL);

        key(window, SDLK_A, SDL_KMOD_CTRL);
        for (const auto* input : {"A", "中🙂"}) {
            SDL_Event event{};
            event.type = SDL_EVENT_TEXT_INPUT;
            event.text.text = input;
            event.text.windowID = SDL_GetWindowID(window.window());
            require(SDL_PushEvent(&event), "Cannot enqueue delayed script editing intents");
        }
        pump_until(
            window,
            [&] {
                return window.status_text() == "Client input suspended";
            },
            "SDL input bypassed the asynchronous client editing override");
        window.execute_ui_command("acceptance.readInput");
        window.execute_ui_command("acceptance.cancelRead");
        pump_until(
            window,
            [&] {
                return window.status_text() == "Read fence cancelled";
            },
            "Cancelling a read remained blocked behind editing");
        require(call(host, "script.command", {{"name", "acceptance.inputState"}})
                    .at("observedRead")
                    .is_null(),
                "A document read bypassed preceding suspended input");
        window.open_path("README.md");
        pump_until(
            window,
            [&] {
                return !window.active_document_id().empty() && window.active_document_id() != id;
            },
            "A suspended editing policy blocked opening another document");
        const auto independent_id = window.active_document_id();
        SDL_Event independent{};
        independent.type = SDL_EVENT_TEXT_INPUT;
        independent.text.text = "Independent";
        independent.text.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&independent), "Cannot enqueue independent document input");
        pump_until(
            window,
            [&] {
                return read_document(host, independent_id)
                           .at("text")
                           .get<std::string>()
                           .find("Independent") != std::string::npos;
            },
            "A suspended editing policy blocked input in another document");
        require(read_document(host, id).at("text") ==
                    "// GUI acceptance\nint main() { return 0; }\n",
                "A later input bypassed the suspended command for the same document");
        window.execute_ui_command("acceptance.releaseClientInput");
        pump_until(
            window,
            [&] {
                return call(host, "script.command", {{"name", "acceptance.inputState"}})
                    .at("waiting")
                    .get<bool>();
            },
            "The captured client input did not reach the shared service command override");
        require(call(host, "script.command", {{"name", "acceptance.releaseInput"}}) == true,
                "Cannot release the editing policy gate");
        settle(window);
        require(read_document(host, id).at("text") == "A中🙂",
                "Deferred user editing commands lost order or reused a stale revision");
        const auto observed =
            call(host, "script.command", {{"name", "acceptance.inputState"}}).at("observedRead");
        require(observed.at("document").at("id") == id &&
                    observed.at("document").at("revision") ==
                        read_document(host, id).at("revision"),
                "An awaited document read lost its captured identity or revision");
        require(observed.at("newline") == "\n",
                "An awaited document read used another document's line ending");
        require(window.active_document_id() == independent_id,
                "Completed input changed the active document across await");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, independent_id).at("text") == "# Astra GUI acceptance\n",
                "Independent input did not preserve its own history");
        window.execute_ui_command("ui.closeTab");
        settle(window);
        require(window.active_document_id() == id,
                "Closing the independent clean tab did not restore the original tab");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "A",
                "Deferred editing did not preserve one transaction per input");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") ==
                    "// GUI acceptance\nint main() { return 0; }\n",
                "Deferred editing history lost the original source");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        const auto before_native_undo = read_document(host, id);
        SDL_Event undo_input{};
        undo_input.type = SDL_EVENT_TEXT_INPUT;
        undo_input.text.text = "Native undo ordering";
        undo_input.text.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&undo_input), "Cannot enqueue client-delayed native undo input");
        SDL_Event undo_key{};
        undo_key.type = SDL_EVENT_KEY_DOWN;
        undo_key.key.key = SDLK_U;
        undo_key.key.mod = SDL_KMOD_CTRL | SDL_KMOD_ALT;
        undo_key.key.down = true;
        undo_key.key.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&undo_key), "Cannot enqueue immediate native undo policy");
        settle(window);
        const auto after_native_undo = read_document(host, id);
        require(after_native_undo.at("text") == before_native_undo.at("text") &&
                    std::stoull(after_native_undo.at("revision").get<std::string>()) ==
                        std::stoull(before_native_undo.at("revision").get<std::string>()) + 2,
                "A direct native JS edit bypassed earlier client input awaiting its prelude");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        SDL_Event saved_input{};
        saved_input.type = SDL_EVENT_TEXT_INPUT;
        saved_input.text.text = "Saved batch 中🙂";
        saved_input.text.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&saved_input), "Cannot enqueue same-batch save input");
        SDL_Event saved_key{};
        saved_key.type = SDL_EVENT_KEY_DOWN;
        saved_key.key.key = SDLK_S;
        saved_key.key.mod = SDL_KMOD_CTRL;
        saved_key.key.down = true;
        saved_key.key.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&saved_key), "Cannot enqueue same-batch save shortcut");
        settle(window);
        require(astra::io::read_file(workspace / "src/main.cpp").text == "Saved batch 中🙂",
                "Save captured a revision before preceding input committed");
        require(!read_document(host, id).at("dirty").get<bool>(),
                "Same-batch save did not persist its captured input");
        SDL_Event copy_input{};
        copy_input.type = SDL_EVENT_TEXT_INPUT;
        copy_input.text.text = " Copy";
        copy_input.text.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&copy_input), "Cannot enqueue same-batch line copy input");
        SDL_Event copy_key{};
        copy_key.type = SDL_EVENT_KEY_DOWN;
        copy_key.key.key = SDLK_C;
        copy_key.key.mod = SDL_KMOD_CTRL;
        copy_key.key.down = true;
        copy_key.key.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&copy_key), "Cannot enqueue same-batch line copy shortcut");
        settle(window);
        char* copied = SDL_GetClipboardText();
        require(copied != nullptr, "Cannot read copied line");
        const std::string copied_line(copied);
        SDL_free(copied);
        require(copied_line == "Saved batch 中🙂 Copy",
                "Line copy read an old selection or relied on the stale native viewport");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, "alpha\nbeta");
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        key(window, SDLK_RIGHT);
        key(window, SDLK_RIGHT);
        key(window, SDLK_DOWN, SDL_KMOD_SHIFT);
        require(window.status_text() == "Client shaped navigation",
                "Vertical SDL input bypassed the client JS override");
        const auto vertical_selection = read_document(host, id).at("selections").at(0);
        require(vertical_selection.at("anchor") == 2 && vertical_selection.at("head") == 8,
                "Asynchronous shaped navigation lost its visual column or Shift extension");
        key(window, SDLK_UP);
        require(read_document(host, id).at("selections").at(0).at("head") == 2,
                "Default upward movement lost the retained shaped column");
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        key(window, SDLK_DOWN, SDL_KMOD_CTRL | SDL_KMOD_ALT);
        require(read_document(host, id).at("selections").size() == 2,
                "Add cursor shortcut did not reach the service");
        text(window, "X");
        require(read_document(host, id).at("text") == "Xalpha\nXbeta",
                "Multicursor text entry was not atomic");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "alpha\nbeta",
                "One undo did not restore every cursor edit");
        key(window, SDLK_Y, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "Xalpha\nXbeta",
                "Redo did not restore multicursor text");
        window.execute_ui_command("acceptance.awaitEdits");
        settle(window);
        require(window.status_text() == "Awaited editing passed",
                "Composed editing Promise resolved before actual service completion");
        const auto before_cancel = read_document(host, id);
        window.execute_ui_command("acceptance.cancelEdit");
        settle(window);
        require(window.status_text() == "Queued edit cancelled",
                "Queued editor action did not settle after cancellation");
        const auto after_cancel = read_document(host, id);
        require(before_cancel.at("revision") == after_cancel.at("revision") &&
                    before_cancel.at("text") == after_cancel.at("text"),
                "Cancelled queued editor action mutated the document");
        for (unsigned pair = 0; pair < 3; ++pair)
            for (const auto keycode : {SDLK_Z, SDLK_Y}) {
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.key = keycode;
                event.key.mod = SDL_KMOD_CTRL;
                event.key.down = true;
                event.key.windowID = SDL_GetWindowID(window.window());
                require(SDL_PushEvent(&event), "Cannot queue rapid history shortcuts");
            }
        settle(window);
        const auto after_batch = read_document(host, id);
        require(after_batch.at("text") == after_cancel.at("text") &&
                    std::stoull(after_batch.at("revision").get<std::string>()) ==
                        std::stoull(after_cancel.at("revision").get<std::string>()) + 6,
                "Rapid queued history commands lost order or used stale revisions");

        key(window, SDLK_A, SDL_KMOD_CTRL);
        key(window, SDLK_C, SDL_KMOD_CTRL);
        SDL_Event composing{};
        composing.type = SDL_EVENT_TEXT_EDITING;
        composing.edit.text = "中文";
        composing.edit.start = 0;
        composing.edit.length = 2;
        composing.edit.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&composing), "IME composition event could not be queued");
        settle(window);
        require(read_document(host, id).at("text") == "Xalpha\nXbeta",
                "IME preedit modified the document before commit");
        const auto before_composition = read_document(host, id);
        const auto composition_tabs = window.view_state().tabs;
        std::string long_composition;
        for (unsigned index = 0; index < 1300; ++index) {
            long_composition += "中🙂e\xCC\x81";
        }
        const auto enqueue_composition = [&](const std::string& value) {
            composing.edit.text = value.c_str();
            require(SDL_PushEvent(&composing), "Cannot queue document composition");
            require(window.pump(), "Window closed during document composition");
        };
        const auto worker_before = window.view_state().input_layout.at("workerSubmitted");
        enqueue_composition(long_composition);
        const auto cold_composition = window.view_state().input_layout;
        require(cold_composition.at("documentCompositionPending") == true &&
                    cold_composition.at("documentCompositionDrawn") == false &&
                    cold_composition.at("documentCompositionBytes") == long_composition.size(),
                "Cold long IME preview blocked paint or displayed stale geometry");
        settle(window);
        require(window.view_state().input_layout.at("documentCompositionDrawn") == true &&
                    window.view_state().input_layout.at("documentCompositionPending") == false &&
                    window.view_state().input_layout.at("workerSubmitted").get<std::size_t>() ==
                        worker_before.get<std::size_t>() + 1,
                "Long IME preview did not publish one background paragraph");
        const auto warm_composition = window.view_state();
        for (unsigned frame = 0; frame < 8; ++frame) {
            require(window.pump(), "Window closed during warm IME paint");
        }
        enqueue_composition(long_composition);
        settle(window);
        require(window.view_state().input_layout.at("workerSubmitted") ==
                        warm_composition.input_layout.at("workerSubmitted") &&
                    window.view_state().document_viewport.at("shaped") ==
                        warm_composition.document_viewport.at("shaped") &&
                    read_document(host, id) == before_composition,
                "Warm IME paint resubmitted shaping or changed document state");
        window.execute_ui_command("acceptance.ime.recolor");
        settle(window);
        require(window.view_state().input_layout.at("documentCompositionColor") == 0xFFF0B040 &&
                    window.view_state().input_layout.at("documentCompositionDrawn") == true,
                "IME preview retained geometry with obsolete theme colors");
        window.request_screenshot(remote ? "build/graphics/gui-document-ime-tcp.png"
                                         : "build/graphics/gui-document-ime.png");
        settle(window);
        window.execute_ui_command("acceptance.ime.restore");
        settle(window);

        const auto cleared_composition = [&] {
            const auto state = window.view_state().input_layout;
            require(state.at("documentCompositionBytes") == 0 &&
                        state.at("documentCompositionPending") == false &&
                        state.at("documentCompositionDrawn") == false &&
                        read_document(host, id) == before_composition,
                    "Cancelled IME preview survived or changed document state");
        };
        long_composition += "!";
        enqueue_composition(long_composition);
        key(window, SDLK_ESCAPE);
        cleared_composition();
        enqueue_composition(long_composition);
        window.execute_ui_command("ui.new");
        settle(window);
        require(window.active_document_id() != id, "IME blocked opening another document");
        cleared_composition();
        window.execute_ui_command("ui.closeTab");
        settle(window);
        require(window.active_document_id() == id && window.view_state().tabs == composition_tabs,
                "Composition cancellation changed retained tabs");
        enqueue_composition(long_composition);
        SDL_Event focus_lost{};
        focus_lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        focus_lost.window.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&focus_lost), "Cannot queue IME focus loss");
        settle(window);
        cleared_composition();

        const std::string short_composition = "中文";
        enqueue_composition(short_composition);
        settle(window);
        for (const auto& invalid :
             {std::string(16385, 'x'), std::string("\xFF"), std::string("candidate\nline")}) {
            enqueue_composition(invalid);
            require(!window.last_error().empty() &&
                        window.view_state().input_layout.at("documentCompositionBytes") ==
                            short_composition.size() &&
                        read_document(host, id) == before_composition,
                    "Invalid IME candidate replaced the retained preview or document");
            window.execute_ui_command("ui.cancel");
            settle(window);
            cleared_composition();
            enqueue_composition(short_composition);
            settle(window);
        }
        std::cerr << "GUI acceptance: asynchronous document IME passed\n";
        text(window, "中文🙂 e\xCC\x81");
        require(read_document(host, id).at("text") == "中文🙂 e\xCC\x81", "Unicode input failed");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        key(window, SDLK_V, SDL_KMOD_CTRL);
        const auto pasted = read_document(host, id).at("text");
        require(pasted == "Xalpha\nXbeta", "Production clipboard paste failed: " + pasted.dump());
        std::cerr << "GUI acceptance: clipboard and multicursor passed\n";

        const auto search_downloads = concurrent->text_downloads;
        const auto search_requests = concurrent->find_requests;
        const auto replacement_requests = concurrent->replace_requests;
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        const auto before_find = read_document(host, id);
        key(window, SDLK_F, SDL_KMOD_CTRL);
        require(window.view_state().inline_form_layout.has_value(),
                "Find did not open a nonmodal component");
        require(window.idle(), "Visible nonmodal find incorrectly holds an input task");
        SDL_Rect ime_area{};
        int ime_cursor{}, ime_logical_width{}, ime_logical_height{}, ime_pixels_width{},
            ime_pixels_height{};
        require(
            SDL_GetTextInputArea(window.window(), &ime_area, &ime_cursor) &&
                SDL_GetWindowSize(window.window(), &ime_logical_width, &ime_logical_height) &&
                SDL_GetWindowSizeInPixels(window.window(), &ime_pixels_width, &ime_pixels_height),
            "Cannot verify native IME input area");
        const auto ime_box = window.view_state().inline_form_layout->fields[0];
        require(ime_box.contains(
                    static_cast<float>(ime_area.x) * static_cast<float>(ime_pixels_width) /
                        static_cast<float>(ime_logical_width),
                    static_cast<float>(ime_area.y) * static_cast<float>(ime_pixels_height) /
                        static_cast<float>(ime_logical_height)),
                "IME candidate area remained at the document instead of find input");
        composing.edit.text = "中🙂";
        require(SDL_PushEvent(&composing), "Cannot enqueue input-field IME preedit");
        settle(window);
        require(window.view_state().inline_form.at("values").at("query") == "",
                "IME preedit changed find input before commit");
        text(window, "中🙂e\xCC\x81");
        key(window, SDLK_BACKSPACE);
        require(window.view_state().inline_form.at("values").at("query") == "中🙂",
                "Find backspace split a grapheme");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(window.view_state().inline_form.at("values").at("query") == "中🙂e\xCC\x81",
                "Find input undo changed the service document");
        const auto input_box = window.view_state().inline_form_layout->fields[0];
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_DOWN,
                input_box.x + 3 * window.layout().scale,
                input_box.y + input_box.height / 2);
        pointer(window,
                SDL_EVENT_MOUSE_MOTION,
                input_box.right() - 4 * window.layout().scale,
                input_box.y + input_box.height / 2);
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_UP,
                input_box.right() - 4 * window.layout().scale,
                input_box.y + input_box.height / 2);
        settle(window);
        key(window, SDLK_C, SDL_KMOD_CTRL);
        const std::unique_ptr<char, decltype(&SDL_free)> copied_query(SDL_GetClipboardText(),
                                                                      SDL_free);
        require(copied_query != nullptr, "Cannot inspect input-field clipboard");
        const std::string query_clipboard(copied_query.get());
        require(query_clipboard == "中🙂e\xCC\x81",
                "Find input mouse drag and clipboard split Unicode selection");
        require(read_document(host, id) == before_find,
                "Find input modified editor content or selections");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, "alpha");
        require(read_document(host, id) == before_find,
                "Search preview moved the document selection");
        key(window, SDLK_C, SDL_KMOD_ALT);
        require(window.view_state().inline_form.at("values").at("caseSensitive") == false,
                "Alt+C did not toggle find case sensitivity");
        key(window, SDLK_TAB);
        key(window, SDLK_SPACE);
        require(window.view_state().inline_form.at("values").at("caseSensitive") == true,
                "Focused toggle did not handle Space");
        key(window, SDLK_TAB, SDL_KMOD_SHIFT);
        key(window, SDLK_R, SDL_KMOD_ALT);
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, "(");
        require(window.view_state().inline_form.at("error") == true && window.last_error().empty(),
                "Invalid find regex did not report a component error");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, "alpha");
        key(window, SDLK_R, SDL_KMOD_ALT);
        require(window.view_state().inline_form.at("error") == false,
                "Corrected query retained a stale component error");
        key(window, SDLK_RETURN);
        require(read_document(host, id).at("selections").front().at("anchor") == 1,
                "Find selected the wrong UTF-8 match");
        key(window, SDLK_RETURN, SDL_KMOD_SHIFT);
        require(read_document(host, id).at("selections").front().at("anchor") == 1,
                "Previous match did not wrap");
        require(window.view_state().inline_form.at("message") == "Match found · wrapped",
                "Backward wrap did not update find component");
        click(window, window.view_state().inline_form_layout->actions[1]);
        require(read_document(host, id).at("selections").front().at("anchor") == 1,
                "Mouse Next did not use the same search policy");
        key(window, SDLK_H, SDL_KMOD_CTRL);
        text(window, "ALPHA");
        require(window.view_state().inline_form.at("values").at("query") == "alpha",
                "Find-to-replace lost query state");
        window.request_screenshot(remote ? "build/graphics/gui-inline-replace-tcp.png"
                                         : "build/graphics/gui-inline-replace.png");
        settle(window);
        key(window, SDLK_RETURN);
        require(read_document(host, id).at("text") == "XALPHA\nXbeta",
                "Replace did not edit the document");
        key(window, SDLK_ESCAPE);
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "Xalpha\nXbeta",
                "Document replace was not one undo transaction");
        require(concurrent->find_requests == search_requests + 3 &&
                    concurrent->replace_requests == replacement_requests + 1 &&
                    concurrent->preview_requests > 0,
                "Document find/replace bypassed the asynchronous service APIs");
        require(concurrent->text_downloads == search_downloads,
                "Document search downloaded full text to the GUI");
        concurrent->hold_next_find = true;
        window.execute_ui_command("acceptance.findWithoutInput");
        pump_until(
            window,
            [&] {
                return concurrent->find_requests == search_requests + 4;
            },
            "Document search was not dispatched");
        SDL_Event live_input{};
        live_input.type = SDL_EVENT_TEXT_INPUT;
        live_input.text.text = "live";
        live_input.text.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&live_input), "Push editing during search failed");
        pump_until(
            window,
            [&] {
                return read_document(host, id).at("text") == "Xlive\nXbeta";
            },
            "Pending search blocked SDL editing");
        const auto live_document = read_document(host, id);
        window.execute_ui_command("ui.cancel");
        for (int tick = 0; tick < 16; ++tick)
            require(window.pump(), "Search cancellation closed the window");
        concurrent->release_find();
        settle(window);
        require(read_document(host, id) == live_document,
                "Late cancelled search changed live editing or selections");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "Xalpha\nXbeta",
                "Editing during search did not retain undo history");
        key(window, SDLK_F, SDL_KMOD_CTRL);
        const auto previews = concurrent->preview_requests;
        concurrent->hold_next_find = true;
        live_input.text.text = "Xbeta";
        require(SDL_PushEvent(&live_input), "Cannot update query during nonmodal acceptance");
        pump_until(
            window,
            [&] {
                return concurrent->preview_requests == previews + 1;
            },
            "Nonmodal query did not start an asynchronous preview");
        const auto editing_area = window.layout().editor;
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_DOWN,
                editing_area.x + 120 * window.layout().scale,
                editing_area.y + 10 * window.layout().scale);
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_UP,
                editing_area.x + 120 * window.layout().scale,
                editing_area.y + 10 * window.layout().scale);
        live_input.text.text = "live";
        require(SDL_PushEvent(&live_input), "Cannot enqueue editor input with visible search");
        pump_until(
            window,
            [&] {
                return read_document(host, id).at("text").get<std::string>().find("live") !=
                       std::string::npos;
            },
            "Visible pending find component blocked editor input");
        require(window.view_state().inline_form_layout.has_value(),
                "Editing outside find unexpectedly closed its component");
        const auto edited_with_find = read_document(host, id);
        window.execute_ui_command("ui.cancel");
        for (int tick = 0; tick < 16; ++tick)
            require(window.pump(), "Find cancellation closed editor");
        concurrent->release_find();
        settle(window);
        require(read_document(host, id) == edited_with_find,
                "Late preview overwrote editing after closing the find component");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == "Xalpha\nXbeta",
                "Nonmodal editing lost its undo transaction");
        key(window, SDLK_F, SDL_KMOD_CTRL);
        live_input.text.text = "remembered 中🙂";
        require(SDL_PushEvent(&live_input),
                "Cannot enqueue last query before immediate cancellation");
        SDL_Event immediate_cancel{};
        immediate_cancel.type = SDL_EVENT_KEY_DOWN;
        immediate_cancel.key.windowID = SDL_GetWindowID(window.window());
        immediate_cancel.key.key = SDLK_ESCAPE;
        immediate_cancel.key.down = true;
        require(SDL_PushEvent(&immediate_cancel), "Cannot enqueue same-batch find cancellation");
        settle(window);
        require(!window.view_state().inline_form_layout, "Immediate cancellation kept find open");
        key(window, SDLK_F, SDL_KMOD_CTRL);
        require(window.view_state().inline_form.at("values").at("query") == "remembered 中🙂",
                "Same-batch cancellation lost the latest find input");
        key(window, SDLK_ESCAPE);
        std::cerr << "GUI acceptance: document search passed\n";

        key(window, SDLK_F, SDL_KMOD_CTRL);
        const auto held_document_form = window.view_state().inline_form;
        const auto body_before_workspace_form = read_document(host, id).at("text");
        window.execute_ui_command("ui.workspaceReplace");
        settle(window);
        require(window.view_state().sidebar_form_layout.has_value(),
                "Workspace search was not mounted in the sidebar");
        require(window.view_state().inline_form.at("id") == held_document_form.at("id"),
                "Mounting workspace search replaced document find");
        auto mounted = window.view_state();
        require(mounted.layout.sidebar_rows.y >= mounted.sidebar_form_layout->panel.bottom(),
                "Workspace results overlapped search inputs");
        text(window, "temporary replacement");
        require(window.view_state().inline_form.at("values") == held_document_form.at("values"),
                "Workspace input modified document find");
        require(read_document(host, id).at("text") == body_before_workspace_form,
                "Workspace input edited the document body");
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_DOWN,
                mounted.layout.editor.x + 100 * mounted.layout.scale,
                mounted.layout.editor.y + 10 * mounted.layout.scale);
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_UP,
                mounted.layout.editor.x + 100 * mounted.layout.scale,
                mounted.layout.editor.y + 10 * mounted.layout.scale);
        text(window, "editable");
        require(read_document(host, id).at("text") != body_before_workspace_form,
                "Mounted workspace input blocked body editing");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, id).at("text") == body_before_workspace_form,
                "Body undo with mounted inputs lost content");
        mounted = window.view_state();
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_DOWN,
                mounted.sidebar_form_layout->fields[0].x + 15 * mounted.layout.scale,
                mounted.sidebar_form_layout->fields[0].y + 10 * mounted.layout.scale);
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_UP,
                mounted.sidebar_form_layout->fields[0].x + 15 * mounted.layout.scale,
                mounted.sidebar_form_layout->fields[0].y + 10 * mounted.layout.scale);
        window.execute_ui_command("workbench.cancelDocumentSearch");
        settle(window);
        window.execute_ui_command("acceptance.prepareUnsavedReplacement");
        settle(window);
        const auto replacement_tabs = window.view_state().tabs;
        const auto unsaved_found =
            std::find_if(replacement_tabs.begin(), replacement_tabs.end(), [&](const auto& tab) {
                return tab != id;
            });
        require(unsaved_found != replacement_tabs.end(),
                "Unsaved replacement buffer was not created");
        const auto unsaved_replacement = *unsaved_found;
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, "needle");
        key(window, SDLK_RETURN);
        key(window, SDLK_TAB);
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window, "thread");
        key(window, SDLK_RETURN);
        require(astra::io::read_file(workspace / "rename.txt").text == "needle and needle\n",
                "Workspace replace ran before confirmation");
        require(read_document(host, unsaved_replacement).at("text") == "needle in unsaved",
                "Workspace replacement changed unsaved content before confirmation");
        window.request_screenshot("build/graphics/gui-replace-confirm.png");
        settle(window);
        window.execute_ui_command("ui.confirmReplace");
        settle(window);
        require(astra::io::read_file(workspace / "rename.txt").text == "thread and thread\n",
                "Workspace replace did not update disk");
        require(read_document(host, unsaved_replacement).at("text") == "thread in unsaved" &&
                    read_document(host, unsaved_replacement).at("dirty") == true,
                "Workspace replacement did not preserve an unsaved editable buffer");
        window.execute_ui_command("acceptance.undoUnsavedReplacement");
        settle(window);
        require(read_document(host, unsaved_replacement).at("text") == "needle in unsaved",
                "Workspace replacement was not one undoable unsaved-buffer edit");
        window.execute_ui_command("acceptance.closeUnsavedReplacement");
        settle(window);
        std::cerr << "GUI acceptance: workspace replace passed\n";

        key(window, SDLK_F, SDL_KMOD_CTRL);
        const auto document_form_before_cancel = window.view_state().inline_form.at("id");
        window.execute_ui_command("ui.search");
        settle(window);
        SDL_Event workspace_input{};
        workspace_input.type = SDL_EVENT_TEXT_INPUT;
        workspace_input.text.windowID = SDL_GetWindowID(window.window());
        workspace_input.text.text = "latest 中🙂";
        SDL_Event workspace_cancel{};
        workspace_cancel.type = SDL_EVENT_KEY_DOWN;
        workspace_cancel.key.windowID = SDL_GetWindowID(window.window());
        workspace_cancel.key.key = SDLK_ESCAPE;
        workspace_cancel.key.down = true;
        require(SDL_PushEvent(&workspace_input) && SDL_PushEvent(&workspace_cancel),
                "Cannot enqueue same-batch workspace input and cancellation");
        settle(window);
        require(window.view_state().sidebar_form.at("values").at("query") == "latest 中🙂",
                "Workspace cancellation lost retained input");
        require(window.view_state().inline_form.at("id") == document_form_before_cancel,
                "Workspace cancellation closed document find");
        window.execute_ui_command("ui.search");
        settle(window);
        require(window.view_state().sidebar_form.at("values").at("query") == "latest 中🙂",
                "Refocusing workspace search lost input");
        window.request_screenshot("build/graphics/gui-workspace-search.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        window.execute_ui_command("workbench.cancelDocumentSearch");
        settle(window);

        std::cerr << "GUI acceptance: opening dirty close after workspace replace\n";
        window.execute_ui_command("ui.closeTab");
        window.request_screenshot("build/graphics/gui-dirty-close.png");
        settle(window);
        require(window.active_document_id() == id, "Dirty close silently discarded a document");
        window.execute_ui_command("ui.cancel");
        settle(window);
        std::cerr << "GUI acceptance: saving after workspace replace\n";
        key(window, SDLK_S, SDL_KMOD_CTRL);
        require(!read_document(host, id).at("dirty").get<bool>(),
                "Save did not mark the persisted revision clean");
        require(astra::io::read_file(workspace / "src/main.cpp").text == "Xalpha\nXbeta",
                "Save output differs from the service document");

        window.execute_ui_command("ui.closeTab");
        settle(window);
        require(window.active_document_id().empty(), "Clean close did not close the tab");
        window.open_path("src/main.cpp");
        settle(window);
        require(read_document(host, window.active_document_id()).at("text") == "Xalpha\nXbeta",
                "Reopen lost saved text");
        key(window, SDLK_A, SDL_KMOD_CTRL);
        text(window,
             "// Astra · real editing, real pixels\n\n#include <iostream>\n\nint main() {\n    // "
             "中文、emoji 🙂 与组合字符 e\xCC\x81\n    std::cout << \"Hello, Astra\" << '\\n';\n   "
             " return 0;\n}\n\nclass Sample { public: int field; };\n");
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        for (unsigned i = 0; i < 4; ++i) {
            key(window, SDLK_DOWN);
        }
        std::cerr << "GUI syntax objects: selecting function\n";
        window.execute_ui_command("ui.selectFunction");
        settle(window);
        const auto selected_function =
            read_document(host, window.active_document_id()).at("selections").front();
        require(selected_function.at("head") != selected_function.at("anchor"),
                "Syntax object selection did not use the parsed function");
        const auto object_id = window.active_document_id();
        const auto before_objects = read_document(host, object_id);
        const auto object_source = before_objects.at("text").get<std::string>();
        const auto select_object_at = [&](const char* command, std::size_t offset) {
            const auto deferred_before = concurrent->notification_deferrals;
            concurrent->defer_notifications_once();
            const auto positioned =
                call(host,
                     "document.setSelections",
                     {{"id", object_id},
                      {"baseRevision", before_objects.at("revision")},
                      {"selections", Json::array({{{"anchor", offset}, {"head", offset}}})}});
            // This mutation uses another client. Native idle cannot prove that
            // its TCP notification has arrived at the GUI connection.
            pump_until(
                window,
                [&] {
                    return concurrent->observed_selection(positioned) && window.idle();
                },
                std::string("GUI did not observe external selection before ") + command);
            require(concurrent->notification_deferrals == deferred_before + 1,
                    "External selection test did not exercise delayed notification delivery");
            std::cerr << "GUI syntax objects: observed external selection for " << command << '\n';
            window.execute_ui_command(command);
            settle(window);
            const auto selected = read_document(host, object_id);
            require(selected.at("text") == object_source &&
                        selected.at("revision") == before_objects.at("revision") &&
                        selected.at("dirty") == before_objects.at("dirty"),
                    "Syntax selection changed document content, revision or save state");
            return selected.at("selections").front();
        };
        const auto string_begin = object_source.find("Hello, Astra");
        const auto selected_string = select_object_at("ui.selectString", string_begin + 2);
        require(selected_string.at("anchor") == string_begin &&
                    selected_string.at("head") ==
                        string_begin + std::string_view("Hello, Astra").size(),
                "JS string policy did not select its Unicode byte-positioned inner content");
        const auto selected_block =
            select_object_at("ui.selectBlock", object_source.find("return 0"));
        require(object_source.at(selected_block.at("anchor").get<std::size_t>()) == '{' &&
                    object_source.at(selected_block.at("head").get<std::size_t>() - 1) == '}',
                "JS block policy did not select the enclosing block");
        const auto selected_class =
            select_object_at("ui.selectClass", object_source.find("int field"));
        require(selected_class.at("anchor") == object_source.find("class Sample") &&
                    object_source.at(selected_class.at("head").get<std::size_t>() - 1) == '}',
                "JS class policy did not select the enclosing class");
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        window.execute_ui_command("ui.explorer");
        settle(window);
        window.request_screenshot(remote ? "build/graphics/gui-editor-tcp.png"
                                         : "build/graphics/gui-editor.png");
        settle(window);
        window.execute_ui_command("ui.commandPalette");
        window.request_screenshot("build/graphics/gui-palette.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        window.execute_ui_command("ui.commandPalette");
        settle(window);
        text(window, "User sidebar policy");
        key(window, SDLK_RETURN);
        require(window.status_text() == "User sidebar policy",
                "JS command palette bypassed the current user command");
        key(window, SDLK_B, SDL_KMOD_CTRL);
        key(window, SDLK_END, SDL_KMOD_CTRL);
        window.execute_ui_command("ui.commandPalette");
        settle(window);
        text(window, "Move to document start");
        key(window, SDLK_RETURN);
        require(
            read_document(host, window.active_document_id()).at("selections").front().at("head") ==
                0,
            "JS command palette did not await and publish a service editing command");
        window.open_path("closing.txt");
        settle(window);
        key(window, SDLK_END, SDL_KMOD_CTRL);
        const auto closing_id = window.active_document_id();
        SDL_Event closing_input{};
        closing_input.type = SDL_EVENT_TEXT_INPUT;
        closing_input.text.text = " change";
        closing_input.text.windowID = SDL_GetWindowID(window.window());
        SDL_Event closing_key{};
        closing_key.type = SDL_EVENT_KEY_DOWN;
        closing_key.key.key = SDLK_W;
        closing_key.key.mod = SDL_KMOD_CTRL;
        closing_key.key.down = true;
        closing_key.key.windowID = SDL_GetWindowID(window.window());
        require(SDL_PushEvent(&closing_input) && SDL_PushEvent(&closing_key),
                "Cannot enqueue same-batch input and close");
        settle(window);
        require(window.active_document_id() == closing_id &&
                    read_document(host, closing_id).at("text") == "baseline change",
                "Close bypassed preceding input or discarded newly dirty content");
        require(astra::io::read_file(workspace / "closing.txt").text == "baseline",
                "Close saved before its explicit choice");
        window.request_screenshot("build/graphics/gui-close-choice.png");
        settle(window);
        key(window, SDLK_RETURN);
        require(window.active_document_id() != closing_id &&
                    astra::io::read_file(workspace / "closing.txt").text == "baseline change",
                "JS save-and-close did not await durable save");
        window.open_path("closing.txt");
        settle(window);
        key(window, SDLK_END, SDL_KMOD_CTRL);
        text(window, " discard");
        window.execute_ui_command("ui.closeTab");
        settle(window);
        key(window, SDLK_D);
        require(astra::io::read_file(workspace / "closing.txt").text == "baseline change",
                "Discard-close wrote dirty content");
        window.open_path("closing.txt");
        settle(window);
        key(window, SDLK_END, SDL_KMOD_CTRL);
        text(window, " cancel");
        const auto cancelled_id = window.active_document_id();
        window.execute_ui_command("ui.closeTab");
        settle(window);
        key(window, SDLK_ESCAPE);
        require(window.active_document_id() == cancelled_id &&
                    read_document(host, cancelled_id).at("dirty") == true,
                "Cancel-close discarded the captured dirty document");
        window.execute_ui_command("ui.closeTab");
        settle(window);
        window.execute_ui_command("ui.discard");
        settle(window);
        require(window.active_document_id() != cancelled_id,
                "JS close decision command did not answer the matching native choice");
        window.execute_ui_command("ui.new");
        settle(window);
        text(window, "untitled preserved");
        const auto untitled_id = window.active_document_id();
        window.execute_ui_command("ui.closeTab");
        settle(window);
        key(window, SDLK_RETURN);
        key(window, SDLK_ESCAPE);
        require(window.active_document_id() == untitled_id &&
                    read_document(host, untitled_id).at("text") == "untitled preserved",
                "Cancelling Save As lost an untitled buffer during close");
        window.execute_ui_command("ui.closeTab");
        settle(window);
        key(window, SDLK_D);
        window.open_path("cache.cpp");
        settle(window);
        const auto cache_id = window.active_document_id();
        const auto initial_cache = window.view_state().document_viewport;
        require(initial_cache.at("cached") > 10 && initial_cache.at("rows") <= 256,
                "Document viewport did not publish bounded cached rows");
        const auto scroll = [&](float delta) {
            const auto geometry = window.layout();
            int logical_width{}, logical_height{}, pixels_width{}, pixels_height{};
            require(SDL_GetWindowSize(window.window(), &logical_width, &logical_height) &&
                        SDL_GetWindowSizeInPixels(window.window(), &pixels_width, &pixels_height),
                    "Cannot read viewport scroll dimensions");
            SDL_Event wheel{};
            wheel.type = SDL_EVENT_MOUSE_WHEEL;
            wheel.wheel.windowID = SDL_GetWindowID(window.window());
            wheel.wheel.y = delta;
            wheel.wheel.mouse_x = (geometry.editor.x + 100 * geometry.scale) *
                                  static_cast<float>(logical_width) /
                                  static_cast<float>(pixels_width);
            wheel.wheel.mouse_y = (geometry.editor.y + 100 * geometry.scale) *
                                  static_cast<float>(logical_height) /
                                  static_cast<float>(pixels_height);
            require(SDL_PushEvent(&wheel), "Cannot enqueue viewport scrolling");
            settle(window);
        };
        scroll(-1);
        const auto after_scroll = window.view_state().document_viewport;
        require(after_scroll.at("shaped").get<std::size_t>() <=
                        initial_cache.at("shaped").get<std::size_t>() + 6 &&
                    after_scroll.at("reused") > initial_cache.at("reused"),
                "Small scroll reshaped unchanged viewport rows");
        scroll(1);
        const auto before_edit = window.view_state().document_viewport;
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        text(window, "// changed\n");
        const auto after_edit = window.view_state().document_viewport;
        require(after_edit.at("shaped").get<std::size_t>() <=
                        before_edit.at("shaped").get<std::size_t>() + 6 &&
                    after_edit.at("reused") > before_edit.at("reused"),
                "One line insertion reshaped unchanged shifted code rows");
        window.request_screenshot(std::filesystem::path("build/graphics") /
                                  (remote ? "viewport-cache-tcp.png" : "viewport-cache-local.png"));
        settle(window);
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_document(host, cache_id).at("text") ==
                    astra::io::read_file(workspace / "cache.cpp").text,
                "Viewport reuse changed document undo behavior");
        const auto after_undo = window.view_state().document_viewport;
        require(after_undo.at("cached") <= 256 &&
                    after_undo.at("accountedBytes") <= 8 * 1024 * 1024,
                "GUI viewport layout cache exceeded its budgets");
        std::cout << "GUI viewport scroll/edit cache: initial=" << initial_cache.at("shaped")
                  << ", scroll=" << after_scroll.at("shaped")
                  << ", edit=" << after_edit.at("shaped") << ", undo=" << after_undo.at("shaped")
                  << ": PASS\n";
        window.execute_ui_command("ui.closeTab");
        settle(window);
        window.open_path("long.txt");
        const auto cold_deadline = Clock::now() + std::chrono::seconds(15);
        while (!window.view_state().document_viewport.value("layoutsPending", false)) {
            require(Clock::now() < cold_deadline && window.pump(),
                    "Cold long paragraph was never queued");
            require(window.last_error().empty(), "Cold paragraph failed: " + window.last_error());
            SDL_Delay(1);
        }
        const auto long_id = window.active_document_id();
        const auto editor = window.layout().editor;
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_DOWN,
                editor.x + 120 * window.layout().scale,
                editor.y + 10 * window.layout().scale);
        pointer(window,
                SDL_EVENT_MOUSE_BUTTON_UP,
                editor.x + 120 * window.layout().scale,
                editor.y + 10 * window.layout().scale);
        text(window, "Ω");
        const auto pointed = read_document(host, long_id).at("text").get<std::string>();
        require(pointed.find("Ω") > 0 && pointed.find("Ω") < 32,
                "Typing overtook a pointer awaiting long-row geometry");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        require(read_all_text(host, long_id) == long_line,
                "Deferred pointer/text transaction could not be undone");
        key(window, SDLK_END, SDL_KMOD_CTRL);
        text(window, "!");
        key(window, SDLK_S, SDL_KMOD_CTRL);
        require(astra::io::read_file(workspace / "long.txt").text == long_line + "!",
                "Long-line continuation or caret clipped text beyond 64 KiB");
        window.execute_ui_command("ui.closeTab");
        settle(window);
        window.open_path("long-navigation.txt");
        const auto navigation_deadline = Clock::now() + std::chrono::seconds(15);
        while (!window.view_state().document_viewport.value("layoutsPending", false)) {
            require(Clock::now() < navigation_deadline && window.pump(),
                    "Navigation long paragraph was never queued");
            require(window.last_error().empty(),
                    "Navigation paragraph failed: " + window.last_error());
            SDL_Delay(1);
        }
        const auto navigation_id = window.active_document_id();
        for (const auto code : {SDLK_END, SDLK_DOWN}) {
            SDL_Event input{};
            input.type = SDL_EVENT_KEY_DOWN;
            input.key.windowID = SDL_GetWindowID(window.window());
            input.key.key = code;
            input.key.down = true;
            require(SDL_PushEvent(&input), "Cannot queue cold long-row navigation");
        }
        text(window, "Ω");
        astra::ui::TextLayoutEngine navigation_fonts;
        astra::ui::TextLayoutOptions navigation_style;
        navigation_style.dpi_scale = window.layout().scale;
        navigation_style.font_families = {"Cascadia Code", "Consolas", "monospace"};
        const auto source_layout = navigation_fonts.layout(navigation_source, navigation_style);
        const auto target_layout = navigation_fonts.layout(navigation_target, navigation_style);
        const auto expected_head = target_layout
                                       .hit_test(source_layout.caret(navigation_source.size()).x,
                                                 target_layout.height() / 2)
                                       .byte_offset;
        const auto navigated = read_all_text(host, navigation_id);
        require(navigated.find("Ω") == navigation_source.size() + 1 + expected_head,
                "Long-row vertical navigation used a text column instead of actual geometry");
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        window.execute_ui_command("ui.closeTab");
        settle(window);
        std::cout << "GUI " << (remote ? "TCP" : "local")
                  << " real SDL input -> JS policy -> shared dispatcher: open, Unicode, "
                     "multicursor, undo/redo, clipboard, find/replace, workspace replace, syntax "
                     "objects, dirty close, save/reopen: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI smoke failed: " << error.what() << '\n';
        return 1;
    }
}
