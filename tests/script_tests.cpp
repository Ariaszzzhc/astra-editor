#include <astra/script/runtime.hpp>
#include <astra/service/editor_service.hpp>
#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

using astra::script::Runtime;
using astra::script::ScriptError;
using Json = astra::protocol::Json;

namespace {
Json echo(std::string_view method, const Json& params) {
    return Json{{"method", method}, {"params", params}};
}
} // namespace

TEST_CASE("QuickJS evaluates real JavaScript and serializes Unicode JSON") {
    Runtime runtime(echo);
    CHECK(runtime.evaluate("[1+2, '中文🙂', {answer: 6*7}]") ==
          Json::array({3, "中文🙂", {{"answer", 42}}}));
    CHECK(runtime.evaluate("undefined").is_null());
    CHECK(runtime.evaluate("astra.request('echo', {value: 42})")["params"]["value"] == 42);
}

TEST_CASE("QuickJS measures UTF-8 bytes independently of UTF-16 string length") {
    Runtime runtime(echo);
    CHECK(
        runtime.evaluate(
            "['', 'ASCII', '中文🙂', 'e\\u0301', 'a\\0b'].map(text=>astra.utf8ByteLength(text))") ==
        Json::array({0, 5, 10, 3, 3}));
    CHECK(runtime.evaluate("astra.utf8ByteLength('x'.repeat(65534)+'🙂')") == 65538);
    CHECK_THROWS_AS(static_cast<void>(runtime.evaluate(
                        "astra.utf8ByteLength({toString(){throw new Error('coercion')}})")),
                    ScriptError);
    CHECK_THROWS_AS(static_cast<void>(runtime.evaluate("astra.utf8ByteLength()")), ScriptError);
    CHECK(runtime.evaluate("astra.utf8ByteLength('🙂')") == 4);
}

TEST_CASE("JavaScript commands register override and retain closure state") {
    Runtime runtime(echo);
    (void)runtime.evaluate("let count=0; astra.registerCommand('increment', p => count += p.by)");
    CHECK(runtime.execute_command("increment", {{"by", 2}}) == 2);
    CHECK(runtime.execute_command("increment", {{"by", 3}}) == 5);
    (void)runtime.evaluate("astra.registerCommand('increment', () => 9)");
    CHECK(runtime.execute_command("increment") == 9);
    CHECK(runtime.commands() == std::vector<std::string>{"increment"});
    CHECK_THROWS_AS((void)runtime.execute_command("missing"), ScriptError);
}

TEST_CASE("QuickJS command composition retains callbacks that replace themselves") {
    Runtime runtime(echo);
    static_cast<void>(runtime.evaluate("astra.registerCommand('replace',p=>{astra.registerCommand('"
                                       "replace',()=>9);return p.value;});"));
    CHECK(runtime.evaluate("astra._invokeCommand('replace',{value:42})") == 42);
    CHECK(runtime.evaluate("astra._invokeCommand('replace')") == 9);
    CHECK(runtime.evaluate("astra._commandNames()") == Json::array({"replace"}));
    CHECK_THROWS_AS(static_cast<void>(runtime.evaluate("astra._invokeCommand('missing')")),
                    ScriptError);
    CHECK(runtime.evaluate("6*7") == 42);
}

TEST_CASE("JavaScript custom command edits the production document service") {
    astra::service::EditorService service;
    std::size_t id{};
    Runtime runtime([&](std::string_view method, const Json& params) {
        const auto response = service.dispatch(
            Json{{"jsonrpc", "2.0"}, {"id", ++id}, {"method", method}, {"params", params}});
        if (!response || response->contains("error")) {
            throw ScriptError(response ? response->dump() : "No response");
        }
        return response->at("result");
    });
    const auto created = runtime.evaluate("astra.request('document.create', {text:'hello'})");
    (void)runtime.evaluate(
        "astra.registerCommand('replace', p => astra.request('document.edit', {id:p.id, "
        "baseRevision:p.revision, edits:[{begin:0,end:5,text:'你好'}]}))");
    const auto edited = runtime.execute_command("replace", created);
    CHECK(edited["revision"] == "1");
    const auto read = runtime.evaluate("astra.request('document.get', {id:'1'})");
    CHECK(read["text"] == "你好");
    CHECK_THROWS_AS((void)runtime.execute_command("replace", created), ScriptError);
    CHECK(runtime.evaluate("1+1") == 2);
}

