#include <astra/ui/workbench.hpp>
#include <astra/ui/input_queue.hpp>
#include <astra/ui/inline_form.hpp>
#include <astra/ui/form_registry.hpp>
#include <astra/ui/view_store.hpp>
#include <astra/io/file_store.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <utility>

namespace {
using Json = astra::protocol::Json;

class Channel final : public astra::ui::EditorChannel {
  public:
    struct Request {
        Json body;
        std::promise<std::optional<std::string>> response;
    };

    std::deque<Request> requests;

    std::future<std::optional<std::string>> submit(std::string body) override {
        std::promise<std::optional<std::string>> response;
        auto future = response.get_future();
        requests.push_back({Json::parse(body), std::move(response)});
        return future;
    }

    std::vector<std::string> take_notifications() override {
        return {};
    }

    void reply(Json result) {
        REQUIRE_FALSE(requests.empty());
        auto incoming = std::move(requests.front());
        requests.pop_front();
        incoming.response.set_value(
            Json{{"jsonrpc", "2.0"}, {"id", incoming.body.at("id")}, {"result", result}}.dump());
    }
};

std::map<std::string, std::string> modules() {
    std::map<std::string, std::string> result;
    for (const auto& [name, file] :
         std::map<std::string, std::string>{{"astra/ui", "api.js"},
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
        result[name] = astra::io::read_file(std::filesystem::path("data/workbench") / file).text;
    }
    return result;
}

std::future<Json> ready(Json value) {
    std::promise<Json> result;
    result.set_value(std::move(value));
    return result.get_future();
}

struct InlineHost {
    std::optional<astra::ui::InlineForm> form;
    Json spec, document{{"id", "captured"}, {"revision", "7"}};
    std::vector<Json> updates;
    unsigned sequence{};

    std::future<Json> operator()(std::string_view method, const Json& params) {
        if (method == "_ui.read")
            return ready({{"document", document}});
        if (method == "_ui.form.open") {
            spec = params;
            form.emplace(params, std::to_string(++sequence));
            return ready(form->state());
        }
        if (method == "_ui.form.read")
            return ready(form && params.at("id") == form->id() ? form->state() : Json(nullptr));
        if (method == "_ui.form.close") {
            form.reset();
            return ready(true);
        }
        if (method == "_ui.form.update")
            return ready(form && form->update(params));
        if (method == "_ui.update") {
            updates.push_back(params);
            if (params.contains("document"))
                document = params.at("document");
            return ready(nullptr);
        }
        CHECK(method == "_ui.action");
        return ready(nullptr);
    }
};

struct ViewHost {
    astra::ui::ViewStore views;
    std::vector<Json> pages;

    std::future<Json> operator()(std::string_view method, const Json& params) {
        if (method == "_ui.view.begin")
            return ready(views.begin(params.at("spec")));
        if (method == "_ui.view.append") {
            pages.push_back(params.at("rows"));
            return ready(views.append(params.at("id").get<std::string>(),
                                      params.at("version").get<std::string>(),
                                      params.at("rows")));
        }
        if (method == "_ui.view.abort")
            return ready(views.abort(params.at("id").get<std::string>(),
                                     params.at("version").get<std::string>()));
        if (method == "_ui.view.commit")
            return views.commit(params.at("id").get<std::string>(),
                                params.at("version").get<std::string>(),
                                params.value("subtitle", std::string{}),
                                params.value("empty", std::string{}));
        if (method == "_ui.view.navigate" || method == "_ui.view.read" ||
            method == "_ui.view.select") {
            const auto id = params.at("id").get<std::string>();
            if (params.contains("version") && params.at("version") != views.at(id).version)
                return ready(nullptr);
            if (method == "_ui.view.navigate")
                return ready(views.navigate(id, params.at("action").get<std::string>(), 8));
            if (method == "_ui.view.select" &&
                !views.select(id, params.at("row").get<std::string>(), 8))
                return ready(nullptr);
            return ready(views.state(id));
        }
        return ready(nullptr);
    }
};

struct MountedHost {
    astra::ui::FormRegistry forms;
    ViewHost view;
    std::vector<Json> updates;
    std::optional<std::promise<Json>> confirmation;
    Json confirmation_spec;
    unsigned sequence{};

    MountedHost() {
        view.views.register_spec(
            {{"id", "search"}, {"title", "Search"}, {"icon", "search"}, {"order", 1}});
    }

    std::future<Json> operator()(std::string_view method, const Json& params) {
        if (method.starts_with("_ui.view."))
            return view(method, params);
        if (method == "_ui.read") {
            auto state =
                Json{{"tabs", Json::array()}, {"document", nullptr}, {"focusedForm", nullptr}};
            if (const auto* focused = forms.focused())
                state["focusedForm"] = {{"id", focused->id()}, {"view", focused->view()}};
            return ready(state);
        }
        if (method == "_ui.form.open") {
            static_cast<void>(forms.open(
                std::make_unique<astra::ui::InlineForm>(params, std::to_string(++sequence))));
            return ready(forms.find(params.at("id").get<std::string>())->state());
        }
        if (method.starts_with("_ui.form.")) {
            auto* form = forms.find(params.at("id").get<std::string>());
            if (!form)
                return ready(nullptr);
            if (method == "_ui.form.read")
                return ready(form->state());
            if (method == "_ui.form.update")
                return ready(form->update(params));
            if (method == "_ui.form.setValues") {
                if (params.at("version") != form->version())
                    return ready(nullptr);
                static_cast<void>(form->set_values(params));
                return ready(form->state());
            }
            if (method == "_ui.form.focus") {
                const auto field = params.at("field").get<std::string>();
                const auto found = std::find_if(
                    form->fields().begin(), form->fields().end(), [&](const auto& value) {
                        return value.id == field;
                    });
                REQUIRE(found != form->fields().end());
                forms.focus(form->id(), static_cast<std::size_t>(found - form->fields().begin()));
                return ready(form->state());
            }
            if (method == "_ui.form.blur") {
                form->focus(std::nullopt);
                return ready(true);
            }
            if (method == "_ui.form.close")
                return ready(static_cast<bool>(forms.close(form->id())));
        }
        if (method == "_ui.update") {
            updates.push_back(params);
            return ready(nullptr);
        }
        if (method == "_ui.dialog") {
            REQUIRE_FALSE(confirmation);
            confirmation_spec = params;
            forms.blur();
            confirmation.emplace();
            return confirmation->get_future();
        }
        if (method == "_ui.dialog.cancel" || method == "_ui.dialog.answer") {
            if (confirmation && params.at("id") == confirmation_spec.at("id")) {
                confirmation->set_value(method == "_ui.dialog.answer" ? Json(params.at("accepted"))
                                                                      : Json(nullptr));
                confirmation.reset();
            }
            return ready(nullptr);
        }
        CHECK(method == "_ui.action");
        return ready(nullptr);
    }

    void poll(astra::ui::Workbench& workbench) {
        for (int tick = 0; tick < 32; ++tick) {
            view.views.poll();
            workbench.poll();
        }
    }
};

Json workspace_matches(bool truncated = false, bool issues = false) {
    return {
        {"state", "completed"},
        {"result",
         {{"matches", Json::array({{{"path", "a.txt"}, {"line", 0}, {"begin", 0}, {"end", 4}}})},
          {"truncated", truncated},
          {"issues",
           issues ? Json::array({{{"path", "b.txt"}, {"message", "unreadable"}}})
                  : Json::array()}}}};
}

void reply_workspace_search(Channel& channel, MountedHost& host, astra::ui::Workbench& workbench) {
    REQUIRE(channel.requests.size() == 1);
    REQUIRE(channel.requests.front().body.at("method") == "workspace.search");
    channel.reply({{"operationId", "workspace-search"}});
    host.poll(workbench);
    REQUIRE(channel.requests.size() == 1);
    REQUIRE(channel.requests.front().body.at("method") == "operation.get");
    channel.reply(workspace_matches());
    host.poll(workbench);
}
} // namespace

TEST_CASE("workbench workspace search owns mounted inputs and guarded generic view publication") {
    auto channel = std::make_shared<Channel>();
    MountedHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](auto method, const auto& params) {
            return host(method, params);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("workbench.search",
                      {{"query", "中🙂"}, {"regex", true}, {"scope", "disk"}, {"maxMatches", 10}},
                      [&](const auto& result) {
                          outcome = result;
                      });
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    const auto parameters = channel->requests.front().body.at("params");
    CHECK(parameters.at("query") == "中🙂");
    CHECK(parameters.at("regex") == true);
    CHECK(parameters.at("scope") == "disk");
    CHECK(parameters.at("maxMatches") == 10);
    CHECK_FALSE(parameters.contains("replacement"));
    REQUIRE(host.forms.mounted("search"));
    CHECK(host.forms.mounted("search")->state().at("values").at("query") == "中🙂");
    CHECK_FALSE(outcome);
    reply_workspace_search(*channel, host, workbench);
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(host.view.views.at("search").index.size() == 1);
    CHECK(host.forms.mounted("search")->message() == "1 match(es)");
    CHECK_FALSE(host.confirmation);
    CHECK(channel->requests.empty());
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench unsafe workspace search previews cannot start replacement") {
    for (const auto truncated : {false, true}) {
        auto channel = std::make_shared<Channel>();
        MountedHost host;
        astra::ui::Workbench workbench(
            channel,
            [&](auto method, const auto& params) {
                return host(method, params);
            },
            modules());
        workbench.execute("workbench.search", {{"query", "find"}, {"replacement", "replace"}}, {});
        host.poll(workbench);
        reply_workspace_search(*channel, host, workbench);
        auto state = host.forms.mounted("search")->state();
        state["action"] = "replaceAll";
        workbench.emit("ui.form.action", state);
        host.poll(workbench);
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "workspace.search");
        channel->reply({{"operationId", "fresh-search"}});
        host.poll(workbench);
        REQUIRE(channel->requests.size() == 1);
        channel->reply(workspace_matches(truncated, !truncated));
        host.poll(workbench);
        CHECK_FALSE(host.confirmation);
        CHECK(host.forms.mounted("search")->error());
        CHECK(channel->requests.empty());
        CHECK(workbench.idle());
        CHECK(workbench.take_errors().empty());
    }
}

