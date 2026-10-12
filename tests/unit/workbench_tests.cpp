#include <astra/ui/workbench.hpp>
#include <astra/ui/icons.hpp>
#include <astra/ui/input_queue.hpp>
#include <astra/ui/inline_form.hpp>
#include <astra/ui/form_registry.hpp>
#include <astra/ui/view_store.hpp>
#include <astra/ui/picker_index.hpp>
#include <astra/ui/view_page.hpp>
#include <astra/ui/chrome_state.hpp>
#include <astra/ui/editor_tooltip.hpp>
#include <astra/ui/editor_link.hpp>
#include <astra/io/file_store.hpp>
#include <doctest/doctest.h>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <utility>
#include <tuple>
#include <chrono>
#include <thread>

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
    for (const auto& module : astra::ui::icon_modules()) {
        result.emplace(module.id, module.source);
    }
    result["astra/defaults"] = astra::io::read_file("resources/defaults.js").text;
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
                                            {"astra/outline", "outline.js"},
                                            {"astra/chrome", "chrome.js"},
                                            {"astra/commands", "commands.js"},
                                            {"astra/workbench", "workbench.js"}}) {
        result[name] =
            astra::io::read_file(std::filesystem::path("resources/workbench") / file).text;
    }
    return result;
}

std::future<Json> ready(Json value) {
    std::promise<Json> result;
    result.set_value(std::move(value));
    return result.get_future();
}

struct PickerHost {
    astra::ui::PickerIndex picker;
    std::optional<std::promise<Json>> input;
    Json specification;
    std::vector<Json> publications;

    std::future<Json> operator()(std::string_view method, const Json& params) {
        if (method == "_ui.dialog") {
            REQUIRE_FALSE(input);
            specification = params;
            picker.reset(params.at("items").get<std::vector<std::string>>(),
                         params.value("dynamic", false),
                         params.value("iconPaths", Json::array()).get<std::vector<std::string>>(),
                         params.value("badges", Json::array()).get<std::vector<std::string>>());
            input.emplace();
            return input->get_future();
        }
        if (method == "_ui.dialog.cancel") {
            if (input) {
                input->set_value(nullptr);
                input.reset();
            }
            return ready(nullptr);
        }
        REQUIRE(method == "_ui.picker.publish");
        publications.push_back(params);
        return ready(
            input && specification.at("id") == params.at("id") &&
            picker.publish(params.at("generation").get<std::string>(),
                           params.at("items").get<std::vector<std::string>>(),
                           params.value("message", std::string{}),
                           params.value("error", false),
                           params.value("iconPaths", Json::array()).get<std::vector<std::string>>(),
                           params.value("badges", Json::array()).get<std::vector<std::string>>()));
    }

    void query(astra::ui::Workbench& workbench, std::string text) {
        picker.filter(text);
        workbench.emit("ui.picker.query",
                       {{"id", specification.at("id")},
                        {"generation", picker.generation()},
                        {"query", std::move(text)}});
    }
};

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
        if (method == "_ui.read")
            return ready({{"document", nullptr}});
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
          {"fileCount", 1},
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
    CHECK(outcome->value.at("matchCount") == 1);
    CHECK(outcome->value.at("fileCount") == 1);
    CHECK(outcome->value.at("paths") == Json::array({"a.txt"}));
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

TEST_CASE("workbench dynamic picker cancels obsolete providers and returns captured items") {
    auto channel = std::make_shared<Channel>();
    PickerHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules(),
        R"js(
        let releaseOld, oldSignal;
        astra.registerCommand('acceptance.pick', () => astra.ui.pick({
            title: 'Dynamic',
            items: async (query, {signal}) => {
                if (query === 'old') {
                    oldSignal = signal;
                    await new Promise(resolve => releaseOld = resolve);
                }
                if (query === 'error') throw new Error('Failed\nprovider');
                return {items: [{label: query, path: query + '.cpp', iconPath: query + '.cpp', badge: 'F9'}],
                        message: '1 match'};
            }
        }));
        astra.registerCommand('acceptance.release', () => {
            releaseOld();
            return oldSignal.aborted;
        });
        )js");
    auto poll = [&] {
        for (int tick = 0; tick < 20; ++tick)
            workbench.poll();
    };
    std::optional<astra::script::Runtime::Outcome> outcome, released;
    workbench.execute("acceptance.pick", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    poll();
    REQUIRE(host.input);
    CHECK(host.specification.at("dynamic") == true);
    host.query(workbench, "old");
    poll();
    CHECK(host.publications.empty());
    CHECK_FALSE(host.picker.ready());
    host.query(workbench, "new🙂");
    poll();
    REQUIRE(host.picker.ready());
    CHECK(host.picker.at(0).label == "new🙂");
    CHECK(host.picker.at(0).icon_path == "new🙂.cpp");
    CHECK(host.picker.at(0).badge == "F9");
    workbench.execute("acceptance.release", Json::object(), [&](const auto& value) {
        released = value;
    });
    poll();
    REQUIRE(released);
    CHECK_FALSE(released->error);
    CHECK(released->value == true);
    CHECK(host.publications.size() == 1);
    CHECK(workbench.awaiting_input());
    host.query(workbench, "error");
    poll();
    CHECK(host.picker.ready());
    CHECK(host.picker.error());
    CHECK(host.picker.message() == "Failed provider");
    host.query(workbench, "recovered");
    poll();
    CHECK_FALSE(host.picker.error());
    REQUIRE(host.picker.size() == 1);
    host.input->set_value({{"generation", host.picker.generation()}, {"index", 0}});
    host.input.reset();
    poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value.at("path") == "recovered.cpp");
    CHECK(outcome->value.at("badge") == "F9");
    CHECK(workbench.take_errors().empty());
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench picker preserves static item data and rejects obsolete dynamic selections") {
    auto channel = std::make_shared<Channel>();
    PickerHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules(),
        R"js(
        astra.registerCommand('acceptance.static', () => astra.ui.pick({
            title: 'Static', dynamic: true,
            items: [{label: 'First', data: 42, iconPath: 'first.cpp', badge: 'Ctrl+F9'}, 'Second']
        }));
        astra.registerCommand('acceptance.dynamic', () => astra.ui.pick({
            title: 'Dynamic', items: query => [{label: query, data: query}]
        }));
        )js");
    auto poll = [&] {
        for (int tick = 0; tick < 20; ++tick)
            workbench.poll();
    };
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("acceptance.static", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    poll();
    REQUIRE(host.input);
    CHECK_FALSE(host.picker.dynamic());
    CHECK(host.picker.at(0).icon_path == "first.cpp");
    CHECK(host.picker.at(0).badge == "Ctrl+F9");
    host.input->set_value(0);
    host.input.reset();
    poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value.at("data") == 42);
    CHECK(outcome->value.at("badge") == "Ctrl+F9");
    outcome.reset();
    workbench.execute("acceptance.dynamic", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    poll();
    host.query(workbench, "first");
    poll();
    const auto obsolete = host.picker.generation();
    host.query(workbench, "latest");
    poll();
    REQUIRE(host.picker.ready());
    host.input->set_value({{"generation", obsolete}, {"index", 0}});
    host.input.reset();
    poll();
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("message") == "Picker returned an obsolete selection");
    CHECK(workbench.idle());
    CHECK(channel->requests.empty());
    CHECK(workbench.take_errors().empty());
}

