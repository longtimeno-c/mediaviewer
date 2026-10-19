// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Screens (docs/design/25 §10, PR 58): a view tree an add-on posts as data and both
// hosts render with their own controls.
//
// The add-on posts a whole tree, then diffs. The core keeps the authoritative
// copy, checks every tree and every diff against the vocabulary below, and
// hands each chrome one canonical JSON snapshot to build from in one pass on
// its UI thread. A tree that is not valid is refused whole: the chrome never
// sees half of one.
//
// Accessibility is enforced here, not hoped for: every control, image,
// thumbnail, surface and progress node must carry a `label`, so VoiceOver and
// Narrator name each one, and every control the hosts build from it is
// focusable (keyboard-complete by construction).
//
// Events come back from the chrome through check_event, which refuses an event
// for a node that does not exist or is not interactive, and a value of the
// wrong kind or out of range: an add-on is never handed a value its own tree
// could not produce. Portable, no I/O (D9).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/json.h"

namespace mv::addon::screen {

enum class node_type : std::uint8_t {
  stack,
  grid,
  scroll,
  split,
  text,
  image,
  thumbnail,
  thumbnail_grid,
  list,
  table,
  button,
  toggle,
  choice,
  slider,
  number,
  text_field,
  progress,
  separator,
  surface,
  // #442 (Driftwire's AirDrop-like window): large round tiles with a name, a
  // caption, initials, a progress ring and a done / failed mark; a click or
  // Return on one is `activate`.
  tiles,
  // 2026-10-19 (Driftwire): a picture of a transfer's path. Two computers
  // and the line between them, a badge for the bearer (Bluetooth, Wi-Fi,
  // internet direct, internet met on the DHT, through one of the person's
  // computers, through their relay server), and dots that move along it
  // while bytes do. The host animates it only in state "moving" or
  // "connecting", and never with Reduce Motion / animations off. Not
  // interactive.
  link,
};

[[nodiscard]] const char* type_name(node_type t) noexcept;
[[nodiscard]] std::optional<node_type> type_from(std::string_view name) noexcept;
// A control the user can act on (focusable, sends events).
[[nodiscard]] bool interactive(node_type t) noexcept;

// Bounds. A tree past any of them is refused (`too_large`).
inline constexpr std::size_t kMaxNodes = 2048;
inline constexpr int kMaxDepth = 24;
inline constexpr std::size_t kMaxItems = 100'000;  // all list / table / grid rows together
inline constexpr std::size_t kMaxJsonBytes = 8u * 1024 * 1024;
inline constexpr std::size_t kMaxLabel = 200;
inline constexpr std::size_t kMaxText = 4096;

struct node {
  node_type type = node_type::stack;
  std::string id;          // unique in the tree: [A-Za-z0-9_.:-]{1,64}
  json::value props;       // an object; only the keys the type allows
  std::vector<node> children;
};

enum class error : std::uint8_t {
  none = 0,
  not_json,         // not JSON, or nested past the parser's limit
  bad_node,         // not an object, unknown type, missing or bad id
  bad_prop,         // a key the type does not allow, or a value of the wrong kind / range
  missing_label,    // a control or picture with no label (VoiceOver / Narrator)
  duplicate_id,
  bad_children,     // children on a leaf, or a split without exactly two
  too_large,
  no_such_node,     // a diff or event names an id the tree does not have
  bad_op,           // a diff entry that is not set / replace / insert / remove, or removes the root
  not_interactive,  // an event for a node that sends none
  bad_value,        // an event value of the wrong kind or out of range
};

[[nodiscard]] const char* error_name(error e) noexcept;

class tree {
 public:
  // Parses and checks a whole tree. On error the tree is unchanged.
  error set(std::string_view json_text);
  // Applies a diff, atomically: every entry or none.
  //   [{"op":"set","id":X,"props":{...}}            merge; a null removes the key
  //    {"op":"replace","id":X,"node":{...}}
  //    {"op":"insert","parent":P,"index":i,"node":{...}}   i = -1 appends
  //    {"op":"remove","id":X}]                       never the root
  error apply(std::string_view diff_json);

  [[nodiscard]] bool empty() const noexcept { return !root_; }
  [[nodiscard]] const node* root() const noexcept { return root_ ? &*root_ : nullptr; }
  [[nodiscard]] const node* find(std::string_view id) const noexcept;
  // Moves on every successful set or apply.
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  // The canonical JSON the chromes read: {"type","id",...props...,"children":[...]}.
  [[nodiscard]] std::string json() const;
  // Every surface id the tree names (so the registry can drop pixels no node shows).
  [[nodiscard]] std::vector<std::string> surface_ids() const;

  // Checks an event the chrome reports and, when it is a value change, keeps
  // the tree in step (a toggle stays toggled while the add-on decides). `kind`
  // is event_kind below; `value` is a JSON value ("true", "0.5", "\"a\"",
  // "[\"id\"]"), or empty for a click.
  error check_event(std::string_view node_id, std::uint32_t kind, std::string_view value);

  void clear() noexcept {
    root_.reset();
    ++revision_;
  }

 private:
  std::optional<node> root_;
  std::uint64_t revision_ = 0;
};

// Event kinds, shared with mediaviewer_addon.h (MV_SLOT_EVENT_*).
enum event_kind : std::uint32_t {
  ev_click = 1,     // button
  ev_change = 2,    // toggle, choice, slider, number, text_field (value)
  ev_submit = 3,    // text_field Return
  ev_select = 4,    // list, table, thumbnail_grid, tiles: selection (array of ids / paths)
  ev_activate = 5,  // list, table, thumbnail_grid: Return / double-click on one; tiles: a click (string)
  ev_open = 7,      // a window, pane or sheet opened (host-generated)
  ev_close = 8,     // it closed (host-generated)
  ev_command = 9,   // a bar / path / context / status item (host-generated)
  // An "interactive" surface (owner, 2026-10-12: zoom the map from the trackpad or mouse):
  ev_zoom = 10,     // {"scale": factor 0.05..20, "x": 0..1, "y": 0..1} -- the anchor, in the surface
  ev_pan = 11,      // {"dx": -10..10, "dy": -10..10} -- the drag, in surface widths / heights
  ev_double = 12,   // {"x": 0..1, "y": 0..1} -- a double-click
};

// One node, checked: used by `tree` and exposed for tests.
[[nodiscard]] error parse_node(const json::value& v, node& out, int depth, std::size_t& nodes,
                               std::size_t& items);
// A JSON value written back out (the canonical form the chromes parse).
void write_value(json::writer& w, const json::value& v);

}  // namespace mv::addon::screen
