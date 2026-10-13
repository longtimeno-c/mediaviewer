// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addon/script_runtime.h"

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

#include "addon/open_manifest.h"
#include "addon/script_thread.h"

// Lua is built as C++ (third_party/lua/VENDOR.txt): include the plain headers,
// never lua.hpp, whose extern "C" would not match the library.
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "mv_interrupt.h"

static_assert(LUA_EXTRASPACE >= sizeof(void*), "the state's extra space holds its owner");

namespace mv::addon {

using clock_type = std::chrono::steady_clock;

struct script_addon::impl {
  script_addon* self = nullptr;
  script_limits limits;

  // ---- limits (the worker, except `interrupt`) ----
  std::atomic<int> interrupt{0};  // a script_stop: set once, by whoever sees a limit first
  std::size_t used = 0;           // bytes the state holds
  std::atomic<std::size_t> peak{0};
  std::uint64_t instructions = 0;  // this task
  clock_type::time_point deadline{};
  std::string task_what;
  lua_State* L = nullptr;

  // ---- the queue ----
  mutable std::mutex m;
  std::condition_variable cv;
  mutable std::condition_variable idle_cv;
  std::deque<script_task> queue;
  bool busy = true;  // building the state counts as work
  bool quit = false;
  bool ended = false;  // the worker has closed the state
  script_status st;
  std::string stop_message;
  std::function<void(script_stop, const std::string&)> on_stopped;

  // mv.notify's rate: four lines a second at most.
  clock_type::time_point notify_window{};
  int notify_count = 0;

  script_thread worker;

  // Records the first limit reached and makes the state stop at its next
  // check (the count hook, or one of the library loops mv_interrupt.h names).
  // Any thread.
  void request_stop(script_stop why, std::string message) {
    int expected = 0;
    if (interrupt.compare_exchange_strong(expected, static_cast<int>(why))) {
      std::lock_guard<std::mutex> lock(m);
      stop_message = std::move(message);
    }
  }
  [[nodiscard]] script_stop stopping() const noexcept {
    return static_cast<script_stop>(interrupt.load(std::memory_order_relaxed));
  }

  void run();
  bool build_state();
  void close_state();
  void begin_task(const std::string& what);
  void end_task();
};

namespace {

script_addon::impl* impl_of(lua_State* L) noexcept {
  return *static_cast<script_addon::impl**>(lua_getextraspace(L));
}

// ---- the watchdog -----------------------------------------------------------
//
// The count hook checks the clock itself, so a script spinning in Lua stops on
// time without this. It exists for the library loops that run no VM
// instruction for as long as the script's arguments say: it raises the flag
// they check. One thread for every add-on, asleep unless a task is running:
// an idle add-on costs nothing (docs/design/25 §11).
class watchdog {
 public:
  static watchdog& get() {
    // Never destroyed: its thread sleeps on the condition variable at exit.
    static watchdog* w = new watchdog;
    return *w;
  }
  void arm(script_addon::impl* who, clock_type::time_point deadline) {
    std::lock_guard<std::mutex> lock(m_);
    if (!started_) {
      started_ = true;
      std::thread([this] { loop(); }).detach();
    }
    armed_.push_back({who, deadline});
    cv_.notify_one();
  }
  void disarm(script_addon::impl* who) {
    std::lock_guard<std::mutex> lock(m_);
    armed_.erase(std::remove_if(armed_.begin(), armed_.end(),
                                [who](const entry& e) { return e.who == who; }),
                 armed_.end());
  }