TEST_CASE("workbench dynamic picker dismissal retires ignored-signal work") {
    auto channel = std::make_shared<Channel>();
    PickerHost host;
    unsigned updates{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.update") {
                ++updates;
                return ready(nullptr);
            }
            return host(method, params);
        },
        modules(),
        R"js(
        let release, cancelledSignal;
        astra.registerCommand('acceptance.pick', () => astra.ui.pick({
            title: 'Dismiss',
            items: async (_, {signal}) => {
                cancelledSignal = signal;
                await new Promise(resolve => release = resolve);
                await astra.ui.update({status: 'Forbidden late mutation'});
                return ['Late'];
            }
        }));
        astra.registerCommand('acceptance.release', () => {
            release();
            return cancelledSignal.aborted;
        });
        )js");
    std::optional<astra::script::Runtime::Outcome> outcome, released;
    workbench.execute("acceptance.pick", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    host.query(workbench, "waiting");
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    REQUIRE(host.input);
    host.input->set_value(nullptr);
    host.input.reset();
    for (int tick = 0; tick < 30; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value.is_null());
    CHECK(workbench.idle());
    workbench.execute("acceptance.release", Json::object(), [&](const auto& value) {
        released = value;
    });
    for (int tick = 0; tick < 30; ++tick)
        workbench.poll();
    REQUIRE(released);
    CHECK(released->value == true);
    CHECK(updates == 0);
    CHECK(host.publications.empty());
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench quick open searches beyond static picker capacity and drains cancellation") {
    auto channel = std::make_shared<Channel>();
    PickerHost host;
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document", {{"path", "src/active.cpp"}}}});
            if (method == "_ui.update") {
                updates.push_back(params);
                return ready(nullptr);
            }
            return host(method, params);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.quickOpen", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    REQUIRE(host.input);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.tree");
    CHECK(channel->requests.front().body.at("params").at("maxEntries") == 100000);
    host.query(workbench, "unused");
    host.query(workbench, "中🙂");
    channel->reply({{"operationId", "files"}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!host.picker.ready()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        workbench.poll();
        if (!channel->requests.empty()) {
            const auto& body = channel->requests.front().body;
            REQUIRE(body.at("method") == "operation.get");
            const auto offset = body.at("params").at("offset").get<std::size_t>();
            const auto end = std::min(offset + 256, std::size_t{5003});
            Json entries = Json::array();
            for (auto row = offset; row < end; ++row) {
                const auto path = row < 5000    ? "generated/item" + std::to_string(row) + ".txt"
                                  : row == 5000 ? "src/中🙂.cpp"
                                  : row == 5001 ? "lib/中🙂-test.cpp"
                                                : "src/active.cpp";
                entries.push_back({{"path", path}, {"directory", false}});
            }
            channel->reply({{"state", "completed"},
                            {"page", {{"nextOffset", end}, {"total", 5003}}},
                            {"result", {{"entries", entries}}}});
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(host.picker.size() == 2);
    CHECK(host.picker.at(0).label == "src/中🙂.cpp");
    CHECK(host.picker.at(1).label == "lib/中🙂-test.cpp");
    CHECK(host.publications.size() == 1);
    host.input->set_value({{"generation", host.picker.generation()}, {"index", 0}});
    host.input.reset();
    for (int tick = 0; tick < 30; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.open");
    CHECK(channel->requests.front().body.at("params").at("path") == "src/中🙂.cpp");
    channel->reply({{"id", "selected"}, {"revision", "0"}});
    for (int tick = 0; tick < 30; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    REQUIRE(updates.size() == 1);
    CHECK(updates.at(0).at("activate") == "selected");
    CHECK(workbench.idle());

    outcome.reset();
    workbench.execute("ui.quickOpen", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    REQUIRE(host.input);
    host.query(workbench, "waiting");
    host.input->set_value(nullptr);
    host.input.reset();
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "workspace.tree");
    channel->reply({{"operationId", "late-files"}});
    for (int tick = 0; tick < 30; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.cancel");
    CHECK(channel->requests.front().body.at("params").at("id") == "late-files");
    channel->reply({{"cancelled", true}});
    for (int tick = 0; tick < 30; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(updates.size() == 1);
    CHECK(channel->requests.empty());
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

TEST_CASE("workbench JS owns complete default keys and explicit client preferences") {
    auto channel = std::make_shared<Channel>();
    astra::ui::Workbench workbench(
        channel,
        [](auto, const auto&) {
            return ready(nullptr);
        },
        modules(),
        R"js(
            astra.bind('Ctrl+S', 'ui.save');
            astra.bind('Left', 'temporary.left');
            astra.bind('Left', 'editor.move_left');
            astra.registerCommand('change.binding', async () => {
                await astra.sleep(0);
                astra.bind('PageDown', 'custom.page');
            });
        )js");
    const auto defaults = workbench.default_keybindings();
    for (const auto& [key, command] :
         std::map<std::string, std::string>{{"Left", "editor.move_left"},
                                            {"Ctrl+Home", "editor.move_document_start"},
                                            {"Shift+Tab", "editor.outdent"},
                                            {"Ctrl+Shift+Z", "editor.redo"},
                                            {"Ctrl+B", "ui.toggleSidebar"},
                                            {"Ctrl+Tab", "ui.nextTab"},
                                            {"Ctrl+Shift+Tab", "ui.previousTab"},
                                            {"Ctrl+PageUp", "ui.previousTab"},
                                            {"Ctrl+PageDown", "ui.nextTab"},
                                            {"PageUp", "ui.pageUp"},
                                            {"PageDown", "ui.pageDown"},
                                            {"Alt+F4", "ui.closeWindow"}}) {
        CAPTURE(key);
        CHECK(defaults.at(key) == command);
    }
    const auto preferences = workbench.keybindings();
    REQUIRE(preferences.size() == 2);
    CHECK(preferences.at("Ctrl+S") == "ui.save");
    CHECK(preferences.at("Left") == "editor.move_left");
    const auto before = workbench.keybinding_generation();
    workbench.execute("change.binding", Json::object(), {});
    for (int tick = 0; tick < 12 && !workbench.idle(); ++tick) {
        workbench.poll();
    }
    REQUIRE(workbench.idle());
    CHECK(workbench.keybinding_generation() > before);
    CHECK(workbench.keybindings().at("PageDown") == "custom.page");
    CHECK(workbench.default_keybindings().at("PageDown") == "ui.pageDown");
    CHECK(workbench.take_errors().empty());
    CHECK(channel->requests.empty());
}

TEST_CASE("workbench JS page scrolling awaits native metrics and clamps the captured document") {
    auto channel = std::make_shared<Channel>();
    Json state{{"document", {{"id", "captured"}, {"lineCount", 80}}},
               {"viewport", {{"firstLine", 37}, {"pageLines", 15}}}};
    std::vector<Json> updates;
    astra::ui::Workbench workbench(
        channel,
        [&](auto method, const auto& params) {
            if (method == "_ui.read") {
                return ready(state);
            }
            if (method == "_ui.update") {
                updates.push_back(params);
                return ready(nullptr);
            }
            throw std::runtime_error("Unexpected page policy request");
        },
        modules());
    for (const auto& [command, first, expected] :
         std::vector<std::tuple<std::string, std::size_t, std::size_t>>{{"ui.pageDown", 37, 52},
                                                                        {"ui.pageDown", 75, 79},
                                                                        {"ui.pageUp", 37, 22},
                                                                        {"ui.pageUp", 5, 0}}) {
        CAPTURE(command);
        CAPTURE(first);
        state["viewport"]["firstLine"] = first;
        const auto before = updates.size();
        workbench.execute(command, Json::object(), {});
        CHECK(updates.size() == before);
        for (int tick = 0; tick < 12 && !workbench.idle(); ++tick) {
            workbench.poll();
        }
        REQUIRE(workbench.idle());
        REQUIRE(updates.size() == before + 1);
        CHECK(updates.back().at("scrollLine") == Json{{"id", "captured"}, {"line", expected}});
    }
    state["document"] = nullptr;
    const auto before = updates.size();
    workbench.execute("ui.pageDown", Json::object(), {});
    for (int tick = 0; tick < 12 && !workbench.idle(); ++tick) {
        workbench.poll();
    }
    CHECK(updates.size() == before);
    CHECK(channel->requests.empty());
    CHECK(workbench.take_errors().empty());
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

TEST_CASE("workbench Explorer reveal cancels obsolete requests and respects preferences") {
    auto channel = std::make_shared<Channel>();
    Json state{{"activity", "1"}, {"document", {{"id", "first"}, {"path", "src\\中🙂.cpp"}}}};
    std::vector<Json> reveals, updates;
    std::optional<std::promise<Json>> delayed;
    bool hold = true;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read") {
                CHECK(params.at("preferences") == Json::array({"explorer"}));
                return ready(state);
            }
            if (method == "_ui.view.read") {
                CHECK(params.at("id") == "explorer");
                if (hold) {
                    REQUIRE_FALSE(delayed);
                    delayed.emplace();
                    return delayed->get_future();
                }
                return ready({{"version", "9"}});
            }
            if (method == "_ui.view.reveal") {
                reveals.push_back(params);
                return ready({{"row", {{"id", params.at("row")}}}});
            }
            if (method == "_ui.update") {
                updates.push_back(params);
                return ready(true);
            }
            CHECK(method == "_ui.view.focus");
            return ready(true);
        },
        modules());
    const auto poll = [&] {
        for (int tick = 0; tick < 32; ++tick)
            workbench.poll();
    };
    workbench.emit("ui.document.activated", {{"activity", "1"}});
    poll();
    REQUIRE(delayed);
    CHECK(reveals.empty());
    state["activity"] = "2";
    state["document"] = {{"id", "second"}, {"path", "lib\\second.cpp"}};
    hold = false;
    workbench.emit("ui.document.activated", {{"activity", "2"}});
    poll();
    REQUIRE(reveals.size() == 1);
    CHECK(reveals.front().at("row") == "lib/second.cpp");
    CHECK(reveals.front().at("activity") == "2");
    CHECK(reveals.front().at("version") == "9");
    CHECK(updates.empty());
    delayed->set_value({{"version", "8"}});
    delayed.reset();
    poll();
    CHECK(reveals.size() == 1);
    CHECK(workbench.idle());
    CHECK(workbench.take_errors().empty());
    workbench.emit("ui.document.activated", {{"activity", "1"}});
    poll();
    CHECK(reveals.size() == 1);
    state["preferences"] = {{"explorer", {{"autoReveal", false}}}};
    state["activity"] = "3";
    workbench.emit("ui.document.activated", {{"activity", "3"}});
    poll();
    CHECK(reveals.size() == 1);
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.revealActiveFile", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    REQUIRE(reveals.size() == 2);
    CHECK(reveals.back().at("activity") == "3");
    REQUIRE(updates.size() == 1);
    CHECK(updates.front().at("sidebar") == "explorer");
    CHECK(updates.front().at("sidebarVisible") == true);
    state["document"] = {{"id", "untitled"}, {"path", ""}};
    state["activity"] = "4";
    state["preferences"] = {{"explorer", {{"autoReveal", true}}}};
    workbench.emit("ui.document.activated", {{"activity", "4"}});
    poll();
    CHECK(reveals.size() == 2);
    CHECK(workbench.idle());
    CHECK(channel->requests.empty());
    CHECK(workbench.take_errors().empty());
}

TEST_CASE("workbench cancelled completion metadata cannot invoke a published hook") {
    auto channel = std::make_shared<Channel>();
    ViewHost host;
    std::optional<std::promise<Json>> metadata;
    bool hold = true;
    std::vector<Json> callbacks;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.update" && params.contains("status") && hold) {
                REQUIRE_FALSE(metadata);
                metadata.emplace();
                return metadata->get_future();
            }
            if (method == "_ui.action") {
                callbacks.push_back(params);
                return ready(true);
            }
            return host(method, params);
        },
        modules(),
        R"js(
            astra.ui.registerView({id:'published',title:'Published'}, () => ({
                rows: [{id:'row',label:'Row'}], update: {status:'Published'}
            }), { published: ({version}, options) =>
                astra.request('_ui.action', {command:'published',version}, options)
            });
            astra.registerCommand('publish', () => astra.ui.refresh('published'));
            astra.registerCommand('cancelPublication', () => astra.ui.cancel('published'));
        )js");
    const auto poll = [&] {
        for (int tick = 0; tick < 32; ++tick) {
            host.views.poll();
            workbench.poll();
        }
    };
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("publish", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    poll();
    REQUIRE(metadata);
    REQUIRE(host.views.at("published").index.size() == 1);
    CHECK(callbacks.empty());
    workbench.execute("cancelPublication", Json::object(), {});
    poll();
    metadata->set_value(true);
    metadata.reset();
    poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(callbacks.empty());
    CHECK(host.views.at("published").index.at(0).value->at("id") == "row");
    hold = false;
    outcome.reset();
    workbench.execute("publish", Json::object(), [&](const auto& result) {
        outcome = result;
    });
    poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    REQUIRE(callbacks.size() == 1);
    CHECK(callbacks.front().at("version") == host.views.at("published").version);
    CHECK(workbench.idle());
    CHECK(workbench.take_errors().empty());
}

TEST_CASE("workbench provider generation prevents older results replacing newer views") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ViewStore views;
    std::size_t publications = 0;
    const auto source = "astra.ui.registerView({id:'explorer',title:'Files',outline:true,"
                        "outlineTitle:'Symbols 中🙂'},"
                        "(p,o)=>astra.request('files',p,o)); "
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
    CHECK(views.state("explorer").at("outlineTitle") == "Symbols 中🙂");
    CHECK(views.state("explorer").at("title") == "Files");
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

TEST_CASE("workbench streamed rows honor backpressure and snapshot reused objects") {
    auto channel = std::make_shared<Channel>();
    ViewHost host;
    const Json spec{{"id", "stream"}, {"title", "Stream"}};
    const auto old = host.views.begin(spec);
    REQUIRE(host.views.append("stream", old, Json::array({{{"id", "old"}, {"label", "Old"}}})));
    auto initial = host.views.commit("stream", old, "", "Empty");
    host.views.poll();
    REQUIRE(initial.get().get<bool>());
    const auto retained = host.views.at("stream").index.snapshot();
    std::promise<Json> acknowledgement;
    bool paused = false;
    const auto source = R"js(
        let produced = 0, closed = 0, completed = 0;
        astra.ui.registerView({id:'stream',title:'Stream'}, () => ({
            rows: (async function* () {
                const row = {id:'',label:'中🙂',data:{index:0}};
                try {
                    for (let index = 0; index < 600; ++index) {
                        row.id = String(index);
                        row.data.index = index;
                        ++produced;
                        yield row;
                    }
                } finally { ++closed; }
            })(),
            complete: async () => {
                ++completed;
                return {subtitle:produced+' streamed rows'};
            }
        }));
        astra.registerCommand('publishStream', () => astra.ui.refresh('stream'));
        astra.registerCommand('inspectStream', () => ({produced,closed,completed}));
    )js";
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.view.append" && !paused) {
                paused = true;
                REQUIRE(host(method, params).get().get<bool>());
                return acknowledgement.get_future();
            }
            return host(method, params);
        },
        modules(),
        source);
    std::optional<astra::script::Runtime::Outcome> published, inspection;
    workbench.execute("publishStream", Json::object(), [&](const auto& result) {
        published = result;
    });
    for (int tick = 0; tick < 200 && !paused; ++tick)
        workbench.poll();
    REQUIRE(paused);
    CHECK_FALSE(published);
    REQUIRE(host.pages.size() == 1);
    CHECK(host.pages.front().size() == 128);
    workbench.execute("inspectStream", Json::object(), [&](const auto& result) {
        inspection = result;
    });
    for (int tick = 0; tick < 20 && !inspection; ++tick)
        workbench.poll();
    REQUIRE(inspection);
    REQUIRE_FALSE(inspection->error);
    CHECK(inspection->value.at("produced") == 129);
    CHECK(inspection->value.at("closed") == 0);
    CHECK(inspection->value.at("completed") == 0);
    CHECK(host.views.at("stream").version == old);
    CHECK(host.views.navigate("stream", "last", 5).at("row").at("id") == "old");
    acknowledgement.set_value(true);
    for (int tick = 0; tick < 500 && !published; ++tick) {
        host.views.poll();
        workbench.poll();
    }
    REQUIRE(published);
    REQUIRE_FALSE(published->error);
    CHECK(published->value == true);
    CHECK(host.views.at("stream").subtitle == "600 streamed rows");
    CHECK(host.views.at("stream").index.size() == 600);
    CHECK(host.views.at("stream").index.at(0).value->at("data").at("index") == 0);
    CHECK(host.views.at("stream").index.at(599).value->at("data").at("index") == 599);
    CHECK(retained.size() == 1);
    CHECK(retained.at(0).value->at("id") == "old");
    CHECK(workbench.idle());
}