TEST_CASE("JavaScript settings and keymaps incrementally override defaults") {
    Runtime runtime(echo);
    (void)runtime.evaluate("astra.configure({theme:{background:'#111',foreground:'#eee'},fontSize:"
                           "14}); astra.bind('Ctrl+S','file.save')");
    (void)runtime.evaluate("astra.configure({fontSize:16,theme:{foreground:'#fff'}}); "
                           "astra.bind('Ctrl+S','custom.save')");
    CHECK(runtime.settings()["theme"]["background"] == "#111");
    CHECK(runtime.settings()["theme"]["foreground"] == "#fff");
    CHECK(runtime.settings()["fontSize"] == 16);
    CHECK(runtime.keybindings().at("Ctrl+S") == "custom.save");
}

TEST_CASE("JavaScript errors do not corrupt the reusable host") {
    Runtime runtime(echo);
    for (const auto* source : {"throw new Error('example')",
                               "let = ;",
                               "astra.request(4)",
                               "astra.registerCommand('bad', 1)",
                               "astra.configure([])",
                               "astra.bind('', 'bad')",
                               "const cycle={}; cycle.self=cycle; cycle"}) {
        CAPTURE(source);
        CHECK_THROWS_AS((void)runtime.evaluate(source), ScriptError);
        CHECK(runtime.evaluate("40+2") == 42);
    }
}

TEST_CASE("JavaScript explicit keymap changes survive identical values and invalid calls") {
    Runtime runtime(echo);
    static_cast<void>(runtime.evaluate("astra.bind('Left', 'editor.move_left')"));
    const auto defaults = runtime.keybinding_generation();
    CHECK(runtime.keybindings(defaults).empty());
    static_cast<void>(runtime.evaluate("astra.bind('Left', 'editor.move_left')"));
    CHECK(runtime.keybinding_generation() > defaults);
    CHECK(runtime.keybindings(defaults).at("Left") == "editor.move_left");
    const auto explicit_preference = runtime.keybinding_generation();
    CHECK_THROWS_AS(static_cast<void>(runtime.evaluate("astra.bind('', 'invalid')")), ScriptError);
    CHECK(runtime.keybinding_generation() == explicit_preference);
    CHECK(runtime.keybindings().at("Left") == "editor.move_left");
    static_cast<void>(runtime.evaluate("astra.bind('Right', 'custom.right')"));
    const auto changes = runtime.keybindings(explicit_preference);
    CHECK(changes.size() == 1);
    CHECK(changes.at("Right") == "custom.right");
}

TEST_CASE("QuickJS interrupts infinite loops and permits subsequent execution") {
    Runtime::Limits limits;
    limits.execution_time = std::chrono::milliseconds(20);
    Runtime runtime(echo, limits);
    const auto begin = std::chrono::steady_clock::now();
    CHECK_THROWS_AS((void)runtime.evaluate("while(true) {}"), ScriptError);
    CHECK(std::chrono::steady_clock::now() - begin < std::chrono::seconds(3));
    CHECK(runtime.evaluate("42") == 42);
}

TEST_CASE("QuickJS memory limits surface exceptions rather than terminate the host") {
    Runtime::Limits limits;
    limits.memory_bytes = 2 * 1024 * 1024;
    Runtime runtime(echo, limits);
    CHECK_THROWS_AS((void)runtime.evaluate("new Array(10000000).fill('memory')"), ScriptError);
    CHECK(runtime.evaluate("42") == 42);
}

TEST_CASE("QuickJS drains finite jobs and enforces thread affinity") {
    Runtime runtime(echo);
    (void)runtime.evaluate("let result=0; Promise.resolve(9).then(v => result=v)");
    CHECK(runtime.evaluate("result") == 9);
    bool rejected{};
    std::thread worker([&] {
        try {
            (void)runtime.evaluate("1");
        } catch (const std::logic_error&) {
            rejected = true;
        }
    });
    worker.join();
    CHECK(rejected);
}

