// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// mv-nle-export: one Local search, written as FCPXML (plan/23 "Export results
// as FCPXML"). Both platforms: the reader loads the installed AI pack
// read-only, exactly as the search agent does, and the document imports into
// Final Cut Pro (File > Import > XML), DaVinci Resolve and Premiere Pro.
//
//   mv-nle-export "birthday cake" [--out results.fcpxml] [--scope-dir DIR]
//                 [--photos | --videos] [--max N] [--no-keyword] [--json]
//                 [--similar FILE [--at MS]]
//
// --json prints the rows instead (the agent's test client uses the same
// session, so this is also the headless check of Phase 1's top-K).
// Exit 0 with results, 1 with none, 2 on a usage or load error.
#include <cstdio>
#include <string>
#include <vector>

#include <mediaviewer/mediaviewer_ai.h>

#include "addon/manifest.h"
#include "addon/store.h"
#include "addons/fcp/fcpxml.h"
#include "addons/fcp/search_session.h"
#include "core/json.h"
#include "io/file.h"
#include "io/paths.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

int usage() {
  std::fputs("usage: mv-nle-export QUERY [--out FILE.fcpxml] [--scope-dir DIR] [--photos|--videos]\n"
             "                     [--max N] [--no-keyword] [--json] [--similar FILE [--at MS]]\n",
             stderr);
  return 2;
}

int run(const std::vector<std::string>& args) {
  std::string query, out, scope_dir, similar;
  std::int64_t at = -1;
  bool json = false, keyword = true;
  mv::nle::request req;
  req.correlation_id = 1;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    const bool more = i + 1 < args.size();
    if (a == "--out" && more) out = args[++i];
    else if (a == "--scope-dir" && more) scope_dir = args[++i];
    else if (a == "--photos") req.kinds = MV_AI_KIND_PHOTOS;
    else if (a == "--videos") req.kinds = MV_AI_KIND_VIDEOS;
    else if (a == "--max" && more) req.max_results = static_cast<std::uint32_t>(std::stoul(args[++i]));
    else if (a == "--no-keyword") keyword = false;
    else if (a == "--json") json = true;
    else if (a == "--similar" && more) similar = args[++i];
    else if (a == "--at" && more) at = std::stoll(args[++i]);
    else if (!a.empty() && a[0] != '-' && query.empty()) query = a;
    else return usage();
  }
  if (query.empty() && similar.empty()) return usage();
  if (!scope_dir.empty()) req.scope = MV_AI_SCOPE_TREE;
  if (!similar.empty()) {
    req.kind = mv::nle::request_kind::similar;
    req.pts_ms = at;
  }

  auto root = mv::io::addons_dir();
  auto thumbs = mv::io::thumb_cache_dir();
  if (!root || !thumbs) {
    std::fputs("no add-ons folder\n", stderr);
    return 2;
  }
  const auto key = mv::addon::pinned_public_key();
  mv::addon::store store(*root, std::vector<std::uint8_t>(key.begin(), key.end()), MV_ADDON_HOST_API);
  auto session = mv::nle::search_session::open(store, *thumbs);
  if (!session) {
    std::fprintf(stderr, "Local search cannot load: %s\n", mv::status_name(session.error()));
    return 2;
  }
  const mv::nle::reply r =
      (*session)->run(req, similar.empty() ? query : similar, scope_dir, 120000);
  if (r.code != mv::status::ok) {
    std::fprintf(stderr, "search failed: %s\n", mv::status_name(r.code));
    return 2;
  }
  if (json) {
    mv::json::writer w;
    w.begin_array();
    for (const mv::nle::row& x : r.rows) {
      w.begin_object();
      w.key("path").string(x.path);
      w.key("pts_ms").integer(x.pts_ms);
      w.key("score").number(x.score);
      w.key("kind").integer(x.kind);
      w.key("more").integer(x.more);
      w.key("duration_ms").integer(x.duration_ms);
      w.key("thumb").string(x.thumb);
      w.end_object();
    }
    w.end_array();
    std::fputs(w.str().c_str(), stdout);
    std::fputc('\n', stdout);
  } else {
    mv::nle::fcpxml_options o;
    o.event_name = "MediaViewer: " + (query.empty() ? std::string("similar") : query);
    if (keyword) o.keyword = "MV: " + (query.empty() ? std::string("similar") : query);
    const std::string doc = mv::nle::fcpxml(r.rows, o);
    if (out.empty()) {
      std::fputs(doc.c_str(), stdout);
    } else if (!mv::io::write_all(out, std::span<const std::uint8_t>(
                                           reinterpret_cast<const std::uint8_t*>(doc.data()), doc.size()))) {
      std::fputs("cannot write the output file\n", stderr);
      return 2;
    }
  }
  return r.rows.empty() ? 1 : 0;
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) {
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? static_cast<std::size_t>(n - 1) : 0, '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
    args.push_back(std::move(s));
  }
  ::SetConsoleOutputCP(CP_UTF8);
  return run(args);
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
