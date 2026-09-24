# 16 — Commands, keyboard, and mouse-free use

**Release scope (2026-09-14):** v1 ships commands delivered through PR 7, packaged in PR 8. Rows for PRs 9–15
describe future updates, including the folder tree (PR 9); they are not initial-release
requirements.

v1 is **fully usable without a mouse.** That is a product requirement, not a settings page.
Configurable keymaps landed in PR 6 (plan/12 2026-09-13) once the default map was in daily
use. The Settings screen remaps the **same live table** the router and `?`
read. Reset restores `default_bindings()`. Import/export JSON and named alternate layouts
(FastStone / IrfanView / vim) stay v1.1.

D1 exists because FastStone users bounce without keyboard behaviour. PR 6's verify line is
the first falsifiable cut of this doc; later PRs **register commands into the same table**
rather than inventing a second input path.

Speed is the constraint. A command that decodes, hits disk, or takes a marshalling hop
per key-repeat is a bug, not a feature. The present-loop gate from PR 1 still holds.

## The split

| v1 (commands through PR 7) | Future updates |
|---|---|
| Default map covering browse, view, and video. Settings remaps that table; `?` stays in sync. Edit, metadata, and trim commands join in their future PRs | Import/export JSON, alternate layouts (FastStone / IrfanView / vim); colour scheme + user font ([10-roadmap.md](10-roadmap.md) v1.1) |
| Every command has a key, including ones that currently look like buttons | Per-profile maps, user chords beyond a couple of prefixes |
| `?` overlay listing the **current mode's** bindings | |
| Focus model that crosses canvas + XAML islands | |

The Mac host (Milestone F) writes chrome twice (**D9**). From PR 9 each row this table
adds lands with a Mac default binding in the same PR (`⌘` for `Ctrl`; a chord wherever
Windows assumes a numpad, e.g. rating is `⌘⇧0`–`5`). Bindings live in the host; the
core exposes command *effects* through the existing ABI. Do not put Win32 virtual-key codes
in `image/`, `player/`, `edit/`, or `meta/`.

## One key router

Islands each registering `KeyboardAccelerator`s is how you get two handlers and a swallowed
arrow key. **One router, on the UI thread, in the native window procedure.**

1. If a XAML text control has focus (rename, search, user comment, go-to / find), keys go
   to the island. `Esc` blurs back to the canvas.
2. Else map `(key, mods, mode)` → `command_id` through the default table and dispatch.
3. Mouse-move, wheel, and pan/zoom springs stay native on the canvas HWND — no marshalling
   hop per mouse-move ([02-architecture.md](02-architecture.md)).

Dispatch is an array index, not a string lookup. `?` lists the same static table
in memory. No I/O on keydown.