TEST_CASE("workbench streamed view failures retain the published snapshot and close iterators") {
    for (const auto* mode : {"throw", "invalid", "oversized", "metadataThrow", "metadataShape"}) {
        CAPTURE(std::string(mode));
        auto channel = std::make_shared<Channel>();
        ViewHost host;
        const Json spec{{"id", "failure"}, {"title", "Failure"}};
        const auto old = host.views.begin(spec);
        REQUIRE(
            host.views.append("failure", old, Json::array({{{"id", "old"}, {"label", "Old"}}})));
        auto initial = host.views.commit("failure", old, "", "Empty");
        host.views.poll();
        REQUIRE(initial.get().get<bool>());
        const auto source = R"js(
            let closed = 0;
            astra.ui.registerView({id:'failure',title:'Failure'}, ({mode}) => ({
                rows: (async function* () {
                    try {
                        for (let index = 0; index < 400; ++index) {
                            if (index === 130 && mode === 'throw') throw Error('Producer failed');
                            yield {id:String(index),
                                label:index === 130 && mode === 'oversized' ? '🙂'.repeat(20000) : 'Row',
                                depth:index === 130 && mode === 'invalid' ? 1 : 0};
                        }
                    } finally { ++closed; }
                })(),
                complete: () => {
                    if (mode === 'metadataThrow') throw Error('Metadata failed');
                    if (mode === 'metadataShape') return {rows:[]};
                    return {};
                }
            }));
            astra.registerCommand('failStream', parameters => astra.ui.refresh('failure',parameters));
            astra.registerCommand('closedStream', () => closed);
        )js";
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                return host(method, params);
            },
            modules(),
            source);
        std::optional<astra::script::Runtime::Outcome> result, closed;
        workbench.execute("failStream", {{"mode", mode}}, [&](const auto& value) {
            result = value;
        });
        for (int tick = 0; tick < 500 && !result; ++tick) {
            host.views.poll();
            workbench.poll();
        }
        REQUIRE(result);
        REQUIRE(result->error);
        CHECK(host.views.at("failure").version == old);
        CHECK(host.views.at("failure").index.size() == 1);
        CHECK(host.views.at("failure").index.at(0).value->at("id") == "old");
        CHECK(host.views.idle());
        workbench.execute("closedStream", Json::object(), [&](const auto& value) {
            closed = value;
        });
        for (int tick = 0; tick < 20 && !closed; ++tick)
            workbench.poll();
        REQUIRE(closed);
        CHECK(closed->value == 1);
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench cancellation closes stalled row and completion producers") {
    for (const auto* phase : {"rows", "complete"}) {
        CAPTURE(std::string(phase));
        auto channel = std::make_shared<Channel>();
        ViewHost host;
        bool waiting = false;
        const auto source = R"js(
            let closed = 0;
            astra.ui.registerView({id:'cancelled',title:'Cancelled'}, ({phase},options) => ({
                rows: (async function* () {
                    try {
                        for (let index=0; index<400; ++index) {
                            if(index===130 && phase==='rows') {
                                const pending = astra.sleep(200000,options);
                                await astra.ui.update({status:'blocked producer'});
                                await pending;
                            }
                            yield {id:String(index),label:'Row'};
                        }
                    } finally { ++closed; }
                })(),
                complete: async () => {
                    if(phase==='complete') {
                        const pending = astra.sleep(200000,options);
                        await astra.ui.update({status:'blocked producer'});
                        await pending;
                    }
                    return {};
                }
            }));
            astra.registerCommand('cancelStream', () => astra.ui.cancel('cancelled'));
            astra.registerCommand('startStream', parameters => astra.ui.refresh('cancelled',parameters));
            astra.registerCommand('closedStream', () => closed);
        )js";
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                if (method == "_ui.update" &&
                    params.value("status", std::string{}) == "blocked producer")
                    waiting = true;
                return host(method, params);
            },
            modules(),
            source);
        std::optional<astra::script::Runtime::Outcome> result, closed;
        workbench.execute("startStream", {{"phase", phase}}, [&](const auto& value) {
            result = value;
        });
        for (int tick = 0; tick < 500 && !waiting; ++tick) {
            host.views.poll();
            workbench.poll();
        }
        REQUIRE(waiting);
        CHECK_FALSE(result);
        workbench.execute("cancelStream", Json::object(), {});
        for (int tick = 0; tick < 40 && !result; ++tick) {
            host.views.poll();
            workbench.poll();
        }
        REQUIRE(result);
        CHECK_FALSE(result->error);
        CHECK(host.views.at("cancelled").version.empty());
        CHECK(host.views.at("cancelled").index.size() == 0);
        CHECK(host.views.idle());
        workbench.execute("closedStream", Json::object(), [&](const auto& value) {
            closed = value;
        });
        for (int tick = 0; tick < 20 && !closed; ++tick)
            workbench.poll();
        REQUIRE(closed);
        CHECK(closed->value == 1);
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench view providers accept direct synchronous and asynchronous iterables") {
    for (const auto* kind : {"sync", "async"}) {
        CAPTURE(std::string(kind));
        auto channel = std::make_shared<Channel>();
        ViewHost host;
        const auto source = R"js(
            astra.ui.registerView({id:'direct',title:'Direct'}, ({kind}) => kind==='sync'
                ? (function* () { yield {id:'sync',label:'中🙂'}; })()
                : (async function* () { yield {id:'async',label:'中🙂'}; })());
            astra.registerCommand('directRows', parameters => astra.ui.refresh('direct',parameters));
        )js";
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                return host(method, params);
            },
            modules(),
            source);
        std::optional<astra::script::Runtime::Outcome> result;
        workbench.execute("directRows", {{"kind", kind}}, [&](const auto& value) {
            result = value;
        });
        for (int tick = 0; tick < 40 && !result; ++tick) {
            host.views.poll();
            workbench.poll();
        }
        REQUIRE(result);
        REQUIRE_FALSE(result->error);
        CHECK(result->value == true);
        CHECK(host.views.at("direct").index.at(0).value->at("id") == kind);
        CHECK(host.views.at("direct").index.at(0).value->at("label") == "中🙂");
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench streamed workspace search publishes complete totals with bounded samples") {
    auto channel = std::make_shared<Channel>();
    MountedHost host;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            return host(method, params);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> result;
    workbench.execute("workbench.search", {{"query", "needle"}}, [&](const auto& value) {
        result = value;
    });
    host.poll(workbench);
    REQUIRE(channel->requests.size() == 1);
    REQUIRE(channel->requests.front().body.at("method") == "workspace.search");
    channel->reply({{"operationId", "pages"}});
    for (std::size_t offset = 0; offset < 530; offset += 256) {
        host.poll(workbench);
        REQUIRE(channel->requests.size() == 1);
        REQUIRE(channel->requests.front().body.at("method") == "operation.get");
        CHECK(channel->requests.front().body.at("params").at("offset") == offset);
        const auto end = std::min(offset + 256, std::size_t{530});
        Json matches = Json::array();
        for (auto index = offset; index < end; ++index)
            matches.push_back({{"path", "file" + std::to_string(index % 210) + ".txt"},
                               {"line", index},
                               {"begin", index},
                               {"end", index + 1}});
        channel->reply({{"state", "completed"},
                        {"page", {{"offset", offset}, {"nextOffset", end}, {"total", 530}}},
                        {"result",
                         {{"matches", std::move(matches)},
                          {"fileCount", 210},
                          {"issues", Json::array()},
                          {"truncated", false}}}});
    }
    for (int tick = 0; tick < 200 && !result; ++tick)
        host.poll(workbench);
    REQUIRE(result);
    REQUIRE_FALSE(result->error);
    CHECK(result->value.at("matchCount") == 530);
    CHECK(result->value.at("fileCount") == 210);
    CHECK(result->value.at("paths").size() == 200);
    CHECK_FALSE(result->value.contains("matches"));
    CHECK(host.view.views.at("search").index.size() == 530);
    CHECK(host.view.views.at("search").index.at(529).value->at("data").at("line") == 529);
    CHECK(host.view.views.at("search").subtitle == "530 matches");
    CHECK(host.forms.mounted("search")->message() == "530 match(es)");
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench malformed search pages retain the view and refuse replacement") {
    for (const auto* mode : {"fileCount",
                             "total",
                             "offset",
                             "nextOffset",
                             "emptyPage",
                             "missingPage",
                             "collections",
                             "fileTotals"}) {
        CAPTURE(std::string(mode));
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
        const auto version = host.view.views.at("search").version;
        const auto retained = host.view.views.at("search").index.snapshot();
        auto state = host.forms.mounted("search")->state();
        state["action"] = "replaceAll";
        workbench.emit("ui.form.action", state);
        host.poll(workbench);
        REQUIRE(channel->requests.size() == 1);
        REQUIRE(channel->requests.front().body.at("method") == "workspace.search");
        channel->reply({{"operationId", "malformed"}});
        host.poll(workbench);
        REQUIRE(channel->requests.size() == 1);
        REQUIRE(channel->requests.front().body.at("method") == "operation.get");
        Json matches = Json::array();
        for (std::size_t index = 0; index < 130; ++index)
            matches.push_back(
                {{"path", "a.txt"}, {"line", index}, {"begin", index}, {"end", index + 1}});
        auto first = workspace_matches();
        first["result"]["matches"] = std::move(matches);
        first["page"] = {{"offset", 0}, {"nextOffset", 130}, {"total", 131}};
        if (std::string_view(mode) == "fileTotals")
            first["result"]["fileCount"] = 2;
        channel->reply(first);
        host.poll(workbench);
        REQUIRE(channel->requests.size() == 1);
        REQUIRE(channel->requests.front().body.at("method") == "operation.get");
        CHECK(channel->requests.front().body.at("params").at("offset") == 130);
        CHECK(host.view.views.at("search").version == version);
        auto last = workspace_matches();
        last["page"] = {{"offset", 130}, {"nextOffset", 131}, {"total", 131}};
        const std::string_view kind(mode);
        if (kind == "fileCount")
            last["result"]["fileCount"] = 3;
        else if (kind == "total")
            last["page"]["total"] = 132;
        else if (kind == "offset")
            last["page"]["offset"] = 129;
        else if (kind == "nextOffset")
            last["page"]["nextOffset"] = 132;
        else if (kind == "emptyPage") {
            last["result"]["matches"] = Json::array();
            last["page"]["nextOffset"] = 130;
        } else if (kind == "missingPage")
            last.erase("page");
        else if (kind == "collections")
            last["result"]["matches"] = Json::object();
        else if (kind == "fileTotals")
            last["result"]["fileCount"] = 2;
        channel->reply(last);
        host.poll(workbench);
        REQUIRE(channel->requests.size() == 1);
        REQUIRE(channel->requests.front().body.at("method") == "operation.cancel");
        CHECK(channel->requests.front().body.at("params").at("id") == "malformed");
        CHECK_FALSE(host.confirmation);
        channel->reply({{"cancelRequested", true}});
        host.poll(workbench);
        CHECK(host.view.views.at("search").version == version);
        CHECK(host.view.views.at("search").index.size() == 1);
        CHECK(retained.size() == 1);
        CHECK(host.forms.mounted("search")->error());
        CHECK_FALSE(host.forms.mounted("search")->busy());
        CHECK_FALSE(host.confirmation);
        CHECK(host.view.views.idle());
        CHECK(channel->requests.empty());
        CHECK(workbench.take_errors().empty());
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench infinite row streams fail bounded capacities before publication") {
    for (const auto* mode : {"rows", "bytes"}) {
        CAPTURE(std::string(mode));
        auto channel = std::make_shared<Channel>();
        std::size_t appended = 0, pages = 0, largest = 0, commits = 0, aborts = 0;
        const auto source = R"js(
            let produced = 0, closed = 0;
            astra.ui.registerView({id:'capacity',title:'Capacity'}, ({mode}) => ({
                rows: (async function* () {
                    const label = mode==='bytes' ? '🙂'.repeat(12000) : 'Row';
                    try {
                        for (let index=0;;++index) {
                            ++produced;
                            yield {id:String(index),label};
                        }
                    } finally { ++closed; }
                })()
            }));
            astra.registerCommand('capacity', parameters => astra.ui.refresh('capacity',parameters));
            astra.registerCommand('inspectCapacity', () => ({produced,closed}));
        )js";
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                if (method == "_ui.view.begin")
                    return ready("capacity-1");
                if (method == "_ui.view.append") {
                    const auto& rows = params.at("rows");
                    CHECK(rows.size() <= 128);
                    CHECK(rows.dump().size() <= 60000);
                    appended += rows.size();
                    ++pages;
                    largest = std::max(largest, rows.dump().size());
                    return ready(true);
                }
                if (method == "_ui.view.commit")
                    ++commits;
                if (method == "_ui.view.abort")
                    ++aborts;
                return ready(nullptr);
            },
            modules(),
            source);
        std::optional<astra::script::Runtime::Outcome> result, inspection;
        workbench.execute("capacity", {{"mode", mode}}, [&](const auto& value) {
            result = value;
        });
        for (int tick = 0; tick < 5000 && !result; ++tick)
            workbench.poll();
        REQUIRE(result);
        REQUIRE(result->error);
        CHECK(result->error->at("name") == "RangeError");
        CHECK(commits == 0);
        CHECK(aborts == 1);
        CHECK(pages > 10);
        CHECK(largest <= 60000);
        CHECK(appended <= 100000);
        workbench.execute("inspectCapacity", Json::object(), [&](const auto& value) {
            inspection = value;
        });
        for (int tick = 0; tick < 20 && !inspection; ++tick)
            workbench.poll();
        REQUIRE(inspection);
        CHECK(inspection->value.at("closed") == 1);
        if (std::string_view(mode) == "rows")
            CHECK(inspection->value.at("produced") == 100001);
        else
            CHECK(inspection->value.at("produced").get<std::size_t>() < 1000);
        CHECK(workbench.idle());
    }
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

