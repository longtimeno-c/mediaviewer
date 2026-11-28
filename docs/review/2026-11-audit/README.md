# Project audit, 2026-11-25

A read-through of the desktop apps (Windows, macOS), the iPhone/iPad app and the add-ons at
`a232bda4`. This page is the index: the issues that matter most, the improvements, the performance
plan and the work packages. Each area's full report, with every finding and its `file:line`, is
beside it.

**How it was done, and its limits.** Eleven read-only passes over the code, one per area. Nothing
was built or run: no screen was clicked through, and no number below is a new measurement. Findings
marked **✔** were re-read in the code by the lead after the area pass reported them; the rest are
the area pass's reading only. "Suspected" means the code path is there but the effect on real files
or hardware was not shown. Parts not read are listed at the end.

| Report | Covers |
|---|---|
| [stills.md](stills.md) | Open, navigate, pan: `core`, `io`, `codec`, `gfx`, `image`, `canvas` |
| [video.md](video.md) | Playback, seek, trim: `player`, video `gfx`, `edit` clips |
| [shell.md](shell.md) | Native hosts, command table (all 228 ids), ABI |
| [file-jobs.md](file-jobs.md) | Copies and moves, atomic replace, Photos backup, Explorer extension, catalogue |
| [meta-edit-tests-ci.md](meta-edit-tests-ci.md) | Metadata writes, photo editing, tests, CI, build flags |
| [windows-chrome.md](windows-chrome.md), [windows-chrome-2.md](windows-chrome-2.md) | WinUI chrome: screen inventory and findings |
| [mac-chrome.md](mac-chrome.md) | SwiftUI chrome: screen inventory, parity with Windows, findings |
| [ios.md](ios.md) | iPhone/iPad app: 21 screens, findings, search latency |
| [addons.md](addons.md) | Add-on host, Local search, Driftwire, Enhance, Import, Phone access, Places, VPN |
| [docs.md](docs.md) | Docs against code; every README number against `docs/perf` |

## 1. Issues

### Could lose or damage a user's file

| # | Issue | Where | Status |
|---|---|---|---|
| D1 | Windows `replace_atomic` passes no backup name to `ReplaceFileW` and deletes the temp on any failure. On `ERROR_UNABLE_TO_MOVE_REPLACEMENT` the original is already gone, so the temp was the only copy. Used by in-place rotate and metadata writes | `src/io/replace_win.cpp:98` | ✔ |
| D2 | A folder moved across volumes ends with `remove_all(source)`: files added to the folder during a long copy are deleted uncopied | `src/shell/file_transfer.cpp:323` | ✔ |
| D3 | Lossless rotate from the viewer is gated on "is a JPEG" only. The metadata writer refuses files with an MPF table, a trailer or extended XMP; rotate does not, so a Motion Photo's video or an Ultra HDR gain map would be dropped from the original | `src/shell/edit_session.cpp:138`, `src/meta/write.cpp:1181`, `src/edit/lossless_jpeg.cpp:394` | ✔ gate; loss on real files suspected |
| D4 | Photos backup checks only that the destination is a directory; an empty `/Volumes/<share>` left after the share drops would pass, and the backup lands on the boot disk | `src/shell/photos_backup.cpp:871` | suspected |
| D5 | macOS first sidecar write uses `link(2)`, which FAT and exFAT (camera cards) do not have | `src/io/replace_mac.cpp:58` | ✔ code; card behaviour suspected |
| D6 | macOS drag-move inside one exFAT card or SMB share fails: `renamex_np(RENAME_EXCL)` with no fallback | file-jobs M1 | |
| D7 | macOS `replace_atomic` drops Finder tags, other xattrs, the ACL and the creation date of the original | meta-edit M1 | |
| D8 | After a rotation lands on disk, Undo is refused; the rotate write skips the post-write checks the metadata writer does | `src/shell/edit_session.cpp:180`, `:619`, `:690` | |
| D9 | A server upload ignores the name the service used (" (2)"), so a later undo removes someone else's file | file-jobs M2 | |
| D10 | Partial `.mvtmp` files stay on the NAS after a drop; the backup manifest has no corruption recovery; a transient share error can overwrite the catalogue's `library.json` | file-jobs M4–M7 | |

