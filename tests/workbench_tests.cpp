#include <astra/ui/workbench.hpp>
#include <astra/io/file_store.hpp>
#include <doctest/doctest.h>

#include <deque>
#include <filesystem>
#include <utility>

namespace {
using Json = astra::protocol::Json;
class Channel final : public astra::ui::EditorChannel {
public:
    struct Request { Json body; std::promise<std::optional<std::string>> response; };
    std::deque<Request> requests;
    std::future<std::optional<std::string>> submit(std::string body) override {
        std::promise<std::optional<std::string>> response;
        auto future = response.get_future();
        requests.push_back({Json::parse(body), std::move(response)});
        return future;
    }
    std::vector<std::string> take_notifications() override { return {}; }
    void reply(Json result) {
        REQUIRE_FALSE(requests.empty());
        auto incoming = std::move(requests.front()); requests.pop_front();
        incoming.response.set_value(Json{{"jsonrpc", "2.0"}, {"id", incoming.body.at("id")}, {"result", result}}.dump());
    }
};
std::map<std::string, std::string> modules() {
    std::map<std::string, std::string> result;
    for (const auto& [name, file] : std::map<std::string, std::string>{
        {"astra/ui", "api.js"}, {"astra/files", "files.js"}, {"astra/git", "git.js"},
        {"astra/search", "search.js"}, {"astra/clipboard", "clipboard.js"}, {"astra/commands", "commands.js"}, {"astra/workbench", "workbench.js"}}) {
        result[name] = astra::io::read_file(std::filesystem::path("data/workbench") / file).text;
    }
    return result;
}
std::future<Json> ready(Json value) { std::promise<Json> result; result.set_value(std::move(value)); return result.get_future(); }
}

TEST_CASE("workbench scripts own UI commands and asynchronous document workflows") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> updates;
    astra::ui::Workbench workbench(channel, [&](std::string_view method, const Json& params) {
        REQUIRE(method == "_ui.update"); updates.push_back(params); return ready(nullptr);
    }, modules());
    REQUIRE(workbench.contains("ui.new")); REQUIRE(workbench.contains("ui.save"));
    CHECK(workbench.settings()["workbench"]["views"].size() == 3);
    bool completed{};
    workbench.execute("ui.new", Json::object(), [&](const auto& result) { CHECK_FALSE(result.error); completed = true; });
    workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.create");
    CHECK_FALSE(completed);
    channel->reply({{"id", "42"}, {"revision", "0"}});
    for (int tick = 0; tick < 4; ++tick) { workbench.poll(); }
    REQUIRE(completed); REQUIRE(updates.size() == 1);
    CHECK(updates[0]["activate"] == "42"); CHECK(workbench.idle());
}

TEST_CASE("workbench user commands replace builtins and preserve service errors") {
    auto channel = std::make_shared<Channel>();
    std::vector<std::string> statuses;
    const auto source = "astra.ui.registerCommand({id:'ui.save',title:'Custom save'},async()=>{await astra.ui.update({status:'Custom policy'});return 7}); astra.registerCommand('failure',()=>astra.request('bad'))";
    astra::ui::Workbench workbench(channel, [&](std::string_view, const Json& params) { statuses.push_back(params.at("status")); return ready(nullptr); }, modules(), source);
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.save", Json::object(), [&](const auto& value) { outcome = value; });
    for (int tick = 0; tick < 4; ++tick) { workbench.poll(); }
    REQUIRE(outcome); CHECK(outcome->value == 7);
    CHECK(statuses == std::vector<std::string>{"Custom policy"}); CHECK(channel->requests.empty());
    workbench.execute("failure", Json::object(), [&](const auto& value) { outcome = value; });
    workbench.poll(); REQUIRE(channel->requests.size() == 1);
    auto incoming = std::move(channel->requests.front()); channel->requests.pop_front();
    incoming.response.set_value(Json{{"jsonrpc", "2.0"}, {"id", incoming.body.at("id")},
        {"error", {{"code", -32001}, {"message", "stale"}, {"data", {{"revision", "9"}}}}}}.dump());
    workbench.poll(); REQUIRE(outcome->error);
    CHECK(outcome->error->at("code") == -32001); CHECK(outcome->error->at("data")["revision"] == "9");
    CHECK(workbench.idle());
}