 private:
  struct entry {
    script_addon::impl* who;
    clock_type::time_point deadline;
  };
  void loop() {
    std::unique_lock<std::mutex> lock(m_);
    for (;;) {
      if (armed_.empty()) {
        cv_.wait(lock);
        continue;
      }
      const auto earliest = std::min_element(armed_.begin(), armed_.end(),
                                             [](const entry& a, const entry& b) {
                                               return a.deadline < b.deadline;
                                             })->deadline;
      if (cv_.wait_until(lock, earliest) == std::cv_status::timeout) {
        const auto now = clock_type::now();
        // Under the lock: a task disarms before its add-on can go away. A
        // fired entry is done with; its task's disarm finds nothing.
        armed_.erase(std::remove_if(armed_.begin(), armed_.end(),
                                    [now](const entry& e) {
                                      if (e.deadline > now) return false;
                                      e.who->request_stop(script_stop::time, {});
                                      return true;
                                    }),
                     armed_.end());
      }
    }
  }
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<entry> armed_;
  bool started_ = false;
};

// ---- memory -------------------------------------------------------------------

void* sandbox_alloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* I = static_cast<script_addon::impl*>(ud);
  const std::size_t old = ptr ? osize : 0;  // with no block, osize names a type
  if (nsize == 0) {
    std::free(ptr);
    I->used -= old;
    return nullptr;
  }
  if (nsize > old) {
    const std::size_t grow = nsize - old;
    // Once over, always over: the add-on stops, and nothing it does in the
    // meantime (catching the error, freeing a little) buys it more.
    if (I->interrupt.load(std::memory_order_relaxed) == static_cast<int>(script_stop::memory) ||
        I->used + grow > I->limits.memory_bytes) {
      I->request_stop(script_stop::memory, {});
      return nullptr;
    }
  }
  void* p = std::realloc(ptr, nsize);
  if (!p) return nullptr;
  I->used = I->used - old + nsize;
  if (I->used > I->peak.load(std::memory_order_relaxed)) {
    I->peak.store(I->used, std::memory_order_relaxed);
  }
  return p;
}

// ---- instructions and time ------------------------------------------------------

void count_hook(lua_State* L, lua_Debug*) {
  script_addon::impl* I = impl_of(L);
  if (I->stopping() == script_stop::none) {
    I->instructions += I->limits.hook_every;
    if (I->instructions > I->limits.instructions) {
      I->request_stop(script_stop::instructions, {});
    } else if (clock_type::now() > I->deadline) {
      I->request_stop(script_stop::time, {});
    }
  }
  if (I->stopping() != script_stop::none) {
    // From now on every instruction raises, so a pcall in a loop cannot
    // swallow the stop: the instruction after it raises outside it.
    lua_sethook(L, count_hook, LUA_MASKCOUNT, 1);
    luaL_error(L, "stopped");
  }
}

// ---- protected calls ------------------------------------------------------------

using protected_fn = std::function<int(lua_State*)>;

int trampoline(lua_State* L) {
  auto* fn = static_cast<const protected_fn*>(lua_touserdata(L, 1));
  lua_remove(L, 1);
  return (*fn)(L);
}

int traceback(lua_State* L) {
  const char* msg = lua_tostring(L, 1);
  if (!msg) msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  luaL_traceback(L, L, msg, 1);
  return 1;
}

std::string error_text(lua_State* L, int index) {
  std::size_t n = 0;
  const char* s = lua_type(L, index) == LUA_TSTRING ? lua_tolstring(L, index, &n) : nullptr;
  std::string out = s ? std::string(s, n) : std::string("error");
  if (out.size() > 1024) out.resize(1024);
  return out;
}

// Runs `fn` on L in protected mode with a traceback handler. Every C++ entry
// into the state goes through here, so a Lua error (a C++ exception: Lua is
// built as C++) never escapes into the runtime.
int protect(lua_State* L, const protected_fn& fn, std::string* error) {
  if (!lua_checkstack(L, 4)) {
    if (error) *error = "stack overflow";
    return LUA_ERRMEM;
  }
  const int base = lua_gettop(L);
  lua_pushcfunction(L, traceback);
  lua_pushcfunction(L, trampoline);
  lua_pushlightuserdata(L, const_cast<protected_fn*>(&fn));
  const int rc = lua_pcall(L, 1, 0, base + 1);
  if (rc != LUA_OK && error) *error = error_text(L, -1);
  lua_settop(L, base);
  return rc;
}

// ---- the environment ------------------------------------------------------------

int safe_load(lua_State* L) {
  std::size_t n = 0;
  const char* text = luaL_checklstring(L, 1, &n);  // text only: no reader functions
  const char* name = luaL_optstring(L, 2, "=(load)");
  // arg 3 (mode) is ignored: always "t". A binary chunk is refused.
  const bool has_env = !lua_isnone(L, 4);
  const int rc = luaL_loadbufferx(L, text, n, name, "t");
  if (rc != LUA_OK) {
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
  }
  if (has_env) {
    lua_pushvalue(L, 4);
    if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1);
  }
  return 1;
}

