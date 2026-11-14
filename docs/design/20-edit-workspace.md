# The Edit workspace

The one visible way into still editing (crop, colour, metadata) and clip trimming: the **Edit
image / Edit video** button, `Enter`, and a docked pane with tabs. It adds no engine and no write
of its own; it fronts the photo edits ([07](07-photo-editing.md)), the metadata writer
([06](06-metadata.md)) and the clip tools ([08](08-video-editing.md)). Engines live in the core
(`edit/`, `shell/edit_session`). Base feature on both hosts.

## Behaviour

1. **One door.** The command bar shows **Edit image** on a still, **Edit video** on a clip
   (between View and Settings; disabled with no item). Key: `Enter` (`Return` on Mac) in browse and
   video modes; the Mac has an **Edit** menu with the same commands.
   - Still: `Enter` toggles the workspace, opening on Crop.
   - Clip: `Enter` / *Edit video* opens the Video Editor window ([21](21-video-editor.md));
     `Ctrl+T` opens the workspace on Trim, trim armed.
   - Inside the workspace `Enter` applies the crop / saves the trim.
   - Document (PDF, DOCX): the button is **Open in <app>** (its default app, never MediaViewer)
     with a ▾ of other apps; `Enter` opens the default. Mac: `OpenInStore.swift` (Launch
     Services), `-openDocumentInApp:`. Windows: `shell/open_with_win.cpp` (on its own thread) →
     `chrome_edit_args.open_apps` → `chrome_cmd_open_in_app`.
