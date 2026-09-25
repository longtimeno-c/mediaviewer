// SPDX-License-Identifier: GPL-2.0-or-later
// PR 10 — the host's edit state, shared by both hosts (plan/16 "Crop" mode,
// `[` `]` from the viewer; plan/07 EditStack).
//
// Pure and UI-thread only: commands in, an effect out; no I/O, no decode, no
// GPU. The host turns effects into work — a redraw, a debounced lossless
// write on the I/O pool, an export job — and reports completions back.
//
//   * One EditStack per (path, size, mtime), kept for the session, so walking
//     away and back keeps the edits. Not persisted: the XMP sidecar that is
//     "truth" for a stack (plan/07) lands with the PR 12 writer.
//   * `[` `]` `H` `V` on a JPEG whose stack holds only rotate/flip ask for a
//     lossless in-place write (edit::rotate_in_viewer + io::replace_atomic).
//     The preview turns at once; the write follows after the host's debounce,
//     one at a time. Turns pressed while a write is in flight are carried to
//     the rewritten file when it lands.
//   * Crop mode edits a draft (rect + straighten) over the whole straightened
//     frame; Enter pushes it onto the stack, Esc drops it.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/result.h"
#include "edit/edit_stack.h"
#include "edit/export.h"
#include "shell/commands.h"

namespace mv::shell {

struct edit_item {
  std::string path;  // UTF-8
  std::uint64_t size = 0;
  std::int64_t mtime = 0;
  std::uint32_t width = 0;   // what is displayed: the decoded, oriented image
  std::uint32_t height = 0;
  bool jpeg = false;         // magic-byte probe result, from the host
};

enum class edit_effect : std::uint8_t {
  none,            // not an edit command, or nothing changed
  redraw,          // the preview changed
  refused,         // cannot apply here (no still, a write failed); beep
  write_rotation,  // redraw, and start (or restart) the lossless-write debounce
  export_image,    // run an export of `export_geometry()` for the current item
};

struct rotation_write {
  std::string path;
  std::uint64_t size = 0;  // the bytes the op was made against
  codec::d4 op{};          // rotate/flip of what is displayed
};

class edit_session {
 public:
  // The item on the canvas changed (or was reloaded after a write). Leaves
  // crop mode unless it is the same bytes reopened (a folder relist). Returns
  // true when a carried rotation still needs a write.
  bool set_item(const edit_item& item);
  // The decoded size became known (or refined). Keeps everything else.
  void set_size(std::uint32_t width, std::uint32_t height) noexcept {
    item_.width = width;
    item_.height = height;
  }
  void clear_item() noexcept;
  [[nodiscard]] bool has_item() const noexcept { return has_item_; }

  // Edit commands only; anything else is `none`.
  [[nodiscard]] edit_effect run(command_id id);
  // Esc in crop mode (back_target::crop).
  void cancel_crop() noexcept;

  [[nodiscard]] bool crop_active() const noexcept { return crop_; }
  // What the canvas draws: the committed stack, or in crop mode the whole
  // straightened frame. Place it against the displayed size with
  // `preview_keeps_frame()` as place()'s keep_frame.
  [[nodiscard]] edit::geometry preview() const;
  [[nodiscard]] bool preview_keeps_frame() const noexcept { return crop_; }
  [[nodiscard]] edit::placement preview_placement() const;
  // Crop mode: the draft rect, normalised to the preview's output frame.
  [[nodiscard]] edit::rect crop_overlay() const noexcept { return draft_rect_; }
  [[nodiscard]] float crop_angle() const noexcept { return draft_angle_; }

  [[nodiscard]] edit::geometry export_geometry() const;
  [[nodiscard]] const edit::edit_stack* stack() const;

  // PR 11. The folded colour state of the current item (identity if none).
  [[nodiscard]] edit::colour colour() const;
  // Sets one parameter (clamped). `redraw` when the value changed.
  [[nodiscard]] edit_effect set_adjust(edit::adjust_param p, float value);
  // Back to identity colour, geometry untouched: one op, so one Ctrl+Z
  // brings every slider back.
  [[nodiscard]] edit_effect reset_adjust();

  // PR 12. A metadata write (rating, comment, orientation tag) landed on
  // `path`: the bytes changed (size, mtime), the pixels did not. Moves the
  // stack keyed by the old identity to the new one so a crop draft, colour
  // adjusts and pending turns survive the rewrite, and updates the current
  // item so a lossless rotation still checks against the bytes now on disk.
  // Unknown identity: nothing to move, nothing done.
  void metadata_rewritten(const std::string& path, std::uint64_t old_size, std::int64_t old_mtime,
                          std::uint64_t new_size, std::int64_t new_mtime);

  // Called when the host's debounce fires. Null while a write is in flight or
  // when there is nothing to write (the turns cancelled out, a crop joined).
  [[nodiscard]] std::optional<rotation_write> take_pending_write();
  // The write job finished. On success the host reloads the item; set_item
  // with the new size/mtime then carries any turns pressed meanwhile.
  void write_finished(bool ok);
  [[nodiscard]] bool write_in_flight() const noexcept { return in_flight_.has_value(); }

