# 20 — The Edit workspace (one visible way in to crop, colour, metadata and trim)

**Status: planned 2026-09-26 from issue #39. Standalone PR 29, both platforms (D9). Phase 1 is
written and run on Mac (the `MV_EDIT_SELFTEST` rig); the Windows half is owed. Revised the same
day on the owner's review (below: "Owner review").** It adds no new edit engine. It puts a visible door and one pane in
front of what PRs 10–14 already built, so that a first-time user can find crop without reading
`?`.

## The problem, measured against the tree

Before this change, **neither host had a visible control for any edit**. Crop (`Shift+C`),
rotate/flip (`[` `]` `H` `V`), straighten (`,` `.` in crop), undo (`Ctrl+Z`), reset (`Ctrl+R`),
export (`Ctrl+S`), the Adjust pane (`Shift+A`), trim (`Ctrl+T`) and the clip tools (`Ctrl+S` on
a clip) could only be reached from the keyboard or the `?` sheet:

- Crop was an ImGui overlay with keyboard nudges only. It had no aspect presets and no
  straighten control.
- Adjust, Metadata and Jobs were three separate, mutually exclusive right-edge panes, and export
  and the clip tools were two separate sheets.
- On Windows, the `TrimMode` and `ClipToolsFlyout` chrome ids were defined but nothing sent them.

The engine was all there. Nothing on screen led to it.

## Decisions