TEST_CASE("workbench file icons use JS associations and async theme updates") {
    auto channel = std::make_shared<Channel>();
    Json theme;
    unsigned generation = 1, writes = 0;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.icons.read") {
                return ready({{"theme", theme}, {"generation", std::to_string(generation)}});
            }
            if (method == "_ui.icons.update") {
                ++writes;
                if (params.at("generation") != std::to_string(generation)) {
                    throw astra::protocol::RpcError(-32001, "Stale icon theme");
                }
                theme = params.at("theme");
                return ready({{"theme", theme}, {"generation", std::to_string(++generation)}});
            }
            return ready(nullptr);
        },
        modules(),
        R"js(
            let savedIcons;
            astra.registerCommand('test.icons', async () => {
                const resolving = astra.ui.icons.resolve([
                    {path: 'src\\MAIN.CPP', directory: false},
                    {path: 'package.json', directory: false},
                    {path: '.git', directory: true},
                    {path: 'thing.unknown', directory: false},
                    {path: '中🙂.ts', directory: false}
                ]);
                if (typeof resolving.then !== 'function') throw new Error('API is not async');
                return await resolving;
            });
            astra.registerCommand('test.icons.custom', async () => {
                savedIcons = await astra.ui.icons.read();
                await astra.ui.icons.update({extensions: {
                    '.d.ts': {glyph: 'symbol', tone: '#123456'},
                    '.cpp': {glyph: 'warning', tone: 'error'}
                }});
                return await astra.ui.icons.resolve([
                    {path: 'source.D.TS', directory: false},
                    {path: 'a.cpp', directory: false}
                ]);
            });
            astra.registerCommand('test.icons.invalid', async () => {
                const before = await astra.ui.icons.read();
                for (const patch of [
                    {extensions: {'.bad': {glyph: 'missing', tone: 'muted'}}},
                    {extensions: {'.bad': {glyph: 'code', tone: 'missing'}}},
                    {filenames: {'../escape': {glyph: 'code', tone: 'muted'}}},
                    {extensions: {'.CPP': {glyph: 'code', tone: 'muted'}}},
                    {theme: 'custom', folder: null},
                    {unexpected: true},
                    {fallback: undefined},
                    {fallback: new Date()},
                    {fallback: {tone: () => 'error'}},
                    {extensions: {'.bad': {asset: 'material/not-installed'}}},
                    {extensions: {'.bad': {asset: 'material/../python'}}},
                    {extensions: {'.bad': {asset: 'material/python', lightAsset: 'missing'}}},
                    {extensions: {'.bad': {asset: 'material/python', glyph: 'warning'}}},
                    {theme: 'missing'},
                    JSON.parse('{"__proto__":{"polluted":true}}')
                ]) {
                    let rejected = false;
                    try { await astra.ui.icons.update(patch); }
                    catch (_) { rejected = true; }
                    if (!rejected) throw new Error('Invalid theme accepted');
                }
                if (JSON.stringify(before) !== JSON.stringify(await astra.ui.icons.read())) {
                    throw new Error('Invalid patch changed theme');
                }
                return true;
            });
            astra.registerCommand('test.icons.delete', async () => {
                await astra.ui.icons.update({extensions: {'.cpp': null}});
                return await astra.ui.icons.resolve([{path: 'a.cpp', directory: false}]);
            });
            astra.registerCommand('test.icons.long', async () => {
                const paths = [
                    {path: 'a'.repeat(32764) + '.cpp', directory: false},
                    {path: '中'.repeat(10920) + '.cpp', directory: false},
                    {path: '.'.repeat(32764) + '.cpp', directory: false}
                ];
                const result = await astra.ui.icons.resolve(paths);
                let rejected = false;
                try {
                    await astra.ui.icons.resolve([
                        {path: '中'.repeat(10923), directory: false}
                    ]);
                } catch (_) { rejected = true; }
                if (!rejected) throw new Error('Oversized UTF-8 path accepted');
                return result.items.map(item => item.icon.asset);
            });
            astra.registerCommand('test.icons.restore', async () => {
                await astra.ui.icons.replace(savedIcons.theme);
                return await astra.ui.icons.resolve([
                    {path: 'source.d.ts', directory: false},
                    {path: 'source.cpp', directory: false}
                ]);
            });
        )js");
    theme = workbench.default_settings().at("fileIcons");
    const auto run = [&](const char* command) {
        std::optional<astra::script::Runtime::Outcome> outcome;
        workbench.execute(command, Json::object(), [&](const auto& result) {
            outcome = result;
        });
        for (int tick = 0; tick < 300 && !outcome; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE_FALSE(outcome->error);
        return outcome->value;
    };
    const auto initial = run("test.icons");
    CHECK(initial.at("items")[0].at("icon") == Json{{"asset", "material/cpp"}});
    CHECK(initial.at("items")[1].at("icon").at("asset") == "material/nodejs");
    CHECK(initial.at("items")[2].at("icon").at("asset") == "material/folder-git");
    CHECK(initial.at("items")[3].at("icon").at("asset") == "material/file");
    CHECK(initial.at("items")[4].at("icon").at("asset") == "material/typescript");
    CHECK(run("test.icons.long") == Json::array({"material/cpp", "material/cpp", "material/cpp"}));
    const auto custom = run("test.icons.custom");
    CHECK(custom.at("generation") == "2");
    CHECK(custom.at("items")[0].at("icon").at("glyph") == "symbol");
    CHECK(custom.at("items")[1].at("icon").at("tone") == "error");
    CHECK(run("test.icons.invalid") == true);
    CHECK(writes == 1);
    const auto deleted = run("test.icons.delete");
    CHECK(deleted.at("items")[0].at("icon").at("asset") == "material/cpp");
    const auto restored = run("test.icons.restore");
    CHECK(restored.at("items")[0].at("icon").at("asset") == "material/typescript-def");
    CHECK(restored.at("items")[1].at("icon").at("asset") == "material/cpp");
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("material cold loading yields and preserves legacy configuration") {
    auto channel = std::make_shared<Channel>();
    Json theme;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.icons.read") {
                return ready({{"theme", theme}, {"generation", "1"}});
            }
            if (method == "_ui.icons.update") {
                theme = params.at("theme");
                return ready({{"theme", theme}, {"generation", "1"}});
            }
            return ready(nullptr);
        },
        modules(),
        R"js(
        astra.configure({fileIcons: {
            fallback: {glyph:'document',tone:'accent'},
            folder: {glyph:'folder',tone:'#123456'}
        }});
        astra.registerCommand('test.pulse', () => 'responsive');
        astra.registerCommand('test.cold', async () => {
            const control = new AbortController();
            const cancelled = astra.ui.icons.resolve([{path:'cancel.py',directory:false}], {signal:control.signal})
                .then(()=>'unexpected', error=>error.name);
            const first = astra.ui.icons.resolve([{path:'main.cpp',directory:false}]);
            const second = astra.ui.icons.resolve([{path:'main.py',directory:false}]);
            await astra.sleep(0);
            await astra.sleep(0);
            control.abort();
            const cpp = await first;
            const python = await second;
            const frozen = !Reflect.set(cpp.items[0].icon, 'asset', 'material/python');
            const legacy = await astra.ui.icons.resolve([
                {path:'unknown',directory:false},{path:'ordinary',directory:true}
            ]);
            await astra.ui.icons.update({folder:{asset:'material/folder-src',openAsset:'material/folder-src-open'}});
            const resource = await astra.ui.icons.resolve([{path:'ordinary',directory:true}]);
            await astra.ui.icons.update({folder:{glyph:'warning',tone:'warning'}});
            const restored = await astra.ui.icons.resolve([{path:'ordinary',directory:true}]);
            return {cancelled:await cancelled,cpp,python,frozen,legacy,resource,restored};
        });
    )js");
    theme = workbench.default_settings().at("fileIcons");
    theme.merge_patch(workbench.settings().at("fileIcons"));
    std::optional<astra::script::Runtime::Outcome> cold, pulse;
    workbench.execute("test.cold", Json::object(), [&](const auto& result) {
        cold = result;
    });
    workbench.poll();
    workbench.execute("test.pulse", Json::object(), [&](const auto& result) {
        pulse = result;
    });
    for (int tick = 0; tick < 5 && !pulse; ++tick) {
        workbench.poll();
    }
    REQUIRE(pulse);
    REQUIRE_FALSE(pulse->error);
    CHECK(pulse->value == "responsive");
    CHECK_FALSE(cold);
    for (int tick = 0; tick < 500 && !cold; ++tick) {
        workbench.poll();
    }
    REQUIRE(cold);
    REQUIRE_FALSE(cold->error);
    CHECK(cold->value.at("cancelled") == "AbortError");
    CHECK(cold->value.at("cpp").at("items")[0].at("icon").at("asset") == "material/cpp");
    CHECK(cold->value.at("python").at("items")[0].at("icon").at("asset") == "material/python");
    CHECK(cold->value.at("frozen") == true);
    CHECK(cold->value.at("legacy").at("items")[0].at("icon").at("tone") == "accent");
    CHECK(cold->value.at("legacy").at("items")[1].at("icon").at("tone") == "#123456");
    CHECK(cold->value.at("resource").at("items")[0].at("icon").at("asset") ==
          "material/folder-src");
    CHECK(cold->value.at("restored").at("items")[0].at("icon").at("glyph") == "warning");
}

