// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The script sandbox (docs/design/25 §9 A, PR 57 verify): a runaway loop and an
// allocation bomb each stop the add-on, not the process, within the
// watchdog's bound; without a permission its API is absent; the ways out of a
// Lua sandbox people know are closed.
#include "catch_compat.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "addon/open_manifest.h"
#include "addon/script_runtime.h"

using mv::addon::script_addon;
using mv::addon::script_config;
using mv::addon::script_limits;
using mv::addon::script_services;
using mv::addon::script_stop;
using namespace std::chrono_literals;

namespace {

using clk = std::chrono::steady_clock;

// What a script said, through mv.log / print (level 0) and mv.notify.
struct probe {
  std::mutex m;
  std::condition_variable cv;
  std::vector<std::string> logs;
  std::vector<std::string> errors;
  std::vector<std::string> notices;
  bool stopped = false;
  script_stop why = script_stop::none;
  std::string message;
  clk::time_point stopped_at{};

  script_services services() {
    script_services s;
    s.log = [this](const std::string&, int level, const std::string& text) {
      std::lock_guard<std::mutex> lock(m);
      (level == 0 ? logs : errors).push_back(text);
    };
    s.notify = [this](const std::string&, const std::string& text) {
      std::lock_guard<std::mutex> lock(m);
      notices.push_back(text);
    };
    s.folder_count = [] { return 132u; };
    s.folder_path = [] { return std::string("/photos/2026"); };
    s.folder_item_name = [](std::uint32_t i) {
      return i < 132 ? "IMG_" + std::to_string(1000 + i) + ".JPG" : std::string();
    };
    s.current_path = [] { return std::string("/photos/2026/IMG_1000.JPG"); };
    return s;
  }
  void watch(script_addon& a) {
    a.set_on_stopped([this](script_stop w, const std::string& msg) {
      std::lock_guard<std::mutex> lock(m);
      stopped = true;
      why = w;
      message = msg;
      stopped_at = clk::now();
      cv.notify_all();
    });
  }
  bool wait_stopped(std::chrono::milliseconds t) {
    std::unique_lock<std::mutex> lock(m);
    return cv.wait_for(lock, t, [this] { return stopped; });
  }
  std::vector<std::string> log_lines() {
    std::lock_guard<std::mutex> lock(m);
    return logs;
  }
};

script_limits fast_limits() {
  script_limits l;
  l.memory_bytes = 16u << 20;
  l.instructions = 20'000'000;
  l.wall = 300ms;
  return l;
}

std::unique_ptr<script_addon> start(probe& p, const std::string& source,
                                    std::uint32_t permissions = 0,
                                    script_limits limits = fast_limits()) {
  script_config c;
  c.addon_id = "acme.test";
  c.permissions = permissions;
  c.limits = limits;
  c.sources.push_back({"main.lua", source});
  auto a = script_addon::start(std::move(c), p.services());
  p.watch(*a);
  return a;
}

// Destroys `a` on another thread and says whether that returned within
// `bound`. A worker that never stops is leaked rather than hanging the run.
bool destroys_within(std::unique_ptr<script_addon>& a, std::chrono::milliseconds bound) {
  auto done = std::make_shared<std::atomic<bool>>(false);
  script_addon* raw = a.release();
  std::thread([raw, done] {
    delete raw;
    done->store(true);
  }).detach();
  const auto until = clk::now() + bound;
  while (!done->load() && clk::now() < until) std::this_thread::sleep_for(5ms);
  return done->load();
}

// Runs the global `fn` and says whether the task ended within `bound` (by
// returning, raising or stopping). On a hang the add-on is leaked.
bool task_ends_within(std::unique_ptr<script_addon>& a, const std::string& fn,
                      std::chrono::milliseconds bound) {
  REQUIRE(a->run_function("test", fn, mv::json::value{}));
  if (a->wait_idle(bound)) return true;
  (void)a.release();
  return false;
}

void run(script_addon& a, const std::string& fn) {
  REQUIRE(a.run_function("test", fn, mv::json::value{}));
  REQUIRE(a.wait_idle(5s));
}

// Runs `body` as the script's function `t` and returns what it logged.
std::vector<std::string> eval(const std::string& body, std::uint32_t permissions = 0) {
  probe p;
  auto a = start(p, "function t()\n" + body + "\nend", permissions);
  REQUIRE(a->wait_idle(5s));
  run(*a, "t");
  INFO(a->status().last_error);
  REQUIRE(a->status().running);
  return p.log_lines();
}

}  // namespace