TEST_CASE("QuickJS bounds self-scheduling jobs and oversized JSON values") {
    Runtime::Limits limits;
    limits.max_pending_jobs = 8;
    Runtime runtime(echo, limits);
    CHECK_THROWS_AS(
        (void)runtime.evaluate("function repeat(){Promise.resolve().then(repeat)}; repeat()"),
        ScriptError);
    CHECK_THROWS_AS((void)runtime.evaluate("42"), ScriptError);
    Runtime fresh(echo);
    CHECK(fresh.evaluate("42") == 42);
    CHECK_THROWS_AS(
        (void)fresh.evaluate("let nested={}; for(let i=0;i<100;i++)nested={child:nested}; nested"),
        ScriptError);
    CHECK(fresh.evaluate("42") == 42);
}

TEST_CASE("QuickJS retains a command while it replaces its own registration") {
    Runtime runtime(echo);
    (void)runtime.evaluate(
        "astra.registerCommand('self', () => {astra.registerCommand('self',()=>2); return 1;})");
    CHECK(runtime.execute_command("self") == 1);
    CHECK(runtime.execute_command("self") == 2);
}

TEST_CASE("JavaScript requests are deferred promises and independent tasks stay responsive") {
    std::vector<std::string> methods;
    std::vector<std::shared_ptr<std::promise<Json>>> replies;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view method, const Json&) {
        methods.emplace_back(method);
        auto reply = std::make_shared<std::promise<Json>>();
        replies.push_back(reply);
        return reply->get_future();
    }});
    const auto first = runtime.start_evaluation(
        "(async()=>{ const p=astra.request('slow'); if(!(p instanceof Promise)) throw Error('not "
        "Promise'); return (await p).answer; })()");
    CHECK(methods.empty());
    runtime.poll();
    REQUIRE(methods == std::vector<std::string>{"slow"});
    CHECK_FALSE(runtime.take_result(first));
    const auto second = runtime.start_evaluation("astra.request('fast')");
    runtime.poll();
    REQUIRE(replies.size() == 2);
    replies[1]->set_value(42);
    runtime.poll();
    auto result = runtime.take_result(second);
    REQUIRE(result);
    CHECK_FALSE(result->error);
    CHECK(result->value == 42);
    CHECK_FALSE(runtime.take_result(first));
    replies[0]->set_value({{"answer", 9}});
    runtime.poll();
    result = runtime.take_result(first);
    REQUIRE(result);
    CHECK(result->value == 9);
    CHECK(runtime.idle());
}

TEST_CASE(
    "QuickJS host requests preserve task identity across awaits and restore it after errors") {
    Runtime* owner{};
    std::vector<std::pair<std::string, Runtime::TaskId>> requests;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view method, const Json&) {
        requests.emplace_back(method, owner->request_owner());
        if (method == "fail")
            throw astra::protocol::RpcError(-32001, "Expected rejection");
        std::promise<Json> reply;
        reply.set_value(42);
        return reply.get_future();
    }});
    owner = &runtime;
    static_cast<void>(runtime.evaluate(R"js(
        astra.registerCommand('nested', () => astra.request('nested'));
        astra.registerCommand('first', async () => {
            await astra.sleep(0);
            try { await astra.request('fail'); } catch (error) {
                if (error.code !== -32001) throw error;
            }
            return await astra._invokeCommand('nested');
        });
    )js"));
    const auto first = runtime.start_command("first");
    const auto second = runtime.start_evaluation("astra.request('independent')");
    CHECK(runtime.request_owner() == 0);
    for (int tick = 0; tick < 12; ++tick) {
        runtime.poll();
        CHECK(runtime.request_owner() == 0);
    }
    CHECK(requests == std::vector<std::pair<std::string, Runtime::TaskId>>{
                          {"independent", second}, {"fail", first}, {"nested", first}});
    const auto result = runtime.take_result(first);
    REQUIRE(result);
    CHECK_FALSE(result->error);
    CHECK(result->value == 42);
    REQUIRE(runtime.take_result(second));
    CHECK(runtime.idle());
}

