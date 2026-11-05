// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One-click Enhance (owner 2026-11-05, docs/design/29 "One click"): what the
// host does while Enhance runs on the photo being edited. The add-on's cards
// tree carries a hidden "run" node (addons/enhance/screens.h); the chrome reads
// its state, id and new file and hands them here (Mac
// mv_chrome_edit_enhance_run, Windows chrome_edit_action::enhance_run). This
// decides, once per change:
//
//   * the glow over the photo (shell/tool_overlay.h draw_work_glow): on while
//     the run works on the photo, off the moment it ends (a short fade);
//     never while the model is only downloading;
//   * the band the canvas leaves for the tool bar under the photo while a run
//     shows its line (working, then its result or its reason);
//   * opening the result: once per run, and only a run this host saw running
//     (a tree re-read later never opens it again).
//
// Portable, header-only, no I/O.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mv::shell {

enum class enhance_run_state : std::int32_t {
  idle = 0,
  running = 1,
  done = 2,
  failed = 3,
  cancelled = 4,
  downloading = 5,  // running, but only fetching the model: no glow
};

// The run node's label ("idle", "running", …) as the state; idle for anything else.
[[nodiscard]] inline enhance_run_state enhance_run_state_from(std::string_view label) noexcept {
  if (label == "running") return enhance_run_state::running;
  if (label == "downloading") return enhance_run_state::downloading;
  if (label == "done") return enhance_run_state::done;
  if (label == "failed") return enhance_run_state::failed;
  if (label == "cancelled") return enhance_run_state::cancelled;
  return enhance_run_state::idle;
}

struct enhance_run_report {
  enhance_run_state state = enhance_run_state::idle;
  std::uint64_t id = 0;
  std::string result;  // done: the new file's path, or a Photos-library key
};

// Windows' wire (the chrome parks it as a tree path): "<state>\t<id>\t<result>".
[[nodiscard]] inline bool parse_enhance_run(std::string_view wire, enhance_run_report& out) {
  const std::size_t a = wire.find('\t');
  if (a == std::string_view::npos) return false;
  const std::size_t b = wire.find('\t', a + 1);
  if (b == std::string_view::npos) return false;
  int state = 0;
  for (const char c : wire.substr(0, a)) {
    if (c < '0' || c > '9' || state > 100) return false;
    state = state * 10 + (c - '0');
  }
  if (a == 0 || state > static_cast<int>(enhance_run_state::downloading)) return false;
  std::uint64_t id = 0;
  for (const char c : wire.substr(a + 1, b - a - 1)) {
    if (c < '0' || c > '9' || id > (~0ull) / 10 - 10) return false;
    id = id * 10 + static_cast<std::uint64_t>(c - '0');
  }
  out.state = static_cast<enhance_run_state>(state);
  out.id = id;
  out.result = std::string(wire.substr(b + 1));
  return true;
}

// The glow's fade after the run ends (Reduce Motion: none).
inline constexpr std::uint64_t kWorkGlowFadeMs = 300;

struct enhance_run_follow {
  // The glow: since when it is on (0 off), and when it went off (0 never).
  std::uint64_t glow_on_ms = 0;
  std::uint64_t glow_off_ms = 0;
  // The tool bar's band under the photo: a run's line shows.
  bool band = false;
  std::uint64_t seen_running = 0;  // the newest run seen working (or downloading)
  std::uint64_t opened = 0;        // the newest run whose result was opened
  std::string opened_path;         // ... and that result

  struct step {
    bool changed = false;  // the glow or the band moved: publish
    std::string open;      // open this result now ("" none)
  };

  step update(const enhance_run_report& r, std::uint64_t now_ms) {
    step s;
    const bool working = r.state == enhance_run_state::running;
    const bool busy = working || r.state == enhance_run_state::downloading;
    if (busy && r.id != 0) seen_running = r.id;
    if (working && glow_on_ms == 0) {
      glow_on_ms = now_ms == 0 ? 1 : now_ms;
      glow_off_ms = 0;
      s.changed = true;
    } else if (!working && glow_on_ms != 0) {
      glow_on_ms = 0;
      glow_off_ms = now_ms == 0 ? 1 : now_ms;
      s.changed = true;
    }
    const bool want_band = r.state != enhance_run_state::idle;
    if (want_band != band) {
      band = want_band;
      s.changed = true;
    }
    if (r.state == enhance_run_state::done && !r.result.empty() && r.id != 0 && r.id == seen_running &&
        r.id != opened) {
      opened = r.id;
      opened_path = r.result;
      s.open = r.result;
    }
    return s;
  }

  // The photo went away under a run (another item, Edit closed, the add-on
  // gone): no glow and no band, and nothing opens. `next` the photo now on
  // screen: the result just opened keeps its line ("Saved as …").
  bool reset(std::uint64_t now_ms, const std::string& next = {}) {
    if (!next.empty() && next == opened_path) return false;
    const bool was = glow_on_ms != 0 || band;
    if (glow_on_ms != 0) glow_off_ms = now_ms == 0 ? 1 : now_ms;
    glow_on_ms = 0;
    band = false;
    opened = seen_running;
    return was;
  }
};

}  // namespace mv::shell