TEST_CASE("a script's function runs on its own worker with its payload", "[script][runtime]") {
  probe p;
  auto a = start(p, R"lua(
    local main_thread = coroutine.running()
    function count(payload)
      mv.log(type(payload), payload and payload.n or "nil")
      mv.notify("counted\n" .. tostring(payload and payload.n))
    end
  )lua");
  REQUIRE(a->wait_idle(5s));
  REQUIRE(a->status().running);
  mv::json::value payload;
  payload.k = mv::json::kind::object;
  mv::json::value n;
  n.k = mv::json::kind::number;
  n.is_integer = true;
  n.i = 7;
  payload.o.emplace_back("n", n);
  REQUIRE(a->run_function("command count", "count", payload));
  REQUIRE(a->wait_idle(5s));
  REQUIRE(p.log_lines() == std::vector<std::string>{"table\t7"});
  std::lock_guard<std::mutex> lock(p.m);
  REQUIRE(p.notices == std::vector<std::string>{"counted 7"});  // control characters become spaces
}

TEST_CASE("the environment holds the safe libraries and nothing else", "[script][sandbox]") {
  const auto out = eval(R"lua(
    mv.log(type(io), type(os), type(package), type(debug), type(require), type(dofile),
           type(loadfile), type(warn), type(string.dump))
    mv.log(type(string.format), type(table.concat), type(math.floor), type(utf8.char),
           type(coroutine.wrap), type(pcall), type(setmetatable))
    mv.log(("x"):rep(3), #"abc", string.format("%d", 42))
  )lua");
  REQUIRE(out.size() == 3);
  REQUIRE(out[0] == "nil\tnil\tnil\tnil\tnil\tnil\tnil\tnil\tnil");
  REQUIRE(out[1] == "function\tfunction\tfunction\tfunction\tfunction\tfunction\tfunction");
  REQUIRE(out[2] == "xxx\t3\t42");
}

TEST_CASE("no bytecode is made or loaded; load takes text into the sandbox", "[script][sandbox]") {
  const auto out = eval(R"lua(
    -- string.dump is gone, so a script cannot make a binary chunk; one written
    -- by hand is refused by load and by the runtime's own loader.
    local f, err = load("\27Lua\84\0\25\147\13\10\26\10\4\8\8\120\86", "bin", "b")
    mv.log(f == nil, err:find("binary") ~= nil)
    local g = load("return 1 + 1")
    mv.log(g())
    local h = load("return x", "c", "t", { x = 5 })
    mv.log(h())
    local ok = pcall(load, function() return "return 1" end)
    mv.log(ok)
    -- the loaded chunk sees the sandbox, not something wider
    mv.log(load("return type(io)")())
  )lua");
  REQUIRE(out == std::vector<std::string>{"true\ttrue", "2", "5", "false", "nil"});
}

TEST_CASE("the string metatable cannot be reached or replaced", "[script][sandbox]") {
  const auto out = eval(R"lua(
    mv.log(getmetatable(""))
    mv.log(pcall(function() return getmetatable("").__index end))
    mv.log(pcall(setmetatable, "", {}))
    mv.log(("a"):upper())
  )lua");
  REQUIRE(out.size() == 4);
  REQUIRE(out[0] == "false");
  REQUIRE(out[1].rfind("false\t", 0) == 0);
  REQUIRE(out[2].rfind("false\t", 0) == 0);
  REQUIRE(out[3] == "A");
}

TEST_CASE("collectgarbage keeps only its harmless options", "[script][sandbox]") {
  const auto out = eval(R"lua(
    mv.log(type(collectgarbage("count")), collectgarbage("isrunning"))
    mv.log(collectgarbage())
    for _, o in ipairs({ "stop", "restart", "incremental", "generational", "setpause" }) do
      mv.log(o, (pcall(collectgarbage, o)))
    end
    mv.log(collectgarbage("isrunning"))
  )lua");
  REQUIRE(out[0] == "number\ttrue");
  REQUIRE(out[1] == "0");
  REQUIRE(out[2] == "stop\tfalse");
  REQUIRE(out[3] == "restart\tfalse");
  REQUIRE(out[4] == "incremental\tfalse");
  REQUIRE(out[5] == "generational\tfalse");
  REQUIRE(out[6] == "setpause\tfalse");
  REQUIRE(out[7] == "true");
}

TEST_CASE("without a permission its API is absent and not merely refused", "[script][permissions]") {
  const auto none = eval(R"lua(
    mv.log(type(mv.folder), type(mv.selection), type(mv.thumbnails), mv.permissions["folder.read"])
    mv.log(type(mv.log), type(mv.notify), type(mv.on), mv.id, mv.api)
  )lua");
  REQUIRE(none[0] == "nil\tnil\tnil\tnil");
  REQUIRE(none[1] == "function\tfunction\tfunction\tacme.test\t3");

  const auto folder = eval(R"lua(
    mv.log(type(mv.folder), type(mv.selection), mv.permissions["folder.read"])
    mv.log(mv.folder.count(), mv.folder.path(), mv.folder.name(1), mv.folder.name(132),
           mv.folder.name(133), mv.folder.name(0))
  )lua",
                           mv::addon::perm_folder_read);
  REQUIRE(folder[0] == "table\tnil\ttrue");
  REQUIRE(folder[1] == "132\t/photos/2026\tIMG_1000.JPG\tIMG_1131.JPG\tnil\tnil");

  const auto selection = eval(R"lua(
    mv.log(type(mv.folder), mv.selection.current())
  )lua",
                              mv::addon::perm_selection);
  REQUIRE(selection[0] == "nil\t/photos/2026/IMG_1000.JPG");

  // thumbnails adds nothing to the environment in PR 57: it gates the
  // thumbnail nodes of screens (PR 58).
  const auto thumbs = eval("mv.log(type(mv.thumbnails), mv.permissions.thumbnails)",
                           mv::addon::perm_thumbnails);
  REQUIRE(thumbs[0] == "nil\ttrue");
}