Symbol keys bind to the **character** the active layout produces (`?`, `+`, `\`), not to a US
key position. Symbols that need AltGr do not resolve in v1: on German and French layouts that
is `\` (hold-previous) and, from PR 10, `[` `]`. Those wait for the v1.1 remap.

Keyboard pan moves a tenth of the canvas per step, whatever the zoom, through the springs.
At fit it is not a pan: `↑` `↓` fall through, except that `↓` in fullscreen reveals the strips
(and a clip's transport) for 3 s after the last navigation; the bottom hot-edge does the same.

**Tap and hold may be two commands on one key**, and `Q` / `E` on a clip are the case that
earns it: a tap is a discrete skip (±2 s), a hold is a continuous skim. The distinction is
typematic repeat — the down edge fires the tap so a skip does not wait for key-up, the first
repeat makes it a hold, and key-up settles. This is a shape to use sparingly; it is here
because skip and shuttle are the same gesture at two durations, not to double the key count.

**Modes** (a handful, not a modal editor):

| Mode | When | Arrow keys mean |
|---|---|---|
| **Browse** | Default, canvas focused | Prev / next file. When zoomed, `↑` `↓` pan and `←` `→` still navigate (FastStone). Pan with middle-drag / hold `Space` is mouse; keyboard pan is `Shift+arrows` when zoomed |
| **Filmstrip** | Bottom island focused | Prev / next thumb; `Enter` focuses canvas |
| **Pane** | Folder tree / metadata / adjust / jobs | In-pane traversal; `Esc` returns to canvas |
| **Video** | Current item is a clip, playing or paused | Frame step when paused (`,` `.` and arrows); `J` `K` `L` transport |
| **Slideshow** | After `F5` | Next on a timer; `Esc` leaves |
| **Crop** | Adjust geometry (PR 10): `Shift+C` on a still | Nudge crop; `Enter` commits, `Esc` cancels |

`Esc` walks **out**: crop → pane → gallery → fullscreen / slideshow → canvas. The gallery
covers the canvas like an overlay, so it closes before the window-level states. It does not quit from a
nested mode. Lab `Esc` = quit is a harness thing and dies in PR 6 for the shipped chrome
(`Alt+F4` / `Ctrl+W` still close).

## Focus

Three rings, visible:

- **Canvas** (default)
- **Filmstrip**
- **Pane** (folder tree, metadata, adjust, jobs)

`Tab` / `Shift+Tab` crosses the island boundary (already on PR 3's verify). `Esc` returns to
the canvas. Fullscreen hides chrome; a `↓` or filmstrip hot-edge shows the strip until
navigation settles.

Do not grow an island over the canvas to "make keys easier." That eats mouse-move and the
swapchain ([12-decision-log.md](12-decision-log.md) 2026-09-07).

## Default map

FastStone / IrfanView muscle memory, not vim. Vim is a v1.1 preset.

Number-row `0`–`4` is **zoom**, matching the lab today. Ratings do not steal those keys.

On an empty window, Space starts the T-Rex runner. While the game is active,
`3` toggles its 2D/3D view, Space jumps/retries, and Esc leaves. Switching the view
preserves the current jump, obstacles and score. A retry keeps the chosen view;
leaving the game restores the default 2D view. The runner binding uses the same
command table as image zoom, in its own mode, and does not intercept text input.

### Browse

| Key | Command |
|---|---|
| `←` `→` or `A` `D` | Previous / next, **in every mode including on a clip**. `A` `D` were briefly reinterpreted as the clip's speed/skim pair, which stopped the two most obvious walk-the-folder keys walking the folder as soon as a video was open; transport moved to `Q` `E` |
| `Space` / `Backspace` | Next / previous. **Space is not the lab sweep after PR 6.** On a video or animation, Space is play/pause |
| `Home` / `End` | First / last |
| `PageUp` / `PageDown` | Skip ~10, or previous / next *folder* when the folder tree is populated |
| `Delete` | Recycle Bin, confirm (PR 6). Marks if any, else current |
| `F2` | Rename, IME-aware |
| `Enter` | Open the gallery selection in the normal viewer, with the filmstrip if enabled. No fullscreen or slideshow action from the canvas |
| `F5` | Slideshow |
| `F11` / `F` | Fullscreen, also available from View → Full screen. `F3` stays the frame-time overlay |
| `Ctrl+O` | Open media (file picker) |
| `Ctrl+Shift+O` | Open folder |
| `Ctrl+E` | Show the current file in Explorer, selected |
| `Ctrl+,` | Settings (view defaults and remappable keys). Colour scheme, chrome/canvas/overlay palette, and a user-supplied font are **v1.1** ([10-roadmap.md](10-roadmap.md)) |
| `Ctrl+W` / `Alt+F4` | Close window |
| `Ctrl+Tab` | Next tab (PR 15) |

### View

| Key | Command |
|---|---|
| `0` | Fit |
| `1` | 100 % |
| `2` `3` | 200 % / 400 % |
| `4` | Fill |
| `+` `-` | Zoom toward centre when no cursor; toward cursor when there is one. In the gallery, enlarge / shrink thumbnails instead (`=` also enlarges): 24 DIP steps, 80–344 DIP, initially 152 DIP. Keep the selection visible and retain the chosen size for the session. Separate gallery commands in the shared table allow independent remapping |
| `Ctrl+0` | Reset pan/zoom (not rating-0 — rating is `Ctrl+Shift+0` or numpad, see below) |
| `H` / `V` | Flip horizontal / vertical (PR 10). On a JPEG, a lossless file write like `[` `]` |
| `[` `]` | Rotate −90 / +90. Lossless JPEG when that is the only op (PR 10), from the viewer, no edit pane required. The preview turns at once; the file is rewritten 0.4 s after the last key, atomically, pixels untouched (plan/12 2026-09-24) |
| `Shift+C` | Crop / straighten mode (PR 10; key chosen 2026-09-24 — `C` is clipping). See **Crop** below |
| `Ctrl+Z` / `Ctrl+R` | Undo the last edit / reset edits to the original (PR 10; keys chosen 2026-09-24). A lossless rewrite already on disk is undone by the opposite turn |
| `Ctrl+S` | Export the edits to a new file beside the original, `<name>-edit.jpg` (PR 10; key chosen 2026-09-24). Never overwrites |
| `I` | Metadata pane (PR 9). Windows 2026-09-24: focuses the pane; Left / Right change tab, Down reaches the tag search (type to filter), `Esc` returns to the canvas and a second `Esc` closes it |
| `E` | Adjust pane (PR 11) — **collides with `Q` `E` transport below, landed in 5c. PR 11 picks a different key; this row is not a claim on `E`.** |
| `T` | Filmstrip show/hide. Writes the preference for the mode you are in — folder open or single image (`Settings` menu, PR 4) |
| `G` | Gallery: full-client thumbnail grid of the folder. `W` / `S` and Up / Down move by row, `A` / `D` and Left / Right move by item. `Enter` opens the selection in the normal viewer, leaving fullscreen/slideshow and restoring the filmstrip if enabled. A click opens it in the viewer. `Esc` closes the gallery. Navigation applies while the gallery is visible, even before keyboard focus moves into it. **Child folders (PR 26)** are big tiles when the folder holds only folders, and one row above the photos when it holds both. Up from the first photo row moves onto that row, Left / Right move among them, `Enter` opens the tile, Down returns to the photos. `/` on that row finds a tile by the start of its name |
| `Ctrl+Up` (`⌘↑` on Mac) | Up one folder (PR 26). Opens the enclosing folder and selects the folder you just left. The path stays on screen, including while a photo is open; a long middle collapses until asked for. Not bound to Backspace, which is Previous |
| `Ctrl+Left` / `Ctrl+Right` (`⌘←` `⌘→` on Mac) | Previous / next folder beside the one open (PR 26), while a photo is open. The gallery keeps plain Left / Right for its tiles |
| `Ctrl+Shift+E` | Folder tree show/focus (PR 9). Windows 2026-09-24: focuses the tree; Up / Down walk it, Right / Left open and close a folder, `Enter` opens it and returns to the canvas, `Esc` returns to the canvas and a second `Esc` closes it |
| `O` | On-canvas info overlay (filename, index, exposure triangle once PR 9 can fill it) |
| `Shift+O` | AF-point quads from the maker notes already read (PR 9; the plan gave no key, chosen 2026-09-24). Off by default |
| `Shift+I` | Eyedropper: one-pixel readout under the cursor, sRGB 8-bit + hex (PR 9; key chosen 2026-09-24). Stills only. `Ctrl/Cmd+C` while it is on copies the readout as `#RRGGBB  rgb(r, g, b)  x y`; with it off it copies the marked (or current / gallery-selected) file(s), the macOS start of PR 15's `CF_HDROP` twin |
| Hold `Z` | Loupe: 100 % around a keyboard-nudgeable point (or last cursor). Same texture, camera change, no decode |
| `\` hold | Previous item for burst pick. Uses the five-slot GPU LRU ([04-image-pipeline.md](04-image-pipeline.md)); must not `mv_image_open` a replacement |
| `;` | Play Live Photo / motion once, return to the still. Required: hover-to-play fails the no-mouse bar. PR 7: edge only (no hold-to-play); `;` again, `Esc` or any navigation also returns to the still, which comes back from the LRU. Plays with audio. No transport strip on a Live Photo stop |
| *(unbound)* | **Open RAW of pair** / **Open JPEG of pair** (PR 7). [04](04-image-pipeline.md) put these in the command palette, which was dropped; they are rows in the live table with no default key, listed in Settings (not in `?`) so a paired file is never trapped. Open RAW shows the RAW half on the canvas at the same stop; Open JPEG goes back to the primary |
| `B` | Cycle canvas background (black / gray / white / checkerboard). Checkerboard is the alpha case |
| `S` | Sticky zoom on advance (keep scale + pan fraction). Default off |
| `C` | Clipping blinkies. Display-referred in PR 6; accurate RAW clip from PR 11 |

`Ctrl+0` as reset fights nobody if ratings live elsewhere. **Do not** make `0` rating-zero.

### Marks, copy, move

Explorer-style multi-select on arrow-key browse is wrong — it turns culling into accidental
ranges. **Marks are a separate set.**

| Key | Command |
|---|---|
| `Insert` or `Shift+Space` | Toggle mark on current |
| `Ctrl+A` | Mark all (current filter) |
| `Ctrl+D` | Unmark all |
| `F7` | Copy marked (else current) to last destination; `Shift+F7` picks a folder |
| `F8` | Move, same rule. Same-volume `MoveFileEx`; copy+delete across volumes. I/O thread |
| `Ctrl+C` | Clipboard `CF_HDROP` of original(s) |
| `Ctrl+Shift+C` | Copy path(s) as text |
| `Ctrl+Alt+C` | Flattened PNG/JPEG of the current view (edits baked). Worker, not UI thread |
| `Ctrl+Shift+S` | Windows Share (`IDataTransferManager`) |
| `Ctrl+E` | Reveal in Explorer |
| `Ctrl+Enter` | Open with the user-configured external editor (`ShellExecuteEx`, no wait) |

Copy/move never overwrite an original. Collision: `name (2).ext`. Destinations remembered
(last five) in settings. This is FastStone's culling loop and it is why a viewer replaces a
file manager for a card dump.

`Delete` only ever uses the Recycle Bin. On a location without one (a network share, some
removable drives, the bin turned off) the item is **refused, not deleted**, and the user is told
how many; there is no silent permanent delete. Marks clear only for items that succeeded.

**Opening (argv, drop, PR 6).** The first entry that exists wins: a folder opens that folder; a file
opens its folder with that file selected. One folder dropped opens it; several files from one
folder open it on the first; a mixed drop opens the first file's folder. If nothing exists, nothing
opens and the app beeps. A watcher refresh keeps the current item selected; if it was removed the
next one slides into its place (the previous one at the end).

### Crop (PR 10)

| Key | Command |
|---|---|
| Arrows | Move the crop rectangle 1 % of the frame |
| `Shift` + arrows | Move its bottom-right corner (narrower / wider / shorter / taller) |
| `,` `.` | Straighten −0.5° / +0.5° (±45°). An untouched rectangle follows the angle as its largest fit |
| `[` `]` `H` `V` | Turn / flip with the draft carried along (no file write while cropping) |
| `Enter` | Apply: the draft joins the edit stack |
| `Esc` | Cancel the draft |

`A` `D`, `G` and `Ctrl+O` do nothing while cropping: walking away would drop the draft.

### Rate (PR 12)

| Key | Command |
|---|---|
| Numpad `0`–`5` | Rating. No numpad: `Ctrl+Shift+0`–`Ctrl+Shift+5` |
| `U` | Unflag / clear colour label (label write is v1.1; `U` is a no-op until then) |
| `X` | Reject mark (convenience for `Insert` + next). Does not delete |

### Video (PR 5c) and trim (PR 13)

| Key | Command |
|---|---|
| `Space` | Play / pause |
| tap `Q` `E` | **Skip** −2 s / +2 s, exact. Fires on the down edge so a tap does not wait for key-up. (Tap used to step playback speed; that moved to `Shift+Q` / `Shift+E` and the command-bar dropdown, plan/12 2026-09-13.) |
| hold `Q` `E` | **Skim** −2 s / +2 s per key repeat. Non-exact seek (nearest keyframe) while held, so a shuttle cannot queue a decode-forward per repeat; the release settles exactly, the same two modes as a scrubber drag and its release. Intent accumulates across the burst — re-reading the position each repeat asks to move from a point the last press already rounded backwards |
| `Shift+Q` `Shift+E` | **Playback speed** one rung down / up the ladder 0.25 / 0.5 / 1 / 1.5 / 2 / 4. The command bar's speed dropdown is a *view* of this: native owns the rate, pushes it to the island, and the dropdown posts back — one router, never two owners |
| `J` `K` `L` | −10 s / pause / +10 s |
| `,` `.` | Frame step (already in [05-video-pipeline.md](05-video-pipeline.md)) |
| `↑` `↓` | Volume +/− 10 % on a clip (fitted view; when zoomed they pan). Volume carries across clips |
| `Shift+M` | Mute (`M` is not mute — reserved so a FastStone-layout preset can put Move on `M` in v1.1) |
| `[` `]` | In / out markers when trim is armed (PR 13). In browse they rotate; trim mode takes them |
| `Ctrl+←` `Ctrl+→` | Previous / next keyframe |
| Media keys | SMTC, same commands |

### Slideshow (PR 6)

| Key | Command |
|---|---|
| `F5` | Start |
| `Space` | Pause |
| `+` `-` | Interval |
| `.` | Blackout |
| `R` | Shuffle |
| `Esc` | Leave |

No transition pass. Next is the same navigation command as browse, on a timer, so prefetch
and the generation counter stay in play. A crossfade is two textures in the present loop
for a feature nobody opens a camera dump for.

PR 6 specifics. `F5` starts from the canvas in browse (fullscreen if the window is not; leaving
puts it back). A clip advances at **whichever is later**: the interval, or the end of the clip
while it plays; a paused clip goes on the interval. A finite animation is the same; one that loops
forever goes on the interval. Intervals step 1 / 2 / 3 / 4 / 5 / 7 / 10 / 15 / 20 / 30 / 60 s,
default 4 s. Shuffle visits every item once per round, starting from the current one. Wrap is on.
`.` blacks the canvas out and the canvas idles. The advance tick is a UI-thread timer: between
advances a still is zero presents.

### `?`

`?` toggles a mode-sensitive cheat sheet over the canvas. Chrome, not a settings page.
People learn FastStone this way. It is a XAML flyout with
`ShouldConstrainToRootBounds = false` so it is not clipped by a strip (PR 3). It
does not composite onto the swapchain.

A searchable command palette (`Ctrl+K`) was in this slice and was dropped: a
text field in the island flyout fail-fasts, and keys that are already bindings
never reach the filter. Settings search and `?` cover find-a-command. See
[12-decision-log.md](12-decision-log.md).

## Folder tree

Named in [02-architecture.md](02-architecture.md) and in D1's FastStone rationale; no PR
owned it. **PR 6**, as a **third island, left strip**, hidden by default (`chrome_left_px = 0`
until shown). `Ctrl+Shift+E` shows and focuses it. Arrows walk, `Enter` opens, `PageUp` /
`PageDown` from the canvas move to sibling folders.

Do not put the tree in the command-bar island or the filmstrip island. Do not thumb every
directory — names only, virtualized. A folder tree that decodes is how you miss the
arrow-key verify.

The tree was deferred from PR 6 to the metadata slice, now PR 9, with the other panes. The command id and the
hidden-by-default layout still land in PR 6 so the island math (`usable_canvas`) is not
retrofitted.

## Overlays that stay on the hot path

All of these are extra draws in the **same** present, budgeted, and they still **stop
presenting when idle** ([03-rendering.md](03-rendering.md) rule 4). Toggles are visible
commands that request one redraw.

| Overlay | PR | Cost |
|---|---|---|
| Loupe (hold `Z`) | 6 | Camera / viewport. Same texture |
| Hold-previous (`\` ) | 6 | Five-slot LRU already there. Do not open a second session |
| Clipping blinkies (`C`) | 6 on display-referred luminance; **accurate RAW clip waits for PR 11** full decode, same rule as the adjust pane ([07-photo-editing.md](07-photo-editing.md)) | Shader |
| Pixel grid | 6, only at ≥ 400 % | Shader, cheap |
| Canvas background / checkerboard | 6 | Clear colour + optional shader |
| On-canvas info (`O`) | 6 for filename/index/zoom; exposure triangle fills in PR 9 | ImGui-style overlay or a tiny island; not a swapchain text atlas of EXIF |
| AF-point quads | 9 | A few coloured quads from maker notes already in the property model. No extra file read |
| Eyedropper readout | 9 | **One pixel** staging readback on demand, never a full-texture download. Show sRGB 8-bit and hex |
| Histogram | 11, on the adjust pane as specified. A viewer histogram is the same compute reduction, optional toggle | Compute |

Focus peaking, zebras, channel isolation: v1.1. They are shaders, but they are develop/NLE
chrome and they are not needed to cull a dump.

## Status, sort, filter, typeahead

Chrome, in-memory, no decode.

- **Status / title:** `filename — 3/247 — 6000×4000 — 95 % — ★★★`. Index and listing stats
  come from the folder model, not from the decoder.
- **Sort (PR 4):** name, mtime, size, type. **EXIF date-taken waits for PR 9** so PR 4 does
  not parse every file. Remember the user's sort. *(macOS 2026-09-24: the sort orders, including date taken, ship in the View ▸ Sort By menu. Date-taken keys are read once per file by one background job and the listing re-sorts in place when they land; a file with no stamp sorts by mtime. Windows 2026-09-24: the same five orders and a descending switch in View ▸ Sort by and in Settings, applied by the ABI session (`mv_folder_set_sort`) and saved as `[view] sort`.)*
- **Filter:** all / photos / videos / RAW. In-memory flag on the listing. RAW flag is
  meaningful from PR 7.
- **Typeahead:** with the **filmstrip or gallery** focused, typing jumps to the first item
  whose name starts with what was typed (Explorer-style, 300 ms idle to reset). With the
  **canvas** focused every letter is already a command, so `/` opens a find box over the
  already-loaded listing instead (plan/12 2026-09-13). On the gallery's folder row, `/`
  finds a folder tile by the start of its name. `Ctrl+G` go-to index.
- **Wrap** at end of folder: on by default, toggle in settings.
- **Session:** window placement, last folder, zoom mode (fit / 100 % / sticky), wrap,
  background. Not a catalog.

## Sticky zoom

Culling bursts at 100 % is the point. When advancing:

- **Sticky off (default):** each item fits, matching today's lab.
- **Sticky on (`S`):** keep zoom and pan centre as a fraction of the image. Same-size
  burst: the interesting corner stays. Different aspect: centre is preserved, not a
  jump to 0,0.

This is camera state, not a decode. It must not disable prefetch.

## Animation and multi-page

GIF/APNG/WebP already present on QPC ([04-image-pipeline.md](04-image-pipeline.md)).
When the current item is animated: Space play/pause, `,` `.` frame step, like video.
TIFF pages, ICO sizes, HEIC sequences: `Ctrl+PageUp` / `Ctrl+PageDown`. One navigation
stop in the folder; pages are not extra filmstrip items.

## Speed rules for this surface

A feature that violates these does not ship, even if it is on this page:

1. **Key-repeat next must stay inside the generation-counter + prefetch design.** No
   synchronous `mv_image_open` on the UI thread. Warm arrow-key &lt; 40 ms is still PR 4's
   verify.
2. **Copy, move, delete, reveal, wallpaper, export, share** run on the I/O or worker pool.
   Completions update chrome. The UI thread may open a picker; it may not copy bytes.
3. **Overlays do not start a present loop that never idles.** A still with blinkies off
   and no cursor is zero presents. A still with blinkies *on* may present (it is animating);
   that is an explicit exception, labelled in the F3 overlay, and it is why blinkies are a
   toggle not a default.
4. **Pairing and companion hiding happen at scan**, not on each next.
5. **No slideshow transition shader.** No decode to fill the palette. No full-texture
   readback. No catalog of the disk.
6. **Do not add a command that needs a library/album database.** SQLite is the thumb
   cache and (later) an index of sidecars, not iPhoto.

## Always-on-top, wallpaper, touch

- **Always-on-top** (`Ctrl+Shift+A`): `HWND_TOPMOST`. Photographers put the viewer on a
  second monitor. Cheap, host-side.
- **Set as wallpaper:** I/O thread, `IDesktopWallpaper` / `SystemParametersInfo`. Current
  item only, stills only. Not a slideshow-as-desktop.
- **Touch:** swipe next/prev, pinch zoom toward contact. Same camera as wheel/drag. Not a
  second present path.
- **Dark/light** follows the OS. WinUI free. High contrast too (D1).

## What this is not

Standard-viewer ideas that fail the speed bar, D4/D5, or "this is not a library":

| Idea | Why not in v1 |
|---|---|
| Keymap editor | v1.1, after the default map is real |
| Side-by-side compare workspace | v1.1. Hold-previous is the cheap cousin |
| Burst-stack as one filmstrip item | Heuristic, can hide files. Live Photo / RAW+JPEG pairing is exact; burst grouping waits |
| Print / contact sheet | v1.1. Not the hot path, but it is a week of print UI |
| ~~Card ingest with verify~~ | **Moved to the Import add-on, PRs 16–19 (2026-09-24)** ([18-import.md](18-import.md)) |
| GPS map, keywords, colour labels | v1.1 metadata |
| Quick-export presets on one key | After PR 10 export exists and has been used |
| PiP / compact overlay | v1.1. Second window is a second present path unless it is DWM-only |
| Focus peaking, zebras, RGB channels | v1.1 shaders |
| Cloud albums, AI cull | Rule 6; also not a viewer. Local search and faces are planned separately in [17](17-local-ai-search.md) (post-v1, opt-in) |
| Duplicate finder, catalog, albums | Library product |
| Slideshow crossfade / music | Drops frames / movie player |
| Plugins, scripting, hex view, WIA capture, PDF, Cast | Out of scope |
| Growing XAML over the canvas | D1 amendment |

## PR wiring

Later slices **add rows to the table**. They do not grow a second router.

| PR | What lands |
|---|---|
| 4 | Arrow next/prev, sort (name/mtime/size/type), five-slot LRU that hold-previous will use. No command table yet |
| 5c | Transport commands, SMTC, `,` `.`, media keys, `Q` `E` tap-speed / hold-skim, speed dropdown at the bar's right, bottom-centre transport strip |
| **6** | Router, default browse/view/slideshow map, `?`, Space semantics, marks, F7/F8, status, typeahead, sticky zoom, companions-as-hidden, loupe, hold-previous, blinkies (display-referred), pixel grid, background, folder tree island (or slip), always-on-top, fullscreen chrome hide, animation play/pause |
| 7 | RAW+JPEG pairing, Live Photo pairing (needs HEIC + video), filter: RAW, companion RAW+JPEG as one stop. Landed: `;` play motion, unbound Open RAW / Open JPEG rows, `Esc` ends motion first, RAW / LIVE tile badges. Filter: RAW is **not** in this slice |
| 8 | Package the existing viewer; About and release setup, no new feature commands |
| 9 | Folder tree, `I` pane, `O` overlay fills exposure, AF points, eyedropper, sort by date taken |
| 10 | `[` `]` lossless rotate from the viewer, crop mode keys, `H` / `V` flip (deferred from PR 6 with the other geometry ops). Written for Windows and macOS 2026-09-24 with `Shift+C`, `Ctrl+S`, `Ctrl+Z`, `Ctrl+R` ([12](12-decision-log.md)) |
| 11 | `E` pane, accurate RAW clipping, histogram |
| 12 | Rating keys, `F2` rename writes, user comment in the pane |
| 13 | Trim mode takes `[` `]` |
| 15 | Clipboard formats, Share, tabs, jump list, `Ctrl+Tab`, `Ctrl+E` reveal in Explorer (deferred from PR 6 with the other shell verbs) |
| 16–19 | Import add-on commands, present only while it is installed ([18-import.md](18-import.md#commands)). Base app, PR 16: `F8` across volumes deletes the source only after verify |
| 27–28 | Voice query add-on commands, present only while it is installed ([19-voice.md](19-voice.md#commands)). Hold-to-talk is `Ctrl+Shift+Space` / `⌘⇧Space`; `Space` stays next / play |

**Verify (PR 6, additive with the existing line):** keyboard-only browse of a real folder —
open, next/prev, zoom/fit/100 %, mark, copy-to a destination, delete to Recycle Bin,
fullscreen, slideshow start/stop — without the mouse, with `?` listing those bindings,
and with PR 1's present-loop still holding. The roadmap line
([10-roadmap.md](10-roadmap.md) PR 6) adds a file dropped into the folder appearing
without restart and animation timing matching a browser.

## ABI

No new hot-path ABI. Commands invoke existing session calls (`mv_folder_select`,
`mv_image_open`, camera, later transport / edit / meta). A `command_id` enum may live in
the public header as integers so C# and C++ agree on canvas-owned effects; bindings never
cross the line.

Pixels still do not cross ([14-abi.md](14-abi.md)). `?` is chrome.
)