TEST_CASE("workbench accepted workspace cancellation retains partial write reports") {
    auto channel = std::make_shared<Channel>();
    MountedHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](auto method, const auto& params) {
            return host(method, params);
        },
        modules());
    workbench.execute("workbench.search", {{"query", "find"}, {"replacement", "replace"}}, {});
    host.poll(workbench);
    reply_workspace_search(*channel, host, workbench);
    auto snapshot = host.forms.mounted("search")->state();
    snapshot["action"] = "replaceAll";
    workbench.emit("ui.form.action", snapshot);
    host.poll(workbench);
    reply_workspace_search(*channel, host, workbench);
    REQUIRE(host.confirmation);
    CHECK(host.confirmation_spec.at("title") == "Replace 1 match(es)?");
    CHECK(channel->requests.empty());
    workbench.execute("ui.confirmReplace", Json::object(), {});
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.replace");
    CHECK(channel->requests.front().body.at("params").at("replacement") == "replace");
    workbench.execute("workbench.cancelWorkspaceSearch", Json::object(), {});
    host.poll(workbench);
    channel->reply({{"operationId", "write"}});
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.cancel");
    CHECK(channel->requests.front().body.at("params").at("id") == "write");
    channel->reply({{"cancelRequested", true}});
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.get");
    const Json modified = Json::array({{{"id", "unsaved"}, {"revision", "9"}, {"dirty", true}}});
    channel->reply({{"state", "completed"},
                    {"result",
                     {{"applied", Json::array({"a.txt"})},
                      {"modifiedBuffers", modified},
                      {"pending", Json::array({"b.txt"})},
                      {"pendingBuffers", Json::array({"conflict"})},
                      {"issues", Json::array()},
                      {"cancelled", true}}}});
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.tree");
    channel->reply({{"entries", Json::array()}, {"truncated", false}});
    host.poll(workbench);
    const auto result =
        std::find_if(host.updates.begin(), host.updates.end(), [](const auto& value) {
            return value.contains("notice");
        });
    REQUIRE(result != host.updates.end());
    CHECK(result->at("documents") == modified);
    CHECK(result->at("notice") == "Replaced in 1 disk file(s) and 1 unsaved buffer(s); 2 pending, "
                                  "0 issue(s), cancelled. Search again before retrying");
    CHECK_FALSE(host.forms.mounted("search")->busy());
    CHECK(workbench.take_errors().empty());
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench workspace replacement rechecks native input after confirmation") {
    auto channel = std::make_shared<Channel>();
    MountedHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](auto method, const auto& params) {
            return host(method, params);
        },
        modules());
    workbench.execute("workbench.search", {{"query", "find"}, {"replacement", "replace"}}, {});
    host.poll(workbench);
    reply_workspace_search(*channel, host, workbench);
    auto snapshot = host.forms.mounted("search")->state();
    snapshot["action"] = "replaceAll";
    workbench.emit("ui.form.action", snapshot);
    host.poll(workbench);
    reply_workspace_search(*channel, host, workbench);
    REQUIRE(host.confirmation);
    REQUIRE(host.forms.mounted("search")->set_values({{"id", snapshot.at("id")},
                                                      {"version", snapshot.at("version")},
                                                      {"values", {{"query", "new query"}}}}));
    // Deliberately delay the changed event: publication must fence the actual
    // retained input state, rather than relying on notification delivery order.
    workbench.execute("ui.confirmReplace", Json::object(), {});
    host.poll(workbench);
    CHECK_FALSE(host.confirmation);
    CHECK(channel->requests.empty());
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
    CHECK(host.forms.mounted("search")->state().at("values").at("query") == "new query");
}

TEST_CASE("workbench workspace search cancellation drains late startup identity") {
    auto channel = std::make_shared<Channel>();
    MountedHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](auto method, const auto& params) {
            return host(method, params);
        },
        modules());
    workbench.execute("workbench.search", {{"query", "find"}}, {});
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.search");
    workbench.execute("workbench.cancelWorkspaceSearch", Json::object(), {});
    host.poll(workbench);
    CHECK_FALSE(workbench.idle());
    channel->reply({{"operationId", "late-search"}});
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.cancel");
    CHECK(channel->requests.front().body.at("params").at("id") == "late-search");
    channel->reply({{"cancelRequested", true}});
    host.poll(workbench);
    CHECK(host.view.views.at("search").index.size() == 0);
    CHECK(host.forms.mounted("search")->state().at("values").at("query") == "find");
    CHECK_FALSE(host.forms.focused());
    CHECK_FALSE(host.forms.mounted("search")->busy());
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench superseded workspace failures cannot overwrite newer input or status") {
    auto channel = std::make_shared<Channel>();
    MountedHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](auto method, const auto& params) {
            return host(method, params);
        },
        modules());
    workbench.execute("workbench.search", {{"query", "old"}}, {});
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    workbench.execute("workbench.search", {{"query", "new"}}, {});
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 2);
    auto old = std::move(channel->requests.front());
    channel->requests.pop_front();
    old.response.set_value(Json{{"jsonrpc", "2.0"},
                                {"id", old.body.at("id")},
                                {"error", {{"code", -32004}, {"message", "old query rejected"}}}}
                               .dump());
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("params").at("query") == "new");
    reply_workspace_search(*channel, host, workbench);
    CHECK(host.forms.mounted("search")->state().at("values").at("query") == "new");
    CHECK_FALSE(host.forms.mounted("search")->error());
    for (const auto& update : host.updates)
        if (update.contains("error"))
            CHECK(update.at("error") != "old query rejected");
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench scripts own UI commands and asynchronous document workflows") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            REQUIRE(method == "_ui.update");
            updates.push_back(params);
            return ready(nullptr);
        },
        modules());
    REQUIRE(workbench.contains("ui.new"));
    REQUIRE(workbench.contains("ui.save"));
    CHECK(workbench.settings()["workbench"]["views"].size() == 3);
    bool completed{};
    workbench.execute("ui.new", Json::object(), [&](const auto& result) {
        CHECK_FALSE(result.error);
        completed = true;
    });
    workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.create");
    CHECK_FALSE(completed);
    channel->reply({{"id", "42"}, {"revision", "0"}});
    for (int tick = 0; tick < 4; ++tick) {
        workbench.poll();
    }
    REQUIRE(completed);
    REQUIRE(updates.size() == 1);
    CHECK(updates[0]["activate"] == "42");
    CHECK(workbench.idle());
}

TEST_CASE("workbench user commands replace builtins and preserve service errors") {
    auto channel = std::make_shared<Channel>();
    std::vector<std::string> statuses;
    const auto source = "astra.ui.registerCommand({id:'ui.save',title:'Custom "
                        "save'},async()=>{await astra.ui.update({status:'Custom policy'});return "
                        "7}); astra.registerCommand('failure',()=>astra.request('bad'))";
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view, const Json& params) {
            statuses.push_back(params.at("status"));
            return ready(nullptr);
        },
        modules(),
        source);
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.save", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick) {
        workbench.poll();
    }
    REQUIRE(outcome);
    CHECK(outcome->value == 7);
    CHECK(statuses == std::vector<std::string>{"Custom policy"});
    CHECK(channel->requests.empty());
    workbench.execute("failure", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    auto incoming = std::move(channel->requests.front());
    channel->requests.pop_front();
    incoming.response.set_value(
        Json{{"jsonrpc", "2.0"},
             {"id", incoming.body.at("id")},
             {"error", {{"code", -32001}, {"message", "stale"}, {"data", {{"revision", "9"}}}}}}
            .dump());
    workbench.poll();
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("code") == -32001);
    CHECK(outcome->error->at("data")["revision"] == "9");
    CHECK(workbench.idle());
}

TEST_CASE("workbench document search uses asynchronous service results without downloading text") {
    for (const auto replace : {false, true}) {
        CAPTURE(replace);
        auto channel = std::make_shared<Channel>();
        std::vector<Json> updates;
        const Json document{{"id", "captured"}, {"revision", "7"}};
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                if (method == "_ui.read")
                    return ready({{"document", document}});
                REQUIRE(method == "_ui.update");
                updates.push_back(params);
                return ready(nullptr);
            },
            modules());
        std::optional<astra::script::Runtime::Outcome> outcome;
        Json parameters{
            {"query", "(中🙂)"}, {"regex", true}, {"caseSensitive", false}, {"wholeWord", true}};
        if (replace)
            parameters["replacement"] = "[$1]";
        workbench.execute(replace ? "ui.replace" : "ui.find", parameters, [&](const auto& result) {
            outcome = result;
        });
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") ==
              (replace ? "document.replaceAll" : "document.find"));
        const auto params = channel->requests.front().body.at("params");
        CHECK(params.at("id") == "captured");
        CHECK(params.at("baseRevision") == "7");
        CHECK(params.at("regex") == true);
        CHECK(params.at("query") == "(中🙂)");
        CHECK_FALSE(outcome);
        CHECK_FALSE(workbench.awaiting_input());
        channel->reply({{"operationId", "search-1"}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "operation.get");
        Json result{{"document", document}};
        if (replace) {
            result["replacements"] = 2;
            result["changed"] = true;
        } else {
            result["match"] = {{"begin", 4}, {"end", 11}};
            result["wrapped"] = true;
        }
        channel->reply({{"state", "completed"}, {"result", result}});
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(channel->requests.empty());
        CHECK(workbench.idle());
        REQUIRE(updates.size() == 2);
        CHECK(updates.back().at("document") == document);
        CHECK(updates.back().at("status") ==
              (replace ? "Replaced 2 match(es)" : "Match found; wrapped to beginning"));
    }
}

TEST_CASE("workbench cancellation drains late document search identity") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document", {{"id", "source"}, {"revision", "4"}}}});
            if (method == "_ui.update")
                updates.push_back(params);
            else
                CHECK(method == "_ui.action");
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.find", {{"query", "needle"}}, [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.find");
    workbench.execute("ui.cancel", Json::object(), {});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == false);
    CHECK_FALSE(workbench.idle());
    channel->reply({{"operationId", "late-search"}});
    for (int tick = 0; tick < 10; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.cancel");
    CHECK(channel->requests.front().body.at("params").at("id") == "late-search");
    channel->reply(nullptr);
    for (int tick = 0; tick < 10; ++tick)
        workbench.poll();
    CHECK(workbench.idle());
    CHECK(channel->requests.empty());
    CHECK(std::none_of(updates.begin(), updates.end(), [](const Json& value) {
        return value.contains("document");
    }));
}

