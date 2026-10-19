// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/29 "The agent": the MCP server over a fake environment. The
// handshake and tool list a client sees; the scope (the timeline's media and
// the given folders, never `photos:`, never outside) held by the server for
// every tool; the transcript laid on the timeline with its silences; edits,
// preview and commit writing a new version beside an untouched original; the
// panel's bridge (folders, staleness, versions); frames off by default; and
// the Claude Desktop settings file edited without losing anything.
#include "catch_compat.h"

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "addons/agent/install.h"
#include "addons/agent/scope.h"
#include "addons/agent/script.h"
#include "addons/agent/server.h"
#include "addons/agent/transcript.h"
#include "core/json.h"
#include "nle/bridge_state.h"

using namespace mv;
using namespace mv::agent;

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void spit(const fs::path& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary | std::ios::trunc);
  f << s;
}

struct scratch {
  fs::path dir;
  scratch() {
    dir = fs::temp_directory_path() /
          ("mv-agent-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
  }
  ~scratch() {
    std::error_code ec;
    fs::remove_all(dir, ec);
  }
};

// Interview A001_C002 is at 16.48 s on the timeline and plays 40-90 s of its
// file. Its transcript and sound: speech with a 1.2 s pause at 50.0-51.2 s.
constexpr const char* kTalk = "/Users/editor/Footage/Interview/A001_C002.mov";
constexpr const char* kBroll = "/Users/editor/Footage/broll/Lake.mov";
constexpr const char* kOutside = "/Users/editor/Private/secret.mov";

class fake final : public environment {
 public:
  std::vector<std::string> searched_dirs;
  std::vector<std::string> handed;
  std::vector<std::string> touched;  // every path any call was given
  bool frames = false;

  bool search(std::string_view, std::string_view dir, bool, std::uint32_t, std::uint32_t, std::vector<hit>& out,
              std::string*) override {
    searched_dirs.emplace_back(dir);
    // The pack answers what is in the folder it was given; a folder search
    // can see other files beside the timeline's.
    for (const char* p : {kTalk, kBroll, kOutside, "/Users/editor/Footage/Interview/unused take.mov",
                          "photos:ABC-123"}) {
      if (std::string_view(p).rfind(dir, 0) == 0 || std::string_view(p).rfind("photos:", 0) == 0) {
        out.push_back({p, 45000, 60000, 0.3f, 2, {}});
      }
    }
    return true;
  }
  bool similar(std::string_view path, std::int64_t, std::string_view dir, bool tree, std::uint32_t kinds,
               std::uint32_t max, std::vector<hit>& out, std::string* why) override {
    touched.emplace_back(path);
    return search("", dir, tree, kinds, max, out, why);
  }
  bool transcript(std::string_view path, std::vector<segment>& out, bool& indexed, std::string*) override {
    touched.emplace_back(path);
    indexed = path == kTalk;
    if (indexed) {
      out = {{39000, 45000, "We started the climb before sunrise."},
             {45200, 50000, "The pricing is simple: one ticket for the whole valley."},
             {51200, 56000, "Um, so the lift is included."}};
    }
    return true;
  }
  bool loudness(std::string_view path, std::int64_t from_ms, int hop_ms, std::size_t count, std::vector<float>& db,
                std::string*) override {
    touched.emplace_back(path);
    db.clear();
    for (std::size_t i = 0; i < count; ++i) {
      const std::int64_t ms = from_ms + static_cast<std::int64_t>(i) * hop_ms;
      const bool quiet = (ms >= 50000 && ms < 51200) || (ms >= 45000 && ms < 45200);
      db.push_back(quiet ? -70.0f : -20.0f);
    }
    return true;
  }
  bool probe(std::string_view path, nle::media_probe& out, std::string*) override {
    touched.emplace_back(path);
    out.duration = {60, 1};
    out.has_video = out.has_audio = true;
    out.width = 1920;
    out.height = 1080;
    out.frame = {1, 25};
    return true;
  }
  bool frame_jpeg(std::string_view path, std::int64_t, std::vector<std::uint8_t>& jpeg, std::string*) override {
    touched.emplace_back(path);
    jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
    return true;
  }
  std::vector<std::vector<std::pair<std::string, bool>>> index_requests;
  int coverage_calls = 0;
  std::int64_t clock = 1760000100000;
  bool request_index(const std::vector<std::pair<std::string, bool>>& folders, std::string& state, bool& launched,
                     std::string*) override {
    index_requests.push_back(folders);
    state = "started";
    launched = false;
    return true;
  }
  bool coverage(const std::vector<std::string>& folders, std::vector<int>& states, std::string*) override {
    // Indexing the first time it is asked, done the next.
    states.assign(folders.size(), coverage_calls++ == 0 ? 1 : 2);
    return true;
  }
  void sleep_ms(int ms) override { clock += ms; }
  bool hand_to_editor(const std::string& path, std::string*) override {
    handed.push_back(path);
    return true;
  }
  bool thumbnails_allowed() override { return frames; }
  std::int64_t now_ms() override { return clock; }
  void log(const char*) override {}
};

json::value parse(const std::string& s) {
  auto v = json::parse(s, 64);
  INFO(s.substr(0, 400));
  REQUIRE(v.has_value());
  return *v;
}

std::string rpc(server& s, const std::string& method, const std::string& params = "{}", int id = 1) {
  // One message is one line on the wire.
  const std::string reply = s.handle("{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) + ",\"method\":\"" + method +
                  "\",\"params\":" + params + "}");
  CHECK(reply.find('\n') == std::string::npos);
  return reply;
}

// A tool's text answer, as a client would read it.
json::value tool(server& s, const std::string& name, const std::string& args = "{}", bool expect_error = false) {
  const json::value r = parse(rpc(s, "tools/call", "{\"name\":\"" + name + "\",\"arguments\":" + args + "}"));
  const json::value* result = r.find("result");
  REQUIRE(result);
  const json::value* content = result->find("content");
  REQUIRE(content);
  REQUIRE(!content->a.empty());
  const json::value& text = content->a.back();
  const bool is_error = result->boolean("isError").value_or(false);
  INFO(*text.str("text"));
  CHECK(is_error == expect_error);
  if (is_error) {
    json::value v;
    v.k = json::kind::string;
    v.s = *text.str("text");
    return v;
  }
  return parse(*text.str("text"));
}

// The panel's bridge holding fixture 01 with the B-roll folder given.
void make_bridge(const fs::path& dir, std::int64_t changed_ms = 0) {
  fs::create_directories(dir / "versions");
  spit(dir / "project.fcpxml", slurp(fs::path(MV_TIMELINE_FIXTURES) / "01-interview-simple.fcpxml"));
  nle::bridge_state s;
  s.project = "Interview simple";
  s.dragged_ms = 1760000000000;
  s.changed_ms = changed_ms;
  s.folders = {"/Users/editor/Footage/broll"};
  s.playhead_s = 20.5;
  s.playhead_ms_at = 1760000090000;
  spit(dir / "state.json", nle::to_json(s));
}

}  // namespace