2. **Rail, header, one pane, docked.** A 44 pt icon rail at the right edge and, left of it, the
   inspector: a header over one tab's pane. The canvas frames the picture beside it
   (`input_snapshot.chrome_right_px`, the blit's `origin_x`) — a refit, never a swapchain resize;
   the pane never covers the image. Adjust, Info and Jobs *are* the existing Adjust, Metadata and
   Jobs panes; Crop, Trim and Enhance drive existing commands. Adjust (`edit_tab::colour`) is the
   full Adjust list ([07](07-photo-editing.md)) under a typed-request field.

   | Subject | Tabs (`edit_tab`), rail order |
   |---|---|
   | Still | Crop (0), Adjust (1), Enhance (5), Info (2) |
   | Clip | Trim (3), Jobs (4) |

3. **Keys.** `Shift+C` opens on Crop and starts cropping; `Shift+A` opens on Adjust (closes from
   Adjust, unfolds a folded inspector); `Ctrl+T` opens on Trim. Open: `I` / `Ctrl+J` select Info /
   Jobs (close from them); closed, they open those panes alone. On item change the workspace stays
   on a tab the new subject offers or closes (`follow_subject`).
4. **Crop.** Presets Free, Original, 1:1, 4:3, 3:2, 16:9, 5:4 with a portrait/landscape swap,
   kept for the session; `A` cycles, `X` swaps. A locked preset fits the largest rect of that ratio
   centred on the draft; `Shift`+arrows resize keeping it; straightening shrinks it evenly.
   Straighten ±45° (`,` `.` step 0.5°). Arrows move the rect 1 % per press. **On the canvas:**
   eight handles; a press within 12 px of an edge/corner drags it, inside moves the rect, elsewhere
   pans or drags the file out (`shell/crop_drag.h` `crop_hit` / `crop_drag_rect`,
   `edit_session::drag_crop`). Locked: a corner keeps the opposite corner and follows the pointer
   projected on the diagonal; an edge resizes the other side about its centre. With an angle the
   rect stops where a rotated source corner would show. The edge under the pointer sets the cursor.
5. **Compare** (header and rail) puts a before/after split on the canvas; `Y` held shows the
   before alone. Neither touches the stack. Disabled with a reason when there is nothing to compare
   ("Compare" below).
6. **Originals stay protected.** Everything goes through `edit_session`: the stack is
   non-destructive, *Save copy…* exports a new file ([07](07-photo-editing.md) "Export").
7. **Info: every tag editable** (one file, one change set, via `meta/write`'s checks, sidecar rule
   and snapshot, [06](06-metadata.md)). *Summary*: **Date taken** (moves every capture-time tag;
   with no capture stamp shows a labelled fallback and edits empty), **Location** (typed
   coordinates; blank removes after confirmation; [06](06-metadata.md) "The Location row"),
   **Revert all**. *All tags*: edit, remove or add any allowed tag; a lock marks file-structure rows.
8. **Esc** drops a crop draft first, then closes.

## Layout

Left to right: the canvas (picture fitted beside the pane), the inspector (header rows 1–2, then
the tab's pane), the rail (tabs, a divider, Red-eye, then Undo / Compare when folded and Done at the
foot). Crop pane: Apply / Cancel, aspect-ratio chips with the orientation swap, Straighten slider
with ↺, Rotate and flip, a key hint line; the canvas dims outside the rect and draws a thirds grid.

Trim pane: In / Out, *Preview* (`P`), *Clear*, *Save* (`Enter`, keyframes), *Save exact*
(`Shift+Enter`), *Copy without in–out*, *Split at playhead*, *More clip tools…*.

Buttons use the command bar's `FlatButtonStyle` (no border, hover wash, held wash when selected);
a primary action (Save copy…, Apply, a card's button) is filled with the accent.

### The rail and the header

The 44 pt rail runs from the command bar to the window foot; the inspector sits left of it. While
open, the filmstrip spans the canvas, not the window.

- **Tabs:** selected = accent wash + 2 pt leading bar. Clicking the active tab **folds** the
  inspector; any icon unfolds on its tab (`edit_workspace.collapsed`, `rail_pick`). Also folds when
  `clamp_inspector` returns 0. Closing forgets the fold.
- **Tools:** Red-eye only (the canvas tool with no Enhance card); click arms, again disarms.
  Remove, Faces and Extend arm from their Enhance cards.
- **Foot:** Undo and Compare while folded (the header has them otherwise), and Done. Tooltips and
  accessible names show live keys (`SettingsStore.keyLabel` / `KeyOf`).

**Header** height follows content (Windows sends `chrome_cmd_edit_strip_height`, 72–220 DIP).
Row 1: file name, edit-count pill, Photos/iCloud or *Cloud* mark. Row 2: **Auto** · Undo · Reset ·
Compare · **Save copy…** (accent, always labelled); when tight the middle three go icon-only first,
then Auto (Windows below 340). **Auto** (`auto_enhance`, a sun icon): tone then white balance, one
undo step ([07](07-photo-editing.md) "✦ Auto"); selected while it is the last edit, a click takes
it back.

### Compare

`shell/compare_source.h`, one model on both hosts.

- **What is compared** (`compare_kind` → `mv_edit_view.compare` / `chrome_edit_args.compare`):
  - an **Enhance result** (a file Enhance writes beside its photo: `<stem>-enhance|-restored|
    -extended|-removed|-eyes|-faces` `.jpg`/`.png`, also free names `"… (2).jpg"`,
    [29](29-enhance.md); or one registered via `note_result(result, source[, where])`, any
    thread, newest 64): before = the **source photo**, after = the result as shown;
  - else a photo **with edits**: before = no edit (identity placement, no colour);
  - else Compare is **disabled** with a tooltip (*No edits yet to compare*; *the original photo is
    not beside this one*; *Looking for the original photo…*; on Windows on the disabled button's
    holder). `Y` then shows the same words as a notice.
- **Finding the source** (no UI-thread I/O): the noted source; else the host's listing
  (`is_source_name`; Mac `_items`, Windows ±8 neighbours); else `source_of(path, exists)` on a
  worker (Windows `kMsgCompareSource`).
- **Source pixels** come through the viewer pipeline off UI/render threads (Mac `load_side_still`,
  Windows `mv::abi::request_side_still`), one immutable texture by atomic exchange
  (`take_side_still`, `before_token`), only when first wanted. Windows skips tiled stills (> ~64 MP).
- **On the canvas:** a second blit over the after's left part in the same present (scissor on
  Metal, narrower viewport on D3D11), same camera. `source_map` fits the source into the result's
  frame (Extend: at its own scale where `where` says). Frame-changing edits use `linked_camera`.
- **Divider** (`shell/compare_overlay.h`): 2 px line, 28 DIP handle, starts at `before_split`
  0.5, dragged in a 14 DIP band (hit-test `compare_frame_now()`). **Before | After** labels are a
  header row (Mac `compareLabels`, Windows `BuildCompareLabels`), not on the photo.
- **When:** stills only, not over the `|` two-up or in crop mode; closing or a new photo takes it
  down; `Y` works with the workspace closed. One present per change (`before_changed`).

### The Enhance tab

Tab 5, any still. Five cards: **Remove objects · Restyle · Faces · Enhance image · Extend edges**
— icon, title, one-line purpose, a chip only when something is needed (*Needs Enhance*, *Update
Enhance*, *Needs People*, *Opening…*, *Working…*). The worked-on card lifts to a lighter surface;
the running one has the accent outline. One column, two from 560. Without the Enhance add-on a
banner and each card offer **Get Enhance…**.

- **Host buttons:** the brush (*Start brushing*, Undo stroke, Clear), the frame (*Set the new
  frame*, arms Extend), Faces (*Choose faces* / *Done*; the Faces panel leads the tab while armed).
- **One click** ([29](29-enhance.md) "One click"): while the tab shows, Enhance's cards screen
  (place `edit`) is open on the photo; each card draws its tool's controls, a time estimate and one
  button. Remove and Extend run from the tool bar. A run glows over the photo; the result opens
  with Edit kept on this tab.
- **Performance chip** in the tab header (Enhance's `tierlevel`; [29](29-enhance.md) "The
  performance level"; Mac `EnhanceLevel`, Windows `OpenEnhanceLevel`).
- Hosts: Mac `EnhanceRun.swift`, `EnhanceHubView.swift`; Windows `IslandHost.EnhanceDock.cs`,
  `IslandHost.EnhanceHub.cs`. Fill quality details: [29](29-enhance.md#remove-what-was-brushed-in-edit-filled-in).

### Canvas tools

`shell/canvas_tool.h`, one model on both hosts. A canvas tool is what a press on the photo does
while armed: **Remove brush**, **red-eye**, **white-balance picker**, **Faces**, **Extend**'s
frame. Rail, cards, Adjust pane, `B` and the Edit menu arm the same host state, so tools work on
any tab and before the working image exists (only the picker needs it). Arming opens the workspace
on the tool's home tab, unfolded, applying any crop draft (tool and crop are exclusive). An armed
tool stays armed on the next still; it disarms on a clip, an animation or workspace close.

- **Keys** ([16](16-commands.md) "Canvas tools"): `[` `]` size; `Ctrl/⌘+Z` / `Ctrl/⌘+Shift+Z`
  one stroke or fix (then the edit's own undo); `Backspace`/`Delete` clear; `Enter` applies; hold
  `Space` to pan; `Alt`/`⌥` held erases; `Esc` disarms keeping strokes (dimmed). Rotate, flip, crop,
  Bin and navigating away are refused while strokes are pending, with a notice; menus grey.
- **Context bar:** a 44 pt capsule (≤ 640) in the transport's slot; the canvas reserves a 56 pt
  band (`kToolBarBand` in `chrome_bottom_px`). Brush: Paint/Erase · Size · Undo · Redo · Clear ·
  count · **Remove ↵** · Done. Red-eye: status · Size · **Find eyes** · Undo · Done. Extend: frame
  chips · **Ratio** · readout · **Fill ↵** · Cancel. Fixed slot widths so nothing shifts. Mac
  `ToolBarView.swift` (`mv_tool_bar_view`); Windows `IslandHost.ToolBar.cs` (`SetToolBar`,
  `chrome_tool_bar_args`; `chrome_cmd_canvas_tool` / `tool_size_set` / `tool_erase_set` =
  1096–1098).
- **Sizes** are screen pixels, 4–400 (brush 40, red-eye 24), via `canvas_map::screen_long_px`.
- **Ring** replaces the OS cursor, drawn in the same present (`shell/tool_overlay.h`): dark and
  light rings, centre dot, dashed when erasing; size label for 800 ms after a change.
- **Brush** (`remove_brush`): ≤ 64 strokes / 8192 dabs; undo/redo per stroke; **Clear is an undo
  step**; erase subtracts from the mask and hand-off (`remove_brush::handoff`). Strokes survive
  disarm, dimmed; a new item or geometry forgets them. While strokes exist rotate/flip are refused
  (`blocked_by_strokes`).
- **Mask texture** (`shell/brush_mask.{h,cpp}`): committed strokes are rasterised by one pool job
  at a time (1024 px long side, RGBA8 accent tint) into an **immutable** texture
  (`gfx::create_overlay_texture`, `image::upload_overlay`) handed over by atomic exchange; render
  uploads nothing. Dabs the landed build lacks travel in the snapshot (≤ `kBrushDabs` = 512).
- **Red-eye:** with People on, eyes are read once per item on the pool (`mv_eyes_json` /
  `ai_eyes_json`); the ring snaps to an eye within 1.5 radii at 1.1× its iris. A click lands a fix
  (one undo step, 300 ms pulse); a click on a fix removes it. **Find eyes** fixes every eye ≥ 12 %
  red as one step (`find_red_eyes`).
- **Faces** (`shell/faces_tool.h`): faces read once per item on the pool; each gets a box of its
  edit region (`edit::face_region`), a numbered badge, its People name; chosen = accent + wash. All
  start chosen (eyes-closed faces alone if any); a click toggles. Bar: "3 faces · 2 chosen" · Open
  eyes · Improve · **Do all** · Done. The **Faces panel** (Mac `FacesPanelView.swift`, Windows
  `IslandHost.Faces.cs`) lists each face with its fixes (**Open eyes**, **Improve face**, **Restore
  face** via Enhance's `faces` command), **Do all**, All / None, and *Get Local search…* / *Turn on
  People…* / *Try again* as needed (`faces_tool::needs_read`). Fixes are a table
  (`shell::kFaceActions`, `addon::media::face_fix`; Restore face is `face_action_row::enhance`). A
  fix changes **only the chosen faces** (IoU ≥ 0.3, `faces_mask_of` → `faces_mask`; test
  `[faces-mask]`). Results `<stem>-eyes.jpg` / `<stem>-faces.jpg` open at once with choices carried
  (`faces_tool::carry`). Windows `mv_eyes_json` / `mv_adjust_faces_to`; Mac `-facesFetch`,
  `-adjustFaces:`, `-facesRun:action:`. Opening Faces starts Local search on demand
  (`MvAddonsWakeAi` / `StartAiOnDemand`).
- ***Eyes closed*** (`addon::media::with_eyes_read`): faces re-detected upright
  (`refine_landmarks`), each eye read by `edit/eye_state` (a compiled-in OpenVINO model); both under
  `kEyeOpen` (0.63) badges the face. Unreadable eyes (`edit::eyes_readable`, `looks_shut`) are
  unknown, never shut. Bench `[.eye-state-bench]`.
- **Fix method:** Improve matches light/colour area by area, skipping patchy donors
  (`kLookPatchy`); Open eyes prefers same-moment donors (`burst_neighbours`, `kOccasionSeconds`),
  aims the iris (`edit::aim_irises`: `kOuterToIris`, `kOuterToIrisTurn`, `kGazeFollow`), and keeps a
  result only if its eyes read open (`kEyeFixedOpen` 0.9); else no file and a note. Up to six
  donors. Benches `mv_ai_tests "[.iris-aim-bench]"`, `"[.faces-bench]"`, `"[.faces-real-bench]"`.
- **Presents:** the overlay animates only to its deadlines (label, pulse); an armed tool at rest
  presents nothing.

#### Extend

`canvas_tool::extend` (*Set the new frame*, Mac Edit ▸ Extend Edges, `tool_extend`; home tab
Enhance). A frame whose edges and corners drag past the photo's edges (inside moves it). The frame
is shell state, `extend_draft` in `shell/edit_workspace.h` (each side as a fraction of width/height;
negative = crop), **never an edit**.

- **Limits:** past 25 % a side the readout adds "· may smear" and the band turns caution amber
  (`MVTheme.warm` / `TrackWarm`); 50 % a side is a hard stop; ≥ 5 % of the photo stays inside.
- **Canvas** (`tool_overlay.h` `draw_extend_frame`): new area hatched in the accent, excluded photo
  dimmed, eight handles. The camera fits photo + frame + 8 % (`extend_fit` →
  `input_snapshot::fit_rect_uv`, `fit_seq`) on drag end, chip choice or arming, never mid-drag.
  Another tool, item or a clip drops the frame. Redraws via `brush_rev`.
- **Ratio** chip: Crop's list (`shell/crop_aspect.h`) + swap. A ratio grows the frame about its
  centre, never shrinks it (`extend_to_aspect`; `extend_lock` so choices don't pile up); drags keep
  it (`extend_drag_aspect`). Out of reach (the stop, Enhance's 16384 / 100 MP caps): chip amber with
  the reason (`tool_bar::extend_short`). New frames start Free. The host hands Enhance margins,
  never a preset name. Wire: `tool_bar` 84 bytes, mirrored by `chrome_tool_bar_args`,
  `mv_tool_bar_view`, `ChromeToolBarArgs`; `tool_bar_action::extend_aspect + i` (32 +
  `crop_aspect`), `extend_aspect_swap` (9). Tests `[extend]`.
- **Readout** "+18 % · 6048×4032" uses Enhance's own sums (`extend_size`).
- **Fill** (`Enter`): the in-photo part becomes a crop (`edit_session::crop_inside`, one undo step);
  the overhang goes as margins via `still_extend` (Windows `mv_edit_still_extend`) and Enhance runs
  at once (`enhance/tool {"photo":"extend","run":true}`). **Cancel** (`Esc`) drops the frame;
  `Backspace`/`Delete` reset to Original. An unfilled frame blocks navigation; `X` swaps the ratio.

### Width classes

`shell/pane_width.h` `class_of`, read at the root and passed down only on a class change:

| Class | Width | Adjust | Other |
|---|---|---|---|
| compact | 280–339 | one column, labels above sliders, histogram 72, Curves content width | header icons only |
| regular | 340–559 | one column, labels inline, histogram 96, Curves ≤ 340 | |
| wide | 560–719 | two columns of section cards (Light, Colour, B&W, Vignette, Detail · Levels, Curves, Selective Colour, Red-Eye), labels above | Crop ratios in one row; Enhance cards two columns |
| xwide | ≥ 720 | wide, labels inline, plus a scopes row (histogram and square Curves) | |

`↑` `↓` walk the left column then the right; Windows moves the same cards between panels (never
rebuilt). Section cards carry a chevron, changed dot, Auto pill and ↺. A slider's value becomes a
number field on click; double-click resets to 0; tone sliders draw gradient tracks (Mac
`ToneSlider`). Info labels are min(100, 30 % of the pane) wide.

### The right column

Every right pane (workspace, Adjust, Info, Jobs) is one column of one user width
(`shell/pane_width.h`). The left edge drags: 280–720 (higher minimums for dense/large-type themes),
≤ 60 % of the window, canvas ≥ 320; snaps within 6 of 360 / 560; double-click resets to 360. View →
Wider / Narrower side pane steps 40 (`pane_wider` / `pane_narrower`). Saved on release (Mac
`MVRightPaneWidth`; Windows `[view] right_pane_dip`). The picture tracks the edge (a
`refit_seq` refit); neither swapchain nor window resizes.

- **Mac:** `NSLayoutGuide rightColumn`; `MvPaneEdgeView` (6 pt) shows the cursor and a hairline that
  becomes a 2 pt accent line while hovered/dragged; an accessibility splitter.
- **Windows:** a 6 DIP grab strip in the main window's client area (`IDC_SIZEWE`); islands re-laid at
  most once per refresh, pane content pushed once on release. Minimum 300 DIP × type scale
  (`chrome_cmd_inspector_min`); edge state via `chrome_cmd_pane_edge_state`.

Adjust's ask field stays under the header; the rest scrolls. Histogram corner triangles light on
clipping (a click toggles blinkies, `C`). Curves: a square ≤ 340, plot inset 6.

## Architecture

| Layer | Piece | Role |
|---|---|---|
| Core | `shell/edit_workspace.h/.cpp` | Pure model: `edit_tab`, tabs per subject, `route_workspace`, `apply_step`, `follow_subject`. `test_edit_workspace` |
| Core | `shell/edit_session` | `crop_aspect` presets/orientation, ratio-keeping resize, `A` / `X`, `set_straighten(deg)`, `edit_count()` |
| Core | `shell/commands.h`, `command_table.cpp` | `edit_workspace` (`Enter`), `crop_aspect_cycle` (`A`), `crop_aspect_swap` (`X`), `show_original` / `show_original_release` (`Y`), keyless `crop_aspect_set` (preset, +16 portrait), `crop_straighten_set` (degrees) |
| Core | `meta/write` | `write_fields::tags` (any Exif/Iptc/Xmp key), `date_taken`, `access_of()`, snapshot for `revert` |
| Core | `meta/tables` | `editable_properties_table`: properties plus raw value and access |
| Mac | `main_mac.mm`, SwiftUI | `EditView.swift` (header, `CropPane`, `TrimPane`), `EditRailView.swift`, `EnhanceHubView.swift`, `EditStore` (polls one POD view by generation), `MetadataView` tag editor |
| Windows | `main.cpp`, WinUI | `IslandHost.Edit.cs` (header + Crop/Trim/Enhance; header alone over Adjust/Info/Jobs), rail `IslandHost.Rail.cs` (`ShowEditRail`, island 11), one blittable `SetEditView` (`chrome_edit_args`, 104 bytes); notifications `edit_tab`, `edit_action`, `edit_strip_height`, `inspector_min`, `meta_tags`, `meta_date`; docking via `chrome_right_px` (`present_lab.cpp` `usable_canvas`); metadata editor `IslandHost.Panels.cs` |

The view is POD and the work is existing jobs; no present path is added.

## Accessibility and platforms

Every control is keyboard-reachable (keys in hint lines and the `?` sheet); controls carry names
with their key ("Apply crop, Return"); selected tab/preset exposed as selected; straighten reads
degrees. Colours from theme tokens (`MVTheme` / theme resources); layout in points / DIPs.

## Verify

`MV_EDIT_SELFTEST=<dir>` (both hosts) drives the workspace through its button commands, writing a
capture and a state line per step (inert unless set). Still: 3:2 → Apply → Auto, Enhance tab → Info
(set `Exif.Image.Artist` and the date, read back) → Save copy → Revert; checks the copy's ratio and
the original's SHA-1. Clip: Trim → arm → Jobs → Esc. It pins: a visible-UI 3:2 crop to a copy with
the original unchanged; `Enter` / `Esc` order; Undo / Reset / `Y` / Compare; the keyboard-only path
`Enter`, `Shift+C`, `A` to 3:2, `Enter`, `Ctrl+S`.

Tests (`mv_tests`): `[crop-drag]`, `[canvas-tool]`, `[tool]` (key router), `[remove-brush]`
(`[.perf-bench]` times the snapshot publish, < 0.1 ms), `[compare-source]`, `[auto_enhance]`,
`[pane-width]`, `[extend]`, plus workspace cases in `test_edit_workspace`, `test_edit_session`,
`test_key_router`, `test_meta_write`. Column drag pacing: `--pane-drag-soak`
([DEVELOPMENT](../DEVELOPMENT.md)).

## Craft rules

The UI checklist for every chrome surface on both hosts.

- **Spacing.** 12 pt inset from a card or pane edge; buttons 8–12 pt from what they act on; even
  padding; edges align or sit ≥ 8 pt apart. Nested corners: outer radius = inner + padding.
- **Type.** The theme's family, 11–15 pt, sentence case ("Auto", not "AUTO"); hierarchy from
  spacing and one weight step; tabular digits for changing numbers. Slider values sit right of the
  label as a percentage or count, never "1.50" (`SliderWords` / `ProgressWords.SliderValue`).
- **Colour.** Theme tokens only, correct in light and dark; on dark, depth is a lighter surface,
  not a shadow. Accent for primary actions and focus; caution amber for warnings.
- **One surface per thing.** A card is a fill (icon + title, one line, controls, one primary
  button): no box in a box; an outline only in high-contrast modes (Mac `SettingsCardOutline`,
  Windows `CardOutline`).
- **Say it once.** Each control and message appears in one place; status chips only when action is
  needed (no "Ready").
- **Selection lifts; the ring is the keyboard.** Chosen/active items take a lighter surface or wash;
  the accent ring marks keyboard focus only (Windows high contrast keeps its ring).
- **Icons.** SF Symbols (Mac) / Segoe Fluent (Windows), 13–16 pt, filled at ≤ 14 pt; every
  icon-only control has a tooltip and accessible name. Icons show the action — no sparkles or wands
  for "AI". Glyphs typed as text (✦ ★ ✓ ⚠ ▾ ▶ + −) become icon-set symbols.
- **Keyboard and accessibility.** Every control reachable by keyboard, named for screen readers
  with its live key; works at any type scale and DPI.
- **One progress language.** Every bar is the shared component (Mac `ProgressTrack` /
  `ProgressLine` in `ProgressLine.swift`; Windows `JobBar` / `ProgressLine` in
  `IslandHost.Progress.cs`): 3 pt track, accent fill that only moves forward, a calm sweep with no
  fraction, one line under it ("Enhancing · 40 % · about 1 min left"), a quiet Cancel; the outcome
  replaces the line in place. Spinners only for sub-second waits; longer waits show a skeleton or
  the progress line.
- **Motion.** State changes animate once (~150 ms); arrivals fade in. Never per frame or while
  idle; Reduce Motion / animations off cut. A one-click result cross-fades over its source (250 ms,
  `input_snapshot.result_fade_ms`, `canvas/refinement.h` `k_result_fade_seconds`).
- **Notices** share one card per host (Mac `NoticeCard` in `NoticeStore.swift`; Windows
  `NoticeCardBorder` in `IslandHost.HintNotice.cs`): 12 pt padding, radius 10.
- **Nothing floats over the photo** except the canvas tools' own overlays and the working glow;
  readouts live in bars and headers.

## Not built

Redo and a history list for the still stack; Compare keeping the after's crop, keyboard divider,
clips; a "Discard strokes?" prompt (crop from a menu clears strokes silently); renaming a person or
turning People on in place from Faces; `A` / arrows in Extend; Windows restoring the brush texture
after a D3D11 device rebuild; persisting a still's stack; editor add-on panes as tabs.
