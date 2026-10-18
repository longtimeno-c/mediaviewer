// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv_agent's one export, mv_addon_get, and its two interfaces (docs/design/29):
//
//   mv.agent.1  `MediaViewer --mcp`: serve one MCP session over the stdio
//               pipes, with the host table for sound and frames and the
//               host's Local search services.
//   mv.slots.1  the "AI agents" window both hosts draw from the tree posted
//               here: what the agent sees (the privacy line), Add to Claude
//               Code, Add to Claude Desktop (asks first), the command for any
//               other client, frames on or off, and (Windows) the FCPXML file
//               and folders to work on.
//
// The add-on writes only its own data folder (agent.json) and, when the
// editor confirms, Claude Desktop's settings file (the old one kept as .bak).
// Every thunk is the ABI boundary: no exception crosses it (docs/design/14),
// and nothing here logs a path, a name or a query (rule 6).
#include <mediaviewer/mediaviewer_addon.h>
#include <mediaviewer/mediaviewer_agent.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "addons/agent/environment.h"
#include "addons/agent/install.h"
#include "addons/agent/platform.h"
#include "addons/agent/server.h"
#include "core/json.h"
#include "nle/bridge_state.h"

#ifndef MV_AGENT_VERSION
#define MV_AGENT_VERSION "0.0.0-dev"
#endif

namespace {

using namespace mv;
using namespace mv::agent;

constexpr uint32_t kHostApiMin = 3;
constexpr uint32_t kHostApiMax = 3;
constexpr const char* kScreen = "agents";
constexpr const char* kPrefsFile = "agent.json";

template <typename Fn>
mv_status guard(Fn&& fn) noexcept {
  try {
    return fn();
  } catch (const std::bad_alloc&) {
    return MV_ERR_OUT_OF_MEMORY;
  } catch (...) {
    return MV_ERR_INTERNAL;
  }
}

#define MV_AG_HAS(h, field) \
  ((h)->struct_size >= offsetof(mv_host_api, field) + sizeof((h)->field) && (h)->field != nullptr)

// ---- what the editor set here (agent.json in the add-on's data folder) -----------------

struct prefs {
  bool frames = false;            // thumbnails to the model
  std::string timeline_file;      // Windows: the FCPXML to work on
  std::vector<std::string> folders;  // Windows: the folders given
};

prefs load_prefs(const std::string& path) {
  prefs p;
  std::string text;
  if (path.empty() || !platform::read_file(path, text)) return p;
  const auto v = json::parse(text, 8);
  if (!v || v->k != json::kind::object) return p;
  p.frames = v->boolean("frames").value_or(false);
  if (const std::string* t = v->str("timeline_file")) p.timeline_file = *t;
  if (const json::value* f = v->find("folders"); f && f->k == json::kind::array) {
    for (const json::value& e : f->a) {
      if (e.k == json::kind::string && !e.s.empty()) p.folders.push_back(e.s);
    }
  }
  return p;
}

bool save_prefs(const std::string& path, const prefs& p) {
  if (path.empty()) return false;
  json::writer w;
  w.begin_object().key("frames").boolean(p.frames).key("timeline_file").string(p.timeline_file).key("folders");
  w.begin_array();
  for (const std::string& f : p.folders) w.string(f);
  w.end_array().end_object();
  return platform::write_file_atomic(path, w.take());
}

std::string data_dir(const mv_host_api* h) {
  char buf[4096];
  if (!h->data_dir || h->data_dir(h->host, buf, sizeof buf) != MV_OK || !*buf) return {};
  return buf;
}

std::string prefs_path(const mv_host_api* h) {
  const std::string d = data_dir(h);
  return d.empty() ? std::string() : (std::filesystem::path(d) / kPrefsFile).string();
}

// The panel's shared folder on the Mac; the add-on's own on Windows.
std::string default_bridge_dir(const mv_host_api* h) {
#if defined(__APPLE__) && defined(MV_AGENT_BRIDGE_GROUP)
  (void)h;
  const char* home = std::getenv("HOME");
  if (!home) return {};
  return std::string(home) + "/Library/Group Containers/" MV_AGENT_BRIDGE_GROUP "/" +
         std::string(nle::kBridgeFolder);
#else
  const std::string d = data_dir(h);
  return d.empty() ? std::string() : (std::filesystem::path(d) / std::string(nle::kBridgeFolder)).string();
#endif
}

// ---- the environment over the host -------------------------------------------------------

class host_env final : public environment {
 public:
  host_env(const mv_host_api* h, const mv_agent_services* s, bool frames) : h_(h), s_(s), frames_(frames) {}

