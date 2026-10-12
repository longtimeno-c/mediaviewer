// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A test add-on that fills every screen and slot point (docs/design/25 PRs 58-59).
//
// Built as its own shared library (mv_slots_test_addon), loaded by the unit
// tests through the real loader (signed manifest, store, mv_addon_get, the v3
// host table) on every build that runs mv_import_tests (Windows, macOS, Linux
// CI). It links nothing of the core, like a real add-on: only the C header and
// the header-only JSON writer.
//
// What it answers, deterministically:
//   an info section (needs a location): a label ("Portugal" inside a
//     rough box around mainland Portugal, else "Elsewhere"), the host's own
//     coordinate text, and a 120 pt `surface` filled with the theme's
//     surface colour and a pin at its centre;
//   a search provider (files + local): files of the
//     folder inside that box when the query is part of "portugal"; a query
//     starting "slow" waits up to 2 s first (stopping early when stale);
//   badge "geotagged": every file of the folder with coordinates;
//   window "explorer": every node type of §10, with a 2,000-tile thumbnail grid;
//   items: a bar item and a path item that open it, a context-menu entry and a
//     status item it updates.
// "mv.slots_test.1" exposes what it saw, for the tests.
#include <mediaviewer/mediaviewer_addon.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"

namespace {

struct test_api {
  uint32_t struct_size;
  uint32_t reserved;
  // The last event's "kind node value" line, and counts of each call.
  void (*last_event)(char* out, uint32_t cap);
  uint64_t (*calls)(uint32_t which);  // 0 info, 1 search, 2 screen, 3 badges, 4 event
};

struct state {
  const mv_host_api* host = nullptr;
  std::mutex m;
  std::string last_event;
  std::atomic<uint64_t> calls[5]{};
  std::atomic<uint32_t> chosen{0};
  uint64_t explorer_generation = 0;
};

state* g = nullptr;

bool in_portugal(double lat, double lon) { return lat >= 36.9 && lat <= 42.2 && lon >= -9.6 && lon <= -6.1; }

mv_status MV_CALL info_section(void*, const mv_slot_info_request* r) {
  g->calls[0]++;
  if (!r || !r->location.has) return MV_OK;
  const bool pt = in_portugal(r->location.lat, r->location.lon);
  const std::string label = pt ? "Portugal" : "Elsewhere";
  mv::json::writer w;
  w.begin_object().key("type").string("stack").key("id").string("root").key("spacing").integer(4);
  w.key("children").begin_array();
  w.begin_object().key("type").string("text").key("id").string("label").key("text").string(label)
      .key("style").string("title").end_object();
  w.begin_object().key("type").string("text").key("id").string("coords").key("text").string(r->location_text)
      .key("style").string("secondary").key("copyable").boolean(true).end_object();
  w.begin_object().key("type").string("surface").key("id").string("pixels").key("surface").string("pixels")
      .key("height").integer(120).key("label").string("Test surface for " + label);
  w.key("pin").begin_object().key("x").number(0.5).key("y").number(0.5).end_object();
  w.key("pin_label").string(label).end_object();
  w.end_array().end_object();
  if (g->host->screen_post(g->host->host, r->section_id, r->generation, w.str().c_str()) != MV_OK) {
    return MV_OK;  // stale: another file is on screen now
  }
  const float scale = r->scale > 0 ? r->scale : 1.0f;
  const uint32_t width = std::clamp<uint32_t>(static_cast<uint32_t>((r->width_pt > 0 ? r->width_pt : 280) * scale), 16, 2048);
  const uint32_t height = static_cast<uint32_t>(120 * scale);
  std::vector<uint8_t> px(static_cast<size_t>(width) * height * 4);
  const uint32_t c = r->theme.surface;
  for (size_t i = 0; i < px.size(); i += 4) {
    px[i + 0] = static_cast<uint8_t>(c >> 8);   // B
    px[i + 1] = static_cast<uint8_t>(c >> 16);  // G
    px[i + 2] = static_cast<uint8_t>(c >> 24);  // R
    px[i + 3] = 0xFF;
  }
  (void)g->host->surface_post(g->host->host, r->section_id, "pixels", r->generation, px.data(), width, height,
                              width * 4);
  return MV_OK;
}

struct walk_ctx {
  std::vector<std::string> paths;
  bool portugal_only = false;
  uint32_t seen = 0;
};

int32_t MV_CALL collect(void* user, const mv_addon_location_entry* e) {
  auto* c = static_cast<walk_ctx*>(user);
  ++c->seen;
  if (e->location.has && (!c->portugal_only || in_portugal(e->location.lat, e->location.lon))) {
    c->paths.emplace_back(e->path_utf8);
  }
  return 1;
}

mv_status MV_CALL search(void*, const mv_slot_search_request* r) {
  g->calls[1]++;
  const std::string folded = r->folded_utf8 ? r->folded_utf8 : "";
  if (folded.rfind("slow", 0) == 0) {
    for (int i = 0; i < 200; ++i) {
      if (g->host->is_stale(g->host->host, r->generation)) return MV_ERR_CANCELLED;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  const std::string word = folded.rfind("slow", 0) == 0 ? folded.substr(4) : folded;
  std::vector<mv_slot_search_row> rows;
  walk_ctx ctx;
  ctx.portugal_only = true;
  if (!word.empty() && std::string("portugal").find(word) != std::string::npos && r->folder_utf8 &&
      *r->folder_utf8) {
    const mv_status st = g->host->locations_in(g->host->host, r->folder_utf8, r->generation, &collect, &ctx);
    if (st == MV_ERR_CANCELLED) return st;
    g->host->search_progress(g->host->host, r->provider_id, r->generation, ctx.seen, ctx.seen);
    for (const std::string& p : ctx.paths) rows.push_back({p.c_str(), "Portugal"});
  }
  return g->host->search_post(g->host->host, r->provider_id, r->generation, rows.data(),
                              static_cast<uint32_t>(rows.size()), 1);
}

mv_status MV_CALL badges(void*, const mv_slot_badges_request* r) {
  g->calls[3]++;
  walk_ctx ctx;
  const mv_status st = g->host->locations_in(g->host->host, r->folder_utf8, r->generation, &collect, &ctx);
  if (st != MV_OK) return st;
  std::vector<const char*> ptrs;
  for (const std::string& p : ctx.paths) ptrs.push_back(p.c_str());
  return g->host->badges_post(g->host->host, "geotagged", r->generation, ptrs.data(),
                              static_cast<uint32_t>(ptrs.size()), 1);
}

// The explorer window: one of every node type (§10).
std::string explorer_tree(const mv_slot_screen_request* r) {
  mv::json::writer w;
  auto leaf = [&w](const char* type, const char* id) {
    w.begin_object().key("type").string(type).key("id").string(id);
  };
  leaf("split", "root");
  w.key("axis").string("horizontal").key("fraction").number(0.3);
  w.key("children").begin_array();
  {
    leaf("scroll", "side");
    w.key("children").begin_array();
    leaf("stack", "controls");
    w.key("spacing").integer(8).key("children").begin_array();
    leaf("text", "heading");
    w.key("text").string("Explorer").key("style").string("heading").end_object();
    leaf("text", "state");
    w.key("text").string("Off").end_object();
    leaf("toggle", "switch");
    w.key("label").string("Show test rows").key("value").boolean(false).end_object();
    leaf("choice", "zoom");
    w.key("label").string("Zoom").key("value").string("region");
    w.key("options").begin_array();
    w.begin_object().key("id").string("country").key("text").string("Country").end_object();
    w.begin_object().key("id").string("region").key("text").string("Region").end_object();
    w.end_array().end_object();
    leaf("slider", "radius");
    w.key("label").string("Town radius").key("min").integer(1).key("max").integer(50).key("step").integer(1)
        .key("value").integer(15).end_object();
    leaf("number", "count");
    w.key("label").string("Results").key("min").integer(0).key("max").integer(100).key("value").integer(10)
        .end_object();
    leaf("text_field", "filter");
    w.key("label").string("Filter").key("placeholder").string("Type a place").key("value").string("")
        .end_object();
    leaf("progress", "reading");
    w.key("label").string("Reading tags").key("value").number(0.25).end_object();
    leaf("separator", "rule");
    w.end_object();
    leaf("button", "close");
    w.key("label").string("Close").key("style").string("primary").end_object();
    leaf("image", "logo");
    w.key("label").string("Logo").key("src").string("logo.png").key("width").integer(32).key("height")
        .integer(32).end_object();
    leaf("thumbnail", "current");
    w.key("label").string("The file on screen").key("path")
        .string(r->path_utf8 && *r->path_utf8 ? r->path_utf8 : "/fixtures/none.jpg").key("size").integer(96)
        .end_object();
    leaf("grid", "facts");
    w.key("columns").integer(2).key("children").begin_array();
    leaf("text", "k1");
    w.key("text").string("Files").end_object();
    leaf("text", "v1");
    w.key("text").string("2000").end_object();
    w.end_array().end_object();
    leaf("list", "rows");
    w.key("label").string("Test list").key("items").begin_array();
    w.begin_object().key("id").string("pt").key("text").string("Portugal").key("detail").string("12 files")
        .end_object();
    w.begin_object().key("id").string("es").key("text").string("Spain").end_object();
    w.end_array().end_object();
    leaf("table", "files");
    w.key("label").string("Files by place");
    w.key("columns").begin_array();
    w.begin_object().key("id").string("name").key("title").string("Name").end_object();
    w.begin_object().key("id").string("place").key("title").string("Place").end_object();
    w.end_array();
    w.key("rows").begin_array();
    w.begin_object().key("id").string("r1").key("cells").begin_array().string("IMG_0001.JPG").string("Sintra")
        .end_array().end_object();
    w.end_array().end_object();
    w.end_array().end_object();  // controls
    w.end_array().end_object();  // side
  }
  leaf("thumbnail_grid", "tiles");
  w.key("label").string("Photos").key("tile").integer(128).key("multi").boolean(true);
  w.key("items").begin_array();
  for (int i = 0; i < 2000; ++i) {
    char path[64];
    std::snprintf(path, sizeof path, "/fixtures/tiles/IMG_%04d.JPG", i);
    w.begin_object().key("path").string(path).end_object();
  }
  w.end_array().end_object();
  w.end_array().end_object();  // root
  return w.take();
}

mv_status MV_CALL screen_open(void*, const mv_slot_screen_request* r) {
  g->calls[2]++;
  {
    std::lock_guard lock(g->m);
    g->explorer_generation = r->generation;
  }
  const std::string tree = explorer_tree(r);
  return g->host->screen_post(g->host->host, r->screen_id, r->generation, tree.c_str());
}

void MV_CALL on_event(void*, const mv_slot_event* e) {
  g->calls[4]++;
  {
    std::lock_guard lock(g->m);
    g->last_event = std::to_string(e->kind) + " " + (e->node_id ? e->node_id : "") + " " +
                    (e->value_json ? e->value_json : "");
  }
  const std::string node = e->node_id ? e->node_id : "";
  const std::string value = e->value_json ? e->value_json : "";
  if (e->kind == MV_SLOT_EVENT_CHANGE && node == "switch") {
    const std::string diff = std::string(R"([{"op":"set","id":"state","props":{"text":")") +
                             (value == "true" ? "On" : "Off") + R"("}}])";
    (void)g->host->screen_diff(g->host->host, e->screen_id, e->generation, diff.c_str());
  } else if (e->kind == MV_SLOT_EVENT_CLICK && node == "close") {
    (void)g->host->screen_show(g->host->host, e->screen_id, 0);
  } else if (e->kind == MV_SLOT_EVENT_COMMAND && node == "mark") {
    // The paths ride along as a JSON array; count the strings.
    const uint32_t n = static_cast<uint32_t>(std::count(value.begin(), value.end(), '"') / 2);
    g->chosen += n;
    const std::string text = std::to_string(g->chosen.load()) + " chosen";
    (void)g->host->status_post(g->host->host, "count", text.c_str());
  }
}