TEST_CASE("workbench document replace opens a nonmodal native form without a service request") {
    auto channel = std::make_shared<Channel>();
    InlineHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.replace", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    REQUIRE(host.form);
    CHECK(outcome->value == host.form->state());
    CHECK(host.form->focus() == 1);
    CHECK(host.spec.at("fields").size() == 2);
    CHECK(host.spec.at("toggles").size() == 3);
    CHECK(host.spec.at("actions").size() == 3);
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
    CHECK_FALSE(workbench.awaiting_input());
    auto snapshot = host.form->state();
    host.form.reset();
    workbench.emit("ui.form.closed", snapshot);
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench explicit form navigation cancels preview and serializes repeated actions") {
    auto channel = std::make_shared<Channel>();
    InlineHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules());
    const auto poll = [&] {
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
    };
    workbench.execute("ui.find", Json::object(), {});
    poll();
    REQUIRE(host.form);
    REQUIRE(host.form->edit(astra::editing::Command::insert_text, {.text = "中🙂"}));
    auto snapshot = host.form->state();
    workbench.emit("ui.form.changed", snapshot);
    snapshot["action"] = "next";
    workbench.emit("ui.form.action", snapshot);
    poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.find");
    CHECK(channel->requests.front().body.at("params").at("select") == true);
    CHECK(host.form->busy());
    snapshot["action"] = "previous";
    workbench.emit("ui.form.action", snapshot);
    poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.find");
    channel->reply({{"operationId", "first"}});
    poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.get");
    channel->reply({{"state", "completed"},
                    {"result",
                     {{"match", {{"begin", 3}, {"end", 10}}},
                      {"wrapped", false},
                      {"document", host.document}}}});
    poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.find");
    CHECK(channel->requests.front().body.at("params").at("direction") == "backward");
    channel->reply({{"operationId", "second"}});
    poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.get");
    channel->reply({{"state", "completed"}, {"result", {{"match", nullptr}, {"wrapped", false}}}});
    poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
    CHECK_FALSE(host.form->busy());
    CHECK(host.form->message() == "No matches");
    CHECK(workbench.take_errors().empty());
}

TEST_CASE("workbench inline cancellation captures input before queued changes are dispatched") {
    auto channel = std::make_shared<Channel>();
    InlineHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules());
    const auto poll = [&] {
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
    };
    workbench.execute("ui.find", Json::object(), {});
    poll();
    REQUIRE(host.form);
    REQUIRE(host.form->edit(astra::editing::Command::insert_text, {.text = "last 中🙂"}));
    const auto queued = host.form->state();
    workbench.execute("ui.cancel", Json::object(), {});
    poll();
    CHECK_FALSE(host.form);
    workbench.emit("ui.form.changed", queued);
    workbench.emit("ui.form.closed", queued);
    poll();
    workbench.execute("ui.find", Json::object(), {});
    poll();
    REQUIRE(host.form);
    CHECK(host.form->state().at("values").at("query") == "last 中🙂");
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
    CHECK(workbench.take_errors().empty());
}

TEST_CASE(
    "workbench closed inline forms cancel and drain late operations without publishing results") {
    auto channel = std::make_shared<Channel>();
    InlineHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules());
    const auto poll = [&] {
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
    };
    workbench.execute("ui.find", Json::object(), {});
    poll();
    REQUIRE(host.form);
    REQUIRE(host.form->edit(astra::editing::Command::insert_text, {.text = "needle"}));
    auto snapshot = host.form->state();
    workbench.emit("ui.form.changed", snapshot);
    snapshot["action"] = "next";
    workbench.emit("ui.form.action", snapshot);
    poll();
    REQUIRE(channel->requests.size() == 1);
    host.form.reset();
    workbench.emit("ui.form.closed", snapshot);
    poll();
    CHECK_FALSE(workbench.idle());
    channel->reply({{"operationId", "late"}});
    poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.cancel");
    channel->reply(nullptr);
    poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
    CHECK(workbench.take_errors().empty());
    CHECK(std::none_of(host.updates.begin(), host.updates.end(), [](const auto& update) {
        return update.contains("document");
    }));
    workbench.execute("ui.find", Json::object(), {});
    poll();
    REQUIRE(host.form);
    CHECK(host.form->state().at("values").at("query") == "needle");
}

TEST_CASE("workbench inline search reports service errors in the current component") {
    auto channel = std::make_shared<Channel>();
    InlineHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules());
    const auto poll = [&] {
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
    };
    workbench.execute("ui.find", Json::object(), {});
    poll();
    REQUIRE(host.form);
    REQUIRE(host.form->edit(astra::editing::Command::insert_text, {.text = "("}));
    auto snapshot = host.form->state();
    workbench.emit("ui.form.changed", snapshot);
    snapshot["action"] = "next";
    workbench.emit("ui.form.action", snapshot);
    poll();
    REQUIRE(channel->requests.size() == 1);
    auto incoming = std::move(channel->requests.front());
    channel->requests.pop_front();
    incoming.response.set_value(
        Json{{"jsonrpc", "2.0"},
             {"id", incoming.body.at("id")},
             {"error", {{"code", -32602}, {"message", "Invalid expression"}}}}
            .dump());
    poll();
    CHECK(host.form->error());
    CHECK_FALSE(host.form->busy());
    CHECK(host.form->message() == "Invalid expression");
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench platform input preserves per-document order and captured read fences") {
    astra::ui::InputQueue queue;
    const auto first =
        queue.accept("one", "editor.insert_text", {{"id", "ignored"}, {"text", "A"}});
    const auto second = queue.accept("one", "editor.move_right", Json::object());
    const auto independent = queue.accept("two", "editor.insert_text", {{"text", "B"}});
    CHECK(queue.at(first).parameters.at("id") == "one");
    CHECK(queue.ready() == std::vector<astra::ui::InputQueue::Identity>{first, independent});
    CHECK_THROWS_AS(queue.started(second, 12), std::logic_error);
    queue.started(first, 11);
    CHECK(queue.ready() == std::vector<astra::ui::InputQueue::Identity>{independent});
    CHECK_THROWS_AS(queue.started(independent, 11), std::logic_error);
    queue.started(independent, 13);
    CHECK(queue.ready().empty());
    const astra::ui::InputQueue::Identity owners[]{11, 13};
    CHECK(queue.waiting_only(owners));
    CHECK_FALSE(queue.waiting_only(std::span(owners, 1)));
    const auto external = queue.fence("one", 0);
    CHECK(queue.pending("one", external));
    CHECK_FALSE(queue.pending("one", queue.fence("one", 11)));
    CHECK_FALSE(queue.pending("two", queue.fence("two", 13)));
    // Reading another document must still fence its earlier accepted inputs.
    CHECK(queue.pending("one", queue.fence("one", 13)));
    const auto later = queue.accept("one", "editor.move_left", Json::object());
    queue.retire(first);
    CHECK_FALSE(queue.waiting_only(owners));
    CHECK(queue.ready() == std::vector<astra::ui::InputQueue::Identity>{second});
    queue.started(second, 12);
    CHECK_FALSE(queue.pending("one", queue.fence("one", 12)));
    queue.retire(second);
    CHECK_FALSE(queue.pending("one", external));
    CHECK(queue.pending("one", queue.fence("one", 0)));
    CHECK(queue.ready() == std::vector<astra::ui::InputQueue::Identity>{later});
    queue.retire(independent);
    queue.retire(later);
    queue.retire(first);
    CHECK(queue.empty());
}

TEST_CASE("workbench platform input rejects invalid capacity atomically and releases bytes") {
    astra::ui::InputQueue queue;
    CHECK_THROWS_AS(static_cast<void>(queue.accept("", "editor.insert_text", Json::object())),
                    astra::protocol::RpcError);
    CHECK_THROWS_AS(static_cast<void>(queue.accept("one", "editor.insert_text", Json::array())),
                    astra::protocol::RpcError);
    CHECK(queue.empty());
    for (unsigned i = 0; i < 256; ++i)
        static_cast<void>(queue.accept("one", "editor.insert_text", {{"text", i}}));
    const auto before = queue.fence("one", 0);
    CHECK_THROWS_AS(static_cast<void>(queue.accept("one", "editor.insert_text", Json::object())),
                    astra::protocol::RpcError);
    CHECK(queue.fence("one", 0) == before);
    queue.clear();
    CHECK(queue.empty());
    CHECK_FALSE(queue.pending("one", before));
    const auto large =
        queue.accept("one",
                     "editor.insert_text",
                     {{"text", std::string(astra::protocol::Dispatcher::max_body_bytes / 2, 'a')}});
    CHECK_THROWS_AS(
        static_cast<void>(queue.accept(
            "one",
            "editor.insert_text",
            {{"text", std::string(astra::protocol::Dispatcher::max_body_bytes / 2, 'b')}})),
        astra::protocol::RpcError);
    CHECK(queue.ready() == std::vector<astra::ui::InputQueue::Identity>{large});
    queue.retire(large);
    const auto next =
        queue.accept("two",
                     "editor.insert_text",
                     {{"text", std::string(astra::protocol::Dispatcher::max_body_bytes / 2, 'b')}});
    CHECK(next > large);
    CHECK(queue.at(next).document == "two");
}

TEST_CASE("workbench default client editing commands are registered and overridable") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> actions;
    std::vector<astra::script::Runtime::TaskId> owners;
    astra::ui::Workbench* owner{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.action.cancel")
                return ready(nullptr);
            CHECK(method == "_ui.action");
            actions.push_back(params);
            owners.push_back(owner->request_owner());
            return ready(params.at("parameters"));
        },
        modules(),
        R"js(
            astra.registerCommand('editor.move_down', async parameters => {
                await astra.sleep(0);
                return await astra.ui.native('editor.move_up', parameters);
            });
            astra.registerCommand('nestedInput', async parameters => {
                await astra.sleep(0);
                return await astra.ui.execute('editor.insert_text', parameters);
            });
        )js");
    owner = &workbench;
    for (const auto* command : {"editor.insert_text",
                                "editor.move_up",
                                "editor.move_down",
                                "editor.select_word",
                                "editor.backspace",
                                "editor.indent"})
        CHECK(workbench.contains(command));
    std::optional<astra::script::Runtime::Outcome> inserted, moved;
    const auto input = workbench.execute(
        "nestedInput", {{"id", "captured"}, {"text", "中🙂"}}, [&](const auto& result) {
            inserted = result;
        });
    const auto movement = workbench.execute(
        "editor.move_down", {{"id", "other"}, {"extend", true}}, [&](const auto& result) {
            moved = result;
        });
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(inserted);
    REQUIRE(moved);
    CHECK_FALSE(inserted->error);
    CHECK_FALSE(moved->error);
    CHECK(inserted->value == Json{{"id", "captured"}, {"text", "中🙂"}});
    CHECK(moved->value == Json{{"id", "other"}, {"extend", true}});
    REQUIRE(actions.size() == 2);
    for (std::size_t i = 0; i < actions.size(); ++i) {
        if (actions[i].at("command") == "editor.insert_text")
            CHECK(owners[i] == input);
        else {
            CHECK(actions[i].at("command") == "editor.move_up");
            CHECK(owners[i] == movement);
        }
    }
    CHECK(workbench.request_owner() == 0);
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench editing awaits native completion and preserves parameters and errors") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> completion;
    Json action;
    int drains{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.action") {
                action = params;
                return completion.get_future();
            }
            REQUIRE(method == "_ui.action.cancel");
            ++drains;
            CHECK(params.at("actionId") == action.at("actionId"));
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute(
        "editor.undo", {{"id", "captured"}, {"baseRevision", "7"}}, [&](const auto& value) {
            outcome = value;
        });
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    REQUIRE(action.at("command") == "editor.undo");
    CHECK(action.at("parameters") == Json{{"id", "captured"}, {"baseRevision", "7"}});
    CHECK_FALSE(outcome);
    CHECK_FALSE(workbench.idle());
    CHECK_FALSE(workbench.awaiting_input());
    completion.set_exception(std::make_exception_ptr(
        astra::protocol::RpcError(-32001, "Stale revision", Json{{"revision", "8"}})));
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("code") == -32001);
    CHECK(outcome->error->at("data").at("revision") == "8");
    CHECK(drains == 1);
    CHECK(workbench.idle());
}