int safe_collectgarbage(lua_State* L) {
  static const char* const kOptions[] = {"collect", "count", "step", "isrunning", nullptr};
  const int o = luaL_checkoption(L, 1, "collect", kOptions);
  switch (o) {
    case 0:
      lua_gc(L, LUA_GCCOLLECT);
      lua_pushinteger(L, 0);
      return 1;
    case 1: {
      const int kb = lua_gc(L, LUA_GCCOUNT);
      const int b = lua_gc(L, LUA_GCCOUNTB);
      lua_pushnumber(L, static_cast<lua_Number>(kb) + static_cast<lua_Number>(b) / 1024.0);
      return 1;
    }
    case 2: {
      const int step = static_cast<int>(std::clamp<lua_Integer>(luaL_optinteger(L, 2, 0), 0, 1 << 20));
      lua_pushboolean(L, lua_gc(L, LUA_GCSTEP, step));
      return 1;
    }
    default:
      lua_pushboolean(L, lua_gc(L, LUA_GCISRUNNING));
      return 1;
  }
}

std::string joined_args(lua_State* L) {
  std::string out;
  const int n = lua_gettop(L);
  for (int i = 1; i <= n; ++i) {
    std::size_t len = 0;
    const char* s = luaL_tolstring(L, i, &len);
    if (i > 1) out.push_back('\t');
    out.append(s, std::min<std::size_t>(len, 1024));
    lua_pop(L, 1);
    if (out.size() >= 1024) {
      out.resize(1024);
      break;
    }
  }
  return out;
}

int mv_log(lua_State* L) {
  script_addon* a = script_owner(L);
  const std::string text = joined_args(L);
  if (a && a->services().log) a->services().log(a->id(), 0, text);
  return 0;
}

// One line a person reads: control characters become spaces, at most 200
// bytes, cut on a character boundary.
std::string notice_text(std::string_view s) {
  // Only the head can be shown: never copy a script's 60 MB string to cut it.
  s = s.substr(0, 204);
  std::string out;
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    out.push_back(c < 0x20 || c == 0x7F ? ' ' : ch);
  }
  if (out.size() > 200) {
    std::size_t cut = 200;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
    out.resize(cut);
  }
  return out;
}

int mv_notify(lua_State* L) {
  std::size_t n = 0;
  const char* s = luaL_checklstring(L, 1, &n);
  script_addon* a = script_owner(L);
  script_addon::impl* I = impl_of(L);
  const auto now = clock_type::now();
  if (now - I->notify_window > std::chrono::seconds(1)) {
    I->notify_window = now;
    I->notify_count = 0;
  }
  const bool shown = ++I->notify_count <= 4 && a && a->services().notify;
  if (shown) a->services().notify(a->id(), notice_text(std::string_view(s, n)));
  lua_pushboolean(L, shown);
  return 1;
}

constexpr const char* kHandlersKey = "mv.handlers";

int mv_on(lua_State* L) {
  const char* event = luaL_checkstring(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION);
  if (std::strlen(event) > 64) return luaL_argerror(L, 1, "event name too long");
  lua_getfield(L, LUA_REGISTRYINDEX, kHandlersKey);
  if (lua_getfield(L, -1, event) != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, event);
  }
  const lua_Unsigned len = lua_rawlen(L, -1);
  if (len >= 64) return luaL_error(L, "too many handlers for %s", event);
  lua_pushvalue(L, 2);
  lua_rawseti(L, -2, static_cast<lua_Integer>(len) + 1);
  return 0;
}

void install_core(lua_State* L, script_addon& a) {
  lua_pushstring(L, a.id().c_str());
  lua_setfield(L, -2, "id");
  lua_pushinteger(L, kContributionApi);
  lua_setfield(L, -2, "api");
  lua_newtable(L);
  for (std::uint32_t bit = 1; bit <= kPermissionsAll; bit <<= 1) {
    if (a.permissions() & bit) {
      lua_pushboolean(L, 1);
      lua_setfield(L, -2, permission_name(bit));
    }
  }
  lua_setfield(L, -2, "permissions");
  lua_pushcfunction(L, mv_log);
  lua_setfield(L, -2, "log");
  lua_pushcfunction(L, mv_notify);
  lua_setfield(L, -2, "notify");
  lua_pushcfunction(L, mv_on);
  lua_setfield(L, -2, "on");
}