  bool search(std::string_view query, std::string_view dir, bool tree, std::uint32_t kinds, std::uint32_t max,
              std::vector<hit>& out, std::string* why) override {
    if (!s_ || !s_->search_json) return none(why);
    const std::string q(query), d(dir);
    return hits(
        [&](char* buf, uint32_t cap, uint32_t* need) {
          return s_->search_json(s_->ctx, q.c_str(), d.c_str(), tree ? 1u : 0u, kinds, max, buf, cap, need);
        },
        out, why);
  }
  bool similar(std::string_view path, std::int64_t pts_ms, std::string_view dir, bool tree, std::uint32_t kinds,
               std::uint32_t max, std::vector<hit>& out, std::string* why) override {
    if (!s_ || !s_->similar_json) return none(why);
    const std::string p(path), d(dir);
    return hits(
        [&](char* buf, uint32_t cap, uint32_t* need) {
          return s_->similar_json(s_->ctx, p.c_str(), pts_ms, d.c_str(), tree ? 1u : 0u, kinds, max, buf, cap, need);
        },
        out, why);
  }
  bool transcript(std::string_view path, std::vector<segment>& out, bool& indexed, std::string* why) override {
    indexed = false;
    out.clear();
    if (!s_ || !s_->transcript_json) return none(why);
    const std::string p(path);
    std::string text;
    const mv_status st = call_json(
        [&](char* buf, uint32_t cap, uint32_t* need) { return s_->transcript_json(s_->ctx, p.c_str(), buf, cap, need); },
        text);
    if (st != MV_OK) {
      if (why) {
        *why = st == MV_ERR_UNSUPPORTED_FORMAT ? "the installed Local search is too old for transcripts: update it in Settings"
                                               : (st == MV_ERR_NOT_FOUND ? "Local search is not installed"
                                                                         : mv_status_name_of(st));
      }
      return false;
    }
    const auto v = json::parse(text, 8);
    if (!v) return false;
    indexed = v->boolean("indexed").value_or(false);
    if (const json::value* segs = v->find("segments"); segs && segs->k == json::kind::array) {
      for (const json::value& s : segs->a) {
        segment g;
        g.start_ms = s.integer("start_ms").value_or(0);
        g.end_ms = s.integer("end_ms").value_or(0);
        if (const std::string* t = s.str("text")) g.text = *t;
        if (g.end_ms > g.start_ms) out.push_back(std::move(g));
      }
    }
    return true;
  }
  bool loudness(std::string_view path, std::int64_t from_ms, int hop_ms, std::size_t count, std::vector<float>& db,
                std::string* why) override {
    db.clear();
    if (!h_->audio_open || !h_->audio_read || !h_->audio_close) {
      if (why) *why = "this MediaViewer cannot read sound for add-ons";
      return false;
    }
    constexpr uint32_t kRate = 16000;
    void* a = nullptr;
    const std::string p(path);
    const mv_status st = h_->audio_open(h_->host, p.c_str(), kRate, from_ms, nullptr, &a);
    if (st != MV_OK || !a) {
      if (why) *why = st == MV_ERR_UNSUPPORTED_FORMAT ? "the file has no sound" : "the sound could not be read";
      return false;
    }
    const std::size_t per_hop = static_cast<std::size_t>(kRate) * static_cast<std::size_t>(hop_ms) / 1000;
    std::vector<float> buf(16000);
    double acc = 0;
    std::size_t in_hop = 0;
    bool first = true;
    std::int64_t skip = 0;
    while (db.size() < count) {
      uint32_t got = 0;
      int64_t t0 = 0;
      if (h_->audio_read(h_->host, a, buf.data(), static_cast<uint32_t>(buf.size()), &got, &t0) != MV_OK || got == 0) {
        break;
      }
      std::size_t k = 0;
      if (first) {
        // The decoder starts at the keyframe-ish point at or before from_ms.
        skip = std::max<std::int64_t>(0, (from_ms - t0) * kRate / 1000);
        first = false;
      }
      for (; k < got && db.size() < count; ++k) {
        if (skip > 0) {
          --skip;
          continue;
        }
        acc += static_cast<double>(buf[k]) * buf[k];
        if (++in_hop == per_hop) {
          const double rms = std::sqrt(acc / static_cast<double>(per_hop));
          db.push_back(static_cast<float>(rms > 1e-6 ? 20.0 * std::log10(rms) : -120.0));
          acc = 0;
          in_hop = 0;
        }
      }
    }
    h_->audio_close(h_->host, a);
    if (db.empty()) {
      if (why) *why = "no sound in that range";
      return false;
    }
    return true;
  }
  bool probe(std::string_view path, nle::media_probe& out, std::string* why) override {
    out = {};
    const std::string p(path);
    if (h_->sampler_open && h_->sampler_close) {
      mv_addon_sampler_options o{};
      o.struct_size = sizeof o;
      mv_addon_video_info info{};
      void* s = nullptr;
      if (h_->sampler_open(h_->host, p.c_str(), &o, &info, &s) == MV_OK && s) {
        out.has_video = true;
        out.width = static_cast<int>(info.width);
        out.height = static_cast<int>(info.height);
        out.duration = nle::from_ms(info.duration_ms);
        h_->sampler_close(h_->host, s);
      }
    }
    if (h_->audio_open && h_->audio_close) {
      void* a = nullptr;
      int64_t dur = 0;
      if (h_->audio_open(h_->host, p.c_str(), 16000, 0, &dur, &a) == MV_OK && a) {
        out.has_audio = true;
        out.audio_channels = 2;
        out.audio_rate = 48000;
        if (out.duration.num == 0) out.duration = nle::from_ms(dur);
        h_->audio_close(h_->host, a);
      }
    }
    if (!out.has_video && !out.has_audio) {
      // A still: shown for as long as the edit asks.
      std::uint64_t size = 0;
      int64_t mtime = 0;
      uint32_t dir = 0;
      if (h_->stat_file && h_->stat_file(h_->host, p.c_str(), &size, &mtime, &dir) == MV_OK && !dir) {
        out.has_video = true;
        return true;
      }
      if (why) *why = "not a file MediaViewer can play";
      return false;
    }
    return true;
  }
  bool frame_jpeg(std::string_view path, std::int64_t pts_ms, std::vector<std::uint8_t>& jpeg, std::string* why) override {
    if (!h_->video_frame_rgb || !h_->moment_thumbnail) {
      if (why) *why = "this MediaViewer cannot make frames for add-ons";
      return false;
    }
    const std::string p(path);
    std::vector<std::uint8_t> rgb(512u * 512u * 3u);
    uint32_t w = 0, hgt = 0;
    if (h_->video_frame_rgb(h_->host, p.c_str(), pts_ms, 512, rgb.data(), rgb.size(), &w, &hgt) != MV_OK) {
      if (why) *why = "the frame could not be decoded";
      return false;
    }
    char out[4096];
    if (h_->moment_thumbnail(h_->host, p.c_str(), pts_ms, rgb.data(), w, hgt, out, sizeof out) != MV_OK) {
      if (why) *why = "the frame could not be encoded";
      return false;
    }
    std::string bytes;
    if (!platform::read_file(out, bytes)) return false;
    jpeg.assign(bytes.begin(), bytes.end());
    return true;
  }
  bool hand_to_editor(const std::string& fcpxml, std::string* why) override {
    return platform::open_in_final_cut(fcpxml, why);
  }
  bool thumbnails_allowed() override { return frames_; }
  std::int64_t now_ms() override {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
  }
  void log(const char* ascii) override {
    if (h_->log) h_->log(h_->host, 0, ascii);
    std::fprintf(stderr, "mediaviewer agent: %s\n", ascii);
  }

