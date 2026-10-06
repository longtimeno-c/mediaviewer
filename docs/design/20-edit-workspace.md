# The Edit workspace

The single visible entry point to still editing (crop, colour, metadata) and clip trimming: the
**Edit image / Edit video** button, `Enter`, and the docked pane with its tabs.

The workspace adds no edit engine and no write of its own. It is a door and one pane in front
of the photo edits ([07](07-photo-editing.md)), the metadata writer ([06](06-metadata.md)) and
the clip tools ([08](08-video-editing.md)). It is a base feature on both hosts; its engines live
in the core (`edit/`, `shell/edit_session`).

## Behaviour

1. **One door, two labels.** The command bar has **Edit image** on a still and **Edit video**
   on a clip, between View and Settings; it is disabled with no item. The key is **`Enter`**
   (`Return` on Mac) in browse and video modes. The Mac menu bar has an **Edit** menu with the
   same commands and keys.
   - On a still, `Enter` toggles the workspace, opening on Crop.
   - On a clip, `Enter` (and *Edit video*) opens the **Video Editor window**
     ([21](21-video-editor.md)), not this pane. `Ctrl+T` on a clip opens the workspace on its
     Trim tab and arms trim.
   - Inside the workspace `Enter` keeps its meaning: apply the crop, save the trim.
   - A document (PDF, DOCX) has nothing to edit: the button is **Open in <app>** (its default
     app, never MediaViewer) with a ▾ listing the other apps that open it, and `Enter` opens the
     default. Mac: `OpenInStore.swift` asks Launch Services; `-openDocumentInApp:` opens it.
     Windows: `shell/open_with_win.cpp` (Explorer's recommended Open with handlers, on a thread
     of its own), pushed through `chrome_edit_args.open_apps`, run by `chrome_cmd_open_in_app`.
2. **One pane, tabs, docked.** The workspace is a strip at the top of the right pane column
   (title, file name and edit count, tabs, Undo / Reset / Show original / Save copy…) over one
   tab's pane. It docks: the canvas frames the picture in the rect beside it
   (`input_snapshot.chrome_right_px`, the blit's `origin_x`), so the pane never covers the image;
   the strip starts under the path row. Docking is a refit, not a swapchain resize. The Colour,
   Info and Jobs tabs *are* the existing Adjust, Metadata and Jobs panes; Crop and Trim are panes
   that drive the existing commands.

   | Subject | Tabs (`edit_tab`) |
   |---|---|
   | Still | Crop (0), Colour (1), Info (2) |
   | Clip | Trim (3), Jobs (4) |

3. **The old keys land in the workspace.** `Shift+C` opens it on Crop and starts cropping;
   `Shift+A` opens it on Colour (and closes it from Colour); `Ctrl+T` opens it on Trim and arms
   trim. While open, `I` and `Ctrl+J` select Info and Jobs (and close from them). Closed, `I` and
   `Ctrl+J` open their panes alone. When the item changes, the workspace stays open on a tab the
   new subject offers, or closes when nothing is editable (`follow_subject`).
4. **Crop** has aspect presets Free, Original, 1:1, 4:3, 3:2, 16:9 and 5:4, with a
   portrait/landscape swap; the preset is kept for the session. In crop mode `A` cycles the preset
   and `X` swaps orientation. A locked preset fits the largest rect of that ratio centred on the
   draft; `Shift`+arrows resize keeping the ratio; a straighten shrinks it evenly. Straighten is a
   ±45° slider (`,` `.` step 0.5°). Rotate, flip, Apply and Cancel are buttons; the arrows move
   the rect (1 % of the frame per press).
5. **Show original** is `Y`, held (and a strip button). It publishes an identity edit view on the
   same blit and never touches the stack.
6. **Originals stay protected.** Everything goes through `edit_session`: the stack is
   non-destructive, *Save copy…* is the export ([07](07-photo-editing.md) "Export", a new file),
   and the lossless viewer rotate is the same debounced write as before.
7. **Info tab: every tag editable** (one file, one change set, through the metadata writer).
   *Summary* has an editable **Date taken** (moves every capture-time tag together), **Remove
   location** and **Revert all**. *All tags* lets any tag the file allows be edited or removed,
   and one be added; a lock marks rows that describe the file itself (sizes, offsets, maker notes,
   orientation). `meta/write`'s checks, sidecar rule and whole-metadata snapshot apply
   ([06](06-metadata.md)).
8. **Esc** drops a crop draft first; a second `Esc` closes the workspace.

## Layout

Still, Crop tab, a 3:2 preset locked:

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

Clip, Trim tab (`Ctrl+T`), trim armed:

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

Buttons use the command bar's flat style (`FlatButtonStyle`: CozetteVector, no border, a hover
wash, a held wash when selected) throughout the workspace and the metadata pane.

