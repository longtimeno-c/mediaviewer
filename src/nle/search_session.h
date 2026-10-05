// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One search, start to finish, over the AI pack's mv.ai.1 table (docs/design/23):
// what the search agent runs for each request from Final Cut Pro, and what
// a Windows NLE bridge or the FCPXML export would run the same way. It asks
// the table the app's chrome asks, so a result here is the in-app result
// (same rows, same order, same scores); it only gathers each row's path,
// tile, clip length and matching moments into a search_wire reply.
//
// open() loads the installed pack through its reader door
// (MV_AI_READER_ENTRY_SYMBOL): verified like any add-on load, read-only, the
// text towers only. over() wraps a table the caller owns (tests).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include <mediaviewer/mediaviewer_ai.h>

#include "nle/search_wire.h"
#include "core/result.h"

namespace mv::addon {
class store;
class loaded_addon;
}  // namespace mv::addon

namespace mv::nle {

class search_session {
 public:
  ~search_session();
  search_session(const search_session&) = delete;
  search_session& operator=(const search_session&) = delete;

  // The installed "ai" pack, read-only. status::unsupported_format: the pack
  // predates the reader ("Local search needs an update"); status::corrupt: it
  // failed verification; status::not_found: Local search is not installed.
  [[nodiscard]] static result<std::unique_ptr<search_session>> open(const addon::store& s,
                                                                    std::string thumbs_dir);
  [[nodiscard]] static std::unique_ptr<search_session> over(const mv_ai_api* api);

  // Blocks until the pack has loaded its towers and vectors (or failed, or
  // `ms` passed). True when searches can answer.
  [[nodiscard]] bool wait_ready(int ms) const;

  // `text` is the query for request_kind::text and the file for ::similar.
  // Waits at most `timeout_ms` for the pack. [worker-thread]
  [[nodiscard]] reply run(const request& r, const std::string& text, const std::string& scope_dir,
                          int timeout_ms);

  // The remembered folders, as mv.ai.1 roots_json has them: the panel's scope
  // picker. status::busy while the pack is still loading. [worker-thread]
  [[nodiscard]] result<std::string> roots_json(int timeout_ms) const;
  // Named people for the word being typed (mv.ai.1 suggest_json), for the
  // panel's completions. [worker-thread]
  [[nodiscard]] result<std::string> suggest_json(const std::string& query, int timeout_ms) const;

  // The host's event sink: a finished search wakes run() at once.
  void on_event(const mv_addon_event& e);

 private:
  search_session() = default;
  const mv_ai_api* api_ = nullptr;
  std::unique_ptr<addon::loaded_addon> addon_;
  mutable std::mutex m_;
  mutable std::condition_variable cv_;
  std::set<std::uint64_t> done_;
};

}  // namespace mv::nle