 private:
  static const char* mv_status_name_of(mv_status st) {
    switch (st) {
      case MV_ERR_BUSY: return "Local search is busy";
      case MV_ERR_TIMEOUT: return "Local search did not answer in time";
      default: return "Local search failed";
    }
  }
  static bool none(std::string* why) {
    if (why) *why = "Local search is not installed (Settings > Add-ons)";
    return false;
  }
  template <typename Fn>
  static mv_status call_json(Fn&& fn, std::string& out) {
    std::string buf(64 * 1024, '\0');
    for (int tries = 0; tries < 4; ++tries) {
      uint32_t need = 0;
      const mv_status st = fn(buf.data(), static_cast<uint32_t>(buf.size()), &need);
      if (st == MV_ERR_INVALID_ARG && need > buf.size()) {
        buf.assign(need + 1, '\0');
        continue;
      }
      if (st != MV_OK) return st;
      out.assign(buf.c_str());
      return MV_OK;
    }
    return MV_ERR_INTERNAL;
  }
  template <typename Fn>
  bool hits(Fn&& fn, std::vector<hit>& out, std::string* why) {
    std::string text;
    const mv_status st = call_json(fn, text);
    if (st != MV_OK) {
      if (why) {
        *why = st == MV_ERR_NOT_FOUND ? "Local search is not installed"
                                      : (st == MV_ERR_UNSUPPORTED_FORMAT ? "the installed Local search is too old"
                                                                         : mv_status_name_of(st));
      }
      return false;
    }
    const auto v = json::parse(text, 8);
    if (!v || v->k != json::kind::array) return false;
    for (const json::value& r : v->a) {
      hit x;
      if (const std::string* p = r.str("path")) x.path = *p;
      x.pts_ms = r.integer("pts_ms").value_or(-1);
      x.duration_ms = r.integer("duration_ms").value_or(0);
      if (const json::value* s = r.find("score"); s && s->k == json::kind::number) {
        x.score = static_cast<float>(s->is_integer ? static_cast<double>(s->i) : s->d);
      }
      x.kind = static_cast<std::uint32_t>(r.integer("kind").value_or(0));
      if (const json::value* m = r.find("moments"); m && m->k == json::kind::array) {
        for (const json::value& e : m->a) {
          if (e.k == json::kind::number && e.is_integer) x.moments.push_back(e.i);
        }
      }
      if (!x.path.empty()) out.push_back(std::move(x));
    }
    return true;
  }