int folder_count(lua_State* L) {
  script_addon* a = script_owner(L);
  lua_pushinteger(L, a && a->services().folder_count ? a->services().folder_count() : 0);
  return 1;
}
int folder_path(lua_State* L) {
  script_addon* a = script_owner(L);
  const std::string p = a && a->services().folder_path ? a->services().folder_path() : std::string();
  if (p.empty()) {
    lua_pushnil(L);
  } else {
    lua_pushlstring(L, p.data(), p.size());
  }
  return 1;
}
int folder_name(lua_State* L) {
  const lua_Integer i = luaL_checkinteger(L, 1);  // 1-based, as Lua counts
  script_addon* a = script_owner(L);
  if (i < 1 || i > 0xFFFFFFFFll || !a || !a->services().folder_item_name) {
    lua_pushnil(L);
    return 1;
  }
  const std::string n = a->services().folder_item_name(static_cast<std::uint32_t>(i - 1));
  if (n.empty()) {
    lua_pushnil(L);
  } else {
    lua_pushlstring(L, n.data(), n.size());
  }
  return 1;
}
void install_folder(lua_State* L, script_addon&) {
  lua_newtable(L);
  lua_pushcfunction(L, folder_count);
  lua_setfield(L, -2, "count");
  lua_pushcfunction(L, folder_path);
  lua_setfield(L, -2, "path");
  lua_pushcfunction(L, folder_name);
  lua_setfield(L, -2, "name");
  lua_setfield(L, -2, "folder");
}

int selection_current(lua_State* L) {
  script_addon* a = script_owner(L);
  const std::string p = a && a->services().current_path ? a->services().current_path() : std::string();
  if (p.empty()) {
    lua_pushnil(L);
  } else {
    lua_pushlstring(L, p.data(), p.size());
  }
  return 1;
}
void install_selection(lua_State* L, script_addon&) {
  lua_newtable(L);
  lua_pushcfunction(L, selection_current);
  lua_setfield(L, -2, "current");
  lua_setfield(L, -2, "selection");
}

// mv.settings.get(key): what the person chose on this add-on's own settings
// page (or the page's default), as a Lua value; nil for a key the page lacks.
int settings_get(lua_State* L) {
  std::size_t n = 0;
  const char* key = luaL_checklstring(L, 1, &n);
  script_addon* a = script_owner(L);
  std::optional<json::value> v;
  if (a && a->services().setting) {
    if (auto text = a->services().setting(std::string(key, n))) v = json::parse(*text);
  }
  if (v) {
    push_json(L, *v);
  } else {
    lua_pushnil(L);
  }
  return 1;
}
void install_settings(lua_State* L, script_addon&) {
  lua_newtable(L);
  lua_pushcfunction(L, settings_get);
  lua_setfield(L, -2, "get");
  lua_setfield(L, -2, "settings");
}

std::mutex& bindings_mutex() {
  static std::mutex m;
  return m;
}
std::vector<script_binding>& bindings() {
  static std::vector<script_binding> b{
      {"core", 3, 0, install_core},
      {"folder", 3, perm_folder_read, install_folder},
      {"selection", 3, perm_selection, install_selection},
      // PR 56's settings page, read by its own add-on (docs/design/25 "Settings page").
      {"settings", 3, 0, install_settings},
  };
  return b;
}