TEST_CASE("a runaway loop stops the add-on within the bound", "[script][limits]") {
  for (const char* loop : {
           "while true do end",
           // a pcall in the loop cannot swallow the stop
           "while true do pcall(function() while true do end end) end",
           "local co = coroutine.wrap(function() while true do coroutine.yield() end end)\n"
           "while true do co() end",
           "local function f() return f() end f()",  // a proper tail call: no stack growth
       }) {
    INFO(loop);
    probe p;
    auto a = start(p, std::string("function spin()\n") + loop + "\nend");
    REQUIRE(a->wait_idle(5s));
    const auto t0 = clk::now();
    REQUIRE(a->run_function("command spin", "spin", {}));
    REQUIRE(p.wait_stopped(5s));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(p.stopped_at - t0);
    INFO(took.count() << " ms");
    REQUIRE((p.why == script_stop::instructions || p.why == script_stop::time));
    REQUIRE(took < fast_limits().wall + 400ms);
    REQUIRE_FALSE(a->status().running);
    REQUIRE(a->status().stop == p.why);
    REQUIRE_FALSE(a->status().message.empty());
    // Stopped is stopped: nothing more runs.
    REQUIRE_FALSE(a->run_function("command spin", "spin", {}));
  }
}

TEST_CASE("an allocation bomb stops the add-on at the memory ceiling", "[script][limits]") {
  for (const char* bomb : {
           "local t = {} while true do t[#t + 1] = string.rep('x', 1 << 16) .. #t end",
           // catching the out-of-memory error buys nothing
           "local t = {} while true do pcall(function() t[#t + 1] = {} end) end",
           "local s = string.rep('x', 1 << 30)",  // 1 GB: under rep's own cap, over ours
           "local s = 'x' while true do s = s .. s end",
           "local t = {} for i = 1, math.maxinteger do t[i] = i end",
       }) {
    INFO(bomb);
    probe p;
    auto a = start(p, std::string("function bomb()\n") + bomb + "\nend");
    REQUIRE(a->wait_idle(5s));
    const auto t0 = clk::now();
    REQUIRE(a->run_function("command bomb", "bomb", {}));
    REQUIRE(p.wait_stopped(5s));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(p.stopped_at - t0);
    INFO(took.count() << " ms; " << p.message);
    REQUIRE(p.why == script_stop::memory);
    REQUIRE(took < fast_limits().wall + 400ms);
    REQUIRE(a->status().memory_peak <= fast_limits().memory_bytes);
  }
}

