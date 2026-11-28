# Commands, keyboard, and mouse-free use

The command table, the key router, modes and focus, the default key map, and the chrome around
browse, overlays and search. The app is fully usable without a mouse.

| File | Holds |
|---|---|
| [`src/shell/commands.h`](../../src/shell/commands.h) | Ids, keys, modes |
| [`src/shell/command_table.cpp`](../../src/shell/command_table.cpp) | Default map, labels |
| [`src/shell/key_router.h`](../../src/shell/key_router.h) / `.cpp` | Mode and `Esc` resolution |
| [`src/shell/command_coverage.inc`](../../src/shell/command_coverage.inc) | Which host handles each id, and where |

Both hosts share the table and router. Tests: `tests/test_key_router*.cpp`,
`tests/test_command_coverage.cpp`, `test_command_contract.cpp`. The Mac's Settings list
(`MvCommandSupported`) is the Mac half of the coverage file, so a command cannot be dispatched
and unlisted or vice versa; an unhandled id is a row with a reason. A new `command_id` fails the
test until it has a row. A few Windows ids (`back`, island arguments) are handled outside
`run_command`; the coverage file says so.

## One table, editable

- `default_bindings()`: static rows `(key, mods, mode mask, repeat policy, command, hold,
  release)`. `live_bindings()` is a copy Settings remaps (`rebind_live`); Reset restores. Router,
  `?`, Settings and menu/tooltip labels all read the live table (`KeyOf` / `SettingsStore.keyLabel`).
- A remap onto a key held in an overlapping mode **swaps** the rows. Remaps persist **by row
  index** (`settings.ini [keys]` / `NSUserDefaults`), so rows are only **appended**; every row off
  its default (unbound included) is saved and restored exactly (`live_key_remaps`,
  `apply_key_remaps`, `set_live_key`).
- **Mac menu bar** chords read `menu_chord` from the live table; Settings refuses chords fixed
  menu items own (`⌘H` `⌘Q` `⌘M` `⌘⌫`); mid-crop, items with no crop row are off (`command_live_in`).
- Ids share the island's `chrome_command` wire space: 1–19 predate the table, 15/16/18/19/20 are
  island → native notifications, 76 retired. Never renumbered.
- Key `none`: in Settings, not routed or in `?`. Island-only commands (`zoom_preset`,
  `select_item`, `gallery_activate`, sliders…) carry an argument and have no key.