TEST_CASE("QuickJS host cancellation cleans up a dispatched request without JS continuation") {
    for (const bool deadline : {false, true}) {
        CAPTURE(deadline);
        std::promise<Json> native;
        std::vector<Json> cancelled;
        Runtime::TaskId cleanup_owner{};
        Runtime runtime(Runtime::AsyncRequest{[&](std::string_view method, const Json& params) {
            CHECK(method == "native.read");
            CHECK(params.at("id") == "owned-clipboard");
            return native.get_future();
        }});
        runtime.set_request_cancel_handler([&](std::string_view method, const Json& params) {
            cleanup_owner = runtime.request_owner();
            cancelled.push_back({{"method", method}, {"params", params}});
            native.set_exception(std::make_exception_ptr(
                astra::protocol::RpcError(-32004, "Native request stopped")));
            std::promise<Json> cleanup;
            cleanup.set_value(nullptr);
            return cleanup.get_future();
        });
        static_cast<void>(runtime.evaluate(R"js(
            let drained = null, published = false;
            astra.on('runtime.requestDrained', value => drained = value);
        )js"));
        const auto task = runtime.start_evaluation(deadline ? R"js(
            (async () => {
                try { await astra.request('native.read', {id:'owned-clipboard'}, {timeoutMs:20}); }
                catch (error) { return error.name; }
                published = true;
            })()
        )js"
                                                            : R"js(
            (async () => {
                await astra.request('native.read', {id:'owned-clipboard'});
                published = true;
            })()
        )js");
        runtime.poll();
        CHECK_THROWS_AS(runtime.set_request_cancel_handler({}), ScriptError);
        if (deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        else
            runtime.cancel(task);
        runtime.poll();
        runtime.poll();
        const auto result = runtime.take_result(task);
        REQUIRE(result);
        if (deadline) {
            CHECK_FALSE(result->error);
            CHECK(result->value == "TimeoutError");
        } else {
            REQUIRE(result->error);
            CHECK(result->error->at("name") == "AbortError");
        }
        CHECK(cancelled == std::vector<Json>{{{"method", "native.read"},
                                              {"params", {{"id", "owned-clipboard"}}}}});
        CHECK(runtime.evaluate("published") == false);
        CHECK(cleanup_owner == task);
        CHECK(runtime.request_owner() == 0);
        CHECK(runtime.evaluate("drained") ==
              Json{{"method", "native.read"},
                   {"cancelled", true},
                   {"error", {{"code", -32004}, {"message", "Native request stopped"}}}});
        CHECK(runtime.idle());
    }
}

TEST_CASE("QuickJS host cleanup failure is observable while the native response still drains") {
    std::promise<Json> native;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view, const Json&) {
        return native.get_future();
    }});
    runtime.set_request_cancel_handler([](std::string_view, const Json&) {
        std::promise<Json> cleanup;
        cleanup.set_exception(
            std::make_exception_ptr(std::runtime_error("Cleanup transport failed")));
        return cleanup.get_future();
    });
    const auto task = runtime.start_evaluation("astra.request('native.read')");
    runtime.poll();
    runtime.cancel(task);
    REQUIRE(runtime.take_result(task));
    CHECK_THROWS_WITH_AS(runtime.poll(), "Cleanup transport failed", std::runtime_error);
    CHECK_FALSE(runtime.idle());
    native.set_value("late ignored value");
    CHECK_NOTHROW(runtime.poll());
    CHECK(runtime.idle());
}

TEST_CASE("QuickJS invalid host cleanup retains its outstanding native response") {
    std::promise<Json> native;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view, const Json&) {
        return native.get_future();
    }});
    runtime.set_request_cancel_handler([](std::string_view, const Json&) {
        return std::future<Json>{};
    });
    const auto task = runtime.start_evaluation(R"js(
        (async () => {
            try { await astra.request('native.read', {}, {timeoutMs:20}); }
            catch (error) { return error.message; }
        })()
    )js");
    runtime.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    runtime.poll();
    const auto outcome = runtime.take_result(task);
    REQUIRE(outcome);
    CHECK(outcome->value == "Host cancellation returned an invalid future");
    CHECK_FALSE(runtime.idle());
    native.set_value("late ignored value");
    runtime.poll();
    CHECK(runtime.idle());
}