TEST_CASE("library loops that run no VM instruction still stop on time", "[script][limits]") {
  for (const char* slow : {
           // exponential pattern backtracking: one C call
           "string.find(string.rep('a', 40), string.rep('a-', 40) .. 'b')",
           // 2^62 iterations inside table.move
           "table.move({}, 1, math.maxinteger - 1, 2, {})",
       }) {
    INFO(slow);
    probe p;
    auto a = start(p, std::string("function slow()\n") + slow + "\nend");
    REQUIRE(a->wait_idle(5s));
    const auto t0 = clk::now();
    REQUIRE(a->run_function("command slow", "slow", {}));
    REQUIRE(p.wait_stopped(5s));
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(p.stopped_at - t0);
    INFO(took.count() << " ms");
    REQUIRE((p.why == script_stop::time || p.why == script_stop::instructions));
    REQUIRE(took < fast_limits().wall + 400ms);
  }
  // string.rep of nothing, many times, is nothing, at once.
  const auto out = eval("mv.log(#string.rep('', 1e15), #string.rep('', 1e15, ''))");
  REQUIRE(out[0] == "0\t0");
}

TEST_CASE("deep recursion and C recursion end in an error and never a crash", "[script][limits]") {
  for (const char* deep : {
           "local function f(n) return 1 + f(n + 1) end f(1)",
           "local function f() return coroutine.wrap(f)() end f()",
           "local function f() return pcall(f) end f()",
           "local t = setmetatable({}, {__index = function(t, k) return t[k] end}) local _ = t.x",
           "string.rep('(', 300):find(string.rep('(', 300))",
           "load(string.rep('(', 1000) .. '1' .. string.rep(')', 1000))",
       }) {
    INFO(deep);
    probe p;
    auto a = start(p, std::string("function deep()\n") + deep + "\nend");
    REQUIRE(a->wait_idle(5s));
    REQUIRE(a->run_function("command deep", "deep", {}));
    REQUIRE(a->wait_idle(10s));
    // Either a Lua error (the add-on carries on) or a limit (it stops):
    // never the process.
    const auto s = a->status();
    INFO(s.last_error << " / " << s.message);
    REQUIRE((s.running || s.stop != script_stop::none));
  }
}

TEST_CASE("a script that does not load stops with its reason", "[script][load]") {
  {
    probe p;
    auto a = start(p, "function broken(\n");
    REQUIRE(p.wait_stopped(5s));
    REQUIRE(p.why == script_stop::load_error);
    REQUIRE(p.message.find("main.lua:") != std::string::npos);
  }
  {
    probe p;
    auto a = start(p, "error('nope')");
    REQUIRE(p.wait_stopped(5s));
    REQUIRE(p.why == script_stop::load_error);
    REQUIRE(p.message.find("nope") != std::string::npos);
  }
  {
    // A runaway top level is a limit, not a load error.
    probe p;
    auto a = start(p, "while true do end");
    REQUIRE(p.wait_stopped(5s));
    REQUIRE((p.why == script_stop::instructions || p.why == script_stop::time));
  }
  {
    // Bytecode as a source file: refused by the loader's text-only mode.
    probe p;
    auto a = start(p, std::string("\x1bLua\x54\x00", 6));
    REQUIRE(p.wait_stopped(5s));
    REQUIRE(p.why == script_stop::load_error);
    REQUIRE(p.message.find("binary") != std::string::npos);
  }
}

