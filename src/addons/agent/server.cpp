// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "addons/agent/server.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <system_error>

#include "addons/agent/transcript.h"

namespace mv::agent {
namespace {

using nle::rational;

constexpr std::size_t kMaxText = 400 * 1024;  // one tool answer, before it says "narrow it"
constexpr const char* kMcpVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

double secs(rational t) { return nle::seconds(t); }

std::string read_file(const std::string& path, bool* ok) {
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  if (!f) {
    *ok = false;
    return {};
  }
  *ok = true;
  return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::int64_t stamp_of(const std::string& path) {
  std::error_code ec;
  const auto t = std::filesystem::last_write_time(std::filesystem::path(path), ec);
  if (ec) return 0;
  const auto size = std::filesystem::file_size(std::filesystem::path(path), ec);
  return static_cast<std::int64_t>(t.time_since_epoch().count()) ^ (ec ? 0 : static_cast<std::int64_t>(size) << 1);
}

bool write_atomically(const std::string& path, const std::string& text) {
  const std::string tmp = path + ".mvtmp";
  {
    std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) return false;
  }
  std::error_code ec;
  std::filesystem::rename(std::filesystem::path(tmp), std::filesystem::path(path), ec);
  if (ec) std::filesystem::remove(std::filesystem::path(tmp), ec);
  return !ec;
}

// A new file next to the others, never over one.
std::string unique_path(const std::string& dir, const std::string& file) {
  std::filesystem::path p = std::filesystem::path(dir) / file;
  std::error_code ec;
  if (!std::filesystem::exists(p, ec)) return p.string();
  const std::string stem = p.stem().string(), ext = p.extension().string();
  for (int n = 2; n < 1000; ++n) {
    std::filesystem::path q = std::filesystem::path(dir) / (stem + " " + std::to_string(n) + ext);
    if (!std::filesystem::exists(q, ec)) return q.string();
  }
  return (std::filesystem::path(dir) / (stem + " new" + ext)).string();
}

std::optional<rational> time_arg(const json::value& args, std::string_view key) {
  const json::value* v = args.find(key);
  if (!v) return std::nullopt;
  if (v->k == json::kind::string) return nle::parse_clock(v->s);
  if (v->k == json::kind::number) {
    if (v->is_integer) return rational{v->i, 1};
    return nle::reduce({static_cast<std::int64_t>(std::llround(v->d * 1000.0)), 1000});
  }
  return std::nullopt;
}

int int_arg(const json::value& args, std::string_view key, int fallback) {
  const auto v = args.integer(key);
  return v ? static_cast<int>(*v) : fallback;
}

std::uint32_t kinds_of(const json::value& args) {
  const json::value* k = args.find("kinds");
  if (!k || k->k != json::kind::array) return 2u | 1u | 8u;  // video, photos, audio
  std::uint32_t out = 0;
  for (const json::value& e : k->a) {
    if (e.k != json::kind::string) continue;
    if (e.s == "video") out |= 2u;
    if (e.s == "photo" || e.s == "photos") out |= 1u;
    if (e.s == "audio") out |= 8u;
  }
  return out ? out : (2u | 1u | 8u);
}

}  // namespace

// ---- JSON helpers -----------------------------------------------------------------------

namespace {

void write_value(json::writer& w, const json::value& v) {
  switch (v.k) {
    case json::kind::null: w.null(); break;
    case json::kind::boolean: w.boolean(v.b); break;
    case json::kind::number:
      if (v.is_integer) {
        w.integer(v.i);
      } else {
        char buf[40];
        const auto r = std::to_chars(buf, buf + sizeof buf, v.d);
        w.raw(std::string_view(buf, static_cast<std::size_t>(r.ptr - buf)));
      }
      break;
    case json::kind::string: w.string(v.s); break;
    case json::kind::array:
      w.begin_array();
      for (const json::value& e : v.a) write_value(w, e);
      w.end_array();
      break;
    case json::kind::object:
      w.begin_object();
      for (const auto& [k, e] : v.o) {
        w.key(k);
        write_value(w, e);
      }
      w.end_object();
      break;
  }
}

}  // namespace

std::string to_text(const json::value& v) {
  json::writer w;
  write_value(w, v);
  return w.take();
}

std::string base64(const std::vector<std::uint8_t>& bytes) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const std::uint32_t n = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += kAlphabet[(n >> 6) & 63];
    out += kAlphabet[n & 63];
  }
  if (i < bytes.size()) {
    std::uint32_t n = std::uint32_t{bytes[i]} << 16;
    if (i + 1 < bytes.size()) n |= std::uint32_t{bytes[i + 1]} << 8;
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += i + 1 < bytes.size() ? kAlphabet[(n >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// ---- the server ---------------------------------------------------------------------------

server::server(environment& env, server_options options) : env_(env), o_(std::move(options)) {}

const char* server::instructions() {
  return "MediaViewer's timeline agent edits the Final Cut Pro project the editor dragged onto the MediaViewer "
         "panel. Work only with that timeline's own media and the folders the editor gave; the server refuses "
         "anything else, including the Photos library.\n"
         "How to edit well:\n"
         "1. Read first: get_timeline, then transcript. Understand the story before cutting. Name only what "
         "get_timeline lists; never guess a clip.\n"
         "2. Propose before you change: a short plan (what goes, with times and why; every group of retakes) and "
         "wait for the editor's yes unless they asked you to just do it. A tweak to the plan is not a yes.\n"
         "3. Cut on boundaries the server gives: a silence's cut_from/cut_to, or the edges of phrases and words. "
         "Never cut inside a word. Silences of 0.4 s or more are the safest cuts. Word times inside a phrase are "
         "estimates: prefer silences and phrase edges; the server keeps about 80 ms of air at each side.\n"
         "   Fillers by default: um, uh, er, erm, ah, hmm only; 'like', 'so', 'you know' only when asked. Never cut "
         "a laugh, a reaction or the beat before a punchline. End every cut version on a complete sentence.\n"
         "4. Keep what the editor built: J and L cuts, connected B-roll and music, titles, effects and colour are "
         "carried by the server; do not remove them unless asked. Respect roles (dialogue, music, effects).\n"
         "5. Compound, multicam and sync clips are units: trim or move them, never open them. Items marked "
         "not editable are carried as they are.\n"
         "6. Never lose a take the brief needs: when unsure, keep it and say so. Do not cut the only copy of a "
         "line the editor asked to keep.\n"
         "7. Send a batch in the timeline's own times (all times in one edit call refer to the timeline before "
         "that call), check preview_changes and read transcript of the edited timeline around each cut (no "
         "word cut in half or repeated across a join; at most three fix passes), then commit. Each commit is a new project beside the original "
         "(\"<Project> (Claude N)\"); the original and every media file stay as they were.\n"
         "8. If get_timeline says the project changed in Final Cut Pro since it was dragged, ask the editor to "
         "drag it onto the panel again before editing.\n"
         "Times are seconds on the timeline (0 is the start of the project) unless a field says file time. "
         "The server's prompts (prompts/list) are step-by-step skills for common edits: tighten an interview, "
         "remove retakes, cut to a length, cover jump cuts with B-roll, assemble selects from a script.";
}

std::string server::source_path() const {
  if (!o_.timeline_file.empty()) return o_.timeline_file;
  if (o_.bridge_dir.empty()) return {};
  return (std::filesystem::path(o_.bridge_dir) / std::string(nle::kBridgeProject)).string();
}

nle::bridge_state server::bridge() const {
  if (o_.bridge_dir.empty()) return {};
  bool ok = false;
  const std::string text = read_file((std::filesystem::path(o_.bridge_dir) / std::string(nle::kBridgeState)).string(), &ok);
  if (!ok) return {};
  return nle::bridge_state_from_json(text).value_or(nle::bridge_state{});
}

scope server::current_scope() const {
  std::vector<std::string> folders = o_.folders;
  for (const std::string& f : bridge().folders) folders.push_back(f);
  std::vector<std::string> media;
  if (original_) media = original_->media_paths();
  if (working_) {
    // What the working copy added came from the folders, so it is in them already.
    for (const std::string& p : working_->media_paths()) media.push_back(p);
  }
  return scope(std::move(media), std::move(folders));
}

bool server::load(std::string* why) {
  const std::string path = source_path();
  if (path.empty()) {
    *why = "no timeline: drag a project from the Final Cut Pro browser onto the MediaViewer panel "
           "(Extensions > MediaViewer Search), or start the server with --timeline FILE.fcpxml";
    return false;
  }
  const std::int64_t stamp = stamp_of(path);
  if (original_ && path == loaded_from_ && stamp == loaded_stamp_) return true;
  if (original_ && !changes_.empty()) {
    newer_waiting_ = true;  // keep the edits; say a newer drag is waiting
    return true;
  }
  bool ok = false;
  std::string text = read_file(path, &ok);
  if (!ok) {
    *why = o_.timeline_file.empty()
               ? "no timeline yet: drag a project from the Final Cut Pro browser onto the MediaViewer panel"
               : "the timeline file cannot be read";
    return false;
  }
  auto t = std::make_unique<nle::timeline>();
  std::string error;
  const std::string wanted = o_.timeline_file.empty() ? bridge().project : std::string();
  if (!t->load(text, &error, wanted) && !(wanted.size() && t->load(text, &error))) {
    *why = "the timeline does not read: " + error;
    return false;
  }
  working_ = std::make_unique<nle::timeline>(*t);
  original_ = std::move(t);
  changes_.clear();
  loaded_from_ = path;
  loaded_stamp_ = stamp;
  newer_waiting_ = false;
  env_.log("timeline loaded");
  return true;
}

// ---- JSON-RPC -----------------------------------------------------------------------------

std::string server::respond(const json::value& id, const std::string& result_json) {
  json::writer w;
  w.begin_object().key("jsonrpc").string("2.0").key("id");
  write_value(w, id);
  w.key("result").raw(result_json).end_object();
  return w.take();
}

std::string server::fail(const json::value* id, int code, std::string_view message) {
  json::writer w;
  w.begin_object().key("jsonrpc").string("2.0").key("id");
  if (id) {
    write_value(w, *id);
  } else {
    w.null();
  }
  w.key("error").begin_object().key("code").integer(code).key("message").string(message).end_object().end_object();
  return w.take();
}

std::string server::handle(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
  if (line.empty()) return {};
  const auto msg = json::parse(line, 64);
  if (!msg) return fail(nullptr, -32700, "parse error");
  if (msg->k == json::kind::array) {
    // JSON-RPC batches (MCP before 2025-06-18).
    std::string out = "[";
    bool any = false;
    for (const json::value& m : msg->a) {
      const std::string r = handle(to_text(m));
      if (r.empty()) continue;
      if (any) out += ',';
      out += r;
      any = true;
    }
    return any ? out + "]" : std::string();
  }
  if (msg->k != json::kind::object) return fail(nullptr, -32600, "invalid request");
  const json::value* id = msg->find("id");
  const std::string* method = msg->str("method");
  if (!method) {
    // A response to something we never asked: ignore.
    return {};
  }
  static const json::value kEmpty = [] {
    json::value v;
    v.k = json::kind::object;
    return v;
  }();
  const json::value* params = msg->find("params");
  if (params && params->k != json::kind::object) {
    return id ? fail(id, -32602, "params must be an object") : std::string();
  }
  const json::value& p = params ? *params : kEmpty;
  if (!id) return {};  // notifications: initialized, cancelled, ...
  if (*method == "initialize") {
    const std::string* asked = p.str("protocolVersion");
    protocol_ = kMcpVersions[0];
    if (asked) {
      for (const char* v : kMcpVersions) {
        if (*asked == v) protocol_ = v;
      }
    }
    initialized_ = true;
    json::writer w;
    w.begin_object();
    w.key("protocolVersion").string(protocol_);
    w.key("capabilities").begin_object().key("tools").begin_object().key("listChanged").boolean(false).end_object()
        .key("prompts").begin_object().key("listChanged").boolean(false).end_object().end_object();
    w.key("serverInfo").begin_object().key("name").string("mediaviewer").key("title").string("MediaViewer timeline")
        .key("version").string(o_.version).end_object();
    w.key("instructions").string(instructions());
    w.end_object();
    env_.log("initialize");
    return respond(*id, w.take());
  }
  if (*method == "ping") return respond(*id, "{}");
  if (*method == "tools/list") return respond(*id, tools_list());
  if (*method == "tools/call") {
    const std::string* name = p.str("name");
    if (!name) return fail(id, -32602, "tools/call needs a name");
    const json::value* a = p.find("arguments");
    const outcome r = dispatch(*name, a && a->k == json::kind::object ? *a : kEmpty);
    json::writer w;
    w.begin_object().key("content").begin_array();
    if (!r.image_base64.empty()) {
      w.begin_object().key("type").string("image").key("data").string(r.image_base64).key("mimeType")
          .string("image/jpeg").end_object();
    }
    w.begin_object().key("type").string("text").key("text").string(r.text).end_object();
    w.end_array().key("isError").boolean(r.error).end_object();
    return respond(*id, w.take());
  }
  if (*method == "resources/list") return respond(*id, R"({"resources":[]})");
  if (*method == "prompts/list") return respond(*id, prompts_list());
  if (*method == "prompts/get") {
    std::string error;
    const std::string r = prompt_get(p, &error);
    return r.empty() ? fail(id, -32602, error) : respond(*id, r);
  }
  return fail(id, -32601, "method not found");
}

std::string server::call(std::string_view tool, std::string_view arguments_json, bool* is_error) {
  auto a = json::parse(arguments_json.empty() ? std::string_view("{}") : arguments_json, 32);
  json::value empty;
  empty.k = json::kind::object;
  const outcome r = dispatch(tool, a && a->k == json::kind::object ? *a : empty);
  if (is_error) *is_error = r.error;
  return r.text;
}

std::string server::tools_list() const {
  // Schemas are JSON Schema; descriptions are what the model reads. One
  // message is one line: the layout's newlines go.
  std::string s = R"JSON({"tools":[
{"name":"get_timeline","title":"Read the timeline","description":"The Final Cut Pro project the editor dragged onto the MediaViewer panel, as the working copy (with your uncommitted edits) or as dragged (original:true). Items in timeline order with ids, kind, times in seconds on the timeline (at, end), lane (0 storyline, >0 above, <0 below), the file each plays and its file range, role, title text, markers, and flags (effects, retimed, split audio, opaque units, not editable with why). Also the playhead, the scope folders, and whether Final Cut Pro changed the project after it was dragged (stale).","inputSchema":{"type":"object","properties":{"original":{"type":"boolean","description":"the project as dragged, without your edits"}}},"annotations":{"readOnlyHint":true,"openWorldHint":false}},
{"name":"transcript","title":"Transcript and silences","description":"What is said in the timeline's clips, laid on the timeline: phrases (refined against the sound), words with estimated times, and silences measured in the sound with the range to cut (cut_from..cut_to keeps a little air). With no arguments: every storyline clip in order. Narrow with item (an id) or from/to (timeline seconds); path gives a scope file's transcript in file time. Clips not transcribed yet say so: the editor can index them in MediaViewer.","inputSchema":{"type":"object","properties":{"item":{"type":"integer"},"path":{"type":"string"},"from":{"type":["number","string"]},"to":{"type":["number","string"]},"words":{"type":"boolean","default":true},"min_silence_ms":{"type":"integer","default":400,"minimum":100,"maximum":10000}}},"annotations":{"readOnlyHint":true,"openWorldHint":false}},
{"name":"search","title":"Search the scope","description":"Local search (what is seen, said, heard; people as @Name; \"quoted words\" must be said) over the timeline's own media and the folders the editor gave, nothing else. Results: file, the moment (file seconds), score, and which timeline items already use it.","inputSchema":{"type":"object","properties":{"query":{"type":"string"},"kinds":{"type":"array","items":{"enum":["video","photo","audio"]}},"max":{"type":"integer","default":20,"minimum":1,"maximum":100}},"required":["query"]},"annotations":{"readOnlyHint":true,"openWorldHint":false}},
{"name":"find_similar","title":"Find alternatives","description":"Other takes or similar shots to a timeline item (item, optionally at a timeline time inside it) or to a moment of a scope file (path, at_file), inside the scope only.","inputSchema":{"type":"object","properties":{"item":{"type":"integer"},"at":{"type":["number","string"]},"path":{"type":"string"},"at_file":{"type":["number","string"]},"max":{"type":"integer","default":20,"minimum":1,"maximum":100}}},"annotations":{"readOnlyHint":true,"openWorldHint":false}},
{"name":"edit","title":"Edit the working copy","description":"Applies a batch of operations to the working copy, all or nothing. Item ids and times refer to the timeline before this call (later times are mapped through earlier cuts). Ops: {op:cut,from,to[,ripple:false]} removes a range (ripple closes the gap; connected B-roll is cut with it, music plays through); {op:trim,item,from,to} or {op:trim,item,head,tail} (seconds off each end, negative lengthens into the media); {op:split,item,at}; {op:remove,item[,ripple:false]}; {op:move,item,before} (reorder; no time ops in the same batch); {op:source,item,source_in[,path]} another take or range; {op:insert,path,source_in,duration[,before]}; {op:connect,path,source_in,duration,at[,lane:1][,with_audio:false]} B-roll; {op:marker,at,text[,todo]}; {op:unmarker,item,marker}; {op:title,item,text}; {op:volume,item,db}; {op:enable,item,on}. Paths must be in the scope. Times snap to frames. Every change leaves a 'Claude:' marker. Returns the change list; item ids change after an edit: call get_timeline before addressing items again.","inputSchema":{"type":"object","properties":{"ops":{"type":"array","items":{"type":"object"},"minItems":1,"maxItems":2000}},"required":["ops"]},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
{"name":"scope_files","title":"List the given folders","description":"The media files in the folders the editor gave (B-roll, other takes), with their kind, size and, for a short list, length; and which already play on the timeline. Hidden files and packages (libraries) are skipped. Only these files and the timeline's own can be used.","inputSchema":{"type":"object","properties":{"folder":{"type":"string","description":"one of the given folders (default: all of them)"},"max":{"type":"integer","default":200,"minimum":1,"maximum":2000}}},"annotations":{"readOnlyHint":true,"openWorldHint":false}},
{"name":"analyse","title":"Index the timeline's media","description":"Asks MediaViewer (started in the background if it is not running) to index the timeline's own media and the given folders for Local search: what is seen, said (transcripts) and heard. Then reports progress: each folder not indexed, indexing or indexed, and which timeline files have a transcript. Call again with status_only to follow it. Indexing runs in MediaViewer, at its own pace, while the editor works.","inputSchema":{"type":"object","properties":{"status_only":{"type":"boolean","description":"only report progress, ask for nothing"},"wait_s":{"type":"integer","default":10,"minimum":0,"maximum":120,"description":"how long to wait for progress before answering"}}},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":true,"openWorldHint":false}},
{"name":"preview_changes","title":"Preview the changes","description":"What the working copy changed against the dragged project: each change with where it is, the runtime before and after, and the name commit would write.","inputSchema":{"type":"object","properties":{}},"annotations":{"readOnlyHint":true,"openWorldHint":false}},
{"name":"commit","title":"Send to Final Cut Pro","description":"Writes the working copy as a new project beside the original, \"<Project> (Claude N)\" in the same event, and asks Final Cut Pro to import it. The original project and every media file stay as they are. Edits stay in the working copy, so a later commit is the next version.","inputSchema":{"type":"object","properties":{"name":{"type":"string","description":"a name for the new project (default: <Project> (Claude N))"}}},"annotations":{"readOnlyHint":false,"destructiveHint":false,"idempotentHint":false,"openWorldHint":false}},
{"name":"reset","title":"Discard the edits","description":"Throws the working copy's edits away and takes the project as last dragged.","inputSchema":{"type":"object","properties":{}},"annotations":{"readOnlyHint":false,"destructiveHint":true,"openWorldHint":false}},
{"name":"thumbnail","title":"Look at a frame","description":"A frame of the timeline (at, timeline seconds; the topmost video there) or of an item, as a JPEG. Off unless the editor turned on 'Let the agent see frames' in Settings > Add-ons > AI agents.","inputSchema":{"type":"object","properties":{"at":{"type":["number","string"]},"item":{"type":"integer"}}},"annotations":{"readOnlyHint":true,"openWorldHint":false}}
]})JSON";
  s.erase(std::remove(s.begin(), s.end(), '\n'), s.end());
  return s;
}

// ---- skills as prompts ----------------------------------------------------------------------

namespace {

struct skill {
  std::string name, description, body;
};

// SKILL.md: "---\nname: x\ndescription: y\n---\nbody".
std::vector<skill> read_skills(const std::string& dir) {
  std::vector<skill> out;
  if (dir.empty()) return out;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(dir), ec)) {
    if (!e.is_directory(ec)) continue;
    bool ok = false;
    const std::string text = read_file((e.path() / "SKILL.md").string(), &ok);
    if (!ok || text.rfind("---", 0) != 0) continue;
    const std::size_t end = text.find("\n---", 3);
    if (end == std::string::npos) continue;
    skill s;
    std::size_t from = text.find('\n') + 1;
    while (from < end) {
      const std::size_t nl = text.find('\n', from);
      const std::string line = text.substr(from, (nl == std::string::npos ? end : std::min(nl, end)) - from);
      if (line.rfind("name: ", 0) == 0) s.name = line.substr(6);
      if (line.rfind("description: ", 0) == 0) s.description = line.substr(13);
      if (nl == std::string::npos) break;
      from = nl + 1;
    }
    const std::size_t body = text.find('\n', end + 4);
    s.body = body == std::string::npos ? std::string() : text.substr(body + 1);
    if (!s.name.empty() && !s.body.empty()) out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(), [](const skill& a, const skill& b) { return a.name < b.name; });
  return out;
}

}  // namespace