TEST_CASE("QuickJS shared promises resume each registered task independently of the resolver") {
    Runtime* owner{};
    std::vector<std::pair<std::string, Runtime::TaskId>> requests;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view method, const Json&) {
        requests.emplace_back(method, owner->request_owner());
        std::promise<Json> reply;
        reply.set_value(42);
        return reply.get_future();
    }});
    owner = &runtime;
    static_cast<void>(runtime.evaluate(R"js(
        let resolveShared;
        const shared = new Promise(resolve => { resolveShared = resolve; });
        astra.registerCommand('shared', async parameters => {
            await shared;
            await Promise.resolve();
            return await astra.request(parameters.method);
        });
        astra.registerCommand('release', () => { resolveShared(); return true; });
    )js"));
    const auto first = runtime.start_command("shared", {{"method", "first"}});
    const auto second = runtime.start_command("shared", {{"method", "second"}});
    runtime.poll();
    CHECK(requests.empty());
    const auto resolver = runtime.start_command("release");
    for (int tick = 0; tick < 6; ++tick)
        runtime.poll();
    CHECK(requests == std::vector<std::pair<std::string, Runtime::TaskId>>{{"first", first},
                                                                           {"second", second}});
    for (const auto task : {first, second, resolver}) {
        const auto result = runtime.take_result(task);
        REQUIRE(result);
        CHECK_FALSE(result->error);
    }
    CHECK(runtime.request_owner() == 0);
    CHECK(runtime.idle());
}

TEST_CASE("QuickJS cancelled shared-promise continuations cannot issue late mutations") {
    std::vector<std::string> requests;
    Runtime runtime([&](std::string_view method, const Json&) -> Json {
        requests.emplace_back(method);
        return true;
    });
    static_cast<void>(runtime.evaluate(R"js(
        let releaseCancelled, caughtCancellation = false;
        const cancelledGate = new Promise(resolve => { releaseCancelled = resolve; });
        astra.registerCommand('waitCancelled', async () => {
            await cancelledGate;
            try { await astra.request('mutate'); }
            catch (error) { caughtCancellation = error.name === 'AbortError'; }
        });
        astra.registerCommand('releaseCancelled', () => { releaseCancelled(); return true; });
    )js"));
    const auto cancelled = runtime.start_command("waitCancelled");
    runtime.cancel(cancelled);
    const auto outcome = runtime.take_result(cancelled);
    REQUIRE(outcome);
    CHECK(outcome->error->at("name") == "AbortError");
    runtime.poll();
    CHECK(runtime.execute_command("releaseCancelled") == true);
    runtime.poll();
    CHECK(requests.empty());
    CHECK(runtime.evaluate("caughtCancellation") == true);
    CHECK(runtime.idle());
}

TEST_CASE("QuickJS late operation cancellation retains its originating request identity") {
    Runtime* owner{};
    std::promise<Json> delayed;
    std::vector<Runtime::TaskId> owners;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view method, const Json& params) {
        owners.push_back(owner->request_owner());
        if (method == "slow")
            return delayed.get_future();
        CHECK(method == "operation.cancel");
        CHECK(params.at("id") == "late");
        std::promise<Json> reply;
        reply.set_value(nullptr);
        return reply.get_future();
    }});
    owner = &runtime;
    const auto task = runtime.start_evaluation("astra.request('slow')");
    runtime.poll();
    runtime.cancel(task);
    REQUIRE(runtime.take_result(task));
    runtime.poll();
    delayed.set_value({{"operationId", "late"}});
    runtime.poll();
    runtime.poll();
    CHECK(owners == std::vector<Runtime::TaskId>{task, task});
    CHECK(runtime.request_owner() == 0);
    CHECK(runtime.idle());
}