TEST_CASE("an MCP client's handshake and tool list", "[agent][mcp]") {
  fake env;
  server s(env, {});
  const json::value init =
      parse(rpc(s, "initialize", R"({"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"t"}})"));
  const json::value* r = init.find("result");
  REQUIRE(r);
  CHECK(*r->str("protocolVersion") == "2025-03-26");
  CHECK(*r->find("serverInfo")->str("name") == "mediaviewer");
  CHECK(r->find("capabilities")->find("tools"));
  CHECK(r->str("instructions")->find("Never cut inside a word") != std::string::npos);
  // An unknown version gets the newest.
  CHECK(*parse(rpc(s, "initialize", R"({"protocolVersion":"1999-01-01"})")).find("result")->str("protocolVersion") ==
        "2025-06-18");
  CHECK(s.handle(R"({"jsonrpc":"2.0","method":"notifications/initialized"})").empty());
  CHECK(parse(rpc(s, "ping")).find("result"));
  const json::value list = parse(rpc(s, "tools/list"));
  const json::value* tools = list.find("result")->find("tools");
  REQUIRE(tools);
  std::vector<std::string> names;
  for (const json::value& t : tools->a) {
    names.push_back(*t.str("name"));
    CHECK(t.find("inputSchema"));
    CHECK(t.str("description")->size() > 40);
  }
  CHECK(names == std::vector<std::string>{"get_timeline", "transcript", "search", "find_similar", "edit",
                                          "scope_files", "analyse", "preview_changes", "commit", "reset",
                                          "script", "script_edit", "script_to_timeline", "script_export",
                                          "thumbnail"});
  CHECK(parse(rpc(s, "no/such")).find("error")->integer("code") == -32601);
  CHECK(parse(s.handle("{not json")).find("error")->integer("code") == -32700);
  CHECK(parse(s.handle("[1]")).k == json::kind::array);
}