std::string server::prompts_list() const {
  json::writer w;
  w.begin_object().key("prompts").begin_array();
  for (const skill& s : read_skills(o_.skills_dir)) {
    w.begin_object().key("name").string(s.name).key("description").string(s.description).end_object();
  }
  w.end_array().end_object();
  return w.take();
}

std::string server::prompt_get(const json::value& params, std::string* error) const {
  const std::string* name = params.str("name");
  for (const skill& s : read_skills(o_.skills_dir)) {
    if (name && s.name == *name) {
      json::writer w;
      w.begin_object().key("description").string(s.description).key("messages").begin_array().begin_object()
          .key("role").string("user").key("content").begin_object().key("type").string("text").key("text")
          .string(s.body).end_object().end_object().end_array().end_object();
      return w.take();
    }
  }
  if (error) *error = "no such prompt";
  return {};
}

server::outcome server::dispatch(std::string_view tool, const json::value& args) {
  try {
    if (tool == "get_timeline") return get_timeline(args);
    if (tool == "transcript") return transcript(args);
    if (tool == "search") return search(args, false);
    if (tool == "find_similar") return search(args, true);
    if (tool == "edit") return edit(args);
    if (tool == "scope_files") return scope_files(args);
    if (tool == "analyse") return analyse(args);
    if (tool == "preview_changes") return preview();
    if (tool == "commit") return commit(args);
    if (tool == "reset") return reset();
    if (tool == "thumbnail") return thumbnail(args);
  } catch (const std::bad_alloc&) {
    return {"out of memory", true, {}};
  } catch (...) {
    return {"internal error", true, {}};
  }
  return {"unknown tool", true, {}};
}