TEST_CASE("workbench file icons capture input and reject racing updates") {
    auto channel = std::make_shared<Channel>();
    Json theme;
    std::promise<Json> delayed;
    unsigned reads = 0, writes = 0;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.icons.read") {
                if (++reads == 1)
                    return delayed.get_future();
                return ready({{"theme", theme}, {"generation", "2"}});
            }
            if (method == "_ui.icons.update") {
                ++writes;
                CHECK(params.at("generation") == "1");
                throw astra::protocol::RpcError(-32001, "Theme changed while awaiting update");
            }
            return ready(nullptr);
        },
        modules(),
        R"js(
            astra.registerCommand('test.icons.capture', async () => {
                const paths = [{path: 'captured.cpp', directory: false}];
                const reading = astra.ui.icons.resolve(paths);
                paths[0].path = 'changed.json';
                return await reading;
            });
            astra.registerCommand('test.icons.race', async () => {
                try { await astra.ui.icons.update({fallback: {tone: 'error'}}); }
                catch (error) { return error.code; }
                throw new Error('Racing update accepted');
            });
            astra.registerCommand('test.icons.cancel', async () => {
                const control = new AbortController();
                const updating = astra.ui.icons.update({fallback: {tone: 'error'}}, {
                    signal: control.signal
                });
                control.abort();
                try { await updating; }
                catch (error) { return error.name; }
                throw new Error('Cancelled update accepted');
            });
        )js");
    theme = workbench.default_settings().at("fileIcons");
    SUBCASE("mutable paths are captured before read") {
        std::optional<astra::script::Runtime::Outcome> outcome;
        workbench.execute("test.icons.capture", Json::object(), [&](const auto& result) {
            outcome = result;
        });
        workbench.poll();
        CHECK_FALSE(outcome);
        delayed.set_value({{"theme", theme}, {"generation", "1"}});
        for (int tick = 0; tick < 300 && !outcome; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE_FALSE(outcome->error);
        CHECK(outcome->value.at("items")[0].at("path") == "captured.cpp");
        CHECK(outcome->value.at("items")[0].at("icon").at("asset") == "material/cpp");
    }
    SUBCASE("concurrent theme publication rejects stale generation") {
        std::optional<astra::script::Runtime::Outcome> outcome;
        workbench.execute("test.icons.race", Json::object(), [&](const auto& result) {
            outcome = result;
        });
        workbench.poll();
        delayed.set_value({{"theme", theme}, {"generation", "1"}});
        for (int tick = 0; tick < 300 && !outcome; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE_FALSE(outcome->error);
        CHECK(outcome->value == -32001);
        CHECK(writes == 1);
    }
    SUBCASE("cancelled theme read cannot publish a later update") {
        std::optional<astra::script::Runtime::Outcome> outcome;
        workbench.execute("test.icons.cancel", Json::object(), [&](const auto& result) {
            outcome = result;
        });
        workbench.poll();
        delayed.set_value({{"theme", theme}, {"generation", "1"}});
        for (int tick = 0; tick < 300 && !outcome; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        REQUIRE_FALSE(outcome->error);
        CHECK(outcome->value == "AbortError");
        CHECK(writes == 0);
    }
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
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
    Json badges;
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
                badges = params.at("badges");
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
    const auto undo = std::ranges::find(items, Json("Undo"));
    REQUIRE(undo != items.end());
    CHECK(badges.at(static_cast<std::size_t>(undo - items.begin())) == "Ctrl+Z");
    const auto custom = std::ranges::find(items, Json("Choose 中🙂"));
    REQUIRE(custom != items.end());
    CHECK(badges.at(static_cast<std::size_t>(custom - items.begin())) == "F9");
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
            if (method == "_ui.dialog")
                return confirmation.get_future();
            return views(method, params);
        },
        modules());
    workbench.execute(
        "ui.gitDiscard",
        {{"target", {{"path", "file.cpp"}, {"staged", false}, {"previewId", "captured-preview"}}}},
        {});
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

TEST_CASE("workbench mouse hover cancels debounce and joins UTF16 diagnostics with real policy") {
    auto channel = std::make_shared<Channel>();
    astra::ui::EditorTooltip tooltip;
    const Json document{{"id", "source"},
                        {"revision", "7"},
                        {"selections", Json::array({{{"anchor", 0}, {"head", 0}}})}};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document", document}});
            REQUIRE(method == "_ui.editorTooltip.publish");
            return ready(tooltip.publish(params));
        },
        modules());
    const Json target{{"id", "source"},
                      {"baseRevision", "7"},
                      {"byteOffset", 15},
                      {"line", 1},
                      {"utf16Column", 6}};
    tooltip.sync(target);
    workbench.emit("ui.editor.hover", *tooltip.take_request());
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    tooltip.sync(nullptr);
    workbench.emit("ui.editor.hover", *tooltip.take_request());
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());

    tooltip.sync(target);
    workbench.emit("ui.editor.hover", *tooltip.take_request());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (channel->requests.empty() && std::chrono::steady_clock::now() < deadline) {
        workbench.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "language.diagnostics");
    channel->reply({{"available", true},
                    {"revision", "7"},
                    {"diagnostics",
                     Json::array({{{"message", "Unicode 🙂 warning"},
                                   {"range",
                                    {{"start", {{"line", 1}, {"character", 6}}},
                                     {"end", {{"line", 1}, {"character", 7}}}}}},
                                  {{"message", "outside"},
                                   {"range",
                                    {{"start", {{"line", 1}, {"character", 0}}},
                                     {"end", {{"line", 1}, {"character", 6}}}}}}})}});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("params").at("byteOffset") == 15);
    CHECK(channel->requests.front().body.at("params").at("method") == "hover");
    channel->reply({{"revision", "7"},
                    {"result",
                     {{"contents",
                       {{"kind", "markdown"},
                        {"value", "```cpp\nint square(int value)\n```\n**Description**"}}}}}});
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    CHECK(tooltip.text().find("Unicode 🙂 warning") != std::string::npos);
    CHECK(tooltip.text().find("int square") != std::string::npos);
    CHECK(tooltip.text().find("outside") == std::string::npos);
    CHECK(tooltip.text().find("```") == std::string::npos);
    CHECK_FALSE(tooltip.pending());
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench Ctrl hover confirms definitions and rejects cancelled or obsolete results") {
    auto channel = std::make_shared<Channel>();
    astra::ui::EditorLink link;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            REQUIRE(method == "_ui.editorLink.publish");
            return ready(link.publish(params));
        },
        modules());
    const Json target{{"id", "source"}, {"baseRevision", "7"}, {"byteOffset", 15}};
    link.sync(target);
    workbench.emit("ui.editor.link", *link.take_request());
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    link.sync(nullptr);
    workbench.emit("ui.editor.link", *link.take_request());
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    for (const bool obsolete : {false, true}) {
        link.sync(target);
        workbench.emit("ui.editor.link", *link.take_request());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (channel->requests.empty() && std::chrono::steady_clock::now() < deadline) {
            workbench.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("params").at("method") == "definition");
        CHECK(channel->requests.front().body.at("params").at("byteOffset") == 15);
        channel->reply({{"revision", "7"},
                        {"obsolete", obsolete},
                        {"resolvedLocations", Json::array({{{"path", "main.cpp"}}})}});
        for (int tick = 0; tick < 24; ++tick)
            workbench.poll();
        CHECK(link.available() == !obsolete);
        CHECK_FALSE(link.pending());
        link.sync(nullptr);
        workbench.emit("ui.editor.link", *link.take_request());
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
    }
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench pointer definition captures hit offset and rejects stale document") {
    auto channel = std::make_shared<Channel>();
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.read")
                return ready({{"document",
                               {{"id", "source"},
                                {"revision", "7"},
                                {"selections", Json::array({{{"anchor", 0}, {"head", 0}}})}}}});
            return ready(nullptr);
        },
        modules());
    for (const auto& target : {Json{{"id", "other"}, {"baseRevision", "7"}, {"byteOffset", 15}},
                               Json{{"id", "source"}, {"baseRevision", "6"}, {"byteOffset", 15}}}) {
        workbench.execute("ui.definition", target, {});
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        CHECK(channel->requests.empty());
    }
    workbench.execute(
        "ui.definition", {{"id", "source"}, {"baseRevision", "7"}, {"byteOffset", 15}}, {});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("params").at("byteOffset") == 15);
    channel->reply({{"revision", "7"}, {"resolvedLocations", Json::array()}});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench hover bounds Unicode and settles unavailable or obsolete results") {
    Json diagnostics{{"available", true}, {"revision", "7"}, {"diagnostics", Json::array()}};
    Json hover{{"revision", "7"}, {"result", nullptr}};
    std::string expected;
    SUBCASE("large Unicode Markdown remains a bounded valid publication") {
        std::string text = "```cpp\nint square();\n```\n";
        for (unsigned i = 0; i < 10000; ++i)
            text += "🙂中";
        hover["result"] = {{"contents", Json::array({{{"value", text}}, {{"value", text}}})}};
        expected = "int square";
    }
    SUBCASE("unavailable language server reports its failure without a hover request") {
        diagnostics["available"] = false;
        diagnostics["reason"] = "Language server is unavailable";
        diagnostics["error"] = "process exited";
        expected = "process exited";
    }
    SUBCASE("obsolete hover completes the pending publication without text") {
        hover["obsolete"] = true;
    }
    auto channel = std::make_shared<Channel>();
    astra::ui::EditorTooltip tooltip;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"document", {{"id", "source"}, {"revision", "7"}}}});
            REQUIRE(method == "_ui.editorTooltip.publish");
            return ready(tooltip.publish(params));
        },
        modules());
    tooltip.sync({{"id", "source"},
                  {"baseRevision", "7"},
                  {"byteOffset", 0},
                  {"line", 0},
                  {"utf16Column", 0}});
    workbench.emit("ui.editor.hover", *tooltip.take_request());
    bool hover_requested = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        workbench.poll();
        if (!channel->requests.empty()) {
            const auto method = channel->requests.front().body.at("method");
            if (method == "language.diagnostics")
                channel->reply(diagnostics);
            else {
                REQUIRE(method == "language.request");
                hover_requested = true;
                channel->reply(hover);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while ((!workbench.idle() || tooltip.pending()) &&
             std::chrono::steady_clock::now() < deadline);
    CHECK_FALSE(tooltip.pending());
    CHECK(tooltip.text().size() <= 16384);
    if (expected.empty())
        CHECK(tooltip.text().empty());
    else
        CHECK(tooltip.text().find(expected) != std::string::npos);
    CHECK(hover_requested == diagnostics.at("available").get<bool>());
    CHECK(workbench.take_errors().empty());
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
                return ready({{"tabs", tabs}, {"tabDocuments", Json::array()}});
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

TEST_CASE("workbench window exit batches clean backgrounds and retains changed dirty documents") {
    auto channel = std::make_shared<Channel>();
    std::vector<std::string> tabs{"active", "dirty"};
    std::map<std::string, Json> documents;
    documents["active"] = {{"id", "active"}, {"revision", "7"}, {"dirty", false}};
    documents["dirty"] = {{"id", "dirty"}, {"revision", "7"}, {"dirty", true}};
    for (unsigned i = 0; i < 35; ++i) {
        const auto id = "clean" + std::to_string(i);
        tabs.push_back(id);
        documents[id] = {{"id", id}, {"revision", "7"}, {"dirty", false}};
    }
    std::vector<std::string> activated, statuses;
    std::optional<std::promise<Json>> dialog;
    Json spec;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read") {
                Json result{{"tabs", tabs},
                            {"document", documents.at(params.value("id", "active"))}};
                if (params.contains("tabLimit")) {
                    CHECK(params.at("tabLimit") == 32);
                    const auto first =
                        std::min(tabs.size(), params.at("tabOffset").get<std::size_t>());
                    result["tabDocuments"] = Json::array();
                    for (auto i = first; i < std::min(tabs.size(), first + 32); ++i) {
                        result["tabDocuments"].push_back(documents.at(tabs[i]));
                    }
                }
                return ready(result);
            }
            if (method == "_ui.barrier") {
                return ready(true);
            }
            if (method == "_ui.dialog") {
                spec = params;
                dialog.emplace();
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
            if (method == "_ui.update") {
                if (params.contains("closedDocument")) {
                    const auto id = params.at("closedDocument").get<std::string>();
                    CHECK_FALSE(documents.at(id).at("dirty").get<bool>());
                    std::erase(tabs, id);
                }
                if (params.contains("activate")) {
                    activated.push_back(params.at("activate"));
                }
                if (params.contains("status")) {
                    statuses.push_back(params.at("status"));
                }
            }
            CHECK(method != "_ui.window.close");
            return ready(nullptr);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("ui.closeWindow", Json::object(), [&](const auto& value) {
        outcome = value;
    });
    for (unsigned tick = 0; tick < 16; ++tick) {
        workbench.poll();
    }
    REQUIRE(channel->requests.size() == 32);
    CHECK(activated.empty());
    CHECK_FALSE(dialog);
    const auto changed_id = channel->requests.front().body.at("params").at("id").get<std::string>();
    int error_code = -32001;
    SUBCASE("stale revisions reenter a dirty close decision") {
        documents.at(changed_id)["revision"] = "8";
        documents.at(changed_id)["dirty"] = true;
    }
    SUBCASE("unsaved changes without a new text revision remain protected") {
        documents.at(changed_id)["dirty"] = true;
        error_code = -32003;
    }
    SUBCASE("other service failures stay visible and prevent checkpoint or teardown") {
        error_code = -32010;
    }
    for (unsigned index = 0; index < 32; ++index) {
        const auto& params = channel->requests.front().body.at("params");
        CHECK(channel->requests.front().body.at("method") == "document.close");
        CHECK(params.at("baseRevision") == "7");
        CHECK(params.at("discardChanges") == false);
        CHECK(params.at("id") != "active");
        CHECK(params.at("id") != "dirty");
        if (index == 0) {
            auto request = std::move(channel->requests.front());
            channel->requests.pop_front();
            request.response.set_value(
                Json{{"jsonrpc", "2.0"},
                     {"id", request.body.at("id")},
                     {"error", {{"code", error_code}, {"message", "close rejected"}}}}
                    .dump());
        } else {
            channel->reply({{"id", params.at("id")}, {"closed", true}});
        }
    }
    for (unsigned tick = 0; tick < 20; ++tick) {
        workbench.poll();
    }
    if (error_code == -32010) {
        REQUIRE(outcome);
        REQUIRE(outcome->error);
        CHECK(outcome->error->at("code") == -32010);
        CHECK(tabs.size() == 6);
        CHECK(channel->requests.empty());
    } else {
        REQUIRE(channel->requests.size() == 3);
        CHECK(std::ranges::find(statuses, "Documents changed while closing; checking again") !=
              statuses.end());
        for (unsigned index = 0; index < 3; ++index) {
            const auto id = channel->requests.front().body.at("params").at("id").get<std::string>();
            CHECK_FALSE(documents.at(id).at("dirty").get<bool>());
            channel->reply({{"id", id}, {"closed", true}});
        }
        for (unsigned tick = 0; tick < 20; ++tick) {
            workbench.poll();
        }
        REQUIRE(dialog);
        CHECK(spec.at("kind") == "choice");
        CHECK(tabs == std::vector<std::string>{"active", "dirty", changed_id});
        CHECK(channel->requests.empty());
        workbench.execute("ui.cancel", Json::object(), {});
        for (unsigned tick = 0; tick < 20; ++tick) {
            workbench.poll();
        }
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == false);
        CHECK(documents.at(changed_id).at("dirty") == true);
        CHECK(tabs.size() == 3);
    }
    CHECK(channel->requests.empty());
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

TEST_CASE("workbench Git diff owns native text actions and exact discard targets in JS") {
    auto channel = std::make_shared<Channel>();
    ViewHost views;
    std::promise<Json> selection;
    std::promise<Json> confirmation;
    Json panel, gutter, confirm;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"tabs", Json::array({"document-id"})},
                              {"document",
                               {{"id", "document-id"},
                                {"path", "file.cpp"},
                                {"revision", "7"},
                                {"dirty", false}}}});
            if (method == "_ui.update" && params.contains("gutter"))
                gutter = params.at("gutter");
            if (method == "_ui.dialog") {
                if (params.at("kind") == "text") {
                    panel = params;
                    return selection.get_future();
                }
                REQUIRE(params.at("kind") == "confirm");
                confirm = params;
                return confirmation.get_future();
            }
            return views(method, params);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("workbench.gitDiff", {{"path", "file.cpp"}}, [&](const auto& result) {
        outcome = result;
    });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "git.diff");
    channel->reply({{"patch", "--- old\r\n+++ new\r\n@@ hunk\r\n+中🙂\r\n+added\r\n-removed\r\n"},
                    {"previewId", "exact-preview"},
                    {"gutter", Json::array({{{"line", 0}, {"kind", "modified"}}})}});
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    REQUIRE(panel.is_object());
    CHECK(panel.at("monospace") == true);
    CHECK(panel.at("actions").size() == 3);
    CHECK(panel.at("lineTones") == Json::array({{{"begin", 2}, {"end", 3}, {"tone", "accent"}},
                                                {{"begin", 3}, {"end", 5}, {"tone", "added"}},
                                                {{"begin", 5}, {"end", 6}, {"tone", "removed"}}}));
    CHECK(gutter.at("id") == "document-id");
    CHECK(gutter.at("revision") == "7");
    CHECK(workbench.awaiting_input());
    CHECK_FALSE(outcome);
    CHECK(channel->requests.empty());
    SUBCASE("dismissal does not invoke Git or retain a discarded preview") {
        selection.set_value(nullptr);
        for (int tick = 0; tick < 10; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        workbench.execute("ui.gitDiscard", Json::object(), {});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        CHECK(confirm.is_null());
        CHECK(channel->requests.empty());
        CHECK(workbench.idle());
    }
    SUBCASE("discard action awaits a confirmation before issuing the captured preview") {
        selection.set_value("ui.gitDiscard");
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(confirm.is_object());
        CHECK(confirm.at("detail").get<std::string>().starts_with("file.cpp\n"));
        CHECK(channel->requests.empty());
        confirmation.set_value(false);
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(channel->requests.empty());
        CHECK(workbench.idle());
    }
    SUBCASE("stage action uses its captured diff path") {
        selection.set_value("ui.gitStage");
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "git.stage");
        CHECK(channel->requests.front().body.at("params").at("paths") == Json::array({"file.cpp"}));
        channel->reply({{"entries", Json::array()}});
        for (int tick = 0; tick < 30; ++tick) {
            views.views.poll();
            workbench.poll();
        }
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(workbench.idle());
    }
    SUBCASE("unadvertised action identities cannot dispatch service commands") {
        selection.set_value("ui.gitCommit");
        for (int tick = 0; tick < 12; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(channel->requests.empty());
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench Git text projection handles asynchronous pages and staged binary panels") {
    auto channel = std::make_shared<Channel>();
    Json panel;
    unsigned reads{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read") {
                ++reads;
                return ready({{"tabs", Json::array()}});
            }
            if (method == "_ui.dialog") {
                panel = params;
                return ready(nullptr);
            }
            return ready(nullptr);
        },
        modules());
    bool staged{};
    Json model;
    SUBCASE("working-tree tones retain row identity across yields and mixed newlines") {
        std::string text;
        for (int row = 0; row < 1536; ++row) {
            text += "+中🙂";
            text += row % 3 == 0 ? "\r\n" : (row % 3 == 1 ? "\r" : "\n");
        }
        model = {{"patch", text}, {"previewId", "preview"}};
    }
    SUBCASE("staged binary comparisons offer no discard or working-tree gutters") {
        staged = true;
        model = {{"binary", true}, {"patch", "+ignored"}, {"previewId", "preview"}};
    }
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("workbench.gitDiff",
                      {{"path", "captured.cpp"}, {"staged", staged}},
                      [&](const auto& result) {
                          outcome = result;
                      });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("params").at("staged") == staged);
    channel->reply(model);
    for (int tick = 0; tick < 2000 && !outcome; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    REQUIRE(panel.is_object());
    if (staged) {
        CHECK(reads == 0);
        CHECK(panel.at("actions").size() == 2);
        CHECK(panel.at("lineTones").empty());
        CHECK(panel.at("text") == "Binary file — textual diff is unavailable.");
    } else {
        CHECK(panel.at("actions").size() == 3);
        CHECK(panel.at("lineTones") ==
              Json::array({{{"begin", 0}, {"end", 1536}, {"tone", "added"}}}));
    }
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench delayed Git stage preserves an unrelated modal session") {
    auto channel = std::make_shared<Channel>();
    ViewHost views;
    std::promise<Json> selection, unrelated;
    bool unrelated_open{}, unrelated_dismissed{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"tabs", Json::array()}});
            if (method == "_ui.dialog") {
                if (params.at("title") == "Unrelated panel") {
                    unrelated_open = true;
                    return unrelated.get_future();
                }
                return selection.get_future();
            }
            if (method == "_ui.update" && params.value("dismissPrompt", false) && unrelated_open)
                unrelated_dismissed = true;
            return views(method, params);
        },
        modules(),
        "astra.ui.registerCommand({id:'acceptance.other',title:'Unrelated panel'},"
        "async()=>await astra.ui.showText({title:'Unrelated panel',text:'Other content'}));");
    std::optional<astra::script::Runtime::Outcome> stage, other;
    workbench.execute("workbench.gitDiff", {{"path", "captured.cpp"}}, [&](const auto& outcome) {
        stage = outcome;
    });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    channel->reply({{"patch", "+change"}, {"previewId", "preview"}});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(workbench.awaiting_input());
    selection.set_value("ui.gitStage");
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "git.stage");
    workbench.execute("acceptance.other", Json::object(), [&](const auto& outcome) {
        other = outcome;
    });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(unrelated_open);
    channel->reply({{"entries", Json::array()}});
    for (int tick = 0; tick < 30; ++tick) {
        views.views.poll();
        workbench.poll();
    }
    REQUIRE(stage);
    CHECK_FALSE(stage->error);
    CHECK_FALSE(unrelated_dismissed);
    CHECK_FALSE(other);
    CHECK(workbench.awaiting_input());
    unrelated.set_value(nullptr);
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(other);
    CHECK_FALSE(other->error);
    CHECK(workbench.idle());
}