  const mv_host_api* h_;
  const mv_agent_services* s_;
  bool frames_;
};

// ---- the add-on --------------------------------------------------------------------------

struct addon_state {
  const mv_host_api* host = nullptr;
  std::mutex m;
  std::uint64_t generation = 0;
  bool open = false;
  bool asking_desktop = false;  // "Add to Claude Desktop…" waits for the editor's yes
  std::string message;          // the last action's outcome
  std::string claude;           // the claude command line, "" none
  bool claude_checked = false;
  bool code_added = false;
  bool serving = false;
};

addon_state* g = nullptr;

void check_clients() {
  g->claude = platform::find_claude();
  g->code_added = false;
  if (!g->claude.empty()) {
    std::string out;
    int code = 1;
    g->code_added = platform::run(g->claude, claude_code_get_args(), out, code, 15000) && code == 0;
  }
  g->claude_checked = true;
}

std::string tree() {
  const std::string exe = platform::self_executable();
  const prefs p = load_prefs(prefs_path(g->host));
  std::string desktop_text;
  const std::string desktop_path = platform::desktop_config_path();
  const bool desktop_file = platform::read_file(desktop_path, desktop_text);
  std::string desktop_cmd;
  const bool desktop_added = desktop_file && desktop_config_has(desktop_text, &desktop_cmd);

  json::writer w;
  auto text = [&](const char* id, const std::string& t, const char* style = "body", bool copyable = false) {
    w.begin_object().key("type").string("text").key("id").string(id).key("text").string(t).key("style").string(style);
    if (copyable) w.key("copyable").boolean(true);
    w.end_object();
  };
  auto button = [&](const char* id, const char* label, const char* style = "plain") {
    w.begin_object().key("type").string("button").key("id").string(id).key("label").string(label).key("style")
        .string(style).end_object();
  };
  auto card = [&](const char* id) {
    w.begin_object().key("type").string("stack").key("id").string(id).key("look").string("card").key("spacing")
        .integer(8).key("children").begin_array();
  };
  auto row = [&](const char* id) {
    w.begin_object().key("type").string("stack").key("id").string(id).key("axis").string("horizontal").key("spacing")
        .integer(8).key("children").begin_array();
  };
  auto end = [&] { w.end_array().end_object(); };

  w.begin_object().key("type").string("scroll").key("id").string("scroll").key("children").begin_array();
  w.begin_object().key("type").string("stack").key("id").string("root").key("spacing").integer(14).key("children")
      .begin_array();
  text("title", "AI agents", "title");
  text("intro",
       "Claude, or any agent that speaks MCP, can read and edit the Final Cut Pro project you drag onto the "
       "MediaViewer panel: tighten an interview, cut to a length, cover jump cuts with B-roll. Each pass comes back "
       "as a new project beside yours; your project and your files are never changed.");

  card("privacy");
  text("privacy_title", "What the agent sees", "heading");
  text("privacy_body",
       "File paths, timecodes and the transcript text of the timeline you dragged and of the folders you give. "
       "Nothing else: not your Photos library, not other folders, not the rest of your index. A cloud model such as "
       "Claude receives what it reads. It sees no pictures unless you turn on frames below. MediaViewer opens no "
       "network connection for this and logs only status names.");
  w.begin_object().key("type").string("toggle").key("id").string("frames").key("label")
      .string("Let the agent see frames (pictures from the timeline go to the model)").key("value").boolean(p.frames)
      .end_object();
  end();

  card("code");
  text("code_title", "Claude Code", "heading");
  if (!g->claude_checked) {
    text("code_status", "Checking for Claude Code…", "secondary");
  } else if (g->claude.empty()) {
    text("code_status", "Claude Code is not installed on this computer.", "secondary");
  } else {
    text("code_status", g->code_added ? "Added: Claude Code can use MediaViewer in every project."
                                      : "Not added yet.",
         "secondary");
    row("code_buttons");
    if (g->code_added) {
      button("code_remove", "Remove from Claude Code", "destructive");
    } else {
      button("code_add", "Add to Claude Code", "primary");
    }
    end();
  }
  end();

  card("desktop");
  text("desktop_title", "Claude Desktop", "heading");
  if (!platform::desktop_installed() && !desktop_file) {
    text("desktop_status", "Claude Desktop is not installed on this computer.", "secondary");
  } else if (g->asking_desktop) {
    text("desktop_ask",
         "MediaViewer will add itself to Claude Desktop's settings file (" + desktop_path +
             "). Everything else in it stays; the old file is kept beside it as .bak. Quit and reopen Claude Desktop "
             "afterwards.");
    row("desktop_confirm_row");
    button("desktop_confirm", "Add It", "primary");
    button("desktop_cancel", "Cancel");
    end();
  } else {
    text("desktop_status",
         desktop_added ? (desktop_cmd == exe ? "Added." : "Added, for another copy of MediaViewer: add it again to use this one.")
                       : "Not added yet.",
         "secondary");
    row("desktop_buttons");
    button("desktop_add", desktop_added && desktop_cmd == exe ? "Add Again…" : "Add to Claude Desktop…",
           desktop_added ? "plain" : "primary");
    if (desktop_added) button("desktop_remove", "Remove from Claude Desktop", "destructive");
    end();
  }
  end();

  card("other");
  text("other_title", "Another MCP client", "heading");
  text("other_help", "Start MediaViewer with --mcp over stdio:", "secondary");
  text("other_command", claude_code_command_line(exe), "mono", true);
  text("other_json", client_config_snippet(exe), "mono", true);
  end();

  card("timeline");
  text("timeline_title", "The timeline", "heading");
#if defined(__APPLE__)
  {
    nle::bridge_state b;
    std::string st;
    if (platform::read_file((std::filesystem::path(default_bridge_dir(g->host)) / std::string(nle::kBridgeState)).string(), st)) {
      b = nle::bridge_state_from_json(st).value_or(nle::bridge_state{});
    }
    text("timeline_status",
         b.project.empty() ? "In Final Cut Pro, open Extensions > MediaViewer Search and drag a project onto it."
                           : "The agent works on “" + b.project + "”. Drag it onto the panel again after you change it.",
         "secondary");
    std::string folders;
    for (const std::string& f : b.folders) folders += (folders.empty() ? "" : ", ") + std::filesystem::path(f).filename().string();
    text("timeline_folders", folders.empty() ? "No folder given: add folders in the panel." : "Folders: " + folders,
         "secondary");
  }
#else
  text("timeline_help",
       "Export your timeline as FCPXML (DaVinci Resolve: File > Export > Timeline; Premiere Pro: File > Export > "
       "Final Cut Pro XML) and give its path. New versions are written beside it.",
       "secondary");
  w.begin_object().key("type").string("text_field").key("id").string("timeline_file").key("label")
      .string("Timeline file (FCPXML)").key("value").string(p.timeline_file).key("placeholder")
      .string("C:\\Projects\\Interview.fcpxml").end_object();
  for (std::size_t i = 0; i < p.folders.size(); ++i) {
    const std::string id = "folder_row:" + std::to_string(i);
    row(id.c_str());
    text(("folder:" + std::to_string(i)).c_str(), p.folders[i], "body");
    button(("folder_remove:" + std::to_string(i)).c_str(), "Remove");
    end();
  }
  w.begin_object().key("type").string("text_field").key("id").string("folder_add").key("label").string("Give a folder")
      .key("pick").string("folder").key("value").string("").end_object();
#endif
  end();

  if (!g->message.empty()) text("message", g->message, "secondary");
  w.end_array().end_object();  // root
  w.end_array().end_object();  // scroll
  return w.take();
}

void post() {
  uint64_t gen = 0;
  {
    std::lock_guard lock(g->m);
    if (!g->open) return;
    gen = g->generation;
  }
  const std::string t = tree();
  (void)g->host->screen_post(g->host->host, kScreen, gen, t.c_str());
}

void on_click(const std::string& id) {
  const std::string exe = platform::self_executable();
  if (id == "code_add" || id == "code_remove") {
    if (g->claude.empty()) check_clients();
    std::string out;
    int code = 1;
    const bool ran = !g->claude.empty() &&
                     platform::run(g->claude, id == "code_add" ? claude_code_add_args(exe) : claude_code_remove_args(),
                                   out, code, 30000);
    g->message = ran && code == 0 ? (id == "code_add" ? "Added to Claude Code. Start a new Claude Code session to use it."
                                                      : "Removed from Claude Code.")
                                  : "Claude Code did not take it: run the command below in a terminal.";
    check_clients();
  } else if (id == "desktop_add") {
    g->asking_desktop = true;
    g->message.clear();
  } else if (id == "desktop_cancel") {
    g->asking_desktop = false;
  } else if (id == "desktop_confirm" || id == "desktop_remove") {
    g->asking_desktop = false;
    const std::string path = platform::desktop_config_path();
    std::string old, updated, why;
    const bool had = platform::read_file(path, old);
    const bool ok = id == "desktop_confirm" ? desktop_config_with(had ? old : "", exe, updated, &why)
                                            : desktop_config_without(old, updated, &why);
    if (!ok) {
      g->message = why;
    } else {
      std::error_code ec;
      std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
      if (had) (void)platform::write_file_atomic(path + ".bak", old);
      g->message = platform::write_file_atomic(path, updated)
                       ? (id == "desktop_confirm" ? "Added to Claude Desktop. Quit and reopen Claude Desktop to use it."
                                                  : "Removed from Claude Desktop.")
                       : "Claude Desktop's settings file could not be written.";
    }
  } else if (id.rfind("folder_remove:", 0) == 0) {
    prefs p = load_prefs(prefs_path(g->host));
    const std::size_t i = static_cast<std::size_t>(std::strtoul(id.c_str() + 14, nullptr, 10));
    if (i < p.folders.size()) p.folders.erase(p.folders.begin() + static_cast<std::ptrdiff_t>(i));
    (void)save_prefs(prefs_path(g->host), p);
  }
}

void on_change(const std::string& id, const std::string& value_json) {
  const auto v = json::parse(value_json, 4);
  if (!v) return;
  prefs p = load_prefs(prefs_path(g->host));
  if (id == "frames" && v->k == json::kind::boolean) {
    p.frames = v->b;
  } else if (id == "timeline_file" && v->k == json::kind::string) {
    p.timeline_file = v->s;
  } else if (id == "folder_add" && v->k == json::kind::string && !v->s.empty()) {
    bool have = false;
    for (const std::string& f : p.folders) have = have || f == v->s;
    if (!have) p.folders.push_back(v->s);
  } else {
    return;
  }
  if (!save_prefs(prefs_path(g->host), p)) g->message = "The setting could not be saved.";
}

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  return guard([&] {
    if (!r || !r->screen_id || std::strcmp(r->screen_id, kScreen) != 0) return MV_ERR_INVALID_ARG;
    {
      std::lock_guard lock(g->m);
      g->generation = r->generation;
      g->open = true;
      g->message.clear();
    }
    post();  // at once, then again once the clients are checked
    check_clients();
    post();
    return MV_OK;
  });
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  (void)guard([&] {
    if (!e || !e->node_id) return MV_OK;
    if (e->kind == MV_SLOT_EVENT_COMMAND) {
      if (std::strcmp(e->node_id, "open") == 0) (void)g->host->screen_show(g->host->host, kScreen, 1);
      return MV_OK;
    }
    if (!e->screen_id || std::strcmp(e->screen_id, kScreen) != 0) return MV_OK;
    if (e->kind == MV_SLOT_EVENT_CLOSE) {
      std::lock_guard lock(g->m);
      g->open = false;
      g->asking_desktop = false;
      return MV_OK;
    }
    const std::string id = e->node_id;
    if (e->kind == MV_SLOT_EVENT_CLICK) on_click(id);
    if (e->kind == MV_SLOT_EVENT_CHANGE || e->kind == MV_SLOT_EVENT_SUBMIT) {
      on_change(id, e->value_json ? e->value_json : "");
    }
    post();
    return MV_OK;
  });
}

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, nullptr, nullptr, &screen_open, nullptr, &on_event};

