# 16 — Commands, keyboard, and mouse-free use

v1 is **fully usable without a mouse.** That is a product requirement, not a settings page.
Configurable keymaps are **v1.1** ([10-roadmap.md](10-roadmap.md)). Do not build a remap UI
before the default map has been used in anger — same argument as batch metadata in
[06-metadata.md](06-metadata.md).

D1 exists because FastStone users bounce without keyboard behaviour. PR 6's verify line is
the first falsifiable cut of this doc; later PRs **register commands into the same table**
rather than inventing a second input path.

Speed is the constraint. A command that decodes, hits disk, or takes a marshalling hop
per key-repeat is a bug, not a feature. The present-loop gate from PR 1 still holds.

## The split

| v1 (PR 6, then additive) | v1.1 |
|---|---|
| One complete default map covering browse, view, video, edit, metadata, trim | Remap UI, import/export JSON, alternate layouts (FastStone / IrfanView / vim) |
| Every command has a key, including ones that currently look like buttons | Per-profile maps, user chords beyond a couple of prefixes |
| `?` overlay listing the **current mode's** bindings | |
| Command palette (`Ctrl+K`) so nothing has to be memorised | |
| Focus model that crosses canvas + XAML islands | |

The Mac host (Milestone F) writes chrome twice (**D9**). Bindings live in the host; the
core exposes command *effects* through the existing ABI. Do not put Win32 virtual-key codes
in `image/`, `player/`, `edit/`, or `meta/`.

## One key router

Islands each registering `KeyboardAccelerator`s is how you get two handlers and a swallowed
arrow key. **One router, on the UI thread, in the native window procedure.**

1. If a XAML text control has focus (rename, search, user comment, palette filter), keys go
   to the island. `Esc` blurs back to the canvas.
2. Else map `(key, mods, mode)` → `command_id` through the default table and dispatch.
3. Mouse-move, wheel, and pan/zoom springs stay native on the canvas HWND — no marshalling
   hop per mouse-move ([02-architecture.md](02-architecture.md)).

Dispatch is an array index, not a string lookup. The palette filters the same static table
in memory. No I/O on keydown.

**Tap and hold may be two commands on one key**, and `A` / `D` on a clip are the case that
earns it: a tap is a discrete adjustment (speed), a hold is a continuous one (skim). The
distinction is typematic repeat — the down edge of a tap does nothing, the first repeat makes
it a hold, and key-up decides. This is a shape to use sparingly; it is here because shuttling
and speed are the same gesture at two durations, not to double the key count.

**Modes** (a handful, not a modal editor):

| Mode | When | Arrow keys mean |
|---|---|---|
| **Browse** | Default, canvas focused | Prev / next file. When zoomed, `↑` `↓` pan and `←` `→` still navigate (FastStone). Pan with middle-drag / hold `Space` is mouse; keyboard pan is `Shift+arrows` when zoomed |
| **Filmstrip** | Bottom island focused | Prev / next thumb; `Enter` focuses canvas |
| **Pane** | Folder tree / metadata / adjust / jobs | In-pane traversal; `Esc` returns to canvas |
| **Video** | Current item is a clip, playing or paused | Frame step when paused (`,` `.` and arrows); `J` `K` `L` transport |
| **Slideshow** | After `Enter` | Next on a timer; `Esc` leaves |
| **Crop** | Adjust geometry (PR 9) | Nudge crop; `Enter` commits, `Esc` cancels |

`Esc` walks **out**: crop → pane → fullscreen / slideshow → canvas. It does not quit from a
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

### Browse

| Key | Command |
|---|---|
| `←` `→` or `A` `D` | Previous / next, **in every mode including on a clip**. `A` `D` were briefly reinterpreted as the clip's speed/skim pair, which stopped the two most obvious walk-the-folder keys walking the folder as soon as a video was open; transport moved to `Q` `E` |
| `Space` / `Backspace` | Next / previous. **Space is not the lab sweep after PR 6.** On a video or animation, Space is play/pause |
| `Home` / `End` | First / last |
| `PageUp` / `PageDown` | Skip ~10, or previous / next *folder* when the folder tree is populated |
| `Delete` | Recycle Bin, confirm (PR 6). Marks if any, else current |
| `F2` | Rename, IME-aware |
| `Enter` | Slideshow |
| `F` | Fullscreen. `F3` stays the frame-time overlay |
| `Ctrl+O` | Open folder |
| `Ctrl+W` / `Alt+F4` | Close window |
| `Ctrl+Tab` | Next tab (PR 14) |

