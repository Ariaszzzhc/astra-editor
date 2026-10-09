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
        {"astra/search", "search.js"}, {"astra/commands", "commands.js"}, {"astra/workbench", "workbench.js"}}) {
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