// The libraries a script gets, and what is taken out of them.
int open_sandbox(lua_State* L) {
  luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
  luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
  luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
  luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
  luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
  luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
  lua_settop(L, 0);

  lua_pushglobaltable(L);
  for (const char* name : {"dofile", "loadfile", "require", "warn"}) {
    lua_pushnil(L);
    lua_setfield(L, -2, name);
  }
  lua_pushcfunction(L, safe_load);
  lua_setfield(L, -2, "load");
  lua_pushcfunction(L, safe_collectgarbage);
  lua_setfield(L, -2, "collectgarbage");
  lua_pushcfunction(L, mv_log);
  lua_setfield(L, -2, "print");

  // string.dump writes bytecode; nothing loads it, and nothing makes it.
  lua_getfield(L, -1, LUA_STRLIBNAME);
  lua_pushnil(L);
  lua_setfield(L, -2, "dump");
  lua_pop(L, 1);
  // The string metatable is shared by every string: lock it, so
  // getmetatable("") returns false and nothing replaces __index.
  lua_pushliteral(L, "");
  if (lua_getmetatable(L, -1)) {
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    lua_pop(L, 1);
  }
  lua_pop(L, 1);

  lua_newtable(L);
  lua_setfield(L, LUA_REGISTRYINDEX, kHandlersKey);

  // mv, with the bindings this add-on's API and permissions allow.
  script_addon* a = script_owner(L);
  lua_newtable(L);
  std::vector<script_binding> list;
  {
    std::lock_guard<std::mutex> lock(bindings_mutex());
    list = bindings();
  }
  // impl_of gives the config through the owner.
  for (const script_binding& b : list) {
    const bool api_ok = kContributionApi >= b.api;
    const bool perm_ok = (a->permissions() & b.permission) == b.permission;
    if (api_ok && perm_ok) b.install(L, *a);
  }
  lua_setfield(L, -2, "mv");
  lua_pop(L, 1);
  return 0;
}

std::string limit_message(script_stop why, const std::string& what, const script_limits& l) {
  const std::string task = what.empty() ? std::string() : " (" + what + ")";
  switch (why) {
    case script_stop::memory:
      return "It asked for more than " + std::to_string(l.memory_bytes >> 20) +
             " MB of memory, the most an add-on may use.";
    case script_stop::instructions:
      return "One of its tasks" + task + " ran for more than " +
             std::to_string(l.instructions / 1'000'000) + " million steps.";
    case script_stop::time:
      return "One of its tasks" + task + " ran for more than " +
             std::to_string(l.wall.count()) + " ms.";
    default:
      return {};
  }
}

}  // namespace

}  // namespace mv::addon

// The flag mv_interrupt.h's loops check (third_party/lua/VENDOR.txt).
int mv_lua_interrupted(lua_State* L) {
  return mv::addon::impl_of(L)->interrupt.load(std::memory_order_relaxed) != 0 ? 1 : 0;
}