## Architecture

| Layer | Piece | Role |
|---|---|---|
| Core, shared | `shell/edit_workspace.h/.cpp` | The pure model: `edit_tab`, which tabs a still or clip offers, the tab a command opens (`route_workspace`), `apply_step`, `follow_subject`. Unit-tested (`test_edit_workspace`) |
| Core, shared | `shell/edit_session` | `crop_aspect` presets and orientation, ratio-keeping resize, `A` / `X` in crop, `set_straighten(deg)`, `edit_count()` for the strip |
| Core, shared | `shell/commands.h`, `command_table.cpp` | `edit_workspace` (`Enter` in browse and video), `crop_aspect_cycle` (`A` in crop), `crop_aspect_swap` (`X` in crop), `show_original` / `show_original_release` (`Y` held, stills), and the island-only, keyless `crop_aspect_set` (argument = preset, +16 for portrait) and `crop_straighten_set` (argument = degrees) |
| Core, shared | `meta/write` | `write_fields::tags` (any Exif / Iptc / Xmp key, set or remove) and `date_taken`; `access_of()`; a whole-metadata snapshot that `revert` splices back |
| Core, shared | `meta/tables` | `editable_properties_table`: the properties table plus each tag's raw value and access |
| Mac host | `main_mac.mm`, SwiftUI | `EditView.swift` (strip, `CropPane`, `TrimPane`), `EditStore` (polls one POD view by generation), the Edit menu, the command-bar button, `MetadataView`'s tag editor. Right-edge panes hang under the strip while it is open and the canvas docks beside them |
| Windows host | `main.cpp`, WinUI | A fifth panel island (`IslandHost.Edit.cs`): the strip and, under it, the Crop or Trim pane; on Colour / Info / Jobs the island is the strip alone and native places that pane under it. One blittable `SetEditView` entry (`chrome_edit_args`, 72 bytes) feeds the strip, panes and the button. The island sends keyed command ids plus four notifications (`edit_tab`, `edit_action`, `meta_tags`, `meta_date`). The canvas docks through `chrome_right_px` (`present_lab.cpp` `usable_canvas`). The metadata editor is in `IslandHost.Panels.cs` |

Nothing here blocks the UI thread (the view is POD; the work is the existing edit, export, metadata
and clip jobs), and no canvas path is added (Show original publishes an identity `edit_view` on
the existing blit).

## Accessibility and platforms

- **Keyboard:** every control is reachable; keys are in the strip's hint lines and the `?` sheet.
- **Screen reader:** strip and pane controls carry labels naming the action and key ("Apply
  crop, Return"); the selected tab and preset are exposed as selected; the straighten slider reads
  degrees.
- **Theme:** the chrome's theme tokens (`MVTheme` on Mac, theme resources on Windows), never fixed
  colours. The crop overlay is drawn on the canvas.
- **DPI:** layout in points / DIPs.

## Verify

`MV_EDIT_SELFTEST=<dir>` (both hosts; `main.cpp`, `main_mac.mm`) drives the workspace through the
same commands its buttons run, writing a window capture (Windows: `PrintWindow`, swapchain and
islands included) and a state line per step. Inert unless set; it counts as a harness run (no
hand-off to a running viewer, no `[recent]` entry).

On a still: open → pick 3:2 (or 1:1) → Apply → Colour → Info (set `Exif.Image.Artist` and the
date, read both back) → Original → Save copy → Esc → Revert. It checks that the copy's ratio
matches the preset within a pixel and that the original's SHA-1 is unchanged after the revert.
On a clip: open on Trim → arm → Jobs → Esc.

The behaviour it pins:

1. Crop with visible UI: Edit image → Crop → 3:2 → Apply → Save copy… gives a 3:2 copy; the
   original's bytes are unchanged.
2. `Enter` on a still opens the workspace; `Esc` cancels a crop draft first, then closes.
3. Undo steps back through the crop, Reset returns to the original, and holding `Y` shows the
   original while held and changes nothing.
4. Keyboard only: `Enter`, `Shift+C`, `A` until 3:2, `Enter`, `Ctrl+S`.

Tests: `test_edit_workspace`, and the workspace cases in `test_edit_session`, `test_key_router`
and `test_meta_write`.

## Not built

- On-canvas crop handles (drag corners, edges, inside).
- Redo (`Ctrl+Shift+Z`) and a named history list for stills; a split-screen before/after.
- Persisting a still's stack across sessions.
- Editor add-on panes docking as extra tabs.