TEST_CASE("QuickJS cancelled late shared operations retain service ownership") {
    std::promise<Json> delayed;
    std::vector<std::string> requests;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view method, const Json&) {
        requests.emplace_back(method);
        CHECK(method == "document.syntax");
        return delayed.get_future();
    }});
    const auto task = runtime.start_evaluation("astra.request('document.syntax')");
    runtime.poll();
    REQUIRE(requests == std::vector<std::string>{"document.syntax"});
    runtime.cancel(task);
    const auto outcome = runtime.take_result(task);
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("name") == "AbortError");
    runtime.poll();
    CHECK_FALSE(runtime.idle());
    delayed.set_value({{"operationId", "shared-parse"}, {"shared", true}, {"state", "pending"}});
    runtime.poll();
    runtime.poll();
    CHECK(requests == std::vector<std::string>{"document.syntax"});
    CHECK(runtime.request_owner() == 0);
    CHECK(runtime.idle());
}

TEST_CASE("QuickJS shared operation timeout drains its waiter without cancelling the service") {
    std::promise<Json> delayed;
    std::vector<std::string> requests;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view method, const Json& params) {
        requests.emplace_back(method);
        if (method == "document.syntax") {
            std::promise<Json> reply;
            reply.set_value({{"operationId", "shared-parse"}, {"shared", true}});
            return reply.get_future();
        }
        CHECK(method == "operation.get");
        CHECK(params.at("id") == "shared-parse");
        return delayed.get_future();
    }});
    const auto task = runtime.start_evaluation(R"js(
        (async () => {
            try {
                await astra.request('document.syntax', {}, {timeoutMs: 50});
            } catch (error) {
                return error.name;
            }
        })()
    )js");
    runtime.poll();
    runtime.poll();
    REQUIRE(requests == std::vector<std::string>{"document.syntax", "operation.get"});
    CHECK_FALSE(runtime.take_result(task));
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    runtime.poll();
    const auto outcome = runtime.take_result(task);
    REQUIRE(outcome);
    CHECK_FALSE(outcome->error);
    CHECK(outcome->value == "TimeoutError");
    CHECK_FALSE(runtime.idle());
    delayed.set_value({{"state", "completed"}, {"result", {{"state", "ready"}}}});
    runtime.poll();
    runtime.poll();
    CHECK(requests == std::vector<std::string>{"document.syntax", "operation.get"});
    CHECK(runtime.idle());
}

TEST_CASE("JavaScript async commands preserve service error codes and can recover") {
    Runtime runtime([](std::string_view method, const Json&) -> Json {
        if (method == "fail") {
            throw astra::protocol::RpcError(-32001, "stale revision", Json{{"revision", "9"}});
        }
        return 7;
    });
    (void)runtime.evaluate("astra.registerCommand('recover', async()=>{try{await "
                           "astra.request('fail')}catch(e){if(e.code!==-32001||e.data.revision!=='"
                           "9')throw e;} return await astra.request('ok');})");
    CHECK(runtime.execute_command("recover") == 7);
    (void)runtime.evaluate(
        "astra.registerCommand('fail', async()=>{await astra.request('fail');})");
    const auto id = runtime.start_command("fail");
    runtime.poll();
    const auto outcome = runtime.take_result(id);
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("code") == -32001);
    CHECK(runtime.evaluate("40+2") == 42);
}

TEST_CASE("JavaScript cancellation prevents queued and later mutations") {
    int mutations{};
    Runtime runtime([&](std::string_view, const Json&) -> Json {
        ++mutations;
        return nullptr;
    });
    (void)runtime.evaluate("astra.registerCommand('change',async()=>{try{await "
                           "astra.request('first')}catch(e){} await astra.request('second');})");
    const auto id = runtime.start_command("change");
    runtime.cancel(id);
    const auto outcome = runtime.take_result(id);
    REQUIRE(outcome);
    REQUIRE(outcome->error);
    CHECK(outcome->error->at("name") == "AbortError");
    for (int tick = 0; tick < 4; ++tick) {
        runtime.poll();
    }
    CHECK(mutations == 0);
    CHECK(runtime.idle());
}

