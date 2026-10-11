// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/keymap.h"

#include <algorithm>
#include <iterator>

#include "core/json.h"

namespace mv::shell {
namespace {

using enum command_id;

struct slug_row {
  command_id id;
  std::string_view slug;
};

// Every command but none, the retired palette and the add-on slots, by its
// enumerator's name. A pack names commands by these, so they never change;
// a new command appends a row (tests/test_keymap.cpp checks every one).
constexpr slug_row kSlugs[] = {
    {open, "open"},
    {fit, "fit"},
    {one_to_one, "one_to_one"},
    {zoom_in, "zoom_in"},
    {zoom_out, "zoom_out"},
    {zoom_preset, "zoom_preset"},
    {overlay, "overlay"},
    {select_item, "select_item"},
    {prev, "prev"},
    {next, "next"},
    {open_folder, "open_folder"},
    {toggle_gallery, "toggle_gallery"},
    {close_gallery, "close_gallery"},
    {gallery_activate, "gallery_activate"},
    {toggle_filmstrip, "toggle_filmstrip"},
    {back, "back"},
    {first, "first"},
    {last, "last"},
    {skip_back, "skip_back"},
    {skip_forward, "skip_forward"},
    {play_pause, "play_pause"},
    {jump_back, "jump_back"},
    {pause, "pause"},
    {jump_forward, "jump_forward"},
    {frame_back, "frame_back"},
    {frame_forward, "frame_forward"},
    {rate_down, "rate_down"},
    {rate_up, "rate_up"},
    {skim_back, "skim_back"},
    {skim_forward, "skim_forward"},
    {skim_settle, "skim_settle"},
    {reset_stats, "reset_stats"},
    {close_window, "close_window"},
    {zoom_200, "zoom_200"},
    {zoom_400, "zoom_400"},
    {fullscreen, "fullscreen"},
    {fill, "fill"},
    {reset_view, "reset_view"},
    {cycle_background, "cycle_background"},
    {sticky_zoom, "sticky_zoom"},
    {clipping, "clipping"},
    {loupe, "loupe"},
    {loupe_release, "loupe_release"},
    {hold_previous, "hold_previous"},
    {hold_previous_release, "hold_previous_release"},
    {always_on_top, "always_on_top"},
    {info_overlay, "info_overlay"},
    {pan_up, "pan_up"},
    {pan_down, "pan_down"},
    {pan_left, "pan_left"},
    {pan_right, "pan_right"},
    {loupe_nudge_left, "loupe_nudge_left"},
    {loupe_nudge_right, "loupe_nudge_right"},
    {loupe_nudge_up, "loupe_nudge_up"},
    {loupe_nudge_down, "loupe_nudge_down"},
    {toggle_mark, "toggle_mark"},
    {mark_all, "mark_all"},
    {unmark_all, "unmark_all"},
    {copy_to, "copy_to"},
    {copy_to_pick, "copy_to_pick"},
    {move_to, "move_to"},
    {move_to_pick, "move_to_pick"},
    {delete_to_recycle_bin, "delete_to_recycle_bin"},
    {slideshow_start, "slideshow_start"},
    {slideshow_pause, "slideshow_pause"},
    {slideshow_faster, "slideshow_faster"},
    {slideshow_slower, "slideshow_slower"},
    {blackout, "blackout"},
    {shuffle, "shuffle"},
    {help, "help"},
    {go_to, "go_to"},
    {folder_tree, "folder_tree"},
    {typeahead, "typeahead"},
    {reveal_in_explorer, "reveal_in_explorer"},
    {gallery_up, "gallery_up"},
    {gallery_down, "gallery_down"},
    {gallery_open_selected, "gallery_open_selected"},
    {open_settings, "open_settings"},
    {gallery_larger, "gallery_larger"},
    {gallery_smaller, "gallery_smaller"},
    {play_motion, "play_motion"},
    {open_raw, "open_raw"},
    {open_jpeg, "open_jpeg"},
    {mute, "mute"},
    {game_toggle_3d, "game_toggle_3d"},
    {metadata_pane, "metadata_pane"},
    {af_points, "af_points"},
    {eyedropper, "eyedropper"},
    {copy_clipboard, "copy_clipboard"},
    {rotate_ccw, "rotate_ccw"},
    {rotate_cw, "rotate_cw"},
    {flip_horizontal, "flip_horizontal"},
    {flip_vertical, "flip_vertical"},
    {crop_mode, "crop_mode"},
    {crop_commit, "crop_commit"},
    {crop_move_left, "crop_move_left"},
    {crop_move_right, "crop_move_right"},
    {crop_move_up, "crop_move_up"},
    {crop_move_down, "crop_move_down"},
    {crop_narrower, "crop_narrower"},
    {crop_wider, "crop_wider"},
    {crop_shorter, "crop_shorter"},
    {crop_taller, "crop_taller"},
    {straighten_ccw, "straighten_ccw"},
    {straighten_cw, "straighten_cw"},
    {export_image, "export_image"},
    {undo_edit, "undo_edit"},
    {reset_edits, "reset_edits"},
    {folder_up, "folder_up"},
    {folder_prev, "folder_prev"},
    {folder_next, "folder_next"},
    {adjust_pane, "adjust_pane"},
    {adjust_exposure, "adjust_exposure"},
    {adjust_contrast, "adjust_contrast"},
    {adjust_saturation, "adjust_saturation"},
    {adjust_temperature, "adjust_temperature"},
    {adjust_tint, "adjust_tint"},
    {adjust_reset, "adjust_reset"},
    {open_import, "open_import"},
    {import_now, "import_now"},
    {set_rating_0, "set_rating_0"},
    {set_rating_1, "set_rating_1"},
    {set_rating_2, "set_rating_2"},
    {set_rating_3, "set_rating_3"},
    {set_rating_4, "set_rating_4"},
    {set_rating_5, "set_rating_5"},
    {edit_comment, "edit_comment"},
    {trim_mode, "trim_mode"},
    {trim_in, "trim_in"},
    {trim_out, "trim_out"},
    {trim_clear, "trim_clear"},
    {trim_preview, "trim_preview"},
    {trim_keyframe, "trim_keyframe"},
    {trim_reencode, "trim_reencode"},
    {keyframe_prev, "keyframe_prev"},
    {keyframe_next, "keyframe_next"},
    {jobs_pane, "jobs_pane"},
    {clip_tools, "clip_tools"},
    {clip_split, "clip_split"},
    {trim_remove_middle, "trim_remove_middle"},
    {copy_path, "copy_path"},
    {copy_flattened, "copy_flattened"},
    {share, "share"},
    {edit_workspace, "edit_workspace"},
    {crop_aspect_cycle, "crop_aspect_cycle"},
    {crop_aspect_swap, "crop_aspect_swap"},
    {show_original, "show_original"},
    {show_original_release, "show_original_release"},
    {crop_aspect_set, "crop_aspect_set"},
    {crop_straighten_set, "crop_straighten_set"},
    {search_open, "search_open"},
    {search_similar, "search_similar"},
    {search_next_match, "search_next_match"},
    {search_prev_match, "search_prev_match"},
    {next_page, "next_page"},
    {prev_page, "prev_page"},
    {new_window, "new_window"},
};

struct claim {
  int row;
  key k;
  std::uint8_t mods;
  mode_mask modes;
};

bool same_key(key a, std::uint8_t am, key b, std::uint8_t bm) noexcept {
  return a != key::none && a == b && am == bm;
}

// The live rows a binding's command owns, in table order: the built-in rows
// for a stable name, the contributed slot's row for "<add-on>/<command>".
std::vector<int> rows_of(std::string_view name, std::span<const binding> live) {
  std::vector<int> rows;
  command_id id = none;
  if (const auto slash = name.find('/'); slash != std::string_view::npos) {
    const std::string_view addon = name.substr(0, slash);
    const std::string_view cmd = name.substr(slash + 1);
    for (int n = 0; n < kAddonCommandSlots; ++n) {
      const auto slot = static_cast<command_id>(static_cast<int>(addon_cmd_0) + n);
      const addon_command_row* r = addon_command(slot);
      if (r && r->addon == addon && r->id == cmd) id = slot;
    }
  } else {
    id = command_by_slug(name);
  }
  if (id == none) return rows;
  for (std::size_t i = 0; i < live.size(); ++i) {
    if (live[i].command == id) rows.push_back(static_cast<int>(i));
  }
  return rows;
}

}  // namespace

std::string_view command_slug(command_id id) noexcept {
  for (const slug_row& r : kSlugs) {
    if (r.id == id) return r.slug;
  }
  return {};
}

command_id command_by_slug(std::string_view slug) noexcept {
  for (const slug_row& r : kSlugs) {
    if (r.slug == slug) return r.id;
  }
  return none;
}

keymap_result apply_keymap(std::span<const addon::keymap_binding> bindings) {
  keymap_result out;
  reset_live_bindings();
  const std::span<const binding> live = live_bindings();

  // 1. What the pack asks for, row by row, first claim wins.
  std::vector<claim> claims;
  for (const addon::keymap_binding& b : bindings) {
    const std::vector<int> rows = rows_of(b.command, live);
    if (rows.empty()) {
      out.skipped.push_back("unknown command: " + b.command);
      continue;
    }
    for (std::size_t j = 0; j < b.keys.size(); ++j) {
      if (j >= rows.size()) {
        out.skipped.push_back(b.command + " has " + std::to_string(rows.size()) +
                              (rows.size() == 1 ? " key" : " keys") + "; extra key " + b.keys[j]);
        continue;
      }
      const int row = rows[j];
      claim c{row, key::none, mod_none, live[static_cast<std::size_t>(row)].modes};
      if (!b.keys[j].empty() && !parse_key_label(b.keys[j], c.k, c.mods)) {
        out.skipped.push_back("not a key: " + b.keys[j]);
        continue;
      }
      const auto taken = std::find_if(claims.begin(), claims.end(), [&](const claim& o) {
        return same_key(o.k, o.mods, c.k, c.mods) && (o.modes & c.modes) != 0;
      });
      if (taken != claims.end()) {
        const std::string_view owner = command_slug(live[static_cast<std::size_t>(taken->row)].command);
        out.skipped.push_back(b.keys[j] + " is already given to " +
                              (owner.empty() ? std::string("another command") : std::string(owner)));
        continue;
      }
      claims.push_back(c);
    }
  }

  // 2. Write them, and take each claimed key from any other row that holds
  // it in an overlapping mode.
  for (const claim& c : claims) {
    const binding& before = live[static_cast<std::size_t>(c.row)];
    if (before.k != c.k || before.mods != c.mods) ++out.rows_changed;
    (void)set_live_key(c.row, c.k, c.mods);
  }
  for (std::size_t i = 0; i < live.size(); ++i) {
    const binding& b = live[i];
    if (b.k == key::none) continue;
    const bool claimed_here = std::any_of(claims.begin(), claims.end(),
                                          [&](const claim& c) { return c.row == static_cast<int>(i); });
    if (claimed_here) continue;
    const bool clash = std::any_of(claims.begin(), claims.end(), [&](const claim& c) {
      return same_key(c.k, c.mods, b.k, b.mods) && (c.modes & b.modes) != 0;
    });
    if (clash) {
      (void)set_live_key(static_cast<int>(i), key::none, mod_none);
      ++out.displaced;
    }
  }
  return out;
}

bool keymap_bindings_from_json(std::string_view text, std::vector<addon::keymap_binding>& out) {
  // The pack was held to the manifest's rules (addon/keymap.cpp) when its
  // add-on was checked; this only reads it back, still bounded. The shell
  // links no add-on code on Windows (the core DLL owns it), so the reading
  // is here rather than through addon::parse_keymaps.
  out.clear();
  if (text.size() > (256u << 10)) return false;
  const auto doc = json::parse(text, 8);
  if (!doc || doc->k != json::kind::object) return false;
  const json::value* bindings = doc->find("bindings");
  if (!bindings || bindings->k != json::kind::array ||
      bindings->a.size() > addon::kKeymapMaxBindings) {
    return false;
  }
  for (const json::value& b : bindings->a) {
    const std::string* command = b.str("command");
    const json::value* keys = b.find("keys");
    if (!command || command->empty() || command->size() > 100 || !keys ||
        keys->k != json::kind::array || keys->a.size() > addon::kKeymapMaxKeys) {
      return false;
    }
    addon::keymap_binding kb;
    kb.command = *command;
    for (const json::value& k : keys->a) {
      if (k.k != json::kind::string || k.s.size() > 32) return false;
      kb.keys.push_back(k.s);
    }
    out.push_back(std::move(kb));
  }
  return true;
}

}  // namespace mv::shell