TEST_CASE("workbench direct Git action cancels and drains only its comparison session") {
    auto channel = std::make_shared<Channel>();
    ViewHost views;
    std::promise<Json> presentation;
    std::string dialog_id;
    unsigned cancelled{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read")
                return ready({{"tabs", Json::array()}});
            if (method == "_ui.dialog") {
                dialog_id = params.at("id").get<std::string>();
                return presentation.get_future();
            }
            if (method == "_ui.dialog.cancel" && params.at("id") == dialog_id) {
                if (!cancelled++)
                    presentation.set_value(nullptr);
                return ready(nullptr);
            }
            return views(method, params);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> diff, stage;
    workbench.execute("workbench.gitDiff", {{"path", "captured.cpp"}}, [&](const auto& result) {
        diff = result;
    });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    channel->reply({{"patch", "+change"}, {"previewId", "preview"}});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(workbench.awaiting_input());
    workbench.execute("ui.gitStage", Json::object(), [&](const auto& result) {
        stage = result;
    });
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    CHECK(cancelled > 0);
    REQUIRE(diff);
    REQUIRE(diff->error);
    CHECK(diff->error->at("name") == "AbortError");
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "git.stage");
    CHECK(channel->requests.front().body.at("params").at("paths") == Json::array({"captured.cpp"}));
    channel->reply({{"entries", Json::array()}});
    for (int tick = 0; tick < 30; ++tick) {
        views.views.poll();
        workbench.poll();
    }
    REQUIRE(stage);
    CHECK_FALSE(stage->error);
    CHECK(workbench.idle());
}

TEST_CASE("workbench Git cancellation drains a pending gutter metadata read") {
    auto channel = std::make_shared<Channel>();
    ViewHost views;
    std::promise<Json> metadata;
    std::string read_id;
    unsigned cancelled{}, panels{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.read") {
                read_id = params.at("readId").get<std::string>();
                return metadata.get_future();
            }
            if (method == "_ui.read.cancel") {
                CHECK(params.at("readId") == read_id);
                if (!cancelled++)
                    metadata.set_value(nullptr);
                return ready(nullptr);
            }
            if (method == "_ui.dialog")
                ++panels;
            return views(method, params);
        },
        modules());
    std::optional<astra::script::Runtime::Outcome> diff, stage;
    workbench.execute("workbench.gitDiff", {{"path", "captured.cpp"}}, [&](const auto& value) {
        diff = value;
    });
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    channel->reply({{"patch", "+change"}, {"previewId", "preview"}});
    for (int tick = 0; tick < 6; ++tick)
        workbench.poll();
    REQUIRE_FALSE(read_id.empty());
    workbench.execute("ui.gitStage", Json::object(), [&](const auto& value) {
        stage = value;
    });
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    CHECK(cancelled > 0);
    REQUIRE(diff);
    REQUIRE(diff->error);
    CHECK(diff->error->at("name") == "AbortError");
    CHECK(panels == 0);
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "git.stage");
    CHECK(channel->requests.front().body.at("params").at("paths") == Json::array({"captured.cpp"}));
    channel->reply({{"entries", Json::array()}});
    for (int tick = 0; tick < 30; ++tick) {
        views.views.poll();
        workbench.poll();
    }
    REQUIRE(stage);
    CHECK_FALSE(stage->error);
    CHECK(workbench.idle());
}

TEST_CASE("workbench separates shipped style defaults from explicit client preferences") {
    auto channel = std::make_shared<Channel>();
    astra::ui::Workbench workbench(
        channel,
        [](std::string_view, const Json&) {
            return ready(nullptr);
        },
        modules(),
        "astra.configure({workbench:{uiFontSize:13},theme:{accent:'#123456'}});");
    const auto defaults = workbench.default_settings();
    CHECK(defaults.at("theme").at("accent") == "#0078D4");
    CHECK(defaults.at("workbench").at("uiFontFamilies").size() == 3);
    const auto preferences = workbench.settings();
    CHECK(preferences.at("theme").size() == 1);
    CHECK_FALSE(preferences.at("workbench").contains("sidebarWidth"));
    auto effective = defaults;
    effective.merge_patch({{"workbench", {{"uiFontSize", 11}, {"sidebarWidth", 320}}},
                           {"theme", {{"accent", "#ABCDEF"}}}});
    effective.merge_patch(preferences);
    CHECK(effective.at("workbench").at("uiFontSize") == 13);
    CHECK(effective.at("workbench").at("sidebarWidth") == 320);
    CHECK(effective.at("theme").at("accent") == "#123456");
    CHECK(channel->requests.empty());
}