namespace mv::addon {

const char* script_stop_name(script_stop s) noexcept {
  switch (s) {
    case script_stop::none: return "none";
    case script_stop::memory: return "memory";
    case script_stop::instructions: return "instructions";
    case script_stop::time: return "time";
    case script_stop::load_error: return "load_error";
    case script_stop::stopped: return "stopped";
  }
  return "none";
}

script_addon* script_owner(lua_State* L) noexcept {
  script_addon::impl* I = impl_of(L);
  return I ? I->self : nullptr;
}

void add_script_binding(const script_binding& binding) {
  std::lock_guard<std::mutex> lock(bindings_mutex());
  bindings().push_back(binding);
}

// ---- the worker -------------------------------------------------------------------

void script_addon::impl::begin_task(const std::string& what) {
  task_what = what;
  instructions = 0;
  deadline = clock_type::now() + limits.wall;
  lua_sethook(L, count_hook, LUA_MASKCOUNT, static_cast<int>(limits.hook_every));
  watchdog::get().arm(this, deadline);
}

void script_addon::impl::end_task() {
  watchdog::get().disarm(this);
  if (L) lua_settop(L, 0);
}

bool script_addon::impl::build_state() {
  L = lua_newstate(sandbox_alloc, this);
  if (!L) {
    request_stop(script_stop::memory, {});
    return false;
  }
  *static_cast<impl**>(lua_getextraspace(L)) = this;
  begin_task("loading");
  std::string error;
  int rc = protect(L, [](lua_State* s) { return open_sandbox(s); }, &error);
  for (std::size_t i = 0; rc == LUA_OK && i < self->config_.sources.size(); ++i) {
    const script_source& src = self->config_.sources[i];
    const std::string chunk = "@" + src.name;
    rc = protect(L,
                 [&](lua_State* s) {
                   if (luaL_loadbufferx(s, src.text.data(), src.text.size(), chunk.c_str(), "t") !=
                       LUA_OK) {
                     return lua_error(s);
                   }
                   lua_call(s, 0, 0);
                   return 0;
                 },
                 &error);
  }
  end_task();
  if (rc == LUA_OK && stopping() == script_stop::none) return true;
  if (stopping() == script_stop::none) {
    request_stop(script_stop::load_error, "Its script did not load: " + error);
  }
  return false;
}

void script_addon::impl::close_state() {
  if (!L) return;
  // __gc metamethods run during close; with the flag up and the hook at
  // every instruction, each stops at its first.
  request_stop(script_stop::stopped, {});
  lua_sethook(L, count_hook, LUA_MASKCOUNT, 1);
  lua_close(L);
  L = nullptr;
}

void script_addon::impl::run() {
  bool ok = false;
  try {
    ok = build_state();
  } catch (...) {
    // A host allocation threw between begin_task and end_task: the watchdog
    // must not keep a pointer to this impl past its life.
    watchdog::get().disarm(this);
    request_stop(script_stop::load_error, "Its script did not load.");
  }
  if (ok) {
    std::unique_lock<std::mutex> lock(m);
    st.running = true;
    busy = false;
    idle_cv.notify_all();
    for (;;) {
      cv.wait(lock, [this] { return quit || !queue.empty() || stopping() != script_stop::none; });
      if (quit || stopping() != script_stop::none) break;
      script_task t = std::move(queue.front());
      queue.pop_front();
      busy = true;
      lock.unlock();
      if (!t.stale || !t.stale(t.generation)) {
        begin_task(t.what);
        script_call call(L, self);
        try {
          if (t.run) t.run(call);
        } catch (...) {
          // A host binding threw (std::bad_alloc): the task is lost, the
          // add-on is not.
        }
        if (!call.error().empty() && stopping() == script_stop::none) {
          std::lock_guard<std::mutex> g(m);
          st.last_error = call.error();
        }
        end_task();
        if (!call.error().empty() && stopping() == script_stop::none && self->services_.log) {
          self->services_.log(self->id(), 2, call.error());
        }
      }
      lock.lock();
      busy = false;
      ++st.tasks;
      idle_cv.notify_all();
    }
  }
  // Stopped: by a limit, by a load error, or by the host.
  const script_stop why_first = stopping() == script_stop::none ? script_stop::stopped : stopping();
  const std::string what = task_what;
  close_state();
  std::string message;
  std::function<void(script_stop, const std::string&)> notify_stopped;
  {
    std::lock_guard<std::mutex> lock(m);
    notify_stopped = on_stopped;
    message = stop_message.empty() ? limit_message(why_first, what, limits) : stop_message;
    st.running = false;
    st.stop = why_first;
    st.message = message;
    queue.clear();
    busy = false;
    ended = true;
  }
  idle_cv.notify_all();
  if (why_first != script_stop::stopped && self->services_.log) {
    self->services_.log(self->id(), 2, std::string("stopped (") + script_stop_name(why_first) +
                                           "): " + message);
  }
  if (notify_stopped) notify_stopped(why_first, message);
}

// ---- script_addon -----------------------------------------------------------------

std::unique_ptr<script_addon> script_addon::start(script_config config, script_services services) {
  std::unique_ptr<script_addon> a(new script_addon);
  a->config_ = std::move(config);
  a->services_ = std::move(services);
  a->impl_ = std::make_unique<impl>();
  a->impl_->self = a.get();
  a->impl_->limits = a->config_.limits;
  impl* I = a->impl_.get();
  if (!I->worker.start(I->limits.stack_bytes, [I] { I->run(); })) {
    I->request_stop(script_stop::load_error, "MediaViewer could not start its worker.");
    std::lock_guard<std::mutex> lock(I->m);
    I->st.stop = script_stop::load_error;
    I->st.message = I->stop_message;
    I->ended = true;
    I->busy = false;
  }
  return a;
}

script_addon::~script_addon() {
  if (!impl_) return;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    impl_->quit = true;
  }
  impl_->request_stop(script_stop::stopped, {});
  impl_->cv.notify_all();
  impl_->worker.join();
}

