# Commands, keyboard, and mouse-free use

The command table, the one key router, modes and focus, the default key map as defined in
code, and the chrome behaviour around browse, overlays and search. MediaViewer is fully usable
without a mouse.

Source of truth: [`src/shell/commands.h`](../../src/shell/commands.h) (ids, keys, modes),
[`src/shell/command_table.cpp`](../../src/shell/command_table.cpp) (the default map and labels),
[`src/shell/key_router.h`](../../src/shell/key_router.h) / `.cpp` (mode and `Esc` resolution).
Both hosts use the same table and router; tests are `tests/test_key_router*.cpp`.

## One table, editable

- `default_bindings()` is a static array of rows: `(key, mods, mode mask, repeat policy,
  command, hold, release)`. `live_bindings()` starts as a copy; the **Settings** screen remaps
  rows in place (`rebind_live`) and **Reset** restores the defaults. The router, `?` and
  Settings all read the live table, so they cannot drift. Settings captures a key the way the
  host edge translates it (symbols through the active layout, keypad digits as their own keys),
  so any key that routes can be recorded.
- A remap onto a key another row holds in an overlapping mode **swaps** the two rows, so nothing
  is silently left unbound. Remaps persist by row index (`settings.ini [keys]` on Windows,
  `NSUserDefaults` on macOS), which is why new rows are only ever **appended**.
- Command ids are one wire space shared with the island's `chrome_command`: ids 1–19 predate the
  table, 15/16/18/19/20 are reserved island → native notifications, 76 (`palette`) is retired.
  Ids are never renumbered.
- A row whose key is `none` is listed in Settings (so it can be given a key) but not routed or
  shown in `?` — Open RAW / Open JPEG of a pair.
- Island-only commands (`zoom_preset`, `select_item`, `gallery_activate`, the adjust sliders,
  crop preset buttons…) carry an argument and have no key.
- **Add-on commands** (Import: `open_import`, `import_now`; Local search: `search_*`) are listed,
  routed and shown only while their add-on is loaded (`set_addon_commands_available` per
  family); otherwise the key falls through as if unbound. The exception is `Ctrl+F`, which runs
  base-app **file search** when Local search is absent.

## One key router

One router, on the UI thread, before the island's pre-translate. Pure: no platform calls, no
allocation, no I/O on keydown. Platform keys are translated to `mv::shell::key` at the host edge
(`main.cpp` / `main_mac.mm`), so nothing below `shell/` sees a key.

1. If a text control has focus (search, find, comment, tag edit), keys go to it; `Esc` blurs
   back. The host makes sure the field gets the standard editing chords (`Ctrl+A/C/X/V/Z`,
   `Ctrl+Y` redo on Windows).
2. Otherwise `(key, mods, mode)` → row through an O(1) index (`key × mod combo × mode`, one byte
   per slot), rebuilt only when the table changes.
3. Mouse-move, wheel and pan/zoom springs stay native on the canvas; no marshalling hop per
   event.

