# Edit workspace and Enhance refresh: final implementation spec (2026-11-04)

**Base design:** Canvas, which scored highest with both judges (33.5 and 31). It is grafted with the best ideas from Pro and Photos.

**Sources:** the reader maps, plus my own checks of the worktree at `f1297e9c`. I built nothing and ran nothing.

## Already landed on this branch (do not rebuild; the reader maps predate these)

- **`d7e8929f` Photos-library edits.**
  - `addon_media` has `set_photos_hooks(resolve, add)`, `readable_path` and `is_photos_key` (`src/abi/addon_media.h:76-86`).
  - `full_still`, the faces jobs and `write_still` stage a key's result and add it as a new photo.
  - Save copy exports to the staging folder.
  - The `Info.plist` sentence is updated.
- **`dd355710` Enhance performance level.**
  - `src/addons/enhance/tier.h` defines `tier { potato, intel, standard, maxxed }` plus `tier_policy {threads, restyle_steps, max_quality}` and `recommend_tier`.
  - `prefs.tier` defaults to `"auto"`.
  - `engine::effective_tier` (engine.cpp:407) applies the level.
  - The settings tree has a Performance card (screens.cpp:221-240).
- **`f1297e9c` Extend the picture (add-on half).**
  - `photo_target::extend = 3` (prefs.h:27).
  - `extend_margins`, `extend_preset` (presets "wider", "taller", "all", "square", "16x9", "4x5"; at most 2× each way and 16384 px a side), `extend_canvas` and `extend_tiles` (remove.h:52-68).
  - `runner::run_extend` (runner.h:126).
  - It is served by Remove's model with no second download (engine.cpp:184, 220, 436).
  - Test: `"[enhance][extend]"`.
- **`651d3afa` (WIP).** Mac `WandMenu` (EditView.swift:297), the Mac red-eye hover ring (`redEyeRing`, main_mac.mm:10535), and the typing fix. **The Windows halves are owed:** the wand twin and the red-eye ring.

What is left for owner items 7 and 8 is UX: navigating to the new photo, iCloud progress, a level chip in the hub, and gating by level in the hub. For item 1's Extend, what is left is **the canvas drag**: the add-on already fills margins it is given.

---

## 1. The experience

You press `Enter` on a photo. The picture glides left. On the right appear:

- a slim **44 pt icon rail**: Crop · Adjust · Enhance · Info, then a hairline, then the tools Remove · Red-eye · Faces · Extend, then at the foot Undo · Compare · Done;
- beside the rail, the **inspector**, which you can drag wider by its edge. The picture refits live.

**Adjust tab.**
- At the top is a field: "a bit warmer, less noise…". Typing works. If the parser understands nothing, it offers **Restyle with "…"?**.
- Below it is a histogram with clipping triangles, then section cards. Each card has a chevron, a changed dot, an AUTO pill and ↺.
- Slider readouts are clickable so you can type a value. Double-clicking a slider zeroes it.
- At 560 pt or wider the cards flow into two columns. At 720 pt or wider, the histogram and a **square, fully visible Curves** sit side by side.
- **✦ Auto** in the header applies one-click auto-enhance as a single undo step. Clicking it again takes it back.

**Enhance tab.** Five cards: **Remove objects · Restyle · Faces · Enhance image · Extend edges**. Each card shows its state ("Ready", "Download 208 MB", "Needs People", "Default level"). Runs, previews and results stay inside the pane.

**Canvas tools.** Choosing Remove (or `B`) arms the brush:
- The OS cursor becomes a crisp two-tone ring sized in screen pixels.
- A capsule **context bar** appears below the picture, in the band the transport uses on clips. The picture refits above it, so nothing covers the photo.
- `[` `]` resize the ring, with a fading "48 px" label beside it.
- ⌥/Alt-drag erases. Hold Space to pan.
- ⌘/Ctrl+Z undoes a stroke; ⇧⌘Z / Ctrl+Shift+Z redoes it.
- `Enter` removes; `Esc` puts the brush down.
- Rotate, flip and Delete are blocked while you brush, so strokes are never silently wiped.

**Red-eye.** The ring snaps to the eye. A click leaves a small ring with a check and a short pulse. Clicking a fixed spot undoes it. **Find eyes** fixes all of them as one undo step.

**Extend.** You drag crop handles *past* the photo. The new area shows hatched, and warns past 25 %. `Enter` fills it.

**Results.** Every result lands as a new file, or as a new Photos-library photo for library items, with **Show · Keep editing · Discard**. Discard moves the file to the Bin.

---

## 2. Layout and resize mechanics

### 2.1 Shared width model (new, portable): `src/shell/pane_width.h` (header-only) + `tests/test_pane_width.cpp`

```cpp
struct pane_width_limits { float min = 280, def = 360, max = 720, rail = 44, canvas_min = 320; };
float clamp_inspector(float want, float client, float theme_min, const pane_width_limits& = {});
//   min_eff = max(min, theme_min); max_eff = max(min_eff, min(max, 0.6f*client - rail))
//   client - rail - min_eff < canvas_min  → 0 (rail only)
float docked_right(float inspector, bool open, bool collapsed, const pane_width_limits& = {}); // rail + inspector
float snap_inspector(float w);   // within 6 of 360 or 560 → snap (the breakpoints)
enum class width_class : uint8_t { compact /*280–339*/, regular /*340–559*/, wide /*560–719*/, xwide /*≥720*/ };
width_class class_of(float w);
```

**One width for every right pane.** The Edit inspector, Adjust, Info and Jobs all share it. Only the docked workspace narrows the canvas, as today.

**Theme minimum (`theme_min`).** It comes from the chrome, because density and type scale change it:
- Mac: `EditLayout.minimumWidth(density:typeScale:)`, exposed like `aiBadgeWidth` (ChromeHost.swift:67).
- Windows: `300 × typeScale`, sent by C#.

**Persistence.** Only the *preferred* width is stored. It is rounded to 4 and saved on release.
- Mac: `NSUserDefaults "MVRightPaneWidth"`.
- Windows: new `std::int32_t right_pane_dip` in `view_settings` (`src/shell/settings.h:52`). It is not part of `flags()` and travels beside `sort`. 0 means 360.
- The width is read before the first layout. The core open never waits on it.

**Pane-edge gestures.**
- Double-clicking the edge resets the width to 360.
- New commands `pane_wider` / `pane_narrower` (±40) are appended with no default key, so they show in the View menu and can be remapped.
- Mac accessibility: the edge view has `NSAccessibilitySplitterRole` with an adjustable value.