// ---- get_timeline -------------------------------------------------------------------------

namespace {

void write_item(json::writer& w, const nle::timeline& t, std::size_t i) {
  const nle::item& it = t.items()[i];
  w.begin_object();
  w.key("id").integer(static_cast<std::int64_t>(i));
  w.key("kind").string(nle::kind_name(it.kind));
  if (!it.name.empty()) w.key("name").string(it.name);
  w.key("at").number(secs(it.at));
  w.key("end").number(secs(nle::add(it.at, it.duration).value_or(it.at)));
  if (it.lane != 0) w.key("lane").integer(it.lane);
  if (it.parent >= 0) w.key("on").integer(it.parent);
  if (it.storyline >= 0) w.key("storyline").integer(it.storyline);
  if (!it.source.empty()) {
    w.key("file").string(it.source);
    w.key("file_in").number(secs(it.source_in));
    w.key("file_out").number(secs(nle::add(it.source_in, it.duration).value_or(it.source_in)));
  }
  if (!it.role.empty()) w.key("role").string(it.role);
  if (!it.enabled) w.key("enabled").boolean(false);
  if (!it.text.empty()) w.key("text").string(it.text);
  if (it.has_effects) w.key("effects").boolean(true);
  if (it.retimed) w.key("retimed").boolean(true);
  if (it.split_audio) w.key("split_audio").boolean(true);
  if (it.opaque) w.key("unit").boolean(true);
  if (!it.editable) w.key("not_editable").string(it.not_editable);
  if (!it.markers.empty()) {
    w.key("markers").begin_array();
    for (const nle::marker& m : it.markers) {
      w.begin_object().key("at").number(secs(m.at)).key("text").string(m.value);
      if (m.kind == "chapter-marker") w.key("chapter").boolean(true);
      if (m.todo) w.key(m.completed ? "done" : "todo").boolean(true);
      w.end_object();
    }
    w.end_array();
  }
  w.end_object();
}

}  // namespace