TEST_CASE("workbench editing cancellation drains the deferred native future") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> completion;
    bool submitted{}, cancelled{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.action") {
                submitted = true;
                return completion.get_future();
            }
            REQUIRE(method == "_ui.action.cancel");
            cancelled = true;
            completion.set_value(nullptr);
            return ready(nullptr);
        },
        modules(),
        "let control=new AbortController(); "
        "astra.registerCommand('edit',()=>astra.ui.native('editor.insert_text',{text:'中🙂'},{"
        "signal:control.signal})); astra.registerCommand('abort',()=>control.abort());");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("edit", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 3; ++tick)
        workbench.poll();
    REQUIRE(submitted);
    CHECK_FALSE(outcome);
    workbench.execute("abort", Json::object(), {});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(cancelled);
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("name") == "AbortError");
    CHECK(workbench.idle());
}

TEST_CASE(
    "workbench document reads await a native fence while immediate presentation reads proceed") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> completion;
    Json captured;
    unsigned immediate{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            REQUIRE(method == "_ui.read");
            if (params.value("immediate", false)) {
                ++immediate;
                return ready({{"sidebarVisible", true}});
            }
            captured = params;
            return completion.get_future();
        },
        modules(),
        "astra.registerCommand('ordered',()=>astra.ui.read({id:'captured'}));astra.registerCommand("
        "'instant',()=>astra.ui.read({immediate:true}));");
    std::optional<astra::script::Runtime::Outcome> ordered, instant;
    workbench.execute("ordered", Json::object(), [&](const auto& value) {
        ordered = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(captured.at("id") == "captured");
    CHECK(captured.at("readId").is_string());
    CHECK_FALSE(ordered);
    CHECK_FALSE(workbench.awaiting_input());
    workbench.execute("instant", Json::object(), [&](const auto& value) {
        instant = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(instant);
    CHECK_FALSE(instant->error);
    CHECK(immediate == 1);
    CHECK_FALSE(ordered);
    completion.set_value({{"document", {{"id", "captured"}, {"revision", "9"}}}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(ordered);
    CHECK_FALSE(ordered->error);
    CHECK(ordered->value.at("document").at("revision") == "9");
    CHECK(workbench.idle());
}

TEST_CASE("workbench cancelling a document read drains its native fence") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> completion;
    Json captured;
    unsigned cancellations{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read") {
                captured = params;
                return completion.get_future();
            }
            REQUIRE(method == "_ui.read.cancel");
            CHECK(params.at("readId") == captured.at("readId"));
            ++cancellations;
            completion.set_value(nullptr);
            return ready(nullptr);
        },
        modules(),
        "let control=new "
        "AbortController();astra.registerCommand('ordered',()=>astra.ui.read({id:'captured'},{"
        "signal:control.signal}));astra.registerCommand('abort',()=>control.abort());");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ordered", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK_FALSE(outcome);
    workbench.execute("abort", Json::object(), {});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("name") == "AbortError");
    CHECK(cancellations == 1);
    CHECK(workbench.idle());
}

TEST_CASE("workbench provider generation prevents older results replacing newer views") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ViewStore views;
    std::size_t publications = 0;
    const auto source =
        "astra.ui.registerView({id:'explorer',title:'Files'},(p,o)=>astra.request('files',p,o)); "
        "astra.registerCommand('refresh',p=>astra.ui.refresh('explorer',p));";
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) -> std::future<Json> {
            if (method == "_ui.view.begin")
                return ready(views.begin(params.at("spec")));
            if (method == "_ui.view.append")
                return ready(views.append(params.at("id").get<std::string>(),
                                          params.at("version").get<std::string>(),
                                          params.at("rows")));
            if (method == "_ui.view.abort")
                return ready(views.abort(params.at("id").get<std::string>(),
                                         params.at("version").get<std::string>()));
            if (method == "_ui.view.commit") {
                ++publications;
                return views.commit(params.at("id").get<std::string>(),
                                    params.at("version").get<std::string>(),
                                    "",
                                    "Empty");
            }
            return ready(nullptr);
        },
        modules(),
        source);
    workbench.execute("refresh", {{"generation", 1}}, {});
    workbench.poll();
    workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    workbench.execute("refresh", {{"generation", 2}}, {});
    workbench.poll();
    workbench.poll();
    REQUIRE(channel->requests.size() == 2);
    channel->reply({{"rows", Json::array({{{"id", "old"}, {"label", "Old"}}})}});
    channel->reply({{"rows", Json::array({{{"id", "new"}, {"label", "New"}}})}});
    for (int tick = 0; tick < 30; ++tick) {
        views.poll();
        workbench.poll();
    }
    REQUIRE(publications == 1);
    REQUIRE(views.at("explorer").index.size() == 1);
    CHECK(views.at("explorer").index.at(0).value->at("id") == "new");
    CHECK(views.idle());
    CHECK(workbench.idle());
}

TEST_CASE("workbench custom views publish bounded Unicode pages and guard row action versions") {
    auto channel = std::make_shared<Channel>();
    ViewHost host;
    const auto source = R"js(
        const descriptor = {id:'custom',title:'Custom',icon:'symbol'};
        astra.ui.registerView(descriptor, async () => ({
            rows:Array.from({length:500}, (_,i) => ({
                id:String(i), label:'中🙂'.repeat(100), data:{value:i}
            }))
        }), {activate:data => astra.request('edit',data)});
        descriptor.id = 'mutated-after-registration';
        descriptor.title = '';
        astra.registerCommand('publish', () => astra.ui.refresh('custom'));
        astra.registerCommand('invalid', async () => {
            try {
                await astra.ui.renderView('custom',{rows:[{id:'bad',label:'Bad',depth:1}]});
            } catch (error) { return true; }
            return false;
        });
    )js";
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules(),
        source);
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("publish", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 300 && !outcome; ++tick) {
        host.views.poll();
        workbench.poll();
    }
    REQUIRE(outcome);
    REQUIRE_FALSE(outcome->error);
    CHECK(outcome->value.get<bool>());
    REQUIRE(host.views.at("custom").index.size() == 500);
    REQUIRE(host.pages.size() > 5);
    for (const auto& page : host.pages) {
        CHECK(page.size() <= 128);
        CHECK(page.dump().size() <= 60000);
    }
    const auto version = host.views.at("custom").version;
    const auto row = *host.views.at("custom").index.at(17).value;
    workbench.emit("ui.view.action", {{"id", "custom"}, {"version", "obsolete"}, {"row", row}});
    for (int tick = 0; tick < 10; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    workbench.emit("ui.view.action", {{"id", "custom"}, {"version", version}, {"row", row}});
    for (int tick = 0; tick < 10; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "edit");
    CHECK(channel->requests.front().body.at("params").at("value") == 17);
    channel->reply(nullptr);
    outcome.reset();
    workbench.execute("invalid", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 100 && !outcome; ++tick) {
        host.views.poll();
        workbench.poll();
    }
    REQUIRE(outcome);
    REQUIRE_FALSE(outcome->error);
    CHECK(outcome->value.get<bool>());
    CHECK(host.views.at("custom").version == version);
    CHECK(host.views.at("custom").index.size() == 500);
    CHECK(host.views.idle());
    CHECK(workbench.idle());
}

TEST_CASE("workbench async view controls share commands and guard activation versions") {
    auto channel = std::make_shared<Channel>();
    ViewHost host;
    const auto source = R"js(
        let activated;
        astra.ui.registerView({id:'controls',title:'Controls'}, async () => ({
            rows:Array.from({length:40}, (_,i)=>({id:String(i),label:'Row '+i,data:{index:i}}))
        }), {activate:async data=> { await astra.sleep(0); activated=data.index; }});
        astra.registerCommand('exercise', async () => {
            await astra.ui.refresh('controls');
            const pending=astra.ui.views.read('controls');
            if(typeof pending.then!=='function') throw Error('View reads must be asynchronous');
            await pending;
            await astra.ui.views.select('controls','17');
            await astra.ui.execute('ui.view.next',{id:'controls'});
            await astra.ui.execute('ui.view.activate',{id:'controls'});
            const selected=await astra.ui.views.read('controls');
            const stale=await astra.ui.views.navigate('controls','activate',{version:'obsolete'});
            astra.ui.registerCommand({id:'ui.view.next',title:'Custom next',palette:false},
                async ({id})=>astra.ui.views.select(id,'25'));
            await astra.ui.execute('ui.view.next',{id:'controls'});
            return {activated,selected,stale,override:await astra.ui.views.read('controls')};
        });
    )js";
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules(),
        source);
    std::optional<astra::script::Runtime::Outcome> result;
    workbench.execute("exercise", Json::object(), [&](const auto& outcome) {
        result = outcome;
    });
    for (int tick = 0; tick < 100 && !result; ++tick) {
        host.views.poll();
        workbench.poll();
    }
    REQUIRE(result);
    REQUIRE_FALSE(result->error);
    CHECK(result->value.at("activated") == 18);
    CHECK(result->value.at("selected").at("row").at("id") == "18");
    CHECK(result->value.at("selected").at("scroll") == 11);
    CHECK(result->value.at("stale").is_null());
    CHECK(result->value.at("override").at("row").at("id") == "25");
    CHECK(workbench.idle());
}

TEST_CASE("workbench view cancellation releases an unresolved native publication fence") {
    auto channel = std::make_shared<Channel>();
    ViewHost host;
    const auto source = R"js(
        astra.ui.registerView({id:'custom',title:'Custom'}, async () => ({
            rows:[{id:'row',label:'Row'}]
        }));
        astra.registerCommand('publish',() => astra.ui.refresh('custom'));
        astra.registerCommand('cancel',() => astra.ui.cancel('custom'));
    )js";
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules(),
        source);
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("publish", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 30; ++tick)
        workbench.poll();
    REQUIRE_FALSE(outcome);
    REQUIRE_FALSE(host.views.idle());
    REQUIRE(host.views.at("custom").index.size() == 0);
    workbench.execute("cancel", Json::object(), {});
    for (int tick = 0; tick < 100 && !workbench.idle(); ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(host.views.idle());
    CHECK(host.views.at("custom").index.size() == 0);
    CHECK(workbench.idle());
}