void script_addon::set_on_stopped(std::function<void(script_stop, const std::string&)> cb) {
  script_stop stop = script_stop::none;
  std::string message;
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (!impl_->ended) {
      impl_->on_stopped = std::move(cb);
      return;
    }
    stop = impl_->st.stop;
    message = impl_->st.message;
  }
  // It stopped before anyone asked: say so now, on this thread.
  if (cb) cb(stop, message);
}

bool script_addon::post(script_task task) {
  {
    std::lock_guard<std::mutex> lock(impl_->m);
    if (impl_->ended || impl_->quit || impl_->stopping() != script_stop::none) return false;
    if (impl_->queue.size() >= impl_->limits.queue) return false;
    impl_->queue.push_back(std::move(task));
  }
  impl_->cv.notify_one();
  return true;
}

bool script_addon::run_function(std::string what, std::string function_name, json::value payload) {
  script_task t;
  t.what = std::move(what);
  t.run = [name = std::move(function_name), arg = std::move(payload)](script_call& c) {
    std::vector<json::value> args;
    args.push_back(arg);
    (void)c.call_global(name, args);
  };
  return post(std::move(t));
}

script_status script_addon::status() const {
  std::lock_guard<std::mutex> lock(impl_->m);
  script_status s = impl_->st;
  s.memory_peak = impl_->peak.load(std::memory_order_relaxed);
  return s;
}

bool script_addon::wait_idle(std::chrono::milliseconds timeout) const {
  std::unique_lock<std::mutex> lock(impl_->m);
  return impl_->idle_cv.wait_for(lock, timeout, [this] {
    return impl_->ended || (!impl_->busy && impl_->queue.empty());
  });
}

// ---- script_call -------------------------------------------------------------------

bool script_call::call_global(std::string_view name, const std::vector<json::value>& args) {
  if (!L_) return false;
  const std::string fn(name);
  const int rc = protect(L_,
                         [&](lua_State* s) {
                           if (lua_getglobal(s, fn.c_str()) != LUA_TFUNCTION) {
                             return luaL_error(s, "%s is not a function", fn.c_str());
                           }
                           luaL_checkstack(s, static_cast<int>(args.size()) + 4, "arguments");
                           for (const json::value& v : args) push_json(s, v);
                           lua_call(s, static_cast<int>(args.size()), 0);
                           return 0;
                         },
                         &error_);
  return rc == LUA_OK;
}

bool script_call::with_state(const std::function<void(lua_State*)>& fn) {
  if (!L_) return false;
  return protect(L_,
                 [&](lua_State* s) {
                   fn(s);
                   return 0;
                 },
                 &error_) == LUA_OK;
}

bool script_call::call_handlers(std::string_view event, const std::vector<json::value>& args) {
  if (!L_) return false;
  const std::string ev(event);
  const int rc = protect(L_,
                         [&](lua_State* s) {
                           lua_getfield(s, LUA_REGISTRYINDEX, kHandlersKey);
                           if (lua_getfield(s, -1, ev.c_str()) != LUA_TTABLE) return 0;
                           const lua_Unsigned n = lua_rawlen(s, -1);
                           for (lua_Unsigned i = 1; i <= n; ++i) {
                             lua_rawgeti(s, -1, static_cast<lua_Integer>(i));
                             luaL_checkstack(s, static_cast<int>(args.size()) + 4, "arguments");
                             for (const json::value& v : args) push_json(s, v);
                             lua_call(s, static_cast<int>(args.size()), 0);
                           }
                           return 0;
                         },
                         &error_);
  return rc == LUA_OK;
}

// ---- JSON --------------------------------------------------------------------------

void push_json(lua_State* L, const json::value& v) {
  luaL_checkstack(L, 3, "json");
  switch (v.k) {
    case json::kind::null:
      lua_pushnil(L);
      return;
    case json::kind::boolean:
      lua_pushboolean(L, v.b);
      return;
    case json::kind::number:
      if (v.is_integer) {
        lua_pushinteger(L, v.i);
      } else {
        lua_pushnumber(L, v.d);
      }
      return;
    case json::kind::string:
      lua_pushlstring(L, v.s.data(), v.s.size());
      return;
    case json::kind::array: {
      lua_createtable(L, static_cast<int>(std::min<std::size_t>(v.a.size(), 1u << 20)), 0);
      lua_Integer i = 1;
      for (const json::value& e : v.a) {
        push_json(L, e);
        lua_rawseti(L, -2, i++);
      }
      return;
    }
    case json::kind::object: {
      lua_createtable(L, 0, static_cast<int>(std::min<std::size_t>(v.o.size(), 1u << 20)));
      for (const auto& [k, e] : v.o) {
        push_json(L, e);
        lua_setfield(L, -2, k.c_str());
      }
      return;
    }
  }
}