**The in-place rule.** CLAUDE.md used to say "never modify a user's original file", while rotating a
JPEG from the viewer and in-file metadata writes rewrite it in place by design
(`docs/design/07-photo-editing.md` "From the viewer"). The owner dropped the rule on 2026-11-25:
in-place writes stay. D1 and D3 are bugs either way, because they lose data the write was meant to
keep.

### Privacy and security

| # | Issue | Where | Status |
|---|---|---|---|
| S1 | `CrashCapture.Scrub` leaks filenames: the name pattern stops at whitespace ("My Holiday" survives from "My Holiday Photo.jpg") and lists only image and video extensions (`.pdf`, `.docx`, `.mp3` pass whole). No tests | `src.managed/MediaViewer.Chrome/CrashCapture.cs:33` | ✔ |
| S2 | Windows add-on load: the DLL's hash is checked, then it is loaded by path (time-of-check gap); no Authenticode check | `src/addon/manifest.cpp:812`, `loader_win.cpp:60` | |
| S3 | An older signed add-on version installs on a fresh install; the developer signing key is kept out of releases only by CI | `src/addon/store.cpp:258`, `cmake/import.cmake:93` | |
| S4 | Driftwire: five failed nearby connects from anyone on the LAN (bare TCP is enough) block nearby receiving for 30 s, shown only in the log. Issue #497 item 2 still open; the receiver still accepts protocol 1/2 hellos | `src/addons/driftwire/engine.cpp:1294`, `channel.cpp:242` | |
| S5 | Remote cache on Windows: a server-chosen name containing `:` may write outside the cache folder | `src/remote/cache.cpp:68` | suspected |
| S6 | VPN on Windows: DNS can leak in "All traffic"; the system proxy is changed before its undo record is written | `src/addons/vpn/vpn_win.cpp:200`, `service.cpp:589` | leak suspected |
| S7 | Add-on screen thumbnail nodes accept any path, UNC included, which is network access for an add-on | `IslandHost.Screens.cs:1316`, `src/addon/screen.cpp:140` | suspected |
| S8 | iPhone: Keychain writes are delete-then-add, so a kill between them loses the pairing; the privacy manifest lacks the App Group reason and the VPN extension has none | ios B4, B13 | |
| S9 | Windows release builds have no `/guard:cf` or `/CETCOMPAT`; the viewer parses untrusted media in process. Every metadata fuzzer is still non-gating | `ci.yml:1229-1250` | ✔ flags absent |

Checked and sound: add-on signatures and unpack, the Lua sandbox (no route to the network), model
downloads (HTTPS, SHA-256), Driftwire path checks and nonces, phone pairing codes (128-bit, single
use, 5 minutes), crash-report and telemetry tags on the native side, the updater's signature,
channel and rollback checks.

### Blocks the UI thread (hard rule 1)

| # | Issue | Where | Status |
|---|---|---|---|
| U1 | Windows: dropped files and paths from a second launch are probed with `GetFileAttributesW` on the UI thread; each launch path is probed twice | `src/shell/main.cpp:1289`, `:1305`, `:1332` | ✔ |
| U2 | Windows: the SQLite thumbnail store is opened on the UI thread in `mv_folder_open` | stills C3 | |
| U3 | Mac: an add-on screen's image node reads and decodes its file in the view body, on every redraw | `ScreenView.swift:707` | ✔ |
| U4 | Windows: `Directory.Exists` on every Local search folder-list redraw | `ManagePanel.cs:1362` | |
| U5 | iPhone: Keychain write inside the connection lock that the grid takes while drawing; the 64 px tile is decoded on the main thread on each cache miss | `LinkConnection.swift:591`, ios B6 | ✔ write under lock |
| U6 | Starting a Photos backup can block on share I/O; Enhance's Cancel blocks the add-on's worker; Import's resume hashes files on the control thread | file-jobs M3, addons M2, I1 | |
| U7 | A hung first instance makes every later launch wait up to 10 s with no window | `src/shell/single_instance_win.cpp:18` | suspected |

### Wrong behaviour

