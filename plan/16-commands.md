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
for 3 s after the last navigation; the bottom hot-edge does the same. A clip's transport is not
one of those strips: it floats over the video in both modes and auto-hides on its own rule
(below, and [12](12-decision-log.md) 2026-09-26).

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

`Esc` walks **out**: crop → pane → gallery → fullscreen / slideshow → canvas → result list. The gallery
covers the canvas like an overlay, so it closes before the window-level states. A search result
list ("Search: …", plan/17) is a place rather than an overlay, so it is the outermost step: with
nothing else to leave, `Esc` is the path bar's **Back to folder** (2026-09-27, both hosts; the
router's `result_list` target). Over a list the grid closes first and the next `Esc` goes back, the
same two steps as the gallery over a folder then the canvas. It does not quit from a
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
swapchain ([12-decision-log.md](12-decision-log.md) 2026-09-07). The one exception is a clip's
transport bar, which floats over the bottom of the video and auto-hides (2026-09-26):

- Playing, it hides after `kTransportIdleMs` (2.5 s) with no activity; pointer movement, a click,
  the wheel, a transport command (`Space` `K` `J` `L` `Q` `E` `,` `.`, speed, mute, `↑` `↓`
  volume), `Tab`, or a fullscreen change bring it straight back.
- Paused, ended, hovered, scrubbing, a menu open, keyboard focus in it, or a screen reader
  running: it stays. Never over the gallery or Settings, never on a Live Photo stop.
- Fullscreen: the pointer hides with it while over the video. Windowed: never.
- Only visual: no refit, no second action on the wake, no repaint while idle. The rule is
  `shell/transport_autohide.h`, shared by both hosts.

## Default map

FastStone / IrfanView muscle memory, not vim. Vim is a v1.1 preset.

Number-row `0`–`4` is **zoom**, matching the lab today. Ratings do not steal those keys.