namespace {

struct json_out {
  json::writer w;
  std::unordered_set<const void*> open;  // tables on the way down: a cycle
  std::size_t max_bytes;
  std::string why;
};

bool to_json_rec(lua_State* L, int index, json_out& o, int depth) {
  if (depth > 32) {
    o.why = "nested deeper than 32";
    return false;
  }
  index = lua_absindex(L, index);
  switch (lua_type(L, index)) {
    case LUA_TNIL:
      o.w.null();
      return true;
    case LUA_TBOOLEAN:
      o.w.boolean(lua_toboolean(L, index) != 0);
      return true;
    case LUA_TNUMBER:
      if (lua_isinteger(L, index)) {
        o.w.integer(lua_tointeger(L, index));
      } else {
        const lua_Number d = lua_tonumber(L, index);
        if (d != d || d > 1e308 || d < -1e308) {
          o.why = "a number that is not finite";
          return false;
        }
        o.w.number(d);
      }
      return true;
    case LUA_TSTRING: {
      std::size_t n = 0;
      const char* s = lua_tolstring(L, index, &n);
      if (!json::detail::valid_utf8(std::string_view(s, n))) {
        o.why = "a string that is not UTF-8 text";
        return false;
      }
      o.w.string(std::string_view(s, n));
      return true;
    }
    case LUA_TTABLE: {
      const void* p = lua_topointer(L, index);
      if (!o.open.insert(p).second) {
        o.why = "a table that contains itself";
        return false;
      }
      luaL_checkstack(L, 4, "json");
      // A sequence 1..n with nothing else is an array; an empty table is [].
      const lua_Unsigned n = lua_rawlen(L, index);
      std::size_t keys = 0;
      bool string_keys = true;
      lua_pushnil(L);
      while (lua_next(L, index) != 0) {
        ++keys;
        if (lua_type(L, -2) != LUA_TSTRING) string_keys = false;
        lua_pop(L, 1);
      }
      if (keys == n) {
        o.w.begin_array();
        for (lua_Unsigned i = 1; i <= n; ++i) {
          lua_rawgeti(L, index, static_cast<lua_Integer>(i));
          const bool ok = to_json_rec(L, -1, o, depth + 1);
          lua_pop(L, 1);
          if (!ok) return false;
          if (o.w.str().size() > o.max_bytes) {
            o.why = "too large";
            return false;
          }
        }
        o.w.end_array();
      } else if (string_keys) {
        o.w.begin_object();
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
          std::size_t kn = 0;
          const char* k = lua_tolstring(L, -2, &kn);
          if (!json::detail::valid_utf8(std::string_view(k, kn))) {
            lua_pop(L, 2);
            o.why = "a key that is not UTF-8 text";
            return false;
          }
          o.w.key(std::string_view(k, kn));
          const bool ok = to_json_rec(L, -1, o, depth + 1);
          lua_pop(L, 1);
          if (!ok) {
            lua_pop(L, 1);
            return false;
          }
          if (o.w.str().size() > o.max_bytes) {
            lua_pop(L, 1);
            o.why = "too large";
            return false;
          }
        }
        o.w.end_object();
      } else {
        o.why = "a table that is neither a list nor has only string keys";
        return false;
      }
      o.open.erase(p);
      return true;
    }
    default:
      o.why = std::string("a ") + luaL_typename(L, index) + " value";
      return false;
  }
}

}  // namespace

bool lua_to_json(lua_State* L, int index, std::string& out, std::size_t max_bytes,
                 std::string& why) {
  json_out o;
  o.max_bytes = max_bytes;
  if (!to_json_rec(L, index, o, 0)) {
    why = o.why;
    return false;
  }
  out = o.w.take();
  if (out.size() > max_bytes) {
    why = "too large";
    return false;
  }
  return true;
}

}  // namespace mv::addon