server::outcome server::get_timeline(const json::value& args) {
  std::string why;
  if (!load(&why)) return {why, true, {}};
  const bool original = args.boolean("original").value_or(false);
  const nle::timeline& t = original ? *original_ : *working_;
  const nle::bridge_state b = bridge();
  const scope sc = current_scope();
  json::writer w;
  w.begin_object();
  w.key("project").string(t.project().name);
  w.key("event").string(t.project().event_name);
  w.key("fcpxml").string(t.version());
  const rational frame = t.project().frame;
  if (frame.num > 0) {
    w.key("frame_s").number(secs(frame));
    w.key("fps").number(1.0 / secs(frame));
  }
  w.key("length").number(secs(t.length()));
  w.key("length_clock").string(nle::format_clock(t.length()));
  w.key("working_copy").boolean(!original);
  w.key("pending_changes").integer(static_cast<std::int64_t>(changes_.size()));
  if (!o_.timeline_file.empty()) {
    w.key("source").string("file");
  } else {
    w.key("source").string("panel");
    w.key("dragged").string(nle::dragged_ago(b.dragged_ms, env_.now_ms()));
    const bool stale = b.changed_ms > 0 && b.project == original_->project().name;
    w.key("stale").boolean(stale);
    if (stale) {
      w.key("warning").string("Final Cut Pro changed this project after it was dragged: ask the editor to drag it "
                              "onto the MediaViewer panel again before editing");
    }
    if (!b.active_project.empty() && b.active_project != original_->project().name) {
      w.key("final_cut_has_open").string(b.active_project);
    }
    if (b.playhead_s >= 0) {
      // FCP's playhead is sequence time; the timeline here starts at 0.
      const double start = secs(t.project().tc_start);
      w.key("playhead").number(b.playhead_s >= start ? b.playhead_s - start : b.playhead_s);
      w.key("playhead_age_s").integer((env_.now_ms() - b.playhead_ms_at) / 1000);
    }
  }
  if (newer_waiting_) {
    w.key("newer_drag").string("the editor dragged the project again; reset takes it (your uncommitted edits go)");
  }
  w.key("scope").begin_object().key("folders").begin_array();
  for (const std::string& f : sc.folders()) w.string(f);
  w.end_array().key("timeline_files").integer(static_cast<std::int64_t>(original_->media_paths().size())).end_object();
  w.key("items").begin_array();
  for (std::size_t i = 0; i < t.items().size(); ++i) write_item(w, t, i);
  w.end_array();
  w.end_object();
  std::string text = w.take();
  if (text.size() > kMaxText) {
    // A very long project: the storyline only.
    json::writer s;
    s.begin_object().key("project").string(t.project().name).key("length").number(secs(t.length()))
        .key("note").string("long project: only the storyline is listed")
        .key("items").begin_array();
    for (std::size_t i = 0; i < t.items().size(); ++i) {
      if (t.items()[i].parent == -1) write_item(s, t, i);
    }
    s.end_array().end_object();
    text = s.take();
  }
  return {std::move(text), false, {}};
}