TEST_CASE("workbench provider generation prevents older results replacing newer views") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> models;
    const auto source = "astra.ui.registerView({id:'explorer',title:'Files'},(p,o)=>astra.request('files',p,o)); astra.registerCommand('refresh',p=>astra.ui.refresh('explorer',p));";
    astra::ui::Workbench workbench(channel, [&](std::string_view, const Json& params) {
        if (params.contains("model")) { models.push_back(params.at("model")); } return ready(nullptr);
    }, modules(), source);
    workbench.execute("refresh", {{"generation", 1}}, {});
    workbench.poll(); workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    workbench.execute("refresh", {{"generation", 2}}, {});
    workbench.poll(); workbench.poll();
    REQUIRE(channel->requests.size() == 2);
    channel->reply({{"entries", Json::array({"old"})}});
    channel->reply({{"entries", Json::array({"new"})}});
    for (int tick = 0; tick < 8; ++tick) { workbench.poll(); }
    REQUIRE(models.size() == 1); CHECK(models[0]["entries"] == Json::array({"new"}));
    CHECK(workbench.idle());
}

TEST_CASE("workbench clipboard pages Unicode bytes and cuts one captured transaction") {
    auto channel = std::make_shared<Channel>();
    std::string clipboard;
    const Json document{{"id","original"},{"revision","7"},{"selections",Json::array({
        {{"anchor",7},{"head",0}},{{"anchor",10},{"head",13}},{{"anchor",0},{"head",7}}})}};
    astra::ui::Workbench workbench(channel, [&](std::string_view method,const Json& params) {
        if (method == "_ui.read") return ready({{"document",document},{"selectionLines",Json::array()}});
        if (method == "_ui.clipboard.write") clipboard=params.at("text").get<std::string>();
        return ready(nullptr);
    },modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.cut",Json::object(),[&](const auto& result){outcome=result;});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(channel->requests.size()==1);
    CHECK(channel->requests.front().body.at("params").at("begin")==0);
    channel->reply({{"revision","7"},{"range",{{"end",3}}},{"text","中"}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(channel->requests.size()==1);
    CHECK(channel->requests.front().body.at("params").at("begin")==3);
    channel->reply({{"revision","7"},{"range",{{"end",7}}},{"text","🙂"}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(channel->requests.size()==1);
    CHECK(channel->requests.front().body.at("params").at("begin")==10);
    channel->reply({{"revision","7"},{"range",{{"end",13}}},{"text","e\xCC\x81"}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    CHECK(clipboard=="中🙂\ne\xCC\x81");
    REQUIRE(channel->requests.size()==1);
    const auto& edit=channel->requests.front().body;
    CHECK(edit.at("method")=="document.edit");
    CHECK(edit.at("params").at("id")=="original");
    CHECK(edit.at("params").at("baseRevision")=="7");
    CHECK(edit.at("params").at("edits")==Json::array({{{"begin",0},{"end",7},{"text",""}},{{"begin",10},{"end",13},{"text",""}}}));
    channel->reply({{"id","original"},{"revision","8"}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(outcome);CHECK_FALSE(outcome->error);CHECK(workbench.idle());
}

TEST_CASE("workbench stale clipboard read never writes or cuts") {
    auto channel=std::make_shared<Channel>();
    bool wrote{};
    astra::ui::Workbench workbench(channel,[&](std::string_view method,const Json&){
        if(method=="_ui.read")return ready({{"document",{{"id","1"},{"revision","2"},{"selections",Json::array({{{"anchor",0},{"head",4}}})}}}});
        if(method=="_ui.clipboard.write")wrote=true;
        return ready(nullptr);
    },modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.cut",Json::object(),[&](const auto& result){outcome=result;});
    for(int tick=0;tick<4;++tick)workbench.poll();
    channel->reply({{"revision","3"},{"range",{{"end",4}}},{"text","text"}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(outcome);REQUIRE(outcome->error);CHECK_FALSE(wrote);CHECK(channel->requests.empty());
    CHECK(outcome->error->at("message").get<std::string>().find("changed while copying")!=std::string::npos);
}

TEST_CASE("workbench paste captures document identity and normalizes CRLF") {
    auto channel=std::make_shared<Channel>();
    std::promise<Json> clipboard;
    astra::ui::Workbench workbench(channel,[&](std::string_view method,const Json&){
        if(method=="_ui.read")return ready({{"document",{{"id","captured"},{"revision","9"}}},{"newline","\r\n"}});
        if(method=="_ui.clipboard.read")return clipboard.get_future();
        return ready(nullptr);
    },modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.paste",Json::object(),[&](const auto& result){outcome=result;});
    for(int tick=0;tick<4;++tick)workbench.poll();
    CHECK_FALSE(outcome);CHECK(channel->requests.empty());
    clipboard.set_value("中\r\n🙂\re\xCC\x81\n");
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(channel->requests.size()==1);
    const auto& request=channel->requests.front().body;
    CHECK(request.at("method")=="script.command");
    CHECK(request.at("params").at("params").at("id")=="captured");
    CHECK(request.at("params").at("params").at("baseRevision")=="9");
    CHECK(request.at("params").at("params").at("text")=="中\r\n🙂\r\ne\xCC\x81\r\n");
    channel->reply({{"id","captured"},{"revision","10"}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(outcome);CHECK_FALSE(outcome->error);CHECK(workbench.idle());
}

TEST_CASE("workbench modal input awaits the user without hiding unrelated work") {
    auto channel=std::make_shared<Channel>();
    std::promise<Json> input;
    astra::ui::Workbench workbench(channel,[&](std::string_view method,const Json&){
        if(method=="_ui.dialog")return input.get_future();
        return ready(nullptr);
    },modules(),"astra.registerCommand('background',()=>astra.request('other')); astra.registerCommand('prompt',()=>astra.ui.input({title:'Value'}));");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("prompt",Json::object(),[&](const auto& value){outcome=value;});
    for(int tick=0;tick<4;++tick)workbench.poll();
    CHECK_FALSE(workbench.idle());CHECK(workbench.awaiting_input());CHECK_FALSE(outcome);
    workbench.execute("background",Json::object(),{});
    workbench.poll();CHECK_FALSE(workbench.awaiting_input());
    channel->reply(nullptr);
    for(int tick=0;tick<4;++tick)workbench.poll();
    CHECK(workbench.awaiting_input());
    input.set_value("chosen");
    for(int tick=0;tick<6;++tick)workbench.poll();
    REQUIRE(outcome);CHECK_FALSE(outcome->error);CHECK(outcome->value=="chosen");CHECK(workbench.idle());
}

TEST_CASE("workbench input cancellation dismisses its native dialog and drains the late future") {
    auto channel=std::make_shared<Channel>();
    std::promise<Json> input;
    unsigned cancelled{};
    astra::ui::Workbench workbench(channel,[&](std::string_view method,const Json&){
        if(method=="_ui.dialog")return input.get_future();
        if(method=="_ui.dialog.cancel"){++cancelled;input.set_value(nullptr);}
        return ready(nullptr);
    },modules(),"let control=new AbortController(); astra.registerCommand('prompt',()=>astra.ui.input({title:'Value'},{signal:control.signal})); astra.registerCommand('abort',()=>control.abort());");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("prompt",Json::object(),[&](const auto& value){outcome=value;});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(workbench.awaiting_input());
    workbench.execute("abort",Json::object(),{});
    for(int tick=0;tick<8;++tick)workbench.poll();
    REQUIRE(outcome);REQUIRE(outcome->error);CHECK(outcome->error->at("name")=="AbortError");
    CHECK(cancelled==1);CHECK(workbench.idle());
}

TEST_CASE("workbench reload confirmation captures a revision before awaiting input") {
    auto channel=std::make_shared<Channel>();
    std::promise<Json> confirmation;
    astra::ui::Workbench workbench(channel,[&](std::string_view method,const Json&){
        if(method=="_ui.read")return ready({{"document",{{"id","capture"},{"revision","42"},{"dirty",true},{"path","source.cpp"}}}});
        if(method=="_ui.dialog")return confirmation.get_future();
        return ready(nullptr);
    },modules());
    workbench.execute("ui.reload",Json::object(),{});
    for(int tick=0;tick<4;++tick)workbench.poll();
    CHECK(channel->requests.empty());CHECK(workbench.awaiting_input());
    confirmation.set_value(true);
    for(int tick=0;tick<6;++tick)workbench.poll();
    REQUIRE(channel->requests.size()==1);
    CHECK(channel->requests.front().body.at("method")=="workspace.reload");
    CHECK(channel->requests.front().body.at("params")==Json{{"id","capture"},{"baseRevision","42"},{"discardChanges",true}});
    channel->reply({{"id","capture"},{"revision","43"}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    CHECK(workbench.idle());
}

TEST_CASE("workbench composed commands honor user replacements through awaited execution") {
    auto channel=std::make_shared<Channel>();
    astra::ui::Workbench workbench(channel,[](std::string_view,const Json&){return ready(nullptr);},modules(),
        "astra.ui.registerCommand({id:'ui.saveAs',title:'User save'},async p=>{await astra.sleep(0);return p.value+1;});astra.ui.registerCommand({id:'composed',title:'Composed'},()=>astra.ui.execute('ui.saveAs',{value:41}));");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("composed",Json::object(),[&](const auto& value){outcome=value;});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(outcome);CHECK_FALSE(outcome->error);CHECK(outcome->value==42);CHECK(channel->requests.empty());
}

TEST_CASE("workbench Git discard awaits confirmation and retains its exact preview") {
    auto channel=std::make_shared<Channel>();
    std::promise<Json> confirmation;
    astra::ui::Workbench workbench(channel,[&](std::string_view method,const Json&){
        if(method=="_ui.read")return ready({{"gitPath","file.cpp"},{"gitStaged",false},{"gitPreview","captured-preview"}});
        if(method=="_ui.dialog")return confirmation.get_future();
        return ready(nullptr);
    },modules());
    workbench.execute("ui.gitDiscard",Json::object(),{});
    for(int tick=0;tick<6;++tick)workbench.poll();
    CHECK(channel->requests.empty());CHECK(workbench.awaiting_input());
    confirmation.set_value(true);
    for(int tick=0;tick<6;++tick)workbench.poll();
    REQUIRE(channel->requests.size()==1);
    CHECK(channel->requests.front().body.at("method")=="git.discard");
    CHECK(channel->requests.front().body.at("params").at("previewId")=="captured-preview");
    channel->reply({{"entries",Json::array()}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    REQUIRE(channel->requests.size()==1);
    CHECK(channel->requests.front().body.at("method")=="workspace.tree");
    channel->reply({{"entries",Json::array()}});
    for(int tick=0;tick<6;++tick)workbench.poll();
    CHECK(workbench.idle());
}

TEST_CASE("workbench Git commit skips input when no staged changes exist") {
    auto channel=std::make_shared<Channel>();bool prompted{};
    astra::ui::Workbench workbench(channel,[&](std::string_view method,const Json&){
        if(method=="_ui.dialog")prompted=true;
        return ready(nullptr);
    },modules());
    workbench.execute("ui.gitCommit",Json::object(),{});workbench.poll();
    REQUIRE(channel->requests.size()==1);
    CHECK(channel->requests.front().body.at("method")=="git.status");
    channel->reply({{"entries",Json::array({{{"path","untracked.cpp"},{"staged","untracked"}}})}});
    for(int tick=0;tick<4;++tick)workbench.poll();
    CHECK_FALSE(prompted);CHECK(channel->requests.empty());CHECK(workbench.idle());
}