TEST_CASE("the timeline the editor dragged, with the playhead and the scope", "[agent][mcp]") {
  scratch tmp;
  make_bridge(tmp.dir);
  fake env;
  server_options o;
  o.bridge_dir = tmp.dir.string();
  server s(env, o);
  const json::value t = tool(s, "get_timeline");
  CHECK(*t.str("project") == "Interview simple");
  CHECK(*t.str("source") == "panel");
  CHECK(*t.str("dragged") == "dragged 1 min ago");
  CHECK_FALSE(*t.boolean("stale"));
  CHECK(t.find("playhead")->d == 20.5);
  CHECK(t.find("scope")->find("folders")->a.size() == 1);
  const json::value* items = t.find("items");
  REQUIRE(items->a.size() == 3);
  CHECK(*items->a[1].str("file") == kTalk);
  CHECK(items->a[1].find("at")->d == 16.48);
  CHECK(items->a[1].find("file_in")->d == 40.0);
  // FCP changed it since: the agent is told to ask for a new drag.
  scratch tmp2;
  make_bridge(tmp2.dir, 1760000050000);
  server_options o2;
  o2.bridge_dir = tmp2.dir.string();
  server s2(env, o2);
  const json::value t2 = tool(s2, "get_timeline");
  CHECK(*t2.boolean("stale"));
  CHECK(t2.str("warning")->find("drag it") != std::string::npos);
  // Nothing dragged yet: a clear refusal.
  scratch empty;
  server_options o3;
  o3.bridge_dir = empty.dir.string();
  server s3(env, o3);
  CHECK(tool(s3, "get_timeline", "{}", true).s.find("drag a project") != std::string::npos);
}

TEST_CASE("the scope: the timeline's media and the given folders, nothing else", "[agent][scope]") {
  const scope sc({kTalk, "photos:XYZ"}, {"/Users/editor/Footage/broll", "/"});
  CHECK(sc.allows(kTalk));
  CHECK(sc.allows(kBroll));
  CHECK(sc.allows("/Users/editor/Footage/broll/sub/x.mov"));
  CHECK_FALSE(sc.allows("/Users/editor/Footage/broll-other/x.mov"));
  CHECK_FALSE(sc.allows(kOutside));
  CHECK_FALSE(sc.allows("/Users/editor/Footage/broll/../../Private/secret.mov"));
  CHECK(sc.check("photos:XYZ") == scope::verdict::photos);  // even when a timeline named one
  CHECK(sc.check("PHOTOS:abc") == scope::verdict::photos);
  CHECK(sc.check("relative/x.mov") == scope::verdict::relative);
  CHECK(sc.check("") == scope::verdict::empty);
  CHECK(sc.folders().size() == 2);
  CHECK_FALSE(sc.allows("/etc/passwd"));  // "/" is never a folder of the scope
  // A link inside a given folder that points outside is outside.
  scratch tmp;
  fs::create_directories(tmp.dir / "given");
  fs::create_directories(tmp.dir / "private");
  spit(tmp.dir / "private" / "x.mov", "x");
  std::error_code ec;
  fs::create_directory_symlink(tmp.dir / "private", tmp.dir / "given" / "link", ec);
  if (!ec) {
    const scope linked({}, {(tmp.dir / "given").string()});
    CHECK_FALSE(linked.allows((tmp.dir / "given" / "link" / "x.mov").string()));
  }
}

TEST_CASE("every tool holds the scope, whatever the model asks", "[agent][scope]") {
  scratch tmp;
  make_bridge(tmp.dir);
  fake env;
  server_options o;
  o.bridge_dir = tmp.dir.string();
  server s(env, o);
  // Search: only the timeline's files and the given folder come back.
  const json::value r = tool(s, "search", R"({"query":"pricing"})");
  for (const json::value& h : r.find("results")->a) {
    const std::string p = *h.str("path");
    CHECK((p == kTalk || p.rfind("/Users/editor/Footage/broll/", 0) == 0));
  }
  bool talk = false;
  for (const json::value& h : r.find("results")->a) talk = talk || *h.str("path") == kTalk;
  CHECK(talk);
  // The pack was asked inside the scope's folders only, never everywhere.
  for (const std::string& d : env.searched_dirs) {
    CHECK((d == "/Users/editor/Footage/broll" || d == "/Users/editor/Footage/Interview"));
  }
  CHECK(tool(s, "search", R"({"query":"in:photos beach"})", true).s.find("Photos") != std::string::npos);
  CHECK(tool(s, "transcript", "{\"path\":\"" + std::string(kOutside) + "\"}", true).s.find("outside") !=
        std::string::npos);
  CHECK(tool(s, "transcript", R"({"path":"photos:ABC"})", true).s.find("Photos") != std::string::npos);
  CHECK(tool(s, "find_similar", "{\"path\":\"" + std::string(kOutside) + "\"}", true).s.find("refused") !=
        std::string::npos);
  const std::string before = slurp(tmp.dir / "project.fcpxml");
  CHECK(tool(s, "edit",
             "{\"ops\":[{\"op\":\"connect\",\"path\":\"" + std::string(kOutside) +
                 "\",\"source_in\":0,\"duration\":2,\"at\":5}]}",
             true)
            .s.find("outside") != std::string::npos);
  CHECK(tool(s, "edit", R"({"ops":[{"op":"insert","path":"photos:ABC","source_in":0,"duration":2}]})", true)
            .s.find("Photos") != std::string::npos);
  // Nothing outside was ever read.
  for (const std::string& p : env.touched) {
    CHECK(p != kOutside);
    CHECK(p.rfind("photos:", 0) != 0);
  }
  CHECK(slurp(tmp.dir / "project.fcpxml") == before);
}