TEST_CASE("workbench public UI requests cancel before native mutation") {
    auto channel = std::make_shared<Channel>();
    unsigned dispatched{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view, const Json&) {
            ++dispatched;
            return ready(nullptr);
        },
        modules(),
        R"js(
        astra.registerCommand('acceptance.cancelUi', async ({preAborted}) => {
            const calls = [
                options => astra.ui.update({status: 'Must not publish'}, options),
                options => astra.ui.forms.open({id: 'must-not-open'}, options),
                options => astra.ui.forms.read('form', options),
                options => astra.ui.forms.update({id: 'form', message: 'Must not publish'}, options),
                options => astra.ui.forms.close('form', options),
                options => astra.ui.forms.focus('form', 'text', false, options),
                options => astra.ui.forms.blur('form', options),
                options => astra.ui.forms.setValues({id: 'form', values: {text: 'Must not replace'}}, options),
                options => astra.ui.clipboard.read(options),
                options => astra.ui.clipboard.write('Must not replace', options)
            ];
            const outcomes = [];
            for (const call of calls) {
                const controller = new AbortController();
                if (preAborted) {
                    controller.abort();
                }
                const pending = call({signal: controller.signal});
                if (!(pending instanceof Promise)) {
                    throw Error('UI API must return Promise');
                }
                controller.abort();
                try {
                    await pending;
                    outcomes.push('Unexpected success');
                } catch (error) {
                    outcomes.push(error.name);
                }
            }
            return outcomes;
        });
        )js");
    for (const bool pre_aborted : {false, true}) {
        std::optional<astra::script::Runtime::Outcome> result;
        workbench.execute(
            "acceptance.cancelUi", {{"preAborted", pre_aborted}}, [&](const auto& value) {
                result = value;
            });
        for (int tick = 0; tick < 32; ++tick)
            workbench.poll();
        REQUIRE(result);
        REQUIRE_FALSE(result->error);
        CHECK(result->value == std::vector<std::string>(10, "AbortError"));
        CHECK(dispatched == 0);
        CHECK(channel->requests.empty());
        CHECK(workbench.take_errors().empty());
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench public UI request deadlines discard late native results") {
    const std::vector<std::string> methods{"_ui.update",
                                           "_ui.form.open",
                                           "_ui.form.read",
                                           "_ui.form.update",
                                           "_ui.form.close",
                                           "_ui.form.focus",
                                           "_ui.form.blur",
                                           "_ui.form.setValues",
                                           "_ui.clipboard.read",
                                           "_ui.clipboard.write"};
    for (std::size_t index = 0; index < methods.size(); ++index) {
        CAPTURE(methods[index]);
        auto channel = std::make_shared<Channel>();
        std::promise<Json> native;
        unsigned dispatched{};
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                if (method == "_ui.clipboard.cancel") {
                    CHECK(params.at("id").is_string());
                    return ready(nullptr);
                }
                CHECK(method == methods[index]);
                if (method == "_ui.form.focus")
                    CHECK(params.at("selectAll") == false);
                if (method == "_ui.form.focus")
                    CHECK(params.at("field") == "text");
                ++dispatched;
                return native.get_future();
            },
            modules(),
            R"js(
            astra.registerCommand('acceptance.deadlineUi', async ({index}) => {
                const calls = [
                    options => astra.ui.update({status:'Pending'}, options),
                    options => astra.ui.forms.open({id:'form'}, options),
                    options => astra.ui.forms.read('form', options),
                    options => astra.ui.forms.update({id:'form'}, options),
                    options => astra.ui.forms.close('form', options),
                    options => astra.ui.forms.focus('form', 'text', false, options),
                    options => astra.ui.forms.blur('form', options),
                    options => astra.ui.forms.setValues({id:'form', values:{text:'Pending'}}, options),
                    options => astra.ui.clipboard.read(options),
                    options => astra.ui.clipboard.write('Pending', options)
                ];
                try {
                    await calls[index]({timeoutMs: 30});
                    await astra.ui.update({status:'Must not continue after timeout'});
                    return 'Unexpected success';
                } catch (error) {
                    return error.name;
                }
            });
            )js");
        std::optional<astra::script::Runtime::Outcome> result;
        workbench.execute("acceptance.deadlineUi", {{"index", index}}, [&](const auto& value) {
            result = value;
        });
        workbench.poll();
        REQUIRE(dispatched == 1);
        REQUIRE_FALSE(result);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!result && std::chrono::steady_clock::now() < deadline) {
            workbench.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(result);
        CHECK_FALSE(result->error);
        CHECK(result->value == "TimeoutError");
        native.set_value("Late result must not resume the caller");
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        CHECK(dispatched == 1);
        CHECK(channel->requests.empty());
        CHECK(workbench.take_errors().empty());
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench style APIs await native completion and return authoritative values") {
    auto channel = std::make_shared<Channel>();
    std::promise<Json> reading, updating;
    unsigned reads{}, updates{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.style.read") {
                ++reads;
                return reading.get_future();
            }
            if (method == "_ui.style.update") {
                ++updates;
                CHECK(params.at("patch") == Json{{"workbench", {{"uiFontSize", 16}}}});
                CHECK(params.at("id").is_string());
                return updating.get_future();
            }
            throw std::logic_error("Unexpected style API request");
        },
        modules(),
        R"js(
            astra.registerCommand('acceptance.style', async () => {
                const read = astra.ui.style.read();
                if (!(read instanceof Promise)) throw Error('Style read must return Promise');
                const current = await read;
                const update = astra.ui.style.update({workbench: {uiFontSize: 16}});
                if (!(update instanceof Promise)) throw Error('Style update must return Promise');
                return {current, updated: await update};
            });
        )js");
    std::optional<astra::script::Runtime::Outcome> result;
    workbench.execute("acceptance.style", Json::object(), [&](const auto& outcome) {
        result = outcome;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(reads == 1);
    CHECK(updates == 0);
    CHECK_FALSE(result);
    reading.set_value({{"workbench", {{"uiFontSize", 13}}}});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    CHECK(updates == 1);
    CHECK_FALSE(result);
    updating.set_value({{"workbench", {{"uiFontSize", 16}}}});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(result);
    CHECK_FALSE(result->error);
    CHECK(result->value.at("current").at("workbench").at("uiFontSize") == 13);
    CHECK(result->value.at("updated").at("workbench").at("uiFontSize") == 16);
    CHECK(channel->requests.empty());
}

TEST_CASE("workbench JS textobject commands capture fenced document and selection guards") {
    for (const auto& [command, kind, inner] :
         std::vector<std::tuple<std::string, std::string, bool>>{
             {"ui.selectFunction", "function", false},
             {"ui.selectClass", "class", false},
             {"ui.selectBlock", "block", false},
             {"ui.selectString", "string", true}}) {
        auto channel = std::make_shared<Channel>();
        std::promise<Json> reading;
        std::vector<Json> updates;
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                if (method == "_ui.read") {
                    CHECK(params.contains("readId"));
                    return reading.get_future();
                }
                CHECK(method == "_ui.update");
                updates.push_back(params);
                return ready(true);
            },
            modules());
        std::optional<astra::script::Runtime::Outcome> outcome;
        workbench.execute(command, Json::object(), [&](const auto& result) {
            outcome = result;
        });
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        CHECK(channel->requests.empty());
        CHECK_FALSE(outcome);
        const auto selections = Json::array({{{"anchor", 12}, {"head", 12}}});
        reading.set_value(
            {{"document", {{"id", "captured"}, {"revision", "7"}, {"selections", selections}}}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "document.syntax");
        CHECK(channel->requests.front().body.at("params") == Json{{"id", "captured"},
                                                                  {"baseRevision", "7"},
                                                                  {"begin", 0},
                                                                  {"end", 0},
                                                                  {"includeOutline", false}});
        channel->reply({{"id", "captured"}, {"revision", "7"}, {"state", "ready"}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        CHECK(channel->requests.front().body.at("method") == "document.selectObject");
        CHECK(channel->requests.front().body.at("params") == Json{{"id", "captured"},
                                                                  {"baseRevision", "7"},
                                                                  {"baseSelections", selections},
                                                                  {"kind", kind},
                                                                  {"inner", inner},
                                                                  {"expand", false}});
        const auto selected = Json{{"id", "captured"},
                                   {"revision", "7"},
                                   {"selections", Json::array({{{"anchor", 4}, {"head", 30}}})}};
        channel->reply(selected);
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(outcome);
        CHECK_FALSE(outcome->error);
        CHECK(outcome->value == selected);
        REQUIRE(updates.size() == 1);
        CHECK(updates.front() == Json{{"document", selected}, {"revealCaretFor", "captured"}});
        CHECK(channel->requests.empty());
    }
}

TEST_CASE("workbench JS textobject API rejects invalid flags and retires unavailable syntax") {
    for (const auto* state : {"unsupported", "obsolete"}) {
        auto channel = std::make_shared<Channel>();
        std::vector<Json> updates;
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                if (method == "_ui.read")
                    return ready({{"document",
                                   {{"id", "source"},
                                    {"revision", "9"},
                                    {"selections", Json::array({{{"anchor", 0}, {"head", 0}}})}}}});
                CHECK(method == "_ui.update");
                updates.push_back(params);
                return ready(true);
            },
            modules(),
            R"js(
            astra.registerCommand('acceptance.invalidObject', async () => {
                for (const [kind, params] of [['missing', {}], ['function', {inner: 'true'}]]) {
                    const selecting = astra.ui.selectObject(kind, params);
                    if (!(selecting instanceof Promise)) throw Error('Selection must be async');
                    let rejected = false;
                    try {
                        await selecting;
                    } catch (error) {
                        rejected = error.name === 'TypeError';
                    }
                    if (!rejected) throw Error('Invalid syntax object options were accepted');
                }
            });
        )js");
        std::optional<astra::script::Runtime::Outcome> result;
        workbench.execute("acceptance.invalidObject", Json::object(), [&](const auto& outcome) {
            result = outcome;
        });
        for (int tick = 0; tick < 10; ++tick)
            workbench.poll();
        REQUIRE(result);
        CHECK_FALSE(result->error);
        CHECK(channel->requests.empty());
        result.reset();
        workbench.execute("ui.selectFunction", Json::object(), [&](const auto& outcome) {
            result = outcome;
        });
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(channel->requests.size() == 1);
        channel->reply({{"id", "source"}, {"revision", "9"}, {"state", state}});
        for (int tick = 0; tick < 8; ++tick)
            workbench.poll();
        REQUIRE(result);
        CHECK_FALSE(result->error);
        CHECK(result->value.is_null());
        CHECK(channel->requests.empty());
        REQUIRE(updates.size() == 1);
        CHECK(updates.front().contains("status"));
        CHECK_FALSE(updates.front().contains("document"));
    }
}

TEST_CASE("workbench JS Outline reads bounded syntax pages and owns symbol activation") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ViewPage page;
    page.sync({"captured", "3", "2", 60, 2, true});
    unsigned updates{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.outline.publish")
                return ready(page.publish(params));
            if (method == "_ui.outline.activate")
                return ready(page.activate(params.at("version").get<std::string>(),
                                           params.at("row").get<std::string>()));
            if (method == "_ui.read")
                return ready({{"activity", "2"},
                              {"document",
                               {{"id", "captured"},
                                {"revision", "3"},
                                {"selections", Json::array({{{"anchor", 0}, {"head", 0}}})}}}});
            CHECK(method == "_ui.update");
            ++updates;
            CHECK(params.at("revealCaretFor") == "captured");
            return ready(nullptr);
        },
        modules());
    workbench.emit("ui.outline.request", *page.take_request());
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    const auto requested = channel->requests.front().body.at("params");
    CHECK(channel->requests.front().body.at("method") == "document.syntax");
    CHECK(requested.at("baseRevision") == "3");
    CHECK(requested.at("outlineBegin") == 60);
    CHECK(requested.at("outlineLimit") == 2);
    CHECK(requested.at("begin") == 0);
    CHECK(requested.at("end") == 0);
    channel->reply({{"id", "captured"},
                    {"revision", "3"},
                    {"state", "ready"},
                    {"outlinePage", {{"offset", 60}, {"total", 81}}},
                    {"outline",
                     Json::array({{{"name", "函数中🙂"},
                                   {"kind", "function"},
                                   {"depth", 5},
                                   {"selectionBegin", 72},
                                   {"selectionEnd", 91}},
                                  {{"name", "Nested"},
                                   {"kind", "class"},
                                   {"depth", 12},
                                   {"selectionBegin", 100},
                                   {"selectionEnd", 120}}})}});
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    CHECK_FALSE(page.pending());
    REQUIRE(page.rows().size() == 2);
    CHECK(page.rows().at(0).depth == 5);
    CHECK(page.rows().at(1).depth == 8);
    CHECK(page.rows().at(0).value->at("icon") == "function");
    CHECK(page.rows().at(0).value->at("iconTone") == "accent");
    std::optional<astra::script::Runtime::Outcome> outcome;
    workbench.execute("workbench.outline.activate",
                      {{"version", page.state().at("version")}, {"row", "symbol-60"}},
                      [&](const auto& result) {
                          outcome = result;
                      });
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.setSelections");
    CHECK(channel->requests.front().body.at("params").at("baseRevision") == "3");
    CHECK(channel->requests.front().body.at("params").at("selections") ==
          Json::array({{{"anchor", 72}, {"head", 91}}}));
    channel->reply({{"id", "captured"}, {"revision", "3"}});
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(updates == 1);
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench JS Outline supersession drains late service replies without publication") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ViewPage page;
    page.sync({"old", "1", "1", 0, 1, true});
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            REQUIRE(method == "_ui.outline.publish");
            return ready(page.publish(params));
        },
        modules());
    workbench.emit("ui.outline.request", *page.take_request());
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    channel->reply({{"id", "old"},
                    {"revision", "1"},
                    {"state", "pending"},
                    {"operationId", "shared-parse"},
                    {"shared", true}});
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.get");
    page.sync({"new", "2", "2", 0, 1, true});
    workbench.emit("ui.outline.request", *page.take_request());
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 2);
    channel->reply({{"state", "completed"},
                    {"result", {{"id", "old"}, {"revision", "1"}, {"state", "ready"}}}});
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    CHECK(page.pending());
    CHECK(page.state().at("id") == "new");
    channel->reply({{"id", "new"}, {"revision", "2"}, {"state", "unsupported"}});
    for (int tick = 0; tick < 16; ++tick)
        workbench.poll();
    CHECK_FALSE(page.pending());
    CHECK(page.message() == "No parser for this file type");
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench JS Outline failures settle with bounded scalar-safe messages") {
    for (const bool cancelled : {false, true}) {
        auto channel = std::make_shared<Channel>();
        astra::ui::ViewPage page;
        page.sync({"1", "3", "1", 0, 2, true});
        const auto source =
            std::string("const outlineCancelled = ") + (cancelled ? "true;" : "false;") + R"js(
                astra.ui.registerOutline({loading: 'Loading custom symbols'}, async () => {
                    const error = new Error('🙂'.repeat(700) + '\n failure');
                    if (outlineCancelled) error.name = 'AbortError';
                    throw error;
                });
            )js";
        astra::ui::Workbench workbench(
            channel,
            [&](std::string_view method, const Json& params) {
                REQUIRE(method == "_ui.outline.publish");
                return ready(page.publish(params));
            },
            modules(),
            source);
        workbench.emit("ui.outline.request", *page.take_request());
        for (int tick = 0; tick < 20; ++tick)
            workbench.poll();
        CHECK_FALSE(page.pending());
        CHECK(page.rows().size() == 0);
        CHECK(page.message().size() == 512 * 4);
        CHECK(workbench.take_errors().size() == (cancelled ? 0U : 1U));
        CHECK(channel->requests.empty());
        CHECK(workbench.idle());
    }
}