On an empty window, Space starts the T-Rex runner. While the game is active,
`3` toggles its 2D/3D view, Space jumps/retries, and Esc leaves — the runner sprints
off and the scene folds away (the intro in reverse) before the welcome card returns;
opening a file over the runner skips that. Switching the view
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
| *(menu, unbound)* | **Recent folders** (PR 15; 2026-09-28): the jump list's folders, reachable without a mouse. Mac: File ▸ Open Recent (the menu bar, or Help's search). Windows: Open ▸ Recent folders on the command bar (`Tab` to the bar, `Enter` on Open, `Right` into the list). Chrome over the same list, not a keyed row; a folder that has gone beeps and leaves the list, as a welcome-card click does |
| `Ctrl+E` | Show the current file in Explorer, selected |
| `Ctrl+,` | Settings (view defaults and remappable keys). Colour scheme, chrome/canvas/overlay palette, and a user-supplied font are **v1.1** ([10-roadmap.md](10-roadmap.md)) |
| `Ctrl+W` / `Alt+F4` | Close window |
| `Ctrl+Tab` | Next window / tab (the multi-window PR after PR 15; [12](12-decision-log.md) 2026-09-25 (later)) |

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
| `Shift+A` | Adjust pane (PR 11; `⇧A` on Mac). Not `E`: that is the clip transport (`Q` `E`, 5c), as this row used to warn. Shows the pane and focuses its first slider; again (or the pane's close button) hides it. Stills only |
| `T` | Filmstrip show/hide. Writes the preference for the mode you are in — folder open or single image (`Settings` menu, PR 4) |
| `G` | Gallery: full-client thumbnail grid of the folder. `W` / `S` and Up / Down move by row, `A` / `D` and Left / Right move by item. `Enter` opens the selection in the normal viewer, leaving fullscreen/slideshow and restoring the filmstrip if enabled. A click opens it in the viewer. `Esc` closes the gallery. Navigation applies while the gallery is visible, even before keyboard focus moves into it. **Child folders (PR 26)** are big tiles when the folder holds only folders, and one row above the photos when it holds both. Up from the first photo row moves onto that row, Left / Right move among them, `Enter` opens the tile, Down returns to the photos. `/` on that row finds a tile by the start of its name. **A clip does not play under the gallery (issue #44):** opening it pauses a playing clip, a clip selected in it waits paused on its first frame for Play, and closing it resumes only the clip that was playing when it opened |
| `Ctrl+Up` (`⌘↑` on Mac) | Up one folder (PR 26). Opens the enclosing folder and selects the folder you just left. The path lives in the command bar, just left of `?`, so it stays on screen (including while a photo is open) without a row of its own; a long middle opens a menu of hidden ancestors. Up (↑) and Root (house) icon buttons remain outside the scrolling trail on both hosts; Root opens the first breadcrumb (the highest folder reached). Not bound to Backspace, which is Previous |
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
| `F8` | Move, same rule. Same-volume `MoveFileEx`; across volumes a **verified** copy (hashed while read, read back uncached, compared), then the delete (plan/18: never an unverified source). I/O thread |
| `Ctrl+C` | Clipboard `CF_HDROP` of original(s) |
| `Ctrl+Shift+C` | Copy path(s) as text |
| `Ctrl+Alt+C` | Flattened PNG/JPEG of the current view (edits baked). Worker, not UI thread |
| `Ctrl+Shift+S` | Windows Share (`IDataTransferManager`) |
| `Ctrl+E` | Reveal in Explorer |
| `Ctrl+Enter` | Open with the user-configured external editor (`ShellExecuteEx`, no wait) |

**Drag-out** (mouse, both hosts, 2026-09-28): a gallery or filmstrip cell drags the original file
(and its pair), copy-only and read-only; a marked cell drags every marked item in listing order, an
unmarked one only itself. The canvas's drag (a drag at fit on Windows, `⌘`-drag on the Mac) is
copy-only too. A drag of ours let go over our own window, gallery or filmstrip is refused, not a
reopen of the folder it came from.

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

### Import (Milestone G, only while the add-on is installed)

| Windows | Mac | Command |
|---|---|---|
| `Ctrl+Shift+I` | `⌘⇧I` | Open the Import window (with the viewer's marks for "Marked in viewer") |
| `Ctrl+F` | `⌘F` | Search. **Without Local search** (not installed, being removed, or failed to load): **file search** (2026-09-28, see "File search" below). With it: Local search (AI pack, plan/17), the search panel, keyboard-focused. The same command runs from the search icon at the right end of the command bar's folder path (and beside "Search: …" while a result list is shown), which is always there; with the pack starting at launch, the key or a click opens the panel once it attaches; the gallery sits below the command bar on both hosts, so the icon stays in view over the grid. In the field, typing searches (plan/17 "Query syntax"); while a name is offered for the word being typed, `Tab` completes it (2026-09-28); `Enter` (or Down) moves into the results on the first tile — words still being searched (just typed, or while indexing) wait for their answer, then move; nothing found stays in the field. In the grid, arrows move, `Enter` opens the results in the viewer on that tile, `Ctrl+Enter` / `⌘↩` opens them all as the gallery, a typed character goes back to the field; `Esc` returns to the field, then closes (2026-09-28) |
| `Ctrl+Shift+F` | `⌘⇧F` | Find similar to the still or paused frame on screen |
| `N` / `Shift+N` | `N` / `⇧N` | On a clip opened from results: next / previous matching moment |
| `Ctrl+Shift+F7` | `⌘⇧F7` | Import the marked (else current) files now with the last preset |

Both rows are in the one command table and are listed, routed and shown in `?` / Settings **only while
Import is installed and loaded**; with it absent the keys fall through as if unbound (plan/18).
Checked free against the live table on 2026-09-24. Inside the Import window (its own keys, not
table rows): `Enter` / `Return` imports (on a focused tile it opens that file in the viewer, for
culling first), `Ctrl+Enter` / `⌘Return` imports from anywhere, `Space` toggles a tile and pauses /
resumes while copying, `Shift+Space` toggles its day, `Ctrl+Tab` / `Ctrl+Shift+Tab` (`⌃Tab`)
the next / previous source, `Ctrl+J` / `⌘J` ejects, `Esc` closes the window and the import carries on.

### Edit workspace (PR 29, [20](20-edit-workspace.md))

| Key | Command |
|---|---|
| `Enter` (`Return`) | Open / close the Edit workspace: **Edit image** on a still, **Edit video** on a clip. Browse and video modes only; in crop it still applies the crop, in trim it still saves the cut, in the gallery it still opens the tile |
| `Shift+C` / `Shift+A` / `Ctrl+T` | Open the workspace on Crop / Colour / Trim (and start cropping / arm trim, as before) |
| `I` / `Ctrl+J` | With the workspace open: its Info / Jobs tab (closed, they are their own panes, as before) |
| `A` / `X` (in crop) | Next aspect preset (Free, Original, 1:1, 4:3, 3:2, 16:9, 5:4) / swap portrait and landscape. `A` never walked the folder in crop |
| hold `Y` | Show the original (the edit stack is untouched). Browse mode, stills |

Appended to the table (every earlier row keeps its Settings index). Checked free on 2026-09-26: `Enter`
had no browse/video row, `X` and `Y` had none, `A` is dead in crop.

### Rate (PR 12)

| Key | Command |
|---|---|
| Numpad `0`–`5` | Rating. No numpad: `Ctrl+Shift+0`–`Ctrl+Shift+5` |
| `U` | Unflag / clear colour label (label write is v1.1; `U` is a no-op until then) |
| `X` | Reject mark (convenience for `Insert` + next). Does not delete. **Not built in PR 12** (a mark, not a metadata write; see 12) |
| `Ctrl+I` (`⌘I`) | Edit comment (PR 12): shows the metadata pane and puts the keyboard in its comment field. `Return` saves, `Esc` drops the edit; both return to the canvas. **PR 29 (Mac):** the pane has no comment box any more (every tag is edited in *All tags*); the key shows the pane |

Rating keys write the item on screen only (a batch is v1.1), in browse, video, island and gallery
modes, not in a slideshow or crop mode. A JPEG is rewritten in place; anything else gets an XMP
sidecar. The command bar shows what landed ("★★★★☆", or "— IMG_1234.xmp" when a sidecar took it).
**macOS: `⌘⇧3`, `⌘⇧4` and `⌘⇧5` are the system's screenshot shortcuts and never reach the app**
unless they are turned off in System Settings ▸ Keyboard ▸ Keyboard Shortcuts ▸ Screenshots; the
keypad works regardless. Remapping in Settings is the way round it until the owner picks another
chord.

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
| `Ctrl+T` | Arm / disarm trim on a clip (PR 13). Trim layers over video: the keys above keep working |
| `P` | Trim: preview the cut (A–B loop over exactly what the keyframe save writes) |
| `Enter` / `Shift+Enter` | Trim: save the keyframe cut (instant) / the frame-accurate re-encode (slower) |
| `Ctrl+X` | Trim: a copy without in–out |
| `Backspace` / `Delete` | Trim: clear the markers. In trim, `Delete` never moves the clip to the Trash / Recycle Bin |
| `Ctrl+←` `Ctrl+→` | Previous / next keyframe, in trim mode (outside trim they are the sibling-folder walk) |
| `Ctrl+J` | Jobs pane (PR 13): `↑` `↓` choose, `Delete` cancel, `R` retry, `Enter` reveal the output |
| `Ctrl+S` / `Ctrl+B` | On a clip: clip tools (PR 14) / split at the playhead |
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
- **File search (2026-09-28, owner: "can we still have basic file search"):** part of the base
  app, no add-on and no index. `Ctrl+F` / `⌘F` or the path bar's search icon, while Local
  search is not installed, shows the gallery if it is hidden and opens a field over the grid
  ("Find files by name in this folder"). Typing filters the grid, folder tiles too, by a case-
  and accent-insensitive substring of the name, ~70 ms after the last key; a count shows
  "N of M". It is a view over the gallery only: the arrows move among the matches, a hidden
  selection hands over to the first match, and the viewer and filmstrip still walk the whole
  folder. `Enter` / Down go to the grid on the first match; `Esc` clears the field, then
  closes it; closing the gallery or opening another folder closes it too. This folder only
  (the listing already in memory): no disk walk. Names are folded once per listing off the
  UI thread; a keystroke is one pass over them (≤ 2 ms for 10,000 items). With Local search
  installed the key and the icon open its panel instead, where `file:name` searches names
  (plan/17 "Query syntax") among the indexed files.
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
| Catalog, albums | Library product. (An exact-duplicate finder is in the Import add-on, PR 54, [18](18-import.md#find-duplicates-pr-54).) |
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
| 11 | `Shift+A` adjust pane (not `E`, which is the clip transport), accurate RAW clipping, histogram |
| 12 | Rating keys, `F2` rename writes, user comment in the pane |
| 13 | Trim mode takes `[` `]`. Written for Windows and macOS 2026-09-25 ([12](12-decision-log.md)): `Ctrl+T` arms trim on a clip; in trim `P` previews the cut as an A–B loop, `Enter` saves the keyframe cut, `Shift+Enter` the re-encode, `Ctrl+X` removes in–out, `Ctrl+←` `Ctrl+→` walk keyframes, `Backspace` / `Delete` clear the markers (never trash the clip). `Ctrl+J` is the Jobs pane (`Delete` cancels the focused job, `R` retries, `Enter` reveals) |
| 14 | `Ctrl+S` on a clip opens the clip tools (rotate, split, frame, audio, remux, GIF / WebP; Export stays `Ctrl+S` on a still); `Ctrl+B` splits at the playhead |
| 15 | Clipboard formats (`Ctrl+Shift+C` path, `Ctrl+Alt+C` edited copy), Share (`Ctrl+Shift+S`), jump list, `Ctrl+E` reveal in Explorer (deferred from PR 6 with the other shell verbs). Tabs and `Ctrl+Tab` moved to the multi-window PR |
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