TEST_CASE("the transcript on the timeline, with the silences to cut", "[agent][transcript]") {
  scratch tmp;
  make_bridge(tmp.dir);
  fake env;
  server_options o;
  o.bridge_dir = tmp.dir.string();
  server s(env, o);
  const json::value r = tool(s, "transcript", R"({"item":1})");
  const json::value& clip = r.find("clips")->a.at(0);
  CHECK(*clip.boolean("transcribed"));
  const json::value* phrases = clip.find("phrases");
  REQUIRE(phrases->a.size() == 3);
  // File 40 s is timeline 16.48 s: the first phrase starts where the clip does.
  CHECK(phrases->a[0].find("at")->d == 16.48);
  CHECK(phrases->a[1].str("text")->find("pricing") != std::string::npos);
  // Phrase two refined to the sound: 45.2 s of the file is 21.68 s on the timeline.
  CHECK(phrases->a[1].find("at")->d == 21.68);
  const json::value* words = phrases->a[2].find("words");
  REQUIRE(words);
  CHECK(words->a[0].a[0].s == "Um,");
  // The 1.2 s pause: file 50.0-51.2 s, timeline 26.48-27.68 s, cut with 80 ms of air each side.
  const json::value* silences = clip.find("silences");
  REQUIRE(!silences->a.empty());
  bool found = false;
  for (const json::value& x : silences->a) {
    if (x.find("at")->d == 26.48 && x.find("end")->d == 27.68) {
      found = true;
      CHECK(x.find("cut_from")->d == 26.56);
      CHECK(x.find("cut_to")->d == 27.6);
    }
  }
  CHECK(found);
  // A clip that is not transcribed says so.
  const json::value all = tool(s, "transcript");
  CHECK(all.find("clips")->a.size() == 3);
  CHECK_FALSE(*all.find("clips")->a[0].boolean("transcribed"));
}

TEST_CASE("edit, preview, commit: a new version beside an untouched original", "[agent][edit]") {
  scratch tmp;
  make_bridge(tmp.dir);
  const std::string original = slurp(tmp.dir / "project.fcpxml");
  fake env;
  server_options o;
  o.bridge_dir = tmp.dir.string();
  server s(env, o);
  const json::value e = tool(s, "edit",
                             R"({"ops":[{"op":"cut","from":26.56,"to":27.6},
                                        {"op":"connect","path":"/Users/editor/Footage/broll/Lake.mov","source_in":2,"duration":3,"at":30}]})");
  CHECK(e.integer("applied") == 2);
  CHECK(std::abs(e.find("length")->d - (e.find("length_before")->d - 1.04)) < 1e-6);
  const json::value p = tool(s, "preview_changes");
  CHECK(*p.str("would_write") == "Interview simple (Claude 1)");
  CHECK(p.find("shorter_by")->d == 1.04);
  CHECK(p.find("changes")->a.size() == 2);
  // The working copy has them; the original as dragged does not.
  CHECK(tool(s, "get_timeline").find("items")->a.size() > tool(s, "get_timeline", R"({"original":true})").find("items")->a.size());
  const json::value c = tool(s, "commit");
  CHECK(*c.str("version") == "Interview simple (Claude 1)");
  CHECK(*c.boolean("handed_to_final_cut"));
  const fs::path written = *c.str("file");
  CHECK(written.parent_path() == tmp.dir / "versions");
  REQUIRE(env.handed.size() == 1);
  // The version reads back as a project of its own, beside the original.
  nle::timeline v;
  REQUIRE(v.load(slurp(written), nullptr));
  CHECK(v.project().name == "Interview simple (Claude 1)");
  CHECK(v.project().uid.empty());
  // The dragged project is exactly as it was; the bridge lists the version.
  CHECK(slurp(tmp.dir / "project.fcpxml") == original);
  const auto state = nle::bridge_state_from_json(slurp(tmp.dir / "state.json"));
  REQUIRE(state);
  REQUIRE(state->versions.size() == 1);
  CHECK(state->versions[0].name == "Interview simple (Claude 1)");
  // A second commit is the next version, never over the first.
  const json::value c2 = tool(s, "commit");
  CHECK(*c2.str("version") == "Interview simple (Claude 2)");
  CHECK(*c2.str("file") != *c.str("file"));
  CHECK(tool(s, "commit", R"({"name":"Interview simple"})", true).s.find("refused") != std::string::npos);
  // A bad batch changes nothing.
  CHECK(tool(s, "edit", R"({"ops":[{"op":"cut","from":1,"to":2},{"op":"split","item":0,"at":999}]})", true)
            .s.find("nothing in this batch") != std::string::npos);
  CHECK(tool(s, "preview_changes").find("changes")->a.size() == 2);
  // Reset takes the project as dragged.
  tool(s, "reset");
  CHECK(tool(s, "preview_changes").find("changes")->a.empty());
}