TEST_CASE("a script error is reported and the add-on carries on", "[script][runtime]") {
  probe p;
  auto a = start(p, "function bad() error('boom') end function good() mv.log('fine') end");
  REQUIRE(a->wait_idle(5s));
  run(*a, "bad");
  REQUIRE(a->status().running);
  REQUIRE(a->status().last_error.find("boom") != std::string::npos);
  run(*a, "missing");
  REQUIRE(a->status().last_error.find("missing is not a function") != std::string::npos);
  run(*a, "good");
  REQUIRE(p.log_lines() == std::vector<std::string>{"fine"});
}

TEST_CASE("one add-on spinning does not hold up another", "[script][runtime]") {
  probe slow_p;
  probe fast_p;
  script_limits generous = fast_limits();
  generous.wall = 2000ms;
  generous.instructions = 10'000'000'000ull;
  auto slow = start(slow_p, "function spin() while true do end end", 0, generous);
  auto fast = start(fast_p, "function hi() mv.log('hi') end");
  REQUIRE(slow->wait_idle(5s));
  REQUIRE(fast->wait_idle(5s));
  REQUIRE(slow->run_function("spin", "spin", {}));
  std::this_thread::sleep_for(50ms);
  const auto t0 = clk::now();
  run(*fast, "hi");
  REQUIRE(clk::now() - t0 < 500ms);
  REQUIRE(fast_p.log_lines() == std::vector<std::string>{"hi"});
  REQUIRE(slow_p.wait_stopped(5s));
  REQUIRE(slow_p.why == script_stop::time);
}

TEST_CASE("stale tasks are dropped unrun and destroying stops a running task", "[script][runtime]") {
  probe p;
  auto a = start(p, "n = 0 function bump() n = n + 1 mv.log(n) end");
  REQUIRE(a->wait_idle(5s));
  for (std::uint64_t g = 1; g <= 3; ++g) {
    mv::addon::script_task t;
    t.what = "bump";
    t.generation = g;
    t.stale = [](std::uint64_t gen) { return gen < 3; };
    t.run = [](mv::addon::script_call& c) { c.call_global("bump"); };
    REQUIRE(a->post(std::move(t)));
  }
  REQUIRE(a->wait_idle(5s));
  REQUIRE(p.log_lines() == std::vector<std::string>{"1"});

  // The host stops an add-on mid-task (remove, quit): it returns promptly.
  probe q;
  script_limits generous = fast_limits();
  generous.wall = 60s;
  generous.instructions = 1'000'000'000'000ull;
  auto b = start(q, "function spin() while true do end end", 0, generous);
  REQUIRE(b->wait_idle(5s));
  REQUIRE(b->run_function("spin", "spin", {}));
  std::this_thread::sleep_for(50ms);
  const auto t0 = clk::now();
  b.reset();
  REQUIRE(clk::now() - t0 < 500ms);
  REQUIRE(q.why == script_stop::stopped);
}