**S1 built (2026-11-04):** §2.1, §2.2, the column and edge of §2.3 / §2.4 (no rail yet: hosts use
`pane_width_limits{.rail = 0}` and `shown_inspector`, which keeps a pane at its minimum where the
clamp says "rail only"), `pane_wider` / `pane_narrower` (ids 212, 213), §2.6's Curves and
histogram fit on both hosts, the Mac canvas guide (the transport centres on the canvas), and
`--pane-drag-soak` on both labs. One change to §2.1: the clamp's maximum also keeps the canvas at
`canvas_min` or wider (`client - rail - canvas_min`), so 644 pt gives 280, not 342. Owed by S2:
the Windows theme minimum is 300 DIP flat until the island sends its type scale
(`chrome_cmd_*`, S2's ids), and the edge's hot / drag feedback on Windows
(`chrome_cmd_pane_edge_state`, 1095). Not run here: the soak (either lab), the in-app checks,
both PR 1 gates and the selftest; the Windows halves are not compiled on the Mac (C# is).
S1 review (2026-11-04): Windows no longer re-pushes the Info / Adjust content on every drag step
(once on release), a lost capture before any move no longer snaps an unset width to 280, a click
that does not move the column keeps the preference (both hosts), `apply_view_state` falls back to
`resize_seq` if the size or scale did change, and the Mac re-clamps on a theme change.

**S2 built (2026-11-04):** the rail (both hosts: Mac `EditRailView` / `railHost`, Windows
`IslandHost.Rail.cs`, pane island id 11, `ShowEditRail`), the intrinsic header (Mac
`sizingOptions = [.intrinsicContentSize]` and `*TopUnder` constraints; Windows
`chrome_cmd_edit_strip_height` = 1094), the edge feedback on Windows (`chrome_cmd_pane_edge_state`
= 1095, sent native → island through a new `Notify` entry, `chrome_notify_args`), the Windows
theme minimum (`chrome_cmd_inspector_min` = **1099**, an id this plan did not reserve; 1096–1098
stay S3's), ✦ Auto (`auto_enhance` = 214; `edit::compute_auto_sections`,
`adjust_pane::request_auto` / `auto_enhance_on`), the wand bits 32–256 plus `kWandAutoBusy` = 512,
`chrome_edit_args` 96 → 104 bytes (`chips`, `collapsed`) and `mv_edit_view` likewise,
`edit_tab::enhance` = 5 with `edit_workspace.collapsed` and `rail_pick`, `chrome_edit_action`
`arm_brush` / `arm_red_eye` / `show_enhance` (6–8), the theme tokens (`surface2`, `hoverWash`,
`selectionWash`, `onAccent`, `warm`, `cool`, `tintG`, `tintM`; Windows `Surface2`,
`SelectionWash`, `Track*`), the four width classes on both hosts, and the Enhance tab's five
cards and the ✦ popover / Flyout launching the existing sheet (B's docked run is S5's). Not run
here: the app on either host (no screenshots at 320/420/640/760 yet), both PR 1 gates, the
selftest; the Windows native and C# are compiled only as far as the Mac allows (C# compiles).
S2 review (2026-11-04): a ✦ Auto waiting for the working image now runs only on the photo it was
pressed on (`adjust_pane::item`, `auto_enhance_settle` / `-autoEnhanceSettle`); moving on, the
pane dropping the image or a failed read ends the wait instead of leaving ✦ busy or running Auto on
the next photo. The Windows selftest presses ✦ Auto on the Adjust tab (a still with no colour drops
its working image when Adjust hides) and gains the "a bit warmer" line; the rail is in the Windows
Tab ring and its tooltips follow a remap; `chrome_cmd_inspector_min` joins both checksums; the
Windows header reports only height changes and re-lays only on a class change; the Mac title row's
ideal width no longer includes the whole file name, so a long name does not drop ✦ Auto's label.

### 2.2 Canvas refit without touching the swapchain (both present labs)

- **`src/shell/input_state.h`.** Add `std::uint32_t refit_seq` beside `resize_seq` (line 269), plus `std::uint8_t chrome_live_resize`.
  - From now on, `resize_seq` means only a real window, DPI or backing change.
  - These bump `refit_seq`: dock/undock, an edge drag, a rail collapse, the context-bar band, the filmstrip toggle, and the chrome bar height.
- **`src/shell/present_lab.cpp`** (the `resize_seq` block around line 514) and **`src/shell/present_lab_mac.mm`** (1444-1457) get a new branch: `if (snap.refit_seq != seen_refit_seq_)`.
  - It re-runs only the fit/fill on `usable_canvas` / `usable_window_w`.
  - `immediate = chrome_live_resize != 0`, so the picture tracks the edge exactly during a drag. On release it uses the spring (ω=18, ζ=1).
  - It never calls `swapchain_.resize`, `layer_.resize`, `ResizeBuffers` or `pacer_.set_refresh`.
- **Mac guard.** `src/gfx/metal_layer.mm:69` `resize()` returns early when width, height and scale are unchanged. This fixes today's chrome-only churn, where `drawableSize` is reassigned and `refresh_output_info()` is called on every `resize_seq`.
- **Mac.** New `-[MvMetalView syncChrome]` bumps only `refit_seq`.
  - Move these chrome-only callers of `syncSize` to it: `-applyFilmstripLayout` (5614), `-updateChromeBarHeight` (6019), `-syncWorkspace` (7979), and the page sidebar (13211).
  - `syncSize` stays for `setFrameSize` (2357) and `viewDidChangeBackingProperties`.
- **Windows.** `apply_view_state` (main.cpp:9619-9624) bumps `refit_seq` instead of `resize_seq`. `WM_SIZE` (9928) keeps `resize_seq`.

### 2.3 Mac (`src/shell/main_mac.mm` + Swift)

**Column guide.**
- Replace the five `widthAnchor == kMetaPaneWidthPoints` constraints (3721 adjust, 3734 jobs, 3757 strip, 3762 edit pane, 3789 meta) with `NSLayoutGuide *rightColumn` plus one `rightColumnWidth` constraint. Every pane's leading edge pins to the guide.
- New properties go at 469-490: `rightColumn`, `rightColumnWidth`, `railHost`, `paneEdge`, `canvasGuide`, `toolBarHost`.
- `kMetaPaneWidthPoints` becomes `kInspectorDefaultPoints = 360`. Delete `kEditStripHeightPoints` (418).

**Rail.**
- `railHost` is a `lazyHost(EditRailView)` (new `src.swift/.../EditRailView.swift`), pinned trailing, 44 wide, from `commandBar.bottom` to `adjustHost.bottom`.
- `rightColumn.trailing` = `railHost.leading` while the workspace is open, and the container's trailing edge otherwise.

**Header and tabs.**
- `editStripHost` becomes the header (§2.5). It is hosted with `sizingOptions = [.intrinsicContentSize]`, as the resume notice does (ChromeHost.swift:243-245), with low horizontal hugging and compression.
- In `-syncWorkspace` (7936-7980), `metaTop`, `adjustTop`, `jobsTop` and `editPaneTop` anchor to `editStripHost.bottomAnchor` while the workspace is open. This removes the 124 constants at 7954-7957.
- Drop `.clipped()` at EditView.swift:93.
- `adjustHost` gets `wantsLayer` and `masksToBounds`.

**Edge.** New `MvPaneEdgeView : NSView`, 6 pt wide and centred on `rightColumn.leading`, spanning `commandBar.bottom` to `adjustHost.bottom`. It is added after `editPaneHost` and below `transportHost` (3743).
- `resetCursorRects` sets `[NSCursor resizeLeftRight]`.
- Its own layer draws a 1 px `MVTheme.hairline` line, which becomes a 2 px `accent` line with a 4×28 grip pill on hover or drag.
- It is hidden unless a right pane is visible.
- `mouseDown` records the grab offset and sets `snap->chrome_live_resize = 1`.
- `mouseDragged` sets `rightColumnWidth.constant = clamp_inspector(...)`, then calls `[self.view syncChrome]` and `[self layoutAIBadge]`. AppKit already coalesces drags to one per frame.
- `mouseUp` snaps, persists, clears `chrome_live_resize`, calls `syncChrome` (which starts the spring), and calls `MetadataStore.setPaneWidth` **once**. That call runs `askSlots()`, so it must never run per step (MetadataStore.swift:265-270).
- A double-click resets the width.

**Other call sites.**
- `-dockedRightPoints` (7982) returns `docked_right(clamped, _ws.open, _railCollapsed)`.
- `-layoutAIBadge` (10902) uses the same value.
- `-[MvMetalView setFrameSize:]` (2357) re-clamps.

**Centring.** Add `canvasGuide`, running from container leading + `chrome_left` to `rightColumn.leading`, or to the rail. `transportHost` (3619) and `toolBarHost` centre on it. This also fixes the transport sitting off-centre while the column is docked.

### 2.4 Windows (`src/shell/main.cpp` + C#)

**State and sizing.**
- `app_state` (243) gains `bool pane_edge_drag`, `int pane_grab_dx`, `LONGLONG pane_last_layout_qpc` and `bool rail_collapsed`. The preferred width lives in `app->settings.right_pane_dip`.
- `right_pane_px(width, dpi)` (9365) becomes `right_pane_px(const app_state&, int width, int dpi)`, which returns `MulDiv(docked_right(clamp_inspector(...)), dpi, 96)`.
- Update every caller and declaration: 1363, 1543 (`layout_ai_badge`), 4455, 4595 (`layout_editor`), 9394 (`layout_panels`), 9565 (`update_client_metrics`).

**Islands.**
- `layout_panels` (9377-9441) places the inspector islands at `width - side … width - rail` and the new `_editRail` island at `width - rail`.
- `_editRail` is created in `AttachPanels` (Panels.cs:59-90); its content is in the new `IslandHost.Rail.cs`.
- The edit strip's height is intrinsic. `BuildEditStrip` measures `DesiredSize.Height` on `SizeChanged` and sends a new notification `chrome_cmd_edit_strip_height` (arg = DIP). It is appended in `chrome_host.h` near 197-205 and mirrored in `IslandHost.cs` `Checksum()` (266-292).
- Native stores `app->edit_strip_dip`, clamped to [72, 220] and re-laid only on a change of more than 1 DIP. It replaces `kEditStripDip = 140` (3738).

**Edge.** The islands are child HWNDs, so the grab strip is the main window's own client area.
- `pane_edge_hit(app, x)` is true for 6 DIP left of `width - side`, when a right pane is visible and the gallery, Settings and the editor are not covering it.
- In `window_on_pointer`:

  | Message | Handling |
  |---|---|
  | `WM_SETCURSOR` (10064) | `IDC_SIZEWE` |
  | `WM_LBUTTONDOWN` (10086) | Checked **before** `adjust_picking` (10093) and the brush (10101). Sets focus, `SetCapture`, records the grab offset, sets `chrome_live_resize = 1`, returns 0. A double-click resets. |
  | `WM_MOUSEMOVE` (9990) | While dragging, re-lays at most once per refresh interval (QPC): `layout_panels`, `update_client_metrics`, `++refit_seq`, `publish`. Returns before the brush, text and drag-out code. |
  | `WM_LBUTTONUP` | Flushes the last move, `ReleaseCapture`, snaps, saves `view_settings`, clears `chrome_live_resize`, `++refit_seq`. |
  | New `WM_CAPTURECHANGED` | Ends the drag. |

- Edge feedback: new notification `chrome_cmd_pane_edge_state` (0 idle, 1 hot, 2 drag) turns the edit root's left `BorderBrush` to accent at 2 px (Edit.cs:327-328).

**C# cost control.**
- On `SizeChanged`, `RenderCurve` and `RenderHistogram` are deferred to one `CompositionTarget.Rendering` tick.
- `RenderHistogram`'s fallback width of 300 (Adjust.cs:965) applies only before the first measure.

**Measured fallback.** The drag soak records UI-thread `layout_panels` → island layout time. **If p95 is over 12 ms**, the drag shows a guide line instead:
- `input_snapshot.pane_guide_x` is drawn by the overlay;
- the islands move only on release;
- the canvas still refits live.

### 2.5 Inspector structure and breakpoints (both hosts)

**Header (intrinsic height).**
- Row 1: file name (truncated in the middle), an edit-count pill, a "Photos" or "iCloud" chip, and **✦ Auto** with a chevron that opens the tools popover.
- Row 2: Undo · Reset · Compare · spacer · **Save copy…**, accent-filled.
- Below 340 pt the rows switch to icons only. Tooltips and screen-reader names carry the live key (`KeyOf` / `SettingsStore.keyLabel`).
- Mac uses `ViewThatFits`. Windows uses `ApplyWidthClass`, which swaps `Visibility`.

**Tabs move to the rail.**
- The wire values stay: crop 0, colour 1 (now labelled "Adjust"), info 2, trim 3, jobs 4.
- **Append `enhance = 5`** before `count` in `src/shell/edit_workspace.h:21-28`.
- `route_workspace`, `follow_subject` and `apply_step` offer tab 5 on any still. Without the add-on, its body is the install offer.
- Clicking the active rail icon collapses the inspector to the rail.

**Breakpoints.** `class_of(width)`, inspector width only.

| Class | Width | Adjust layout | Other |
|---|---|---|---|
| compact | 280–339 | One column, labels above the sliders, histogram 72, Curves = W−28 | Icon-only header |
| regular | 340–559 | One column, labels inline, histogram 96, Curves = min(W−28, 340) | |
| wide | 560–719 | Ask field and histogram across the full width. **Two masonry columns** (independent stacks). Left: Light, Colour, B&W, Vignette, Detail. Right: Levels, Curves, Selective Colour, Red-Eye. Curves = column − 16, ≤ 340. | Crop aspect chips in one row; straighten beside rotate |
| xwide | ≥ 720 | wide, plus a **scopes row**: histogram and Curves side by side, each W/2 | |

- `↑` `↓` walk the left column, then the right one. `adjust_step` ordering is unchanged.
- **Height rule:** everything except the header and the Ask field scrolls. That includes the histogram, clip readout, status, note, Reset and hint (AdjustView.swift:79-102 and 114-118 move inside the `ScrollView` at 103).

**Mac files.**
- `AdjustView.swift`:
  - `body` (69-170) becomes `AdjustSections(width:)` plus `graphs`.
  - `PaneWidthClass` is an `EnvironmentKey` computed by one `GeometryReader` at the root. It changes only when the class changes, so a drag does not rebuild the tree.
  - Use `MVTheme.font`, `space` and `radius` in place of `.headline`, `.caption` and `.regularMaterial`.
- `Theme.swift`: new tokens `surface2`, `hairline`, `accent`, `selectionWash`, `warm`, `cool`, `tintG` and `tintM`, the last four for gradient tracks.
- Slider rows: label, track (a gradient Canvas behind it for Temp, Tint, Hue and Saturation) and value. Clicking the value opens a `ThemeField`.
- `EditView.swift`: the HStacks at 147, 207, 247, 252 and 262 become `ViewThatFits { HStack; VStack }`.
- `MetadataView.swift`: the label widths of 100/106/130 become `min(100, W*0.3)`.

**Windows files.**
- `IslandHost.Adjust.cs` `BuildAdjustPane` (291):
  - The section roots live in either one `StackPanel` or a two-column `Grid` of two `StackPanel`s.
  - `ApplyWidthClass` **reparents** them (same XamlRoot, never rebuilt), so slider state and focus survive.
  - Width is not part of the `EditShape` rebuild key (Edit.cs:358-370).
- Gradient tracks override the `SliderTrackFill` and `SliderTrackValueFill` resources on that `Slider`. This is a resource override, not a `ControlTemplate`.
- Value click-to-type uses `FakeInput`.
- Double-tapping the row `Grid` resets the slider.

### 2.6 Curves and histogram fix (both hosts)

**Mac `CurveEditor`** (AdjustView.swift:406-500):
- One `plotRect = rect.insetBy(dx: 6, dy: 6)` is shared by drawing (426-456) and by `unit()` and the drag (467-498).
- `.clipShape` (458) goes. The frame is drawn as a stroke, so the end points and edge lines are fully visible.
- Square, capped per class. The `.aspectRatio(1, .fit)` at 341-350 stays.

**Windows** (Adjust.cs):
- `_curveCanvas.Height` tracks `ActualWidth` (capped) on `SizeChanged`, replacing the fixed 220 at 554.
- The same 6 px inset goes into `CurveUnit` (685), `RenderCurve` (751), `OnCurvePressed` (691) and `OnCurveMoved` (716).
- Add `PointerCaptureLost` → `_curveDrag = -1`, and an explicit `ReleasePointerCapture`.

**Both hosts:**
- an "in → out" readout ("128 → 142") while dragging;
- double-click a point to delete it;
- a histogram underlay in the channel tint;
- clicking a histogram clipping triangle runs the existing blinkies command (`C`).

---

## 3. Enhance hub (tab 5): five tools and the level

### 3.1 Hub frame

**Host-native cards** (Mac `EnhanceHubView.swift`, Windows `IslandHost.EnhanceHub.cs`), one per tool.
- Each card has an icon, a one-line purpose, a state chip and a primary button.
- The state comes from the wand bits in `src/shell/adjust_pane.h:40-44`. Append:
  - `kWandExtend = 32` (the remove spec exists);
  - `kWandDetail = 64`;
  - `kWandFacesReady = 128` (AI pack and People on);
  - `kWandAutoOn = 256`.
- The chips read the add-on's status. Download sizes and level gates come from the slot's settings JSON.

**The ✦ chevron in the header** opens a tools popover with the same five rows, for compact widths.
- Mac: `.popover` replaces the `Menu` in `WandMenu` (EditView.swift:297-349).
- Windows: `Button.Flyout` of `EditButton` rows. The control check allows it. **This closes the missing Windows wand twin.**

**Running inside the pane (owner call B).**
- A new manifest placement `"edit"`. Extend `one_of(*place, {...})` in `src/addon/manifest.cpp:162` and the `screen_place` enum.
- Enhance publishes `kToolScreen = "tool"` (screens.h:27). The host docks it in the Enhance tab under the selected card:
  - Mac: `SlotWindows.open` (ScreenView.swift:1417) gains the `edit` case and hands a `SlotScreenRoot` to `EnhanceHubView`.
  - Windows: IslandHost.Screens.cs:1856-1918 gains `edit` and builds `BuildScreenNode` into `BuildEditPane` row 2 (Edit.cs:337-345) when the tab is 5.
- `screens.cpp`: new `tool_tree(view)`, factored from `sheet_tree` (257-328) without the `photo` choice. It contains the knob card, preview `surface` (width = column), glow `progress`, eta, Cancel, and the result row.
- `engine.cpp` `event` (484-541) handles the new command `enhance/tool`, payload `{"photo":"remove|restyle|detail|extend"}`. It sets `prefs_.photo`, then publishes `tool_tree`.
- **Fallback if the owner declines B:** each card runs `enhance/open` with the same payload, so the sheet opens preselected. Every other slice works either way.

**Result row (all tools).**
- **Show** navigates to the new item.
- **Keep editing** navigates and reopens Edit on the same tab.
- **Discard** goes through the existing `recycle` host function to the Bin. It never hard-deletes, and the original is never touched.
- Files are written as today (doc 29 naming), so there is no staging/commit path and no new write row.
- *Deferred, not in these slices:* Photos' staged "stand-in" on the canvas.

**S5 built (2026-11-05):** owner call B on both hosts. Core: manifest place `"edit"`
(contribution API 12, one per add-on, refused for an item's `opens`, a status item and
`screen_show`), `slot_registry::screen_command` / `mv_slots_screen_command` (ABI 0.36). Enhance: the
`tool` screen (`tool_tree`: the knob cards without the photo choice, preview, glow progress, time
left, Cancel, the result row), `engine::tool_command` for `enhance/tool {"photo", "run"?}`, Discard
through `recycle_file` (the sheet keeps its status). Mac: `DockedToolPanel` under the chosen card
(`ScreenButtonHook` for Show / Keep editing), `enhanceTool:run:`,
`enhanceShowResult:keep:` (a library result selected by the key the add returned once listed);
the rail's Extend and the brush's Remove go to the panel. Windows: `IslandHost.EnhanceDock.cs`
(row 2 of the Edit pane on tab 5, `TakeScreenButton`), `main.cpp` `enhance_tool` /
`enhance_dock_close` / `enhance_show_result`, `chrome_edit_action` 9–14. Tests: `"[enhance][tool]"`,
`"[addon][slots][screens]"`, `test_addon_manifest`'s place case. Not run here: the app on either
host (the in-pane runs, Discard to the Bin, Keep editing on tab 5 and the SHA-1 check of the
verify line), both PR 1 gates; the Windows native C++ is not compiled on the Mac (C# compiles).
S5 review (2026-11-05): Windows' `tool_extend` command (menu, remapped key) still opened the sheet
while the Mac's went to the docked panel; it now calls `enhance_tool(app, "extend", false)` as
the rail does. Remove no longer has a run button inside the panel, before or after a result: the
card above already shows *Remove* (Return in the brush), which hands the strokes on screen over,
so a second one showed two Removes in one card and could run on dabs handed over earlier. The
hosts' hooks for it are gone. "With B declined, the sheet opens preselected" holds only for
Restyle (an Enhance without the docked screen has no "open on tool X" command).

### 3.2 The five tools

| Tool | UI | Host work | Add-on work | Model | Output |
|---|---|---|---|---|---|
| **Remove objects** | Card: "Brush over what to remove" · **Start brushing** (arms §4.2) · stroke count · **Remove** | Brush tool (§4.2). `set_still_mask` hand-off as today (addon_media.h:96-98). `Enter` in tool mode runs `enhance/tool {"photo":"remove","run":true}`. | **`remove_boxes(mask)`** in `remove.{h,cpp}`: connected components on a ≤256 px downscale (dilated 2 r), one grown box per blob (factor `remove_box`'s `grow` lambda, remove.cpp:55-67), merging neighbours when the union is under 1.3× their summed area. Falls back to `remove_box` when there are more than 8 blobs. `run_remove` (runner.cpp:650-690) loops the boxes with progress and cancel per box. Feather `clamp(box.w/256,2,16)`. | LaMa (existing `remove/`) | `<stem>-removed.jpg` |
| **Restyle** | `text_field` prompt, suggestion chips (buttons that fill it), Quality (options above the level are shown disabled with "needs Default/Maxxed"), Keep, Strength | The Ask field's fallback sets `k.restyle.prompt` through `set_setting`, then `enhance/tool {"photo":"restyle"}` | Existing restyle knobs. The quality gate already exists (`tier_policy.max_quality`) | InstructPix2Pix (existing) | `<stem>-enhance.jpg` |
| **Faces** | Host panel: face thumbnails from `faces.db`, name and "from 37 photos of Anna". Per face: **Open eyes** (only when shut is detected) and **Improve face**, plus **Do all**. With People off, a button to turn it on. Canvas: `canvas_tool::faces` draws a box per face; a click toggles it. | Existing `adjust_open_eyes` / `addon::media::improve_faces` (addon_media.cpp ~487/573), on the pool, limited to the selected faces through a new `faces_mask` in the pack request. **Windows twin of `adjustFaces:`** (main.cpp `adjust_faces`) is owed. | none | AI pack plus People donors (2026-11-02 call) | `<stem>-eyes.jpg` / `<stem>-faces.jpg` |
| **Enhance image** (sharpen, de-noise, upscale) | **Upscale** ×1 / ×2 / ×4 (the `scale` knob; ×1 means restore at the same size, i.e. sharpen) · **De-noise** slider (new) · **Strength** (existing) · 1:1 before/after split on the preview surface | none | `models/still/model.json`: add the `denoise` knob and `realesr-general-wdn-x4v3` as a second file. `picture_job.cpp`: Real-ESRGAN's two-weight interpolation, `w = (1-d)·x4v3 + d·wdn`, built once per run, plus the ×1 path (downscale the ×4 output with the existing `shrink`). | Real-ESRGAN x4v3 plus wdn (BSD-3; **the download needs the owner's OK**) | `<stem>-enhance.jpg` |
| **Extend edges** | Card: **Set the new frame** opens Crop in Extend mode (§4.4). The add-on's existing presets appear as chips. Readout "+18 % · 6048×4032". | §4.4: draft, overlay, `set_still_extend` | `run_extend` reads the host's margins when set, otherwise the preset. New: `extend_from_fractions(l,t,r,b,w,h)` in remove.cpp, clamped by `extend_preset`'s caps (2×, 16384, 100 MP). | LaMa (Remove's files) | `<stem>-extended.jpg` |

**S10 built (2026-11-05):** Restore faces, as [docs/design/29](../design/29-enhance.md) "Restore
faces" describes: the `face` kind (appended to `model_kind`), `models/face/model.json`
(RestoreFormer++ default, GFPGAN v1.4 "Natural", Strength 0.7), both pinned in `remote.json`;
`face_restore.{h,cpp}` (FFHQ alignment, warp, feathered mask, paste back, the People guard); the
pack's `faces_json` row and the host table's `still_faces` row (appended last); Enhance's `faces`
command; the Faces card's **Restore faces…** and a ✦ popover / `Flyout` row on both hosts
(`kWandRestoreFaces` = 1024, Windows `chrome_edit_action::restore_faces` = 15 after S5's 9–14 on
the merge, where S10's `face` kind also became 9 after S7's five). The guard is 0.55, measured on LFW
(`tools/enhance/face_guard.py`). Merged with S5 it runs docked under the Faces card (Enhance's
`faces` tool, `tool.faces`), the sheet when the docked screen is not there. Not
verified here: the Windows native half (compile-checked by mirroring only; C# compiled), an in-app
run on either host, Windows timings.

**Owner call C, later slice S10:** a *generative* "improve whole face" restorer. GFPGAN v1.4 is Apache-2.0 (the weights' and the ONNX export's licences still to check). It would run with a People-embedding identity guard: a restored face whose embedding drifts from the person's references keeps the original. CodeFormer is excluded (S-Lab non-commercial licence). Until the owner rules, "Improve face" means the existing `improve_faces`.

### 3.3 Performance level (landed; finish the surfaces)

- **Hub chip:** "Default level" in the Enhance tab header, which opens Settings → Add-ons → Enhance (the existing Performance card).
- **Level gating:**
  - Restyle Quality options above `max_quality` show disabled with the level they need.
  - On Potato, the Restyle card reads "Needs Intel or higher" when `restyle_steps` is under the floor. This is an owner call; the default is to show the card with Standard only.
- **Optional, needs the owner's download OK:** a per-level model file (Maxxed → RealESRGAN_x4plus) via `tier_policy.still_file`. Fetch only the delta, keep the old files until the new ones verify by SHA-256, then prune. The 5 GB pin and the free-space rule still apply.
- `machine.{h,cpp}` stays in `src/addons/ai/`. `tier.h` already reproduces the rule, so it is not moved.

**S9 built (2026-11-04):** `tier.h`: Restyle's steps Potato 20, Intel 25, Default 40, Maxxed 50
(`tier_policy.restyle_steps`), text guidance 7.5 unchanged; `quality_rank`,
`tier_for_quality`, `level_words`. `screens.{h,cpp}`: `level_quality` (the quality a Restyle
section shows and runs: the person's while the level allows it, else the level's highest, so
Default starts at High), `sheet_view.level`, Restyle's options above the level emitted with
`"disabled": true` and "(needs Default level)" / "(needs Maxxed level)", and the settings page's
"In use: <Level> level" (`tierlevel`). `engine.cpp`: the sheet's level, a change above it not kept.
Core `addon/screen.cpp`: a choice option's `"disabled"` (checked, and a change to it refused);
hosts draw it (`ScreenView.swift` `ChoiceNode`, `IslandHost.Screens.cs`). The hub chip: Mac
`EnhanceLevel` / `LevelChip` (`EnhanceHubView.swift`), Windows `BuildEnhanceLevelChip` /
`NoteEnhanceLevel` (`IslandHost.EnhanceHub.cs`); the tab opens Enhance's settings screen while it
shows to read `tierlevel`. Potato shows Restyle with Standard only (the default). Per-level files:
not built (call F). Not run: either host in the app (no windows here); Windows C# compiled only.
S9 review: a level changed in Settings reposts an open sheet (its gating and quality follow at
once); Windows asks for Enhance's page again when the add-on loads while the tab shows, and its
ComboBox goes back from a disabled option the keyboard reaches.

---

## 4. Canvas tools: keys, context bar, overlay

**S3 built (2026-11-04):** tool mode on both hosts. Portable: `shell/canvas_tool.h` (+
`test_canvas_tool`), `shell/tool_overlay.h` (replaces `brush_overlay.h`), `mode::tool` / `kTool`,
`blocked_in_tool`, `back_target::tool`, `view_state.tool` / `tool_pending`, `route.blocked`, the
twelve `tool_*` rows (ids 215–226, as reserved), `edit_session::remove_red_eye` / `add_red_eyes`,
`remove_brush` redo / erase dabs / `handoff()`, `canvas_map::screen_long_px`, `steady_ms`,
`edit_view.tool_*`, and `mv_eyes_json` (the Windows exe reaches the pack's eyes through it). Mac:
the tool state in `main_mac.mm` (`_tool`, `-armTool:`, `-toolDown`, the blank cursor,
`-toolFetchEyes`, Find eyes), `ToolBarView.swift` in `toolBarHost` (`mv_tool_bar_view`,
`mv_chrome_tool_action` / `_set_size` / `_set_erase`), Edit ▸ Remove Brush / Red-Eye / Faces /
Extend Edges. Windows: the same in `main.cpp` (decoupled from the Colour pane's pick, the ring on
`WM_MOUSEMOVE`, `SetCursor(nullptr)`, `kMsgToolJob`), the transport's island holding
`IslandHost.ToolBar.cs` (`SetToolBar`, `chrome_tool_bar_args` 56 bytes, `chrome_cmd_canvas_tool`
/ `tool_size_set` / `tool_erase_set` = 1096–1098, in both checksums), Alt + wheel with
`SC_KEYMENU` swallowed. The S2 Flyout wand was already built. The Adjust pane's dead size sliders
are gone (the size is the bar's). Not built here: the crop "Discard 3 strokes?" question in the bar
(crop is refused while a tool is armed; with the tool down a crop still clears strokes silently),
the brush's tint texture and the 64-stroke cap (S4a), Faces and Extend as canvas tools (S8, S6).
S3 review: the Windows ring re-dashes on Alt at once (`handle_app_key`, the Mac's `flagsChanged:`
twin), `?` lists browse under tool mode on Windows too, a red-eye eyes fetch that lands for a photo
left behind reads the one on screen, the Windows bar refreshes on every edit publish, and
`Ctrl/⌘+Z` with the brush armed and no stroke left is the edit's own undo. Not run
here: the app on either host, both PR 1 gates, the 0-presents check with a tool armed; the Windows
native C++ is not compiled (C# compiles).

**S4a built (2026-11-05):** brush v2 on both hosts. `remove_brush` is a stack of 64 strokes /
8192 dabs at 0.5 r spacing with undo, redo, an undoable Clear (8 kept) and `reset` for a forgotten
brushing; `shell/brush_mask.{h,cpp}` builds the committed strokes' tint on the pool (1024 px,
incremental, one job at a time) and both labs take an immutable texture from the worker by atomic
exchange (`gfx::create_overlay_texture`, `image::upload_overlay`) and draw it with `AddImage` in
`draw_tool_overlay`; `edit_view` carries only the dabs a landed build lacks. Rotate and flip are
refused while strokes exist in any mode (`blocked_by_strokes`, Mac menu items grey); the bar's Undo
follows `can_undo` (a Clear undoes), `tool_bar` / `chrome_tool_bar_args` 60 bytes on both chromes.
`mv_tests "[remove-brush]"` covers the verify list; the hidden publish bench gives 0.0001 ms
(8192 dabs, 512 live) and a 2.1–2.3 ms cold tint build on the worker (2.8 MB, M-series Mac, three
runs). Not run: either app, both PR 1 gates, the 0-presents check with strokes; the Windows native
C++ is not compiled here (C# compiles). Not built: the crop "Discard N strokes?" question.

**S4b built (2026-11-05):** per-blob LaMa, add-on only. `remove_boxes(mask)` (remove.{h,cpp})
downscales the mask to at most 256 cells a side, dilates 2 cells, labels 8-connected blobs and
gives each `remove_box`'s grown box of its exact bounds (the grow factored as `grown_box`); two
boxes whose union is under 1.3x their summed area merge (bounds joined and grown again) until none
do. More than 8 boxes after merging, or more than 64 blobs to merge, falls back to one
`remove_box` over everything (the plan said "more than 8 blobs"; counting after the merge keeps
clusters of nearby dabs per-blob). `run_remove` fills the boxes top to bottom with progress and a
cancel check before and after each fill; a box's model mask holds its own pixels and the later
boxes' (never shown what is being taken out), and only its own are pasted back
(`paste_back_box`), feathered `clamp(box.w/256, 2, 16)`. `mv_import_tests "[enhance][remove]"`
covers the verify line (two far blobs give two boxes each within 2x its blob's own grown box, near
blobs merge, nine far blobs fall back, on random dabs every brushed pixel is pasted by exactly one
box, the one around it, a run fills twice with every pixel outside both boxes the
source's own, a cancel during the first fill never starts the second and writes nothing). Not
run: the real LaMa on a two-blob photo (the hidden `[.remove-model]` case still brushes one
stroke); the "Quality over speed" second pass for large boxes is not part of this slice (Q built
it, below).

**S8 built (2026-11-05):** Faces on the canvas, both hosts. Portable: `shell/faces_tool.{h,cpp}`
(+ `test_faces_tool`): the cards from the pack's answer and the decoded still (thumbnails,
`edit::eyes_look_shut`), choosing (every face starts chosen), `face_at`, `apply_faces` onto
`edit_view` (`tool_face_*`, appended), `kFaceActions` (the per-face fixes as data: Open eyes only
where shut, Improve face; S10 appends Restore), `pack_faces_op`. `edit::face_region` is a face's
edit region (the drawn box). `addon::media::fix_faces` / `fix_faces_in` take a `faces_mask`
(`open_closed_eyes` / `improve_faces` gained it too) and fix each chosen face on a crop written
back inside its region, faded at its edge: `mv_ai_tests "[faces-mask]"` checks 0 pixels differ
outside the chosen boxes for Open eyes, Improve and Do all, and `faces_mask_of` matches the
canvas's boxes to the run's answer. The pack's `eyes_json` gains `"name"` and `"photos"`. Overlay:
brackets, numbered badge, wash, name pill, dashed while busy. Bar: `tool_bar_action` 6–8 (Open
eyes, Improve, Do all on the chosen faces); `eyes` / `spots` carry faces / chosen. Mac:
`FacesPanelView.swift` (`FacesStore`, `mv_chrome_faces_panel` / `_face_card` / `_faces_op`),
`-facesFetch`, `-facesRun:action:`, the hub's card and the rail arm the tool. Windows:
`IslandHost.Faces.cs` (`SetFacesPanel`, `chrome_faces_args` 40 / `chrome_face_card_args` 48
bytes), `chrome_cmd_faces` = **1100** (not reserved by this plan; renumber on a collision),
`main.cpp` `faces_fetch` / `adjust_faces` (the twin of the Mac's `adjustFaces:`, through the new
`mv_adjust_faces`, writing the same `-eyes.jpg` / `-faces.jpg`). Not run here: either app, both PR 1
gates, the 0-presents check with Faces armed, the real pack on a real photo (the shut-eye guess is
tested on drawn faces only), and "the Windows adjust_faces gives the same -eyes.jpg as the Mac on a
shared fixture" (both call the same `fix_faces`; the Windows native C++ is not compiled here, C#
is). S8 review: "Try again" re-armed the tool (a toggle: it put it down) and
"Turn on People…" left the panel spinning on "Looking for faces…" with nothing reading; both now
send the appended `faces_op::retry` (and the refused states offer *Check again*), a refused or
failed read is read again on re-arm (`needs_read`), a cancelled Mac read still lands (failed), a
fix left running on another photo no longer marks the next photo's faces busy (`run_item`), the
panel's rows light their boxes (`faces_op::hover`), and the Mac's per-pass panel poll no longer
builds a whole `tool_bar`.

**S6 built (2026-11-05):** Extend on the canvas, both hosts. Portable: `extend_draft` and its
pure maths in `shell/edit_workspace.h` (`extend_hit` / `extend_drag` / `clamp_extend`: 25 %
warning, 50 % stop, 5 % of the photo kept; `extend_crop` / `extend_overhang` split a mixed frame;
`extend_fit` the camera's frame with 8 % around; `extend_size` the readout through Enhance's sums;
the add-on's presets as `extend_preset_draft`), `input_snapshot::fit_rect_uv` →
`canvas::camera::set_fit_frame` in both labs (refit on drag end, a chip or arming; never during a
drag), `edit_view::tool_extend*` and `draw_extend_frame` in `tool_overlay.h` (hatched new area,
caution tint past 25 %, dimmed crop part, handles, readout), `edit_session::crop_inside` (the crop
part of a mixed frame, one stack op), `tool_bar` 72 bytes (`extend_growth` / `_warn` / `_preset`;
`tool_bar_action::extend_preset` + i) on both chromes. Host table: `still_extend` appended after
`still_mask` (`addon_media::set_still_extend`, `mv_edit_still_extend` for the Windows exe,
`host_services.still_extend`, `addon_abi.cpp`, `addons_mac.mm`). Add-on: `extend_from_fractions`
(remove.cpp; 2×, 16384, 100 MP, shrunk evenly), `run_extend` takes the host's frame when set, else
the preset; `run_port::still.suffix` names it `<stem>-extended.jpg`; a sheet opened with a frame
starts on Extend (`engine_port.framed`). Hosts: Mac `main_mac.mm` (`-extend*`, the press through
the brush's drag path, resize cursors) and `ToolBarView.swift` (chips, readout, Fill ↵, Cancel;
a menu when narrow); Windows `main.cpp` (`extend_*`, capture, `IDC_SIZE*` cursors) and
`IslandHost.ToolBar.cs`. Tests: `mv_tests "[extend]"` (draft: hit, drag, stops, warn, move, split,
fit, 3:2 → 16:9 size, caps; camera fit frame; `crop_inside` composes and undoes as one op),
`mv_import_tests "[enhance][extend]"` (the host-margins case: 3:2 → 16:9 dragged, written as
`<stem>-extended.jpg` at the size the readout showed, the photo's rect bit-exact; no frame → the
preset; the host's sums equal the add-on's on a grid and for every preset), `mv_ai_tests
"[photos]"` (the frame through the real host table's `still_extend`, a library photo added once).
Fill runs Extend in Enhance's docked panel (`enhance/tool {"photo":"extend","run":true}`, wired in
the merge of S5, S6 and S8; the sheet without a docked screen). Not built: `A` /
`X` / arrows with crop meanings in Extend.
Not run: either app, both PR 1 gates and the PR 1 soak with Extend armed (the pan-gate check),
the 0-presents check with Extend armed, the real LaMa on a dragged frame; the Windows native C++
is not compiled here (C# compiles).
S6 review: another tool armed over Extend now drops its frame and the room (both hosts); the Mac
resets the frame on a new item as Windows did; the fit frame applies only while Extend is active on
a still (a clip under the armed tool fits as itself); arming fits (`fit_seq`) so a zoomed view shows
the room; the frame in `addon_media` is tagged with the edit's geometry, so an undo of Fill's crop
or a later turn or crop makes it answer 0 (no stale fractions); a sheet opened with a frame hides
the preset choice and says the frame is used (`sheet_view::framed`); the canvas readout is gone
(the bar's, owner call A) and the bar adds "· may smear" in the overlay's amber past 25 %; the
hatch no longer crawls as a band grows; Windows releases the drag before `ReleaseCapture`.

### 4.1 Tool mode (shared, portable)

**`src/shell/commands.h`.**
- `mode::tool = 10` (mode 10 is free; `count` follows `skim = 9` at line 103).
- `kTool = 1 << 10`, added to `kAllModes` (line 121).

**`src/shell/canvas_tool.h`** (new, header-only) + `tests/test_canvas_tool.cpp`:
- `enum class canvas_tool : uint8_t { none, remove_brush, red_eye, wb_pick, faces, extend };`
- `struct tool_state { canvas_tool tool; float brush_px = 40 /*4–400*/; float redeye_px = 24; bool pan_held, erase_held; uint64_t hud_until_ms; };`
- `arm_tool(edit_workspace&, canvas_tool)` selects the home tab:
  - remove, faces → 5;
  - red_eye, wb_pick → 1;
  - extend → 0.
- Crop mode and a tool are exclusive.
- `adjust_pick` maps onto it: wb is pick 1 and red-eye pick 2 (wire values kept), and remove becomes `canvas_tool::remove_brush`. Windows' `brush_active() = pick()==remove` (main.cpp:2673) goes.

**`key_router`** (`src/shell/key_router.{h,cpp}`):
- `view_state` gains `bool tool` and `bool tool_pending` (strokes or spots exist).
- `resolve_mode`: after crop, `if (s.tool && s.item == item_kind::still) return mode::tool;`.
- On a miss, tool mode layers over browse: `lookup(mode::browse)` unless the pure function `blocked_in_tool(cmd, pending)` says no.
  - Always blocked: `rotate_ccw` / `rotate_cw`, `flip_*`, `crop_mode`, `delete_to_recycle_bin`.
  - Also blocked while `pending`: `next` / `prev` / `first` / `last` / `folder_*`, and Space-as-next.
  - A blocked key shows the notice "Remove or clear the strokes first".
- `back_target::tool` is **appended** to the enum (key_router.h:59-73). `resolve_back` checks it after `crop`. Esc puts the tool down and keeps the strokes, dimmed.

**New command rows**, all **appended** to `command_table.cpp`, with `command_coverage.inc`, `keymap.cpp` names and Mac `MvCommandSupported`:

| Key | Mode | Command |
|---|---|---|
| `[` / `]` (repeat) | kTool | `tool_smaller` / `tool_bigger` (×1/1.2). Tool rows win over the kBrowse\|kCrop rotate rows at 186-187. |
| Ctrl/⌘+Z · Ctrl/⌘+Shift+Z | kTool | `tool_undo` / `tool_redo` (one stroke or spot) |
| Backspace, Delete | kTool | `tool_clear` (undoable). **Never** `delete_to_recycle_bin`. |
| Enter | kTool | `tool_apply` (Remove / Extend fill / Done) |
| Space | kTool, momentary | `tool_pan` / `tool_pan_release`. Overrides Space = next on a still. |
| `B` | kBrowse (still) | `tool_brush`: open on Enhance and arm the brush. Plain `B` is unbound today; only Ctrl+B in video/trim. **Owner call D.** |
| none (menu and remap only) | | `auto_enhance`, `pane_wider`, `pane_narrower`, `tool_red_eye`, `tool_extend`, `tool_faces` |

- Island-only controls are `chrome_cmd_*` ids ≥ 1094 with an argument:
  - `chrome_cmd_canvas_tool` (arg = tool×1000 + size);
  - `chrome_cmd_tool_size_set` (px);
  - `chrome_cmd_tool_erase_set`.
- Alt/⌥+wheel resizes at the host edge, with no row. On Windows it swallows `SC_KEYMENU`.
- Keys not taken: `A`/`D` (they still walk when nothing is pending), `X`, `U`, `C`, `O`, `Z`, `\`, `G`, `S`, `T`, `0`–`4`, `R`, `?`.

### 4.2 Context bar (chrome, never over the photo)

**Host.** It reuses the transport slot: stills have no transport.
- Mac: while `canvas_tool != none` on a still, `transportHost`'s root swaps to `ToolBarView.swift`, which reads the new `EditStore` fields `tool`, `toolPx`, `erase`, `strokes`, `spots`, `busy`, `progress`.
- Windows: the transport island builds `BuildToolBar` (new partial `IslandHost.ToolBar.cs`) from a POD `chrome_tool_bar_args { visible, tool, size_px, erase, strokes, spots, can_apply, busy, progress_permille, extend_w, extend_h }`, with `static_assert` and a C# mirror.
- Allowed controls only: Button, Slider, TextBlock and the existing `JobBar` (no ProgressBar).

**Band.** While a tool is armed, the host adds `kToolBarBand = 56` to `chrome_bottom_px` and bumps `refit_seq`, with the spring. The picture refits above the capsule (44 high, ≤ 640 wide, centred on `canvasGuide`, 8 above the filmstrip).
- Doc 20 still holds: the transport's slot is used, nothing new sits over the picture. It needs **one doc 12 row**: "transport slot hosts the tool bar on stills".

**Contents.**

| Tool | Bar |
|---|---|
| Brush | Paint / Erase · Size (log slider, "48 px") · Undo · Redo · Clear · "3 strokes" · **Remove ↵** · Done |
| Red-eye | "Click a red pupil · 2 fixed" · Size · **Find eyes** · Undo · Done |
| Faces | "3 faces · 2 selected" · Open eyes · Improve · Done |
| Extend | preset chips · "+18 % · 6048×4032" · **Fill ↵** · Cancel |
| Busy | "Removing… 4 s · Cancel" |

- Hints come from the live table: "[ ] size · ⌥ erase · Space pan · ↩ remove".
- When the brush is full: "Brush full: Remove, then brush more", instead of beeping (main_mac.mm:10535, main.cpp:2698).

### 4.3 Brush model and drawing

**`src/shell/remove_brush.{h,cpp}`.**
- `brush_dab` gains `uint8_t erase`. Spacing goes to 0.5 r.
- Strokes become a stack: up to 64 strokes and 8192 dabs, with `undo_stroke`, `redo_stroke` and `clear` (an undoable entry).
- **Only the stroke in progress** (≤ `kBrushDabs` = 512, unchanged in `input_state.h:106`) travels in `input_snapshot`.

**Size in screen pixels.** At `begin()`, `r_uv = size_px / shown_long_side_screen_px`, using the rect `brushUV` (main_mac.mm:10517) and `brush_press` (main.cpp:2689) already map through. Add `screen_long_px()` to `still_map()` on both hosts. Delete:
- `brushRadius = 0.025·2^(size/100)` (main_mac.mm:10502);
- the size packing in `set_adjust_pick` (main.cpp:2616).

**Committed-stroke tint.** New `src/shell/brush_mask.{h,cpp}`, portable.
- A worker rasterises the committed strokes into RGBA8 tint (accent at 40 %, a 1 px darker edge, erase writes 0), 1024 px on the long side.
- It runs after each stroke end, undo, redo or clear.
- It creates an **immutable** texture: `D3D11_SUBRESOURCE_DATA` / `MTLTexture` from bytes, via the gfx port.
- The texture is handed over through the existing SPSC result ring and drawn with ImGui `AddImage` in `draw_crop_overlay` (present_lab.cpp:1629, present_lab_mac.mm:683).
- No new shader. It costs ≤ 4 MB, once per stroke, inside the ~2 ms upload budget.

**Overlay.** `src/shell/brush_overlay.h` becomes `tool_overlay.h`, shared by both labs on ImGui's foreground list:
- the live stroke as capsules between dabs;
- the ring: 1.5 px dark, 1 px light and a 2 px centre dot; dashed for erase;
- the size label for 800 ms after a change;
- red-eye spots (ring plus check), the hover-to-remove ×, and a 300 ms pulse;
- face boxes;
- the extend frame and handles;
- the guide line (§2.4 fallback).

**Colours.** `input_snapshot.overlay_accent_rgba` comes from the theme. No literals.

**Presents.** Frames are presented only while a stroke, the view or `hud_until_ms` is live. A still with a tool armed presents 0.

**Cursor.** Hidden over the canvas while a tool is armed:
- Windows: `SetCursor(nullptr)` in `WM_SETCURSOR`.
- Mac: an empty `NSCursor` cursor rect in `MvMetalView`.
- The open hand shows while Space is held.

**Pan.**
- `mouseDown` (main_mac.mm:2403) and `WM_LBUTTONDOWN` (main.cpp:10101) pan when `pan_held` is set, or on a middle or right drag.
- Windows `brush_active` becomes `app->brush_on && still && !crop`.

**Geometry guard.** The geometry keys are blocked in tool mode (§4.1). The menu items grey while `tool_pending`. Crop, entered from a menu, asks "Discard 3 strokes?" in the bar, instead of silently clearing at main_mac.mm:7854 and main.cpp:2186.

### 4.4 Red-eye and Extend specifics

**Red-eye.**
- Windows `WM_MOUSEMOVE` (10002) publishes the ring whenever `tool == red_eye`. This is the missing twin of Mac `redEyeRing`.
- `adjust_pick_at` (main.cpp:2640) and `adjustPickAtX` (main_mac.mm:10605) gain hit-testing against existing spots: a click on a spot removes it (`edit_session` spot remove).
- Each spot is one undo step.
- **Snap:** while armed with People on, a pool job reads this still's eye landmarks (the `ai_eyes_json` source used by `adjustFaces:`). Within 1.5× the ring radius of an eye centre, the ring snaps there and resizes to 1.1× the iris.
- **Find eyes** fixes every detected eye whose red ratio is over the threshold, as one `adjust_all` step.

**Extend.**
- New `extend_draft { float l, t, r, b; bool on; }` in `edit_workspace.h`. It is shell state only, **never** in the edit stack.
- In Crop, an **Extend** toggle (or the rail icon) arms `canvas_tool::extend`.
- The crop handles may go past [0,1]:
  - soft warning past 25 % per side ("fills may smear past here", warning tint);
  - hard cap at 50 % per side, which matches `extend_preset`'s 2×.
- The camera fits the virtual rect plus 8 % through new `input_snapshot.fit_rect_uv` and `refit_seq`.
- The area outside the photo is hatched.
- `A`, `X` and the arrows keep their crop meanings. Presets: Original, 1:1, 4:3, 3:2, 16:9, 9:16, +10 %, +25 %.
- **Apply (Enter)** splits the rect:
  - the intersection with the photo becomes an ordinary crop (`clamp_rect`, edit_stack.cpp:29);
  - the overhang becomes margins through new `media::set_still_extend(path, l, t, r, b)` beside `set_still_mask` (addon_media.h:97), plus an appended host-table row `still_extend` (`mediaviewer_addon.h` after `still_mask`, `addon_abi.cpp`, `addon_media.cpp`);
  - then `enhance/tool {"photo":"extend","run":true}`.
- Without Enhance, overhang is refused with a notice.

---

## 5. iCloud / Photos-library results

**S0 built (2026-11-04):** items 1–5 below, as [docs/design/26](../design/26-photos-library.md)
"Edits of a library photo" describes. Show is the arrow and the notice until S5's result row: the
new photo is listed next to its source (`folder_model::insert_virtual`); S5's **Show** selects that
key. The hydration notice on Windows is the title's status line (no chrome change). Not verified
here: the real-library walk-through and the Windows placeholder run (no Windows build on the Mac).

`d7e8929f` made `addon_media` the single sink: `readable_path`, `full_still` and `write_still` all go through the `resolve` / `add` hooks. What remains:

1. **Navigate to the new photo.** `photos_add_fn` returns the new item's key: `"photos:" + placeholderForCreatedAsset.localIdentifier`, read inside `performChangesAndWait` (`photos_items_mac.mm`). The host refreshes the virtual list and the result row's **Show** selects it. Notice: "Added to your Photos library".
2. **Add permission.**
   - `can_add()` is true for ReadWrite, AddOnly or limited access.
   - If neither is granted, request AddOnly on the click.
   - Add `NSPhotoLibraryAddUsageDescription` to `packaging/macos/Info.plist.in`.
3. **iCloud progress.** `resolve` gains an optional progress callback (`PHImageRequestOptions.progressHandler` / `networkAccessAllowed`), on the worker and cancelled by `job_context`. The header chip shows "Downloading from iCloud… 40 %", and the sliders wait as they do for RAW.
4. **Audit.** `run_remove` and `run_extend` `open_still`, the eyes and faces jobs, Save copy and Extend's margins must all go through `readable_path`. Add a test: each writer, given a `photos:` key with fake hooks, calls `add` exactly once and writes nothing beside the source.
5. **Windows.** No library (doc 26 is Mac-only).
   - An iCloud Drive or OneDrive placeholder (`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`) is hydrated by the worker read, with the same chip.
   - Results are written beside the original, and the sync client uploads them.
6. **Rule 5 holds.** Rotate and flip on a library item stay edits only. Nothing existing changes.

---

## 6. Ordered slices and file ownership

Every slice is dual-track (Mac + Windows) unless marked, and opens as a **draft PR**.

**Shared hot files** (★). These must merge serially, in slice order:
- `main.cpp`, `main_mac.mm`;
- `commands.h`, `command_table.cpp`, `command_coverage.inc`, `keymap.cpp`;
- `input_state.h`, `present_lab.cpp`, `present_lab_mac.mm`;
- `edit_workspace.h`, `chrome_host.h`;
- `IslandHost.cs` (`Checksum()`), `mediaviewer_addon.h`, `addon_media.{h,cpp}`, `adjust_pane.h`.

**Id order.** New `command_id`s, rows, `chrome_cmd` ids and host-table rows are reserved now in `docs/plans/edit-refresh.md`, in the order below:
- S1: `pane_wider`, `pane_narrower`.
- S2: `auto_enhance`; `chrome_cmd_edit_strip_height` and `chrome_cmd_pane_edge_state` (1094, 1095).
- S3: `tool_*` rows; `chrome_cmd_canvas_tool`, `chrome_cmd_tool_size_set`, `chrome_cmd_tool_erase_set` (1096–1098).
- S6: the `still_extend` row.

Command and row append order is persisted (remaps go by row index), so these slices merge in this order.

| # | Slice | Owns (new or exclusive) | Touches ★ shared | Parallel? |
|---|---|---|---|---|
| **S0** | Photos results finish (Mac) + Windows placeholder chip | `photos_items_mac.{h,mm}`, `Info.plist.in`, `tests/test_edit_session.cpp` photos fixture | `addon_media.{h,cpp}` (callback signature), `main_mac.mm` (Show/navigate, chip), `main.cpp` (hydration chip) | Yes, alongside S4b, S7, S9 |
| **S1** | Resizable column + `refit_seq` + Curves/histogram fit + pane-drag soak | `pane_width.h` + test, `metal_layer.mm` guard, `settings.h`, `AdjustView.swift` (`CurveEditor`, scroll move), `IslandHost.Adjust.cs` curve and histogram blocks, soak harness flags | `input_state.h`, both present labs, `main_mac.mm` (guide, edge, `syncChrome`), `main.cpp` (`right_pane_px` and callers, pointer, `WM_CAPTURECHANGED`), `commands.h`/table (`pane_wider`/`pane_narrower`) | Must be first among ★ slices |
| **S2** | Inspector refresh: rail, intrinsic header, ✦ Auto, wand popover/Flyout, breakpoints, theme tokens, tab 5 shell (cards launching the existing sheet preselected) | `EditRailView.swift`, `EnhanceHubView.swift`, `Theme.swift`, `ChromeHost.swift`, `EditView.swift`, `MetadataView.swift`, `IslandHost.Rail.cs`, `IslandHost.EnhanceHub.cs`, `IslandHost.Edit.cs`, `IslandHost.Panels.cs` (`AttachPanels`), `adjust_pane.cpp` (`request_auto(section_set)`) | `edit_workspace.h` (enhance = 5), `adjust_pane.h` (wand bits), `chrome_host.*`, `IslandHost.cs`, `main*.{cpp,mm}` (`layout_panels`, `syncWorkspace`), `commands.h`/table (`auto_enhance`), `AdjustView.swift` and `IslandHost.Adjust.cs` (layout; after S1) | After S1 |
| **S3** | Tool mode, context bar, overlay, red-eye UX, Windows ring | `canvas_tool.h` + test, `tool_overlay.h` (replaces `brush_overlay.h`), `ToolBarView.swift`, `IslandHost.ToolBar.cs`, `key_router.{h,cpp}`, `edit_session` spot remove | `commands.h`/table/coverage/keymap, `input_state.h` (`overlay_accent_rgba`, `kToolBarBand` via `chrome_bottom_px`), both present labs, `main*.{cpp,mm}` (`adjust_pick_at` / `adjustPickAtX`, `WM_MOUSEMOVE`, cursor) | After S2 |
| **S4a** | Brush v2 (host) | `remove_brush.{h,cpp}`, `brush_mask.{h,cpp}` (worker texture), `edit_view.h` | both present labs (`AddImage`), `main*.{cpp,mm}` (size_px, pan, guard), gfx port for the immutable texture | After S3 |
| **S4b** | Per-blob LaMa (add-on only) | `src/addons/enhance/remove.{h,cpp}`, `runner.cpp` `run_remove`, `tests/test_enhance.cpp` | none | **Yes, any time** |
| **S5** | Docked Enhance panel (`place "edit"`, `tool_tree`, `enhance/tool`, result row); **owner call B** | `src/addon/manifest.cpp`, `screens.{h,cpp}`, `engine.cpp`, enhance `manifest.json`, `ScreenView.swift`, `IslandHost.Screens.cs` | `EnhanceHubView.swift` / `IslandHost.EnhanceHub.cs` (from S2), `IslandHost.Edit.cs` row 2 | Add-on part in parallel with S3/S4a; host part after S2 |
| **S6** | Extend on the canvas | `remove.cpp` `extend_from_fractions`, `runner.cpp` `run_extend` margins source | `edit_workspace.h` (`extend_draft`), `input_state.h` (`fit_rect_uv`), both present labs (overlay past the frame), `addon_media.{h,cpp}` + `mediaviewer_addon.h` + `addon_abi.cpp` (`still_extend`), `main*.{cpp,mm}` | After S4a |
| **S7** | Enhance image: the five models of "Models" (supersedes the wdn x4v3 row of §3.2) | `models/still/model.json`, `picture_job.cpp`, `spec.cpp`, `tests/test_enhance.cpp` | none | **Yes, any time** |
| **S8** | Faces on the canvas + host Faces panel + Windows `adjust_faces` twin | `FacesPanelView.swift`, `IslandHost.Faces.cs`, pack request `faces_mask` | `main*.{cpp,mm}`, `tool_overlay.h` (boxes) | After S3 |
| **S9** | Level surfaces: hub chip, quality gating; optional per-level files (owner OK) | `screens.cpp` (settings/hub chip data), `engine.cpp`, `tier.h` (`still_file`), `downloads.cpp` | none on the host beyond the chip, which reads existing slot JSON | **Yes, any time** |
| **S10** | Generative face restore; **owner call C**, spike first | new `face` kind (appended to `model_kind`, spec.h:31), `models/face/` | `addon_media` faces request (`still_faces` row) | Last |

Each slice updates the docs it changes:
- `docs/design/20`, `16`, `29` and `07` (Extend is never a stack op);
- `docs/design/12` rows: transport slot as tool bar; `[`/`]` mean size in tool mode; A/D dead while strokes are pending; the 280–720 width budget; `refit_seq`; docked add-on screens;
- `docs/DEVELOPMENT.md` (soak flag, self-test lines, keys);
- the new `docs/plans/edit-refresh.md`.

---

## 7. Verification per slice

**Every slice:**
- both PR 1 present-loop gates (Windows and Mac);
- launch → first pixel and arrow timings, runs alternated base/new/base/new with the base in its own worktree;
- 0 presents on a still with the workspace open;
- `MV_EDIT_SELFTEST=<dir>` keyboard path `Enter`, `Shift+C`, `A`→3:2, `Enter`, `Ctrl+S` gives a 3:2 copy with the original's SHA-1 unchanged;
- `tools/check-winui-controls.ps1`.

| # | Verify |
|---|---|
| S0 | `mv_ai_tests "[photos]"` + new writer-audit case: every writer calls `add` once and writes nothing beside the source. Real library: Save copy, Detail and Open eyes on an iCloud-only photo each add one asset, the source asset's modification date is unchanged, the staging folder is empty, and Show selects the new asset. Windows: a placeholder hydrates with the chip and `-edit.jpg` is written beside it. |
| S1 | `test_pane_width` (clamp, rail-only collapse, 60 % cap, snaps). **`--pane-drag-soak`**, built before the drag code, on both labs: sweeps 280↔720 every 2 s for 20 s while panning a cached still. Passes when no frame is over 2× refresh, p99 is within 10 % of the base soak, and there are 0 presents after settle. On Windows it also reports UI-thread layout p95; over 12 ms switches the drag to the guide line. In app: the Curves square is fully visible with all four corner points hittable at 280/360/720 in a 900 pt window; the width persists across relaunch; Mac `drawableSize` is not reassigned during the drag (log counter). Selftest gains a drag to 600 and a curve point dragged to (1,1) and re-grabbed. |
| S2 | All four classes render with nothing clipped at density and type scale 1.0 and 1.25 (screenshots at 320/420/640/760). `test_edit_workspace`: tab 5 routing, `follow_subject`. ✦ Auto is one undo step, and a second click undoes it. The Windows Flyout wand lists the five tools. Screen-reader names carry the live keys. Selftest types "a bit warmer" and checks that temp rose by one step. |
| S3 | `test_key_router`: in kTool, `[`/`]` never rotate, Delete/Backspace never resolve to `delete_to_recycle_bin`, A/D walk when nothing is pending and are dead while something is, Esc order (text → popup → crop → **tool** → pane), Space pans and is not next. `test_command_coverage`. `test_canvas_tool`. In app on both hosts: the red-eye ring follows and snaps, a click fixes, the spot shows, a click on a spot removes it, Ctrl/⌘+Z undoes it, and Find eyes is one step. 0 presents with a tool armed and the pointer still; the pulse and size label stop presenting after their deadline. |
| S4a | `mv_tests "[remove-brush]"`: undo, redo, erase, cap, and clear-is-undoable. A 4 px stroke on a 6000 px photo at 400 % zoom gives `r_uv` ≈ 4/screen px. A `[.perf-bench]` case keeps the snapshot publish under 0.1 ms. The tint texture upload stays inside 2 ms. Rotate is refused with a notice while strokes exist. |
| S4b | `mv_import_tests "[enhance][remove]"`: two far-apart blobs give two boxes, each ≤ 2× its blob's grown box; pixels outside both boxes decode equal to the source's same encode; cancel works between boxes. |
| S5 | Remove, Restyle, Detail and Extend each run inside the pane on both hosts and show the result row. Discard puts the file in the Bin (never a hard delete). Keep editing reopens on tab 5. The original's SHA-1 is unchanged. `test_addon_manifest`: `place "edit"` is accepted, and unknown placements are refused. With B declined, the sheet opens preselected. |
| S6 | `"[enhance][extend]"` + new host-margins case: 3:2 → 16:9 via a drag writes `<stem>-extended.jpg` at the previewed size, with the photo rect bit-exact (feather 0). The overlay past the frame is pan-gate neutral (PR 1 soak with Extend armed). The crop part of a mixed rect lands as a normal stack crop. |
| S7 | `mv_import_tests "[enhance][still]"`: tile starts, tent cross-fade, sizes ×1/×2/×4, progress and cancel per tile, the blow-up guard, the sheet's choice and per-model download, the shipped pins. Hidden `[.enhance-image-model]` on the downloaded files: size, range, a blurred crop sharper, SCUNet's PSNR up. |
| S8 | Only selected faces change (pixel diff outside the boxes is 0). Donors come from People. The Windows `adjust_faces` produces the same `-eyes.jpg` as the Mac on a shared fixture. |
| S9 | `"[enhance][tier]"` still passes. The hub chip shows the effective level. Quality above the level is disabled with its reason. With per-level files: switching level fetches only the delta, the old files answer until the new ones verify, and choosing the same level again means Retry. |
| S10 | Spike report: licence check and identity-guard drift threshold on LFW pairs (`~/Dev/.mv-toolchain/faceeval`). Nothing is merged without owner call C. |

## Quality over speed (owner, 2026-11-04)

"Quality is preferred over speed" for every Enhance result (the viewer's own perf rules stay strict).
Concretely, applied in the second build round:
- **Restyle:** steps by level Potato 20, Intel 25, Default 40, Maxxed 50. The quality default is the
  highest the level allows (Default → High). Text guidance stays 7.5.
- **Remove:** after the 512-px LaMa fill, a second pass refines large boxes. A box over 1.5× the model's
  side is filled again on 512 tiles at full resolution, blended by weight as Extend is. The feather
  scales with the box.
- **Extend:** tiles of at least 768 px when the photo allows, two-thirds overlapping. Every margin pixel
  is in at least three tiles. (Built, measured softer, replaced: tiles of the model's side at least,
  two deep, and a detail pass; see Q's note below.)
- **Faces (GFPGAN):** the fp32 file, alignment by the FFHQ template, strength default 0.7, the People
  guard on. Eyes and whole-face transfer keep their look-matching.
- **Enhance image:** Real-ESRGAN tiles with more overlap, and the app's own noise reduction before it.
- Every run shows progress with the time left and can be cancelled.

**Q built (2026-11-05):** Remove and Extend, add-on only (`remove.{h,cpp}`, `runner.cpp`
`run_remove`). Remove's second pass: a box over 1.5× the model's side gets model-side tiles at full
resolution, half overlapping, over its own brushed pixels and their feather's reach
(`refine_tiles`), tent-weighted by how much each tile knew (none under a tenth, full from two
fifths), the feather `remove_feather` (longer side / 256, 2–16). One change to the plan's letter:
the tiles give the first pass their **detail** (the fill less itself at the first pass's scale),
not their whole fill. Filling them in whole was built and measured first: on a hole wider than a
tile the 512-px tiles see only nearby texture (a rock smeared over a horizon the first pass had
continued) and disagreeing tiles averaged to ghosts; with detail only the shapes stay the first
pass's and the grain comes back (inside a removed rock 1.79 → 2.60, the water beside it 4.94).
Extend: tiles of max(3× depth, 768) up to the picture's shorter side, two-thirds overlapping, two
more of other sizes at each end, every margin pixel in at least three (tested over the presets and
random margins); square tiles only (the old end tiles could be squashed). Progress counts fills,
cancel is checked between fills and tiles on both. Measured on a busy machine (docs/design/29):
Remove 4–14 s → 19–45 s, Extend 47–121 s → 158–170 s on a 3840 × 2160 photo. Extend's margins
lose their colour fringes and dark corners but are softer than before (more 768-px tiles filled at
512 and averaged); the detail transfer could serve Extend too. **Changed in the merge of Q, S6 and
S8 (2026-11-05):** measured with the real LaMa (docs/design/29 "Extend"), the three-deep averaging
took most of the detail, so Extend is back to tiles of the model's side at least, half overlapping
(two deep), and Remove's detail transfer now runs for Extend wherever a tile was larger than the
model's side (`extend_detail`): the sharpest margins of every tiling tried. Not run: either app,
Windows (add-on C++ mirrors existing patterns, not compiled there). Review (2026-11-05):
`refine_tiles` now leaves out the tiles `refine_fill` would skip (no hole, or a tenth known or
less), so Remove's progress counts only real fills and the time left no longer jumps; the output
is unchanged (those tiles had no weight).

## Models (owner OK 2026-11-04; downloaded and checked here, `~/Dev/.mv-toolchain/enhance-models/`)

From a surveyed and adversarially verified shortlist (licences checked at the source, not by HF tags).
Each is pinned in `src/addons/enhance/remote.json` and fetched by a user only when they use the tool.
Measured on a blurred, noisy, JPEG-35 crop (192 px, M-series CPU):

| Tool | Model | Pin (repo @ revision, file) | Bytes, SHA-256 | Licence | Tensors | Measured |
|---|---|---|---|---|---|---|
| Restore faces (main) | RestoreFormer++ | facefusion/models-3.0.0 @ 728b9659bd9691bf32cbf7f61af478d94b7ba81e, restoreformer_plus_plus.onnx | 294,264,232, 164818b70d1745da4108a45c9263b05452236d14ac0ffab2202af0ac4a438195 | Apache-2.0 (wzhouxiff/RestoreFormerPlusPlus; credit them) | `input` [1,3,512,512] RGB -1..1, output 0 (ignore 1, 2) | best identity of 11 methods (PMRF, ICLR 2025) |
| Restore faces (natural) | GFPGAN v1.4 | neurobytemind/GFPGANv1.4.onnx @ e3fd467e3a83420614abb9bf0b1c2c3addd359bc, GFPGANv1.4.onnx | 340,256,686, cd7311b8d9e13cdb1e208b12363182da58c7bf45e26d1aa67bbeac4751aae92e | Apache-2.0 | `input` → `1288`, 512, -1..1 | 0.8 s a face |
| Enhance image: maximum detail (default) | Real-HAT-GAN SRx4 "sharper" | MahoCommerce/Real_HAT_GAN_sharper-ONNX @ f0c8c3a46b1a45706d34ac73cbd71c21893a1ebb, onnx/model.onnx | 161,105,121, b3df277d8f68cf440eca120d39aa659fb08e2537a22d368646e79acfc2062db6 | Apache-2.0 (XPixelGroup/HAT) | `pixel_values` [1,3,192,192] RGB 0..1 → `reconstruction` [1,3,768,768]; fixed: tile at 192, overlap, fp32 only | 5.7 s a 192 tile; by far the sharpest |
| Enhance image: natural | 4xRealWebPhoto v4 (DAT-2) | Phips/4xRealWebPhoto_v4_dat2 @ 95e9cd50553951c27d86bafbdc1bc27ca8e750d2, 4xRealWebPhoto_v4_fp32_opset17.onnx | 48,760,847, a9a3a9099e0108b793556ed9cf2123a61a727bbc611dd8d93475f7144596589e | CC-BY-4.0 (credit Philip Hofmann) | `input` → `output`, dynamic, ×4, RGB 0..1 | 4.7 s a 192 tile; faithful, soft |
| Remove noise | SCUNet real-PSNR | deepghs/image_restoration @ 77885239ff28876d6b37dd080ab86aa1de2c49cc, SCUNet-PSNR.onnx | 91,264,256, b0f8c12f1575bb49e39a85924152f1c6d4b527a4aae0432c9e5c7397123465e3 | Apache-2.0 (KAIR MIT) | `input` → `output`, dynamic, same size, RGB 0..1 | 0.4 s a 192 tile |
| Remove blur (focus, JPEG) | NAFNet-REDS-width64 | the same repo @ 77885239…, NAFNet-REDS-width64.onnx | 275,371,113, fb5b264996d26cb1f7399a0af4e25cd26dbd5241910e58f7a4f1275734f76ea1 | MIT | as SCUNet | 0.1 s a 192 tile |
| Remove motion blur | NAFNet-GoPro-width64 | the same repo @ 77885239…, NAFNet-GoPro-width64.onnx | 275,371,113, 00c07294585512d162bc2313a2a12cd8d9cd532d0981d0d01f444bd48c4330cd | MIT | as SCUNet | — |

**Same size means "shrink to the detail, then ×4" (measured 2026-11-05, owner report "it doesn't look any
sharper").** Running a ×4 model on a soft photo and downscaling the result back changes nothing: on a soft
1500 px face (a 250 px photo blown up), the 4×-then-downscale path scored a Laplacian variance of 2.3 (x4v3)
and 3.1 (HAT) against 2.4 for the original, and HAT took 2,597 s for a 768 px crop on a loaded Mac. Shrinking
the photo first to where its detail lives (÷4 here), then running the ×4 model back to the original size,
scored 7.4 (x4v3) and **17.6 (HAT)** in **9.5 s**. So the same-size path picks a shrink factor d in 2…4 from
the photo's own detail (the largest d whose downscale-then-upscale round trip loses almost no
high-frequency energy, so sharp photos get d = 2 or are left as they are), resizes to 1/d, runs the ×4 model,
and resizes to the exact source size; ×2 and ×4 sizes use the same shrink before the model when the photo
is soft. Applies to HAT, RealWebPhoto and x4v3 alike.

**Built (2026-11-05):** `picture_job.h` `detail_shrink` / `plan_shrink` / `restore_at_detail`, run
by `run_still` for the upscalers (HAT, RealWebPhoto; x4v3 is only the video kind now). d is 1 or
1.25…4 (a sharp photo is not shrunk at all, rather than at 2), from the photo's three busiest
full-resolution crops; see [29](../design/29-enhance.md) "Shrunk to the detail". On the owner's
photo through the app's own tiled path: d = 4, Laplacian variance 2.13 → 18.71, 138 s for 9 HAT
tiles at load 10–12 (the 9.5 s above was a single untiled Python run). **Remove motion blur** is
hidden behind `kOfferMotionBlur` (its NAFNet-GoPro export is defective).

**S7 built (2026-11-05):** the five Enhance image models above, each its own kind
(`natural`, `denoise`, `deblur`, `motion` appended to `model_kind` after `remove`), downloaded
from the sheet when chosen; tiles blended by tent weight (`restore_picture`); a guard for tiles a
model blows up (NAFNet's exports do, GoPro on about a third of photo crops). See
[29](../design/29-enhance.md) "Enhance image". Not built here: the app's own noise reduction
before the upscalers (Remove noise is its own choice instead); Core ML for these (fp32 CPU). The
same-size shrink to the detail above was built after S7 (see its built note).

Enhance image offers: **Sharpen & restore** (HAT; at the same size, shrink to the detail then ×4, as above)
as the default, **Natural** (RealWebPhoto), **Remove noise**, **Remove blur**, **Remove motion blur**;
and the size (same, ×2, ×4) where the model upscales. Every tiled model blends overlapping tiles by
weight, as Extend does. Restore faces offers RestoreFormer++ (default) or GFPGAN ("natural"), aligned to
the FFHQ 5-point template, pasted back with a feathered mask, at a Strength (default 0.7), with the
People guard. CC-BY-4.0 credit lines go in each kind's model.json credit and the add-on's notices.

## Owner calls

Resolved 2026-11-04 by the owner's brief ("the UI should be unreal… get it done"): **A** yes (the tool
bar takes the transport band on stills, nothing floats over the photo), **B** yes (Enhance tools dock
in the Edit pane, the sheet as fallback), **D** yes (plain `B` arms the brush), **E** yes (warn at 25 %,
stop at 50 % a side), **G** yes (A/D dead while strokes are pending). **C** yes (owner, 2026-11-04): GFPGAN v1.4,
the ONNX export `neurobytemind/GFPGANv1.4.onnx` @ `e3fd467e3a83420614abb9bf0b1c2c3addd359bc`,
`GFPGANv1.4.onnx`, 340,256,686 bytes, SHA-256
`cd7311b8d9e13cdb1e208b12363182da58c7bf45e26d1aa67bbeac4751aae92e`, Apache-2.0 (GFPGAN's licence).
Measured here: input `input` [1,3,512,512] RGB scaled to -1..1, output `1288` [1,3,512,512] in about
-1..1 (clip), 0.8 s a face on CPU (M-series); on a blurred unaligned LFW face it restores detail, with
small artefacts where the face was not aligned, so S10 aligns each face to the FFHQ 5-point template at
512 (similarity from the pack's landmarks), restores, and pastes back through an inverse warp with a
feathered face mask, blended at a Strength knob (default 0.7) and guarded by People (the restored
face's embedding must stay the same person, else that face is left as it was). Test copy at
`~/Dev/.mv-toolchain/gfpgan/`. **F** no: there is no ONNX of wdn x4v3 on Hugging Face (only a .pth
mirror), so S7 builds only what needs no new download; per-level files are not built.

### As proposed

| Call | Question |
|---|---|
| **A** | Confirm that the tool bar takes the transport slot on stills in a reserved band. Nothing floats over the photo. |
| **B** | Dock an add-on screen (`place "edit"`) in the Enhance tab. The fallback is the existing sheet. |
| **C** | A generative face restorer: GFPGAN with a People guard. |
| **D** | Plain `B` as the default key for the brush. |
| **E** | Extend's soft warning at 25 % and hard cap at 50 % per side. |
| **F** | Downloading wdn x4v3 (S7) and any per-level model files (S9). |
| **G** | A/D dead while strokes are pending, versus keeping strokes per item. |