TEST_CASE("JavaScript AbortSignal and timeout reject without blocking the owner") {
    std::promise<Json> reply;
    Runtime runtime(Runtime::AsyncRequest{[&](std::string_view, const Json&) {
        return reply.get_future();
    }});
    const auto id = runtime.start_evaluation(
        "(async()=>{const c=new AbortController();const p=astra.request('read',{}, "
        "{signal:c.signal});c.abort();try{await p}catch(e){return e.name}})()");
    runtime.poll();
    auto outcome = runtime.take_result(id);
    REQUIRE(outcome);
    CHECK(outcome->value == "AbortError");
    const auto timed = runtime.start_evaluation(
        "(async()=>{try{await astra.request('read',{}, {timeoutMs:2})}catch(e){return e.name}})()");
    runtime.poll();
    CHECK_FALSE(runtime.take_result(timed));
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    runtime.poll();
    outcome = runtime.take_result(timed);
    REQUIRE(outcome);
    CHECK(outcome->value == "TimeoutError");
    reply.set_value(nullptr);
    runtime.poll();
    CHECK(runtime.idle());
}

TEST_CASE("JavaScript awaits operation pages and forwards cancellation after delayed replies") {
    std::vector<std::string> cancelled;
    std::size_t pages{};
    Runtime runtime([&](std::string_view method, const Json& p) -> Json {
        if (method == "search") {
            return {{"operationId", "search-1"}};
        }
        if (method == "operation.cancel") {
            cancelled.push_back(p.at("id").get<std::string>());
            return nullptr;
        }
        ++pages;
        const auto offset = p.at("offset").get<std::size_t>();
        return {{"state", "completed"},
                {"result", {{"matches", Json::array({offset})}}},
                {"page", {{"nextOffset", offset + 1}, {"total", 3}}}};
    });
    CHECK(runtime.evaluate("astra.request('search')")["matches"] == Json::array({0, 1, 2}));
    CHECK(pages == 3);
    std::promise<Json> delayed;
    Runtime remote(Runtime::AsyncRequest{[&](std::string_view method, const Json& p) {
        if (method == "read") {
            return delayed.get_future();
        }
        cancelled.push_back(p.at("id").get<std::string>());
        std::promise<Json> done;
        done.set_value(nullptr);
        return done.get_future();
    }});
    const auto task = remote.start_evaluation("astra.request('read')");
    remote.poll();
    remote.cancel(task);
    static_cast<void>(remote.take_result(task));
    remote.poll();
    delayed.set_value({{"operationId", "late-1"}});
    remote.poll();
    remote.poll();
    CHECK(cancelled == std::vector<std::string>{"late-1"});
    CHECK(remote.idle());
}

TEST_CASE("JavaScript cancelled request drains report service rejection and preserve transport "
          "failures") {
    for (const auto code : {-32001, -32602, -32603}) {
        CAPTURE(code);
        std::promise<Json> delayed;
        Runtime runtime(Runtime::AsyncRequest{[&](std::string_view, const Json&) {
            return delayed.get_future();
        }});
        static_cast<void>(runtime.evaluate(
            "let receipt=null; astra.on('runtime.requestDrained',value=>receipt=value);"));
        const auto task =
            runtime.start_evaluation("astra.request('document.find',{query:'needle'})");
        runtime.poll();
        runtime.cancel(task);
        const auto cancelled = runtime.take_result(task);
        REQUIRE(cancelled);
        CHECK(cancelled->error->at("name") == "AbortError");
        runtime.poll();
        delayed.set_exception(std::make_exception_ptr(
            astra::protocol::RpcError(code, "late rejection", Json{{"revision", "8"}})));
        if (code == -32603) {
            CHECK_THROWS_AS(runtime.poll(), astra::protocol::RpcError);
            CHECK(runtime.evaluate("receipt").is_null());
        } else {
            CHECK_NOTHROW(runtime.poll());
            runtime.poll();
            CHECK(runtime.evaluate("receipt") == Json{{"method", "document.find"},
                                                      {"cancelled", true},
                                                      {"error",
                                                       {{"code", code},
                                                        {"message", "late rejection"},
                                                        {"data", {{"revision", "8"}}}}}});
            CHECK(runtime.take_errors().empty());
        }
        CHECK(runtime.idle());
    }
}

