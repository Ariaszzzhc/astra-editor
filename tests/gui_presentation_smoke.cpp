#include <astra/io/file_store.hpp>
#include <astra/service/service_host.hpp>
#include <astra/ui/editor_window.hpp>
#include "support/temporary_directory.hpp"

#include <SDL3/SDL.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
using Clock=std::chrono::steady_clock;
using Window=astra::ui::EditorWindow;
namespace fs=std::filesystem;
void require(bool value,const std::string& message) { if (!value) throw std::runtime_error(message); }
void settle(Window& window) {
    const auto until=Clock::now()+std::chrono::seconds(20);
    do {
        require(window.pump(),"Window closed during presentation acceptance");
        require(window.last_error().empty(),window.last_error());
        if (window.idle()) return;
        SDL_Delay(1);
    } while(Clock::now()<until);
    throw std::runtime_error("Presentation workbench did not settle");
}
void mouse(Window& window,Uint32 type,float x,float y) {
    int width{},height{},pixels_x{},pixels_y{};
    require(SDL_GetWindowSize(window.window(),&width,&height) && SDL_GetWindowSizeInPixels(window.window(),&pixels_x,&pixels_y),"Cannot read window dimensions");
    SDL_Event event{}; event.type=type;
    if(type==SDL_EVENT_MOUSE_MOTION) {
        event.motion.windowID=SDL_GetWindowID(window.window()); event.motion.state=SDL_BUTTON_LMASK;
        event.motion.x=x*static_cast<float>(width)/static_cast<float>(pixels_x);
        event.motion.y=y*static_cast<float>(height)/static_cast<float>(pixels_y);
    } else {
        event.button.windowID=SDL_GetWindowID(window.window()); event.button.button=SDL_BUTTON_LEFT;
        event.button.down=type==SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.x=x*static_cast<float>(width)/static_cast<float>(pixels_x);
        event.button.y=y*static_cast<float>(height)/static_cast<float>(pixels_y);
    }
    require(SDL_PushEvent(&event),"Cannot enqueue native pointer input"); settle(window);
}
void click(Window& window,float x,float y) { mouse(window,SDL_EVENT_MOUSE_BUTTON_DOWN,x,y);mouse(window,SDL_EVENT_MOUSE_BUTTON_UP,x,y); }
void key(Window& window,SDL_Keycode code,SDL_Keymod mods=SDL_KMOD_NONE) {
    SDL_Event event{};event.type=SDL_EVENT_KEY_DOWN;event.key.windowID=SDL_GetWindowID(window.window());event.key.key=code;event.key.mod=mods;event.key.down=true;
    require(SDL_PushEvent(&event),"Cannot enqueue native key input");settle(window);
}
void run(const fs::path& workspace) {
    constexpr int files=1500;
    fs::create_directory(workspace/"generated");
    for(int i=0;i<files;++i) { std::ofstream output(workspace/"generated"/("item"+std::to_string(i)+".txt"));output<<"indexed row\n"; }
    for(const auto* name:{"one.cpp","two_with_a_long_name.cpp","three.cpp"}) {
        std::ofstream output(workspace/name);output<<"// Astra workspace\nint answer(int input) { return input + 42; }\n";
    }
    {std::ofstream output(workspace/".clangd");output<<"CompileFlags:\n  Add: [-nostdinc, -nostdinc++]\n";}
    astra::service::ServiceHostOptions service;
    service.workspace=workspace;service.defaults_script=astra::io::read_file("data/defaults.js").text;
    service.resource_directory=fs::current_path();service.watch_files=false;
    auto host=std::make_shared<astra::service::ServiceHost>(std::move(service));
    {
        astra::ui::EditorWindowOptions options;
        options.workspace_name="ASTRA WORKBENCH";options.resource_directory=fs::current_path();options.open_paths={"one.cpp"};
        Window window(host,std::move(options));settle(window);
        const auto original=window.view_state();
        require(original.visible_file_rows==files+5,"Indexed file rows were lost");
        require(original.layout.outline_rows.height==0,"Outline should default to collapsed inside Explorer");
        const auto one=window.active_document_id();
        click(window,original.layout.sidebar_rows.x+50*original.layout.scale,original.layout.sidebar_rows.y+original.layout.sidebar_row_height/2);
        require(window.view_state().visible_file_rows==5,"Folder collapse did not remove its descendant range");
        click(window,original.layout.sidebar_rows.x+50*original.layout.scale,original.layout.sidebar_rows.y+original.layout.sidebar_row_height/2);
        require(window.view_state().visible_file_rows==original.visible_file_rows,"Folder expansion did not restore its indexed descendants");
        const auto handle=window.layout().sidebar_handle;
        mouse(window,SDL_EVENT_MOUSE_BUTTON_DOWN,handle.x+handle.width/2,handle.y+80*original.layout.scale);
        mouse(window,SDL_EVENT_MOUSE_MOTION,handle.x+handle.width/2+90*original.layout.scale,handle.y+80*original.layout.scale);
        mouse(window,SDL_EVENT_MOUSE_BUTTON_UP,handle.x+handle.width/2+90*original.layout.scale,handle.y+80*original.layout.scale);
        require(window.layout().sidebar.width>original.layout.sidebar.width+70*original.layout.scale,"Sidebar splitter did not resize the native layout");
        click(window,window.layout().outline_header.x+60*original.layout.scale,window.layout().outline_header.y+10*original.layout.scale);
        require(window.layout().outline_rows.height>0,"Outline header did not expand its native region");
        window.open_path("two_with_a_long_name.cpp");settle(window);const auto two=window.active_document_id();
        window.open_path("three.cpp");settle(window);const auto three=window.active_document_id();
        auto state=window.view_state();
        require(state.tab_strip.visible.size()==3,"Wide workbench should show all three tabs");
        require(state.tab_strip.visible[0].bounds.width!=state.tab_strip.visible[1].bounds.width,"Tab sizing ignored shaped filename widths");
        const auto first=state.tab_strip.visible.front().bounds,last=state.tab_strip.visible.back().bounds;
        mouse(window,SDL_EVENT_MOUSE_BUTTON_DOWN,first.x+45*state.layout.scale,first.y+first.height/2);
        mouse(window,SDL_EVENT_MOUSE_MOTION,last.x+45*state.layout.scale,last.y+last.height/2);
        mouse(window,SDL_EVENT_MOUSE_BUTTON_UP,last.x+45*state.layout.scale,last.y+last.height/2);
        require(window.view_state().tabs==std::vector<std::string>{two,three,one},"Native tab dragging did not reorder tabs");
        key(window,SDLK_TAB,SDL_KMOD_CTRL);
        require(window.active_document_id()==two,"JS tab cycling ignored the reordered view state");
        require(SDL_SetWindowSize(window.window(),static_cast<int>(680*state.layout.scale),static_cast<int>(600*state.layout.scale)),"Cannot resize the native SDL window");settle(window);
        require(window.layout().editor.width>=200*window.layout().scale,"Narrow layout lost the document region");
        require(window.view_state().tab_strip.next.width>0,"Narrow workbench did not expose tab overflow controls");
        window.request_screenshot("build/graphics/gui-workbench-narrow.png");settle(window);
        require(SDL_SetWindowSize(window.window(),static_cast<int>(1440*state.layout.scale),static_cast<int>(900*state.layout.scale)),"Cannot restore the native SDL window");settle(window);
        window.execute_ui_command("ui.explorer");settle(window);
        window.request_screenshot("build/graphics/gui-workbench.png");settle(window);
        // Mode switches and text input can arrive in the same SDL event batch.
        SDL_Event mode{};mode.type=SDL_EVENT_KEY_DOWN;mode.key.windowID=SDL_GetWindowID(window.window());mode.key.key=SDLK_P;mode.key.mod=SDL_KMOD_CTRL;mode.key.down=true;
        SDL_Event text{};text.type=SDL_EVENT_TEXT_INPUT;text.text.windowID=SDL_GetWindowID(window.window());text.text.text="three.cpp";
        SDL_Event enter{};enter.type=SDL_EVENT_KEY_DOWN;enter.key.windowID=SDL_GetWindowID(window.window());enter.key.key=SDLK_RETURN;enter.key.down=true;
        require(SDL_PushEvent(&mode)&&SDL_PushEvent(&text)&&SDL_PushEvent(&enter),"Cannot enqueue quick-open event batch");settle(window);
        require(window.active_document_id()==three,"Quick open did not settle focus before same-frame typing");
        window.execute_ui_command("ui.saveAs");settle(window);
        window.request_screenshot("build/graphics/gui-workbench-input.png");settle(window);
        key(window,SDLK_ESCAPE);
        require(!fs::exists(workspace/"renamed-three.cpp"),"Cancelled async save dialog wrote a file");
        mode.key.key=SDLK_S;mode.key.mod=SDL_KMOD_CTRL|SDL_KMOD_SHIFT;
        SDL_Event select=mode;select.key.key=SDLK_A;select.key.mod=SDL_KMOD_CTRL;
        text.text.text="renamed-three.cpp";
        require(SDL_PushEvent(&mode)&&SDL_PushEvent(&select)&&SDL_PushEvent(&text)&&SDL_PushEvent(&enter),"Cannot enqueue async save-as event batch");settle(window);
        require(astra::io::read_file(workspace/"renamed-three.cpp").text==astra::io::read_file(workspace/"three.cpp").text,
            "Async save dialog lost same-batch text/focus or changed document content");
        window.execute_ui_command("ui.closeWindow");
        const auto deadline=Clock::now()+std::chrono::seconds(20);
        while(window.pump()) {require(Clock::now()<deadline,"Workbench shutdown timed out");SDL_Delay(1);}
    }
    host->shutdown();
}
}
int main() {
    fs::path workspace;
    try {
        const auto parent=fs::weakly_canonical(fs::absolute("build/graphics"));fs::create_directories(parent);
        workspace=parent/("gui-presentation-"+std::to_string(Clock::now().time_since_epoch().count()));
        require(fs::create_directory(workspace),"Cannot create presentation fixture");run(workspace);
        const auto issues=astra::test::cleanup_owned_directory(workspace,parent,"gui-presentation-");
        for(const auto& issue:issues) std::cerr<<issue.path<<": "<<issue.message<<'\n';
        require(issues.empty(),"Presentation fixture cleanup failed");
        std::cout<<"Windows native workbench: 1505 indexed files, folder collapse, splitter, outline, shaped tabs, drag reorder, overflow, resize, same-frame open/save focus, async dialog cancellation and shutdown: PASS\n";return 0;
    } catch(const std::exception& error) {std::cerr<<"GUI presentation failed: "<<error.what()<<"\nPreserved fixture: "<<workspace<<'\n';return 1;}
}