TEST_CASE("workbench clipboard pages Unicode bytes and cuts one captured transaction") {
    auto channel = std::make_shared<Channel>();
    std::string clipboard;
    const Json document{{"id", "original"},
                        {"revision", "7"},
                        {"selections",
                         Json::array({{{"anchor", 7}, {"head", 0}},
                                      {{"anchor", 10}, {"head", 13}},
                                      {{"anchor", 0}, {"head", 7}}})}};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document", document}, {"selectionLines", Json::array()}});
            if (method == "_ui.clipboard.write")
                clipboard = params.at("text").get<std::string>();
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.cut", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("params").at("begin") == 0);
    channel->reply({{"revision", "7"}, {"range", {{"end", 3}}}, {"text", "中"}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("params").at("begin") == 3);
    channel->reply({{"revision", "7"}, {"range", {{"end", 7}}}, {"text", "🙂"}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("params").at("begin") == 10);
    channel->reply({{"revision", "7"}, {"range", {{"end", 13}}}, {"text", "e\xCC\x81"}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(clipboard == "中🙂\ne\xCC\x81");
    REQUIRE(channel->requests.size() == 1);
    const auto& edit = channel->requests.front().body;
    CHECK(edit.at("method") == "document.edit");
    CHECK(edit.at("params").at("id") == "original");
    CHECK(edit.at("params").at("baseRevision") == "7");
    CHECK(edit.at("params").at("edits") ==
          Json::array({{{"begin", 0}, {"end", 7}, {"text", ""}},
                       {{"begin", 10}, {"end", 13}, {"text", ""}}}));
    channel->reply({{"id", "original"}, {"revision", "8"}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(workbench.idle());
}

TEST_CASE("workbench stale clipboard read never writes or cuts") {
    auto channel = std::make_shared<Channel>();
    bool wrote{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.read")
                return ready({{"document",
                               {{"id", "1"},
                                {"revision", "2"},
                                {"selections", Json::array({{{"anchor", 0}, {"head", 4}}})}}}});
            if (method == "_ui.clipboard.write")
                wrote = true;
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.cut", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    channel->reply({{"revision", "3"}, {"range", {{"end", 4}}}, {"text", "text"}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK_FALSE(wrote);
    CHECK(channel->requests.empty());
    CHECK(outcome->error->at("message").get<std::string>().find("changed while copying") !=
          std::string::npos);
}

TEST_CASE("workbench copy obtains revision checked line bounds when the native viewport is old") {
    auto channel = std::make_shared<Channel>();
    std::string clipboard;
    const auto prefix = std::string(65534, 'x');
    const auto suffix = std::string("🙂\r\n");
    const auto end = prefix.size() + suffix.size();
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready(
                    {{"document",
                      {{"id", "source"},
                       {"revision", "7"},
                       {"selections",
                        Json::array(
                            {{{"anchor", 0}, {"head", 0}}, {{"anchor", 1000}, {"head", 1000}}})},
                       {"selectionPositions", Json::array({{{"line", 0}}, {{"line", 0}}})}}},
                     {"selectionLines", Json::array({nullptr, nullptr})}});
            if (method == "_ui.clipboard.write")
                clipboard = params.at("text").get<std::string>();
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.copy", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.lines");
    CHECK(channel->requests.front().body.at("params") ==
          Json{{"id", "source"}, {"firstLine", 0}, {"count", 1}});
    SUBCASE("partial viewport rows still copy the entire logical line once") {
        channel->reply({{"revision", "7"},
                        {"lines",
                         Json::array({{{"line", 0},
                                       {"begin", 0},
                                       {"end", prefix.size()},
                                       {"nextBegin", end},
                                       {"complete", false}}})}});
        for (int tick = 0; tick < 4; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "document.get");
        CHECK(channel->requests.front().body.at("params").at("end") == end);
        channel->reply({{"revision", "7"}, {"range", {{"end", prefix.size()}}}, {"text", prefix}});
        for (int tick = 0; tick < 4; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("params").at("begin") == prefix.size());
        channel->reply({{"revision", "7"}, {"range", {{"end", end}}}, {"text", suffix}});
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(clipboard == prefix + suffix);
        CHECK(workbench.idle());
    }
    SUBCASE("a changed line revision prevents clipboard publication") {
        channel->reply({{"revision", "8"}, {"lines", Json::array()}});
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE(outcome->error);
        CHECK(clipboard.empty());
        CHECK(channel->requests.empty());
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench paste captures document identity and normalizes CRLF") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> clipboard;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.read")
                return ready(
                    {{"document", {{"id", "captured"}, {"revision", "9"}}}, {"newline", "\r\n"}});
            if (method == "_ui.clipboard.read")
                return clipboard.get_future();
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.paste", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK_FALSE(outcome);
    CHECK(channel->requests.empty());
    clipboard.set_value("中\r\n🙂\re\xCC\x81\n");
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    const auto& request = channel->requests.front().body;
    CHECK(request.at("method") == "script.command");
    CHECK(request.at("params").at("params").at("id") == "captured");
    CHECK(request.at("params").at("params").at("baseRevision") == "9");
    CHECK(request.at("params").at("params").at("text") == "中\r\n🙂\r\ne\xCC\x81\r\n");
    channel->reply({{"id", "captured"}, {"revision", "10"}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(workbench.idle());
}

TEST_CASE("workbench modal input awaits the user without hiding unrelated work") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> input;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.dialog")
                return input.get_future();
            return ready(nullptr);
        },
        modules(),
        "astra.registerCommand('background',()=>astra.request('other')); "
        "astra.registerCommand('prompt',()=>astra.ui.input({title:'Value'}));");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("prompt", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK_FALSE(workbench.idle());
    CHECK(workbench.awaiting_input());
    CHECK_FALSE(outcome);
    workbench.execute("background", Json::object(), {});
    workbench.poll();
    CHECK_FALSE(workbench.awaiting_input());
    channel->reply(nullptr);
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(workbench.awaiting_input());
    input.set_value("chosen");
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == "chosen");
    CHECK(workbench.idle());
}

TEST_CASE("workbench input cancellation dismisses its native dialog and drains the late future") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> input;
    unsigned cancelled{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.dialog")
                return input.get_future();
            if (method == "_ui.dialog.cancel") {
                ++cancelled;
                input.set_value(nullptr);
            }
            return ready(nullptr);
        },
        modules(),
        "let control=new AbortController(); "
        "astra.registerCommand('prompt',()=>astra.ui.input({title:'Value'},{signal:control.signal})"
        "); astra.registerCommand('abort',()=>control.abort());");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("prompt", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(workbench.awaiting_input());
    workbench.execute("abort", Json::object(), {});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("name") == "AbortError");
    CHECK(cancelled == 1);
    CHECK(workbench.idle());
}

TEST_CASE("workbench reload confirmation captures a revision before awaiting input") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> confirmation;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.read")
                return ready({{"document",
                               {{"id", "capture"},
                                {"revision", "42"},
                                {"dirty", true},
                                {"path", "source.cpp"}}}});
            if (method == "_ui.dialog")
                return confirmation.get_future();
            return ready(nullptr);
        },
        modules());
    workbench.execute("ui.reload", Json::object(), {});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.awaiting_input());
    confirmation.set_value(true);
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.reload");
    CHECK(channel->requests.front().body.at("params") ==
          Json{{"id", "capture"}, {"baseRevision", "42"}, {"discardChanges", true}});
    channel->reply({{"id", "capture"}, {"revision", "43"}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(workbench.idle());
}

TEST_CASE(
    "workbench close owns captured revision decisions and retains edits arriving during save") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> choice;
    std::vector<Json> updates;
    Json spec;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read") {
                CHECK(params.at("id") == "captured");
                return ready({{"document",
                               {{"id", "captured"},
                                {"revision", "7"},
                                {"dirty", true},
                                {"path", "source.cpp"}}}});
            }
            if (method == "_ui.dialog") {
                spec = params;
                return choice.get_future();
            }
            if (method == "_ui.update")
                updates.push_back(params);
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.closeTab", {{"id", "captured"}}, [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    CHECK(workbench.awaiting_input());
    CHECK(channel->requests.empty());
    CHECK_FALSE(outcome);
    CHECK(spec.at("kind") == "choice");
    CHECK(spec.at("items") == Json::array({"Save", "Don't save", "Cancel"}));
    const auto no_close = [&] {
        CHECK(std::ranges::none_of(updates, [](const Json& update) {
            return update.contains("closedDocument");
        }));
    };
    SUBCASE("cancellation never closes or writes") {
        choice.set_value(nullptr);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == false);
        CHECK(channel->requests.empty());
        no_close();
    }
    SUBCASE("discard uses the revision reviewed before the choice") {
        choice.set_value(1);
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "document.close");
        CHECK(channel->requests.front().body.at("params") ==
              Json{{"id", "captured"}, {"baseRevision", "7"}, {"discardChanges", true}});
        channel->reply(true);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == true);
        CHECK(updates.back().at("closedDocument") == "captured");
    }
    SUBCASE("a changed revision rejects discard without removing the tab") {
        choice.set_value(1);
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        auto incoming = std::move(channel->requests.front());
        channel->requests.pop_front();
        incoming.response.set_value(
            Json{{"jsonrpc", "2.0"},
                 {"id", incoming.body.at("id")},
                 {"error", {{"code", -32001}, {"message", "Stale revision"}}}}
                .dump());
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE(outcome->error);
        CHECK(outcome->error->at("code") == -32001);
        no_close();
    }
    SUBCASE("save uses its resulting revision including formatting") {
        choice.set_value(0);
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "workspace.save");
        CHECK(channel->requests.front().body.at("params") ==
              Json{{"id", "captured"}, {"baseRevision", "7"}});
        channel->reply(
            {{"id", "captured"}, {"revision", "8"}, {"dirty", false}, {"path", "source.cpp"}});
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "workspace.tree");
        channel->reply({{"entries", Json::array()}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "document.close");
        CHECK(channel->requests.front().body.at("params") ==
              Json{{"id", "captured"}, {"baseRevision", "8"}, {"discardChanges", false}});
        channel->reply(true);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == true);
    }
    SUBCASE("new dirty edits during save keep the buffer open") {
        choice.set_value(0);
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "workspace.save");
        channel->reply(
            {{"id", "captured"}, {"revision", "8"}, {"dirty", true}, {"path", "source.cpp"}});
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "workspace.tree");
        channel->reply({{"entries", Json::array()}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == false);
        CHECK(channel->requests.empty());
        no_close();
    }
    CHECK(workbench.idle());
}

