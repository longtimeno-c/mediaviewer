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
2. **A rail, a header, one pane, docked.** The workspace is a 44 pt icon **rail** at the window's
   right edge (the tabs, the tools, Undo, Compare, Done; "The rail and the header" below) and,
   beside it, the **inspector**: a header (file name, edit count, a Photos / cloud chip, ✦ Auto;
   Undo / Reset / Compare / Save copy…) over one tab's pane. It docks: the canvas frames the
   picture in the rect beside it (`input_snapshot.chrome_right_px`, the blit's `origin_x`), so
   the pane never covers the image; the header starts under the path row. Docking is a refit,
   not a swapchain resize. The Adjust, Info and Jobs tabs *are* the existing Adjust, Metadata and
   Jobs panes; Crop, Trim and Enhance are panes that drive existing commands. The Adjust tab
   (`edit_tab::colour`, labelled Adjust since docs/plans/edit-refresh.md S2) is the full Adjust list: Light, Colour, Black &
   White and Vignette sections, each collapsible with its own reset (↺, shown once a slider in it
   moved) and, but the vignette and curves, an AUTO; above them, a field takes a typed request and
   says what it changed ([07](07-photo-editing.md) "Typed requests"); Black & White has an on switch; Levels and
   Curves pick a channel (Image, Red, Green, Blue), and Curves has a point editor over the histogram
   (drag the line to add a point, drag a point off the square to remove it); Selective Colour picks
   a band (eight swatches); Colour has **Pick white balance** and Red-Eye **Fix red eyes** with a
   size, each taking the next click (or clicks) on the photo
   ([07](07-photo-editing.md) "Evaluation", "Auto").

   | Subject | Tabs (`edit_tab`), in the rail's order |
   |---|---|
   | Still | Crop (0), Adjust (1), Enhance (5), Info (2) |
   | Clip | Trim (3), Jobs (4) |

3. **The old keys land in the workspace.** `Shift+C` opens it on Crop and starts cropping;
   `Shift+A` opens it on Adjust (and closes it from Adjust, or unfolds a folded inspector);
   `Ctrl+T` opens it on Trim and arms trim. While open, `I` and `Ctrl+J` select Info and Jobs (and close from them). Closed, `I` and
   `Ctrl+J` open their panes alone. When the item changes, the workspace stays open on a tab the
   new subject offers, or closes when nothing is editable (`follow_subject`).
4. **Crop** has aspect presets Free, Original, 1:1, 4:3, 3:2, 16:9 and 5:4, with a
   portrait/landscape swap; the preset is kept for the session. In crop mode `A` cycles the preset
   and `X` swaps orientation. A locked preset fits the largest rect of that ratio centred on the
   draft; `Shift`+arrows resize keeping the ratio; a straighten shrinks it evenly. Straighten is a
   ±45° slider (`,` `.` step 0.5°). Rotate, flip, Apply and Cancel are buttons; the arrows move
   the rect (1 % of the frame per press).
5. **Show original** is `Y`, held (and the header's and the rail's Compare). It publishes an
   identity edit view on the same blit and never touches the stack.
6. **Originals stay protected.** Everything goes through `edit_session`: the stack is
   non-destructive, *Save copy…* is the export ([07](07-photo-editing.md) "Export", a new file),
   and the lossless viewer rotate is the same debounced write as before.
7. **Info tab: every tag editable** (one file, one change set, through the metadata writer).
   *Summary* has an editable **Date taken** (moves every capture-time tag together; on a
   file with no capture stamp the row shows a labelled fallback, Created / Signed at / File
   created, and Edit opens it empty: [06](06-metadata.md), #415), an
   editable **Location** (#352: coordinates typed in, blank removes after a confirmation;
   [06](06-metadata.md) "The Location row") and **Revert all**. *All tags* lets any tag the file allows be edited or removed,
   and one be added; a lock marks rows that describe the file itself (sizes, offsets, maker notes,
   orientation). `meta/write`'s checks, sidecar rule and whole-metadata snapshot apply
   ([06](06-metadata.md)).
8. **Esc** drops a crop draft first; a second `Esc` closes the workspace.

## Layout

Still, Crop tab, a 3:2 preset locked (regular width):

```
┌ Open  View  Settings  About ──────────────────────────────── [ Done ] ? ┐
│                                                 ┌ IMG_2041.JPG (3 edits) [✦ Auto ▾]┬────┐
│                                                 │ ↶ Undo ⟲ Reset ◐ Compare [Save…] │▌⌗ │ Crop
│        ┌──────────────────────────┐             ├──────────────────────────────────┤ ≡  │ Adjust
│   dim  │  ·  ·  thirds grid  ·  · │  dim        │ CROP                             │ ✦  │ Enhance
│        │                          │             │ [ Apply ] [ Cancel ]             │ ⓘ  │ Info
│        └──────────────────────────┘             │ ASPECT RATIO                     │ ── │
│         5472 × 3648  +1.5°                      │ Free Orig 1:1 4:3 [3:2]          │ ⌫  │ Remove
│                                                 │ 16:9 5:4   ▭ Landscape           │ ◉  │ Red-eye
│                                                 │ STRAIGHTEN  ──●──  +1.5°  ↺      │ ☺  │ Faces
│                                                 │ ROTATE AND FLIP ⟲ ⟳ ⇋ ⇵          │ ⤢  │ Extend
│                                                 │ ←→↑↓ move · ⇧ resize · ⏎ apply   │    │
│                                                 │                                  │ ↶  │ Undo
│                                                 │                                  │ ◐  │ Compare
│                                                 │                                  │ ✓  │ Done
└─────────────────────────────────────────────────┴──────────────────────────────────┴────┘
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
wash, a held wash when selected) throughout the workspace and the metadata pane; a primary action
(Save copy…, Apply, a card's button) is filled with the accent.

### The rail and the header

docs/plans/edit-refresh.md S2. While the workspace is open a **44 pt rail** docks at the window's
right edge, from the command bar to the filmstrip; the column (the inspector) sits left of it.

- **Tabs:** Crop, Adjust, Enhance, Info on a still; Trim, Jobs on a clip. The selected icon wears
  the accent wash and a 2 pt accent bar on its leading edge. A click on another tab selects it;
  on the active one it **folds the inspector to the rail** (the picture refits into the wider
  rect); any icon of a folded rail unfolds it on that tab (`edit_workspace.collapsed`,
  `rail_pick`). A window too narrow for any inspector beside the canvas (`clamp_inspector`
  returns 0) folds it too, and unfolds when it is wide enough again. A tab's own key unfolds a
  folded inspector rather than closing it. Closing forgets the fold.
- **Tools:** Remove, Red-eye and Faces are canvas tools ("Canvas tools" below): a click arms one
  (Remove and Faces on the Enhance tab, Red-eye on Adjust), a second click puts it down, and the
  armed one wears the selection. Extend opens Enhance's docked panel on Extend (S5) until S6
  makes it a canvas tool too.
- **Foot:** Undo, Compare (Show original), Done. Tooltips and screen-reader names carry the live
  keys (`SettingsStore.keyLabel` / `KeyOf`, so a remap shows).

The **header** is as tall as its content (the Mac hosts it with an intrinsic height; Windows
measures it and sends `chrome_cmd_edit_strip_height`, clamped to 72–220 DIP, re-laid only on a
change of more than 1 DIP), so the panes start right under it at any type scale. Row 1: the file
name (truncated), an edit-count pill, a chip (*Photos* for a Photos-library item, *iCloud* while
its original downloads; on Windows *Cloud* while a placeholder hydrates), and **✦ Auto** with a
chevron. Row 2: Undo · Reset · Compare, and **Save copy…** filled with the accent. Below 340 both
rows are icons only (Mac `ViewThatFits`; Windows hides the labels in place).

**✦ Auto** (`auto_enhance`, unbound, Edit ▸ Auto Enhance on the Mac): tone, then white balance,
one undo step ([07](07-photo-editing.md) "✦ Auto"). It fills with the accent while it is the last
edit, and a click then takes it back. The chevron opens the five Enhance tools (a popover on the
Mac, a `Flyout` of buttons on Windows: the Windows wand twin) with each one's state, and *Show the
Enhance tab*.

### The Enhance tab

Tab 5, any still. Five cards: **Remove objects · Restyle · Faces · Enhance image · Extend edges**,
each an icon, a one-line purpose, a state chip (*Ready*, *Brushing*, *Brushed*, *Brush full*,
*Needs Enhance*, *Needs People*, *Working…*) from the edit view's wand bits
(`adjust_pane.h` `kWand*`) and one primary button: *Start brushing* / *Remove* (the brush, then
the docked panel runs on what is brushed), *Set up Restyle*, *Set up* (Enhance image) and *Choose a
frame* (Extend), *Choose faces* (S8: arms the Faces tool; its chip reads *Choosing* and the
button *Done* while it is armed, and the Faces panel leads the tab). The Faces card also has
**Restore faces…** on a row of its own (docs/plans/edit-refresh.md S10; both hosts): Enhance's
docked panel on its `faces` tool, drawn under the Faces card (the card keeps Choose faces), or without it Enhance's `faces` command, its sheet
started on Restore faces ([29](29-enhance.md) "Restore faces"); enabled by `kWandRestoreFaces`
(1024: Enhance has that command); the ✦ popover / `Flyout` lists it as a row after Faces while
Enhance is installed. *Enhance image* offers its five models: Sharpen & restore, Natural,
Remove noise, Remove blur and Remove motion blur, each downloaded when first chosen (the docked
panel's and the sheet's *Improve* choice, [29](29-enhance.md) "Enhance image"). Without the Enhance
add-on a banner and each card offer **Get Enhance…** (Settings → Add-ons → Enhance). Cards are one
column, two from 560.

**Running inside the pane** (S5, owner call B). Remove, Restyle, Enhance image, Extend and Restore
faces (S10) ask Enhance for its docked screen (place `edit`, [29](29-enhance.md) "Docked in Edit"),
drawn under the chosen card with the add-on screens' renderer: the tool's knobs, the preview, the glowing progress
with the time left, Cancel, and the **result row** — **Show** (Edit closes, the new file or new
Photos-library photo is selected), **Keep editing** (the same with Edit reopened on this tab) and
**Move to Trash / Recycle Bin** (Enhance's, through `recycle_file`; never a hard delete, never the
original). The chosen card wears the accent outline and its own button gives way to the panel's
(Remove keeps Start brushing / Remove / Undo stroke / Clear). A crop draft is applied first, as
Save copy does. The panel closes with Edit, on another photo, or with its ×. Without a docked
screen (an older Enhance) each card opens Enhance's sheet instead. Hosts: Mac `enhanceTool:run:` /
`enhanceShowResult:keep:` (`mv_chrome_edit_enhance_tool` / `_result`); Windows `enhance_tool` /
`enhance_show_result` (`chrome_edit_action` 9–14, and 15 for Restore faces).

The tab's header carries Enhance's **performance level** as a chip (*Default level*; docs/plans/
edit-refresh.md S9), a click away from Settings → Add-ons → Enhance, whose first card is
Performance ([29](29-enhance.md) "The performance level"). The text is the add-on's own: the
`tierlevel` node of its settings page. That page's screen is open while it shows in Settings; the
tab opens it too while it shows (Mac `EnhanceLevel`, Windows `OpenEnhanceLevel`, closed a hop
after the tab goes unless Settings is drawing it) and keeps the last text read, since the level
changes only in Settings, whose trees pass through the same list. Without Enhance there is no chip.

How Remove and Extend fill (Remove's full-resolution second pass on large boxes, Extend's
overlapping tiles; quality over speed) is in
[29](29-enhance.md#remove-what-was-brushed-in-edit-filled-in).

### Canvas tools

docs/plans/edit-refresh.md S3. A **canvas tool** is what a press on the photo does while it is armed
(`shell/canvas_tool.h`, one model on both hosts): the **Remove brush**, **red-eye** and the
**white-balance picker**, and **Faces** (S8; Extend is S6's). The Adjust pane's picks, the
rail, the Enhance tab's cards, the ✦ popover / Windows `Flyout`, `B` and the Edit menu all arm the
same state; it is the host's, not the Colour pane's, so the brush and the red-eye ring work on any
tab and before the working image is built (the white-balance picker alone needs it, and says so).
Arming a tool opens the workspace on the tool's home tab, unfolded, and applies a crop draft first
(a tool and crop are exclusive). An armed tool stays armed on the next still; on a clip, an
animation or with the workspace closed it goes down.

- **Keys:** tool mode ([16](16-commands.md) "Canvas tools"): `[` `]` size, `Ctrl/⌘+Z` and
  `Ctrl/⌘+Shift+Z` one stroke or fix (with no stroke left, the edit's own undo), `Backspace` /
  `Delete` clear, `Enter` applies, hold `Space` to
  pan, `Alt` / `⌥` held erases, `Esc` puts the tool down with the strokes kept (dimmed). Rotate,
  flip, crop and the Bin are refused while a tool is armed, and walking away (`A` `D` …) while
  strokes are pending (owner call G); the notice says why. The menus grey the same.
- **The context bar** (owner call A): a 44 pt capsule, at most 640 wide, centred on the canvas,
  8 above the filmstrip, in the **transport's slot** (a still has no transport). The canvas leaves a
  56 pt band for it (`kToolBarBand` added to `chrome_bottom_px`; a `refit_seq` refit, never a
  swapchain resize), so the bar never covers the picture. Brush: Paint / Erase · Size (a log slider,
  "48 px") · Undo · Redo · Clear · "3 strokes" · **Remove ↵** · Done. Red-eye: "Click a red pupil ·
  2 fixed" · Size · **Find eyes** · Undo · Done. The picker: a line and Done. When the brush is full
  it says "Brush full: Remove, then brush more" instead of beeping. Its tooltip lists the live keys.
  The layout is fixed per tool: the status sits in a slot of one width (168 brush, 160 red-eye;
  shorter words when the bar is compact; the full text is its tooltip), the size readout is 48
  wide and Find eyes keeps its width while busy, so a stroke count appearing or going never
  moves a button.
  Mac: `ToolBarView.swift` in `toolBarHost` (`mv_tool_bar_view`, `mv_chrome_tool_*`); Windows: the
  transport's island holds `IslandHost.ToolBar.cs` (`SetToolBar`, `chrome_tool_bar_args`, 56 bytes;
  `chrome_cmd_canvas_tool` / `tool_size_set` / `tool_erase_set`, 1096–1098), Button, Slider and
  TextBlock only.
- **Sizes are screen pixels:** 4–400, the brush 40 and red-eye 24 by default. A dab's radius is the
  ring over the shown picture's long side on screen (`canvas_map::screen_long_px`), so a 40 px ring
  paints 40 px at any zoom.
- **The ring** replaces the OS cursor over the canvas (Mac: a blank `NSCursor`; Windows:
  `SetCursor(nullptr)` in `WM_SETCURSOR`; the picker keeps a cross, `Space` the open hand). It is
  drawn in the same present as the photo (`shell/tool_overlay.h`, replacing `brush_overlay.h`, one
  body for both labs): a 1.5 px dark ring outside a 1 px light one and a 2 px centre dot, dashed while
  erasing; the size label for 800 ms after a change. Colours are the theme's accent
  (`input_snapshot.home.accent`) and the ring's black and white.
- **Brush:** strokes are a stack of at most 64 strokes and 8192 dabs (`remove_brush`, dabs half a
  radius apart): `Ctrl/⌘+Z` takes one stroke off, `Ctrl/⌘+Shift+Z` puts it back until a new stroke
  starts, and **Clear is one more undo step** (undo puts every stroke back; the newest 8 Clears stay
  undoable). An erase dab takes back what earlier dabs painted, in the mask and in what Enhance is
  handed (`remove_brush::handoff`). Full, a press says "Brush full: Remove, then brush more". Put
  down, the strokes stay dimmed for Remove later; another item or a new geometry forgets them and
  their history (`reset`, not undoable). While strokes exist, rotate and flip are refused with
  "Remove or clear the strokes first" from any key, menu or button, the tool armed or put down
  (`blocked_by_strokes`); the Mac menu items grey.
- **The committed strokes are one texture** (`shell/brush_mask.{h,cpp}`, S4a), not a circle per
  dab: after a stroke ends, an undo, a redo or a Clear (and every 256 dabs of a long stroke, 32
  while erasing), one pool job at a time rasterises the dabs at 1024 px on the long side into an
  RGBA8 tint (the accent at 40 %, a darker 1 px edge, erase writes 0), stamping only the new dabs
  when the build extends the last one. The worker creates the texture **immutable**
  (`gfx::create_overlay_texture`, `D3D11_SUBRESOURCE_DATA`; `image::upload_overlay`,
  `MTLTexture` at creation) and hands it to the render thread by an atomic exchange; the overlay
  draws it as one ImGui image. It is at most 4 MB and made off the render thread, so the render
  thread uploads nothing. Only the dabs a **landed** build lacks travel in the snapshot (the
  newest `kBrushDabs` = 512 at most), so a finished stroke never vanishes while its build runs.
  An undo shows the old texture for the build's few milliseconds.
- **Red-eye:** with People on, the photo's eyes are read once per item on the pool (`mv_eyes_json`
  on Windows, `ai_eyes_json` on the Mac); the ring **snaps** to an eye within 1.5 ring radii and
  takes 1.1× its iris. A click lands a fix (one undo step) with a 300 ms pulse; each fix shows a
  ring and a check, and the one under the pointer a cross: **a click on a fix takes it off** (one
  step). **Find eyes** fixes every detected eye at least 12 % red as **one** undo step
  (`find_red_eyes`, on the pool); without People it says how to turn it on.
- **Faces** (docs/plans/edit-refresh.md S8, `shell/faces_tool.h`): armed, the photo's faces are
  read once per item on the pool (the pack's `eyes_json` with no candidates, then the still
  decoded for each face's thumbnail and `edit::eyes_look_shut`). The canvas draws one box per
  face: its **edit region** (`edit::face_region`: the detector's box grown 12 % a side, joined
  with the whole-face ellipse), corner brackets, a numbered badge the panel shares, the person's
  People name in a pill below, the accent and a faint wash when chosen, a dashed edge while a fix
  runs on it. Every face starts chosen; **a click on a box chooses it or leaves it out** (the
  pointer is a hand over a box). The bar reads "3 faces · 2 chosen" · Open eyes · Improve ·
  **Do all** · Done, on the chosen faces. The **Faces panel** (Mac `FacesPanelView.swift`,
  Windows `IslandHost.Faces.cs`) sits first in the Enhance tab while the tool is armed: each
  face's thumbnail (a click toggles it), its name from People (or *Unnamed person* / *Not in
  People yet*), "from 37 photos of Anna" (People's other stills of them), an *Eyes closed* tag,
  and its own fixes, **Open eyes** (only where the eyes look shut) and **Improve face**; then
  **Do all** (each chosen face's best fix in one file: a clearly better face of the same person,
  else open eyes where they are shut), All / None. Without Local search it offers **Get Local
  search…**; with People off, **Turn on People…** (Settings → Add-ons → Local search, where People
  is turned on), each with **Check again**; a failed read offers **Try again**. A refused or failed
  read is read again on the next arm, a walk, or those buttons (`faces_tool::needs_read`). Hovering
  a face's row lights its box on the photo. A fix left running when the photo changes stays marked
  on its own photo only (`faces_tool::run_item`). The fixes are a table (`shell::kFaceActions`, `addon::media::face_fix`): a
  fix is one more row there and in the chromes' tables (S10's Restore faces). A fix changes
  **only the chosen faces**: the run asks the pack again with the same moment's frames, matches
  the chosen boxes to that answer (`faces_mask_of`, intersection over union ≥ 0.3) and passes a
  `faces_mask`; each face is fixed on a crop and written back inside its edit region only, faded
  to nothing at its edge, so **every pixel outside the chosen boxes is bit for bit the
  original's** (`mv_ai_tests "[faces-mask]"`). Results are `<stem>-eyes.jpg` (Open eyes) or
  `<stem>-faces.jpg` (Improve, Do all), beside the photo, never over it. Windows reaches it all
  through `mv_eyes_json` and `mv_adjust_faces` (`main.cpp` `faces_fetch`, `adjust_faces`, the twin
  of the Mac's `-facesFetch` / `-adjustFaces:` / `-facesRun:action:`).
- **Presents:** the overlay animates only to its deadlines (the label, the pulse): the labs draw
  until then and once more to clear. An armed tool with the pointer still presents nothing.

### Width classes

The inspector's layout follows its width (`shell/pane_width.h` `class_of`), read once at the
root and handed down only when the class changes, so a drag of the edge rebuilds nothing:

| Class | Width | Adjust | Other |
|---|---|---|---|
| compact | 280–339 | one column, labels above the sliders, histogram 72, Curves the content width | header icons only |
| regular | 340–559 | one column, labels inline, histogram 96, Curves ≤ 340 | |
| wide | 560–719 | two independent columns of section cards (Light, Colour, B&W, Vignette, Detail · Levels, Curves, Selective Colour, Red-Eye; Windows adds Faces and Remove on the right), labels above | Crop's ratios in one row, straighten beside rotate; Enhance cards two columns |
| xwide | ≥ 720 | wide, labels inline, and a **scopes row**: the histogram and a square Curves side by side | |

`↑` `↓` walk the left column, then the right. Windows moves the same section cards, histogram and
Curves square between panels (never rebuilt, so slider state and focus survive). Each section is a
card with a chevron, a changed dot, an AUTO pill and ↺; a slider's value is a button that becomes a
field to type a number into (Return sets it, Esc leaves it; Windows uses `FakeInput`), and a
double-click on a slider (or its row) sets it back to 0. Exposure, saturation, vibrance,
temperature, tint and a selective band's hue draw their track as a gradient of what they do (Mac:
the pane's own `ToneSlider`; Windows: the `Slider`'s track resources). The Info pane's labels are
min(100, 30 % of the pane) wide.

### The right column

Every right pane (the workspace, Adjust, Info, Jobs) is one column with one width, the user's
(`shell/pane_width.h`, docs/design/12 2026-11-04). Its left edge drags: 280 to 720 wide (wider
minimums for a dense or large-type theme), at most 60 % of the window and never leaving the canvas
under 320; a release within 6 of 360 or 560 snaps there; a double-click resets to 360. View →
Wider / Narrower side pane step 40 (`pane_wider` / `pane_narrower`, no default key, remappable).
The width is kept across launches (Mac defaults `MVRightPaneWidth`; Windows `[view]
right_pane_dip`), saved when the edge is let go. While the workspace is docked the picture tracks
the edge exactly and springs into place on release; nothing resizes the swapchain
(`input_snapshot.refit_seq`). Nor the window: the transport's and the tool bar's preferred widths
sit just under `NSLayoutPriorityWindowSizeStayPut` on the Mac, so a narrow canvas narrows them
instead of growing the window.

- **Mac:** `NSLayoutGuide rightColumn` with one width constraint; every pane pins its leading edge
  to it. `MvPaneEdgeView` (6 pt, centred on the column's edge, above the panes, below the transport)
  shows the resize cursor and a hairline that turns into a 2 pt accent line with a grip while
  hovered or dragged; it is an accessibility splitter whose value steps like the commands. The Info
  pane's add-on sections are measured once on release, not per step. A theme change
  (`MVChromeThemeDidChange`) re-clamps the column to the new minimum and repaints the edge.
- **Windows:** the islands are child windows, so the grab strip is the main window's own client
  area, 6 DIP left of the column; `WM_SETCURSOR` shows `IDC_SIZEWE`, a press captures, moves re-lay
  the islands at most once per refresh interval, release (or `WM_CAPTURECHANGED`) snaps and saves.
  Mid-drag only the islands move: the Info and Adjust panes' content is pushed once, on release.
  The theme's minimum is 300 DIP × the type scale (or density), sent by the island with the
  theme (`chrome_cmd_inspector_min`). The edge lights like the Mac's: native tells the islands its
  state (`chrome_host::notify`, `chrome_cmd_pane_edge_state`: idle, hot, dragged) and the pane on
  top turns its left border to the accent at 2 DIP.

On both hosts a press that does not move the column leaves the preference as it was (a click on the
edge of a window too narrow for the wish keeps the wish), and a chrome change that does alter the
canvas's size or scale after all is a resize, not a refit.

The Adjust tab's ask field stays under the header; everything below it (histogram, readouts,
sections, Reset) scrolls. The histogram's corner triangles light when shadows crush or highlights
clip, and a click on either shows the blinkies (`C`). Curves is a square as wide as the pane's
content, at most 340, its plot inset 6 so the end points are whole and hittable; it shows "in → out"
while a point is dragged, a double-click removes an inner point, and the channel's own histogram
sits under the line in its tint.

## Architecture

| Layer | Piece | Role |
|---|---|---|
| Core, shared | `shell/edit_workspace.h/.cpp` | The pure model: `edit_tab`, which tabs a still or clip offers, the tab a command opens (`route_workspace`), `apply_step`, `follow_subject`. Unit-tested (`test_edit_workspace`) |
| Core, shared | `shell/edit_session` | `crop_aspect` presets and orientation, ratio-keeping resize, `A` / `X` in crop, `set_straighten(deg)`, `edit_count()` for the strip |
| Core, shared | `shell/commands.h`, `command_table.cpp` | `edit_workspace` (`Enter` in browse and video), `crop_aspect_cycle` (`A` in crop), `crop_aspect_swap` (`X` in crop), `show_original` / `show_original_release` (`Y` held, stills), and the island-only, keyless `crop_aspect_set` (argument = preset, +16 for portrait) and `crop_straighten_set` (argument = degrees) |
| Core, shared | `meta/write` | `write_fields::tags` (any Exif / Iptc / Xmp key, set or remove) and `date_taken`; `access_of()`; a whole-metadata snapshot that `revert` splices back |
| Core, shared | `meta/tables` | `editable_properties_table`: the properties table plus each tag's raw value and access |
| Mac host | `main_mac.mm`, SwiftUI | `EditView.swift` (the header, `CropPane`, `TrimPane`), `EditRailView.swift` (the rail, `railHost`), `EnhanceHubView.swift` (the Enhance tab and the ✦ popover), `EditStore` (polls one POD view by generation; the wand bits, chips and fold every pulse), the Edit menu, the command-bar button, `MetadataView`'s tag editor. Right-edge panes hang under the header while it shows (`metaTopUnder` …) and the canvas docks beside them and the rail |
| Windows host | `main.cpp`, WinUI | A fifth panel island (`IslandHost.Edit.cs`): the header and, under it, the Crop, Trim or Enhance pane (`IslandHost.EnhanceHub.cs`); on Adjust / Info / Jobs the island is the header alone and native places that pane under it. A sixth, the rail (`IslandHost.Rail.cs`, `ShowEditRail`, pane island id 11). One blittable `SetEditView` entry (`chrome_edit_args`, 104 bytes) feeds the header, the rail, the panes and the button. The island sends keyed command ids plus the notifications `edit_tab` (a rail click), `edit_action`, `edit_strip_height`, `inspector_min`, `meta_tags`, `meta_date`; native sends `Notify` (`pane_edge_state`). The canvas docks through `chrome_right_px` (`present_lab.cpp` `usable_canvas`). The metadata editor is in `IslandHost.Panels.cs` |

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

On a still: open → pick 3:2 (or 1:1) → Apply → Adjust → ✦ Auto and the Enhance tab (both hosts
log `auto_enhance:` with the op count before, after and after a second press, and
`prompt_temperature:` for a typed "a bit warmer"; capture `s4d-enhance`) → Info (set `Exif.Image.Artist`
and the date, read both back) → Original → Save copy → Esc → Revert. It checks that the copy's ratio
matches the preset within a pixel and that the original's SHA-1 is unchanged after the revert.
On a clip: open on Trim → arm → Jobs → Esc.

The behaviour it pins:

1. Crop with visible UI: Edit image → Crop → 3:2 → Apply → Save copy… gives a 3:2 copy; the
   original's bytes are unchanged.
2. `Enter` on a still opens the workspace; `Esc` cancels a crop draft first, then closes.
3. Undo steps back through the crop, Reset returns to the original, and holding `Y` shows the
   original while held and changes nothing.
4. Keyboard only: `Enter`, `Shift+C`, `A` until 3:2, `Enter`, `Ctrl+S`.

Tests: `test_canvas_tool` (`mv_tests "[canvas-tool]"`: home tabs, sizes, spots, snap, Find eyes,
the overlay's deadlines), `test_key_router` (`[tool]`: tool mode's keys, refusals and `Esc` order),
`test_remove_brush` (`mv_tests "[remove-brush]"`: undo, redo, erase, the caps, Clear undoable, a
4 px brush on a 6000 px photo at 400 %, the tint's stamp and edge, the builder's incremental build,
the sync's one-at-a-time builds and the snapshot's tail; the hidden `[.perf-bench]` case times the
snapshot publish, under 0.1 ms, and the tint build), `test_adjust` (`[canvas-tool]`: a fix taken off and
Find eyes are one step each), `test_edit_workspace` (tab 5 routing, the rail's fold), `test_auto_tone` (`[auto_enhance]`:
one result, one token, the take-back mark), `test_pane_width` (`mv_tests "[pane-width]"`: the clamp, the
rail-only collapse, the 60 % cap, snaps, steps, the drag soak's frame log), and the workspace cases
in `test_edit_session`, `test_key_router` and `test_meta_write`. The column drag's pacing is
`--pane-drag-soak` ([DEVELOPMENT](../DEVELOPMENT.md)).

## Not built

- On-canvas crop handles (drag corners, edges, inside).
- Redo (`Ctrl+Shift+Z`) and a named history list for a still's edit stack (the brush has its own
  redo); a split-screen before/after.
- Crop entered from a menu with strokes kept asks nothing yet (it clears them silently; the plan's
  "Discard 3 strokes?" in the bar is not built); Undo of an earlier rotate or Reset with strokes
  kept also forgets them. Extend as a canvas tool (S6).
- Faces: the panel cannot
  rename a person (People's own screen does); *Turn on People…* opens the settings rather than
  turning People on in place.
- Windows: a rebuilt D3D11 device drops the brush texture; the strokes reappear at the next brush
  change (the host is not told).
- Persisting a still's stack across sessions.
- Editor add-on panes docking as extra tabs.