TEST_CASE("an FCPXML file instead of the panel (Windows, or no Final Cut Pro)", "[agent][edit]") {
  scratch tmp;
  const fs::path file = tmp.dir / "From Resolve.fcpxml";
  spit(file, slurp(fs::path(MV_TIMELINE_FIXTURES) / "03-connected-broll.fcpxml"));
  fake env;
  server_options o;
  o.timeline_file = file.string();
  o.folders = {"/Users/editor/Footage/broll"};
  o.handoff = false;
  server s(env, o);
  CHECK(*tool(s, "get_timeline").str("source") == "file");
  tool(s, "edit", R"({"ops":[{"op":"cut","from":10,"to":11}]})");
  const json::value c = tool(s, "commit");
  CHECK(fs::path(*c.str("file")).parent_path() == tmp.dir);
  CHECK_FALSE(*c.boolean("handed_to_final_cut"));
  CHECK(env.handed.empty());
}

TEST_CASE("the given folders' media, nothing else", "[agent][scope]") {
  scratch tmp;
  fs::create_directories(tmp.dir / "broll" / "day 2");
  fs::create_directories(tmp.dir / "broll" / ".hidden");
  fs::create_directories(tmp.dir / "broll" / "Lib.fcpbundle");
  for (const char* f : {"broll/a.mov", "broll/day 2/b.MP4", "broll/c.wav", "broll/notes.txt", "broll/.hidden/x.mov",
                        "broll/Lib.fcpbundle/y.mov", "elsewhere.mov"}) {
    spit(tmp.dir / f, "x");
  }
  const fs::path file = tmp.dir / "t.fcpxml";
  spit(file, slurp(fs::path(MV_TIMELINE_FIXTURES) / "01-interview-simple.fcpxml"));
  fake env;
  server_options o;
  o.timeline_file = file.string();
  o.folders = {(tmp.dir / "broll").string()};
  server s(env, o);
  const json::value r = tool(s, "scope_files");
  std::vector<std::string> names;
  for (const json::value& f : r.find("files")->a) names.push_back(fs::path(*f.str("path")).filename().string());
  CHECK(names == std::vector<std::string>{"a.mov", "c.wav", "b.MP4"});
  CHECK(tool(s, "scope_files", "{\"folder\":\"" + tmp.dir.string() + "\"}", true).s.find("outside") != std::string::npos);
}

TEST_CASE("analyse asks the app to index the scope, and follows it", "[agent][analyse]") {
  scratch tmp;
  make_bridge(tmp.dir);
  fake env;
  server_options o;
  o.bridge_dir = tmp.dir.string();
  server s(env, o);
  const json::value r = tool(s, "analyse", R"({"wait_s":10})");
  CHECK(*r.str("request") == "started");
  REQUIRE(env.index_requests.size() == 1);
  // The given folder and below; the timeline's own folder by itself.
  const auto& asked = env.index_requests[0];
  REQUIRE(asked.size() == 2);
  CHECK(asked[0] == std::pair<std::string, bool>{"/Users/editor/Footage/broll", true});
  CHECK(asked[1] == std::pair<std::string, bool>{"/Users/editor/Footage/Interview", false});
  CHECK(*r.find("folders")->a[0].str("state") == "indexed");
  // A2 is transcribed in the fake index, the others not: not done yet.
  CHECK_FALSE(*r.boolean("done"));
  bool talk = false;
  for (const json::value& f : r.find("timeline_files")->a) {
    if (*f.str("path") == kTalk) talk = *f.boolean("transcribed");
  }
  CHECK(talk);
  // Status only asks nothing more.
  (void)tool(s, "analyse", R"({"status_only":true,"wait_s":0})");
  CHECK(env.index_requests.size() == 1);
}