### View

| Key | Command |
|---|---|
| `0` | Fit |
| `1` | 100 % |
| `2` `3` | 200 % / 400 % |
| `4` | Fill |
| `+` `-` | Zoom toward centre when no cursor; toward cursor when there is one |
| `Ctrl+0` | Reset pan/zoom (not rating-0 — rating is `Ctrl+Shift+0` or numpad, see below) |
| `H` / `V` | Flip horizontal / vertical |
| `[` `]` | Rotate −90 / +90. Lossless JPEG when that is the only op (PR 9), from the viewer, no edit pane required |
| `I` | Metadata pane (PR 8) |
| `E` | Adjust pane (PR 10) — **collides with `Q` `E` transport below, landed in 5c. PR 10 picks a different key; this row is not a claim on `E`.** |
| `T` | Filmstrip show/hide. Writes the preference for the mode you are in — folder open or single image (`Settings` menu, PR 4) |
| `G` | Gallery: full-client thumbnail grid of the folder. `Esc` or `Enter` leaves it; `Enter` and a click open the item under the cursor (PR 4) |
| `Ctrl+Shift+E` | Folder tree show/focus |
| `O` | On-canvas info overlay (filename, index, exposure triangle once PR 8 can fill it) |
| Hold `Z` | Loupe: 100 % around a keyboard-nudgeable point (or last cursor). Same texture, camera change, no decode |
| `\` hold | Previous item for burst pick. Uses the five-slot GPU LRU ([04-image-pipeline.md](04-image-pipeline.md)); must not `mv_image_open` a replacement |
| `;` | Play Live Photo / motion once, return to the still. Required: hover-to-play fails the no-mouse bar |
| `B` | Cycle canvas background (black / gray / white / checkerboard). Checkerboard is the alpha case |
| `S` | Sticky zoom on advance (keep scale + pan fraction). Default off |
| `C` | Clipping blinkies. Display-referred in PR 6; accurate RAW clip from PR 10 |

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

### Rate (PR 11)

| Key | Command |
|---|---|
| Numpad `0`–`5` | Rating. No numpad: `Ctrl+Shift+0`–`Ctrl+Shift+5` |
| `U` | Unflag / clear colour label (label write is v1.1; `U` is a no-op until then) |
| `X` | Reject mark (convenience for `Insert` + next). Does not delete |

### Video (PR 5c) and trim (PR 12)

| Key | Command |
|---|---|
| `Space` | Play / pause |
| tap `Q` `E` | **Playback speed** one rung down / up the ladder 0.25 / 0.5 / 1 / 1.5 / 2 / 4. The command bar's speed dropdown is a *view* of this: native owns the rate, pushes it to the island, and the dropdown posts back — one router, never two owners |
| hold `Q` `E` | **Skim** −2 s / +2 s per key repeat. Non-exact seek (nearest keyframe) while held, so a shuttle cannot queue a decode-forward per repeat; the release settles exactly, the same two modes as a scrubber drag and its release. Intent accumulates across the burst — re-reading the position each repeat asks to move from a point the last press already rounded backwards |
| `J` `K` `L` | −10 s / pause / +10 s |
| `,` `.` | Frame step (already in [05-video-pipeline.md](05-video-pipeline.md)) |
| `Shift+M` | Mute (`M` is not mute — reserved so a FastStone-layout preset can put Move on `M` in v1.1) |
| `[` `]` | In / out markers when trim is armed (PR 12). In browse they rotate; trim mode takes them |
| `Ctrl+←` `Ctrl+→` | Previous / next keyframe |
| Media keys | SMTC, same commands |

### Slideshow (PR 6)

| Key | Command |
|---|---|
| `Enter` | Start |
| `Space` | Pause |
| `+` `-` | Interval |
| `.` | Blackout |
| `R` | Shuffle |
| `Esc` | Leave |

No transition pass. Next is the same navigation command as browse, on a timer, so prefetch
and the generation counter stay in play. A crossfade is two textures in the present loop
for a feature nobody opens a camera dump for.

### Command palette and `?`

- `Ctrl+K` (also `Ctrl+Shift+P`) opens a searchable list of every registered command with
  the current binding shown. Running an entry is the same dispatch as a key.
- `?` toggles a mode-sensitive cheat sheet over the canvas. Chrome, not a settings page.
  People learn FastStone this way.

Both are XAML flyouts with `ShouldConstrainToRootBounds = false` so they are not clipped
by a strip (PR 3). They do not composite onto the swapchain.

## Folder tree

Named in [02-architecture.md](02-architecture.md) and in D1's FastStone rationale; no PR
owned it. **PR 6**, as a **third island, left strip**, hidden by default (`chrome_left_px = 0`
until shown). `Ctrl+Shift+E` shows and focuses it. Arrows walk, `Enter` opens, `PageUp` /
`PageDown` from the canvas move to sibling folders.

Do not put the tree in the command-bar island or the filmstrip island. Do not thumb every
directory — names only, virtualized. A folder tree that decodes is how you miss the
arrow-key verify.

If PR 6 overruns, the tree slips to PR 8 with the other panes. The command id and the
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
| Clipping blinkies (`C`) | 6 on display-referred luminance; **accurate RAW clip waits for PR 10** full decode, same rule as the adjust pane ([07-photo-editing.md](07-photo-editing.md)) | Shader |
| Pixel grid | 6, only at ≥ 400 % | Shader, cheap |
| Canvas background / checkerboard | 6 | Clear colour + optional shader |
| On-canvas info (`O`) | 6 for filename/index/zoom; exposure triangle fills in PR 8 | ImGui-style overlay or a tiny island; not a swapchain text atlas of EXIF |
| AF-point quads | 8 | A few coloured quads from maker notes already in the property model. No extra file read |
| Eyedropper readout | 8 | **One pixel** staging readback on demand, never a full-texture download. Show sRGB 8-bit and hex |
| Histogram | 10, on the adjust pane as specified. A viewer histogram is the same compute reduction, optional toggle | Compute |

Focus peaking, zebras, channel isolation: v1.1. They are shaders, but they are develop/NLE
chrome and they are not needed to cull a dump.

## Status, sort, filter, typeahead

Chrome, in-memory, no decode.

- **Status / title:** `filename — 3/247 — 6000×4000 — 95 % — ★★★`. Index and listing stats
  come from the folder model, not from the decoder.
- **Sort (PR 4):** name, mtime, size, type. **EXIF date-taken waits for PR 8** so PR 4 does
  not parse every file. Remember the user's sort.
- **Filter:** all / photos / videos / RAW. In-memory flag on the listing. RAW flag is
  meaningful from PR 7.
- **Typeahead:** with canvas or filmstrip focused, typing filters the already-loaded
  listing (Explorer-style, 300 ms idle to reset). `Ctrl+G` go-to index.
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
| Card ingest with verify | v1.1. Adjacent product (Photo Mechanic). Watcher already sees files appear |
| GPS map, keywords, colour labels | v1.1 metadata |
| Quick-export presets on one key | After PR 9 export exists and has been used |
| PiP / compact overlay | v1.1. Second window is a second present path unless it is DWM-only |
| Focus peaking, zebras, RGB channels | v1.1 shaders |
| Face detect, AI cull, cloud albums | Rule 6; also not a viewer |
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
| **6** | Router, default browse/view/slideshow map, `?`, palette, Space semantics, marks, F7/F8, status, typeahead, sticky zoom, companions-as-hidden, loupe, hold-previous, blinkies (display-referred), pixel grid, background, folder tree island (or slip), always-on-top, fullscreen chrome hide, animation play/pause |
| 7 | RAW+JPEG pairing, Live Photo pairing (needs HEIC + video), filter: RAW, companion RAW+JPEG as one stop |
| 8 | `I` pane, `O` overlay fills exposure, AF points, eyedropper, sort by date taken |
| 9 | `[` `]` lossless rotate from the viewer, crop mode keys |
| 10 | `E` pane, accurate RAW clipping, histogram |
| 11 | Rating keys, `F2` rename writes, user comment in the pane |
| 12 | Trim mode takes `[` `]` |
| 14 | Clipboard formats, Share, tabs, jump list, `Ctrl+Tab` |

**Verify (PR 6, additive with the existing line):** keyboard-only browse of a real folder —
open, next/prev, zoom/fit/100 %, mark, copy-to a destination, delete to Recycle Bin,
fullscreen, slideshow start/stop — without the mouse, with the `?` overlay listing those
bindings, and with PR 1's present-loop still holding.

## ABI

No new hot-path ABI. Commands invoke existing session calls (`mv_folder_select`,
`mv_image_open`, camera, later transport / edit / meta). A `command_id` enum may live in
the public header as integers so C# and C++ agree on canvas-owned effects; bindings never
cross the line.

Pixels still do not cross ([14-abi.md](14-abi.md)). The palette and `?` are chrome.
)