// ---- transcript ---------------------------------------------------------------------------

server::outcome server::transcript(const json::value& args) {
  std::string why;
  if (!load(&why)) return {why, true, {}};
  const scope sc = current_scope();
  const nle::timeline& t = *working_;
  transcript_options o;
  o.words = args.boolean("words").value_or(true);
  o.min_silence_ms = std::clamp(int_arg(args, "min_silence_ms", 400), 100, 10000);
  const auto from = time_arg(args, "from");
  const auto to = time_arg(args, "to");

  std::vector<std::size_t> picks;
  nle::item file_item;  // for a path: the whole file as an "item" at 0
  const std::string* path = args.str("path");
  if (path) {
    const scope::verdict v = sc.check(*path);
    if (v != scope::verdict::inside) return {scope::refusal(v), true, {}};
    nle::media_probe probe;
    if (!env_.probe(*path, probe, &why)) return {"the file cannot be read: " + why, true, {}};
    file_item.kind = nle::item_kind::asset_clip;
    file_item.source = *path;
    file_item.duration = probe.duration;
    file_item.name = std::filesystem::path(*path).filename().string();
    file_item.editable = true;
  } else if (const auto id = args.integer("item")) {
    if (*id < 0 || static_cast<std::size_t>(*id) >= t.items().size()) return {"no such item", true, {}};
    picks.push_back(static_cast<std::size_t>(*id));
  } else {
    for (std::size_t i = 0; i < t.items().size(); ++i) {
      const nle::item& it = t.items()[i];
      if (it.parent != -1 || !it.in_storyline || it.source.empty()) continue;
      picks.push_back(i);
    }
  }
  json::writer w;
  w.begin_object();
  w.key("word_times").string("estimated inside each phrase; phrase edges and silences are measured");
  w.key("clips").begin_array();
  std::size_t emitted = 0;
  bool truncated = false;
  auto one = [&](const nle::item& it, std::int64_t id) {
    const rational end = nle::add(it.at, it.duration).value_or(it.at);
    if (from && end <= *from) return;
    if (to && it.at >= *to) return;
    w.begin_object();
    if (id >= 0) w.key("item").integer(id);
    w.key("name").string(it.name);
    if (it.source.empty() || !sc.allows(it.source)) {
      w.key("note").string(it.opaque ? "a unit (compound, multicam, sync): ask for its files by path"
                                     : "no media file");
      w.end_object();
      return;
    }
    w.key("file").string(it.source);
    w.key("at").number(secs(it.at)).key("end").number(secs(end));
    if (it.retimed) {
      w.key("note").string("retimed: its sound is not 1:1 with the timeline; not laid");
      w.end_object();
      return;
    }
    std::vector<segment> segs;
    bool indexed = false;
    std::string twhy;
    const bool have_text = env_.transcript(it.source, segs, indexed, &twhy);
    std::vector<float> loud;
    const std::int64_t margin = 300;
    const std::int64_t loud_from = std::max<std::int64_t>(0, nle::to_ms(it.source_in) - margin);
    const std::size_t count = static_cast<std::size_t>((nle::to_ms(it.duration) + 2 * margin) / o.hop_ms + 1);
    std::string lwhy;
    const bool have_loud = env_.loudness(it.source, loud_from, o.hop_ms, count, loud, &lwhy);
    if (!have_loud) loud.clear();
    const laid_transcript lt = lay(it, t.project().frame, have_text ? segs : std::vector<segment>{}, loud, loud_from, o);
    w.key("transcribed").boolean(have_text && indexed);
    if (!have_text) w.key("transcript_note").string(twhy);
    else if (!indexed) w.key("transcript_note").string("not transcribed yet: the editor can index its folder in MediaViewer");
    if (!have_loud) w.key("loudness_note").string(lwhy);
    else w.key("speech_threshold_db").number(lt.threshold_db);
    w.key("phrases").begin_array();
    for (const phrase& p : lt.phrases) {
      if (from && p.end <= *from) continue;
      if (to && p.at >= *to) continue;
      w.begin_object().key("at").number(secs(p.at)).key("end").number(secs(p.end)).key("text").string(p.text);
      if (o.words && !p.words.empty()) {
        w.key("words").begin_array();
        for (const word& x : p.words) {
          w.begin_array().string(x.text).number(secs(x.at)).number(secs(x.end)).end_array();
        }
        w.end_array();
      }
      w.end_object();
      if (w.str().size() > kMaxText) {
        truncated = true;
        break;
      }
    }
    w.end_array();
    w.key("silences").begin_array();
    for (const silence& s : lt.silences) {
      if (from && s.end <= *from) continue;
      if (to && s.at >= *to) continue;
      w.begin_object().key("at").number(secs(s.at)).key("end").number(secs(s.end)).key("cut_from")
          .number(secs(s.cut_from)).key("cut_to").number(secs(s.cut_to)).end_object();
    }
    w.end_array();
    w.end_object();
    ++emitted;
  };
  if (path) {
    one(file_item, -1);
  } else {
    for (const std::size_t i : picks) {
      if (truncated) break;
      one(t.items()[i], static_cast<std::int64_t>(i));
    }
  }
  w.end_array();
  if (truncated) w.key("truncated").string("too long for one answer: ask again with from/to");
  w.end_object();
  (void)emitted;
  return {w.take(), false, {}};
}

// ---- search, find_similar -----------------------------------------------------------------

