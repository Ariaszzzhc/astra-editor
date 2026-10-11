#include <astra/io/file_store.hpp>
#include <astra/service/service_host.hpp>
#include <astra/ui/editor_window.hpp>
#include <astra/ui/icons.hpp>
#include <astra/ui/text_layout.hpp>
#include "support/temporary_directory.hpp"
#include "support/window_frame.hpp"

#include <SDL3/SDL.h>
#include "include/codec/SkCodec.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <cstdint>
#include <vector>
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

void verify_selected_row(Window& window, const fs::path& path, const char* role) {
    const auto state = window.view_state();
    const auto index = state.sidebar.at("index").get<std::size_t>();
    const auto first = state.sidebar.at("scroll").get<std::size_t>();
    require(index >= first, "Selected row is above the rendered viewport");
    const auto& layout = state.layout;
    const auto x = static_cast<int>(std::floor(layout.sidebar_rows.right() - 6 * layout.scale));
    const auto y = static_cast<int>(
        std::floor(layout.sidebar_rows.y +
                   (static_cast<float>(index - first) + 0.5F) * layout.sidebar_row_height));
    const auto expected =
        std::stoul(state.style.at("theme").at(role).get<std::string>().substr(1), nullptr, 16);
    window.request_screenshot(path);
    settle(window);
    auto data = SkData::MakeFromFileName(path.string().c_str());
    require(data != nullptr, "Selection screenshot is missing");
    auto codec = SkCodec::MakeFromData(std::move(data));
    require(codec != nullptr, "Selection screenshot is not a decodable PNG");
    const auto size = codec->dimensions();
    require(x >= 0 && x < size.width() && y >= 0 && y < size.height(),
            "Selection pixel is outside the screenshot");
    const auto info =
        SkImageInfo::Make(size.width(), size.height(), kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    std::vector<std::uint8_t> pixels(info.computeMinByteSize());
    require(codec->getPixels(info, pixels.data(), info.minRowBytes()) == SkCodec::kSuccess,
            "Selection screenshot decoding failed");
    const auto offset =
        static_cast<std::size_t>(y) * info.minRowBytes() + static_cast<std::size_t>(x) * 4;
    require(pixels.at(offset) == ((expected >> 16) & 255) &&
                pixels.at(offset + 1) == ((expected >> 8) & 255) &&
                pixels.at(offset + 2) == (expected & 255) && pixels.at(offset + 3) == 255,
            std::string("Selected row pixels do not follow theme role ") + role);
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

void enqueue_text(Window& window, const char* value) {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.windowID = SDL_GetWindowID(window.window());
    event.text.text = value;
    require(SDL_PushEvent(&event), "Cannot enqueue picker query input");
}

void await_surface(Window& window) {
    const auto until = Clock::now() + std::chrono::seconds(20);
    // A worker may finish during GPU presentation, before the test observes
    // pending. Completed native geometry is equally valid readiness evidence.
    while (!(window.view_state().text_surface.is_object() &&
             (window.view_state().text_surface.value("pending", false) ||
              window.view_state().text_surface.value("shaped", std::size_t{}) > 0))) {
        require(Clock::now() < until && window.pump(),
                "Long modal text never queued native shaping: " +
                    window.view_state().text_surface.dump() +
                    " input=" + window.view_state().input_layout.dump());
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
    constexpr int files = 5000;
    fs::create_directory(workspace / "generated");
    for (int i = 0; i < files; ++i) {
        std::ofstream output(workspace / "generated" / ("item" + std::to_string(i) + ".txt"));
        output << "indexed row\n";
    }
    for (const auto* name : {"one.cpp", "two_with_a_long_name.cpp", "three.cpp"}) {
        std::ofstream output(workspace / name);
        output << "// Astra workspace\nint answer(int input) { return input + 42; }\n";
        if (std::string_view(name) == "two_with_a_long_name.cpp") {
            output << R"cpp(

namespace astra {

struct EditorState {
    bool dirty{};
    unsigned line{};
    unsigned column{};
};

[[nodiscard]] const char* status(const EditorState& state) noexcept {
    return state.dirty ? "Unsaved changes" : "Ready 中🙂";
}

void move_to(EditorState& state, unsigned line, unsigned column) noexcept {
    state.line = line;
    state.column = column;
}

} // namespace astra
)cpp";
        }
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
    // The client's explicit default-value preference must win over this
    // retained service override when native settings arrive asynchronously.
    service.defaults_script += "\nastra.bind('Ctrl+Shift+Tab', 'ui.nextTab');\n";
    service.defaults_script += "astra.configure({workbench:{uiFontSize:11,sidebarWidth:270}});\n";
    service.resource_directory = fs::current_path();
    service.watch_files = false;
    auto host = std::make_shared<astra::service::ServiceHost>(std::move(service));
    {
        astra::ui::EditorWindowOptions options;
        options.workspace_name = "ASTRA WORKBENCH";
        options.resource_directory = fs::current_path();
        options.open_paths = {"one.cpp"};
        options.user_script = R"js(if(astra.context==='client') {
            astra.bind('Ctrl+Shift+Tab', 'ui.previousTab');
            astra.configure({workbench: {uiFontSize: 13}});
            let revealCapture;
            astra.registerCommand('acceptance.reveal.capture', async () => {
                revealCapture = {
                    state: await astra.ui.read({immediate: true}),
                    view: await astra.ui.views.read('explorer')
                };
            });
            astra.registerCommand('acceptance.reveal.fold', async () => {
                await astra.ui.views.select('explorer', 'generated');
                await astra.ui.views.navigate('explorer', 'left');
            });
            astra.registerCommand('acceptance.reveal.hide', () =>
                astra.ui.update({sidebarVisible: false}));
            astra.registerCommand('acceptance.reveal.disable', () =>
                astra.configure({explorer: {autoReveal: false}}));
            astra.registerCommand('acceptance.reveal.enable', () =>
                astra.configure({explorer: {autoReveal: true}}));
            astra.registerCommand('acceptance.reveal.rejectStale', async () => {
                const current = await astra.ui.views.read('explorer');
                const activity = revealCapture.state.activity;
                const row = revealCapture.state.document.path;
                if (await astra.ui.views.reveal('explorer', row, {
                    activity, version: current.version
                })) throw new Error('Obsolete document activity changed Explorer');
                if (await astra.ui.views.reveal('explorer', row, {
                    version: current.version + '-obsolete'
                })) throw new Error('Obsolete view version changed Explorer');
            });
            let originalStyle;
            let originalIcons;
            astra.registerCommand('acceptance.icons.update', async () => {
                originalIcons = await astra.ui.icons.read();
                await astra.ui.icons.update({extensions: {
                    '.cpp': {glyph: 'warning', tone: '#F3C96B'}
                }});
                const resolved = await astra.ui.icons.resolve([
                    {path: 'one.cpp', directory: false}
                ]);
                if (resolved.items[0].icon.glyph !== 'warning') {
                    throw new Error('Icon theme was not published');
                }
            });
            astra.registerCommand('acceptance.icons.restore', async () => {
                await astra.ui.icons.replace(originalIcons.theme);
            });
            astra.registerCommand('acceptance.style.large', async () => {
                originalStyle = await astra.ui.style.read();
                const updated = await astra.ui.style.update({
                    theme: {background: '#18212A', panel: '#273544', tabs: '#151B23'},
                    workbench: {
                        uiFontSize: 16,
                        uiFontFamilies: ['Consolas', 'Microsoft YaHei UI', 'Segoe UI Emoji']
                    }
                });
                if (updated.workbench.uiFontSize !== 16) throw Error('Style update did not settle');
                await astra.ui.update({status: 'Large style settled'});
            });
            astra.registerCommand('acceptance.style.small', async () => {
                await astra.ui.style.update({
                    ...originalStyle,
                    workbench: {...originalStyle.workbench, uiFontSize: 9}
                });
                await astra.ui.update({status: 'Small style settled'});
            });
            astra.registerCommand('acceptance.style.restore', async () => {
                await astra.ui.style.update(originalStyle);
                await astra.ui.update({status: 'Style restored'});
            });
            astra.registerCommand('acceptance.style.lightFrame', async () => {
                await astra.ui.style.update({theme: {
                    name: 'Acceptance Light', background: '#FAF7EF', foreground: '#18212A',
                    sidebar: '#F3EEE3', activityBar: '#ECE6D8', tabs: '#ECE6D8',
                    panel: '#F3EEE3', activeTab: '#FAF7EF', border: '#B6AB96',
                    selection: '#BED7ED', inactiveSelection: '#DFDDD5', accent: '#0067B8',
                    status: '#ECE6D8', muted: '#64615A', statusForeground: '#18212A',
                    hover: '#E2DED5', currentLine: '#F0EBDD', input: '#FFFFFF',
                    cursor: '#18212A', scrollbar: '#B6AB96', folder: '#8A611C',
                    codeIcon: '#0067B8', scriptIcon: '#876016', added: '#276C3E',
                    changed: '#0067B8', noticeBackground: '#FFF1CA',
                    recoveryBackground: '#E6F1DF', errorBackground: '#FCE6E1',
                    error: '#AD312B', warning: '#806000', keyword: '#803E8F',
                    string: '#9B3C24', comment: '#497333', number: '#4D6C37',
                    type: '#16736A', function: '#79571B', property: '#125D88',
                    variable: '#125D88', heading: '#0067B8', syntaxError: '#B02020'
                }});
                const state = await astra.ui.read({immediate:true});
                if (!Object.values(state.windowFrame).every(value => value === true))
                    throw Error('Actual window frame capabilities were not reported to JS');
                await astra.ui.update({status: 'Light frame settled'});
            });
            let previousOutline;
            let previousChrome;
            astra.registerCommand('acceptance.chrome.custom', async () => {
                previousChrome = astra.ui.registerChrome(async (context, options) => {
                    await astra.sleep(8, options);
                    const row = (id, label, icon) => ({id, label, icon, data: {action: id}});
                    return {
                        brand: 'JS Astra 中🙂', hint: 'JS controls',
                        command: [row('header', 'JS Workbench 中🙂', 'braces')],
                        activity: Array.from({length: 16}, (_, index) => row(`activity-${index}`, `View ${index}`, 'symbol')),
                        manage: [row('manage', 'Custom manage', 'settings')],
                        breadcrumb: [row('parent', '路径中🙂', 'folder'), row('leaf', 'native.js', 'document')],
                        statusLeft: [row('left', 'JS status 中🙂', 'branch')],
                        statusRight: [row('right', 'JS coordinate 中🙂', 'code')]
                    };
                }, {activate: async (data) => {
                    await astra.ui.update({status: 'Chrome ' + data.action});
                }});
                await astra.ui.chrome.refresh();
            });
            astra.registerCommand('acceptance.chrome.restore', async () => {
                astra.ui.registerChrome(previousChrome.provider, previousChrome.handlers,
                    {timeoutMs: previousChrome.timeoutMs});
                await astra.ui.chrome.refresh();
            });
            astra.registerCommand('acceptance.chrome.hang', async () => {
                astra.ui.registerChrome(() => new Promise(() => {}));
                await astra.ui.chrome.refresh();
            });
            astra.registerCommand('acceptance.outline.custom', async () => {
                previousOutline = astra.ui.registerOutline({loading: 'Preparing custom symbols 中🙂'},
                    async (viewport, options) => {
                        await astra.sleep(8, options);
                        return {offset: 0, total: 2, empty: 'Custom Outline empty', rows: [
                            {id: 'custom-outline-first', label: 'Custom symbol 中🙂', depth: 6,
                             icon: 'function', iconTone: 'added', data: {action: 'custom'}},
                            {id: 'custom-outline-second', label: 'Another custom symbol', depth: 2,
                             icon: 'symbol', iconTone: 'changed', data: {action: 'second'}}
                        ]};
                    }, {activate: async (data) => {
                        await astra.ui.update({status: 'Custom Outline ' + data.action});
                    }});
                await astra.ui.outline.refresh();
            });
            astra.registerCommand('acceptance.outline.restore', async () => {
                astra.ui.registerOutline(previousOutline.spec, previousOutline.provider,
                    previousOutline.handlers);
                await astra.ui.outline.refresh();
            });
            astra.registerCommand('acceptance.style.race', async () => {
                return await astra.ui.style.update({workbench: {uiFontSize: 16}});
            });
            astra.registerCommand('acceptance.style.cancelPending', async () => {
                const before = JSON.stringify(await astra.ui.style.read());
                const controller = new AbortController();
                const changing = astra.ui.style.update({workbench: {uiFontSize: 9}}, {
                    signal: controller.signal
                });
                // A separate owner-loop native read ensures the update has
                // reached the queue before aborting its pending future.
                await astra.ui.style.read();
                controller.abort();
                try {
                    await changing;
                    throw Error('Pending style update was accepted after cancellation');
                } catch (error) {
                    if (error.name !== 'AbortError') throw error;
                }
                if (JSON.stringify(await astra.ui.style.read()) !== before) {
                    throw Error('Cancelled pending update changed style');
                }
            });
            astra.registerCommand('acceptance.style.invalid', async () => {
                const before = JSON.stringify(await astra.ui.style.read());
                for (const patch of [
                    {theme: {background: '#FFFFFF', warning: '# 12345'}},
                    {theme: {foreground: null}},
                    {theme: {unknownColor: '#123456'}},
                    {workbench: {uiFontSize: 99}},
                    {workbench: {uiFontFamilies: []}},
                    {workbench: {uiFontSize: 9, uiMonospaceFontFamilies: ['Bad\nFont']}},
                    {workbench: {unknownSetting: 9}},
                    {editor: {fontSize: 9}}
                ]) {
                    let rejected = false;
                    try {
                        await astra.ui.style.update(patch);
                    } catch (error) {
                        rejected = true;
                    }
                    if (!rejected || JSON.stringify(await astra.ui.style.read()) !== before) {
                        throw Error('Invalid style patch changed the published style');
                    }
                }
                const controller = new AbortController();
                controller.abort();
                try {
                    await astra.ui.style.update({workbench: {uiFontSize: 9}}, {
                        signal: controller.signal
                    });
                    throw Error('Cancelled style patch was accepted');
                } catch (error) {
                    if (error.name !== 'AbortError') throw error;
                }
                if (JSON.stringify(await astra.ui.style.read()) !== before) {
                    throw Error('Cancelled style patch changed presentation');
                }
                await astra.ui.update({status: 'Invalid styles retained'});
            });
            astra.ui.registerCommand(
                { id: 'acceptance.ignorePage', title: 'Custom page binding', palette: false },
                async () => {
                    await astra.ui.update({ status: 'Custom page binding' });
                }
            );
            astra.registerCommand('acceptance.rebindPage', async () => {
                await astra.sleep(0);
                astra.bind('PageDown', 'acceptance.ignorePage');
            });
            astra.registerCommand('acceptance.restorePage', async () => {
                await astra.sleep(0);
                astra.bind('PageDown', 'ui.pageDown');
            });
            let stressTabNext = 0, stressTabStyle;
            let releasePicker;
            astra.registerCommand('acceptance.dynamicPicker', async () => {
                const result = await astra.ui.pick({title: 'ASYNC PICKER',
                    items: async (query, {signal}) => {
                        if (query === 'slow') await new Promise(resolve => releasePicker = resolve);
                        if (query === 'error') throw Error('Provider failed');
                        return [{label: query || 'old', value: query || 'old', iconPath: 'one.cpp'}];
                    }
                });
                await astra.ui.update({status: result ? `Picked ${result.value}` : 'Picker cancelled'});
            });
            astra.registerCommand('acceptance.releasePicker', () => releasePicker());
            astra.registerCommand('acceptance.tabs.open', async () => {
                const end = Math.min(stressTabNext + 64, 640);
                let document;
                for (; stressTabNext < end; ++stressTabNext) {
                    document = await astra.request('workspace.open', {
                        path: `generated/item${stressTabNext}.txt`
                    });
                    await astra.ui.update({document});
                }
                if (document) await astra.ui.update({activate: document.id});
            });
            astra.registerCommand('acceptance.tabs.font', async () => {
                stressTabStyle = await astra.ui.style.read();
                await astra.ui.style.update({workbench: {uiFontSize: 9}});
            });
            astra.registerCommand('acceptance.tabs.restore', async () => {
                await astra.ui.style.update(stressTabStyle);
            });
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
            astra.registerCommand('acceptance.revealBackground', async () => {
                const before = await astra.ui.read({immediate: true});
                const background = before.tabs.find(id => id !== before.document.id);
                if (!background || !before.viewport.firstLine) {
                    throw Error('Background reveal acceptance needs a scrolled document');
                }
                await astra.ui.update({revealCaretFor: background});
                const after = await astra.ui.read({immediate: true});
                if (after.document.id !== before.document.id ||
                    after.viewport.firstLine !== before.viewport.firstLine) {
                    throw Error('Background caret reveal changed the active viewport');
                }
                return await astra.request('document.get', {id: background});
            });
            astra.registerCommand('acceptance.verifyBody',async()=>{
                const document=await astra.request('document.get',{id:capturedBody.id});
                if(document.revision!==capturedBody.revision ||
                    JSON.stringify(document.selections)!==JSON.stringify(capturedBody.selections))
                    throw Error('Sidebar input mutated document state');
                await astra.ui.update({status:'Sidebar text isolated'});
            });
            astra.ui.registerView({id:'acceptance-view',title:'Custom 中🙂 '+ 'wide heading '.repeat(6),
                icon:'symbol',order:3,outline:true,outlineTitle:'Symbols 中🙂'},
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
        require(original.style.at("workbench").at("uiFontSize") == 13 &&
                    original.style.at("workbench").at("sidebarWidth") == 270,
                "Service style preferences or explicit same-value client preference were lost");
#if defined(_WIN32)
        const auto verify_frame = [&] {
            const auto state = window.view_state();
            const auto color = [&](const char* role) {
                return 0xFF000000U |
                       static_cast<std::uint32_t>(
                           std::stoul(state.style.at("theme").at(role).get<std::string>().substr(1),
                                      nullptr,
                                      16));
            };
            require(state.window_frame.at("darkMode") && state.window_frame.at("captionColor") &&
                        state.window_frame.at("textColor") && state.window_frame.at("borderColor"),
                    "Native frame capabilities were not exposed in Agent state");
            astra::test::verify_window_frame(
                window.window(), {color("activityBar"), color("foreground"), color("border")});
        };
        verify_frame();
#endif
        window.execute_ui_command("acceptance.icons.update");
        settle(window);
        const auto custom_icons = window.view_state();
        const auto classified = custom_icons.file_icons.at("resolutions").get<std::size_t>() -
                                original.file_icons.at("resolutions").get<std::size_t>();
        require(custom_icons.file_icons.at("generation") != original.file_icons.at("generation") &&
                    classified > 0 && classified < 128 &&
                    custom_icons.file_icons.at("pending") == 0 &&
                    custom_icons.sidebar.at("version") == original.sidebar.at("version") &&
                    custom_icons.document_viewport.at("shaped") ==
                        original.document_viewport.at("shaped"),
                "Icon theme update scanned the tree, rebuilt its source or reshaped the document");
        window.request_screenshot("build/graphics/gui-file-icons-custom.png");
        settle(window);
        for (int repaint = 0; repaint < 8; ++repaint) {
            require(window.pump(), "Window closed during warm icon repaint");
        }
        require(window.view_state().file_icons.at("resolutions") ==
                    custom_icons.file_icons.at("resolutions"),
                "Warm icon repaint reran JS association policy");
        window.execute_ui_command("ui.explorer");
        settle(window);
        key(window, SDLK_END);
        const auto end_icons = window.view_state();
        bool found_cpp_icon{};
        for (const auto& item : end_icons.file_icons.at("visibleRows")) {
            if (item.at("path") == "one.cpp" || item.at("path") == "two_with_a_long_name.cpp") {
                require(item.at("glyph") == static_cast<int>(astra::ui::Icon::warning) &&
                            item.at("tone") == "#F3C96B",
                        "Newly visible Explorer icons did not follow the JS theme");
                found_cpp_icon = true;
            }
        }
        require(found_cpp_icon, "Explorer did not draw the expected C++ file icons");
        window.request_screenshot("build/graphics/gui-file-icons-explorer.png");
        settle(window);
        key(window, SDLK_HOME);
        key(window, SDLK_ESCAPE);
        window.execute_ui_command("acceptance.icons.restore");
        settle(window);
        window.execute_ui_command("acceptance.captureBody");
        settle(window);
        const auto check_styled_input = [&](const char* command, float size, const char* capture) {
            window.execute_ui_command(command);
            settle(window);
            require(window.view_state().style.at("workbench").at("uiFontSize") == size,
                    "Native style did not follow the awaited JS update");
            require(window.layout().sidebar_row_height >=
                        std::ceil(size * window.layout().scale * 1.15F + 8 * window.layout().scale),
                    "Configured UI text does not fit its sidebar row height");
            window.request_screenshot(fs::path("build/graphics") / (std::string(capture) + ".png"));
            settle(window);
            window.execute_ui_command("acceptance.inputEditing");
            settle(window);
            key(window, SDLK_A, SDL_KMOD_CTRL);
            const std::string typed = "0123456789🙂";
            SDL_Event text{};
            text.type = SDL_EVENT_TEXT_INPUT;
            text.text.windowID = SDL_GetWindowID(window.window());
            text.text.text = typed.c_str();
            require(SDL_PushEvent(&text), "Cannot queue styled native input");
            settle(window);
            int width{}, height{};
            require(SDL_GetWindowSizeInPixels(window.window(), &width, &height),
                    "Cannot read styled input window dimensions");
            const auto scale = window.layout().scale;
            const auto box = astra::ui::PickerLayout::measure(
                                 static_cast<float>(width), static_cast<float>(height), scale, 0)
                                 .input;
            const auto x = 40 * scale;
            click(window, box.x + 6 * scale + x, box.y + 10 * scale);
            text.text.text = "中";
            require(SDL_PushEvent(&text), "Cannot insert after styled pointer placement");
            settle(window);
            // Independent native font oracle: modal inputs have a 14-unit
            // reference relative to the configured 13-unit UI base.
            astra::ui::TextLayoutEngine fonts;
            astra::ui::TextLayoutOptions style;
            style.font_size = 14 * size / 13;
            style.dpi_scale = scale;
            style.line_height = 1;
            style.direction = astra::ui::TextDirection::automatic;
            style.font_families = window.view_state()
                                      .style.at("workbench")
                                      .at("uiFontFamilies")
                                      .get<std::vector<std::string>>();
            const auto paragraph = fonts.layout(typed, style);
            const auto offset = paragraph.hit_test(x, paragraph.height() / 2).byte_offset;
            window.request_screenshot(fs::path("build/graphics") /
                                      (std::string(capture) + "-input.png"));
            settle(window);
            key(window, SDLK_RETURN);
            require(window.status_text() ==
                        "Input " + typed.substr(0, offset) + "中" + typed.substr(offset),
                    "Styled input painting and native pointer geometry diverged");
        };
        check_styled_input("acceptance.style.large", 16, "gui-style-large");
        const auto large_style = window.view_state().style;
        window.execute_ui_command("acceptance.style.invalid");
        settle(window);
        require(window.status_text() == "Invalid styles retained" &&
                    window.view_state().style == large_style,
                "Invalid or cancelled style update changed the current appearance");
        window.execute_ui_command("acceptance.style.cancelPending");
        settle(window);
        require(window.view_state().style == large_style &&
                    window.view_state().input_layout.at("pendingStyles") == 0,
                "Cancelled native style queue was not drained");
        window.execute_ui_command("acceptance.choice");
        settle(window);
        window.request_screenshot("build/graphics/gui-style-large-choice.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        check_styled_input("acceptance.style.small", 9, "gui-style-small");
        window.execute_ui_command("acceptance.style.restore");
        settle(window);
        require(window.view_state().style == original.style,
                "Restoring the effective style lost service/client preferences");
#if defined(_WIN32)
        verify_frame();
        const auto frame_document = window.view_state().document;
        window.execute_ui_command("acceptance.style.lightFrame");
        settle(window);
        require(window.status_text() == "Light frame settled",
                "Awaited light frame did not settle");
        verify_frame();
        const auto light_frame = astra::test::read_window_frame(window.window());
        astra::test::capture_window_frame(window.window(),
                                          "build/graphics/gui-native-frame-light.png");
        window.execute_ui_command("acceptance.style.invalid");
        settle(window);
        window.execute_ui_command("acceptance.style.cancelPending");
        settle(window);
        require(astra::test::read_window_frame(window.window()) == light_frame &&
                    window.view_state().document == frame_document,
                "Rejected/cancelled JS frame update changed OS attributes or document state");
        window.execute_ui_command("acceptance.style.restore");
        settle(window);
        verify_frame();
        astra::test::capture_window_frame(window.window(),
                                          "build/graphics/gui-native-frame-dark.png");
        std::cout
            << "GUI JS window frame: actual DWM dark/light theme, Agent/async read capabilities, "
               "invalid/cancel preservation and original document identity: PASS\n";
#endif
        window.execute_ui_command("acceptance.verifyBody");
        settle(window);
        click(window, 18 * original.layout.scale, original.layout.header.height / 2);
        require(!window.view_state().input_layout.contains("bytes") &&
                    window.active_document_id() == one,
                "Decorative header text opened a picker or changed the document");
        const auto command_center = window.layout().command_center;
        click(window,
              command_center.x + command_center.width / 2,
              command_center.y + command_center.height / 2);
        require(window.view_state().input_layout.contains("bytes"),
                "Painted command center did not open the JS command palette");
        window.request_screenshot("build/graphics/gui-chrome-command-center.png");
        settle(window);
        key(window, SDLK_ESCAPE);
        const auto shaped_before_repaint =
            window.view_state().text_labels.at("shaped").get<std::size_t>();
        for (int repaint = 0; repaint < 12; ++repaint) {
            require(window.pump(), "Chrome cache repaint closed the editor");
        }
        require(window.view_state().text_labels.at("shaped") == shaped_before_repaint &&
                    window.view_state().text_labels.at("entries") <= 512 &&
                    window.view_state().text_labels.at("bytes") <= 2 * 1024 * 1024,
                "Stable chrome repaint reshaped text or exceeded the LRU bounds");
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
        window.execute_ui_command("acceptance.reveal.capture");
        settle(window);
        window.execute_ui_command("acceptance.reveal.fold");
        settle(window);
        require(window.view_state().visible_file_rows == 5,
                "Reveal fixture did not hide the generated subtree");
        window.execute_ui_command("acceptance.reveal.hide");
        settle(window);
        window.open_path("generated/item0.txt");
        settle(window);
        const auto generated = window.active_document_id();
        require(generated != one && window.view_state().visible_file_rows == files + 5 &&
                    window.view_state().sidebar.at("row").at("id") == "generated/item0.txt" &&
                    window.view_state().sidebar.at("focused") == false &&
                    window.layout().sidebar.width == 0,
                "Automatic reveal failed or changed sidebar visibility/focus");
        window.execute_ui_command("acceptance.reveal.fold");
        settle(window);
        window.execute_ui_command("ui.refreshTree");
        settle(window);
        require(window.view_state().visible_file_rows == files + 5 &&
                    window.view_state().sidebar.at("row").at("id") == "generated/item0.txt",
                "Refresh publication did not reveal the active path");
        window.open_path("one.cpp");
        settle(window);
        window.execute_ui_command("acceptance.reveal.rejectStale");
        settle(window);
        require(window.view_state().sidebar.at("row").at("id") == "one.cpp",
                "An obsolete A-to-B-to-A continuation changed the current cursor");
        window.execute_ui_command("acceptance.reveal.fold");
        settle(window);
        window.execute_ui_command("acceptance.reveal.disable");
        settle(window);
        window.open_path("generated/item0.txt");
        settle(window);
        require(window.view_state().visible_file_rows == 5 &&
                    window.view_state().sidebar.at("row").at("id") == "generated",
                "Disabled automatic reveal changed the retained fold/cursor");
        window.execute_ui_command("ui.revealActiveFile");
        settle(window);
        require(window.view_state().visible_file_rows == files + 5 &&
                    window.view_state().sidebar.at("row").at("id") == "generated/item0.txt" &&
                    window.view_state().sidebar.at("focused") == true &&
                    window.layout().sidebar.width > 0,
                "Explicit reveal did not override automatic policy and focus Explorer");
        verify_selected_row(window, "build/graphics/gui-explorer-focused-reveal.png", "selection");
        key(window, SDLK_ESCAPE);
        window.execute_ui_command("acceptance.reveal.enable");
        settle(window);
        window.execute_ui_command("ui.closeTab");
        settle(window);
        require(window.active_document_id() == one &&
                    window.view_state().sidebar.at("row").at("id") == "one.cpp" &&
                    window.view_state().sidebar.at("focused") == false,
                "Closing the active file did not reveal the fallback document");
        verify_selected_row(
            window, "build/graphics/gui-explorer-auto-reveal.png", "inactiveSelection");
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
        const auto original_selections = window.view_state().document.at("selections");
        window.execute_ui_command("acceptance.outline.custom");
        settle(window);
        require(window.view_state().outline_total == 2 &&
                    window.view_state().visible_outline_rows == 2 &&
                    window.view_state().outline_first == 0 &&
                    window.view_state().outline.at("rows")[0].at("depth") == 6,
                "Custom asynchronous JS Outline did not replace the indexed page");
        window.request_screenshot("build/graphics/gui-custom-outline.png");
        settle(window);
        click(window,
              window.layout().outline_rows.x + 160 * original.layout.scale,
              window.layout().outline_rows.y + 12 * original.layout.scale);
        require(window.status_text() == "Custom Outline custom" &&
                    window.view_state().document.at("selections") == original_selections,
                "Custom Outline activation bypassed its JS handler or changed document selection");
        window.execute_ui_command("acceptance.outline.restore");
        settle(window);
        require(window.view_state().outline_total == 81,
                "Restoring the shipped JS Outline provider lost document symbols");
        std::cout << "GUI JS Outline: bounded indexed pages, independent scroll, real symbol "
                     "selection, async custom provider/actions and restoration: PASS\n";
        window.open_path("two_with_a_long_name.cpp");
        settle(window);
        const auto two = window.active_document_id();
        window.open_path("three.cpp");
        settle(window);
        const auto three = window.active_document_id();
        key(window, SDLK_TAB, static_cast<SDL_Keymod>(SDL_KMOD_CTRL | SDL_KMOD_SHIFT));
        require(window.active_document_id() == two,
                "Explicit client previous-tab binding lost to the service default");
        key(window, SDLK_TAB, SDL_KMOD_CTRL);
        require(window.active_document_id() == three, "Default JS next-tab binding failed");
        key(window, SDLK_PAGEUP, SDL_KMOD_CTRL);
        require(window.active_document_id() == two, "JS Ctrl+PageUp alias did not change tabs");
        key(window, SDLK_PAGEDOWN, SDL_KMOD_CTRL);
        require(window.active_document_id() == three, "JS Ctrl+PageDown alias did not change tabs");
        window.open_path("one.cpp");
        settle(window);
        key(window, SDLK_HOME, SDL_KMOD_CTRL);
        window.execute_ui_command("acceptance.captureBody");
        settle(window);
        key(window, SDLK_PAGEDOWN);
        require(window.view_state().document_viewport.at("firstLine").get<std::size_t>() > 0,
                "Default JS PageDown did not scroll the document viewport");
        const auto scrolled_line = window.view_state().document_viewport.at("firstLine");
        window.execute_ui_command("acceptance.revealBackground");
        settle(window);
        require(window.view_state().document_viewport.at("firstLine") == scrolled_line,
                "Background command result scrolled the active document after JS completion");
        key(window, SDLK_PAGEUP);
        require(window.view_state().document_viewport.at("firstLine") == 0,
                "Default JS PageUp did not restore the first page");
        window.execute_ui_command("acceptance.rebindPage");
        settle(window);
        key(window, SDLK_PAGEDOWN);
        require(window.status_text() == "Custom page binding" &&
                    window.view_state().document_viewport.at("firstLine") == 0,
                "Runtime JS rebinding was ignored or fell through to native page policy");
        window.execute_ui_command("acceptance.chrome.custom");
        settle(window);
        require(window.view_state().chrome.at("model").at("brand") == "JS Astra 中🙂",
                "Asynchronous JS chrome did not replace native captions");
        const auto chrome_selections = window.view_state().document.at("selections");
        const auto click_chrome = [&](std::string_view bank, std::string_view row) {
            const auto targets = window.view_state().chrome.at("targets");
            const auto found =
                std::find_if(targets.begin(), targets.end(), [&](const auto& target) {
                    return target.at("bank").template get_ref<const std::string&>() == bank &&
                           target.at("row").template get_ref<const std::string&>() == row;
                });
            require(found != targets.end(), "Custom chrome target is not visible");
            const auto& box = found->at("bounds");
            click(window,
                  box.at("x").get<float>() + box.at("width").get<float>() / 2,
                  box.at("y").get<float>() + box.at("height").get<float>() / 2);
            require(window.status_text() == "Chrome " + std::string(row),
                    "Chrome click bypassed its JS action");
            require(window.view_state().document.at("selections") == chrome_selections,
                    "Chrome pointer fell through to document selection");
        };
        click_chrome("command", "header");
        click_chrome("manage", "manage");
        click_chrome("breadcrumb", "parent");
        click_chrome("statusLeft", "left");
        click_chrome("statusRight", "right");
        const auto chrome_scale = window.layout().scale;
        require(SDL_SetWindowSize(window.window(),
                                  static_cast<int>(680 * chrome_scale),
                                  static_cast<int>(360 * chrome_scale)),
                "Cannot resize activity overflow fixture");
        settle(window);
        auto chrome_overflow = window.view_state().chrome;
        require(chrome_overflow.at("next").at("height").get<float>() > 0,
                "Activity overflow has no navigation controls");
        const auto& next_activity = chrome_overflow.at("next");
        click(window,
              next_activity.at("x").get<float>() + next_activity.at("width").get<float>() / 2,
              next_activity.at("y").get<float>() + next_activity.at("height").get<float>() / 2);
        require(window.view_state().chrome.at("activityFirst").get<std::size_t>() == 1,
                "Activity overflow control did not change the physical page");
        SDL_Event chrome_wheel{};
        chrome_wheel.type = SDL_EVENT_MOUSE_WHEEL;
        chrome_wheel.wheel.windowID = SDL_GetWindowID(window.window());
        chrome_wheel.wheel.mouse_x = 20;
        chrome_wheel.wheel.mouse_y = 100;
        chrome_wheel.wheel.y = -100;
        require(SDL_PushEvent(&chrome_wheel), "Cannot enqueue activity wheel input");
        settle(window);
        click_chrome("activity", "activity-15");
        window.request_screenshot("build/graphics/gui-outer-chrome-custom.png");
        settle(window);
        window.execute_ui_command("acceptance.chrome.restore");
        settle(window);
        require(window.view_state().chrome.at("model").at("brand") == "Astra",
                "Restoring default chrome lost its policy");
        require(SDL_SetWindowSize(window.window(),
                                  static_cast<int>(1440 * chrome_scale),
                                  static_cast<int>(900 * chrome_scale)),
                "Cannot restore chrome fixture size");
        settle(window);
        std::cout << "GUI JS outer chrome: async replacement, original Unicode, "
                     "header/manage/path/status actions, activity overflow, no selection "
                     "fallthrough and restoration: PASS\n";
        window.execute_ui_command("acceptance.restorePage");
        settle(window);
        key(window, SDLK_PAGEDOWN);
        require(window.view_state().document_viewport.at("firstLine").get<std::size_t>() > 0,
                "Runtime rebinding back to the JS default did not take effect");
        key(window, SDLK_PAGEUP);
        window.execute_ui_command("acceptance.verifyBody");
        settle(window);
        window.open_path("three.cpp");
        settle(window);
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
        require(window.layout().editor.width >= 360 * window.layout().scale,
                "Narrow layout lost the document region");
        for (const auto& tab : window.view_state().tab_strip.visible) {
            require(tab.bounds.width >= 96 * window.layout().scale &&
                        tab.close.x >= tab.bounds.x + 34 * window.layout().scale,
                    "Narrow tab painted overlapping icon/close targets");
        }
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
        require(window.view_state().input_layout.value("pickerQuery", std::string{}) ==
                        "three.cpp" &&
                    window.view_state().input_layout.value("pickerRows", 0) == 1 &&
                    window.active_document_id() == two,
                "Quick open lost same-batch typing or accepted before its async query completed");
        key(window, SDLK_RETURN);
        require(window.active_document_id() == three,
                "Quick open did not settle focus before same-frame typing");
        key(window, SDLK_P, SDL_KMOD_CTRL);
        enqueue_key(window, SDLK_A, SDL_KMOD_CTRL);
        enqueue_text(window, "itm4999");
        settle(window);
        require(window.view_state().input_layout.value("pickerRows", 0) == 1 &&
                    !window.view_state().input_layout.value("pickerPending", true),
                "Quick open failed to match a file beyond the static 4096-row capacity");
        window.request_screenshot("build/graphics/gui-quick-open.png");
        settle(window);
        key(window, SDLK_RETURN);
        auto selected_file = host->submit(nlohmann::json{
            {"jsonrpc", "2.0"},
            {"id", "quick-open-acceptance"},
            {"method", "workspace.documents"},
            {"params",
             nlohmann::json::object()}}.dump());
        require(selected_file.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "Quick-open identity verification timed out");
        const auto selected_response = selected_file.get();
        require(selected_response.has_value(), "Quick-open identity verification has no reply");
        const auto selected_documents = nlohmann::json::parse(*selected_response).at("result");
        const auto selected_document = std::find_if(
            selected_documents.begin(), selected_documents.end(), [&](const auto& document) {
                return document.at("id") == window.active_document_id();
            });
        require(selected_document != selected_documents.end() &&
                    fs::path(selected_document->at("path").get<std::string>()) ==
                        fs::path("generated/item4999.txt"),
                "Fuzzy quick open selected another workspace identity");
        key(window, SDLK_W, SDL_KMOD_CTRL);
        window.execute_ui_command("acceptance.dynamicPicker");
        settle(window);
        enqueue_key(window, SDLK_A, SDL_KMOD_CTRL);
        enqueue_text(window, "slow");
        const auto picker_deadline = Clock::now() + std::chrono::seconds(20);
        while (!window.view_state().input_layout.value("pickerPending", false)) {
            require(Clock::now() < picker_deadline && window.pump(),
                    "Async provider did not expose its pending query");
            require(window.last_error().empty(), window.last_error());
        }
        require(!window.idle(), "Pending dynamic provider was incorrectly idle");
        enqueue_key(window, SDLK_RETURN);
        require(window.pump(), "Pending picker closed the window");
        require(window.view_state().input_layout.value("pickerPending", false),
                "Enter accepted an obsolete provider result");
        int picker_width{}, picker_height{};
        require(SDL_GetWindowSizeInPixels(window.window(), &picker_width, &picker_height),
                "Cannot measure pending picker");
        const auto pending_picker =
            astra::ui::PickerLayout::measure(static_cast<float>(picker_width),
                                             static_cast<float>(picker_height),
                                             window.layout().scale,
                                             1);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      pending_picker.rows.x + 30,
                      pending_picker.rows.y + 10);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_UP,
                      pending_picker.rows.x + 30,
                      pending_picker.rows.y + 10);
        require(window.pump(), "Pending picker pointer closed the window");
        require(window.view_state().input_layout.value("pickerPending", false),
                "Pointer accepted an obsolete provider result");
        enqueue_key(window, SDLK_A, SDL_KMOD_CTRL);
        enqueue_text(window, "error");
        settle(window);
        require(window.view_state().input_layout.value("pickerError", false) &&
                    window.view_state().input_layout.value("pickerRows", 1) == 0,
                "Provider failure did not remain local to the picker");
        window.execute_ui_command("acceptance.releasePicker");
        settle(window);
        require(window.view_state().input_layout.value("pickerError", false),
                "Late provider result overwrote the current error");
        enqueue_key(window, SDLK_A, SDL_KMOD_CTRL);
        enqueue_text(window, "recovered");
        settle(window);
        require(!window.view_state().input_layout.value("pickerError", true),
                "Dynamic picker did not recover after a provider failure");
        key(window, SDLK_RETURN);
        require(window.status_text() == "Picked recovered",
                "Dynamic picker did not return the current original item");
        window.execute_ui_command("acceptance.dynamicPicker");
        settle(window);
        key(window, SDLK_ESCAPE);
        require(window.status_text() == "Picker cancelled",
                "Escape did not cancel dynamic selection");
        std::cout << "Async quick open: 5000-file fuzzy lookup, pending Enter/pointer guards, "
                     "provider failure/recovery, late results and Escape: PASS\n";
        window.open_path("three.cpp");
        settle(window);
        require(window.active_document_id() == three,
                "Quick-open fixture did not restore the captured save-as document");
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
        const auto composition_bytes = window.view_state().input_layout.at("bytes");
        SDL_Event composition_event{};
        composition_event.type = SDL_EVENT_TEXT_EDITING;
        composition_event.edit.windowID = SDL_GetWindowID(window.window());
        composition_event.edit.text = "预编辑🙂";
        require(SDL_PushEvent(&composition_event), "Cannot queue native IME composition");
        settle(window);
        require(window.view_state().input_layout.at("bytes") == composition_bytes,
                "IME preedit changed the committed input model");
        window.request_screenshot("build/graphics/gui-text-field-ime.png");
        settle(window);
        composition_event.edit.text = "";
        require(SDL_PushEvent(&composition_event), "Cannot retire native IME composition");
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
        window.execute_ui_command("acceptance.style.race");
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
        require(window.view_state().style.at("workbench").at("uiFontSize") == 16 &&
                    window.view_state().input_layout.at("pendingStyles") == 0,
                "Style update did not wait for accepted long-input pointer/text completion");
        window.execute_ui_command("acceptance.style.restore");
        settle(window);
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
        const std::string expected_caption = "Custom 中🙂 wide heading wide heading wide heading "
                                             "wide heading wide heading wide heading ";
        require(custom.sidebar.at("title") == expected_caption &&
                    custom.sidebar.at("outlineTitle") == "Symbols 中🙂",
                "JS chrome captions did not retain their original identity");
        const auto section = custom.layout.sidebar_section;
        click(window, section.x + section.width / 2, section.y + section.height / 2);
        require(window.view_state().sidebar.at("expanded") == false &&
                    window.view_state().sidebar.at("viewport").at("painted") == 0,
                "Shared section geometry did not collapse the indexed view");
        click(window, section.x + section.width / 2, section.y + section.height / 2);
        custom = window.view_state();
        require(custom.sidebar.at("expanded") == true && custom.sidebar.at("total") == 602,
                "Section re-expansion lost its source rows");
        window.request_screenshot("build/graphics/gui-long-sidebar-heading.png");
        settle(window);
        const auto& viewport = custom.sidebar.at("viewport");
        require(viewport.at("painted") == viewport.at("end").get<std::size_t>() -
                                              viewport.at("first").get<std::size_t>() &&
                    viewport.at("painted").get<std::size_t>() < 100,
                "Native row component scanned outside the custom view viewport");
        const auto partial = viewport.at("end").get<std::size_t>() - 1;
        const auto partial_top =
            custom.layout.sidebar_rows.y +
            static_cast<float>(partial - viewport.at("first").get<std::size_t>()) *
                custom.layout.sidebar_row_height;
        require(partial_top < custom.layout.sidebar_rows.bottom(),
                "Custom view did not retain its intersecting final row");
        click(window,
              custom.layout.sidebar_rows.x + 65 * custom.layout.scale,
              (partial_top + custom.layout.sidebar_rows.bottom()) / 2);
        require(window.view_state().sidebar.at("index") == partial &&
                    window.status_text() == "Custom row " + std::to_string(partial - 1) + " 中🙂",
                "Painted final row and native pointer action used different identities");
        key(window, SDLK_HOME);
        custom = window.view_state();
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
        const auto refresh_control = window.view_state().layout.sidebar_refresh;
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      refresh_control.x + refresh_control.width / 2,
                      refresh_control.y + refresh_control.height / 2);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_UP,
                      refresh_control.x + refresh_control.width / 2,
                      refresh_control.y + refresh_control.height / 2);
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
        window.execute_ui_command("acceptance.focusView");
        pump_stream_until([&] {
            return window.view_state().sidebar.at("focused") == true;
        });
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
        pump_stream_until([&] {
            return window.view_state().sidebar.at("row").at("id") == "group";
        });
        const auto cancel_control = window.view_state().layout.sidebar_refresh;
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_DOWN,
                      cancel_control.x + cancel_control.width / 2,
                      cancel_control.y + cancel_control.height / 2);
        enqueue_mouse(window,
                      SDL_EVENT_MOUSE_BUTTON_UP,
                      cancel_control.x + cancel_control.width / 2,
                      cancel_control.y + cancel_control.height / 2);
        settle(window);
        require(window.view_state().sidebar.at("version") == small_version &&
                    window.view_state().sidebar.at("total") == 602 &&
                    window.view_state().sidebar.at("row").at("id") == "group",
                "Large cancellation blocked input or changed the old snapshot");
        require(window.view_state().view_retirement.at("queued") == 0,
                "Cancelled large builder was still retained after idle");
        std::cout << "GUI workbench chrome: original JS Unicode captions, native collapse/expand, "
                     "refresh and busy cancel hit identity with retained indexed rows: PASS\n";
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
        const auto original_tab_count = window.view_state().tabs.size();
        for (unsigned chunk = 0; chunk < 10; ++chunk) {
            window.execute_ui_command("acceptance.tabs.open");
            settle(window);
        }
        const auto stress = window.view_state();
        require(stress.tabs.size() == original_tab_count + 640 &&
                    stress.tab_layout.at("pending") == 0 && !stress.tab_strip.visible.empty() &&
                    stress.tab_strip.visible.back().index == stress.tabs.size() - 1,
                "Measured tab index did not retain and reveal all real opened documents");
        const auto stress_shaped = stress.text_labels.at("shaped");
        for (unsigned repaint = 0; repaint < 8; ++repaint) {
            require(window.pump(), "Window closed while checking large tab repaint reuse");
        }
        require(window.view_state().tab_layout.at("measurements") ==
                        stress.tab_layout.at("measurements") &&
                    window.view_state().text_labels.at("shaped") == stress_shaped,
                "Warm large-tab repaint remeasured or reshaped offscreen captions");
        window.execute_ui_command("acceptance.tabs.font");
        require(window.pump(), "Window closed during large tab font invalidation");
        const auto first_font_pass = window.view_state();
        require(first_font_pass.tab_layout.at("pending").get<std::size_t>() > 0 &&
                    first_font_pass.tab_layout.at("generation") !=
                        stress.tab_layout.at("generation"),
                "Large font update did not defer bounded tab measurement");
        settle(window);
        const auto font_stress = window.view_state();
        require(font_stress.tab_layout.at("pending") == 0 &&
                    font_stress.tab_layout.at("maxMeasuredPass").get<std::size_t>() <= 8 &&
                    font_stress.tab_layout.at("measurements").get<std::size_t>() -
                            stress.tab_layout.at("measurements").get<std::size_t>() ==
                        stress.tabs.size() &&
                    !font_stress.tab_strip.visible.empty() &&
                    font_stress.tab_strip.visible.back().index == font_stress.tabs.size() - 1,
                "Large font update lost exact one-generation measurements or active visibility");
        window.execute_ui_command("acceptance.tabs.restore");
        settle(window);
        window.request_screenshot("build/graphics/gui-tabs-indexed.png");
        settle(window);
        const auto navigation = window.view_state();
        const auto navigation_active = window.active_document_id();
        require(navigation.tab_strip.first > 0 && navigation.tab_strip.previous.width > 0,
                "Tab navigation acceptance needs visible overflow controls");
        const auto previous = navigation.tab_strip.previous;
        click(window, previous.x + previous.width / 2, previous.y + previous.height / 2);
        require(window.view_state().tab_strip.first + 1 == navigation.tab_strip.first &&
                    window.active_document_id() == navigation_active,
                "Previous tab overflow changed activation or skipped an identity");
        const auto next = window.view_state().tab_strip.next;
        click(window, next.x + next.width / 2, next.y + next.height / 2);
        require(window.view_state().tab_strip.first == navigation.tab_strip.first &&
                    window.view_state().tab_layout.at("measurements") ==
                        navigation.tab_layout.at("measurements"),
                "Tab overflow did not restore the viewport or remeasured captions");
        const auto drag_state = window.view_state();
        require(drag_state.tab_strip.visible.size() >= 2,
                "Tab drag acceptance needs two complete caption targets");
        const auto from = drag_state.tab_strip.visible[0];
        const auto to = drag_state.tab_strip.visible[1];
        const auto dragged_id = drag_state.tabs.at(from.index);
        auto reordered = drag_state.tabs;
        reordered.erase(reordered.begin() + static_cast<std::ptrdiff_t>(from.index));
        reordered.insert(reordered.begin() + static_cast<std::ptrdiff_t>(to.index), dragged_id);
        mouse(window,
              SDL_EVENT_MOUSE_BUTTON_DOWN,
              from.bounds.x + 40 * window.layout().scale,
              from.bounds.y + from.bounds.height / 2);
        require(window.active_document_id() == dragged_id,
                "Native tab caption did not activate its document identity");
        mouse(window,
              SDL_EVENT_MOUSE_MOTION,
              to.bounds.x + 40 * window.layout().scale,
              to.bounds.y + to.bounds.height / 2);
        mouse(window,
              SDL_EVENT_MOUSE_BUTTON_UP,
              to.bounds.x + 40 * window.layout().scale,
              to.bounds.y + to.bounds.height / 2);
        require(window.view_state().tabs == reordered && window.active_document_id() == dragged_id,
                "Native tab drag changed identity or failed to reorder the measured sequence");
        window.request_screenshot("build/graphics/gui-tabs-component.png");
        settle(window);
        const auto close_state = window.view_state();
        const auto closing = std::find_if(close_state.tab_strip.visible.begin(),
                                          close_state.tab_strip.visible.end(),
                                          [&](const auto& box) {
                                              return close_state.tabs.at(box.index) == dragged_id;
                                          });
        require(closing != close_state.tab_strip.visible.end(),
                "Dragged active tab was not kept visible for native close");
        click(window,
              closing->close.x + closing->close.width / 2,
              closing->close.y + closing->close.height / 2);
        reordered.erase(std::find(reordered.begin(), reordered.end(), dragged_id));
        require(window.view_state().tabs == reordered && window.active_document_id() != dragged_id,
                "Native tab close targeted another identity after reordering");
        std::cout << "640 real file tabs: cached measured widths, bounded font refresh, "
                     "active reveal, warm repaint reuse, overflow, drag and close identity: PASS\n";
        window.execute_ui_command("acceptance.chrome.hang");
        for (int frame = 0; frame < 24; ++frame) {
            require(window.pump(), "Chrome hang fixture closed unexpectedly");
            SDL_Delay(1);
        }
        require(window.view_state().chrome.at("pending") == true,
                "Signal-ignoring chrome provider did not remain pending");
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
        std::cout << "Windows native workbench: 5005 indexed files, folder collapse, splitter, "
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