TEST_CASE("the add-on's skills are the server's prompts", "[agent][mcp]") {
  fake env;
  server_options o;
  o.skills_dir = MV_AGENT_SKILLS_DIR;
  server s(env, o);
  const json::value list = parse(rpc(s, "prompts/list"));
  std::vector<std::string> names;
  for (const json::value& p : list.find("result")->find("prompts")->a) {
    names.push_back(*p.str("name"));
    CHECK(p.str("description")->size() > 40);
  }
  CHECK(names == std::vector<std::string>{"mediaviewer-assemble-selects", "mediaviewer-cover-jump-cuts",
                                          "mediaviewer-cut-to-length", "mediaviewer-remove-retakes",
                                          "mediaviewer-tighten-interview"});
  const json::value got =
      parse(rpc(s, "prompts/get", R"({"name":"mediaviewer-tighten-interview"})"));
  const std::string text = *got.find("result")->find("messages")->a[0].find("content")->str("text");
  CHECK(text.find("cut_from") != std::string::npos);
  CHECK(text.rfind("---", 0) != 0);  // the frontmatter is not the prompt
  CHECK(parse(rpc(s, "prompts/get", R"({"name":"nope"})")).find("error"));
  CHECK(parse(rpc(s, "initialize")).find("result")->find("capabilities")->find("prompts"));
}

TEST_CASE("frames only when the editor allows them", "[agent][thumbnail]") {
  scratch tmp;
  make_bridge(tmp.dir);
  fake env;
  server_options o;
  o.bridge_dir = tmp.dir.string();
  server s(env, o);
  CHECK(tool(s, "thumbnail", R"({"at":20})", true).s.find("Settings") != std::string::npos);
  env.frames = true;
  const json::value r = parse(rpc(s, "tools/call", R"({"name":"thumbnail","arguments":{"at":20}})"));
  const json::value* content = r.find("result")->find("content");
  CHECK(*content->a[0].str("type") == "image");
  CHECK(*content->a[0].str("data") == "/9j/2Q==");
}

TEST_CASE("adding the agent to Claude Code and Claude Desktop", "[agent][install]") {
  const std::string exe = "/Applications/MediaViewer.app/Contents/MacOS/MediaViewer";
  const auto args = claude_code_add_args(exe);
  CHECK(args == std::vector<std::string>{"mcp", "add", "--scope", "user", "mediaviewer", "--", exe, "--mcp"});
  CHECK(claude_code_command_line("/A b/it's") == "claude mcp add --scope user mediaviewer -- '/A b/it'\\''s' --mcp");
  std::string out, why;
  // A settings file with other servers and keys keeps them.
  REQUIRE(desktop_config_with(R"({"mcpServers":{"other":{"command":"x"}},"globalShortcut":"Cmd+K","n":1.5})", exe,
                              out, &why));
  std::string cmd;
  CHECK(desktop_config_has(out, &cmd));
  CHECK(cmd == exe);
  CHECK(out.find("\"other\"") != std::string::npos);
  CHECK(out.find("globalShortcut") != std::string::npos);
  CHECK(out.find("1.5") != std::string::npos);
  REQUIRE(desktop_config_with("", exe, out, &why));
  CHECK(desktop_config_has(out));
  CHECK_FALSE(desktop_config_with("{oops", exe, out, &why));
  std::string removed;
  REQUIRE(desktop_config_without(R"({"mcpServers":{"mediaviewer":{"command":"y"},"other":{}}})", removed, &why));
  CHECK_FALSE(desktop_config_has(removed));
  CHECK(removed.find("other") != std::string::npos);
  CHECK(client_config_snippet(exe).find("\"--mcp\"") != std::string::npos);
}

script_op add_op(const char* path, const char* name, const char* text, std::int64_t from, std::int64_t to,
                 int index = -1) {
  script_op op;
  op.k = script_op::kind::add;
  op.path = path;
  op.name = name;
  op.text = text;
  op.from_ms = from;
  op.to_ms = to;
  op.index = index;
  return op;
}

script_op id_op(script_op::kind k, const char* id, int index = -1) {
  script_op op;
  op.k = k;
  op.id = id;
  op.index = index;
  return op;
}

