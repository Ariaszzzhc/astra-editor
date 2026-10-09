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
}

TEST_CASE("QuickJS evaluates real JavaScript and serializes Unicode JSON") {
    Runtime runtime(echo);
    CHECK(runtime.evaluate("[1+2, '中文🙂', {answer: 6*7}]") == Json::array({3, "中文🙂", {{"answer",42}}}));
    CHECK(runtime.evaluate("undefined").is_null());
    CHECK(runtime.evaluate("astra.request('echo', {value: 42})")["params"]["value"] == 42);
}

TEST_CASE("JavaScript commands register override and retain closure state") {
    Runtime runtime(echo);
    (void)runtime.evaluate("let count=0; astra.registerCommand('increment', p => count += p.by)");
    CHECK(runtime.execute_command("increment", {{"by",2}}) == 2);
    CHECK(runtime.execute_command("increment", {{"by",3}}) == 5);
    (void)runtime.evaluate("astra.registerCommand('increment', () => 9)");
    CHECK(runtime.execute_command("increment") == 9);
    CHECK(runtime.commands() == std::vector<std::string>{"increment"});
    CHECK_THROWS_AS((void)runtime.execute_command("missing"), ScriptError);
}

TEST_CASE("JavaScript custom command edits the production document service") {
    astra::service::EditorService service;
    std::size_t id{};
    Runtime runtime([&](std::string_view method, const Json& params) {
        const auto response = service.dispatch(Json{{"jsonrpc","2.0"},{"id",++id},{"method",method},{"params",params}});
        if (!response || response->contains("error")) { throw ScriptError(response ? response->dump() : "No response"); }
        return response->at("result");
    });
    const auto created = runtime.evaluate("astra.request('document.create', {text:'hello'})");
    (void)runtime.evaluate("astra.registerCommand('replace', p => astra.request('document.edit', {id:p.id, baseRevision:p.revision, edits:[{begin:0,end:5,text:'你好'}]}))");
    const auto edited = runtime.execute_command("replace", created);
    CHECK(edited["revision"] == "1");
    const auto read = runtime.evaluate("astra.request('document.get', {id:'1'})");
    CHECK(read["text"] == "你好");
    CHECK_THROWS_AS((void)runtime.execute_command("replace", created), ScriptError);
    CHECK(runtime.evaluate("1+1") == 2);
}

TEST_CASE("JavaScript settings and keymaps incrementally override defaults") {
    Runtime runtime(echo);
    (void)runtime.evaluate("astra.configure({theme:{background:'#111',foreground:'#eee'},fontSize:14}); astra.bind('Ctrl+S','file.save')");
    (void)runtime.evaluate("astra.configure({fontSize:16,theme:{foreground:'#fff'}}); astra.bind('Ctrl+S','custom.save')");
    CHECK(runtime.settings()["theme"]["background"] == "#111");
    CHECK(runtime.settings()["theme"]["foreground"] == "#fff");
    CHECK(runtime.settings()["fontSize"] == 16);
    CHECK(runtime.keybindings().at("Ctrl+S") == "custom.save");
}

TEST_CASE("JavaScript errors do not corrupt the reusable host") {
    Runtime runtime(echo);
    for (const auto* source : {"throw new Error('example')", "let = ;", "astra.request(4)",
                              "astra.registerCommand('bad', 1)", "astra.configure([])",
                              "astra.bind('', 'bad')", "const cycle={}; cycle.self=cycle; cycle"}) {
        CAPTURE(source);
        CHECK_THROWS_AS((void)runtime.evaluate(source), ScriptError);
        CHECK(runtime.evaluate("40+2") == 42);
    }
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
        try { (void)runtime.evaluate("1"); } catch (const std::logic_error&) { rejected = true; }
    });
    worker.join();
    CHECK(rejected);
}

TEST_CASE("QuickJS bounds self-scheduling jobs and oversized JSON values") {
    Runtime::Limits limits;
    limits.max_pending_jobs = 8;
    Runtime runtime(echo, limits);
    CHECK_THROWS_AS((void)runtime.evaluate("function repeat(){Promise.resolve().then(repeat)}; repeat()"), ScriptError);
    CHECK_THROWS_AS((void)runtime.evaluate("42"), ScriptError);
    Runtime fresh(echo);
    CHECK(fresh.evaluate("42") == 42);
    CHECK_THROWS_AS((void)fresh.evaluate("let nested={}; for(let i=0;i<100;i++)nested={child:nested}; nested"), ScriptError);
    CHECK(fresh.evaluate("42") == 42);
}

TEST_CASE("QuickJS retains a command while it replaces its own registration") {
    Runtime runtime(echo);
    (void)runtime.evaluate("astra.registerCommand('self', () => {astra.registerCommand('self',()=>2); return 1;})");
    CHECK(runtime.execute_command("self") == 1);
    CHECK(runtime.execute_command("self") == 2);
}
