#include <astra/io/file_store.hpp>
#include <astra/service/service_host.hpp>
#include <astra/ui/editor_window.hpp>
#include <astra/ui/text_layout.hpp>
#include "support/temporary_directory.hpp"

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using Clock = std::chrono::steady_clock;
using Window = astra::ui::EditorWindow;
namespace fs = std::filesystem;

void require(bool value, const std::string& message) {
    if (!value)
        throw std::runtime_error(message);
}

void settle(Window& window) {
    const auto until = Clock::now() + std::chrono::seconds(20);
    do {
        require(window.pump(), "Window closed during presentation acceptance");
        require(window.last_error().empty(), window.last_error());
        if (window.idle())
            return;
        SDL_Delay(1);
    } while (Clock::now() < until);
    throw std::runtime_error("Presentation workbench did not settle");
}

void enqueue_mouse(Window& window, Uint32 type, float x, float y) {
    int width{}, height{}, pixels_x{}, pixels_y{};
    require(SDL_GetWindowSize(window.window(), &width, &height) &&
                SDL_GetWindowSizeInPixels(window.window(), &pixels_x, &pixels_y),
            "Cannot read window dimensions");
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
    require(SDL_PushEvent(&event), "Cannot enqueue native pointer input");
}

void mouse(Window& window, Uint32 type, float x, float y) {
    enqueue_mouse(window, type, x, y);
    settle(window);
}

void click(Window& window, float x, float y) {
    mouse(window, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
    mouse(window, SDL_EVENT_MOUSE_BUTTON_UP, x, y);
}

void enqueue_key(Window& window, SDL_Keycode code, SDL_Keymod mods = SDL_KMOD_NONE) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.windowID = SDL_GetWindowID(window.window());
    event.key.key = code;
    event.key.mod = mods;
    event.key.down = true;
    require(SDL_PushEvent(&event), "Cannot enqueue native key input");
}

void key(Window& window, SDL_Keycode code, SDL_Keymod mods = SDL_KMOD_NONE) {
    enqueue_key(window, code, mods);
    settle(window);
}

void await_surface(Window& window) {
    const auto until = Clock::now() + std::chrono::seconds(20);
    while (!(window.view_state().text_surface.is_object() &&
             window.view_state().text_surface.value("pending", false))) {
        require(Clock::now() < until && window.pump(),
                "Long modal text never queued native shaping");
        require(window.last_error().empty(), window.last_error());
        SDL_Delay(1);
    }
}

void await_input(Window& window, std::size_t submitted) {
    const auto until = Clock::now() + std::chrono::seconds(20);
    while (!window.view_state().input_layout.value("pending", false) ||
           window.view_state().input_layout.at("workerSubmitted").get<std::size_t>() <= submitted) {
        require(Clock::now() < until && window.pump(),
                "Long input never submitted shaping: " + window.view_state().input_layout.dump());
        require(window.last_error().empty(), window.last_error());
        SDL_Delay(1);
    }
}