void last_event(char* out, uint32_t cap) {
  std::lock_guard lock(g->m);
  if (!out || cap == 0) return;
  const size_t n = std::min<size_t>(cap - 1, g->last_event.size());
  std::memcpy(out, g->last_event.data(), n);
  out[n] = '\0';
}

uint64_t calls(uint32_t which) { return which < 5 ? g->calls[which].load() : 0; }

const mv_addon_slots kSlots{sizeof(mv_addon_slots), 0, &info_section, &search, &screen_open, &badges,
                            &on_event};
const test_api kTest{sizeof(test_api), 0, &last_event, &calls};

const void* MV_CALL query(void*, const char* id) {
  if (!id) return nullptr;
  if (std::strcmp(id, MV_SLOTS_INTERFACE) == 0) return &kSlots;
  if (std::strcmp(id, "mv.slots_test.1") == 0) return &kTest;
  return nullptr;
}

void MV_CALL shutdown(void*) {
  delete g;
  g = nullptr;
}

}  // namespace

extern "C" MV_ADDON_EXPORT mv_status MV_CALL mv_addon_get(uint32_t host_api, const mv_host_api* host,
                                                          mv_addon_api* out) {
  if (host_api < 3 || !host || !out || host->struct_size < sizeof(mv_host_api)) {
    return MV_ERR_UNSUPPORTED_FORMAT;
  }
  g = new state();
  g->host = host;
  std::memset(out, 0, sizeof *out);
  out->struct_size = sizeof *out;
  out->id = "slots-test";
  out->version = "1.0.0";
  out->addon = g;
  out->shutdown = &shutdown;
  out->query = &query;
  return MV_OK;
}