1. **The workspace is a base feature, not an add-on.** Crop, rotate, colour and the metadata
   writes are PR 10–12 base features, their engines live in the core (`edit/`,
   `shell/edit_session`), and they are small. Making the workspace installable would mean either
   duplicating those engines in an add-on or hiding base features behind a download. The
   *advanced* video/audio editor is the add-on ([21](21-editor-addon.md), issue #40). It docks
   into this workspace and does not add a second entry point.
2. **One door, two labels.** The command bar has **Edit image** on a still and **Edit video** on
   a clip. The button is disabled with no item. The key is **`Enter`** (`Return` on Mac), which
   was unbound in browse and video modes and is the Photos convention. Inside the workspace,
   `Enter` keeps its existing meaning: it applies the crop and saves the trim. The Mac menu bar
   gets an **Edit** menu listing the same commands with their keys.
3. **One pane, tabs, reusing what exists — docked, not floating.** The workspace is a strip at
   the top of the right pane column (title, tabs, actions) over one tab's pane. Unlike the PR 9
   panes it **docks**: the canvas frames the picture in the rect beside it
   (`input_snapshot.chrome_right_px`, the blit's `origin_x`), so the pane never covers the image.
   Docking is a refit, not a swapchain resize. The Colour, Info and Jobs tabs *are*
   the existing Adjust, Metadata and Jobs panes. Crop and Trim get new panes that drive the
   existing commands. No pane logic is duplicated.
4. **The old keys land in the workspace.** `Shift+C` opens it on Crop and starts cropping.
   `Shift+A` opens it on Colour. `Ctrl+T` opens it on Trim and arms trim. With the workspace
   open, `I` and `Ctrl+J` switch to Info and Jobs. Closed, `I` and `Ctrl+J` still open their
   panes alone (metadata is for viewing too). No key changes meaning outside the workspace
   except `Enter`, which was unbound.
5. **Crop gets presets, a straighten control and visible buttons.** Aspect presets are Free,
   Original, 1:1, 4:3, 3:2, 16:9 and 5:4, with a portrait/landscape swap. In crop mode, `A`
   cycles the preset (it was dead there, since `A`/`D` do not walk mid-crop) and `X` swaps the
   orientation (both are Lightroom's keys). Straighten is a ±45° slider. Rotate, flip, Apply and
   Cancel are buttons. The arrows keep moving the rect, and with a preset locked a resize keeps
   the ratio.
6. **Before/after is `Y`, held** (Lightroom's key; unbound), plus a **Show original** button in
   the strip. It shows the original pixels on the same canvas (the edit view is published as
   identity) and never touches the stack.
7. **Originals stay protected, unchanged.** Everything still goes through `edit_session`: the
   stack is non-destructive, *Save copy…* is PR 10's export (a new file, never an overwrite),
   and the lossless rotate writes are the same debounced PR 10 path. The workspace adds no write.

## Owner review (2026-09-26)

The owner reviewed the first build and asked for four changes, all taken:

1. **No rating-and-comment editor; every tag editable.** The pane's stars and comment box are
   gone. *Summary* has an editable **Date taken** (it moves every capture-time tag together),
   **Remove location**, and **Revert all**. *All tags* lets any tag the file allows be edited or
   removed, and can add one; a lock marks the rows that describe the file itself (sizes,
   offsets, maker notes, orientation — which is the rotate). This widens PR 12's writer; it
   keeps its checks, its sidecar rule and its snapshot, now of every tag
   ([12](12-decision-log.md) 2026-09-26).
2. **The pane must not cover the image or clash with the path bar**: it docks (decision 3) and
   starts under the path row, clipped to its frame.
3. **Buttons match the chrome**: the command bar's flat `FlatButtonStyle` (CozetteVector, no
   border, a hover wash, a held wash when selected) everywhere in the workspace and the
   metadata pane. *Edit image* sits in the bar between View and Settings, like a menu.
4. **Video editing happens in its own window** with a timeline, like iMovie / Final Cut /
   DaVinci — not a side pane. *Edit video* opens the **Video Editor** window; its design is
   [21](21-video-editor.md) (issue #40). The Trim tab below is the interim until it lands.

## Mockups

Still (Crop tab, a 3:2 preset locked):

```
┌ Open  View  Settings  About ─────────────────────────────── [ Edit image ] ? ┐
│                                                         ┌ Edit image ───────── ✕ ┐
│                                                         │ IMG_2041.JPG · 3 edits  │
│        ┌──────────────────────────────┐                 │ [Crop] Colour  Info      │
│   dim  │  ·  ·  ·  thirds grid  ·  ·  │  dim            │ ↶ Undo  ⟲ Reset  ◐ Orig. │
│        │                              │                 │ [ Save copy… ]           │
│        └──────────────────────────────┘                 ├──────────────────────────┤
│         5472 × 3648  +1.5°                              │ Aspect                    │
│                                                         │ Free Orig 1:1 4:3 [3:2]   │
│                                                         │ 16:9 5:4   ⇄ Portrait     │
│                                                         │ Straighten  ──●──  +1.5°  │
│                                                         │ Rotate ⟲ ⟳   Flip ⇋ ⇵     │
│                                                         │ [ Apply  ⏎ ] [ Cancel ⎋ ] │
│                                                         │ ←→↑↓ move · ⇧ resize      │
│                                                         │ A aspect · X swap         │
└─────────────────────────────────────────────────────────┴──────────────────────────┘
```

Clip (Trim tab, trim armed):

```
┌ Edit video ───────────── ✕ ┐
│ GOPR0412.MP4               │
│ [Trim]  Jobs               │
├────────────────────────────┤
│ In  00:12.400   [ Set in ] │
│ Out 01:03.000   [ Set out ]│
│ [ Preview P ]  [ Clear ⌫ ] │
│ [ Save ⏎ ] fast, keyframes │
│ [ Save exact ⇧⏎ ] slower   │
│ [ Copy without in–out ⌘X ] │
│ [ Split at playhead ⌘B ]   │
│ [ More clip tools… ]       │
└────────────────────────────┘
```

The Colour, Info and Jobs tabs show the existing Adjust, Metadata and Jobs panes under the
same strip.

## Architecture

| Layer | Piece | Change |
|---|---|---|
| Core, shared | `shell/edit_workspace.h/.cpp` | The pure model: `edit_tab` (crop, colour, info, trim, jobs), which tabs a still or a clip offers, the tab a command opens, and what `Esc` closes first. Unit-tested |
| Core, shared | `shell/edit_session` | `crop_aspect` presets plus orientation, a ratio-keeping resize, `A` / `X` in crop, `set_straighten(deg)`, `edit_count()` for the strip |
| Core, shared | `shell/commands.h`, `command_table.cpp` | `edit_workspace` (`Enter` in browse and video), `crop_aspect_cycle` (`A` in crop), `crop_aspect_swap` (`X` in crop), `show_original` / `show_original_release` (`Y` held, stills), and the island-only `crop_aspect_set` and `crop_straighten_set` |
| Mac host | `main_mac.mm`, SwiftUI | `EditStripView`, `CropPane` and `TrimPane` (`EditStore` polls one POD view by generation, like the other stores). The right-edge panes hang under the strip while it is open, and the canvas docks beside them. An Edit menu. The command-bar button. The metadata pane's tag editor (`MetadataView`) |
| Core, shared | `meta/write` | `write_fields::tags` (any Exif / Iptc / Xmp key, set or remove) and `date_taken`; `access_of()`; a whole-metadata snapshot (a JPEG's metadata segments and the sidecar, byte for byte) that `revert` splices back |
| Windows host | `main.cpp`, WinUI | The same strip and panes in `IslandHost.Edit.cs`, pushed through one new blittable `SetEditView` entry, as `SetAdjustView` is. The command-bar button. **Written, not yet compiled** (no MSVC on the machine this was written on) |

Rules: nothing here blocks the UI thread (the view is POD and the work is PR 10–14's existing
jobs), and no new canvas path is added (Show original publishes an identity `edit_view` on the
existing blit).

## Phases

- **Phase 1 (this change, PR 29).** The button, `Enter`, the Edit menu (Mac), the strip, the tabs,
  the Crop pane with presets, straighten, rotate and flip, the Trim pane, Undo / Reset / Save copy
  in the strip, Show original (`Y`), and the old keys routed into the workspace.
- **Phase 2.** On-canvas crop handles (drag corners and edges, drag inside to move) through the
  canvas's pointer input, with hit targets of at least 24 DIP. Redo (`Ctrl+Shift+Z`) and a
  history list naming each op ("Crop 3:2", "Exposure +0.3"), which needs a redo tail on
  `edit_stack` (`undo()` only pops today). A split-screen before/after.
- **Phase 3.** The Editor add-on's timeline and grade panes dock as extra tabs when it is installed
  ([21](21-editor-addon.md)). Persisting a still's stack across sessions (the XMP sidecar,
  [07](07-photo-editing.md)) is its own slice.

## Accessibility and platforms

- **Keyboard:** every control is reachable. The keys are listed above and in the strip's hint
  lines, and `?` shows them.
- **Screen reader:** every strip and pane control has an accessibility label that says what it
  does and its key ("Apply crop, Return"). The selected tab and aspect preset are exposed as
  selected, and the straighten slider reads its value in degrees.
- **Light/dark and high contrast:** the strip and panes use the chrome's theme tokens (`MVTheme`
  on Mac, theme resources on Windows), never fixed colours. The crop overlay is drawn on the
  canvas and already contrasts with both.
- **High DPI:** the layout is in points / DIPs, with nothing pixel-sized.

## What was run (Mac, 2026-09-26)

`MV_EDIT_SELFTEST=<dir>` drives the workspace through the commands its buttons run and writes the
window (canvas included, where the OS allows a process to read its own window) and a state line
per step. On a 1800 × 1200 JPEG: open → 1:1 → Apply → Colour → Info (set `Exif.Image.Artist` and
the date; both read back) → Original → Save copy → Esc → Revert. The copy is 1200 × 1200, and the
original's SHA-1 is unchanged after the revert. On a clip: Trim → arm → Jobs → Esc. Light and dark
both render. `mv_tests`: 464 pass. The 15 failures need `tests/data` fixtures this checkout lacks
(HEIF/AVIF, unrelated).

Not yet run: VoiceOver, 200 % on a non-Retina display, both present-loop gates with the pane
docked, and anything on Windows.

## Verify (both platforms)

1. A new user can crop with visible UI: open a JPEG → click **Edit image** → **Crop** → pick
   **3:2** → **Apply** → **Save copy…**. The copy's on-disk ratio is 3:2 within one pixel, and
   the original's bytes are unchanged.
2. `Enter` on a still opens the same workspace. `Enter` on a clip opens it on Trim. `Esc`
   cancels a crop draft first, then closes the workspace.
3. Undo steps back through the crop, Reset returns to the original, and holding `Y` shows the
   original while held and changes nothing.
4. Keyboard only: the whole of step 1 without a pointer (`Enter`, `Shift+C`, `A` until 3:2,
   `Enter`, `Ctrl+S`).
5. The strip and panes read correctly with VoiceOver / Narrator, in light and dark, at 100 % and
   200 %.
6. Both present-loop gates still hold with the workspace open (the panes float over the canvas;
   there is no inset).

`test_edit_workspace` and the new `test_edit_session` / `test_key_router` cases pass on both.