TEST_CASE("JavaScript event subscriptions and registered ES modules use the same host") {
    Runtime runtime(echo);
    runtime.define_module(
        "astra/test",
        "export function setup(){let n=0; astra.registerCommand('counter',()=>++n);}");
    CHECK(runtime.evaluate_module("import {setup} from 'astra/test'; setup();", "entry.js")
              .is_null());
    CHECK(runtime.execute_command("counter") == 1);
    CHECK(runtime.execute_command("counter") == 2);
    CHECK_THROWS_AS((void)runtime.evaluate_module("import 'missing';", "bad.js"), ScriptError);
    (void)runtime.evaluate("let events=[]; let "
                           "token=astra.on('document.changed',p=>{events.push(p.revision);astra."
                           "off('document.changed',token);});");
    runtime.emit("document.changed", {{"revision", "1"}});
    runtime.emit("document.changed", {{"revision", "2"}});
    CHECK(runtime.evaluate("events") == Json::array({"1"}));
}

TEST_CASE("JavaScript async event failures remain observable and release tasks") {
    Runtime runtime(echo);
    (void)runtime.evaluate(
        "astra.on('change',async()=>{await astra.request('read');throw Error('provider failed')})");
    runtime.emit("change", Json::object());
    runtime.poll();
    const auto errors = runtime.take_errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0]["message"] == "provider failed");
    CHECK(runtime.take_errors().empty());
    CHECK(runtime.idle());
}

TEST_CASE("JavaScript synchronous failure cancels requests submitted before throwing") {
    int mutations{};
    Runtime runtime([&](std::string_view, const Json&) -> Json {
        ++mutations;
        return nullptr;
    });
    CHECK_THROWS_AS(
        (void)runtime.start_evaluation("astra.request('mutate'); throw Error('failed');"),
        ScriptError);
    runtime.poll();
    CHECK(mutations == 0);
    CHECK(runtime.idle());
    const auto id = runtime.start_evaluation("astra.request('mutate')");
    runtime.shutdown();
    CHECK(runtime.idle());
    CHECK(mutations == 0);
    CHECK_THROWS_AS((void)runtime.start_evaluation("1"), ScriptError);
    static_cast<void>(id);
}

TEST_CASE("JavaScript pages deliver bounded batches and early exit cancels the operation") {
    std::vector<std::size_t> offsets;
    std::vector<std::string> cancellations;
    Runtime runtime([&](std::string_view method, const Json& p) -> Json {
        if (method == "search") {
            return {{"operationId", "pages"}};
        }
        if (method == "operation.cancel") {
            cancellations.push_back(p.at("id"));
            return nullptr;
        }
        const auto offset = p.at("offset").get<std::size_t>();
        offsets.push_back(offset);
        return {{"state", "completed"},
                {"result", {{"matches", Json::array({offset})}}},
                {"page", {{"nextOffset", offset + 1}, {"total", 3}}}};
    });
    CHECK(runtime.evaluate(
              "(async()=>{let pages=[];for await(const p of astra.operationPages('search',{}, "
              "{limit:1})){pages.push(p.matches[0]);}return pages;})()") == Json::array({0, 1, 2}));
    CHECK(offsets == std::vector<std::size_t>{0, 1, 2});
    CHECK(cancellations.empty());
    offsets.clear();
    CHECK(runtime.evaluate("(async()=>{for await(const p of astra.operationPages('search')){return "
                           "p.matches[0];}})()") == 0);
    // Closing an async iterator may enqueue its finally request after the
    // consumer's result. The owner-loop must still drain that request.
    runtime.poll();
    INFO(Json(cancellations).dump());
    CHECK(offsets == std::vector<std::size_t>{0});
    CHECK(cancellations == std::vector<std::string>{"pages"});
    CHECK(runtime.idle());
}