server::outcome server::search(const json::value& args, bool similar) {
  std::string why;
  if (!load(&why)) return {why, true, {}};
  const scope sc = current_scope();
  const nle::timeline& t = *working_;
  const std::uint32_t kinds = kinds_of(args);
  const std::uint32_t max = static_cast<std::uint32_t>(std::clamp(int_arg(args, "max", 20), 1, 100));
  std::string query, from_path;
  std::int64_t from_ms = -1;
  if (!similar) {
    const std::string* q = args.str("query");
    if (!q || q->empty()) return {"search needs a query", true, {}};
    query = *q;
    // A query cannot widen the scope: the pack's own scope words are not passed on.
    for (const char* bad : {"photos:", "in:photos"}) {
      if (query.find(bad) != std::string::npos) return {scope::refusal(scope::verdict::photos), true, {}};
    }
  } else if (const auto id = args.integer("item")) {
    if (*id < 0 || static_cast<std::size_t>(*id) >= t.items().size()) return {"no such item", true, {}};
    const nle::item& it = t.items()[static_cast<std::size_t>(*id)];
    if (it.source.empty()) return {"that item has no single file to compare (a unit, a title or a gap)", true, {}};
    from_path = it.source;
    rational at = nle::add(it.at, nle::reduce({it.duration.num, it.duration.den * 2})).value_or(it.at);
    if (const auto a = time_arg(args, "at")) at = *a;
    from_ms = nle::to_ms(nle::add(it.source_in, nle::sub(at, it.at).value_or(rational{})).value_or(it.source_in));
  } else if (const std::string* p = args.str("path")) {
    const scope::verdict v = sc.check(*p);
    if (v != scope::verdict::inside) return {scope::refusal(v), true, {}};
    from_path = *p;
    if (const auto a = time_arg(args, "at_file")) from_ms = nle::to_ms(*a);
  } else {
    return {"find_similar needs an item or a path", true, {}};
  }
  if (!from_path.empty() && !sc.allows(from_path)) return {scope::refusal(sc.check(from_path)), true, {}};

  struct dir {
    std::string path;
    bool tree;
  };
  std::vector<dir> dirs;
  for (const std::string& f : sc.folders()) dirs.push_back({f, true});
  for (const std::string& d : sc.media_dirs()) dirs.push_back({d, false});
  std::vector<hit> all;
  std::string last_why;
  bool any_ok = false;
  for (const dir& d : dirs) {
    std::vector<hit> got;
    std::string w;
    const bool ok = similar ? env_.similar(from_path, from_ms, d.path, d.tree, kinds, max * 2, got, &w)
                            : env_.search(query, d.path, d.tree, kinds, max * 2, got, &w);
    if (!ok) {
      last_why = w;
      continue;
    }
    any_ok = true;
    for (hit& h : got) {
      if (!sc.allows(h.path)) continue;  // the media folders hold more than the timeline
      all.push_back(std::move(h));
    }
  }
  if (!any_ok) return {"Local search cannot answer: " + (last_why.empty() ? std::string("no scope") : last_why), true, {}};
  // One row per moment, best score.
  std::sort(all.begin(), all.end(), [](const hit& a, const hit& b) { return a.score > b.score; });
  std::vector<hit> rows;
  std::set<std::pair<std::string, std::int64_t>> seen;
  for (hit& h : all) {
    if (similar && h.path == from_path && std::llabs(h.pts_ms - from_ms) < 2000) continue;  // itself
    if (!seen.insert({h.path, h.pts_ms}).second) continue;
    rows.push_back(std::move(h));
    if (rows.size() >= max) break;
  }
  // A description nothing answers comes back as a plain listing (the pack's
  // "nothing found", scored 0): that is not a match.
  const bool nothing = std::none_of(rows.begin(), rows.end(), [](const hit& h) { return h.score > 0; });
  if (nothing) rows.clear();
  json::writer w;
  w.begin_object();
  if (nothing) w.key("note").string("nothing in the scope matched");
  w.key("results").begin_array();
  for (const hit& h : rows) {
    w.begin_object().key("path").string(h.path);
    if (h.pts_ms >= 0) w.key("at_file").number(static_cast<double>(h.pts_ms) / 1000.0);
    w.key("score").number(h.score);
    w.key("kind").string(h.kind == 2 ? "video" : (h.kind == 8 ? "audio" : "photo"));
    if (h.duration_ms > 0) w.key("file_length").number(static_cast<double>(h.duration_ms) / 1000.0);
    if (!h.moments.empty()) {
      w.key("other_moments").begin_array();
      for (std::size_t k = 0; k < h.moments.size() && k < 8; ++k) w.number(static_cast<double>(h.moments[k]) / 1000.0);
      w.end_array();
    }
    // Where the timeline already uses it.
    w.key("on_timeline").begin_array();
    for (std::size_t i = 0; i < t.items().size(); ++i) {
      const nle::item& it = t.items()[i];
      if (it.source != h.path) continue;
      const std::int64_t in = nle::to_ms(it.source_in), out = in + nle::to_ms(it.duration);
      if (h.pts_ms < 0 || (h.pts_ms >= in && h.pts_ms < out)) w.integer(static_cast<std::int64_t>(i));
    }
    w.end_array();
    w.end_object();
  }
  w.end_array().end_object();
  return {w.take(), false, {}};
}

// ---- scope_files --------------------------------------------------------------------------

namespace {

const char* media_kind(std::string ext) {
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
  static constexpr std::string_view kVideo[] = {".mov", ".mp4", ".m4v", ".mkv", ".webm", ".avi", ".ts", ".mts",
                                                ".m2ts", ".mxf", ".3gp"};
  static constexpr std::string_view kAudio[] = {".wav", ".m4a", ".mp3", ".aif", ".aiff", ".caf", ".flac", ".aac"};
  static constexpr std::string_view kPhoto[] = {".jpg", ".jpeg", ".png", ".heic", ".heif", ".tif", ".tiff", ".dng",
                                                ".cr2", ".cr3", ".arw", ".nef", ".raf", ".orf", ".rw2", ".webp", ".avif"};
  for (const std::string_view e : kVideo) if (ext == e) return "video";
  for (const std::string_view e : kAudio) if (ext == e) return "audio";
  for (const std::string_view e : kPhoto) if (ext == e) return "photo";
  return nullptr;
}

}  // namespace