TEST_CASE("the script: lines in play order, all or none, the same file either side reads", "[agent][script]") {
  script sc;
  std::vector<std::string> added;
  REQUIRE(apply(sc,
                {add_op(kTalk, "A001", "One ticket.", 45200, 50000),
                 add_op(kTalk, "A001", "Before sunrise.", 39000, 45000, 0)},
                nullptr, &added));
  REQUIRE(added == std::vector<std::string>{"l1", "l2"});
  REQUIRE(sc.lines.size() == 2);
  CHECK(sc.lines[0].id == "l2");  // inserted at 0
  CHECK(sc.length_ms() == 4800 + 6000);
  std::string why;
  // A batch that fails leaves the list as it was.
  CHECK_FALSE(apply(sc, {id_op(script_op::kind::remove, "l1"), id_op(script_op::kind::remove, "l9")}, &why));
  CHECK(why.find("no line l9") != std::string::npos);
  CHECK(sc.lines.size() == 2);
  REQUIRE(apply(sc, {id_op(script_op::kind::move, "l2")}, nullptr));
  CHECK(sc.lines[1].id == "l2");
  CHECK_FALSE(apply(sc, {add_op(kTalk, "", "", 5000, 5000)}, &why));  // empty range

  // Round trip, and ids never reused after a remove.
  script back;
  REQUIRE(from_json(to_json(sc), back, &why));
  CHECK(back.lines.size() == 2);
  CHECK(back.lines[0].text == "One ticket.");
  REQUIRE(apply(back, {id_op(script_op::kind::remove, "l2"), add_op(kBroll, "", "", 0, 1000)},
                nullptr, &added));
  CHECK(added == std::vector<std::string>{"l3"});
  CHECK_FALSE(from_json(R"({"format":"other"})", back, &why));
  CHECK_FALSE(from_json(R"({"format":"mediaviewer.script.1","lines":[{"id":"l1","path":"a","from_ms":5,"to_ms":2}]})",
                        back, &why));

  // The tool's ops: seconds, and every mistake named.
  std::vector<script_op> ops;
  REQUIRE(parse_script_ops(parse(R"([{"op":"add","path":"/a.mov","from":1.5,"to":2.25},{"op":"move","id":"l1","at":0},{"op":"clear"}])"),
                           ops, &why));
  CHECK(ops[0].from_ms == 1500);
  CHECK(ops[0].to_ms == 2250);
  CHECK(ops[1].index == 0);
  CHECK_FALSE(parse_script_ops(parse(R"([{"op":"cut"}])"), ops, &why));
  CHECK(why.find("unknown op") != std::string::npos);
  CHECK_FALSE(parse_script_ops(parse(R"([{"op":"add","path":"/a.mov"}])"), ops, &why));

  // Beside a --timeline file, else in the panel's folder.
  CHECK(script_path("/b", "/x/Cut.fcpxml") == "/x/Cut.fcpxml.script.json");
  CHECK(fs::path(script_path("/b", "")) == fs::path("/b") / "script.json");
}

TEST_CASE("the script's documents: TXT, CSV, DOCX, PDF", "[agent][script]") {
  script sc;
  sc.project = "Interview simple";
  REQUIRE(apply(sc,
                {add_op(kTalk, "A001_C002", "The pricing is simple: one ticket.", 45200, 50000),
                 add_op(kTalk, "A001_C002", "=HYPERLINK(\"x\") caf\xC3\xA9 \xE2\x80\x9Cquoted\xE2\x80\x9D", 39000, 45000)},
                nullptr));
  const std::string txt = export_txt(sc);
  CHECK(txt.find("Interview simple") == 0);
  CHECK(txt.find("[A001_C002  0:45.20-0:50.00]\nThe pricing is simple: one ticket.") != std::string::npos);
  const std::string csv = export_csv(sc);
  CHECK(csv.find("line,clip,file,in_s,out_s,duration_s,text\r\n") == 0);
  CHECK(csv.find("1,\"A001_C002\",\"" + std::string(kTalk) + "\",45.200,50.000,4.800,") != std::string::npos);
  CHECK(csv.find("\"'=HYPERLINK(\"\"x\"\")") != std::string::npos);  // text, never a formula

  const std::vector<std::uint8_t> docx = export_docx(sc);
  const std::string d(docx.begin(), docx.end());
  CHECK(d.rfind("PK\x03\x04", 0) == 0);
  CHECK(d.find("[Content_Types].xml") != std::string::npos);
  CHECK(d.find("word/document.xml") != std::string::npos);
  CHECK(d.find("&quot;x&quot;") != std::string::npos);
  CHECK(d.find("PK\x05\x06") != std::string::npos);
  CHECK(export_docx(sc) == docx);  // the same script makes the same file

  const std::vector<std::uint8_t> pdf = export_pdf(sc);
  const std::string f(pdf.begin(), pdf.end());
  CHECK(f.rfind("%PDF-1.4", 0) == 0);
  CHECK(f.find("%%EOF") != std::string::npos);
  // Every xref offset points at its object.
  const std::size_t xref = f.find("\nxref\n") + 1;
  const std::size_t start = std::stoul(f.substr(f.find("startxref\n") + 10));
  CHECK(start == xref);
  std::size_t at = f.find("0000000000 65535 f \n", xref) + 20;
  for (int n = 1; f.compare(at, 1, "t") != 0; ++n, at += 20) {
    const std::size_t off = std::stoul(f.substr(at, 10));
    CHECK(f.compare(off, std::to_string(n).size() + 6, std::to_string(n) + " 0 obj") == 0);
  }
  CHECK(f.find("caf\xE9") != std::string::npos);    // Latin-1 in WinAnsi
  // A long script runs onto more pages, each line still inside the page.
  script big;
  std::vector<script_op> many;
  for (int i = 0; i < 120; ++i) many.push_back(add_op(kTalk, "A001", "A line long enough to wrap once it passes the margin of an A4 page, which is about eighty-eight characters.", i * 1000, i * 1000 + 900));
  REQUIRE(apply(big, many, nullptr));
  const std::vector<std::uint8_t> long_pdf = export_pdf(big);
  const std::string lp(long_pdf.begin(), long_pdf.end());
  const std::size_t count_at = lp.find("/Count ");
  REQUIRE(count_at != std::string::npos);
  CHECK(std::stoi(lp.substr(count_at + 7)) >= 5);
  // MV_SCRIPT_SAMPLES=<dir> keeps the four documents, to open in Word, Preview, a spreadsheet.
  if (const char* keep = std::getenv("MV_SCRIPT_SAMPLES")) {
    spit(fs::path(keep) / "script.txt", txt);
    spit(fs::path(keep) / "script.csv", csv);
    spit(fs::path(keep) / "script.docx", d);
    spit(fs::path(keep) / "script.pdf", f);
    spit(fs::path(keep) / "long.pdf", lp);
  }
  CHECK(f.find("\x93quoted\x94") != std::string::npos);
}