| # | Issue | Where | Status |
|---|---|---|---|
| B1 | Stills: in `submit_decode_to_lru` the "still selected" check and the publish are separate, with no generation check; a cached image published in between leaves the canvas on the old photo | `src/abi/abi.cpp:1209`, `:1263` | ✔ shape; not reproduced |
| B2 | Video: an exact seek at or past the last frame discards every frame, so the picture stays put. The Video Editor's End key does this | `src/player/media_source.cpp:77`, `video_decode.cpp:409` | |
| B3 | Video: a seek and its generation are not published together; during a scrub a frame from the previous seek can be shown | `media_source.cpp:79-89`, `demux.cpp:41` | suspected |
| B4 | Video: after a device loss the clip is retired and never reopened; audio is anchored before queued silence plays out | video C3, C4 | suspected |
| B5 | Mac: marks are matched by file name, so in a list with two `IMG_0001.JPG` both count as marked and an add-on's "files" action takes both | `FolderStore.swift:525`, `SlotItems.swift:46` | ✔ |
| B6 | Mac: thumbnail eviction can drop the selected item (the comment says it cannot) | `FolderStore.swift:690` | ✔ |
| B7 | Windows: `Guarded` and the native entry points catch every exception and only `Debug.WriteLine` it; release builds lose chrome bugs silently (`CrashCapture.Handled` exists) | `IslandHost.cs:1165` | ✔ |
| B8 | Windows: `SaveTheme` writes race on one temp file; Import's card checkboxes start unchecked and can switch auto-import off; Import's async-void handlers crash on unexpected JSON | `ThemePack.cs:160`, `ImportWindow.cs:959`, `:197` | |
| B9 | Windows: the Local search folder list rebuilds every row when any count changes (about once a second while indexing), losing keyboard focus | `ManagePanel.cs:1046` | ✔ |
| B10 | Windows: a failed folder listing wipes the expanded tree; the People grid applies a stale read after a scope change and can stop refreshing; a failed Split leaves faces hidden | windows-chrome-2 | |
| B11 | Windows: gallery zoom is reported to leave on-screen thumbnails at the old size. The resize code does handle a tile whose box is the image, so check in the app first | `IslandHost.Gallery.cs:132` | unverified |
| B12 | Windows image cache is keyed by path only: a file changed by another app shows old pixels on revisit | stills C2 | |
| B13 | Local search: the "index full" flag never clears when folders are removed, and nothing vacuums the database | `src/addons/ai/engine.cpp:1213` | ✔ flag set here |
| B14 | iPhone: pinch zoom shows no extra detail; the original is decoded at screen size and the viewer allows 4× | `LinkImages.swift:159`, `PagerController.swift:433` | ✔ |
| B15 | iPhone: Cancel on the first-copy screen forgets the pairing without asking; music stays paused after a clip; an unreachable server is not retried while the app is open; Save/Share downloads run on after being superseded | ios B5, B7, B9, B10 | |
| B16 | A lossless trim on a stream with no keyframe flags writes audio with no video and reports success | video C10 | suspected |

### Parity, keyboard and accessibility

- **Mac lacks:** Live Photo motion (`;`), Go to item (⌘G), `open_raw`, `open_jpeg`
  (`src/shell/command_coverage.inc:81-93`, ✔). Windows lacks Photos library backup, Final Cut Pro
  search and menu-bar entries by design.
- **No key on either host:** `kind_filter_next`, `pane_wider`, `pane_narrower`, `auto_enhance`,
  `tool_red_eye`, `tool_extend`, `tool_faces`. On Windows that leaves the mouse only.
- **Windows:** the Curves editor cannot be driven from the keyboard; error and success colours have
  no theme role (ignored by high contrast); gallery, update and Import text ignores the type scale.
- **Mac:** the welcome card and canvas are invisible to VoiceOver; gallery and filmstrip cells have
  no button, selected or marked traits; Import, Export and Local search have no menu-bar entries.
- **iPhone:** no hardware-keyboard support on iPad.

### Tests and CI