server::outcome server::scope_files(const json::value& args) {
  std::string why;
  if (!load(&why)) return {why, true, {}};
  const scope sc = current_scope();
  std::vector<std::string> folders = sc.folders();
  if (const std::string* f = args.str("folder")) {
    if (sc.check(*f) != scope::verdict::inside) return {scope::refusal(sc.check(*f)), true, {}};
    folders = {*f};
  }
  if (folders.empty()) {
    return {"no folder given: the editor can add folders in the MediaViewer panel (Folders)", true, {}};
  }
  const std::size_t max = static_cast<std::size_t>(std::clamp(int_arg(args, "max", 200), 1, 2000));
  struct file {
    std::string path, kind;
    std::uint64_t size;
  };
  std::vector<file> files;
  bool more = false;
  for (const std::string& folder : folders) {
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(std::filesystem::path(folder),
                                                     std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      const std::filesystem::path& p = it->path();
      const std::string name = p.filename().string();
      if (!name.empty() && name[0] == '.') {
        if (it->is_directory(ec)) it.disable_recursion_pending();
        continue;
      }
      if (it->is_directory(ec)) {
        // Libraries and bundles are packages, not folders of media.
        const std::string ext = p.extension().string();
        if (ext == ".fcpbundle" || ext == ".photoslibrary" || ext == ".app" || ext == ".fcpxmld") {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (it->is_symlink(ec)) continue;
      const char* kind = media_kind(p.extension().string());
      if (!kind || !sc.allows(p.string())) continue;
      if (files.size() >= max) {
        more = true;
        break;
      }
      files.push_back({p.string(), kind, static_cast<std::uint64_t>(it->file_size(ec))});
    }
  }
  std::sort(files.begin(), files.end(), [](const file& a, const file& b) { return a.path < b.path; });
  const nle::timeline& t = *working_;
  json::writer w;
  w.begin_object().key("files").begin_array();
  for (const file& f : files) {
    w.begin_object().key("path").string(f.path).key("kind").string(f.kind).key("bytes")
        .integer(static_cast<std::int64_t>(f.size));
    if (files.size() <= 40 && f.kind != std::string("photo")) {
      nle::media_probe m;
      std::string pwhy;
      if (env_.probe(f.path, m, &pwhy) && m.duration.num > 0) w.key("length").number(secs(m.duration));
    }
    bool used = false;
    for (const nle::item& it : t.items()) used = used || it.source == f.path;
    if (used) w.key("on_timeline").boolean(true);
    w.end_object();
  }
  w.end_array();
  if (more) w.key("note").string("more files than shown: ask for one folder, or a larger max");
  w.end_object();
  return {w.take(), false, {}};
}

// ---- analyse ------------------------------------------------------------------------------

server::outcome server::analyse(const json::value& args) {
  std::string why;
  if (!load(&why)) return {why, true, {}};
  const scope sc = current_scope();
  // The given folders and below; the folders the timeline's files sit in, by
  // themselves (the scope filters what search answers, not what is indexed).
  std::vector<std::pair<std::string, bool>> folders;
  for (const std::string& f : sc.folders()) folders.emplace_back(f, true);
  for (const std::string& d : sc.media_dirs()) {
    if (d.rfind("photos:", 0) == 0) continue;
    bool covered = false;
    for (const auto& [f, rec] : folders) covered = covered || comparable(d).rfind(comparable(f) + "/", 0) == 0;
    if (!covered) folders.emplace_back(d, false);
  }
  if (folders.empty()) return {"nothing to index: the timeline has no media files and no folder was given", true, {}};
  json::writer w;
  w.begin_object();
  if (!args.boolean("status_only").value_or(false)) {
    std::string state;
    bool launched = false;
    if (!env_.request_index(folders, state, launched, &why)) return {"MediaViewer was not asked: " + why, true, {}};
    w.key("request").string(state);
    if (launched) w.key("launched").string("MediaViewer was started in the background to index");
    if (state == "no_app") {
      w.key("note").string("MediaViewer did not answer: ask the editor to open MediaViewer, then call analyse again");
    } else if (state == "queued") {
      w.key("note").string("MediaViewer has the request and starts once Local search starts");
    } else if (state == "refused") {
      w.key("note").string("MediaViewer refused the request");
    }
    env_.log("index requested");
  }
  // Progress, through the read-only index (it catches up every few seconds).
  std::vector<std::string> paths;
  for (const auto& [f, rec] : folders) paths.push_back(f);
  const int wait_s = std::clamp(int_arg(args, "wait_s", 10), 0, 120);
  const std::int64_t until = env_.now_ms() + wait_s * 1000;
  std::vector<int> states;
  std::vector<std::pair<std::string, bool>> files;
  bool done = false;
  for (;;) {
    states.clear();
    std::string cwhy;
    const bool have = env_.coverage(paths, states, &cwhy);
    files.clear();
    bool all_text = true;
    for (const std::string& m : original_->media_paths()) {
      if (!sc.allows(m)) continue;
      std::vector<segment> segs;
      bool indexed = false;
      std::string twhy;
      (void)env_.transcript(m, segs, indexed, &twhy);
      files.emplace_back(m, indexed);
      all_text = all_text && indexed;
    }
    done = have && all_text && std::all_of(states.begin(), states.end(), [](int s) { return s == 2; });
    if (done || env_.now_ms() >= until) {
      if (!have) w.key("progress_note").string("Local search cannot report progress: " + cwhy);
      break;
    }
    env_.sleep_ms(2000);
  }
  w.key("folders").begin_array();
  for (std::size_t i = 0; i < folders.size(); ++i) {
    const int s = i < states.size() ? states[i] : 0;
    w.begin_object().key("path").string(folders[i].first).key("recursive").boolean(folders[i].second).key("state")
        .string(s == 2 ? "indexed" : (s == 1 ? "indexing" : "not indexed")).end_object();
  }
  w.end_array();
  w.key("timeline_files").begin_array();
  for (const auto& [p, t] : files) w.begin_object().key("path").string(p).key("transcribed").boolean(t).end_object();
  w.end_array();
  w.key("done").boolean(done);
  if (!done) w.key("next").string("call analyse with status_only to follow it; the other tools work meanwhile");
  w.end_object();
  return {w.take(), false, {}};
}

// ---- edit, preview, commit, reset ---------------------------------------------------------

server::outcome server::edit(const json::value& args) {
  std::string why;
  if (!load(&why)) return {why, true, {}};
  const json::value* ops_v = args.find("ops");
  if (!ops_v || ops_v->k != json::kind::array) return {"edit needs \"ops\", an array of operations", true, {}};
  std::vector<nle::edit_op> ops;
  if (!nle::parse_ops(to_text(*ops_v), ops, &why)) return {why, true, {}};
  const scope sc = current_scope();
  std::vector<std::pair<std::string, nle::media_probe>> probes;
  for (std::size_t i = 0; i < ops.size(); ++i) {
    if (ops[i].path.empty()) continue;
    const scope::verdict v = sc.check(ops[i].path);
    if (v != scope::verdict::inside) {
      return {"op " + std::to_string(i) + ": " + scope::refusal(v) + " (nothing in this batch was applied)", true, {}};
    }
    bool have = false;
    for (const auto& [p, m] : probes) have = have || p == ops[i].path;
    if (have) continue;
    nle::media_probe m;
    std::string pwhy;
    if (!env_.probe(ops[i].path, m, &pwhy)) {
      return {"op " + std::to_string(i) + ": the file cannot be read: " + pwhy, true, {}};
    }
    probes.emplace_back(ops[i].path, m);
  }
  const rational before = working_->length();
  nle::editor e(*working_);
  if (!e.apply(ops, probes, &why)) return {why, true, {}};
  for (const nle::change& c : e.changes()) changes_.push_back(c);
  env_.log("edit applied");
  json::writer w;
  w.begin_object();
  w.key("applied").integer(static_cast<std::int64_t>(ops.size()));
  w.key("changes").begin_array();
  for (const nle::change& c : e.changes()) w.string(c.what);
  w.end_array();
  w.key("length_before").number(secs(before)).key("length").number(secs(working_->length()));
  w.key("length_clock").string(nle::format_clock(working_->length()));
  w.key("pending_changes").integer(static_cast<std::int64_t>(changes_.size()));
  w.key("next").string("item ids have changed: call get_timeline before addressing items; preview_changes, then commit");
  w.end_object();
  return {w.take(), false, {}};
}

server::outcome server::preview() {
  std::string why;
  if (!load(&why)) return {why, true, {}};
  rational removed{}, added{};
  for (const nle::change& c : changes_) {
    removed = nle::add(removed, c.removed).value_or(removed);
    added = nle::add(added, c.added).value_or(added);
  }
  std::vector<std::string> taken = original_->project_names();
  for (const nle::bridge_version& v : bridge().versions) taken.push_back(v.name);
  taken.insert(taken.end(), committed_.begin(), committed_.end());
  json::writer w;
  w.begin_object();
  w.key("project").string(original_->project().name);
  w.key("would_write").string(nle::next_version_name(original_->project().name, taken));
  w.key("length_before").number(secs(original_->length())).key("length_after").number(secs(working_->length()));
  w.key("shorter_by").number(secs(nle::sub(original_->length(), working_->length()).value_or(rational{})));
  w.key("removed").number(secs(removed)).key("added").number(secs(added));
  w.key("items_before").integer(static_cast<std::int64_t>(original_->items().size()));
  w.key("items_after").integer(static_cast<std::int64_t>(working_->items().size()));
  w.key("changes").begin_array();
  for (const nle::change& c : changes_) {
    w.begin_object().key("what").string(c.what).key("at").number(secs(c.at)).end_object();
  }
  w.end_array();
  if (changes_.empty()) w.key("note").string("no changes yet");
  w.end_object();
  return {w.take(), false, {}};
}

server::outcome server::commit(const json::value& args) {
  std::string why;
  if (!load(&why)) return {why, true, {}};
  nle::bridge_state b = bridge();
  std::vector<std::string> taken = original_->project_names();
  for (const nle::bridge_version& v : b.versions) taken.push_back(v.name);
  taken.insert(taken.end(), committed_.begin(), committed_.end());
  std::string name = nle::next_version_name(original_->project().name, taken);
  if (const std::string* n = args.str("name"); n && !n->empty()) {
    if (*n == original_->project().name) {
      return {"refused: a version never takes the original project's name (it would sit over it)", true, {}};
    }
    if (n->size() > 200) return {"the name is too long", true, {}};
    name = *n;
  }
  const std::string document = working_->write_version(name);
  // Where it goes: the panel's versions folder, or beside the file worked on.
  std::string dir;
  if (o_.timeline_file.empty()) {
    dir = (std::filesystem::path(o_.bridge_dir) / std::string(nle::kBridgeVersions)).string();
  } else {
    dir = std::filesystem::path(o_.timeline_file).parent_path().string();
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(dir), ec);
  const std::string file = unique_path(dir, nle::version_file_name(name));
  if (!write_atomically(file, document)) return {"the version could not be written", true, {}};
  env_.log("version written");
  committed_.push_back(name);
  if (o_.timeline_file.empty() && !o_.bridge_dir.empty()) {
    b = bridge();  // the panel may have written meanwhile
    b.versions.push_back({name, std::filesystem::path(file).filename().string(), env_.now_ms(),
                          original_->project().name});
    (void)write_atomically((std::filesystem::path(o_.bridge_dir) / std::string(nle::kBridgeState)).string(),
                           nle::to_json(b));
  }
  bool handed = false;
  std::string hwhy;
  if (o_.handoff) handed = env_.hand_to_editor(file, &hwhy);
  json::writer w;
  w.begin_object();
  w.key("version").string(name);
  w.key("file").string(file);
  w.key("length").number(secs(working_->length()));
  w.key("changes").integer(static_cast<std::int64_t>(changes_.size()));
  w.key("handed_to_final_cut").boolean(handed);
  if (handed) {
    w.key("note").string("Final Cut Pro is importing it beside the original, in the same event. The original "
                         "project and the media files are unchanged.");
  } else {
    w.key("note").string(hwhy.empty() ? "import the file into your editor (Final Cut Pro: File > Import > XML)"
                                      : hwhy);
  }
  w.end_object();
  return {w.take(), false, {}};
}

server::outcome server::reset() {
  std::string why;
  changes_.clear();
  original_.reset();
  working_.reset();
  loaded_stamp_ = 0;
  if (!load(&why)) return {why, true, {}};
  return {R"({"reset":true,"note":"the working copy is the project as last dragged"})", false, {}};
}

server::outcome server::thumbnail(const json::value& args) {
  if (!env_.thumbnails_allowed()) {
    return {"refused: frames are off. The editor can turn on 'Let the agent see frames' in MediaViewer's "
            "Settings > Add-ons > AI agents",
            true, {}};
  }
  std::string why;
  if (!load(&why)) return {why, true, {}};
  const nle::timeline& t = *working_;
  const nle::item* pick = nullptr;
  rational at{};
  if (const auto id = args.integer("item")) {
    if (*id < 0 || static_cast<std::size_t>(*id) >= t.items().size()) return {"no such item", true, {}};
    pick = &t.items()[static_cast<std::size_t>(*id)];
    at = nle::add(pick->at, nle::reduce({pick->duration.num, pick->duration.den * 2})).value_or(pick->at);
    if (const auto a = time_arg(args, "at")) at = *a;
  } else if (const auto a = time_arg(args, "at")) {
    at = *a;
    int best_lane = -1000;
    for (const nle::item& it : t.items()) {
      if (it.source.empty() || !it.enabled || it.lane < best_lane) continue;
      if (!(it.at <= at && at < nle::add(it.at, it.duration).value_or(it.at))) continue;
      const auto as = t.assets().find(it.ref);
      if (as != t.assets().end() && !as->second.has_video) continue;
      pick = &it;
      best_lane = it.lane;
    }
  } else {
    return {"thumbnail needs at (timeline seconds) or item", true, {}};
  }
  if (!pick || pick->source.empty()) return {"no picture there", true, {}};
  const scope sc = current_scope();
  if (!sc.allows(pick->source)) return {scope::refusal(sc.check(pick->source)), true, {}};
  const std::int64_t file_ms =
      nle::to_ms(nle::add(pick->source_in, nle::sub(at, pick->at).value_or(rational{})).value_or(pick->source_in));
  std::vector<std::uint8_t> jpeg;
  if (!env_.frame_jpeg(pick->source, file_ms, jpeg, &why)) return {"no frame: " + why, true, {}};
  json::writer w;
  w.begin_object().key("file").string(pick->source).key("at_file").number(static_cast<double>(file_ms) / 1000.0)
      .key("at").number(secs(at)).end_object();
  return {w.take(), false, base64(jpeg)};
}

}  // namespace mv::agent
