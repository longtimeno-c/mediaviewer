// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 — the adjust pane's state, shared by both hosts (WinUI pane on
// Windows, SwiftUI pane on macOS). Pure and UI-thread only, like
// edit_session: events in, requests out; the host runs the jobs.
//
//   * The pane is for stills. It opens with Shift+A (docs/design/16: `E` is the
//     clip transport, so PR 11 does not take it) and focuses its first slider.
//   * docs/design/07: the sliders stay disabled — "Preparing…" — until the FP16
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

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "edit/adjust.h"
#include "edit/histogram.h"

namespace mv::shell {

// Slice 3: what the next click on the photo does while the pane is open.
enum class adjust_pick : std::int32_t {
  none = 0,
  white_balance = 1,  // one click: temperature and tint that make it neutral
  red_eye = 2,        // each click: a red-eye fix there, until turned off
  remove = 3,         // slice 7: drag to brush what Remove takes out, until turned off
};

// The Edit strip's wand menu: what it shows checked or enabled. Both chromes
// read these bits (mv_edit_view.wand, chrome_edit_args.wand).
inline constexpr std::int32_t kWandBrushOn = 1;       // the Remove brush paints on the photo
inline constexpr std::int32_t kWandBrushStrokes = 2;  // something is brushed
inline constexpr std::int32_t kWandBrushFull = 4;     // the brush holds all it can
inline constexpr std::int32_t kWandRestyle = 8;       // Enhance has its "restyle" command
inline constexpr std::int32_t kWandEyesBusy = 16;     // Open closed eyes is running
// docs/plans/edit-refresh.md S2: the Enhance tab's cards and the header's ✦ Auto.
inline constexpr std::int32_t kWandExtend = 32;       // Enhance can extend the picture (Remove's model spec)
inline constexpr std::int32_t kWandDetail = 64;       // Enhance offers "more detail" (its "open" command)
inline constexpr std::int32_t kWandFacesReady = 128;  // the AI pack is there: Open eyes / Improve faces can run
inline constexpr std::int32_t kWandAutoOn = 256;      // ✦ Auto is the last edit: a second click takes it back
inline constexpr std::int32_t kWandAutoBusy = 512;    // ✦ Auto is reading the photo

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
  std::int32_t auto_section = -1;  // edit::adjust_section whose Auto is running, -1 none
  // Slice 2: the four curves (master, red, green, blue), edit::curve's points.
  std::int32_t curve_n[edit::kCurveChannels] = {};
  float curve_x[edit::kCurveChannels][edit::kCurvePoints] = {};
  float curve_y[edit::kCurveChannels][edit::kCurvePoints] = {};
  // Slice 3: what a click on the photo does now (adjust_pick), and how many
  // red-eye fixes the edit holds.
  std::int32_t picking = 0;
  std::int32_t spot_count = 0;
  // Slice 7: the Remove brush's strokes, and whether it is full (kBrushDabs).
  std::int32_t brush_strokes = 0;
  std::int32_t brush_full = 0;
  // Slice 5b: what the last typed request did (edit::prompt_summary), UTF-8,
  // NUL-terminated, cut at a character boundary.
  char prompt_note[128] = {};
  std::uint16_t bins[4 * edit::kHistogramBins] = {};  // edit::pack_histogram
};

static_assert(sizeof(adjust_view) == 24 + 4 * edit::kAdjustParamCount + 16 +
                                         2 * 4 * edit::kCurveChannels * edit::kCurvePoints + 16 + 128 +
                                         2 * 4 * edit::kHistogramBins,
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

  // A section's Auto (#289 auto tone for Light; edit/auto_adjust.h). A token
  // when the working image is ready: the host runs edit::compute_auto on a
  // worker with the colour as it is now and, if auto_tone_landed accepts the
  // token, sets the result as one op (edit_session::set_colour). A colour
  // change, a new item, a dropped working image or another Auto in between
  // makes the token stale.
  [[nodiscard]] std::optional<std::uint64_t> request_auto_tone(
      edit::adjust_section section = edit::adjust_section::light) noexcept;
  // docs/plans/edit-refresh.md S2: several sections at once (edit::section_bit
  // mask; the host runs edit::compute_auto_sections), e.g. the header's ✦ Auto
  // (edit::kAutoEnhanceSections). Same token rules as one section's.
  [[nodiscard]] std::optional<std::uint64_t> request_auto(std::uint32_t sections) noexcept;
  bool auto_tone_landed(std::uint64_t token) noexcept;
  // The section of the Auto in flight (the view's auto_section). For several
  // sections at once: nullopt here, auto_sections() says which.
  [[nodiscard]] std::optional<edit::adjust_section> auto_running() const noexcept;
  // The mask of the Auto in flight, 0 = none.
  [[nodiscard]] std::uint32_t auto_sections() const noexcept { return auto_token_ != 0 ? auto_mask_ : 0u; }
  [[nodiscard]] bool auto_busy() const noexcept { return auto_token_ != 0; }

  // ✦ Auto (auto_enhance) is one undo step, and a second click takes it back:
  // the host records what the stack looked like when it landed; while the
  // stack still is exactly that (same op count, same colour), the next click
  // is an undo. Anything else (a slider, another op, a new item) and the next
  // click runs Auto again.
  void auto_enhance_landed(std::size_t edit_count, const edit::colour& c) noexcept;
  [[nodiscard]] bool auto_enhance_on(std::size_t edit_count, const edit::colour& c) const noexcept;

  // Slice 3: the pick mode (cleared with the item, the working image or the pane).
  void set_pick(adjust_pick p) noexcept { pick_ = readiness_ == adjust_readiness::ready ? p : adjust_pick::none; }
  [[nodiscard]] adjust_pick pick() const noexcept { return visible_ ? pick_ : adjust_pick::none; }

  // Slice 5b: the note under the request field; cleared with the item.
  void set_prompt_note(std::string note) { prompt_note_ = std::move(note); }

  // The POD the chrome draws, with the current slider values.
  [[nodiscard]] adjust_view view(const edit::colour& c) const noexcept;

 private:
  [[nodiscard]] std::optional<std::uint64_t> want(bool has_colour);

  bool visible_ = false;
  adjust_pick pick_ = adjust_pick::none;
  std::string prompt_note_;
  std::uint64_t item_ = 0;
  adjust_readiness readiness_ = adjust_readiness::none;
  bool from_raw_ = false;
  std::uint64_t token_ = 0;        // the build in flight or landed, for item_
  std::uint64_t next_token_ = 1;
  bool histogram_stale_ = true;
  std::uint64_t histogram_token_ = 0;
  std::uint64_t auto_token_ = 0;   // the Auto job in flight, 0 = none
  edit::adjust_section auto_section_ = edit::adjust_section::light;
  std::uint32_t auto_mask_ = 0;     // the sections of the Auto in flight
  bool auto_mark_ = false;          // ✦ Auto landed at auto_mark_count_ / auto_mark_colour_
  std::size_t auto_mark_count_ = 0;
  edit::colour auto_mark_colour_{};
  bool histogram_valid_ = false;
  float clip_high_ = 0.0f, clip_low_ = 0.0f;
  std::uint16_t bins_[4 * edit::kHistogramBins] = {};
};

}  // namespace mv::shell