- Nothing in `src.ios` runs in CI: the package tests and 17 UITest files run by hand (✔).
- No Swift test imports `MediaViewerChrome` (#153); a stub bridge would unblock it.
- About 330 sleep sites in `tests/` (about 2 minutes of fixed sleeps per run) and 57 in the iPhone
  UITests, against the no-sleeps rule.
- `ctest` runs serially everywhere (about 2,100 cases, 15–20 minutes on the Mac) (✔).
- Mac CI never runs the real RAW and HEIC corpus and has no sanitizer leg.
- `tools/check-module-graph.ps1` has a rule for a `phone` module that no longer exists and none for
  `src/mobile` or `src/remote`, so their includes go unchecked (✔). No policy script scans Swift.
- The last completed `ci.yml` run on `main` at audit time failed at "Windows dependencies".

## 2. Performance

Where the README is slower than a native app or the project's own target
([docs.md §4.3](docs.md) has all fourteen):

| Measure | MediaViewer | Compared with |
|---|---|---|
| Cold open, Nikon RAW (Windows) | 957 ms | Windows Photos 754 ms |
| Cold open, Sony RAW (Windows) | 1,012 ms | Windows Photos 976 ms |
| Full RAW decode behind the preview (Windows) | 0.56–1.98 s | target < 500 ms; Mac 0.26–0.49 s |
| RAW first pixel (Windows) | 47–134 ms | target < 60 ms; Mac 28–50 ms |
| Mouse-drag pan p99 (Windows) | 20–85 ms | one 16.7 ms refresh |
| 4K HEVC (Windows) | 22 fps, p99 gap 636 ms | real time |
| 30 fps clip (Windows) | 27.4 fps | Media Player 27.6 fps |
| 1080p HEVC (Mac) | 1 dropped present, 5 skipped frames, A/V p99 11 ms | Windows 0 / 1 / 2.2 ms |
| iPhone search, key to answer | 110–271 ms | search itself 4.2 ms |

Every Windows and Mac number in the README matches its raw report except the convert seek time,
now corrected. Four README numbers have no raw report in `docs/perf` (People suggestions, Mac
conversion speed, the Simulator search times, Mac remote-folder timings).

### Stills (ranked; gains are estimates until measured)

1. **Park the neighbours until the selected photo is up (Windows).** Windows submits the four
   neighbour decodes with the selected one (`src/abi/abi.cpp:1855`, `:2931`); the Mac parks them
   (`src/shell/present_lab_mac.mm:327`) ✔. In the app the Nikon's full decode lands at 1.98 s
   against 0.64 s in the decode bench; on the Mac the gap is 0.12 s. Likely the biggest win.
2. **Build the mip pyramid on several threads.** One thread today on both platforms
   (`src/image/upload.cpp:153-226`, `upload_mac.mm`); `parallel_bands` gives identical output.
3. **Stop reading the whole RAW before the first pixel** (a LibRaw datastream); helps most on a
   cold cache and on cards.
4. **Issue the early open before the ~210 ms wait for the render device** at launch
   (`src/shell/main.cpp:14789-14823`).
5. Lower the priority of Windows background jobs below the view's; decode RAW thumbnails at
   thumbnail size, not 2048 px; keep the selected image and +1 in the LRU for stills over 25 MP;
   use a camera JPEG's embedded MPF preview for its first pixel.
6. **Owner's call:** a half-size RAW develop for the fit view, replaced by the full one on zoom.
   Faster, but it changes the "full quality at fit" behaviour.

### Video

1. **Measure first.** The 4K clip has never been through `--soak` or `--av-soak`; the 22 fps comes
   from the screen-grab script, which resizes the window while it plays. Ruled out by reading:
   software decode, GPU-to-CPU readback, CPU colour conversion.
2. **Deepen the frame ring.** 4 slots on Windows, 6 on the Mac (`src/player/video_internal.h:187`)
   ✔, about 2 queued frames after the on-screen and pending ones, so a 66 ms decoder hiccup shows.
3. Raise the decode threads' priority; stop creating a 4K texture per frame in the Windows software
   fallback; stop waiting for the GPU and flushing the texture cache per frame on the Mac.
4. Structural, only if the soak points there: decode on its own D3D11 device so it never takes the
   render device's lock.

### Build and everything else

- **No LTO/IPO and no CPU baseline flags** anywhere (✔); the core is many static libraries, so hot
  paths cannot inline across them. Turn on for shipped targets, measure open, navigate and export.
- **Export and print bake on one thread** (`src/edit/bake.cpp:146`); OCR, print and resized export
  decode full size when a scaled decode exists.
- **iPhone search:** a fixed 90 ms debounce (`AppModel.swift:1092`) ✔ is most of the floor; then
  per-hit building, two thread hops and a spring animation over the whole grid.
- **Local search:** every model stays loaded for the session after indexing goes idle (PR 693 does
  not cover this); signature checks and pack hashing run at startup.
- **Photos backup:** uses the sequential copy profile on a network share, calls
  `assetResourcesForAsset` per asset on every run (about 15 minutes for 24k assets) and reads each
  file three times to hash it.
- **Chrome redraw storms:** Video Editor edge drag rebuilds the timeline per pointer move; Adjust
  slider ticks rebuild the curve and histogram; Mac add-on thumbnail grids redraw every cell per
  arriving thumbnail; `EnhanceRun` republishes on any add-on's screen change.
- **CI time:** `ctest -j`, drop the plain MSVC Debug leg (ASan covers it), stop building each Swift
  package twice, add a compiler cache.

## 3. Docs

Fixed with this audit: CLAUDE.md (platforms, layout, build line, policy scripts, plans), the design
index (rows 25, 28, 29, platforms, built features that live only in plans), `02` module layout and
include table, `09` CI table, `10`, `15`, `16`, `27`, `28` stale status lines, seven README
statements, `DEVELOPMENT.md:153`, the settings checklist.

Still to do ([docs.md](docs.md) has each with proposed text):

- About 20 of the 34 plans are wholly or mostly built and should be folded into `docs/design`. The
  iPhone app, phone access and self-hosting, remote folders and the VPN have no design doc.
- About 20 items of issue 621 are still true.
- README: VPN and Places are not mentioned; "Search from Final Cut Pro" and "More from add-ons" sit
  under Coming next though largely built; `;` (Live Photo motion) is Windows only.
- `DEVELOPMENT.md`: stale Layout section and an orphan iPhone section at the end.
- Design docs that disagree with the code on prefetch hand-off (`02`), ring headroom (`05`) and
  five more video statements ([video.md](video.md), [stills.md](stills.md)).

## 4. Work packages

Sized so each touches its own files and can land as its own draft PR.

| # | Package | Main files | Covers |
|---|---|---|---|
| 1 | File safety | `src/io/replace_*`, `src/shell/file_transfer.cpp`, `edit_session.cpp`, `photos_backup.cpp` | D1–D8 |
| 2 | Stills speed | `src/abi/abi.cpp`, `src/image/upload*`, launch order in `main.cpp` | Stills 1, 2, 4; B1 |
| 3 | Video | `src/player/*` | B2, B3; ring depth; soak the 4K clip |
| 4 | Windows chrome | `src.managed/*` | S1, B7–B10, U4, redraw storms |
| 5 | Mac chrome | `src.swift/*` | B5, B6, U3, accessibility |
| 6 | iPhone app | `src.ios/*` | B14, B15, S8, U5, debounce |
| 7 | CI and build | `.github/workflows`, `CMakeLists.txt`, `cmake/`, `tools/check-module-graph.ps1` | iOS in CI, `ctest -j`, LTO, `/guard:cf`, module graph |
| 8 | Add-ons | `src/addon`, `src/addons/ai`, `src/addons/driftwire` | B13, model unload, S2–S4 |
| 9 | Docs | `docs/plans`, `docs/design` | Fold built plans; issue 621 |

Windows-only speed changes (stills 1, the 4K clip) cannot be measured on a Mac: they need a run of
`python tools/perf/regenerate.py` on the Windows PC before and after.

## Not covered

- No app was run. Layout, light/dark and animation problems that only show on screen are not here.
- Mac chrome: `AdjustView`, `PersonSheet`, `ManagementView`, `SearchPanel` and most of `ScreenView`
  were pattern-searched, not read.
- Windows chrome: `FakeInput`, `Look`, `FaceTile`, `AiChrome`, `Transfer` helpers; native struct
  offsets were not checked against their headers.
- iPhone: `LinkConnection.swift` 620–1040, the rest of `LinkFiles`, server code past code expiry
  and VPN authentication.
- Hosts: C# and Swift calls were matched to the ABI by name, not by argument types.