- **Keymap packs** ([25](25-open-addons.md#7-the-contribution-model)): Settings → Keyboard
  shortcuts → **Key layout**. **Apply** resets to defaults and writes the layout once
  (`shell/keymap.h` `apply_keymap`); then it is the person's table. Commands are `command_slug`s
  or `<add-on id>/<command id>`; keys fill that command's rows in table order (`""` unbinds), so a
  layout cannot add rows. A taken key leaves the other row unbound (no swap). Unknowns are skipped
  and listed. Contributed rows' remaps save by name (`live_addon_key_remaps`, `[addon_keys]` /
  `mv.addonKeys`).
- **Add-on commands** (`open_import`, `import_now`, `search_*`) exist only while their add-on is
  loaded (`set_addon_commands_available`); else the key falls through, except `Ctrl+F` → file search.

## One key router

UI thread, before the island's pre-translate; pure (no platform calls, allocation or I/O).
Platform keys become `mv::shell::key` at the host edge (`main.cpp` / `main_mac.mm`).

1. A focused text control gets keys (with standard editing chords); `Esc` blurs.
2. Else `(key, mods, mode)` → row via an O(1) index, rebuilt when the table changes.
3. Mouse, wheel and springs stay native on the canvas.

**Keys.** Letters upper-case; symbols bind to the layout's **character** (`?`, `+`, `\`) with
Shift cleared; AltGr symbols do not resolve. macOS: `⌘`/`Control` → `Ctrl`, `Option` → `Alt`.

**Repeat policy.**

| Policy | Behaviour |
|---|---|
| `edge` | Down edge only; typematic repeats fall through |
| `repeat` | Every down, including repeat |
| `tap_hold` | Command on the down edge; first repeat makes it a hold (`hold` per repeat), key-up sends `release`. `Q` / `E` on a clip |
| `momentary` | Command on down, `release` on up (hold `Z`, `\`, `Y`) |

Up to four held keys; deactivation, Settings, focus loss or a table rebuild releases them
(`cancel_holds`). Keyboard pan: a tenth of the canvas per step. In fullscreen, `↓` at fit reveals
the strips for 3 s.

### Modes

Resolved from state at every keydown (`resolve_mode`), never a stack, in this order:

| Mode | When | Notes |
|---|---|---|
| **gallery** | Gallery grid visible, no popup | Owns navigation even with canvas focus. Unmodified printable keys reach only gallery nav, `G`, `F`, `?`, `/`; other letters are typeahead |
| **island** | Filmstrip or gallery island focused | XAML owns in-pane traversal; printable keys are typeahead; named keys and chords route |
| **crop** | Crop mode on a still | Own keys; `A` `D`, `G`, `Ctrl+O` do nothing |
| **tool** | Canvas tool armed on a still (`shell/canvas_tool.h`), not in a slideshow | Over **browse**. `blocked_in_tool` always refuses rotate, flip, crop, Bin; refuses walking away while strokes are pending, saying why once. Menus and buttons follow the same rule |
| **trim** | Trim armed on a clip, not in a slideshow | Over **video** |
| **runner** | Empty-window game up | `Space` jump/retry, `3` 2D/3D |
| **loupe** | `Z` held | Over browse/video: only arrows and `Z` / `\` differ |
| **slideshow** | Slideshow running | |
| **skim** | Clip with filmstrip hidden (`T`) | Over **video**: `←` `→` skip ±2 s like `Q` / `E` |
| **video** | Clip or animation | |
| **browse** | Otherwise | |

The command bar and transport are not modes: focused, `A` / `D` still walk and `Q` / `E` skip.

### `Esc` walks out

`resolve_back` picks one target per press, never quits: text field → popup (`?`, go-to, find) →
Settings → Live Photo motion → selected text → crop (cancel) → canvas tool (put down) → trim
(disarm) → pane → gallery → slideshow → fullscreen → runner → canvas focus → result list ("Back
to folder"; from the empty window, back to it). Nothing to leave: not handled. A confirmation in
a pane field takes the first `Esc` (Windows: `SetMetaEdit` returns `S_FALSE`).
`Ctrl+W` / `Alt+F4` close the window.

## Focus

Visible rings: **canvas** (default), **filmstrip**, **gallery**, **pane** (folder tree, metadata,
adjust, Edit, jobs), command bar, transport. `Tab` / `Shift+Tab` cross the island boundary; `Esc`
returns to the canvas; fullscreen hides chrome. Windows `Tab` order from the canvas: command bar,
folder tree, gallery, Edit, metadata, adjust, jobs, Edit icon rail, transport, filmstrip, AI badge,
canvas (`IslandHost.FocusOrder`).

Only a clip's **transport bar** floats over the canvas (`shell/transport_autohide.h`): playing,
it hides after `kTransportIdleMs` (2.5 s) until input wakes it; it stays while paused, hovered,
focused, scrubbing or with a screen reader. Visual only, no repaint while idle.

The **AI badge** ([06](06-metadata.md#the-ai-badge-345)) sits bottom-right, is in the focus
order, binds no keys, and never presents.

## Settings

`Ctrl+,`. View preferences are one flags word (`shell/settings.h` `view_settings`:
`settings.ini [view]` / `mv.viewFlags`). Sections: Filmstrip, Browsing, **Viewer** (AI badge, on;
stored inverted as `kSettingHideAiBadge`), File types, Appearance, updates, **Storage**
([04](04-image-pipeline.md#thumbnail-cache-upkeep)), add-ons, the key table. Changes apply at once.

## Default map

FastStone / IrfanView muscle memory. Mode masks: **Viewing** = browse, video, island, gallery;
**Walk** = browse, video, slideshow; **All but crop** = every mode except crop. Keys are
Windows-style; on macOS read `⌘` for `Ctrl`.

### Browse

| Key | Modes | Command |
|---|---|---|
| `←` `→` | Walk, gallery | Previous / next (repeat). On a clip with the filmstrip hidden: skim (`Q` / `E`) |
| `A` `D` | All but crop | Previous / next, clips included |
| `Backspace` | Walk | Previous |
| `Space` | browse | Next. Clip/animation: play / pause; slideshow: pause; runner: jump |
| `Home` / `End` | Walk | First / last |
| `PageUp` / `PageDown` | Walk | Back / forward ten |
| `Ctrl+PageUp` / `Ctrl+PageDown` | Browse, island | Previous / next page of a multi-page file (TIFF, PDF, DOCX, PPTX, ICO sizes, HEIC top-level images); notice "Page n of m" |
| `F5` | browse, video | Slideshow |
| `F` / `F11` | all | Fullscreen |
| `Ctrl+O` | All but crop | Open media… |
| `Ctrl+Shift+O` | All but crop | Open folder… |
| `Ctrl+E` | all | Reveal in Explorer / Finder; a Mac Photos library item opens in Photos ([26](26-photos-library.md)) |
| `Ctrl+W` | all | Close window |
| `Ctrl+N` | all | New window (also File ▸ New Window and the Dock menu on the Mac) |
| `Ctrl+,` | All but crop | Settings |
| `Ctrl+G` | Viewing | Go to index. Mac: not built |
| `/` | browse, video, gallery | Find by name, then tag, in the listing (gallery folder row: a folder tile) |
| `Ctrl+Shift+E` | All but crop | Folder tree: show and focus |
| `Ctrl+↑` | Viewing | Up one folder, selecting the folder left |
| `Ctrl+←` / `Ctrl+→` | browse, video | Previous / next sibling folder |
| `Ctrl+[` / `Ctrl+]` | Viewing | Back / Forward through folders, each with the selection it had ([Folder navigation](#folder-navigation)) |
| `Ctrl+Shift+N` | Viewing | New folder in the folder on screen |
| `Ctrl+V` | Viewing | Paste files here: the clipboard's files copied into the folder on screen |
| `Enter` | browse, video | Edit workspace |
| `?` | all | Cheat sheet for the current mode |
| `F3` | all | Frame-time overlay |
| `R` | Viewing | Reset frame-time stats |
| `Esc` | all | Back (above) |
| *(unbound)* | browse, video | **Open RAW of pair** / **Open JPEG of pair**, listed in Settings. Mac: not built |
| *(unbound)* | Viewing | **Show next kind**: cycle View ▸ Show (All → Photos → Videos → RAW → Marked → Picks → Not rejected → Red … Purple; Marked skipped when nothing is marked) |
| *(menu)* | — | **Recent folders** (Open ▸ Recent folders / File ▸ Open Recent); pinned lead, vanished ones drop |
| *(context menu)* | — | **Pin to top / Unpin** on a welcome-card folder row |

### View

| Key | Modes | Command |
|---|---|---|
| `0` `1` `2` `3` `4` | Viewing | Fit / 100 % / 200 % / 400 % / Fill. Ratings never take the number row |
| `Ctrl+0` | Viewing | Reset pan / zoom |
| `+` (`=`) `-` | browse, video, island | Zoom in / out toward the cursor, else the centre |
| `+` (`=`) `-` | gallery | Thumbnail size: 24 DIP steps, 80–344, initially 152; kept for the session |
| `↑` `↓` | browse, video | Pan when zoomed. Fitted clip: volume ±10 % (carries across clips) |
| `Shift+arrows` | browse, video | Pan in four directions |
| `G` | All but crop | Gallery |
| `W` `S`, `↑` `↓` | gallery | Previous / next row |
| `Enter` | gallery | Open the selection in the viewer (leaves fullscreen / slideshow, restores the filmstrip if enabled) |
| `T` | all | Filmstrip show / hide (stored per mode: folder or single image) |
| `Shift+T` | browse, island | Page sidebar show / hide (below). Remembered (`[view] page_sidebar` / `mv.pageSidebar`), on by default. Also View ▸ Page Sidebar |
| *(unbound)* | browse | **Auto enhance** (`auto_enhance`): the Edit header's ✦ Auto ([07](07-photo-editing.md) "Auto") |
| *(unbound)* | Viewing | **Wider / Narrower side pane** (`pane_wider` / `pane_narrower`): right column ±40, 280–720 ([20](20-edit-workspace.md) "The right column") |
| `S` | browse, video, island | Sticky zoom on advance |
| `C` | Viewing | Clipping blinkies |
| `O` | Viewing | On-canvas info overlay (filename, index, exposure triangle) |
| `Shift+O` | Viewing | AF-point quads from maker notes already read |
| `Shift+Z` | browse | Zoom to the AF point: 100 % on the in-focus point (`shell/af_focus.h`); "No AF point recorded" when none |
| `I` | Viewing | Metadata pane, focused: `←` `→` tabs, `↓` tag search; `Esc` to canvas, again closes |
| `Shift+I` | Viewing | Eyedropper: one-pixel sRGB 8-bit + hex under the cursor (stills) |
| `Ctrl+C` | Viewing | Selected words ([Text in images](#text-in-images)) → text; eyedropper on → `#RRGGBB  rgb(r, g, b)  x y`; else the marked (else current) file(s) |
| hold `Z` | browse, video | Loupe: 100 % around the cursor or a keyboard point; arrows nudge. Same texture |
| hold `\` | browse, video | Previous item for burst pick, from the GPU LRU (a 512 MB byte budget, 3–12 entries) (Mac: the last still, kept on the GPU) |
| `\|` | browse, video | **Two-up compare** toggle (below); `--compare` starts with it on |
| `;` | browse, video | Play a Live Photo's motion once with audio; `;`, `Esc` or navigation return early. Mac: not built |
| `Ctrl+Shift+A` | all | Always on top (`HWND_TOPMOST`) |

**Two-up compare** (`shell/compare.h`): current still right, the previous still's resident
texture left, one present, no decode; pan/zoom/`0`–`4` move both at the same fit-relative scale;
walking keeps the split. Stills only (one-up on a clip, in crop, under blackout).

**Page sidebar.** Filmstrip hidden + multi-page PDF/DOCX/PPTX: page thumbnails down the left,
current outlined, click turns the page. Never with the filmstrip; reserves its width; hidden
under gallery, Settings, chrome-off fullscreen. Rendering: [04](04-image-pipeline.md) "Page sidebar".

**Filmstrip hint** (`shell/filmstrip_hint.h`; `IslandHost.HintNotice.cs` / `HintNoticeView.swift`):
a bottom-right card, "Press T to hide the filmstrip and scroll" (document) or "…and skip 2 s with
← →" (clip), when the strip changes what input does. Fades after 8 s, no focus; once `T` is used
or "Got it" clicked it never returns (`[hints] filmstrip_t` / `mv.hint.filmstripT`).

**Gallery.** Full-client grid: item / row navigation, click opens a photo. Child folders are tiles
ahead of the photos in the same grid (Mac; Windows keeps its folder row in mixed folders); one
click selects a tile, a double-click or `Enter` opens it, `/` finds one. Opening the gallery pauses
a clip; closing resumes it. Folder path sits in the command bar left of `?`, with Back, Forward, Up
and Root.

#### Folder navigation

Finder's and Explorer's model on both hosts (`shell/browse_history.h`, beside `browse_path.h`):

- Every move to another folder (tile, crumb, Up, sibling, tree, sidebar place, Open, Recent) records
  the folder left with its selected item and folder tile; a result list is no place. `Ctrl+[` /
  `Ctrl+]` (Go ▸ Back / Forward on the Mac, the bar's chevrons) reopen it with that selection; a new
  move clears Forward. 64 deep; a folder that went is forgotten.
- Another computer's folder has the same bar: server › named folder › … (`remote::key_chain`,
  names from the server), Back / Forward / Up, crumbs that take drops; only a search result list
  shows its title and count instead. The grid starts right of the folder pane, never under it.
- The grid's scroll comes back with Back / Forward (Mac `GalleryScroll`, per folder); any other
  arrival starts at the top on what the host selected. Listings load on the pool as before.
- The folder pane (`Ctrl+Shift+E`, Mac) leads with places: the home folders, every mounted volume
  (re-read on mount / eject, off the main thread), paired servers, the Photos library, then the
  trail's tree. Each row, tile and crumb is a drop target.
- Type-to-select is `/` (letters are commands here).

#### Drag and drop into folders

One rule for every target and both chromes, `shell/file_drop.h` (`decide_drop`), asked on each
drag-over with no I/O (Mac `mv_chrome_drop_verdict`, Windows a synchronous `WM_COPYDATA`):

- A drag from this window **moves**, `⌥` / `Ctrl` copies. A drag from Finder / Explorer or another
  app **copies** ("adds"), `⌘` / `Shift` moves. The grid is the folder on screen; a tile, a crumb or
  a sidebar place is that folder. With no folder open a drop opens what it carries, as the canvas.
- Refused, with words on the target: read-only places (Photos library, read-only lists), another
  computer's folder this device may not change (the server's reason, plan §6), a folder into
  itself, a move onto the folder it is already in, a remote folder leaving its server, and files
  of one server going to another. A remote file dropped on a local folder comes down; a move
  deletes it on the server (into its trash) only after the local copy reads back.
- Feedback: accent ring + wash on the target (Mac), a dashed / thin body ring when refused, a line
  "Move 3 items to Trip" at the foot of the grid (Mac) or the drag caption (Windows). A tile rested
  on for 0.7 s opens (spring-loading).
- The work is `shell/file_transfer.h` on the pool: never replaces (`name (2).ext`), a move on one
  volume is a rename, across volumes a verified copy then removal (a file that will not go takes
  its copy back), a failure removes what it wrote. Each run is a row in the Jobs pane beside clip
  jobs (Mac; it opens after 1.2 s if still running) and ends with a notice. `transfer_target` is the
  one seam; `mixed_target` sends `remote:` paths through the host's remote library
  ([desktop-client §6](../plans/desktop-client.md)).
- Keyboard twins: Paste files here (`Ctrl+V`), New Folder (`Ctrl+Shift+N`; the Mac names it first),
  Move to… / Copy to… (`Shift+F8` / `Shift+F7`).

**Home** (owner 2026-11-18): the first item of the bar's **Open** menu on both hosts (Mac
`kMenuHome`, Windows `chrome_cmd_go_home`). Whatever is open closes (a slideshow, the Edit
workspace, Settings, the folder or a list) and the welcome card lists the pinned and recent
folders again; nothing on disk changes. The Video Editor with an edit not exported asks first,
and Home stops if the edit is kept. No key.

#### Gallery range and share (#413)

- **Shift-click** selects anchor → clicked cell in grid order; the grid stays open. Arrows never
  grow it; another selection, folder or relist ends it.
- While it stands it is the selection for copy, `F7` / `F8`, `Delete`, copy path and Share, ahead
  of marks.
- **Drag-out** from any cell (Mac: `GalleryDrag`): a range cell drags the range, a marked cell the
  marks, else itself.
- Share is the command bar's one button beside Home (below); there is no second one over the grid.

#### Share (#444)

One command, `share` (id 149, `Ctrl+Shift+S` / `⌘⇧S`), from the key, the command bar's one share
button beside Home, File ▸ Share… / Open ▸ Share…, and **Share ▸** on the right-click menu of a
gallery cell, filmstrip cell and the canvas. Shared part: `shell/share_menu.h`.

- **Files**: range, else the outlined folder tile, else marks, else current; right-click on an
  unselected file shares it alone (`share_stops`). The bar button is off with none of them, and
  its name says what it sends ("Share 12 files", "Share the folder …").
- **A folder** (`share_takes_folder`: gallery open, keyboard on a folder tile, no range): the folder
  path, no media. Driftwire walks and sends it whole, as its own **Send a folder** does; Windows
  Share is greyed (it takes files) and the Mac goes straight to Driftwire. A folder tile's or chip's
  right-click has **Share ▸ Driftwire** for that folder (the chrome holds the path). Not for a
  Photos library album, which is no folder on disk.
- **Menu**: [Driftwire](28-driftwire.md) first (or its install), then Windows Share
  (`DataTransferManager`) / `NSSharingServicePicker`. Picking sets `share_target`. Driftwire gets
  pair halves and XMPs; the system gets media only (`share_system_files`).
- Nothing read until asked, nothing sent until picked (rule 6); Windows finds pair halves on the
  pool (`mv_folder_entries`).
- Mac, past 50 files: the picker gets one file per kind and wrapped targets receive all (it costs
  ~0.09 ms/file on the main thread).
- **Wire**: float `(clicked + 1) * 16 + from * 4 + target` (`pack_share_request`); native answers
  via `ShareMenu` (`chrome_share_args`) / `MVChromeHost.presentShare` (`mv_chrome_share`).

### Marks, copy, move

Marks are a set apart from the selection, so arrow browsing never makes ranges. The gallery's
Shift-click range, while it stands, takes precedence.

| Key | Modes | Command |
|---|---|---|
| `Insert` / `Shift+Space` | browse, video | Toggle mark on current |
| `Ctrl+A` | Viewing | Mark all; with words selected in the still, select every word |
| `Ctrl+D` | Viewing | Unmark all |
| `F7` / `Shift+F7` | Viewing | Copy marked (else current) to the last destination / pick a folder |
| `F8` / `Shift+F8` | Viewing | Move, same rule |
| `Delete` | browse, video, gallery | Recycle Bin / Trash, with confirm: range, else marks, else current |
| `F2` | Viewing | Rename the current file (gallery: selected cell); also File ▸ Rename… on the Mac |
| `Ctrl+Shift+C` | Viewing | Copy path(s) as text |
| `Ctrl+Alt+C` | browse | Copy the current still with edits baked, as PNG (worker) |
| `Ctrl+Shift+S` | Viewing | Share menu |
| `Ctrl+P` / `⌘P` | Viewing | Print ([Print](#print)) |

Also in the Mac File menu and the Windows command bar's **Open** flyout; **View** has Start
Slideshow and **Go**. Each item posts its command id.

- **Move**: same volume = rename; across volumes a **verified** copy ([24](24-transfer.md)), then
  delete. Copy / move never overwrite (`name (2).ext`); last five destinations kept.
- **Rename** (`F2`): item on screen only, extension kept; pair half and XMP sidecars move under
  one stem (`io::rename_stop`, [24](24-transfer.md) "Rename"), no re-decode. Refused in a result
  list, for a Photos library file, with unsaved edits or a write in flight, or for invalid names.
- **Delete**: Recycle Bin / Trash only; where there is none the item is refused and counted.
- **Drag-out**: cells (and the canvas) drag originals plus pairs as `CF_HDROP`, copy-only to
  other apps; a marked cell drags the marks. Inside the app they move into folders
  ([Drag and drop into folders](#drag-and-drop-into-folders)).
- **Opening** (argv, drop anywhere): first existing entry wins; a file opens its folder with it
  selected; nothing existing beeps. A watcher refresh keeps the current item (else the next).

### Print

`Ctrl+P` / `⌘P` (also File ▸ Print… and Open flyout) opens the OS print dialog. Not mid-crop, not
in a slideshow. Plain `P` is trim preview.

- **What**: ≥ 2 marks (or a range) → N-up **contact sheet**; else the **still on screen** baked
  as Export bakes it (`edit::render_picture`). No still → beep.
- **Layout** ([`shell/print_layout.h`](../../src/shell/print_layout.h)): ≥ ¼ in margin; one photo
  **fit** or **fill**; sheets **2×2, 3×4, 4×5** or *Automatic*; optional file names. Options live
  in the OS dialog (`PrintDlgEx` page / `NSPrintPanelAccessorizing`), not saved.
- **Threads**: the bake runs on the pool, shrunk to 300 dpi (`edit::shrink_to_box`); Windows
  draws pages on the pool, the Mac uses `NSPrintOperation` with `canSpawnSeparateThread`.
- **Colour**: sRGB (D6), or the source ICC with no colour adjust. Nothing is written to disk.

### Edit (stills)

| Key | Modes | Command |
|---|---|---|
| `[` `]` | browse, crop | Rotate −90° / +90°. A JPEG with nothing else in its stack is rewritten losslessly, atomically, 0.4 s after the last key; the preview turns at once |
| `H` / `V` | browse, crop | Flip horizontal / vertical (lossless on JPEG, like rotate) |
| `Shift+C` | browse | Crop / straighten mode |
| `Shift+A` | browse | Adjust pane, focused (`↑` `↓` slider, `←` `→` change, `Shift` ×10, `0` zero, `A` Auto, `R` reset all); again hides |
| `Ctrl+Z` / `Ctrl+R` | browse, crop | Undo last edit / reset to original |
| `Ctrl+S` | browse | Export to a new file beside the original (`<name>-edit.jpg`); never overwrites |
| hold `Y` | browse | Show the original |
| `Enter` | browse, video | Open / close the **Edit workspace** ([20](20-edit-workspace.md)). `Shift+C` / `Shift+A` / `Ctrl+T` open it on Crop / Colour / Trim; open, `I` / `Ctrl+J` select Info / Jobs |

### Canvas tools

`shell/canvas_tool.h`, [20](20-edit-workspace.md) "Canvas tools". Rows in mode **tool**; else the
browse row, minus what `blocked_in_tool` refuses.

| Key | Modes | Command |
|---|---|---|
| `B` | browse, tool | **Remove brush** (`tool_brush`): Edit on the Enhance tab, brush armed. `Ctrl+B` stays clip split |
| `[` `]` (repeat) | tool | **Smaller / Bigger brush** (`tool_smaller` / `tool_bigger`): ×1/1.2 / ×1.2, 4–400 px, fading size label. Never rotates here |
| `Ctrl+Z` / `Ctrl+Shift+Z` | tool | **Undo / Redo stroke** (`tool_undo` / `tool_redo`); red-eye: one fix; brush with no strokes: the edit's undo |
| `Backspace`, `Delete` | tool | **Clear strokes** (`tool_clear`); red-eye: all fixes as one step. Never the Bin |
| `Enter` | tool | **Apply tool** (`tool_apply`): Remove; otherwise puts the tool down |
| hold `Space` | tool | **Pan while brushing** (`tool_pan` / `tool_pan_release`). Never next |
| `Alt` / `⌥` held | tool | Brush erases (ring dashes); no row, read at the press |
| `Esc` | tool | Tool down (strokes kept, dimmed) |
| *(unbound)* | browse | **Red-eye tool** (`tool_red_eye`), **Faces tool** (`tool_faces`), **Extend edges** (`tool_extend`): from the rail, Enhance tab and Settings |

### Crop

| Key | Command |
|---|---|
| Arrows | Move the rectangle 1 % of the frame |
| `Shift` + arrows | Move its bottom-right corner |
| `,` `.` | Straighten −0.5° / +0.5° (±45°); an untouched rectangle tracks the largest fit |
| `[` `]` `H` `V` | Turn / flip with the draft carried (no file write while cropping) |
| `A` / `X` | Next aspect preset (Free, Original, 1:1, 4:3, 3:2, 16:9, 5:4) / swap orientation (`X` also turns Extend's ratio; elsewhere in tools it stays Reject) |
| `Enter` | Apply: the draft joins the edit stack |
| `Esc` | Cancel the draft |

Mouse: an edge or corner drags it, inside moves it (locked presets keep ratio), elsewhere pans
(`shell/crop_drag.h`).

### Rate

| Key | Modes | Command |
|---|---|---|
| Numpad `0`–`5`, or `Ctrl+Shift+0`–`5` | Viewing | Rating (0 clears) |
| `Ctrl+I` | Viewing | Metadata pane on *All tags* (Windows: comment editor focused; Mac: its search) |
| `X` | browse, video | Reject (rating −1); on a rejected file, clears to unrated |
| Numpad `6`–`9`, or `Ctrl+Shift+6`–`9` | Viewing | Colour label Red / Yellow / Green / Blue; the existing label clears |
| `U` | browse, video | Clear the colour label |

Writes the item on screen only, on the I/O pool: JPEG in place, else an XMP sidecar ([06](06-metadata.md)).
Labels are `xmp:Label` (Red … Purple; Purple has no key). Not in slideshow or crop. macOS
reserves `⌘⇧3`–`6`; use the keypad.

### Video and trim

| Key | Modes | Command |
|---|---|---|
| `Space` | video | Play / pause |
| tap `Q` / `E` | video | Skip −2 s / +2 s, exact, on the down edge |
| hold `Q` / `E` | video | Skim ∓2 s per repeat with keyframe seeks; release settles exactly; intent accumulates |
| tap / hold `←` `→` | skim | As `Q` / `E`, only with the filmstrip hidden |
| `Shift+Q` / `Shift+E` | video | Speed down / up: 0.25 / 0.5 / 1 / 1.5 / 2 / 4. The transport's `1x` menu is a view of this |
| `J` `K` `L` | video | −10 s / pause / +10 s |
| `,` `.` | video | Frame step (also animations) |
| `Shift+M` | video | Mute. The transport's **More** flyout (volume, mute, audio track, A–B loop) is a view of native state |
| `Ctrl+T` | video, trim | Arm / disarm trim |
| `[` `]` | trim | In / out marker at the playhead |
| `P` | trim | Preview the cut: A–B loop over what the keyframe save writes; a More-flyout loop replaces it |
| `Enter` / `Shift+Enter` | trim | Save keyframe cut (instant) / frame-accurate re-encode |
| `Ctrl+X` | trim | A copy without in–out |
| `Backspace` / `Delete` | trim | Clear markers (never trashes the clip) |
| `Ctrl+←` / `Ctrl+→` | trim | Previous / next keyframe |
| `Ctrl+J` | Viewing, trim | Jobs pane: `↑` `↓` choose, `Delete` cancel, `R` retry, `Enter` reveal |
| `Ctrl+S` / `Ctrl+B` | video, trim | Clip tools (rotate, split, frame, audio, remux, GIF / WebP) / split at the playhead |
| Media keys | — | SMTC (Windows) / `MPRemoteCommandCenter` (macOS) |

### Slideshow

| Key | Command |
|---|---|
| `F5` | Start (from browse or video) |
| `Space` | Pause |
| `+` (`=`) `-` | Interval |
| `.` | Blackout (canvas idles) |
| `R` | Shuffle |
| `Esc` | Leave |

Fullscreen while running. Next is the browse command on a UI-thread timer (zero presents between
advances, no transition); a playing clip advances at the later of interval and end. Intervals
1/2/3/4/5/7/10/15/20/30/60 s, default 4 s. Mac: stills only. Keeps the display awake while running.

### Import (while the add-on is installed)

| Windows | Mac | Command |
|---|---|---|
| `Ctrl+Shift+I` | `⌘⇧I` | Open the Import window (viewer marks as "Marked in viewer") |
| `Ctrl+Shift+F7` | `⌘⇧F7` | Import marked (else current) now with the last preset |

### Enhance (while the add-on is installed)

| Windows | Mac | Command |
|---|---|---|
| `Ctrl+Shift+R` | `⌘⇧R` | Open Enhance for the marked (else current) file: the sheet for audio or a clip, the Edit pane's Enhance tab for a photo ([29](29-enhance.md)) |

The Import window's own keys (not table rows) are in [18](18-import.md).

### Driftwire (while the add-on is installed)

One row, **Driftwire**, no default key: opens Driftwire's window with marked or current files. It
is the add-on's manifest command ([28](28-driftwire.md#host-pieces-this-added)).

### Add-on commands (PR 57, from scripts)

Scripted add-ons ([25](25-open-addons.md)) fill the eight `addon_cmd_*` slots; rebindable. A key
that clashes with a built-in or earlier row is dropped and Settings says why (`set_addon_commands`,
`addon_command_notes_json`). Running posts to the add-on's worker; keys never wait on a script.

### Search

| Key | Modes | Command |
|---|---|---|
| `Ctrl+F` | Viewing | **With Local search:** its panel, focused. **Without:** file search |
| `Ctrl+Shift+F` | Viewing | Find similar to the still or paused frame (Local search) |
| `Ctrl+Shift+P` | Viewing | Find this person: largest face in the still or paused frame (Local search, People on) |
| `N` / `Shift+N` | video | On a clip opened from results: next / previous matching moment |

Also from the search icon on the command bar's folder path. Local search field keys:
[17](17-local-ai-search.md).

**File search** (base app, no index): opens "Find files by name or tag in this folder" over the
gallery. Typing filters the grid by case- and accent-insensitive substring (~70 ms debounce, "N
of M"); viewer and filmstrip still walk the whole folder. `Enter` / `↓` to the first match; `Esc`
clears, then closes. Names are folded once per listing off the UI thread; a keystroke is one pass.

**Tags.** Name matches first, then files whose tags hold every word ([06](06-metadata.md)
"Search text"), with the matching tag under the tile. From the background tag pass, never a read
per keystroke (`mv_chrome_tag_search` / `mv_folder_tag_search`); results grow as the pass reads
(re-run on `tag_generation`). Place names come from the Places add-on's provider
([25](25-open-addons.md) §7). Order: names, tags, providers.

`/` opens the same search on the Mac; on Windows it is the find box (first name match, else
first tag match).

### `?`

Mode-sensitive cheat sheet from the live table (`describe_commands()`), as if the canvas had
focus: a XAML flyout (`ShouldConstrainToRootBounds = false`, never on the swapchain) / a SwiftUI
overlay. No command palette.

### Empty window

`Space` starts the T-Rex runner: `Space` jumps / retries, `3` toggles 2D / 3D, `Esc` leaves (the
scene folds away before the welcome card). Opening a file skips that.

## Folder tree

Left strip, hidden by default; `Ctrl+Shift+E` shows and focuses (`↑` `↓` walk, `→` `←`
expand/collapse, `Enter` opens, `Esc` to canvas, again closes). Places above it on the Mac
([Folder navigation](#folder-navigation)). Names only, from `mv_list_subdirectories` on a worker. Rooted at the browsing trail's root (`shell/browse_path.h`),
the open folder expanded and marked; opening outside the trail starts a new root.

## Overlays on the hot path

Extra draws in the same present; a still still idles to zero presents
([rendering](03-rendering.md)). Toggles request one redraw.

| Overlay | Cost |
|---|---|
| Loupe (hold `Z`) | Camera / viewport change; same texture |
| Hold-previous (hold `\`) | The GPU LRU; no second session |
| Two-up compare (`\|`) | Second blit of the resident previous texture; same present |
| Clipping blinkies (`C`) | Shader; may present while on (animates), labelled in F3 |
| Pixel grid (≥ 400 %) | Shader |
| Around the picture (no key) | The theme's `canvas` token as clear and in the blit ([12](12-decision-log.md)) |
| On-canvas info (`O`) | Text from the folder model and metadata already read |
| AF-point quads (`Shift+O`) | A few quads from maker notes already read |
| Zoom to AF point (`Shift+Z`) | Camera target only (`shell/af_focus.h` → `camera::zoom_to`) |
| Eyedropper (`Shift+I`) | One-pixel staging readback on demand |
| Histogram | Adjust pane, after the sliders settle |

## Status, sort, filter, typeahead

Chrome, in memory, no decode.

- **Status:** filename, index / count, dimensions, zoom, rating (title bar), from the folder model.
- **Sort:** name, modified, size, type, date taken; asc/desc (`mv_folder_set_sort`, `[view] sort`
  on Windows; View ▸ Sort By). Date taken comes from the tag pass (cached in meta_cache.db); the
  listing re-sorts in place when it ends; no stamp → mtime.
- **Show** (View ▸ Show): **All**, **Photos**, **Videos**, **RAW**, **Marked**; this run only.
  Navigation walks what is shown; the stop moves to the nearest shown. Scan-time flag, no I/O
  (`io::keeps_stop`, `mv_folder_set_kind_filter`).
- **Show, on the cull**: **Picks** (≥ 1 star), **Not rejected**, **Red** … **Purple**. Read on a
  worker (`meta::read_cull_marks`), cached by path + size + mtime; never blocks first pixel; unread
  files count as unrated. Rating the stop on screen never hides it. Windows filters in the core
  (`MV_FOLDER_HIDE_REJECTED` / `_UNPICKED`, label bits 8–10); Mac `meta_store` (`cull_marks_of`).
- **Typeahead:** filmstrip or gallery focused, typing jumps to the first name starting with it
  (300 ms idle resets). On the canvas letters are commands, so `/` opens find; `Ctrl+G` go-to.
- **Wrap** at folder ends: on by default, a setting.
- **Session:** window placement, last folder, zoom mode, wrap, background. Not a catalog.

## Sticky zoom

Off (default): each item fits. On (`S`): advancing keeps zoom and the pan centre as a fraction
of the image. Camera state only.

## Touch and trackpad

Gestures drive the commands above, never a camera path of their own (`shell/gesture.h`):

| Gesture | Does |
|---|---|
| Pinch (touchscreen, precision touchpad, Mac trackpad) | Zooms toward the fingers like the wheel (same springs, floor, rubber-band) |
| One-finger drag, zoomed (touchscreen) | Pans through `pan_by_screen`'s springs |
| Sideways swipe at fit (touchscreen) | Next / previous, once per touch |
| Two-finger sideways swipe, mouse tilt | Next / previous, once per swipe; on a PDF/DOCX with the filmstrip hidden, scrolls the page |
| Three-finger swipe (Mac, if System Settings sends it) | Next / previous |

No swipe under the gallery or in crop.

## Text in images

Text in a still can be selected with the mouse and copied; no menu, mode or setting.

| Pointer | Does |
|---|---|
| Hover over text | I-beam cursor |
| Press on a word and drag | Selects whole words in reading order; never pans or starts a file drag |
| Double / triple-click | Word / line; dragging extends by words / lines |
| Click without dragging, or press off text | Clears (off text the press pans) |

`Ctrl+C` copies ("Copied 12 words"), `Ctrl+A` selects all words, `Esc` deselects first.

Recognition starts after the pointer rests on a still 500 ms, on the job pool, once per still and
geometry (stills only). `shell/text_in_image.h` feeds the `text_recognizer` port (Vision /
`Windows.Media.Ocr`); `shell/text_select.h` is the shared selection model; the render thread
draws `input_snapshot::text`. On device; no OCR language → a notice, no Store download.

## Animation

GIF / APNG / WebP play on the render thread's frame clock ([04](04-image-pipeline.md)). Animated
items are in video mode: `Space` play / pause, `,` `.` frame step.

## Performance properties of this surface

Key-repeat next stays in the generation-counter + prefetch design; file operations and writes
run on the I/O or worker pool; overlays never start a present loop that does not idle; pairing
happens at scan.

## Window, theme

- **Always on top** (`Ctrl+Shift+A`), host-side. Dark / light follow the OS.
- **New window** (`Ctrl+N`): a new process (viewer state is per process): `--new-window X Y` /
  `--new-instance`. Add-ons run in one window's process ([18](18-import.md) "One host process").

## ABI

No hot-path command ABI: commands call existing session calls (`mv_folder_select`,
`mv_image_open`, camera, transport, edit, meta). Bindings never cross the C ABI;
`describe_commands()` goes to the shell's own island ([ABI](14-abi.md)).

## Not built

- `Ctrl+Enter` open in an external editor; set as wallpaper.
- Tabs, or several windows in one process.
- `Ctrl+PageUp` / `Ctrl+PageDown` for HEIC sequences.
- Text in images: partial-word selection, a right-click menu, text in document pages, clips or
  animations.
- Import / export of a person's key map (layouts ship as keymap packs).
- Voice hold-to-talk (`Ctrl+Shift+Space`).
- **Mac**: `Ctrl+G` go to index, `;` Live Photo motion, Open RAW / JPEG of a pair (left out of
  `MvCommandSupported`, so unlisted and their keys fall through).
