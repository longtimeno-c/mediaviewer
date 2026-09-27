// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 — the adjust pane's state, shared by both hosts (WinUI pane on
// Windows, SwiftUI pane on macOS). Pure and UI-thread only, like
// edit_session: events in, requests out; the host runs the jobs.
//
//   * The pane is for stills. It opens with Shift+A (plan/16: `E` is the
//     clip transport, so PR 11 does not take it) and focuses its first slider.
//   * plan/07: the sliders stay disabled — "Preparing…" — until the FP16
//     working image (image/linear.h) has been built from the real data. For a
//     RAW that is LibRaw's full linear develop, never the embedded preview.
//   * The working image is wanted while the pane is open *or* the item on the
//     canvas has colour ops: the canvas draws adjusted pixels from it with the
//     pane closed too. Until it lands the blit runs the same kernel on the
//     8-bit viewer texture, so walking back to an adjusted photo never shows
//     it unadjusted.
//   * Every request carries a token; a result for an item the user has left
//     is dropped, never shown on the wrong photo.
#pragma once

#include <cstdint>
#include <optional>

#include "edit/adjust.h"
#include "edit/histogram.h"

namespace mv::shell {

enum class adjust_readiness : std::int32_t {
  none = 0,       // no still on the canvas (or a clip): the pane says so
  preparing = 1,  // working image building: sliders disabled
  ready = 2,      // sliders live
  failed = 3,     // the working image could not be built (corrupt, too big)
};

// What both chromes draw, as one POD. Mirrored field for field by the C#
// AdjustViewArgs (IslandHost.Panels.cs) and mv_adjust_view
// (mv_chrome_bridge.h); sizeof is asserted on all three sides.
struct adjust_view {
  std::int32_t readiness = 0;   // adjust_readiness
  std::int32_t from_raw = 0;    // the working data is a RAW's linear develop
  float values[edit::kAdjustParamCount] = {};
  float clip_high = 0.0f;       // fraction of shown pixels, 0..1
  float clip_low = 0.0f;
  std::int32_t histogram_valid = 0;
  std::int32_t reserved = 0;
  std::uint16_t bins[4 * edit::kHistogramBins] = {};  // edit::pack_histogram
};

static_assert(sizeof(adjust_view) == 44 + 2 * 4 * edit::kHistogramBins,
              "keep in sync with AdjustViewArgs and mv_adjust_view");

class adjust_pane {
 public:
  // The still on the canvas changed (0 = none, or a clip). `has_colour`:
  // its stack holds colour ops. Returns a build token when the working image
  // should be built for it now.
  [[nodiscard]] std::optional<std::uint64_t> set_item(std::uint64_t item, bool has_colour);

  // Shift+A / the pane's close button. Returns a build token when opening
  // needs a working image that is not there yet.
  [[nodiscard]] std::optional<std::uint64_t> toggle(bool has_colour);
  [[nodiscard]] std::optional<std::uint64_t> show(bool visible, bool has_colour);
  [[nodiscard]] bool visible() const noexcept { return visible_; }

  // The colour of the current item changed (a slider, undo, reset). Returns a
  // build token if colour on a closed pane now needs the working image.
  [[nodiscard]] std::optional<std::uint64_t> colour_changed(bool has_colour);

  // The build job for `token` finished. False when the token is stale (the
  // host drops the texture it made).
  bool working_landed(std::uint64_t token, bool ok, bool from_raw) noexcept;
  // The working image was released (memory, item change): a later want
  // rebuilds it.
  void working_dropped() noexcept;
  [[nodiscard]] adjust_readiness readiness() const noexcept { return readiness_; }
  [[nodiscard]] bool working_ready() const noexcept { return readiness_ == adjust_readiness::ready; }

  // Histogram requests: the host debounces, then asks. A token when the
  // working image is there and the numbers are stale.
  void histogram_dirty() noexcept { histogram_stale_ = true; }
  [[nodiscard]] std::optional<std::uint64_t> take_histogram_request() noexcept;
  bool histogram_landed(std::uint64_t token, const edit::histogram& h) noexcept;

  // The POD the chrome draws, with the current slider values.
  [[nodiscard]] adjust_view view(const edit::colour& c) const noexcept;

 private:
  [[nodiscard]] std::optional<std::uint64_t> want(bool has_colour);

  bool visible_ = false;
  std::uint64_t item_ = 0;
  adjust_readiness readiness_ = adjust_readiness::none;
  bool from_raw_ = false;
  std::uint64_t token_ = 0;        // the build in flight or landed, for item_
  std::uint64_t next_token_ = 1;
  bool histogram_stale_ = true;
  std::uint64_t histogram_token_ = 0;
  bool histogram_valid_ = false;
  float clip_high_ = 0.0f, clip_low_ = 0.0f;
  std::uint16_t bins_[4 * edit::kHistogramBins] = {};
};

}  // namespace mv::shell