TEST_CASE("the script tools: the agent builds the cut list, the timeline follows it", "[agent][script]") {
  scratch tmp;
  make_bridge(tmp.dir);
  const std::string original = slurp(tmp.dir / "project.fcpxml");
  fake env;
  server_options o;
  o.bridge_dir = tmp.dir.string();
  server s(env, o);
  CHECK(tool(s, "script").find("lines")->a.empty());
  CHECK(tool(s, "script_to_timeline", "{}", true).s.find("empty") != std::string::npos);
  // Text comes from what the index heard in the range.
  const json::value e = tool(s, "script_edit",
                             R"({"ops":[{"op":"add","path":"/Users/editor/Footage/Interview/A001_C002.mov","from":45.2,"to":50},
                                        {"op":"add","path":"/Users/editor/Footage/Interview/A001_C002.mov","from":39,"to":45,"at":0}]})");
  CHECK(e.find("added")->a.size() == 2);
  CHECK(*e.find("lines")->a[1].str("text") == "The pricing is simple: one ticket for the whole valley.");
  CHECK(e.find("length")->d == 10.8);
  // The window reads the same file.
  const script on_disk = load_script((tmp.dir / "script.json").string());
  REQUIRE(on_disk.lines.size() == 2);
  CHECK(on_disk.project == "Interview simple");
  // Outside the scope: refused, nothing changed.
  CHECK(tool(s, "script_edit", R"({"ops":[{"op":"add","path":"/Users/editor/Private/secret.mov","from":0,"to":1}]})", true)
            .s.find("nothing was changed") != std::string::npos);
  CHECK(tool(s, "script_edit", R"({"ops":[{"op":"add","path":"photos:ABC-123","from":0,"to":1}]})", true).k ==
        json::kind::string);
  CHECK(load_script((tmp.dir / "script.json").string()).lines.size() == 2);

  // The storyline becomes the script; commit writes a version, the original stays.
  const json::value t = tool(s, "script_to_timeline");
  CHECK(std::abs(t.find("length")->d - 10.8) < 0.05);
  const json::value c = tool(s, "commit");
  nle::timeline v;
  REQUIRE(v.load(slurp(fs::path(*c.str("file"))), nullptr));
  std::vector<const nle::item*> story;
  for (const nle::item& it : v.items()) {
    if (it.parent == -1 && it.in_storyline && it.storyline == -1) story.push_back(&it);
  }
  REQUIRE(story.size() == 2);
  CHECK(nle::to_ms(story[0]->source_in) == 39000);
  CHECK(nle::to_ms(story[1]->source_in) == 45200);
  CHECK(slurp(tmp.dir / "project.fcpxml") == original);

  // Documents beside it, never over one.
  const json::value x1 = tool(s, "script_export", R"({"format":"docx"})");
  const json::value x2 = tool(s, "script_export", R"({"format":"docx"})");
  CHECK(fs::path(*x1.str("file")).filename() == "Interview simple script.docx");
  CHECK(*x1.str("file") != *x2.str("file"));
  CHECK(fs::exists(*x2.str("file")));
  CHECK(tool(s, "script_export", R"({"format":"rtf"})", true).k == json::kind::string);
}
