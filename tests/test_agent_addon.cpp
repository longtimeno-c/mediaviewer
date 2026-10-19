// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The AI agents add-on through the real loader (docs/design/29 "The add-on").
//
// Its manifest (src/addons/agent/manifest.json) declares one window and one
// key-less command. mv_agent is installed in a signed store, loaded through the
// v3 host table; its window opens from the command and says what the agent
// sees; and `serve` (mv.agent.1, what `MediaViewer --mcp` calls) answers an MCP
// handshake, tools/list and a timeline over real pipes, then returns when the
// client closes its end.
#include "catch_compat.h"

#include <mediaviewer/mediaviewer_agent.h>
#include <sodium.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "addon/host.h"
#include "addon/manifest.h"
#include "addon/screen.h"
#include "addon/slots.h"
#include "addon/store.h"
#include "core/json.h"
#include "import_fixture.h"

using namespace mv::test;
using mv::addon::slot_registry;
using mv::status;

namespace {

struct keypair {
  std::vector<std::uint8_t> pub = std::vector<std::uint8_t>(crypto_sign_PUBLICKEYBYTES);
  std::vector<std::uint8_t> sec = std::vector<std::uint8_t>(crypto_sign_SECRETKEYBYTES);
  keypair() {
    REQUIRE(sodium_init() >= 0);
    crypto_sign_keypair(pub.data(), sec.data());
  }
  [[nodiscard]] std::vector<std::uint8_t> sign(const std::string& text) const {
    std::vector<std::uint8_t> sig(crypto_sign_BYTES);
    crypto_sign_detached(sig.data(), nullptr, reinterpret_cast<const unsigned char*>(text.data()), text.size(),
                         sec.data());
    return sig;
  }
};

std::span<const std::uint8_t> bytes_of(const std::string& s) {
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

template <typename F>
bool eventually(F f) {
  for (int i = 0; i < 1000; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

std::string agent_manifest(const std::string& native, const std::vector<std::uint8_t>& lib) {
  const std::vector<std::uint8_t> raw = read_bytes(MV_AGENT_MANIFEST_JSON);
  const auto doc = mv::json::parse(std::string(raw.begin(), raw.end()));
  REQUIRE(doc);
  mv::json::writer c;
  mv::addon::screen::write_value(c, *doc->find("contributes"));
  mv::json::writer w;
  w.begin_object();
  w.key("schema").integer(1);
  w.key("id").string("agent");
  w.key("name").string("AI agents");
  w.key("version").string("1.0.0");
  w.key("platform").string(std::string(mv::addon::kPlatform));
  w.key("host_api").begin_object().key("min").integer(3).key("max").integer(3).end_object();
  w.key("installed_size").integer(static_cast<std::int64_t>(lib.size()));
  w.key("native").string(native);
  w.key("chrome").string("");
  w.key("archive").begin_object().key("path").string("agent.zip").key("sha256").string(std::string(64, 'a'))
      .key("size").integer(1).end_object();
  w.key("files").begin_array();
  w.begin_object().key("path").string(native).key("sha256").string(mv::addon::sha256_hex(lib))
      .key("size").integer(static_cast<std::int64_t>(lib.size())).key("licence").string("GPL-3.0-or-later")
      .end_object();
  w.end_array();
  w.key("description").string(*doc->str("description"));
  w.key("contributes").raw(c.take());
  w.end_object();
  return w.take();
}

// A pipe: [0] read end, [1] write end, as the serve handles take them.
struct pipe_pair {
  std::intptr_t r = -1, w = -1;
  pipe_pair() {
#if defined(_WIN32)
    HANDLE a = nullptr, b = nullptr;
    REQUIRE(::CreatePipe(&a, &b, nullptr, 1 << 20));
    r = reinterpret_cast<std::intptr_t>(a);
    w = reinterpret_cast<std::intptr_t>(b);
#else
    int fd[2];
    REQUIRE(::pipe(fd) == 0);
    r = fd[0];
    w = fd[1];
#endif
  }
  static void close_end(std::intptr_t& h) {
    if (h == -1) return;
#if defined(_WIN32)
    ::CloseHandle(reinterpret_cast<HANDLE>(h));
#else
    ::close(static_cast<int>(h));
#endif
    h = -1;
  }
  ~pipe_pair() {
    close_end(r);
    close_end(w);
  }
};

void put(std::intptr_t h, const std::string& s) {
#if defined(_WIN32)
  DWORD n = 0;
  REQUIRE(::WriteFile(reinterpret_cast<HANDLE>(h), s.data(), static_cast<DWORD>(s.size()), &n, nullptr));
#else
  REQUIRE(::write(static_cast<int>(h), s.data(), s.size()) == static_cast<ssize_t>(s.size()));
#endif
}

std::string drain(std::intptr_t h) {
  std::string out;
  char buf[65536];
  for (;;) {
#if defined(_WIN32)
    DWORD n = 0;
    if (!::ReadFile(reinterpret_cast<HANDLE>(h), buf, sizeof buf, &n, nullptr) || n == 0) break;
#else
    const ssize_t n = ::read(static_cast<int>(h), buf, sizeof buf);
    if (n <= 0) break;
#endif
    out.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

}  // namespace

TEST_CASE("agent: the shipped manifest declares one window and one key-less command", "[addon][agent]") {
  keypair k;
  const std::string m = agent_manifest("libmv_agent.dylib", {1, 2, 3});
  const auto d = mv::addon::check_manifest(bytes_of(m), k.sign(m), k.pub, MV_ADDON_HOST_API);
  REQUIRE(rejection_name(d.why) == std::string("ok"));
  REQUIRE(d.m.slots.screens.size() == 1);
  CHECK(d.m.slots.screens[0].id == "agents");
  CHECK(d.m.slots.screens[0].place == mv::addon::screen_place::window);
  REQUIRE(d.m.commands.size() == 1);
  CHECK(d.m.commands[0].id == "open");
  CHECK(d.m.commands[0].windows.empty());
  CHECK(d.m.slots.items.empty());
  CHECK(d.m.chrome.empty());
}

#if defined(MV_AGENT_MODULE_PATH)
TEST_CASE("agent: its window and an MCP session over real pipes", "[addon][agent]") {
  keypair k;
  scratch_dir s("addons_agent");
  mv::addon::store st(utf8(s.root() / "addons"), k.pub, MV_ADDON_HOST_API);
  fs::create_directories(s / "addons");
  const std::string native = utf8(fs::path(MV_AGENT_MODULE_PATH).filename());
  const std::vector<std::uint8_t> lib = read_bytes(MV_AGENT_MODULE_PATH);
  REQUIRE(lib.size() > 1000);
  const std::string m = agent_manifest(native, lib);
  auto staged = st.make_staging();
  REQUIRE(staged);
  write_bytes(fs::path(*staged) / native, lib);
  write_text(fs::path(*staged) / "manifest.json", m);
  write_bytes(fs::path(*staged) / "manifest.json.sig", k.sign(m));
  REQUIRE(st.install(*staged));

  slot_registry reg;
  mv::addon::host_services svc;
  svc.slots = &reg;
  auto loaded = mv::addon::loaded_addon::load(st, "agent", svc);
  REQUIRE(loaded);
  REQUIRE((*loaded)->slots_attached());
  CHECK(reg.addon_calls() == 0);  // nothing runs until it is asked

  // The window, from its command: what the agent sees, frames off.
  REQUIRE(reg.run_command("agent", "open", "[]") == status::ok);
  REQUIRE(eventually([&] {
    reg.drain();
    return reg.screens_json().find("What the agent sees") != std::string::npos;
  }));
  const std::string tree = reg.screens_json();
  CHECK(tree.find("not your Photos library") != std::string::npos);
  CHECK(tree.find("\"frames\"") != std::string::npos);
  CHECK(tree.find("--mcp") != std::string::npos);
  REQUIRE(reg.close_screen("agent", "agents") == status::ok);
  reg.drain();

  // mv.agent.1 over pipes, on a copy of a round-trip project.
  const auto* api = static_cast<const mv_agent_api*>((*loaded)->query(MV_AGENT_INTERFACE));
  REQUIRE(api);
  REQUIRE(api->serve);
  const fs::path timeline = s / "Interview.fcpxml";
  write_bytes(timeline, read_bytes(std::string(MV_TIMELINE_FIXTURES) + "/01-interview-simple.fcpxml"));
  pipe_pair to_server, from_server;
  const std::string tl = utf8(timeline);
  mv_agent_serve_args a{};
  a.struct_size = sizeof a;
  a.flags = MV_AGENT_SERVE_NO_HANDOFF;
  a.in_fd = to_server.r;
  a.out_fd = from_server.w;
  a.timeline_utf8 = tl.c_str();
  mv_status served = MV_ERR_INTERNAL;
  std::thread t([&] { served = api->serve((*loaded)->api().addon, &a); });
  put(to_server.w,
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\"}}\n"
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"get_timeline\",\"arguments\":{}}}\n");
  pipe_pair::close_end(to_server.w);  // the client is done: serve returns
  t.join();
  pipe_pair::close_end(from_server.w);
  CHECK(served == MV_OK);
  const std::string out = drain(from_server.r);
  std::vector<std::string> lines;
  for (std::size_t from = 0, nl; (nl = out.find('\n', from)) != std::string::npos; from = nl + 1) {
    lines.push_back(out.substr(from, nl - from));
  }
  REQUIRE(lines.size() == 3);  // one per request, none for the notification
  for (const std::string& l : lines) CHECK(mv::json::parse(l, 64).has_value());
  CHECK(lines[0].find("\"serverInfo\"") != std::string::npos);
  CHECK(lines[1].find("\"get_timeline\"") != std::string::npos);
  CHECK(lines[2].find("Interview simple") != std::string::npos);
  // The project was only read.
  CHECK(read_bytes(utf8(timeline)) == read_bytes(std::string(MV_TIMELINE_FIXTURES) + "/01-interview-simple.fcpxml"));
}
#endif