TEST_CASE("workbench untitled close retains the buffer when Save As is dismissed") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> path;
    unsigned inputs{};
    bool removed{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready(
                    {{"document",
                      {{"id", "untitled"}, {"revision", "3"}, {"dirty", true}, {"path", ""}}}});
            if (method == "_ui.dialog") {
                if (params.at("kind") == "choice")
                    return ready(0);
                CHECK(params.at("kind") == "input");
                ++inputs;
                return path.get_future();
            }
            if (method == "_ui.update" && params.contains("closedDocument"))
                removed = true;
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.closeTab", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(inputs == 1);
    CHECK(workbench.awaiting_input());
    CHECK(channel->requests.empty());
    path.set_value(nullptr);
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == false);
    CHECK_FALSE(removed);
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench closing composes the current user save command") {
    auto channel = std::make_shared<Channel>();
    bool removed{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document",
                               {{"id", "captured"},
                                {"revision", "7"},
                                {"dirty", true},
                                {"path", "source.cpp"}}}});
            if (method == "_ui.dialog")
                return ready(0);
            if (method == "_ui.update" && params.contains("closedDocument"))
                removed = true;
            return ready(nullptr);
        },
        modules(),
        "astra.registerCommand('ui.save',async parameters=>await "
        "astra.request('custom.save',parameters));");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.closeTab", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "custom.save");
    CHECK(channel->requests.front().body.at("params").at("id") == "captured");
    CHECK_FALSE(outcome);
    CHECK_FALSE(removed);
    channel->reply({{"id", "captured"}, {"revision", "9"}, {"dirty", false}});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.close");
    CHECK(channel->requests.front().body.at("params").at("baseRevision") == "9");
    channel->reply(true);
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == true);
    CHECK(removed);
    CHECK(workbench.idle());
}