**Keys and layouts.** Letters are upper-case; symbol keys bind to the **character** the active
layout produces (`?`, `+`, `\`) with Shift cleared, digits keep their key and Shift. Symbols that
need AltGr (e.g. `\`, `[`, `]` on German / French layouts) do not resolve. On macOS both `⌘` and
`Control` map to the table's `Ctrl`, `Option` to `Alt`, and the Mac Delete key is `Delete`.

**Repeat policy.**

| Policy | Behaviour |
|---|---|
| `edge` | Down edge only; typematic repeats fall through |
| `repeat` | Every down, including repeat |
| `tap_hold` | Command on the down edge (a tap does not wait for key-up); the first repeat makes it a hold (`hold` per repeat), key-up sends `release`. Used for `Q` / `E` on a clip |
| `momentary` | Command on down, `release` on up (hold `Z`, hold `\`, hold `Y`) |

Up to four held keys are tracked independently; when the window loses activation, Settings
opens, the canvas gives up focus (on the Mac a pane or the gallery taking first responder), or the
table is rebuilt for an add-on's rows, every held key is released (`cancel_holds`). A hold keeps
its own copy of the release it owes, so a rewritten table cannot lose it.

**Keyboard pan** moves a tenth of the canvas per step through the springs. At fit `↑` `↓` are not a
pan; in fullscreen `↓` reveals the strips for 3 s after the last navigation, as does the bottom
hot edge.

### Modes

Resolved from state at every keydown (`resolve_mode`), never kept on a stack, in this order:

| Mode | When | Notes |
|---|---|---|
| **gallery** | The gallery grid is visible (and no popup) | Owns navigation even with canvas focus. Unmodified printable keys reach only gallery navigation, `G`, `F`, `?`, `/`; other letters are typeahead |
| **island** | Filmstrip or gallery island has focus | In-pane traversal belongs to XAML; unmodified printable keys are typeahead. Named keys and chords still route |
| **crop** | Crop mode on a still | Owns its keys; `A` `D`, `G`, `Ctrl+O` do nothing (walking away would drop the draft) |
| **trim** | Trim armed on a clip (not in a slideshow) | Layers over **video**: unbound keys fall back to the video rows |
| **runner** | The empty-window game is up | `Space` jumps/retries, `3` toggles 2D/3D |
| **loupe** | `Z` held | Layers over browse/video: only arrows (nudge) and `Z` / `\` differ |
| **slideshow** | Slideshow running | |
| **video** | Current item is a clip or an animation | |
| **browse** | Otherwise | |

The command bar and the transport strip are not modes: with them focused, `A` / `D` still walk the
folder and `Q` / `E` still skip.

### `Esc` walks out

`resolve_back` picks one target per press, never quitting: text field → popup (`?`, go-to, find)
→ Settings → Live Photo motion → crop (cancel) → trim (disarm, markers kept) → pane → gallery →
slideshow → fullscreen → runner → canvas focus → result list ("Back to folder", the outermost:
a search result list is a place, not an overlay). A list opened from the empty window (a search
from the welcome) goes back to the empty window: the welcome card with its recent folders, the
canvas cleared (2026-10-05; it used to leave the last result up, and on Windows did nothing).
With nothing to leave, the key is not handled.
`Ctrl+W` / `Alt+F4` close the window.

## Focus

Rings, visible: **canvas** (default), **filmstrip**, **gallery**, **pane** (folder tree,
metadata, adjust, Edit, jobs), plus the command bar and the transport. `Tab` / `Shift+Tab` cross the
island boundary; `Esc` returns to the canvas. Fullscreen hides chrome.

No island grows over the canvas, except a clip's **transport bar**, which floats over the bottom of
the video and auto-hides (`shell/transport_autohide.h`, shared by both hosts):

- Playing, it hides after `kTransportIdleMs` (2.5 s) with no activity; pointer movement, a click,
  the wheel, a transport command (`Space` `K` `J` `L` `Q` `E` `,` `.`, speed, mute, `↑` `↓`
  volume), `Tab`, or a fullscreen change bring it back.
- Paused, ended, hovered, scrubbing, a menu open, keyboard focus in it, or a screen reader running:
  it stays. Never over the gallery or Settings, never on a Live Photo stop.
- Fullscreen: the pointer hides with it while over the video. Windowed: never.
- Visual only: no refit, no second action on the wake, no repaint while idle.

## Default map

FastStone / IrfanView muscle memory. Mode masks below: **Viewing** = browse, video, island,
gallery (not slideshow); **Walk** = browse, video, slideshow; **All but crop** = every mode except
crop. Keys are shown Windows-style; on macOS read `⌘` for `Ctrl`.

### Browse

| Key | Modes | Command |
|---|---|---|
| `←` `→` | Walk, gallery | Previous / next (repeat) |
| `A` `D` | All but crop | Previous / next — on a clip too; transport is `Q` `E` |
| `Backspace` | Walk | Previous |
| `Space` | browse | Next. On a clip or animation: play / pause; in a slideshow: pause; in the runner: jump |
| `Home` / `End` | Walk | First / last |
| `PageUp` / `PageDown` | Walk | Back / forward ten |
| `Ctrl+PageUp` / `Ctrl+PageDown` | Browse, island | Previous / next page of a multi-page file (TIFF, PDF, DOCX); a notice says "Page n of m" |
| `F5` | browse, video | Slideshow |
| `F` / `F11` | all | Fullscreen |
| `Ctrl+O` | All but crop | Open media… |
| `Ctrl+Shift+O` | All but crop | Open folder… |
| `Ctrl+E` | all | Reveal the current file in Explorer / Finder; a Mac Photos library item opens in Photos instead ([26](26-photos-library.md)) |
| `Ctrl+W` | all | Close window |
| `Ctrl+N` | all | New window (⌘N; also File ▸ New Window and the Dock menu on the Mac) |
| `Ctrl+,` | All but crop | Settings |
| `Ctrl+G` | Viewing | Go to index. **Mac: not built** (no go-to popup yet; `⌘G` falls through) |
| `/` | browse, video, gallery | Find by name in the loaded listing (gallery folder row: find a folder tile) |
| `Ctrl+Shift+E` | All but crop | Folder tree: show and focus |
| `Ctrl+↑` | Viewing | Up one folder, selecting the folder just left |
| `Ctrl+←` / `Ctrl+→` | browse, video | Previous / next sibling folder |
| `Enter` | browse, video | Edit workspace (below) |
| `?` | all | Cheat sheet for the current mode |
| `F3` | all | Frame-time overlay |
| `R` | Viewing | Reset frame-time stats |
| `Esc` | all | Back (above) |
| *(unbound)* | browse, video | **Open RAW of pair** / **Open JPEG of pair** — listed in Settings. **Mac: not built** (not listed) |
| *(menu)* | — | **Recent folders**: the jump-list folders; Windows: Open ▸ Recent folders on the command bar; Mac: File ▸ Open Recent. A folder that has gone beeps and leaves the list |

### View

| Key | Modes | Command |
|---|---|---|
| `0` `1` `2` `3` `4` | Viewing | Fit / 100 % / 200 % / 400 % / Fill. The number row is zoom; ratings never take it |
| `Ctrl+0` | Viewing | Reset pan / zoom |
| `+` (`=`) `-` | browse, video, island | Zoom in / out, toward the cursor when there is one, else the centre |
| `+` (`=`) `-` | gallery | Larger / smaller thumbnails: 24 DIP steps, 80–344 DIP, initially 152; selection stays visible, size kept for the session |
| `↑` `↓` | browse, video | Pan when zoomed. On a fitted clip: volume ±10 % (carries across clips) |
| `Shift+arrows` | browse, video | Pan in all four directions |
| `G` | All but crop | Gallery (below) |
| `W` `S`, `↑` `↓` | gallery | Previous / next row |
| `Enter` | gallery | Open the selection in the viewer (leaving fullscreen / slideshow, restoring the filmstrip if enabled) |
| `T` | all | Filmstrip show / hide (stored per mode: folder open or single image) |
| `B` | Viewing | Cycle background: black / gray / white / checkerboard |
| `S` | browse, video, island | Sticky zoom on advance |
| `C` | Viewing | Clipping blinkies |
| `O` | Viewing | On-canvas info overlay (filename, index, exposure triangle) |
| `Shift+O` | Viewing | AF-point quads from the maker notes already read |
| `I` | Viewing | Metadata pane: focuses it; `←` `→` change tab, `↓` reaches tag search; `Esc` returns to the canvas, a second `Esc` closes it |
| `Shift+I` | Viewing | Eyedropper: one-pixel sRGB 8-bit + hex readout under the cursor (stills) |
| `Ctrl+C` | Viewing | With the eyedropper on: copy `#RRGGBB  rgb(r, g, b)  x y`. Otherwise copy the marked (else current / gallery-selected) file(s) |
| hold `Z` | browse, video | Loupe: 100 % around the cursor or a keyboard point; arrows nudge it. Same texture, no decode |
| hold `\` | browse, video | Previous item, for burst pick, from the five-slot GPU LRU (Mac: the last still shown, kept on the GPU) |
| `;` | browse, video | Play a Live Photo's motion once, with audio, and return to the still. `;` again, `Esc` or navigation return early. **Mac: not built** |
| `Ctrl+Shift+A` | all | Always on top (`HWND_TOPMOST`) |

**Gallery.** Full-client grid of the folder. `A` `D` / `←` `→` move by item, `W` `S` / `↑` `↓`
by row; a click opens in the viewer. Child folders are big tiles when the folder holds only
folders, one row above the photos when it holds both; up from the first photo row reaches that
row, `Enter` opens a tile, `↓` returns, `/` finds a tile by name. A clip does not play under the
gallery: opening it pauses a playing clip, a clip selected in it waits paused on its first frame,
and closing it resumes only the clip that was playing. The folder path lives in the command bar
left of `?` (a long middle collapses into a menu), with Up and Root buttons outside the trail.

### Marks, copy, move

Marks are a separate set from the selection, so arrow-key browsing never makes accidental ranges.

| Key | Modes | Command |
|---|---|---|
| `Insert` / `Shift+Space` | browse, video | Toggle mark on current |
| `Ctrl+A` | Viewing | Mark all |
| `Ctrl+D` | Viewing | Unmark all |
| `F7` / `Shift+F7` | Viewing | Copy marked (else current) to the last destination / pick a folder |
| `F8` / `Shift+F8` | Viewing | Move, same rule |
| `Delete` | browse, video | Recycle Bin / Trash, with confirm. Marks if any, else current |
| `Ctrl+Shift+C` | Viewing | Copy path(s) as text |
| `Ctrl+Alt+C` | browse | Copy the current still with edits baked, as PNG (worker thread) |
| `Ctrl+Shift+S` | Viewing | Share (Windows Share / macOS share sheet) |

- **Move** on one volume is a rename (`MoveFileEx`); across volumes it is a **verified** copy
  (hashed while read, read back uncached, compared) and only then the delete. I/O thread.
- Copy / move never overwrite: a collision becomes `name (2).ext`. The last five destinations are
  remembered.
- **Delete** only uses the Recycle Bin / Trash. Where there is none (a share, some removable
  drives, bin off) the item is refused, not deleted, and the user is told how many. Marks clear
  only for items that succeeded. On the Mac, File ▸ Move to Trash (`⌘⌫`) is off while a text
  field has the keyboard (there `⌘⌫` deletes to the start of the line), in trim or crop, with
  the Video Editor open, and during a slideshow: the menu sees the key before anything else does.
- **Drag-out** (mouse): a gallery or filmstrip cell drags the original file (and its pair) as
  `CF_HDROP`, copy-only; a marked cell drags every marked item in listing order. The canvas drags
  too (at fit on Windows, `⌘`-drag on the Mac). A drag of ours dropped on our own window is
  refused.
- **Opening** (argv, drop): the first entry that exists wins; a folder opens that folder, a file
  opens its folder with the file selected; several files from one folder open it on the first; a
  mixed drop opens the first file's folder. Nothing existing: nothing opens and the app beeps. A
  watcher refresh keeps the current item selected; if it was removed the next one takes its
  place (the previous one at the end).

### Edit (stills)

| Key | Modes | Command |
|---|---|---|
| `[` `]` | browse, crop | Rotate −90° / +90°. On a JPEG with nothing else in its stack, a lossless rewrite 0.4 s after the last key, atomically; the preview turns at once |
| `H` / `V` | browse, crop | Flip horizontal / vertical (lossless on a JPEG, like rotate) |
| `Shift+C` | browse | Crop / straighten mode |
| `Shift+A` | browse | Adjust pane: show and focus its first slider; again hides it |
| `Ctrl+Z` / `Ctrl+R` | browse, crop | Undo the last edit / reset to the original |
| `Ctrl+S` | browse | Export the edits to a new file beside the original (`<name>-edit.jpg`); never overwrites |
| hold `Y` | browse | Show the original (edit stack untouched) |
| `Enter` | browse, video | Open / close the **Edit workspace** — Edit image on a still, Edit video on a clip ([edit workspace](20-edit-workspace.md)). `Shift+C` / `Shift+A` / `Ctrl+T` open it on Crop / Colour / Trim; with it open `I` / `Ctrl+J` select its Info / Jobs tabs |

### Crop

| Key | Command |
|---|---|
| Arrows | Move the crop rectangle 1 % of the frame |
| `Shift` + arrows | Move its bottom-right corner (narrower / wider / shorter / taller) |
| `,` `.` | Straighten −0.5° / +0.5° (±45°); an untouched rectangle follows the angle as its largest fit |
| `[` `]` `H` `V` | Turn / flip with the draft carried along (no file write while cropping) |
| `A` / `X` | Next aspect preset (Free, Original, 1:1, 4:3, 3:2, 16:9, 5:4) / swap portrait and landscape |
| `Enter` | Apply: the draft joins the edit stack |
| `Esc` | Cancel the draft |

### Rate

| Key | Modes | Command |
|---|---|---|
| Numpad `0`–`5`, or `Ctrl+Shift+0`–`5` | Viewing | Rating (0 clears) |
| `Ctrl+I` | Viewing | Show the metadata pane (Windows: on *All tags* with its comment editor open on the file's comment and the keyboard in it; `Enter` saves, `Esc` drops; Mac: the pane's *All tags* editor) |

A rating writes the item on screen only, on the I/O pool: a JPEG is rewritten in place, anything
else gets an XMP sidecar. The command bar shows what landed ("★★★★☆", or "— IMG_1234.xmp" when a
sidecar took it). Not in a slideshow or crop. On macOS `⌘⇧3`/`4`/`5` are the system screenshot
shortcuts and do not reach the app unless turned off in System Settings; the keypad, or a remap,
works regardless.

### Video and trim

| Key | Modes | Command |
|---|---|---|
| `Space` | video | Play / pause |
| tap `Q` / `E` | video | Skip −2 s / +2 s, exact, on the down edge |
| hold `Q` / `E` | video | Skim ∓2 s per repeat with non-exact (keyframe) seeks; release settles exactly. Intent accumulates across the burst |
| `Shift+Q` / `Shift+E` | video | Playback speed one rung down / up: 0.25 / 0.5 / 1 / 1.5 / 2 / 4. The command bar's speed dropdown is a view of this; native owns the rate |
| `J` `K` `L` | video | −10 s / pause / +10 s |
| `,` `.` | video | Frame step (also on animations) |
| `Shift+M` | video | Mute (`M` is left free). The transport's More flyout (volume, mute, audio track, A–B loop) is a view of native's state, as the speed dropdown is; `↑` `↓` at fit are the volume |
| `Ctrl+T` | video, trim | Arm / disarm trim |
| `[` `]` | trim | In / out marker at the playhead |
| `P` | trim | Preview the cut: A–B loop over exactly what the keyframe save writes. A loop set from the More flyout replaces it and ends the preview |
| `Enter` / `Shift+Enter` | trim | Save the keyframe cut (instant) / the frame-accurate re-encode (slower) |
| `Ctrl+X` | trim | A copy without in–out |
| `Backspace` / `Delete` | trim | Clear the markers (in trim, `Delete` never trashes the clip) |
| `Ctrl+←` / `Ctrl+→` | trim | Previous / next keyframe |
| `Ctrl+J` | Viewing, trim | Jobs pane: `↑` `↓` choose, `Delete` cancel, `R` retry, `Enter` reveal output |
| `Ctrl+S` / `Ctrl+B` | video, trim | Clip tools (rotate, split, frame, audio, remux, GIF / WebP) / split at the playhead |
| Media keys | — | SMTC (Windows) / `MPRemoteCommandCenter` (macOS), same commands |

### Slideshow

| Key | Command |
|---|---|
| `F5` | Start (from browse or video) |
| `Space` | Pause |
| `+` (`=`) `-` | Interval |
| `.` | Blackout (the canvas idles) |
| `R` | Shuffle |
| `Esc` | Leave |

`F5` goes fullscreen if the window is not, and leaving puts it back. Next is the same navigation
command as browse, on a UI-thread timer, so prefetch and the generation counter stay in play and
a still between advances is zero presents. No transition pass. A clip advances at whichever is
later, the interval or the end of the clip while it plays (a paused clip goes on the interval);
a finite animation likewise, an endless one on the interval. Intervals 1 / 2 / 3 / 4 / 5 / 7 /
10 / 15 / 20 / 30 / 60 s, default 4 s. Shuffle visits every item once per round, starting from the
current one. Wrap is on. The slideshow is stills and clips from the folder; Mac's is stills only.
While a slideshow runs (not paused), or a clip plays, the display and the machine are kept
awake: `SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED)` on Windows, an
`NSProcessInfo` activity on the Mac. Pause, the end of the clip, leaving and exit let it go.

### Import (while the add-on is installed)

| Windows | Mac | Command |
|---|---|---|
| `Ctrl+Shift+I` | `⌘⇧I` | Open the Import window (with the viewer's marks for "Marked in viewer") |
| `Ctrl+Shift+F7` | `⌘⇧F7` | Import the marked (else current) files now with the last preset |

Inside the Import window (its own keys, not table rows): `Enter` imports (on a focused tile it
opens that file in the viewer), `Ctrl+Enter` imports from anywhere, `Space` toggles a tile and
pauses / resumes while copying, `Shift+Space` toggles its day, `Ctrl+Tab` / `Ctrl+Shift+Tab` the
next / previous source, `Ctrl+J` ejects, `Esc` closes the window and the import carries on
([import](18-import.md)).

### Search

| Key | Modes | Command |
|---|---|---|
| `Ctrl+F` | Viewing | **With Local search:** its panel, keyboard-focused. **Without:** file search (below) |
| `Ctrl+Shift+F` | Viewing | Find similar to the still or paused frame on screen (Local search) |
| `N` / `Shift+N` | video | On a clip opened from results: next / previous matching moment (Local search) |

The same command runs from the search icon at the right end of the command bar's folder path
(and beside "Search: …" while a result list is shown). In the Local search field typing searches
([local search](17-local-ai-search.md)); `Tab` completes an offered name; `Enter` or `↓` moves
into the results (waiting for words still being searched); in the grid arrows move, `Enter` opens
the results in the viewer on that tile, `Ctrl+Enter` opens them all as the gallery, a typed
character returns to the field; `Esc` returns to the field, then closes.

**File search** (base app, no index): shows the gallery if hidden and opens "Find files by name
in this folder" over the grid. Typing filters the grid, folder tiles too, by a case- and
accent-insensitive substring, ~70 ms after the last key, with an "N of M" count. A view over the
gallery only: arrows move among matches; the viewer and filmstrip still walk the whole folder.
`Enter` / `↓` go to the grid on the first match; `Esc` clears, then closes; closing the gallery
or opening another folder closes it. Names are folded once per listing off the UI thread; a
keystroke is one pass (≤ 2 ms for 10,000 items). No disk walk.

### `?`

`?` toggles a mode-sensitive cheat sheet generated from the live table
(`describe_commands()`: id, modes, label, key label). On Windows it is a XAML flyout with
`ShouldConstrainToRootBounds = false`, so a strip does not clip it; it does not composite onto the
swapchain. There is no command palette; Settings search and `?` cover finding a command.

### Empty window

On an empty window `Space` starts the T-Rex runner. In it `Space` jumps / retries, `3` toggles a
2D / 3D view (keeping the jump, obstacles and score), and `Esc` leaves: the runner sprints off and
the scene folds away before the welcome card returns. Opening a file over the runner skips that.

## Folder tree

A left strip, hidden by default. `Ctrl+Shift+E` shows and focuses it: `↑` `↓` walk, `→` `←` open
and close a folder, `Enter` opens it and returns to the canvas, `Esc` returns to the canvas and a
second `Esc` closes it. Names only, from `mv_list_subdirectories` on a worker; no thumbnails.

## Overlays on the hot path

All are extra draws in the same present, and a still with no animation still idles to zero
presents ([rendering](03-rendering.md)). Toggles request one redraw.

| Overlay | Cost |
|---|---|
| Loupe (hold `Z`) | Camera / viewport change; same texture |
| Hold-previous (hold `\`) | The five-slot LRU; no second session |
| Clipping blinkies (`C`) | Shader; may present while on (it animates), labelled in F3 |
| Pixel grid (≥ 400 %) | Shader |
| Canvas background / checkerboard (`B`) | Clear colour + optional shader |
| On-canvas info (`O`) | Overlay text from the folder model and metadata already read |
| AF-point quads (`Shift+O`) | A few quads from maker notes already in the property model |
| Eyedropper (`Shift+I`) | One-pixel staging readback on demand, never a full-texture download |
| Histogram | On the adjust pane, computed after the sliders settle |

## Status, sort, typeahead

Chrome, in memory, no decode.

- **Status:** filename, index / count, dimensions, zoom, rating. Index and counts come from the
  folder model.
- **Sort:** name, modified, size, type, date taken; ascending or descending
  (`mv_folder_set_sort`, saved as `[view] sort` on Windows; View ▸ Sort By on both). Date-taken
  keys are read once per file by one background job and the listing re-sorts in place when they
  land; a file with no stamp sorts by mtime.
- **Typeahead:** with the filmstrip or gallery focused, typing jumps to the first name starting
  with what was typed (300 ms idle resets). On the canvas every letter is a command, so `/` opens
  a find box over the loaded listing. `Ctrl+G` goes to an index.
- **Wrap** at folder ends: on by default, a setting.
- **Session:** window placement, last folder, zoom mode, wrap, background. Not a catalog.

## Sticky zoom

- **Off (default):** each item fits.
- **On (`S`):** advancing keeps zoom and the pan centre as a fraction of the image; a different
  aspect keeps the centre. Camera state, not a decode; prefetch is unaffected.

## Animation

GIF / APNG / WebP play on the render thread's frame clock ([image pipeline](04-image-pipeline.md)).
An animated item is in video mode: `Space` play / pause, `,` `.` frame step.

## Performance properties of this surface

- Key-repeat next stays inside the generation-counter + prefetch design; nothing opens an image
  synchronously on the UI thread.
- Copy, move, delete, reveal, export, share and rating writes run on the I/O or worker pool;
  completions update chrome. The UI thread may open a picker but never copies bytes.
- Overlays never start a present loop that does not idle.
- RAW+JPEG and Live Photo pairing and companion hiding happen at scan, not on each next.
- No slideshow transition shader, no full-texture readback, no library database: SQLite is the
  thumbnail cache.

## Window, theme

- **Always on top** (`Ctrl+Shift+A`), host-side.
- Dark / light follow the OS.
- **New window** (`Ctrl+N`, 2026-10-05): another MediaViewer process on the empty window,
  cascaded from the one that asked. The viewer's state is one per process (the hosts and the
  SwiftUI stores are single-window), so a window is a process, not a second `NSWindow` or
  `HWND` in this one. The Mac starts the bundle as a new instance (`--new-window X Y`, the
  asking window's top-left); Windows starts the exe with `--new-instance`, which skips the
  single-instance pipe. Add-ons run in one window's process only (see
  [18](18-import.md) "One host process").

## ABI

No hot-path ABI for commands. Commands invoke existing session calls (`mv_folder_select`,
`mv_image_open`, camera, transport, edit, meta). Bindings never cross the C ABI;
`describe_commands()` goes to the shell's own island. Pixels do not cross ([ABI](14-abi.md)).

## Not built

- `F2` rename.
- `Ctrl+Enter` open in an external editor; set as wallpaper.
- `Ctrl+Tab` / tabs, and several windows inside one process.
- `Ctrl+PageUp` / `Ctrl+PageDown` for ICO sizes and HEIC sequences (pages of TIFF, PDF and DOCX are built).
- `U` clear label and `X` reject mark.
- A filter (all / photos / videos / RAW) on the listing.
- Touch gestures (swipe, pinch).
- Import / export of key maps and named alternate layouts.
- Voice add-on hold-to-talk (`Ctrl+Shift+Space`).
- **On the Mac** (issue #183): `Ctrl+G` go to index, `;` Live Photo motion, and Open RAW /
  Open JPEG of a pair. `MvCommandSupported` leaves them out, so `?` and Settings do not list
  them and their keys fall through. The rest of the View table, the loupe, hold `\` and the
  slideshow's `.` / `R` run on both hosts.