  static constexpr float kNudge = 0.01f;        // of the frame, per key press
  static constexpr float kMinCrop = 0.05f;
  static constexpr float kStraightenStep = 0.5f;  // degrees
  static constexpr std::size_t kCapacity = 64;    // stacks kept per session

 private:
  [[nodiscard]] std::uint64_t key() const noexcept;
  [[nodiscard]] std::uint64_t key_for(const std::string& path, std::uint64_t size,
                                      std::int64_t mtime) const noexcept;
  [[nodiscard]] edit::edit_stack& current();
  [[nodiscard]] edit::size2 frame() const noexcept;  // displayed size through the stack's turns
  [[nodiscard]] bool wants_write() const;
  [[nodiscard]] bool awaiting_disk() const noexcept;  // a write in flight, or landed but not reloaded
  edit_effect turn(edit::op_kind k);
  edit_effect nudge(float dx, float dy, float dw, float dh);
  void begin_crop();

  edit_item item_{};
  bool has_item_ = false;
  std::unordered_map<std::uint64_t, edit::edit_stack> stacks_;
  std::vector<std::uint64_t> order_;  // oldest first, for the capacity bound

  bool crop_ = false;
  edit::rect draft_rect_{};
  float draft_angle_ = 0.0f;
  bool draft_auto_ = false;  // the rect is still the angle's largest fit

  struct flight {
    std::string path;
    std::uint64_t key = 0;
    codec::d4 op{};
    bool landed = false;  // the rewritten file was opened before the job reported
  };
  std::optional<flight> in_flight_;
  struct carry {
    std::string path;
    std::uint64_t old_key = 0;  // the stack the write was taken from
    codec::d4 applied{};        // what is now baked into the file
  };
  std::optional<carry> carry_;
  bool write_failed_ = false;  // for the current item: stop asking
  // Bumped per path each time a rewrite lands, and part of the key: a quick
  // second rewrite can reproduce (size, mtime-in-seconds) exactly.
  std::unordered_map<std::string, std::uint32_t> epochs_;
  void land(std::uint64_t old_key, codec::d4 applied);
};

// ---- The export dialog's answer (plan/10 PR 10: WinUI dialog, SwiftUI sheet) --

// Both chromes post the choice back as one small integer (the island's
// command callback carries a float; every value here is exact in one):
//   bits 0-6  JPEG quality 1..100      bit 7   PNG (else JPEG)
//   bits 8-9  metadata_policy           bits 10-12 long-edge index
//                                        (kExportLongEdges; 0 = full size)
inline constexpr std::uint32_t kExportLongEdges[] = {0, 3840, 2560, 2048, 1600, 1080};
inline constexpr int kExportLongEdgeCount =
    static_cast<int>(sizeof(kExportLongEdges) / sizeof(kExportLongEdges[0]));

[[nodiscard]] std::int32_t pack_export(const edit::export_options& opt) noexcept;
[[nodiscard]] edit::export_options unpack_export(std::int32_t packed) noexcept;

// ---- Jobs the host runs on its I/O pool (worker threads only) ---------------

// Reads the file, checks it is still the bytes the op was made against,
// rotates losslessly and swaps the result in (io::replace_atomic).
[[nodiscard]] expected run_rotation_write(const rotation_write& w);

// Exports next to the original as "<name>-edit.jpg" (or " (2)", …: never an
// overwrite). Returns the path written. `c` is the PR 11 colour state.
[[nodiscard]] result<std::string> run_export(std::string_view source_path, const edit::geometry& g,
                                             const edit::export_options& opt,
                                             const edit::colour& c = {});

// PR 15, Ctrl+Alt+C / ⌘⌥C (plan/16 View): the still with its stack baked, as a
// PNG, for the clipboard. Written to io::clipboard_dir() — never beside the
// original — as "<name>-edit.png", replacing the previous flattened copy (the
// folder holds one file). Pixels + ICC only: nothing from the EXIF rides along
// into a chat window the user pasted into. `png` is the same bytes, for the
// clipboard's image flavour. Worker thread only.
// The PNG bytes alone, for a destination the caller owns (the Mac's
// drag-out file promise writes them where the drop asked). Worker thread only.
[[nodiscard]] result<std::vector<std::uint8_t>> render_flattened_png(std::string_view source_path,
                                                                     const edit::geometry& g,
                                                                     const edit::colour& c = {});
// "IMG_0001.HEIC" -> "IMG_0001-edit.png": the flattened copy's file name.
[[nodiscard]] std::string flattened_file_name(std::string_view source_path);

struct flattened_copy {
  std::string path;
  std::vector<std::uint8_t> png;
};
[[nodiscard]] result<flattened_copy> run_flatten(std::string_view source_path, const edit::geometry& g,
                                                 const edit::colour& c = {});

}  // namespace mv::shell