// ---- mv.agent.1 --------------------------------------------------------------------------

mv_status MV_CALL serve(void*, const mv_agent_serve_args* a) {
  return guard([&] {
    if (!a || a->struct_size < sizeof(mv_agent_serve_args)) return MV_ERR_INVALID_ARG;
    {
      std::lock_guard lock(g->m);
      if (g->serving) return MV_ERR_BUSY;
      g->serving = true;
    }
    struct done {
      ~done() {
        std::lock_guard lock(g->m);
        g->serving = false;
      }
    } const finish;
    const prefs p = load_prefs(prefs_path(g->host));
    server_options o;
    o.bridge_dir = a->bridge_dir_utf8 && *a->bridge_dir_utf8 ? a->bridge_dir_utf8 : default_bridge_dir(g->host);
    if (a->timeline_utf8 && *a->timeline_utf8) {
      o.timeline_file = a->timeline_utf8;
    } else if (!p.timeline_file.empty()) {
      o.timeline_file = p.timeline_file;
    }
    for (uint32_t i = 0; i < a->folder_count && a->folders_utf8; ++i) {
      if (a->folders_utf8[i] && *a->folders_utf8[i]) o.folders.emplace_back(a->folders_utf8[i]);
    }
    for (const std::string& f : p.folders) o.folders.push_back(f);
    o.version = a->host_version && *a->host_version ? a->host_version : MV_AGENT_VERSION;
    o.handoff = (a->flags & MV_AGENT_SERVE_NO_HANDOFF) == 0;
    host_env env(g->host, a->services, p.frames);
    server s(env, std::move(o));
    env.log("serving");
    std::string pending, line;
    while (platform::read_line(a->in_fd, pending, line)) {
      const std::string reply = s.handle(line);
      if (reply.empty()) continue;
      if (!platform::write_all(a->out_fd, reply + "\n")) return MV_ERR_IO;
    }
    env.log("client closed");
    return MV_OK;
  });
}

const mv_agent_api kAgent{sizeof(mv_agent_api), 0, &serve};

const void* MV_CALL query(void*, const char* id) {
  if (!id) return nullptr;
  if (std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  if (std::strcmp(id, MV_AGENT_INTERFACE) == 0) return &kAgent;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  delete g;
  g = nullptr;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  return guard([&] {
    if (host_api < kHostApiMin || host_api > kHostApiMax || !host || !out ||
        host->struct_size < offsetof(mv_host_api, screen_show) + sizeof(host->screen_show) || !host->screen_post ||
        !host->screen_show) {
      return MV_ERR_UNSUPPORTED_FORMAT;
    }
    if (g) return MV_ERR_INVALID_ARG;  // one instance per process
    g = new addon_state();
    g->host = host;
    std::memset(out, 0, sizeof *out);
    out->struct_size = sizeof *out;
    out->id = "agent";
    out->version = MV_AGENT_VERSION;
    out->addon = g;
    out->shutdown = &shutdown;
    out->query = &query;
    return MV_OK;
  });
}