TEST_CASE("mv.on handlers and JSON both ways", "[script][runtime]") {
  probe p;
  auto a = start(p, R"lua(
    mv.on("search", function(q, gen) mv.log("a", q.text, gen) end)
    mv.on("search", function(q, gen) mv.log("b", #q.list, q.list[2], q.flag) end)
  )lua");
  REQUIRE(a->wait_idle(5s));
  mv::addon::script_task t;
  t.what = "search";
  t.run = [](mv::addon::script_call& c) {
    auto q = mv::json::parse(R"({"text":"cat","list":[1,"two",null],"flag":false})");
    mv::json::value gen;
    gen.k = mv::json::kind::number;
    gen.is_integer = true;
    gen.i = 9;
    c.call_handlers("search", {*q, gen});
  };
  REQUIRE(a->post(std::move(t)));
  REQUIRE(a->wait_idle(5s));
  REQUIRE(p.log_lines() == std::vector<std::string>{"a\tcat\t9", "b\t2\ttwo\tfalse"});
}

TEST_CASE("the default limits as shipped stop a spin within a second", "[script][limits]") {
  probe p;
  auto a = start(p, "function spin() local x = 0 while true do x = x + 1 end end", 0,
                 script_limits{});
  REQUIRE(a->wait_idle(5s));
  const auto t0 = clk::now();
  REQUIRE(a->run_function("spin", "spin", {}));
  REQUIRE(p.wait_stopped(5s));
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(p.stopped_at - t0);
  WARN("default limits: stopped by " << mv::addon::script_stop_name(p.why) << " after "
                                     << took.count() << " ms");
  REQUIRE(took < script_limits{}.wall + 400ms);
}

TEST_CASE("a __gc metamethod never runs: not during a task, a stop or teardown", "[script][sandbox]") {
  // Lua runs a finalizer with hooks off (GCTM), so the count hook and the
  // watchdog could not stop one that loops: during a collect it held the
  // worker for good, and at lua_close it hung the destructor's join.
  {
    auto p = std::make_unique<probe>();
    auto a = start(*p, R"lua(
      function t()
        setmetatable({}, {__gc = function() while true do end end})
        collectgarbage()
        collectgarbage()
        mv.log("collected")
      end
    )lua");
    REQUIRE(a->wait_idle(5s));
    const bool ended = task_ends_within(a, "t", 3s);
    if (!ended) (void)p.release();
    REQUIRE(ended);
    REQUIRE(p->log_lines() == std::vector<std::string>{"collected"});
    REQUIRE(destroys_within(a, 3s));
  }
  {
    // Left for lua_close: the host's stop must still return.
    auto p = std::make_unique<probe>();
    auto a = start(*p, "keep = setmetatable({}, {__gc = function() while true do end end})");
    REQUIRE(a->wait_idle(5s));
    const bool destroyed = destroys_within(a, 3s);
    if (!destroyed) (void)p.release();
    REQUIRE(destroyed);
  }
  {
    // A stopped add-on says nothing more, from a finalizer or anywhere.
    auto p = std::make_unique<probe>();
    auto a = start(*p, R"lua(
      function t()
        for i = 1, 100 do setmetatable({}, {__gc = function() mv.notify("after") end}) end
        while true do end
      end
    )lua");
    REQUIRE(a->wait_idle(5s));
    REQUIRE(a->run_function("spin", "t", {}));
    REQUIRE(p->wait_stopped(5s));
    const bool destroyed = destroys_within(a, 3s);
    if (!destroyed) (void)p.release();
    REQUIRE(destroyed);
    std::lock_guard<std::mutex> lock(p->m);
    REQUIRE(p->notices.empty());
  }
}

TEST_CASE("weak tables are strong, so no collect runs unbounded", "[script][sandbox]") {
  // A chain of ephemerons (weak keys, each value the next key) inserted in a
  // shuffled order made one atomic GC step converge in O(n^2), where neither
  // the count hook nor the watchdog's flag is looked at: > 4 s at n = 200,000
  // under the shipped 64 MB.
  auto p = std::make_unique<probe>();
  auto a = start(*p, R"lua(
    function t()
      local n = 200000
      local keys = {}
      for i = 1, n do keys[i] = {} end
      local idx = {}
      for i = 1, n do idx[i] = i end
      for i = n, 2, -1 do local j = math.random(i) idx[i], idx[j] = idx[j], idx[i] end
      local w = setmetatable({}, {__mode = "k"})
      for _, i in ipairs(idx) do if i < n then w[keys[i]] = keys[i + 1] end end
      root = keys[1]
      keys, idx = nil, nil
      collectgarbage()
      local v = setmetatable({}, {__mode = "v"})
      v[1] = {}
      collectgarbage()
      mv.log("kept", v[1] ~= nil)
    end
  )lua", 0, script_limits{});
  REQUIRE(a->wait_idle(5s));
  const bool ended = task_ends_within(a, "t", 3s);
  if (!ended) (void)p.release();
  REQUIRE(ended);
  // Ending at all is the point (before, the collect never returned). A slow
  // runner (TSan, a loaded CI box) may spend the 1 s building the chain.
  if (p->wait_stopped(200ms)) {
    REQUIRE((p->why == script_stop::time || p->why == script_stop::instructions));
  } else {
    REQUIRE(p->log_lines() == std::vector<std::string>{"kept\ttrue"});
  }
}

TEST_CASE("table.insert, table.remove and a sort with a C comparator stop on time", "[script][limits]") {
  // __len says how far these loops go (up to 2^63), and on a plain table or
  // with a C function as comparator they run no VM instruction: before the
  // flag was checked in them, each held the worker for good.
  for (const char* slow : {
           "local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end})\n"
           "table.insert(t, 1, 0)",
           "local t = setmetatable({}, {__len = function() return math.maxinteger - 1 end})\n"
           "table.remove(t, 1)",
           "local t = setmetatable({}, {__len = function() return 2^31 - 2 end})\n"
           "table.sort(t, rawequal)",
       }) {
    INFO(slow);
    auto p = std::make_unique<probe>();
    auto a = start(*p, std::string("function slow()\n") + slow + "\nend");
    REQUIRE(a->wait_idle(5s));
    const auto t0 = clk::now();
    REQUIRE(a->run_function("command slow", "slow", {}));
    const bool stopped = p->wait_stopped(3s);
    if (!stopped) {
      (void)a.release();
      (void)p.release();
    }
    REQUIRE(stopped);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(p->stopped_at - t0);
    INFO(took.count() << " ms");
    REQUIRE((p->why == script_stop::time || p->why == script_stop::instructions));
    REQUIRE(took < fast_limits().wall + 400ms);
  }
}

TEST_CASE("a plain string.find stops on time", "[script][limits]") {
  // A plain find (or a pattern with no specials) is a memchr / memcmp loop,
  // O(len(s) * len(p)) with no VM instruction: 16 MB against 8 MB held the
  // worker for good under the shipped 64 MB.
  for (const char* slow : {
           "local s = string.rep('a', 1 << 24) local p = string.rep('a', 1 << 23) .. 'b'\n"
           "s:find(p, 1, true)",
           "local s = string.rep('a', 1 << 24) local p = string.rep('a', 1 << 23) .. 'b'\n"
           "s:find(p)",
       }) {
    INFO(slow);
    auto p = std::make_unique<probe>();
    auto a = start(*p, std::string("function slow()\n") + slow + "\nend", 0, script_limits{});
    REQUIRE(a->wait_idle(5s));
    const auto t0 = clk::now();
    REQUIRE(a->run_function("command slow", "slow", {}));
    const bool stopped = p->wait_stopped(4s);
    if (!stopped) {
      (void)a.release();
      (void)p.release();
    }
    REQUIRE(stopped);
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(p->stopped_at - t0);
    INFO(took.count() << " ms");
    REQUIRE(p->why == script_stop::time);
    REQUIRE(took < script_limits{}.wall + 400ms);
  }
  // A find that succeeds is unchanged.
  const auto out = eval("mv.log(('hello world'):find('o w', 1, true), ('abc'):find('c'))");
  REQUIRE(out[0] == "5\t3\t3");  // the first call is cut to one value
}

TEST_CASE("mv.notify shows at most 200 bytes of a long line, cut on a character", "[script][runtime]") {
  probe p;
  auto a = start(p, R"lua(
    function t()
      mv.notify(string.rep("x", 199) .. "\u{e9}" .. string.rep("y", 1 << 22))
    end
  )lua", 0, script_limits{});
  REQUIRE(a->wait_idle(5s));
  run(*a, "t");
  std::lock_guard<std::mutex> lock(p.m);
  REQUIRE(p.notices.size() == 1);
  REQUIRE(p.notices[0] == std::string(199, 'x'));  // the two-byte e-acute would cross 200
}