TEST_CASE("workbench JS Outline retired failures stay quiet before the next viewport event") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ViewPage page;
    page.sync({"1", "3", "1", 0, 1, true});
    unsigned rejected{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.outline.refresh") {
                page.refresh();
                return ready(page.state());
            }
            REQUIRE(method == "_ui.outline.publish");
            const auto accepted = page.publish(params);
            if (!accepted)
                ++rejected;
            return ready(accepted);
        },
        modules(),
        R"js(
            let firstOutlineRequest = true;
            astra.ui.registerOutline({loading: 'Loading custom symbols'}, async () => {
                if (firstOutlineRequest) {
                    firstOutlineRequest = false;
                    await astra.ui.outline.refresh();
                    throw new Error('Retired provider failure');
                }
                return {
                    offset: 0, total: 1, empty: 'No symbols',
                    rows: [{id: 'current', label: 'Current symbol'}]
                };
            });
        )js");
    workbench.emit("ui.outline.request", *page.take_request());
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    CHECK(rejected == 1);
    CHECK(page.pending());
    CHECK(page.needs_request());
    CHECK(page.message().empty());
    CHECK(workbench.take_errors().empty());
    workbench.emit("ui.outline.request", *page.take_request());
    for (int tick = 0; tick < 20; ++tick)
        workbench.poll();
    CHECK_FALSE(page.pending());
    REQUIRE(page.rows().size() == 1);
    CHECK(page.rows().at(0).value->at("id") == "current");
    CHECK(workbench.take_errors().empty());
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench JS chrome projects coordinates paths and dispatches registered actions") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ChromeState state;
    state.sync(
        {{"workspace", "Workspace 中🙂"},
         {"branch", "main"},
         {"message", "Ready"},
         {"activity", "7"},
         {"activeView", "explorer"},
         {"sidebarVisible", true},
         {"accepting", true},
         {"views", Json::array({{{"id", "explorer"}, {"title", "Explorer"}, {"icon", "files"}}})},
         {"document",
          {{"id", "1"},
           {"revision", "3"},
           {"path", "src/中🙂/module.cpp"},
           {"encoding", "UTF-8"},
           {"tabSize", 4},
           {"insertSpaces", true},
           {"languageActive", true},
           {"language", {{"errors", 3}, {"warnings", 2}, {"pending", false}}},
           {"caret", {{"line", 2}, {"utf16Column", 4}, {"visualColumn", 7}}}}}});
    unsigned activated{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.chrome.publish")
                return ready(state.publish(params));
            if (method == "_ui.chrome.activate")
                return ready(state.activate(params.at("version").get<std::string>(),
                                            params.at("bank").get<std::string>(),
                                            params.at("row").get<std::string>()));
            REQUIRE(method == "_ui.update");
            CHECK(params.at("status") == "Registered header action");
            ++activated;
            return ready(nullptr);
        },
        modules(),
        R"js(
        astra.registerCommand('ui.commandPalette', async () => {
            await astra.ui.update({status: 'Registered header action'});
        });
    )js");
    workbench.emit("ui.chrome.request", *state.take_request());
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    REQUIRE_FALSE(state.pending());
    CHECK(state.model().at("brand") == "Astra");
    CHECK(state.model().at("hint") == "Ctrl+Shift+P");
    CHECK(state.model().at("activity")[0].at("selected") == true);
    CHECK(state.model().at("breadcrumb").size() == 3);
    CHECK(state.model().at("breadcrumb")[1].at("label") == "中🙂");
    CHECK(state.model().at("statusRight")[0].at("label") == "Ln 3, Col 8");
    CHECK(state.model().at("statusRight")[2].at("label") == "Spaces: 4");
    CHECK(state.model().at("statusLeft").back().at("label") == "3 errors · 2 warnings");
    CHECK(state.model().at("statusLeft").back().at("tone") == "error");
    CHECK(state.model().at("statusLeft").back().at("data").at("name") == "ui.diagnostics");
    workbench.execute(
        "workbench.chrome.activate",
        {{"version", state.state().at("version")}, {"bank", "command"}, {"row", "commands"}},
        {});
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    CHECK(activated == 1);
    CHECK(channel->requests.empty());
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench JS chrome body projects welcome notice and recovery command policy") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ChromeState state;
    state.sync({{"workspace", "Body"},
                {"views", Json::array()},
                {"document", nullptr},
                {"activeView", "explorer"},
                {"sidebarVisible", false},
                {"accepting", true},
                {"recoveryAvailable", true},
                {"noticeSequence", "17"},
                {"notice", "File changed 中🙂\nBuffer preserved"}});
    std::vector<std::string> actions;
    Json notice_parameters;
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.chrome.publish")
                return ready(state.publish(params));
            if (method == "_ui.chrome.activate")
                return ready(state.activate(params.at("version").get<std::string>(),
                                            params.at("bank").get<std::string>(),
                                            params.at("row").get<std::string>()));
            if (method == "_ui.chrome.reject")
                return ready(state.reject(params.at("version").get<std::string>()));
            REQUIRE(method == "_ui.update");
            if (params.contains("capturedNotice")) {
                notice_parameters = params.at("capturedNotice");
                return ready(nullptr);
            }
            actions.push_back(params.at("status").get<std::string>());
            return ready(nullptr);
        },
        modules(),
        R"js(
        for (const name of ['ui.new','ui.quickOpen','ui.commandPalette','ui.recovery','ui.dismissWorkspaceNotice']) {
            astra.registerCommand(name, async (parameters) => {
                if (name === 'ui.dismissWorkspaceNotice')
                    await astra.ui.update({capturedNotice:parameters});
                await astra.ui.update({status:name});
            });
        }
        astra.registerCommand('acceptance.body.unicodeBadge', () => {
            astra.configure({workbench:{commands:{'ui.new':{shortcut:'🙂'.repeat(20)}}}});
        });
        )js");
    workbench.emit("ui.chrome.request", *state.take_request());
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    REQUIRE_FALSE(state.pending());
    CHECK(state.model().at("welcomeTitle")[0].at("label") == "Astra");
    CHECK(state.model().at("welcomeActions")[0].at("badge") == "Ctrl+N");
    CHECK(state.model().at("notices")[0].at("badge") == "Review");
    CHECK(state.model().at("notices")[1].at("label") == "File changed 中🙂 Buffer preserved");
    for (const auto [bank, row] : {std::pair{"welcomeActions", "new"},
                                   {"welcomeActions", "open"},
                                   {"welcomeActions", "commands"},
                                   {"notices", "recovery"},
                                   {"notices", "workspace"}}) {
        workbench.execute("workbench.chrome.activate",
                          {{"version", state.state().at("version")}, {"bank", bank}, {"row", row}},
                          {});
        for (int tick = 0; tick < 24; ++tick)
            workbench.poll();
    }
    const std::vector<std::string> expected{
        "ui.new", "ui.quickOpen", "ui.commandPalette", "ui.recovery", "ui.dismissWorkspaceNotice"};
    CHECK(actions == expected);
    CHECK(notice_parameters == Json{{"noticeSequence", "17"}});
    workbench.execute("acceptance.body.unicodeBadge", Json::object(), {});
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    state.refresh();
    workbench.emit("ui.chrome.request", *state.take_request());
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    REQUIRE_FALSE(state.pending());
    std::string expected_badge;
    for (int scalar = 0; scalar < 16; ++scalar)
        expected_badge += "🙂";
    CHECK(state.model().at("welcomeActions")[0].at("badge") == expected_badge);
    CHECK(channel->requests.empty());
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench JS chrome retired provider failure is suppressed before its next event") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ChromeState state;
    state.sync({{"accepting", true}});
    unsigned rejected{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            if (method == "_ui.chrome.refresh") {
                state.refresh();
                return ready(state.state());
            }
            if (method == "_ui.chrome.reject") {
                const auto accepted = state.reject(params.at("version").get<std::string>());
                if (!accepted)
                    ++rejected;
                return ready(accepted);
            }
            REQUIRE(method == "_ui.chrome.publish");
            return ready(state.publish(params));
        },
        modules(),
        R"js(
        let firstChrome = true;
        astra.ui.registerChrome(async () => {
            if (firstChrome) {
                firstChrome = false;
                await astra.ui.chrome.refresh();
                throw Error('Retired chrome error');
            }
            return {brand: 'Current', hint: '', command: [], activity: [], manage: [],
                    breadcrumb: [], statusLeft: [], statusRight: []};
        });
    )js");
    workbench.emit("ui.chrome.request", *state.take_request());
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    CHECK(rejected == 1);
    CHECK(state.pending());
    CHECK(workbench.take_errors().empty());
    workbench.emit("ui.chrome.request", *state.take_request());
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    CHECK_FALSE(state.pending());
    CHECK(state.model().at("brand") == "Current");
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench JS chrome deadline settles a signal-ignoring provider") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ChromeState state;
    state.sync({{"accepting", true}});
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json& params) {
            REQUIRE(method == "_ui.chrome.reject");
            return ready(state.reject(params.at("version").get<std::string>()));
        },
        modules(),
        R"js(
        astra.ui.registerChrome(() => new Promise(() => {}), {}, {timeoutMs: 20});
    )js");
    workbench.emit("ui.chrome.request", *state.take_request());
    for (int tick = 0; tick < 100 && (tick < 4 || state.pending() || !workbench.idle()); ++tick) {
        workbench.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK_FALSE(state.pending());
    const auto errors = workbench.take_errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().at("name") == "TimeoutError");
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE(
    "workbench JS chrome shutdown context cancels a provider without waiting for its deadline") {
    auto channel = std::make_shared<Channel>();
    astra::ui::ChromeState state;
    state.sync({{"accepting", true}});
    astra::ui::Workbench workbench(
        channel,
        [](std::string_view, const Json&) {
            FAIL("Retired chrome provider reached the native host");
            return ready(nullptr);
        },
        modules(),
        R"js(
        astra.ui.registerChrome(() => new Promise(() => {}));
    )js");
    workbench.emit("ui.chrome.request", *state.take_request());
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE_FALSE(workbench.idle());
    state.sync({{"accepting", false}});
    workbench.emit("ui.chrome.request", *state.take_request());
    for (int tick = 0; tick < 24; ++tick)
        workbench.poll();
    CHECK(workbench.take_errors().empty());
    CHECK(channel->requests.empty());
    CHECK(workbench.idle());
}

TEST_CASE("workbench JS textobject cancellation drains parsing without selecting") {
    auto channel = std::make_shared<Channel>();
    unsigned updates{};
    astra::ui::Workbench workbench(
        channel,
        [&](std::string_view method, const Json&) {
            if (method == "_ui.read")
                return ready({{"document",
                               {{"id", "captured"},
                                {"revision", "3"},
                                {"selections", Json::array({{{"anchor", 4}, {"head", 4}}})}}}});
            if (method == "_ui.update")
                ++updates;
            else
                CHECK(method == "_ui.read.cancel");
            return ready(nullptr);
        },
        modules(),
        R"js(
        let abortSelection;
        astra.registerCommand('acceptance.select', async () => {
            const controller = new AbortController();
            abortSelection = () => controller.abort();
            return await astra.ui.selectObject('parameter', {inner: true, expand: true}, {
                signal: controller.signal
            });
        });
        astra.registerCommand('acceptance.abortSelection', () => abortSelection());
    )js");
    std::optional<astra::script::Runtime::Outcome> result;
    workbench.execute("acceptance.select", Json::object(), [&](const auto& outcome) {
        result = outcome;
    });
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "document.syntax");
    channel->reply({{"operationId", "parse"},
                    {"shared", true},
                    {"state", "pending"},
                    {"id", "captured"},
                    {"revision", "3"}});
    for (int tick = 0; tick < 8; ++tick)
        workbench.poll();
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.get");
    CHECK_FALSE(result);
    workbench.execute("acceptance.abortSelection", Json::object(), {});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    REQUIRE(result);
    REQUIRE(result->error);
    CHECK(result->error->at("name") == "AbortError");
    REQUIRE(channel->requests.size() == 1);
    CHECK(channel->requests.front().body.at("method") == "operation.get");
    channel->reply({{"state", "completed"},
                    {"result", {{"id", "captured"}, {"revision", "3"}, {"state", "ready"}}}});
    for (int tick = 0; tick < 12; ++tick)
        workbench.poll();
    CHECK(channel->requests.empty());
    CHECK(updates == 0);
    CHECK(workbench.take_errors().empty());
    CHECK(workbench.idle());
}