TEST_CASE("workbench named choices correlate answers and reject duplicate active identities") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> choice;
    Json spec;
    unsigned shown{}, answered{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.dialog") {
                ++shown;
                spec = params;
                return choice.get_future();
            }
            if (method == "_ui.dialog.answer") {
                ++answered;
                CHECK(params.at("id") == spec.at("id"));
                choice.set_value(params.at("index"));
            }
            return ready(nullptr);
        },
        modules(),
        "astra.registerCommand('choose',()=>astra.ui.choose({id:'named',title:'Choice',choices:[{"
        "label:'First',value:1},{label:'中🙂',value:2,retained:'original'}]}));astra."
        "registerCommand('answer',p=>astra.ui.answerChoice(p.id,p.index));");
    std::optional<astra::script::Runtime::Outcome> result, duplicate;
    workbench.execute("choose", Json::object(), [&](const auto& value) {
        result = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(shown == 1);
    CHECK_FALSE(result);
    workbench.execute("choose", Json::object(), [&](const auto& value) {
        duplicate = value;
    });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(duplicate);
    REQUIRE(duplicate->error);
    CHECK(shown == 1);
    CHECK_FALSE(result);
    workbench.execute("answer", {{"id", "other"}, {"index", 0}}, {});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(answered == 0);
    CHECK_FALSE(result);
    workbench.execute("answer", {{"id", "named"}, {"index", 1}}, {});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(result);
    CHECK_FALSE(result->error);
    CHECK(result->value == Json{{"label", "中🙂"}, {"value", 2}, {"retained", "original"}});
    CHECK(answered == 1);
    CHECK(workbench.idle());
    workbench.execute("answer", {{"id", "named"}, {"index", 0}}, {});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(answered == 1);
    CHECK(workbench.idle());
}

TEST_CASE("workbench recovery waits for selection and confirmation of the captured session") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> picker, confirmation;
    Json confirm_spec;
    std::vector<Json> updates;
    unsigned dialogs{}, answers{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.update") {
                updates.push_back(params);
                return ready(nullptr);
            }
            if (method == "_ui.dialog") {
                ++dialogs;
                if (params.at("kind") == "pick") {
                    REQUIRE(params.at("items").size() == 1);
                    CHECK(params.at("items")[0].get<std::string>().find("中🙂.txt") !=
                          std::string::npos);
                    return picker.get_future();
                }
                CHECK(params.at("kind") == "confirm");
                confirm_spec = params;
                return confirmation.get_future();
            }
            if (method == "_ui.dialog.answer") {
                ++answers;
                CHECK(params.at("id") == confirm_spec.at("id"));
                confirmation.set_value(params.at("accepted"));
            }
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.confirmRecovery", Json::object(), {});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(answers == 0);
    workbench.execute("ui.recovery", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "recovery.status");
    channel->reply({{"enabled", true}, {"ready", true}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "recovery.list");
    channel->reply(
        {{"sessions",
          Json::array(
              {{{"id", "active"}, {"active", true}, {"buffers", Json::array({{{"id", "1"}}})}},
               {{"id", "empty"}, {"active", false}},
               {{"id", "captured"},
                {"active", false},
                {"buffers", Json::array({{{"path", "中🙂.txt"}, {"bytes", 27}}})}}})}});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(dialogs == 1);
    CHECK_FALSE(outcome);
    CHECK(channel->requests.empty());
    CHECK(workbench.awaiting_input());
    picker.set_value(0);
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(dialogs == 2);
    CHECK(confirm_spec.at("items")[0] == "中🙂.txt (27 bytes)");
    CHECK(channel->requests.empty());
    // New discovery cannot change the session selected by the open confirmation.
    workbench.emit(
        "recovery.available",
        {{"sessions", Json::array({{{"id", "new"}, {"buffers", Json::array({{{"id", "2"}}})}}})}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    SUBCASE("cancel leaves all documents untouched") {
        confirmation.set_value(nullptr);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(channel->requests.empty());
    }
    SUBCASE("accepted recovery awaits real restore before publishing documents") {
        workbench.execute("ui.confirmRecovery", Json::object(), {});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        CHECK(answers == 1);
        CHECK_FALSE(outcome);
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "recovery.restore");
        CHECK(channel->requests.front().body.at("params") == Json{{"sessionId", "captured"}});
        CHECK_FALSE(std::ranges::any_of(updates, [](const Json& update) {
            return update.contains("documents");
        }));
        channel->reply({{"documents",
                         Json::array({{{"id", "restored"}, {"revision", "9"}, {"dirty", true}}})}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "recovery.list");
        channel->reply({{"sessions", Json::array()}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        const auto restored = std::ranges::find_if(updates, [](const Json& update) {
            return update.contains("documents");
        });
        REQUIRE(restored != updates.end());
        CHECK(restored->at("activate") == "restored");
        CHECK(restored->at("documents")[0].at("dirty") == true);
        CHECK(updates.back().at("recoveryAvailable") == false);
    }
    CHECK(workbench.idle());
}

TEST_CASE("workbench recovery displays damaged sessions without attempting restore") {
    auto channel = std::make_shared<Channel>();
    Json shown;
    std::promise<Json> dismissal;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.dialog") {
                if (params.at("kind") == "pick")
                    return ready(0);
                CHECK(params.at("kind") == "text");
                shown = params;
                return dismissal.get_future();
            }
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.recovery", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    channel->reply({{"enabled", true}, {"ready", true}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    channel->reply({{"sessions",
                     Json::array({{{"id", "damaged"},
                                   {"issues",
                                    Json::array({{{"path", "checkpoint.json"},
                                                  {"message", "checksum mismatch"}}})}}})}});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(shown.at("text") == "Issue: checkpoint.json — checksum mismatch");
    CHECK(channel->requests.empty());
    CHECK_FALSE(outcome);
    CHECK(workbench.awaiting_input());
    dismissal.set_value(nullptr);
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench recovery superseded discovery cannot publish older availability") {
    auto channel = std::make_shared<Channel>();
    std::vector<bool> availability;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view, const Json& params) {
            if (params.contains("recoveryAvailable"))
                availability.push_back(params.at("recoveryAvailable"));
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> older, newer;
    workbench.execute("workbench.recoveryScan", Json::object(), [&](const auto& value) {
        older = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    workbench.execute("workbench.recoveryScan", Json::object(), [&](const auto& value) {
        newer = value;
    });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 2);
    channel->reply({{"enabled", false}, {"ready", true}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    CHECK(availability.empty());
    channel->reply({{"enabled", true}, {"ready", true}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "recovery.list");
    channel->reply(
        {{"sessions",
          Json::array({{{"id", "current"}, {"buffers", Json::array({{{"id", "2"}}})}}})}});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(older);
    CHECK_FALSE(older->error);
    REQUIRE(newer);
    CHECK_FALSE(newer->error);
    CHECK(availability == std::vector<bool>{true});
    CHECK(workbench.idle());
}

TEST_CASE("workbench recovery availability notifications supersede a pending list") {
    auto channel = std::make_shared<Channel>();
    std::vector<bool> availability;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view, const Json& params) {
            if (params.contains("recoveryAvailable"))
                availability.push_back(params.at("recoveryAvailable"));
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("workbench.recoveryScan", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    channel->reply({{"enabled", true}, {"ready", true}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    workbench.emit(
        "recovery.available",
        {{"sessions",
          Json::array({{{"id", "announced"}, {"buffers", Json::array({{{"id", "2"}}})}}})}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    channel->reply({{"sessions", Json::array()}});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(availability == std::vector<bool>{true});
    CHECK(workbench.idle());
}

TEST_CASE("workbench recovery disabled and initialization failure do not list sessions") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view, const Json& params) {
            updates.push_back(params);
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("workbench.recoveryScan", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    SUBCASE("disabled") {
        channel->reply({{"enabled", false}, {"ready", false}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(updates.back().at("recoveryAvailable") == false);
    }
    SUBCASE("failed initialization") {
        channel->reply(
            {{"enabled", true}, {"ready", false}, {"error", "Recovery session lock denied"}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE(outcome->error);
        CHECK(outcome->error->at("message") == "Recovery: Recovery session lock denied");
        CHECK(updates.empty());
    }
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench recovery checkpoints await durable completion and propagate failure") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view, const Json& params) {
            updates.push_back(params);
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.checkpoint", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "recovery.flush");
    CHECK_FALSE(outcome);
    REQUIRE(updates.size() == 1);
    CHECK(updates[0].at("status") == "Writing recovery checkpoint...");
    SUBCASE("durable completion") {
        channel->reply({{"enabled", true}, {"durable", true}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value.at("durable") == true);
        CHECK(updates.back().at("status") == "Recovery checkpoint saved");
        CHECK(updates.back().at("error") == "");
    }
    SUBCASE("failure is never presented as saved") {
        auto incoming = std::move(channel->requests.front());
        channel->requests.pop_front();
        incoming.response.set_value(
            Json{{"jsonrpc", "2.0"},
                 {"id", incoming.body.at("id")},
                 {"error", {{"code", -32010}, {"message", "checkpoint storage failed"}}}}
                .dump());
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE(outcome->error);
        CHECK(outcome->error->at("code") == -32010);
        CHECK(updates.size() == 1);
    }
    CHECK(workbench.idle());
}

TEST_CASE("workbench recovery notifications publish availability and storage issues") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view, const Json& params) {
            updates.push_back(params);
            return ready(nullptr);
        },
        modules());
    workbench.emit(
        "recovery.available",
        {{"sessions",
          Json::array(
              {{{"id", "active"}, {"active", true}, {"buffers", Json::array({{{"id", "1"}}})}}})}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(updates.back().at("recoveryAvailable") == false);
    workbench.emit(
        "recovery.warning",
        {{"issues", Json::array({{{"path", "checkpoint.json"}, {"message", "cleanup failed"}}})}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(updates.back().at("error") == "checkpoint.json: cleanup failed");
    workbench.emit("recovery.error", {{"message", "disk full"}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(updates.back().at("error") == "Recovery checkpoint: disk full");
    CHECK(channel->requests.empty());
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench composed commands honor user replacements through awaited execution") {
    auto channel = std::make_shared<Channel>();
    astra::ui::Workbench workbench(
        channel,
        [](std::string_view, const Json&) {
            return ready(nullptr);
        },
        modules(),
        "astra.ui.registerCommand({id:'ui.saveAs',title:'User save'},async p=>{await "
        "astra.sleep(0);return "
        "p.value+1;});astra.ui.registerCommand({id:'composed',title:'Composed'},()=>astra.ui."
        "execute('ui.saveAs',{value:41}));");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("composed", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == 42);
    CHECK(channel->requests.empty());
}

TEST_CASE("workbench composed commands honor raw runtime replacements") {
    auto channel = std::make_shared<Channel>();
    astra::ui::Workbench workbench(
        channel,
        [](std::string_view, const Json&) {
            return ready(nullptr);
        },
        modules(),
        "astra.registerCommand('ui.saveAs',async p=>{await astra.sleep(0);return "
        "p.value+1;});astra.registerCommand('composed',()=>astra.ui.execute('ui.saveAs',{value:41})"
        ");");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("composed", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == 42);
    CHECK(channel->requests.empty());
}

TEST_CASE(
    "workbench command palette merges registries and awaits selected client or service commands") {
    auto channel = std::make_shared<Channel>();
    std::optional<std::promise<Json>> selection;
    Json items;
    Json published;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"serviceCommands", Json::array({"editor.undo", "remote.action"})},
                              {"document", {{"id", "captured"}, {"revision", "9"}}}});
            if (method == "_ui.dialog") {
                CHECK(params.at("kind") == "pick");
                items = params.at("items");
                selection.emplace();
                return selection->get_future();
            }
            if (method == "_ui.update")
                published = params;
            else
                REQUIRE(method == "_ui.dialog.cancel");
            return ready(nullptr);
        },
        modules(),
        "astra.ui.registerCommand({id:'client.action',title:'Choose "
        "中🙂',shortcut:'F9'},()=>astra.request('deferred.client')); "
        "astra.ui.registerCommand({id:'workbench.hidden',title:'Hidden internal command'},()=>0);");
    std::optional<astra::script::Runtime::Outcome> outcome;
    const auto choose = [&](std::string_view label) {
        const auto found = std::ranges::find_if(items, [&](const Json& item) {
            return item.get<std::string>().starts_with(label);
        });
        REQUIRE(found != items.end());
        selection->set_value(static_cast<std::size_t>(found - items.begin()));
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
    };
    workbench.execute("ui.commandPalette", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    REQUIRE(selection);
    CHECK(workbench.awaiting_input());
    CHECK_FALSE(outcome);
    CHECK(channel->requests.empty());
    CHECK(std::ranges::count(items, Json("Undo    Ctrl+Z")) == 1);
    CHECK(std::ranges::none_of(items, [](const Json& item) {
        return item.get<std::string>().starts_with("Hidden internal");
    }));
    choose("Choose 中🙂");
    REQUIRE(channel->requests.size() == 1);
    CHECK_FALSE(outcome);
    CHECK(channel->requests.front().body.at("method") == "deferred.client");
    channel->reply(42);
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == 42);
    outcome.reset();
    workbench.execute("ui.commandPalette", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    choose("remote.action");
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "script.command");
    CHECK(channel->requests.front().body.at("params") ==
          Json{{"name", "remote.action"}, {"params", {{"id", "captured"}, {"baseRevision", "9"}}}});
    channel->reply({{"id", "captured"}, {"revision", "10"}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(published.at("document").at("revision") == "10");
    CHECK(workbench.idle());
    outcome.reset();
    workbench.execute("ui.commandPalette", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    selection->set_value(nullptr);
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench Git discard awaits confirmation and retains its exact preview") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> confirmation;
    ViewHost views;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"gitPath", "file.cpp"},
                              {"gitStaged", false},
                              {"gitPreview", "captured-preview"}});
            if (method == "_ui.dialog")
                return confirmation.get_future();
            return views(method, params);
        },
        modules());
    workbench.execute("ui.gitDiscard", Json::object(), {});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.awaiting_input());
    confirmation.set_value(true);
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "git.discard");
    CHECK(channel->requests.front().body.at("params").at("previewId") == "captured-preview");
    channel->reply({{"entries", Json::array()}});
    for (int tick = 0; tick < 50 && channel->requests.empty(); ++tick) {
        views.views.poll();
        workbench.poll();
    }
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.tree");
    channel->reply({{"entries", Json::array()}});
    for (int tick = 0; tick < 50; ++tick) {
        views.views.poll();
        workbench.poll();
    }
    CHECK(workbench.idle());
}

TEST_CASE("workbench Git commit skips input when no staged changes exist") {
    auto channel = std::make_shared<Channel>();
    bool prompted{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.dialog")
                prompted = true;
            return ready(nullptr);
        },
        modules());
    workbench.execute("ui.gitCommit", Json::object(), {});
    workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "git.status");
    channel->reply(
        {{"entries", Json::array({{{"path", "untracked.cpp"}, {"staged", "untracked"}}})}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK_FALSE(prompted);
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench rename awaits name and plan review before applying captured edits") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> name, approval;
    unsigned dialogs{};
    Json review;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document",
                               {{"id", "source"},
                                {"revision", "7"},
                                {"selections", Json::array({{{"anchor", 12}, {"head", 12}}})}}}});
            if (method == "_ui.dialog") {
                if (++dialogs == 1)
                    return name.get_future();
                review = params;
                return approval.get_future();
            }
            return ready(nullptr);
        },
        modules());
    workbench.execute("ui.rename", Json::object(), {});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.awaiting_input());
    name.set_value("renamed");
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "language.request");
    CHECK(channel->requests.front().body.at("params") == Json{{"id", "source"},
                                                              {"baseRevision", "7"},
                                                              {"method", "rename"},
                                                              {"byteOffset", 12},
                                                              {"newName", "renamed"}});
    channel->reply({{"id", "source"},
                    {"revision", "7"},
                    {"planId", "captured-plan"},
                    {"documents",
                     Json::array({{{"path", "source.cpp"}, {"editCount", 2}},
                                  {{"path", "other.cpp"}, {"editCount", 1}}})}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.awaiting_input());
    CHECK(review.at("items") == Json::array({"source.cpp (2 edits)", "other.cpp (1 edits)"}));
    approval.set_value(true);
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "language.applyWorkspaceEdit");
    CHECK(channel->requests.front().body.at("params").at("planId") == "captured-plan");
    channel->reply({{"documents", Json::array()}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(workbench.idle());
}

TEST_CASE("workbench completion awaits a native choice and rejects a changed caret") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> choice;
    Json picker;
    std::size_t head = 12;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready(
                    {{"document",
                      {{"id", "source"},
                       {"revision", "7"},
                       {"selections", Json::array({{{"anchor", head}, {"head", head}}})}}}});
            if (method == "_ui.dialog") {
                picker = params;
                return choice.get_future();
            }
            return ready(nullptr);
        },
        modules());
    workbench.execute("ui.completion", Json::object(), {});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("params").at("byteOffset") == 12);
    channel->reply({{"id", "source"},
                    {"revision", "7"},
                    {"result",
                     {{"items",
                       Json::array({{{"label", "first"}},
                                    {{"label", "中🙂"}, {"insertText", "selected"}}})}}}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    CHECK(workbench.awaiting_input());
    CHECK(channel->requests.empty());
    CHECK(picker.at("kind") == "pick");
    CHECK(picker.at("items") == Json::array({"first", "中🙂"}));
    SUBCASE("the original choice applies to the captured revision") {
        choice.set_value(1);
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "language.applyCompletion");
        CHECK(channel->requests.front().body.at("params") ==
              Json{{"id", "source"},
                   {"baseRevision", "7"},
                   {"byteOffset", 12},
                   {"completionItem", {{"label", "中🙂"}, {"insertText", "selected"}}}});
        channel->reply({{"id", "source"}, {"revision", "8"}});
    }
    SUBCASE("a moved caret does not receive the old insertion") {
        head = 20;
        choice.set_value(1);
        for (int tick = 0; tick < 6; ++tick)
            workbench.poll();
        CHECK(channel->requests.empty());
    }
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    CHECK(workbench.idle());
}

TEST_CASE("workbench navigation verifies a target revision before selection or activation") {
    auto channel = std::make_shared<Channel>();
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document",
                               {{"id", "source"},
                                {"revision", "7"},
                                {"selections", Json::array({{{"anchor", 12}, {"head", 12}}})}}}});
            if (method == "_ui.update")
                updates.push_back(params);
            return ready(nullptr);
        },
        modules());
    workbench.execute("ui.definition", Json::object(), {});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    channel->reply(
        {{"id", "source"},
         {"revision", "7"},
         {"resolvedLocations",
          Json::array({{{"path", "target.cpp"}, {"targetRevision", "3"}, {"byteOffset", 4}}})}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.open");
    channel->reply({{"id", "target"}, {"revision", "4"}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
    for (const auto& update : updates)
        CHECK_FALSE(update.contains("activate"));
}

TEST_CASE("workbench close cancellation stops an awaited user save from closing the document") {
    auto channel = std::make_shared<Channel>();
    bool removed{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document",
                               {{"id", "captured"},
                                {"revision", "7"},
                                {"dirty", true},
                                {"path", "source.cpp"}}}});
            if (method == "_ui.dialog")
                return ready(0);
            if (method == "_ui.update" && params.contains("closedDocument"))
                removed = true;
            return ready(nullptr);
        },
        modules(),
        "let controller=new "
        "AbortController();astra.registerCommand('close',()=>astra.ui.execute('workbench.close',{"
        "id:'captured',signal:controller.signal}));astra.registerCommand('abort',()=>controller."
        "abort());astra.registerCommand('ui.save',async parameters=>{if(!parameters.signal)throw "
        "Error('Missing close cancellation');return await "
        "astra.request('user.save',{id:parameters.id});});");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("close", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "user.save");
    workbench.execute("abort", Json::object(), {});
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    channel->reply({{"id", "captured"}, {"revision", "8"}, {"dirty", false}});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("name") == "AbortError");
    CHECK_FALSE(removed);
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench progress awaits completion and cancels late service replies") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> input;
    Json shown;
    bool open{};
    unsigned dismissed{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.dialog") {
                shown = params;
                open = true;
                return input.get_future();
            }
            if (method == "_ui.dialog.present") {
                CHECK(open);
                CHECK(params.at("id") == shown.at("id"));
                return ready(true);
            }
            if (method == "_ui.dialog.cancel") {
                ++dismissed;
                if (open) {
                    open = false;
                    input.set_value(nullptr);
                }
                return ready(nullptr);
            }
            return ready(nullptr);
        },
        modules(),
        "astra.registerCommand('progress',()=>astra.ui.withProgress({title:'Durable "
        "work'},options=>astra.request('slow',{},options)));");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("progress", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(shown.at("kind") == "progress");
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "slow");
    CHECK_FALSE(outcome);
    CHECK_FALSE(workbench.awaiting_input());
    SUBCASE("completed work returns its result and dismisses the component") {
        channel->reply({{"durable", true}});
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value.at("durable") == true);
        CHECK_FALSE(open);
        CHECK(dismissed == 1);
        CHECK(workbench.idle());
    }
    SUBCASE("dismissed work rejects cancellation and drains its late operation ID") {
        open = false;
        input.set_value(nullptr);
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE(outcome->error);
        CHECK(outcome->error->at("name") == "AbortError");
        CHECK_FALSE(workbench.idle());
        channel->reply({{"operationId", "late-checkpoint"}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "operation.cancel");
        CHECK(channel->requests.front().body.at("params").at("id") == "late-checkpoint");
        channel->reply(nullptr);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench progress does not start work when its component cannot open") {
    auto channel = std::make_shared<Channel>();
    unsigned cancelled{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.dialog" || method == "_ui.dialog.present") {
                std::promise<Json> failed;
                failed.set_exception(std::make_exception_ptr(
                    astra::protocol::RpcError(-32004, "Another component is active")));
                return failed.get_future();
            }
            CHECK(method == "_ui.dialog.cancel");
            ++cancelled;
            return ready(nullptr);
        },
        modules(),
        "astra.registerCommand('progress',()=>astra.ui.withProgress({title:'Work'},options=>astra."
        "request('must.not.start',{},options)));");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("progress", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("code") == -32004);
    CHECK(channel->requests.empty());
    CHECK(cancelled == 1);
    CHECK(workbench.idle());
}