void run(const fs::path& workspace) {
    constexpr int files = 1500;
    fs::create_directory(workspace / "generated");
    for (int i = 0; i < files; ++i) {
        std::ofstream output(workspace / "generated" / ("item" + std::to_string(i) + ".txt"));
        output << "indexed row\n";
    }
    for (const auto* name : {"one.cpp", "two_with_a_long_name.cpp", "three.cpp"}) {
        std::ofstream output(workspace / name);
        output << "// Astra workspace\nint answer(int input) { return input + 42; }\n";
        if (std::string_view(name) == "one.cpp")
            for (int i = 0; i < 80; ++i)
                output << "int outline" << i << "() { return " << i << "; }\n";
    }
    {
        std::ofstream output(workspace / ".clangd");
        output << "CompileFlags:\n  Add: [-nostdinc, -nostdinc++]\n";
    }
    astra::service::ServiceHostOptions service;
    service.workspace = workspace;
    service.defaults_script = astra::io::read_file("data/defaults.js").text;
    service.resource_directory = fs::current_path();
    service.watch_files = false;
    auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
    {
        astra::ui::EditorWindowOptions options;
        options.workspace_name = "ASTRA WORKBENCH";
        options.resource_directory = fs::current_path();
        options.open_paths = {"one.cpp"};
        options.user_script = R"js(if(astra.context==='client') {
            let nextGate, releaseNext, waitInView=false;
            astra.ui.registerCommand({id:'ui.view.next',title:'Custom view next',palette:false},async ({id,...guards})=>{
                if(nextGate) await nextGate;
                await astra.sleep(8);
                return await astra.ui.views.navigate(id,'next',guards);
            });
            astra.registerCommand('acceptance.holdNext',()=>{
                nextGate=new Promise(resolve=>{releaseNext=resolve;});
            });
            astra.registerCommand('acceptance.releaseNext',()=>{releaseNext();nextGate=undefined;});
            astra.registerCommand('acceptance.waitInView',()=>{waitInView=true;});
            astra.registerCommand('acceptance.focusView',()=>astra.ui.views.focus('acceptance-view'));
            astra.registerCommand('acceptance.refreshView',()=>astra.ui.refresh('acceptance-view'));
            astra.registerCommand('acceptance.largeView', () => astra.ui.refresh('acceptance-view', {large:true}));
            astra.registerCommand('acceptance.largePending', () => astra.ui.refresh('acceptance-view', {large:true,pause:true}));
            astra.registerCommand('acceptance.cancelLarge', () => astra.ui.cancel('acceptance-view'));
            let capturedFocus;
            astra.registerCommand('acceptance.captureFocus', async () => {
                capturedFocus = await astra.ui.views.read('acceptance-view');
            });
            astra.registerCommand('acceptance.staleBlur', async () => {
                const accepted = await astra.ui.views.blur({id: capturedFocus.id, focus: capturedFocus.focus});
                if (accepted) throw Error('Stale focus return was accepted');
            });
            let rowGate, releaseRows;
            astra.registerCommand('acceptance.pauseRows', () => {
                rowGate = new Promise(resolve => { releaseRows = resolve; });
            });
            astra.registerCommand('acceptance.releaseRows', () => {
                releaseRows();
                rowGate = undefined;
            });
            let capturedBody;
            astra.registerCommand('acceptance.captureBody',async()=>{
                const {document}=await astra.ui.read();
                capturedBody=document;
            });
            astra.registerCommand('acceptance.verifyBody',async()=>{
                const document=await astra.request('document.get',{id:capturedBody.id});
                if(document.revision!==capturedBody.revision ||
                    JSON.stringify(document.selections)!==JSON.stringify(capturedBody.selections))
                    throw Error('Sidebar input mutated document state');
                await astra.ui.update({status:'Sidebar text isolated'});
            });
            astra.ui.registerView({id:'acceptance-view',title:'Custom 中🙂',icon:'symbol',order:3},
                ({large,pause}, options) => ({
                    rows: (async function* () {
                        yield {id:'group',label:'Custom group',group:true,icon:'folder'};
                        for (let index=0; index<(large ? 20000 : 600); ++index) {
                            if (pause && index===15000) {
                                const pending=astra.sleep(200000,options);
                                await astra.ui.update({status:'Large builder waiting'});
                                await pending;
                            }
                            if (index===256 && rowGate) {
                                await astra.ui.update({status:'Stream waiting'});
                                await rowGate;
                                options.signal.throwIfAborted();
                            }
                            yield {id:String(index),label:'Custom row '+index+' 中🙂',depth:1,
                                icon:'symbol',data:{index}};
                        }
                        yield {id:'tail',label:'Final row',data:{index:600}};
                    })(),
                    complete: () => ({subtitle:'Registered by JavaScript'})
                }), {
                    activate:async ({index}) => {
                        if(waitInView) await astra.ui.waitForIdle();
                        await astra.ui.update({status:'Custom row '+index+' 中🙂'});
                    }
                });
            astra.registerCommand('acceptance.view',() => astra.ui.showView('acceptance-view'));
            astra.registerCommand('acceptance.invalidView',async () => {
                try {
                    await astra.ui.renderView('acceptance-view',{
                        rows:[{id:'invalid',label:'Invalid',depth:1}]
                    });
                } catch (error) {
                    await astra.ui.update({status:'Invalid view retained old rows'});
                }
            });
            let backgroundOpenComplete=false;
            astra.registerCommand('acceptance.backgroundOpen',async()=>{
                await astra.sleep(100);
                const document=await astra.request('workspace.open',{path:'three.cpp'});
                await astra.ui.update({document,activate:document.id});backgroundOpenComplete=true;
            });
            astra.registerCommand('ui.checkpoint',async context=>{
                if(!backgroundOpenComplete)throw Error('Exit checkpoint overtook accepted script work');
                return await astra.request('recovery.flush',{},context);
            });
            astra.registerCommand('acceptance.waiters',async()=>{
                const [idle,closed]=await Promise.all([astra.ui.waitForIdle(),astra.ui.requestClose()]);
                if(!idle || closed)throw Error('Concurrent native waits did not retain open documents');
                await astra.ui.update({status:'Concurrent waits settled'});
            });
            astra.ui.registerCommand({id:'acceptance.choice',title:'Choose an action'},async()=>{
                const selected=await astra.ui.choose({title:'CHOOSE AN ACTION',detail:'A generic native component returns the selected identity to JavaScript.',
                    choices:[{label:'First 中🙂',value:1},{label:'Second',value:2},{label:'Third',value:3},{label:'Fourth',value:4}],defaultChoice:2,keys:['a','b','c','d']});
                await astra.ui.update({status:selected ? 'Action '+selected.value : 'Action cancelled'});
            });
            astra.ui.registerCommand({id:'acceptance.review',title:'Review items'},async()=>{
                const accepted=await astra.ui.confirm({title:'Review changes',items:Array.from({length:40},(_,i)=>'File '+i+' (2 edits)'),accept:'Apply changes'});
                await astra.ui.update({status:accepted?'Review accepted':'Review cancelled'});
            });
            astra.ui.registerCommand({id:'acceptance.pick',title:'Choose item'},async()=>{
                const choice=await astra.ui.pick({title:'CHOOSE A FILE — TYPE TO FILTER',items:Array.from({length:40},(_,i)=>({label:'File '+i+' 中🙂',value:i}))});
                await astra.ui.update({status:choice ? 'Selected '+choice.value : 'Choice cancelled'});
            });
            astra.ui.registerCommand(
                {
                    id: 'acceptance.inputEditing',
                    title: 'Unicode input editing'
                },
                async () => {
                    const value = await astra.ui.input({
                        title: 'UNICODE INPUT EDITING',
                        value: 'A🙂éB'
                    });
                    await astra.ui.update({
                        status: value === null ? 'Input cancelled' : 'Input ' + value
                    });
                }
            );
            const longInput = '中🙂éאב\t'.repeat(700);
            astra.ui.registerCommand(
                { id: 'acceptance.longInput', title: 'Long input' },
                async () => {
                    const value = await astra.ui.input({
                        title: 'LONG INPUT',
                        value: longInput
                    });
                    await astra.ui.update({
                        status: value === null
                            ? 'Long input cancelled'
                            : 'Long input ' + value.slice(0, 20) + ' length ' + value.length
                    });
                }
            );
            astra.ui.registerCommand(
                { id: 'acceptance.longForm', title: 'Long form input' },
                async () => {
                    await astra.ui.forms.open({
                        id: 'acceptance-long-input',
                        title: 'Long form input',
                        fields: [{ id: 'value', value: longInput, submit: 'accept' }],
                        actions: [{ id: 'accept', label: 'Accept' }]
                    });
                }
            );
            astra.on('ui.form.action', async (snapshot) => {
                if (snapshot.id === 'acceptance-long-input' && snapshot.action === 'accept') {
                    await astra.ui.update({ status: 'Long form accepted' });
                    await astra.ui.forms.close(snapshot.id);
                }
            });
            astra.ui.registerCommand(
                { id: 'acceptance.closeLongForm', title: 'Close long input through JS' },
                async () => {
                    await astra.ui.forms.close('acceptance-long-input');
                }
            );
            astra.ui.registerCommand(
                { id: 'acceptance.overwriteLongForm', title: 'Programmatic long input change' },
                async () => {
                    const state = await astra.ui.forms.read('acceptance-long-input');
                    await astra.sleep(0);
                    const result = await astra.ui.forms.setValues({
                        id: state.id,
                        version: state.version,
                        values: { value: 'changed' }
                    });
                    const current = result || await astra.ui.forms.read(state.id);
                    await astra.ui.forms.update({
                        id: current.id,
                        version: current.version,
                        message: result ? 'Overwrite accepted' : 'Overwrite stale'
                    });
                    await astra.ui.update({
                        status: result ? 'Overwrite accepted' : 'Overwrite stale'
                    });
                }
            );
            astra.ui.registerCommand({id:'acceptance.text',title:'Read indexed text'},async()=>{
                await astra.ui.showText({title:'INDEXED INFORMATION — Esc to close',text:Array.from({length:80},(_,i)=>'Line '+i+' 中🙂').join('\n')});
                await astra.ui.update({status:'Information closed'});
            });
            astra.ui.registerCommand({id:'acceptance.textAction',title:'Generic text action'},async()=>{
                const action=await astra.ui.showText({title:'GENERIC TEXT ACTIONS — Esc to close',
                    text:'Normal 中🙂\nAddition\nRemoval\nHeading',monospace:true,
                    lineTones:[{begin:1,end:2,tone:'added'},{begin:2,end:3,tone:'removed'},{begin:3,end:4,tone:'accent'}],
                    actions:[{id:'inspect',label:'Inspect 中🙂',key:'i'},{id:'apply',label:'Apply',key:'a'},{id:'remove',label:'Remove',key:'r',destructive:true}]});
                await astra.ui.update({status:action ? 'Text action '+action : 'Text action cancelled'});
            });
            astra.ui.registerCommand({id:'acceptance.longText',title:'Long text'},async()=>{
                await astra.ui.showText({title:'LONG TEXT — Esc to close',monospace:true,
                    text:'Native 中🙂\t'+'a'.repeat(70000)+'\n'+Array.from({length:80},(_,i)=>'Row '+i+' 中🙂').join('\n')});
                await astra.ui.update({status:'Long text closed'});
            });
            astra.ui.registerCommand({id:'acceptance.longChoice',title:'Long choice detail'},async()=>{
                const selected=await astra.ui.choose({title:'LONG CHOICE',detail:'Native 中🙂\t'+('word '.repeat(11000)),
                    choices:[{label:'First',value:1},{label:'Second',value:2}]});
                await astra.ui.update({status:selected ? 'Long choice '+selected.value : 'Long choice cancelled'});
            });
            astra.ui.registerCommand({id:'acceptance.longReview',title:'Long review detail'},async()=>{
                const accepted=await astra.ui.confirm({title:'LONG REVIEW',detail:'Native 中🙂\t'+('word '.repeat(11000))});
                await astra.ui.update({status:accepted ? 'Long review accepted' : 'Long review cancelled'});
            });
        })js";
        Window window(host, std::move(options));
        settle(window);
        const auto original = window.view_state();
        require(original.visible_file_rows == files + 5, "Indexed file rows were lost");
        require(original.layout.outline_rows.height == 0,
                "Outline should default to collapsed inside Explorer");
        const auto one = window.active_document_id();
        click(window,
              original.layout.sidebar_rows.x + 50 * original.layout.scale,
              original.layout.sidebar_rows.y + original.layout.sidebar_row_height / 2);
        require(window.view_state().visible_file_rows == 5,
                "Folder collapse did not remove its descendant range");
        click(window,
              original.layout.sidebar_rows.x + 50 * original.layout.scale,
              original.layout.sidebar_rows.y + original.layout.sidebar_row_height / 2);
        require(window.view_state().visible_file_rows == original.visible_file_rows,
                "Folder expansion did not restore its indexed descendants");
        const auto handle = window.layout().sidebar_handle;
        mouse(window,
              SDL_EVENT_MOUSE_BUTTON_DOWN,
              handle.x + handle.width / 2,
              handle.y + 80 * original.layout.scale);
        mouse(window,
              SDL_EVENT_MOUSE_MOTION,
              handle.x + handle.width / 2 + 90 * original.layout.scale,
              handle.y + 80 * original.layout.scale);
        mouse(window,
              SDL_EVENT_MOUSE_BUTTON_UP,
              handle.x + handle.width / 2 + 90 * original.layout.scale,
              handle.y + 80 * original.layout.scale);
        require(window.layout().sidebar.width >
                    original.layout.sidebar.width + 70 * original.layout.scale,
                "Sidebar splitter did not resize the native layout");
        click(window,
              window.layout().outline_header.x + 60 * original.layout.scale,
              window.layout().outline_header.y + 10 * original.layout.scale);
        require(window.layout().outline_rows.height > 0,
                "Outline header did not expand its native region");
        const auto before_outline = window.view_state();
        require(before_outline.outline_total == 81 && before_outline.visible_outline_rows < 20,
                "Outline did not request a bounded visible symbol page");
        // A wheel carries its own pointer location, independent of the last motion.
        mouse(window,
              SDL_EVENT_MOUSE_MOTION,
              window.layout().editor.x + 40 * original.layout.scale,
              window.layout().editor.y + 12 * original.layout.scale);
        int wheel_width{}, wheel_height{}, wheel_pixels_x{}, wheel_pixels_y{};
        require(SDL_GetWindowSize(window.window(), &wheel_width, &wheel_height) &&
                    SDL_GetWindowSizeInPixels(window.window(), &wheel_pixels_x, &wheel_pixels_y),
                "Cannot read wheel coordinate conversion");
        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL;
        wheel.wheel.windowID = SDL_GetWindowID(window.window());
        wheel.wheel.y = -20;
        wheel.wheel.mouse_x = (window.layout().outline_rows.x + 40 * original.layout.scale) *
                              static_cast<float>(wheel_width) / static_cast<float>(wheel_pixels_x);
        wheel.wheel.mouse_y = (window.layout().outline_rows.y + 12 * original.layout.scale) *
                              static_cast<float>(wheel_height) / static_cast<float>(wheel_pixels_y);
        require(SDL_PushEvent(&wheel), "Cannot enqueue Outline scrolling");
        settle(window);
        require(window.view_state().outline_first == 60,
                "Outline wheel did not seek its independent symbol index: " +
                    std::to_string(window.view_state().outline_first));
        require(window.view_state().file_first == before_outline.file_first,
                "Outline scrolling moved the Explorer viewport");
        window.request_screenshot("build/graphics/gui-workbench-outline.png");
        settle(window);
        click(window,
              window.layout().outline_rows.x + 70 * original.layout.scale,
              window.layout().outline_rows.y + 12 * original.layout.scale);
        auto verify = host->submit(nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", "outline-acceptance"},
            {"method", "document.get"},
            {"params", {{"id", one}}}}.dump());
        require(verify.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "Outline verification timed out");
        const auto response = verify.get();
        require(response.has_value(), "Outline verification has no reply");
        const auto metadata = nlohmann::json::parse(*response).at("result");
        require(metadata.at("selections").front().at("anchor") ==
                    astra::io::read_file(workspace / "one.cpp").text.find("outline59"),
                "Scrolled Outline pointer selected a different symbol");
        window.open_path("two_with_a_long_name.cpp");
        settle(window);
        const auto two = window.active_document_id();
        window.open_path("three.cpp");
        settle(window);
        const auto three = window.active_document_id();
        auto state = window.view_state();
        require(state.tab_strip.visible.size() == 3, "Wide workbench should show all three tabs");
        require(state.tab_strip.visible[0].bounds.width != state.tab_strip.visible[1].bounds.width,
                "Tab sizing ignored shaped filename widths");
        const auto first = state.tab_strip.visible.front().bounds,
                   last = state.tab_strip.visible.back().bounds;
        mouse(window,
              SDL_EVENT_MOUSE_BUTTON_DOWN,
              first.x + 45 * state.layout.scale,
              first.y + first.height / 2);
        mouse(window,
              SDL_EVENT_MOUSE_MOTION,
              last.x + 45 * state.layout.scale,
              last.y + last.height / 2);
        mouse(window,
              SDL_EVENT_MOUSE_BUTTON_UP,
              last.x + 45 * state.layout.scale,
              last.y + last.height / 2);
        require(window.view_state().tabs == std::vector<std::string>{two, three, one},
                "Native tab dragging did not reorder tabs");
        key(window, SDLK_TAB, SDL_KMOD_CTRL);
        require(window.active_document_id() == two,
                "JS tab cycling ignored the reordered view state");
        require(SDL_SetWindowSize(window.window(),
                                  static_cast<int>(680 * state.layout.scale),
                                  static_cast<int>(600 * state.layout.scale)),
                "Cannot resize the native SDL window");
        settle(window);
        require(window.layout().editor.width >= 200 * window.layout().scale,
                "Narrow layout lost the document region");
        require(window.view_state().tab_strip.next.width > 0,
                "Narrow workbench did not expose tab overflow controls");
        window.request_screenshot("build/graphics/gui-workbench-narrow.png");
        settle(window);
        require(SDL_SetWindowSize(window.window(),
                                  static_cast<int>(1440 * state.layout.scale),
                                  static_cast<int>(900 * state.layout.scale)),
                "Cannot restore the native SDL window");
        settle(window);
        window.execute_ui_command("ui.explorer");
        settle(window);
        window.request_screenshot("build/graphics/gui-workbench.png");
        settle(window);
        // Mode switches and text input can arrive in the same SDL event batch.
        SDL_Event mode{};
        mode.type = SDL_EVENT_KEY_DOWN;
        mode.key.windowID = SDL_GetWindowID(window.window());
        mode.key.key = SDLK_P;
        mode.key.mod = SDL_KMOD_CTRL;
        mode.key.down = true;
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.windowID = SDL_GetWindowID(window.window());
        text.text.text = "three.cpp";
        SDL_Event enter{};
        enter.type = SDL_EVENT_KEY_DOWN;
        enter.key.windowID = SDL_GetWindowID(window.window());
        enter.key.key = SDLK_RETURN;
        enter.key.down = true;
        require(SDL_PushEvent(&mode) && SDL_PushEvent(&text) && SDL_PushEvent(&enter),
                "Cannot enqueue quick-open event batch");
        settle(window);
        require(window.active_document_id() == three,
                "Quick open did not settle focus before same-frame typing");
        window.execute_ui_command("ui.saveAs");
        settle(window);
        window.request_screenshot("build/graphics/gui-workbench-input.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        require(!fs::exists(workspace / "renamed-three.cpp"),
                "Cancelled async save dialog wrote a file");
        mode.key.key = SDLK_S;
        mode.key.mod = SDL_KMOD_CTRL | SDL_KMOD_SHIFT;
        SDL_Event select = mode;
        select.key.key = SDLK_A;
        select.key.mod = SDL_KMOD_CTRL;
        text.text.text = "renamed-three.cpp";
        require(SDL_PushEvent(&mode) && SDL_PushEvent(&select) && SDL_PushEvent(&text) &&
                    SDL_PushEvent(&enter),
                "Cannot enqueue async save-as event batch");
        settle(window);
        require(astra::io::read_file(workspace / "renamed-three.cpp").text ==
                    astra::io::read_file(workspace / "three.cpp").text,
                "Async save dialog lost same-batch text/focus or changed document content");
        window.execute_ui_command("acceptance.review");
        settle(window);
        for (int row = 0; row < 10; ++row)
            key(window, SDLK_DOWN);
        window.request_screenshot("build/graphics/gui-workbench-review.png");
        settle(window);
        int review_width{}, review_height{};
        require(SDL_GetWindowSizeInPixels(window.window(), &review_width, &review_height),
                "Cannot read confirmation viewport");
        const auto review =
            astra::ui::ConfirmationLayout::measure(static_cast<float>(review_width),
                                                   static_cast<float>(review_height),
                                                   window.layout().scale,
                                                   40);
        click(window,
              review.cancel.x + review.cancel.width / 2,
              review.cancel.y + review.cancel.height / 2);
        require(window.status_text() == "Review cancelled",
                "Review pointer hit did not settle its awaited JS workflow");
        window.execute_ui_command("acceptance.pick");
        settle(window);
        for (int row = 0; row < 20; ++row)
            key(window, SDLK_DOWN);
        window.request_screenshot("build/graphics/gui-workbench-picker.png");
        settle(window);
        const auto picker = astra::ui::PickerLayout::measure(static_cast<float>(review_width),
                                                             static_cast<float>(review_height),
                                                             window.layout().scale);
        require(picker.capacity > 0, "Picker has no visible rows");
        const auto first_item = 20 >= picker.capacity ? 20 - picker.capacity + 1 : 0;
        click(window,
              picker.rows.x + 30 * window.layout().scale,
              picker.rows.y + picker.row_height / 2);
        require(window.status_text() == "Selected " + std::to_string(first_item),
                "Picker pointer selected a display row instead of its source identity");
        window.execute_ui_command("acceptance.pick");
        settle(window);
        SDL_Event filter{};
        filter.type = SDL_EVENT_TEXT_INPUT;
        filter.text.windowID = SDL_GetWindowID(window.window());
        filter.text.text = "File 39";
        require(SDL_PushEvent(&filter), "Cannot enqueue picker filter");
        settle(window);
        key(window, SDLK_RETURN);
        require(window.status_text() == "Selected 39",
                "Picker filtering lost its original source identity");
        window.execute_ui_command("acceptance.text");
        settle(window);
        key(window, SDLK_ESCAPE);
        window.execute_ui_command("acceptance.inputEditing");
        settle(window);
        key(window, SDLK_LEFT);
        key(window, SDLK_BACKSPACE);
        SDL_Event unicode_input{};
        unicode_input.type = SDL_EVENT_TEXT_INPUT;
        unicode_input.text.windowID = SDL_GetWindowID(window.window());
        unicode_input.text.text = "中";
        require(SDL_PushEvent(&unicode_input), "Cannot queue input replacement");
        settle(window);
        key(window, SDLK_Z, SDL_KMOD_CTRL);
        key(window, SDLK_Y, SDL_KMOD_CTRL);
        key(window, SDLK_HOME, SDL_KMOD_SHIFT);
        key(window, SDLK_C, SDL_KMOD_CTRL);
        const std::unique_ptr<char, decltype(&SDL_free)> copied(SDL_GetClipboardText(), SDL_free);
        require(copied && std::string(copied.get()) == "A🙂中",
                "Modal Unicode selection/copy lost boundaries");
        key(window, SDLK_RIGHT);
        key(window, SDLK_DELETE);
        key(window, SDLK_RETURN);
        require(window.status_text() == "Input A🙂中",
                "Modal cursor, grapheme editing or undo changed the result");
        window.execute_ui_command("acceptance.inputEditing");
        settle(window);
        key(window, SDLK_A, SDL_KMOD_CTRL);
        unicode_input.text.text = "0123456789🙂";
        require(SDL_PushEvent(&unicode_input), "Cannot queue same-frame modal text");
        int input_width{}, input_height{};
        require(SDL_GetWindowSizeInPixels(window.window(), &input_width, &input_height),
                "Cannot read modal input dimensions");
        const auto input_box = astra::ui::PickerLayout::measure(static_cast<float>(input_width),
                                                                static_cast<float>(input_height),
                                                                window.layout().scale,
                                                                0)
                                   .input;
        const auto local_x = 40 * window.layout().scale;
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      input_box.x + 6 * window.layout().scale + local_x,
                      input_box.y + 10 * window.layout().scale);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_UP,
                      input_box.x + 6 * window.layout().scale + local_x,
                      input_box.y + 10 * window.layout().scale);
        unicode_input.text.text = "中";
        require(SDL_PushEvent(&unicode_input), "Cannot queue text after modal pointer");
        settle(window);
        astra::ui::TextLayoutEngine input_fonts;
        astra::ui::TextLayoutOptions input_style;
        input_style.font_size = 14;
        input_style.dpi_scale = window.layout().scale;
        input_style.line_height = 1;
        input_style.font_families = {"Segoe UI", "Microsoft YaHei UI", "Segoe UI Emoji"};
        input_style.direction = astra::ui::TextDirection::automatic;
        const std::string typed = "0123456789🙂";
        const auto paragraph = input_fonts.layout(typed, input_style);
        const auto offset = paragraph.hit_test(local_x, paragraph.height() / 2).byte_offset;
        const auto expected = typed.substr(0, offset) + "中" + typed.substr(offset);
        key(window, SDLK_RETURN);
        require(window.status_text() == "Input " + expected,
                "Same-frame modal pointer used stale text geometry");
        std::string long_input;
        for (int repeat = 0; repeat < 700; ++repeat) {
            long_input += "中🙂éאב\t";
        }
        const auto workers_before =
            window.view_state().input_layout.at("workerSubmitted").get<std::size_t>();
        window.execute_ui_command("acceptance.longInput");
        await_input(window, workers_before);
        require(window.view_state().input_layout.value("pending", false) &&
                    window.view_state().input_layout.at("workerSubmitted").get<std::size_t>() >
                        workers_before,
                "Cold long input did not submit background shaping: " +
                    window.view_state().input_layout.dump() + " error=" + window.last_error() +
                    " status=" + window.status_text());
        const auto long_box = astra::ui::PickerLayout::measure(static_cast<float>(input_width),
                                                               static_cast<float>(input_height),
                                                               window.layout().scale,
                                                               0)
                                  .input;
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      long_box.x + 6 * window.layout().scale + local_x,
                      long_box.y + 10 * window.layout().scale);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_UP,
                      long_box.x + 6 * window.layout().scale + local_x,
                      long_box.y + 10 * window.layout().scale);
        unicode_input.text.text = "X";
        require(SDL_PushEvent(&unicode_input), "Cannot queue text behind cold long input pointer");
        SDL_Event accept_input{};
        accept_input.type = SDL_EVENT_KEY_DOWN;
        accept_input.key.windowID = SDL_GetWindowID(window.window());
        accept_input.key.key = SDLK_RETURN;
        require(SDL_PushEvent(&accept_input), "Cannot queue long input acceptance");
        settle(window);
        const auto long_paragraph = input_fonts.layout(long_input, input_style);
        const auto long_offset =
            long_paragraph.hit_test(local_x, long_paragraph.height() / 2).byte_offset;
        const auto long_expected =
            long_input.substr(0, long_offset) + "X" + long_input.substr(long_offset);
        const auto expected_paragraph = input_fonts.layout(long_expected, input_style);
        const auto prefix_bytes = expected_paragraph.byte_offset_from_utf16(20);
        require(window.status_text() ==
                    "Long input " + long_expected.substr(0, prefix_bytes) + " length 5601",
                "Cold long input pointer/text/acceptance reordered or split Unicode");
        const auto cancellation_workers =
            window.view_state().input_layout.at("workerSubmitted").get<std::size_t>();
        window.execute_ui_command("acceptance.longInput");
        await_input(window, cancellation_workers);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      long_box.x + 6 * window.layout().scale + local_x,
                      long_box.y + 10 * window.layout().scale);
        require(SDL_PushEvent(&unicode_input), "Cannot queue cancelled input text");
        key(window, SDLK_ESCAPE);
        settle(window);
        require(window.status_text() == "Long input cancelled" &&
                    window.view_state().input_layout.at("queuedEvents") == 0 &&
                    !window.view_state().input_layout.at("pointerPending").get<bool>(),
                "Escape did not cancel the deferred input sequence");
        window.execute_ui_command("acceptance.inputEditing");
        settle(window);
        key(window, SDLK_RETURN);
        require(window.status_text() == "Input A🙂éB",
                "Cancelled long input contaminated the next dialog");
        window.execute_ui_command("acceptance.longForm");
        for (int frame = 0; frame < 200 && (!window.view_state().inline_form.is_object() ||
                                            window.view_state().inline_form.value(
                                                "id", std::string{}) != "acceptance-long-input");
             ++frame) {
            require(window.pump(), "Long form closed the editor");
        }
        const auto field_box = window.view_state().inline_form_layout->fields.front();
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      field_box.x + 6 * window.layout().scale + local_x,
                      field_box.y + 10 * window.layout().scale);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_UP,
                      field_box.x + 6 * window.layout().scale + local_x,
                      field_box.y + 10 * window.layout().scale);
        require(SDL_PushEvent(&unicode_input), "Cannot queue cold form input");
        settle(window);
        auto form_style = input_style;
        form_style.font_size = 13;
        const auto form_paragraph = input_fonts.layout(long_input, form_style);
        const auto form_offset =
            form_paragraph.hit_test(local_x, form_paragraph.height() / 2).byte_offset;
        const auto form_expected =
            long_input.substr(0, form_offset) + "X" + long_input.substr(form_offset);
        require(window.view_state().inline_form.at("values").at("value") == form_expected,
                "Cold inline form pointer/text used stale or synchronous geometry");
        window.request_screenshot("build/graphics/gui-editable-long-input.png");
        settle(window);
        key(window, SDLK_RETURN);
        require(window.status_text() == "Long form accepted", "Long form action did not settle");
        window.execute_ui_command("acceptance.longForm");
        for (int frame = 0; frame < 200 && (!window.view_state().inline_form.is_object() ||
                                            window.view_state().inline_form.value(
                                                "id", std::string{}) != "acceptance-long-input");
             ++frame) {
            require(window.pump(), "Programmatic form change closed the editor");
        }
        const auto race_field = window.view_state().inline_form_layout->fields.front();
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      race_field.x + 6 * window.layout().scale + local_x,
                      race_field.y + 10 * window.layout().scale);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_UP,
                      race_field.x + 6 * window.layout().scale + local_x,
                      race_field.y + 10 * window.layout().scale);
        // Keep the sequence pending even if the worker completes in this frame.
        // The production drain accepts at most 16 events per frame.
        for (int step = 0; step < 128; ++step) {
            enqueue_key(window, SDLK_LEFT);
        }
        require(SDL_PushEvent(&unicode_input), "Cannot queue racing form input");
        require(window.pump() && window.view_state().input_layout.at("queuedEvents") > 0,
                "Programmatic race did not retain a captured input sequence");
        window.execute_ui_command("acceptance.overwriteLongForm");
        const auto race_deadline = Clock::now() + std::chrono::seconds(20);
        do {
            require(window.pump() && Clock::now() < race_deadline,
                    "Programmatic input change did not settle");
            SDL_Delay(1);
        } while (!window.idle());
        const auto raced_value = window.view_state().inline_form.at("values").at("value");
        const auto race_result = window.view_state().inline_form.at("message").get<std::string>();
        if (race_result == "Overwrite accepted") {
            require(raced_value == "changed",
                    "Obsolete pointer/text overwrote a programmatic value");
            require(window.last_error().empty() ||
                        window.last_error().find("Input target changed") != std::string::npos ||
                        window.last_error().find("Input content or style changed") !=
                            std::string::npos,
                    "Programmatic input race hid an unrelated failure");
        } else {
            require(race_result == "Overwrite stale" && raced_value == "X" + long_input &&
                        window.last_error().empty(),
                    "Stale programmatic input changed the user edit: status=" +
                        window.status_text() + " error=" + window.last_error() +
                        " value=" + raced_value.get<std::string>().substr(0, 64) +
                        " bytes=" + std::to_string(raced_value.get<std::string>().size()));
        }
        std::cout << "GUI programmatic input race: " << race_result << "\n";
        window.execute_ui_command("ui.cancel");
        settle(window);
        window.execute_ui_command("acceptance.longForm");
        for (int frame = 0; frame < 200 && window.view_state().inline_form.is_null(); ++frame) {
            require(window.pump(), "Cold form cancellation closed the editor");
        }
        const auto close_form = window.view_state().inline_form_layout->close;
        const auto cancel_field = window.view_state().inline_form_layout->fields.front();
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      cancel_field.x + 6 * window.layout().scale + local_x,
                      cancel_field.y + 10 * window.layout().scale);
        require(SDL_PushEvent(&unicode_input), "Cannot queue cancelled form input");
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      close_form.x + close_form.width / 2,
                      close_form.y + close_form.height / 2);
        settle(window);
        require(window.view_state().inline_form.is_null() &&
                    window.view_state().input_layout.at("queuedEvents") == 0,
                "Close button did not cancel the deferred form sequence");
        window.execute_ui_command("acceptance.longForm");
        for (int frame = 0; frame < 200 && window.view_state().inline_form.is_null(); ++frame) {
            require(window.pump(), "JS form cancellation closed the editor");
        }
        const auto js_close_field = window.view_state().inline_form_layout->fields.front();
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      js_close_field.x + 6 * window.layout().scale + local_x,
                      js_close_field.y + 10 * window.layout().scale);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_UP,
                      js_close_field.x + 6 * window.layout().scale + local_x,
                      js_close_field.y + 10 * window.layout().scale);
        for (int step = 0; step < 128; ++step) {
            enqueue_key(window, SDLK_LEFT);
        }
        require(SDL_PushEvent(&unicode_input), "Cannot queue JS-cancelled form input");
        const auto js_close_deadline = Clock::now() + std::chrono::seconds(20);
        do {
            require(window.pump() && Clock::now() < js_close_deadline,
                    "JS-close pointer placement did not complete");
            SDL_Delay(1);
        } while (window.view_state().input_layout.at("pointerPending").get<bool>());
        require(window.view_state().input_layout.at("queuedEvents") > 0,
                "JS-close fixture did not retain post-pointer input");
        window.execute_ui_command("acceptance.closeLongForm");
        settle(window);
        require(window.view_state().inline_form.is_null() &&
                    window.view_state().input_layout.at("queuedEvents") == 0,
                "JS form close retained its completed pointer's input sequence");
        window.execute_ui_command("acceptance.text");
        settle(window);
        for (int row = 0; row < 30; ++row)
            key(window, SDLK_DOWN);
        window.request_screenshot("build/graphics/gui-workbench-text.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        require(window.status_text() == "Information closed",
                "Text panel dismissal did not settle its awaited JS workflow");
        window.execute_ui_command("acceptance.captureBody");
        settle(window);
        window.execute_ui_command("acceptance.textAction");
        settle(window);
        SDL_Event shortcut{};
        shortcut.type = SDL_EVENT_KEY_DOWN;
        shortcut.key.windowID = SDL_GetWindowID(window.window());
        shortcut.key.key = SDLK_I;
        SDL_Event shortcut_text{};
        shortcut_text.type = SDL_EVENT_TEXT_INPUT;
        shortcut_text.text.windowID = SDL_GetWindowID(window.window());
        shortcut_text.text.text = "i";
        require(SDL_PushEvent(&shortcut) && SDL_PushEvent(&shortcut_text),
                "Cannot enqueue modal shortcut and its generated text");
        settle(window);
        require(window.status_text() == "Text action inspect",
                "Text shortcut lost the action identity");
        shortcut.key.repeat = true;
        SDL_Event release = shortcut;
        release.type = SDL_EVENT_KEY_UP;
        release.key.repeat = false;
        require(SDL_PushEvent(&shortcut) && SDL_PushEvent(&shortcut_text) &&
                    SDL_PushEvent(&release),
                "Cannot enqueue modal shortcut auto-repeat and release");
        settle(window);
        window.execute_ui_command("acceptance.verifyBody");
        settle(window);
        require(window.status_text() == "Sidebar text isolated",
                "Modal shortcut text leaked into the document");
        window.execute_ui_command("acceptance.textAction");
        settle(window);
        int text_width{}, text_height{};
        require(SDL_GetWindowSizeInPixels(window.window(), &text_width, &text_height),
                "Cannot read text viewport");
        const auto text_panel = astra::ui::TextPanelLayout::measure(static_cast<float>(text_width),
                                                                    static_cast<float>(text_height),
                                                                    window.layout().scale,
                                                                    4,
                                                                    3);
        window.request_screenshot("build/graphics/gui-text-actions.png");
        settle(window);
        click(window,
              text_panel.actions[1].x + text_panel.actions[1].width / 2,
              text_panel.actions[1].y + text_panel.actions[1].height / 2);
        require(window.status_text() == "Text action apply",
                "Text button lost the action identity");
        window.execute_ui_command("acceptance.textAction");
        settle(window);
        key(window, SDLK_ESCAPE);
        require(window.status_text() == "Text action cancelled",
                "Text cancellation returned an action");
        window.execute_ui_command("acceptance.longText");
        await_surface(window);
        require(window.view_state().text_surface.at("rows") == 81,
                "Long text surface lost its indexed rows");
        enqueue_key(window, SDLK_ESCAPE);
        require(window.pump(), "Window closed during cold text cancellation");
        window.execute_ui_command("acceptance.choice");
        settle(window);
        key(window, SDLK_RETURN);
        require(window.status_text() == "Action 3", "Late cancelled layout replaced another modal");
        window.execute_ui_command("acceptance.longText");
        await_surface(window);
        for (int row = 0; row < 40; ++row) {
            enqueue_key(window, SDLK_DOWN);
        }
        settle(window);
        const auto paged_text = window.view_state().text_surface;
        require(paged_text.at("first") == 40 &&
                    paged_text.at("end").get<std::size_t>() - 40 <= 256 &&
                    paged_text.at("cached").get<std::size_t>() <= 256,
                "Long text scrolling lost visible-row or cache bounds");
        key(window, SDLK_ESCAPE);
        for (const auto* command : {"acceptance.longChoice", "acceptance.longReview"}) {
            window.execute_ui_command(command);
            await_surface(window);
            key(window, SDLK_ESCAPE);
        }
        require(window.status_text() == "Long review cancelled",
                "Cold wrapped review did not cancel");
        window.execute_ui_command("acceptance.choice");
        settle(window);
        key(window, SDLK_RETURN);
        require(window.status_text() == "Action 3",
                "Choice dialog lost its configured default identity");
        require(SDL_SetWindowSize(window.window(),
                                  static_cast<int>(360 * state.layout.scale),
                                  static_cast<int>(600 * state.layout.scale)),
                "Cannot resize for vertical choices");
        settle(window);
        window.execute_ui_command("acceptance.choice");
        settle(window);
        int choice_width{}, choice_height{};
        require(SDL_GetWindowSizeInPixels(window.window(), &choice_width, &choice_height),
                "Cannot read choice viewport");
        const auto choices = astra::ui::ChoiceLayout::measure(static_cast<float>(choice_width),
                                                              static_cast<float>(choice_height),
                                                              window.layout().scale,
                                                              4);
        require(choices.buttons[1].y > choices.buttons[0].y,
                "Narrow choice dialog did not use vertical buttons");
        window.request_screenshot("build/graphics/gui-choice-narrow.png");
        settle(window);
        click(window,
              choices.buttons[1].x + choices.buttons[1].width / 2,
              choices.buttons[1].y + choices.buttons[1].height / 2);
        require(window.status_text() == "Action 2",
                "Choice pointer did not return the original identity");
        window.execute_ui_command("acceptance.choice");
        settle(window);
        key(window, SDLK_D);
        require(window.status_text() == "Action 4",
                "Choice shortcut did not return the configured identity");
        window.execute_ui_command("acceptance.choice");
        settle(window);
        key(window, SDLK_ESCAPE);
        require(window.status_text() == "Action cancelled",
                "Choice cancellation did not settle the JS workflow");
        require(SDL_SetWindowSize(window.window(),
                                  static_cast<int>(1440 * state.layout.scale),
                                  static_cast<int>(900 * state.layout.scale)),
                "Cannot restore from vertical choices");
        settle(window);
        window.execute_ui_command("acceptance.view");
        settle(window);
        auto custom = window.view_state();
        require(custom.sidebar.at("id") == "acceptance-view" && custom.sidebar.at("total") == 602,
                "Registered custom view was not presented");
        click(window,
              custom.layout.sidebar_rows.x + 45 * custom.layout.scale,
              custom.layout.sidebar_rows.y + custom.layout.sidebar_row_height / 2);
        require(window.view_state().sidebar.at("total") == 2,
                "Custom view group did not collapse through the generic index");
        click(window,
              custom.layout.sidebar_rows.x + 45 * custom.layout.scale,
              custom.layout.sidebar_rows.y + custom.layout.sidebar_row_height / 2);
        custom = window.view_state();
        require(custom.sidebar.at("total") == 602, "Custom group did not expand");
        click(window,
              custom.layout.sidebar_rows.x + 65 * custom.layout.scale,
              custom.layout.sidebar_rows.y + custom.layout.sidebar_row_height * 1.5F);
        require(window.status_text() == "Custom row 0 中🙂",
                "Native custom row did not invoke JavaScript");
        require(window.view_state().sidebar.at("focused") == true &&
                    window.view_state().sidebar.at("row").at("id") == "0",
                "Pointer selection did not focus the generic row widget");
        for (int i = 0; i < 10; ++i)
            enqueue_key(window, SDLK_DOWN);
        settle(window);
        require(window.view_state().sidebar.at("row").at("id") == "10",
                "Async custom navigation did not preserve rapid input order");
        window.execute_ui_command("acceptance.waitInView");
        settle(window);
        enqueue_key(window, SDLK_RETURN);
        enqueue_key(window, SDLK_DOWN);
        settle(window);
        require(window.status_text() == "Custom row 10 中🙂" &&
                    window.view_state().sidebar.at("row").at("id") == "11",
                "A view handler's idle barrier blocked its queued successor");
        key(window, SDLK_END);
        const auto end_state = window.view_state();
        const auto row_page =
            static_cast<std::size_t>(std::max(1.0F,
                                              std::floor(end_state.layout.sidebar_rows.height /
                                                         end_state.layout.sidebar_row_height)));
        require(window.view_state().sidebar.at("row").at("id") == "tail" &&
                    end_state.file_first <= 601 && 601 - end_state.file_first < row_page,
                "End did not reveal the last indexed row: " + end_state.sidebar.dump());
        key(window, SDLK_PAGEUP);
        const auto paged = window.view_state().sidebar.at("index").get<std::size_t>();
        require(paged == 601 - std::min(std::size_t{601}, row_page),
                "PageUp did not use the visible row viewport");
        key(window, SDLK_HOME);
        key(window, SDLK_LEFT);
        require(window.view_state().sidebar.at("total") == 2,
                "Left did not collapse the current group");
        key(window, SDLK_RIGHT);
        key(window, SDLK_RIGHT);
        require(window.view_state().sidebar.at("row").at("id") == "0",
                "Right did not expand the group and then enter its first child");
        key(window, SDLK_LEFT);
        require(window.view_state().sidebar.at("row").at("id") == "group",
                "Left did not seek the current row's parent");
        key(window, SDLK_RIGHT);
        const auto before_refresh = window.view_state().sidebar.at("version");
        window.execute_ui_command("acceptance.pauseRows");
        settle(window);
        window.execute_ui_command("acceptance.refreshView");
        const auto pump_stream_until = [&](auto predicate) {
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            while (!predicate()) {
                require(window.pump(), "Window closed during streaming view input");
                require(window.last_error().empty(), window.last_error());
                require(Clock::now() < deadline, "Streaming view did not accept input");
                SDL_Delay(1);
            }
        };
        pump_stream_until([&] {
            return window.status_text() == "Stream waiting";
        });
        require(window.view_state().sidebar.at("version") == before_refresh &&
                    window.view_state().sidebar.at("total") == 602,
                "A partial stream replaced the still interactive published view");
        enqueue_key(window, SDLK_DOWN);
        pump_stream_until([&] {
            return window.view_state().sidebar.at("row").at("id") == "1";
        });
        require(window.view_state().sidebar.at("version") == before_refresh,
                "Streaming publication did not preserve the old view while navigating");
        window.execute_ui_command("acceptance.releaseRows");
        settle(window);
        require(window.view_state().sidebar.at("row").at("id") == "1",
                "Refresh lost the stable row cursor");
        key(window, SDLK_HOME);
        key(window, SDLK_RIGHT);
        window.execute_ui_command("acceptance.holdNext");
        settle(window);
        window.execute_ui_command("acceptance.captureFocus");
        settle(window);
        enqueue_key(window, SDLK_DOWN);
        require(window.pump(), "Cannot start delayed view input");
        window.execute_ui_command("ui.focusEditor");
        window.execute_ui_command("acceptance.releaseNext");
        settle(window);
        require(window.view_state().sidebar.at("focused") == false &&
                    window.view_state().sidebar.at("row").at("id") == "0",
                "Late navigation changed a view after focus moved to the editor");
        key(window, SDLK_F6);
        require(window.view_state().sidebar.at("focused") == true, "F6 did not focus the sidebar");
        window.execute_ui_command("acceptance.staleBlur");
        settle(window);
        require(window.view_state().sidebar.at("focused") == true,
                "A stale focus return stole the newly focused row widget");
        key(window, SDLK_TAB);
        require(window.view_state().sidebar.at("focused") == false,
                "Tab did not return editor focus");
        window.execute_ui_command("ui.toggleSidebar");
        settle(window);
        key(window, SDLK_F6);
        require(window.view_state().layout.sidebar.width > 0 &&
                    window.view_state().sidebar.at("focused") == true,
                "F6 did not reveal and focus a hidden sidebar");
        window.execute_ui_command("acceptance.focusView");
        settle(window);
        window.execute_ui_command("acceptance.captureBody");
        settle(window);
        // Text and editing shortcuts must not mutate the active document while
        // keyboard focus belongs to the retained row widget.
        SDL_Event sidebar_text{};
        sidebar_text.type = SDL_EVENT_TEXT_INPUT;
        sidebar_text.text.windowID = SDL_GetWindowID(window.window());
        sidebar_text.text.text = "unexpected";
        require(SDL_PushEvent(&sidebar_text), "Cannot enqueue focused row text input");
        enqueue_key(window, SDLK_BACKSPACE);
        enqueue_key(window, SDLK_A, SDL_KMOD_CTRL);
        enqueue_key(window, SDLK_X, SDL_KMOD_CTRL);
        settle(window);
        window.execute_ui_command("acceptance.verifyBody");
        settle(window);
        require(window.status_text() == "Sidebar text isolated",
                "Sidebar input changed editor text");
        key(window, SDLK_ESCAPE);
        const auto version = window.view_state().sidebar.at("version");
        window.execute_ui_command("acceptance.invalidView");
        settle(window);
        custom = window.view_state();
        require(custom.sidebar.at("version") == version && custom.sidebar.at("total") == 602,
                "Failed custom view publication replaced its valid snapshot");
        require(window.status_text() == "Invalid view retained old rows",
                "View failure did not settle JS");
        window.request_screenshot("build/graphics/gui-custom-view.png");
        settle(window);
        window.execute_ui_command("acceptance.largeView");
        settle(window);
        require(window.view_state().sidebar.at("total") == 20002,
                "Large async view did not publish every source row");
        window.execute_ui_command("acceptance.focusView");
        settle(window);
        key(window, SDLK_END);
        require(window.view_state().sidebar.at("row").at("id") == "tail",
                "Large view lost logarithmic last-row navigation");
        const auto before_retirement = window.view_state().view_retirement.at("completed");
        window.execute_ui_command("acceptance.refreshView");
        settle(window);
        require(window.view_state().sidebar.at("total") == 602,
                "Large source replacement did not retain the small source");
        require(window.view_state().view_retirement.at("completed") > before_retirement &&
                    window.view_state().view_retirement.at("queued") == 0,
                "Idle fence did not await actual large-source retirement");
        const auto small_version = window.view_state().sidebar.at("version");
        window.execute_ui_command("acceptance.largePending");
        const auto large_wait_deadline = Clock::now() + std::chrono::seconds(30);
        while (window.status_text() != "Large builder waiting") {
            require(window.pump(), "Window closed while a large builder streamed");
            require(window.last_error().empty(), window.last_error());
            require(Clock::now() < large_wait_deadline, "Large builder did not reach its pause");
            SDL_Delay(1);
        }
        require(window.view_state().sidebar.at("version") == small_version,
                "An unfinished large builder replaced the published source");
        enqueue_key(window, SDLK_HOME);
        window.execute_ui_command("acceptance.cancelLarge");
        settle(window);
        require(window.view_state().sidebar.at("version") == small_version &&
                    window.view_state().sidebar.at("total") == 602 &&
                    window.view_state().sidebar.at("row").at("id") == "group",
                "Large cancellation blocked input or changed the old snapshot");
        require(window.view_state().view_retirement.at("queued") == 0,
                "Cancelled large builder was still retained after idle");
        window.execute_ui_command("ui.explorer");
        settle(window);
        require(window.view_state().sidebar.at("focused") == true,
                "Explorer command did not focus its row widget");
        key(window, SDLK_END);
        require(window.view_state().sidebar.at("row").at("id") == "two_with_a_long_name.cpp",
                "Explorer keyboard did not select its final indexed path");
        key(window, SDLK_RETURN);
        require(window.active_document_id() == two &&
                    window.view_state().sidebar.at("focused") == false,
                "Explorer keyboard did not activate the file and return editor focus");
        window.execute_ui_command("acceptance.waiters");
        settle(window);
        require(window.status_text() == "Concurrent waits settled",
                "Concurrent idle/close requests deadlocked");
        window.execute_ui_command("acceptance.backgroundOpen");
        window.execute_ui_command("ui.closeWindow");
        const auto deadline = Clock::now() + std::chrono::seconds(20);
        while (window.pump()) {
            require(window.last_error().empty(),
                    "Workbench shutdown failed: " + window.last_error());
            require(Clock::now() < deadline, "Workbench shutdown timed out");
            SDL_Delay(1);
        }
    }
    host->shutdown();
}
} // namespace

int main() {
    fs::path workspace;
    try {
        const auto parent = fs::weakly_canonical(fs::absolute("build/graphics"));
        fs::create_directories(parent);
        workspace = parent /
                    ("gui-presentation-" + std::to_string(Clock::now().time_since_epoch().count()));
        require(fs::create_directory(workspace), "Cannot create presentation fixture");
        run(workspace);
        const auto issues =
            astra::test::cleanup_owned_directory(workspace, parent, "gui-presentation-");
        for (const auto& issue : issues)
            std::cerr << issue.path << ": " << issue.message << '\n';
        require(issues.empty(), "Presentation fixture cleanup failed");
        std::cout << "Windows native workbench: 1505 indexed files, folder collapse, splitter, "
                     "paged Outline scrolling/selection, shaped tabs, drag reorder, overflow, "
                     "resize, same-frame open/save focus, async picker/text/review cancellation, "
                     "custom JS views, guarded publication, and shutdown: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI presentation failed: " << error.what()
                  << "\nPreserved fixture: " << workspace << '\n';
        return 1;
    }
}