TEST_CASE("workbench idle fences exclude other pending script work and drain cancellation") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> fence;
    Json identity;
    unsigned cancelled{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.barrier") {
                identity = params;
                return fence.get_future();
            }
            CHECK(method == "_ui.wait.cancel");
            CHECK(params.at("waitId") == identity.at("waitId"));
            ++cancelled;
            fence.set_value(false);
            return ready(nullptr);
        },
        modules(),
        "let controller=new "
        "AbortController();astra.registerCommand('barrier',()=>astra.ui.waitForIdle({signal:"
        "controller.signal}));astra.registerCommand('pending',()=>astra.request('prior'));astra."
        "registerCommand('abort',()=>controller.abort());");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("pending", Json::object(), {});
    workbench.execute("barrier", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    CHECK_FALSE(workbench.awaiting("_ui.barrier"));
    CHECK_FALSE(outcome);
    channel->reply(nullptr);
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    CHECK(workbench.awaiting("_ui.barrier"));
    workbench.execute("abort", Json::object(), {});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("name") == "AbortError");
    CHECK(cancelled == 1);
    CHECK(workbench.idle());
}

TEST_CASE("workbench concurrent fence kinds remain distinguishable from unrelated jobs") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> idle, close;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.barrier")
                return idle.get_future();
            REQUIRE(method == "_ui.window.close");
            return close.get_future();
        },
        modules(),
        "astra.registerCommand('waiters',()=>Promise.all([astra.ui.waitForIdle(),astra.ui."
        "requestClose()]));");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("waiters", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 5; ++tick)
        workbench.poll();
    constexpr std::string_view methods[]{"_ui.barrier", "_ui.window.close"};
    CHECK(workbench.awaiting(methods));
    CHECK_FALSE(workbench.awaiting("_ui.barrier"));
    CHECK_FALSE(workbench.awaiting_input());
    CHECK_FALSE(outcome);
    idle.set_value(true);
    close.set_value(false);
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == Json::array({true, false}));
    CHECK(workbench.idle());
}

TEST_CASE("workbench window exit composes captured closes before its durable checkpoint") {
    auto channel = std::make_shared<Channel>();
    std::vector<std::string> tabs{"left", "right"};
    std::promise<Json> progress, teardown;
    bool progress_open{}, requested{};
    std::vector<std::string> activated;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"tabs", tabs}});
            if (method == "_ui.barrier")
                return ready(true);
            if (method == "_ui.window.close") {
                requested = true;
                CHECK(tabs.empty());
                return teardown.get_future();
            }
            if (method == "_ui.dialog") {
                CHECK(params.at("kind") == "progress");
                progress_open = true;
                return progress.get_future();
            }
            if (method == "_ui.dialog.cancel") {
                if (progress_open) {
                    progress_open = false;
                    progress.set_value(nullptr);
                }
                return ready(nullptr);
            }
            if (method == "_ui.update") {
                if (params.contains("activate"))
                    activated.push_back(params.at("activate"));
                if (params.contains("closedDocument"))
                    std::erase(tabs, params.at("closedDocument").get<std::string>());
            }
            return ready(nullptr);
        },
        modules(),
        "astra.registerCommand('workbench.close',async p=>{const accepted=await "
        "astra.request('custom.close',p);if(accepted)await "
        "astra.ui.update({closedDocument:p.id});return accepted;});");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.closeWindow", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "custom.close");
    CHECK(channel->requests.front().body.at("params").at("id") == "right");
    CHECK_FALSE(requested);
    SUBCASE("cancelled document close stops exit without checkpointing") {
        channel->reply(false);
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == false);
        CHECK_FALSE(requested);
        CHECK(channel->requests.empty());
        CHECK(tabs.size() == 2);
    }
    SUBCASE("accepted closes await the actual durable reply and teardown acknowledgement") {
        channel->reply(true);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("params").at("id") == "left");
        channel->reply(true);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        CHECK(activated == std::vector<std::string>{"right", "left"});
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "recovery.flush");
        CHECK_FALSE(outcome);
        CHECK_FALSE(requested);
        channel->reply({{"enabled", true}});
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        CHECK(requested);
        CHECK_FALSE(progress_open);
        CHECK_FALSE(outcome);
        teardown.set_value(true);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == true);
    }
    CHECK(workbench.idle());
}

TEST_CASE("workbench window checkpoint failure requires an explicit retry keep-open or bypass") {
    auto channel = std::make_shared<Channel>();
    std::optional<std::promise<Json>> dialog;
    Json spec;
    unsigned closes{}, progresses{};
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"tabs", Json::array()}});
            if (method == "_ui.barrier")
                return ready(true);
            if (method == "_ui.window.close") {
                ++closes;
                return ready(true);
            }
            if (method == "_ui.dialog") {
                spec = params;
                dialog.emplace();
                if (params.at("kind") == "progress")
                    ++progresses;
                return dialog->get_future();
            }
            if (method == "_ui.dialog.cancel" ||
                (method == "_ui.action" && params.at("command") == "ui.cancel")) {
                if (dialog) {
                    dialog->set_value(nullptr);
                    dialog.reset();
                }
                return ready(nullptr);
            }
            if (method == "_ui.dialog.answer") {
                REQUIRE(dialog);
                CHECK(params.at("id") == spec.at("id"));
                dialog->set_value(params.at("index"));
                dialog.reset();
                return ready(nullptr);
            }
            if (method == "_ui.update")
                updates.push_back(params);
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.exitWithoutCheckpoint", Json::object(), {});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(closes == 0);
    CHECK(channel->requests.empty());
    workbench.execute("ui.closeWindow", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    auto incoming = std::move(channel->requests.front());
    channel->requests.pop_front();
    incoming.response.set_value(Json{{"jsonrpc", "2.0"},
                                     {"id", incoming.body.at("id")},
                                     {"error", {{"code", -32010}, {"message", "storage failed"}}}}
                                    .dump());
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    CHECK(closes == 0);
    CHECK_FALSE(outcome);
    REQUIRE(spec.at("kind") == "choice");
    CHECK(spec.at("keys")[2] == "e");
    CHECK(workbench.awaiting_input());
    SUBCASE("keep open cancels without teardown") {
        workbench.execute("ui.cancel", Json::object(), {});
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == false);
        CHECK(closes == 0);
        CHECK(channel->requests.empty());
    }
    SUBCASE("retry awaits another durable service result") {
        workbench.execute("ui.retryExit", Json::object(), {});
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "recovery.flush");
        CHECK(progresses == 2);
        CHECK(closes == 0);
        channel->reply({{"enabled", true}});
        for (int tick = 0; tick < 16; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == true);
        CHECK(closes == 1);
    }
    SUBCASE("explicit bypass is enabled only for this failed checkpoint") {
        workbench.execute("ui.exitWithoutCheckpoint", Json::object(), {});
        for (int tick = 0; tick < 16; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == true);
        CHECK(closes == 1);
        CHECK(channel->requests.empty());
        CHECK(progresses == 1);
    }
    CHECK(workbench.idle());
}

TEST_CASE("workbench formatting discards obsolete results without applying edits") {
    auto channel = std::make_shared<Channel>();
    astra::ui::Workbench workbench(
        channel,
        [](std::string_view method, const Json&) {
            if (method == "_ui.read")
                return ready({{"document", {{"id", "source"}, {"revision", "8"}}}});
            return ready(nullptr);
        },
        modules());
    workbench.execute("ui.format", Json::object(), {});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "language.request");
    channel->reply({{"obsolete", true}, {"result", Json::array({{{"newText", "changed"}}})}});
    for (int tick = 0; tick < 4; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}
