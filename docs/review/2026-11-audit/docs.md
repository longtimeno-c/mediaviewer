# Documentation drift audit (read-only), 2026-11-25

Worktree `relaxed-mclean-5d04bd`, branch `claude/project-audit-performance-180749` (= main a232bda4).
Format: **location → what is wrong → evidence → proposed text**. C = confirmed, S = suspected.

Side finding (not docs): the last completed `ci.yml` run on main (45617024818) failed at
"Windows dependencies" and skipped every other job.

---

## 1. CLAUDE.md

### 1.1 Stack line: iOS app missing; Linux status vague (C)
- **Location:** CLAUDE.md:3, :5-7
- **Wrong:** The only platforms named are Windows and macOS, plus Linux "in progress". The iPhone/iPad app
  (`src.ios`, linking `src/mobile` and the AI/Driftwire engines) is not mentioned. On Linux, two things
  exist today: the present lab (Linux PR 1) and the shipped `driftwire` CLI. "Qt/QML planned" and "EGL"
  are accurate.
- **Evidence:** `src.ios/App`, `src.ios/MobilePackage`; CMakeLists.txt:37,88 (iOS core);
  `src/shell/main_linux.cpp`, `src/gfx/device_gl.cpp`; README.md:46 (Linux driftwire install);
  docs/plans/linux.md:9-21.
- **Proposed:**
  - Line 3: `A fast photo + video viewer for camera dumps. Windows and macOS, one C++ core; an iPhone/iPad app (`src.ios`) searches the same library. Linux: the present lab and the `driftwire` CLI so far.`
  - Line 6: replace `SwiftUI (`src.swift`), Qt/QML planned for Linux` with `SwiftUI (`src.swift`, `src.ios`), Qt/QML planned for Linux`.

### 1.2 Layout block omits 9 of 21 `src/` modules and 3 top-level dirs (C)
- **Location:** CLAUDE.md:14-20
- **Wrong:** These are missing: `addon`, `catalogue`, `geo`, `infer`, `mobile`, `nle`, `remote`,
  `shellext`, `wire`, `examples/`, `packaging/` and `third_party/`. `src.ios` is listed as "chrome", but it
  is a separate app.
- **Evidence:** `ls src` (abi addon addons canvas catalogue codec core edit geo gfx image infer io meta mobile nle player remote shell shellext wire).
- **Proposed** (replace the code block body):
```
src/core src/io src/codec src/gfx   low-level: jobs, I/O, decoders, GPU backends (D3D11/Metal/GL)
src/image src/player src/meta src/edit src/canvas   features
src/addon  add-on host        src/geo  reverse geocoder   src/infer  ONNX Runtime (AI pack)
src/wire src/catalogue src/mobile src/remote   phone/remote protocol, catalogue, phone core, remote folders
src/nle    Final Cut Pro search   src/shellext  Explorer thumbnails / Quick Look
src/abi    C ABI the hosts call       src/shell  native hosts (Win32, *_mac.mm, *_linux.cpp)
src/addons add-ons (talk to the core only via mediaviewer_addon.h)
src.managed / src.swift   desktop chrome     src.ios  iPhone/iPad app
tests/ tools/ docs/ examples/ packaging/ third_party/
```

### 1.3 Dependency claim and module-graph enforcement (C)
- **Location:** CLAUDE.md:22-23
- **Wrong:** The chain `shell → abi → features → codec/gfx/io → core` is a simplification, which is fine
  for CLAUDE.md. The problem is "`tools/check-module-graph.ps1` enforces it": the script does not cover
  `src/mobile` or `src/remote`, and it still has a rule for a `phone` module that no longer exists
  (`src/phone` moved to `src/addons/phone` in 0fcf1489, #686).
- **Evidence:**
  - The `phone` rule: tools/check-module-graph.ps1:77, and `phone` in the abi/shell lists at :79 and :84.
  - Missing folders are skipped (`if (-not (Test-Path $moduleDir)) { continue }`, :111). Includes of
    modules not in the map are ignored (`if (-not $allowed.Contains($target)) { continue }`, :126).
  - So these edges go unchecked: `src/mobile/catalogue.cpp:15` includes `addons/ai/catalog.h`;
    `src/mobile/vpn_abi.cpp:19` includes `addons/vpn/vpn.h`; `src/mobile/files.cpp:9` includes `meta/meta.h`;
    `src/abi/remote_abi.cpp:27` includes `remote/key.h`; `src/shell/photos_backup_server.cpp:10` includes `mobile/link.h`.
- **Proposed:** keep the CLAUDE.md sentence. Fix the script: drop `phone` and add rules for `mobile` and
  `remote`. Until then, reword CLAUDE.md:23 to:
  `` `tools/check-module-graph.ps1` enforces it (its map lists each module's allowed includes; add new modules to it). ``

### 1.4 Build commands (S)
- **Location:** CLAUDE.md:28 (and README.md:382, docs/DEVELOPMENT.md:25)
- **Wrong:** `-G "Visual Studio 17 2022"` fails on a machine that has only VS 2026. CI deliberately
  passes no `-G`.
- **Evidence:** ci.yml:734-737 ("No -G: CMake picks the newest Visual Studio generator"); commit 4f30865b
  (VS 2026 images).
- **Proposed:** `cmake -S . -B build -A x64` (no `-G`). Optionally add the updater tests:
  `dotnet test src.managed\MediaViewer.Updater.Tests\...` (the project exists).

### 1.5 Policy scripts (C)
- **Location:** CLAUDE.md:43,52,55,58 name `check-module-graph`, `licence-check`, `check-hostable-core` and
  `check-winui-controls`. All four exist.
- **Missing:** `tools/check-vcpkg-baseline.ps1` and `tools/telemetry-schema-check.ps1`, which CI's
  `policy` job also runs (ci.yml:58,67).
- **Proposed:** after line 26 ("No user paths…") add `` (`tools/telemetry-schema-check.ps1`) ``. In "CI stays
  green" (line 44) add: `The policy job runs every tools/check-*.ps1, licence-check.ps1 and telemetry-schema-check.ps1.`

### 1.6 Docs section: plans (C)
- **Location:** CLAUDE.md:68 `docs/plans/ — ideas not built yet.`
- **Wrong:** At least 20 of the 34 plans are wholly or mostly built (see §5).
- **Proposed:** `` `docs/plans/` — forward plans. When a slice lands, move what it built into `docs/design/` and strike it from the plan. ``

### 1.7 Issue 621's CLAUDE.md items: fixed (C)
Today's CLAUDE.md contains no "Not scaffolded until PR 1", no "Planned layout" and no rule-3 ladder.

---

## 2. docs/design

### 2.1 README.md index
- **design/README.md:44: Enhance row stale (C).** "photos and a clip's picture (not yet)". Both are built:
  29-enhance.md:121 "Enhance image: a photo restored", :189 "In the Video Editor"; docs/plans/enhance.md:29-34
  (still and clip picture built 2026-10-26); `src/addons/enhance/picture_job.cpp`.
  **Proposed:** `| [29-enhance.md](29-enhance.md) | Enhance: a restored copy of a photo, a clip (picture and sound) or an audio file from a model downloaded on request; its sheet, the Enhance tab in Edit, Remove, Restore faces, Extend |`
- **design/README.md:41: open add-ons row stale (C).** It says "settings, keymaps, screens, slots planned",
  but all four are built:
  - 10-roadmap.md:650-653: settings pages, keymap packs and theme shape built 2026-10-11.
  - Enhance is a "slot add-on" with view-tree screens (29-enhance.md:13-14).
  **Proposed:** `| [25-open-addons.md](25-open-addons.md) | Open add-ons: packages anyone can make and install from a file or a link, signed by their publisher; themes, keymaps, settings pages, Lua script commands, screens and slots |`
- **design/README.md:44-45: order (C).** The 29 row comes before 28. Swap them.
- **design/README.md:45: Driftwire row too narrow (S).** 28 now also covers the NAS service (`driftwire
  serve`/`host`), the Tunnel/VPN and relays (28-driftwire.md:609, 940-964). **Proposed:** append `; the NAS service, the Tunnel and relays`.
- **design/README.md:31: platforms row (C).** It says "Windows and macOS hosts", but 15 has a Linux section
  (15-platforms.md:133). **Proposed:** `Windows and macOS hosts over one core; the Linux host so far`.
- **Missing subsystems (C).** No design doc covers several built features:
  - the iPhone app (`src.ios`, `src/mobile`, TestFlight build 16)
  - phone access / self-hosting (`src/addons/phone`)
  - remote folders on another desktop (`src/remote`, `IslandHost.Remote.cs`)
  - the VPN add-on (`src/addons/vpn`)
  - the phone VPN (`src.ios/App/DriftwireTunnel`)

  These live only in plans (mobile.md, self-hosted.md, desktop-client.md, vpn-addon.md, phone-vpn.md).
  **Proposed:** at minimum, add after line 8: `Built features that still live in plans: the iPhone app ([mobile](../plans/mobile.md)), phone access and self-hosting ([phone-access-addon](../plans/phone-access-addon.md), [self-hosted](../plans/self-hosted.md)), another computer's folders ([desktop-client](../plans/desktop-client.md)) and the VPN ([vpn-addon](../plans/vpn-addon.md), [phone-vpn](../plans/phone-vpn.md)).`
- **design/README.md:8 (C).** "Forward plans that are not built yet live in ../plans/". Same fix as 1.6.

### 2.2 02-architecture.md
- **:7-40: module layout missing modules (C).**
  - Native modules missing: `wire/`, `catalogue/`, `mobile/`, `remote/`.
  - `addons/` lists only "(import, ai)". Actual: ai, driftwire, enhance, import, phone, places, vpn.
  - `gfx/` names D3D11 and Metal only; GL/Wayland exists (`device_gl.cpp`, `gl_surface_wayland.cpp`).
  - `shell/` omits the Linux host (`main_linux.cpp`, `wayland_window_linux.cpp`).
  - Missing app/project dirs: `src.ios/`, `src.managed/MediaViewer.AbiSmokeTest`, `MediaViewer.Updater.Tests`,
    `Shared`, `MediaViewer.Chrome.Tests`.

  **Proposed** additions to the block:
  ```
    wire/        Driftwire's protocol, shared by the add-on, the app's phone feature and the phone
    catalogue/   the catalogue a phone reads (keys, current.json, delivery)
    mobile/      the phone's core: catalogue reader, search, link, sync, backup (src.ios links it; the Mac app too)
    remote/      another computer's folders over the phone link (`remote:` keys, range reads, cache)
  ```
  Also: `addons/  add-ons (ai, driftwire, enhance, import, phone, places, vpn) — reach the core only through the host function table (exception: abi includes addons/phone/host_tray.h)`;
  `gfx/ … and the Metal twins (*_metal.mm) and the OpenGL/Wayland backend (*_gl, *_wayland)`;
  `src.ios/  the iPhone/iPad app (SwiftUI; MobilePackage, App)`.
- **:46-60: include table (C).**
  - Missing rows for `wire` (wire, io, core) and `catalogue` (catalogue, wire, io, core).
  - `addons` row omits `wire`, `catalogue`, `io` (script :78).
  - `abi` row omits the `addons/phone/host_tray.h` exception (script :94-96).
  - `mobile` and `remote` have no rule at all (see 1.3).
- **:54: `fold.cpp` does not exist (C).** `src/geo` has `fold.h` only (ls src/geo). Same error at 27-places.md:26.
  Issue 621 item, still open. **Proposed:** `` `geo.cpp` / `fold.h` ``.

### 2.3 09-build-and-test.md
- **:198-216: CI job table misses jobs (C).** `ios-core` (ci.yml:205, matrix iphoneos/iphonesimulator) and
  `test-media` (ci.yml:579) are absent.
  **Proposed rows:**
  `| `ios-core` | macos | The iPhone engines (cmake/ios.cmake) for iphoneos and iphonesimulator; `tools/ios/run-simulator-smoke.sh` |`
  `| `test-media` | ubuntu | Generates the clip corpus and fetches the RAW/HEIF samples once, cached for every Build leg |`
- **:341 (C):** "tabs / multi-window" not built, but Ctrl+N windows exist (src/shell/main.cpp:10622
  `open_new_window`, command_table.cpp:72). Issue 621 item, still open. **Proposed:** `Explorer property handler; shell "Edit" verb; tabs (Ctrl+N windows are separate processes).`
- **:94 (S):** "Not built yet: a UI smoke test per platform (UIA, XCUITest)". The iPhone app has XCUITests
  (`src.ios/UITests/*.swift`). The desktop statement holds. **Proposed:** add `(the iPhone app has XCUITests in src.ios/UITests)`.
- **:228-232 targets vs measurements:** see §4.3.

### 2.4 10-roadmap.md
- **:645-653 (C):** "Not built: PRs 57–60 … a sandboxed script runtime (Lua) …". PR 57 is built: its own
  section at :599-616, `third_party/lua`, `src/addon/script_*`. The PR 56 halves named inside this bullet
  are built too. Issue 621 item, still open.
  **Proposed:** `- **PRs 58–60** — the rest of the open add-ons ([25](25-open-addons.md)) beyond what is built (script commands, PR 57; settings pages, keymap packs, themes, slots and screens): search providers and files for other makers' add-ons.` (Check 25 §17 for the exact remainder.)
- **:655 (C):** "Windows grouped as tabs" duplicates 16-commands.md:614. Issue 621, still open.
- **:113 (C):** PR 5c lists "scrub-preview thumbnails" as delivered. 05-video-pipeline.md:201 says not
  built, and no code exists (grep). Issue 621, still open. **Proposed:** drop `, scrub-preview thumbnails`.
- **:77, :217, :567 (C):** thumbnail spec `jpg512.1` / `jpg512.3`. Code: `jpg512.4` (src/image/thumb.h:30).
  These are historical PR entries; append `(now jpg512.4)` at :77.
- **:78 (C):** "five-slot GPU LRU". Code: a byte budget of 512 MB, 3–12 entries (src/abi/abi.cpp:730-732).
  Also at 16-commands.md:190 and :520. Issue 621, still open.
  **Proposed:** `a byte-budgeted GPU LRU (512 MB, 3–12 entries)`.
- **:274 (C):** "the CI job has not run yet" (linux-lab-smoke). It has run and passed: runs on
  `claude/windows-deps-cache-key` and `claude/phone-vpn-settings` show "Linux present lab (headless Weston
  smoke)=success". **Proposed:** `(the linux-lab-smoke CI job runs it under headless Weston)`.
- **Missing sections (S):** the roadmap stops at Driftwire (:618). Built features with no entry: Places,
  Enhance, phone access / self-hosting, the iPhone app, remote folders, the VPN, Photos backup's check-library.

### 2.5 15-platforms.md
- **:141 (C):** "the `linux-lab-smoke` CI job is written but has not run yet". It has run and passed (see
  2.4). **Proposed:** `Built; smoke-tested under headless Weston locally and in CI (linux-lab-smoke); its gate is owed on a real session`.
- **:1-60 (C):** no mention of the iPhone app, a third consumer of the core (`cmake/ios.cmake`,
  CMakeLists.txt:88 "The iPhone core … engines only, no viewer"). **Proposed:** after :59 add
  `The iPhone/iPad app (src.ios) is not a host of the viewer: it links the search and Driftwire engines and src/mobile statically (cmake/ios.cmake, docs/plans/mobile.md).`

### 2.6 16-commands.md
I checked the Mac exclusions and the Not built list against `src/shell/command_coverage.inc` and
`MvCommandSupported` (main_mac.mm:196).
- **:146, :159, :192, :620-621: Mac exclusions are accurate (C).** coverage.inc:81 (go_to), :91
  (play_motion), :92-93 (open_raw, open_jpeg) are the only non-retired Mac exclusions.
- **:614: correct (C).** "Tabs, or several windows in one process": Ctrl+N starts a new process
  (main.cpp:10619-10621; main_mac.mm:239-241).
- **:615 (S):** "`Ctrl+PageUp`/`PageDown` for HEIC sequences" is not built. This is consistent with :133
  (HEIC top-level images are pages, sequences stay animations). The ICO half of issue 621 is fixed.
- **:190, :520 (C):** "five-slot GPU LRU". Fix as in 2.4.
- **:498 vs other docs (C):** 16 says "No command palette", and the `palette` id is retired
  (commands.h:224). Yet a palette is still cited in:
  - 29-enhance.md:19 ("`Enhance…` in palette"), :260, :303
  - 04-image-pipeline.md:233 ("palette commands"; issue 621, still open)
  - README.md:325 ("the command palette opens Enhance")
  - src/shell/commands.h:6 (comment)

  **Proposed:** replace "palette" with "Settings → Keyboard shortcuts list" or "context menu". For
  README.md:325: `- **One key away.** `Ctrl+Shift+R` (`⌘⇧R` on a Mac) or the right-click menu opens Enhance on the file in front of you.`
- **Issue 621's duplicates (U/X, filter, touch) (C):** fixed (no longer in Not built).

### 2.7 19-voice.md (C, accurate)
"Not built" is correct. There is no `src/addons/voice`, and no `SFSpeechRecognizer` or `mv_voice` anywhere
(grep). README lists Voice under "Coming next" (README.md:369), which is consistent.

### 2.8 22-editor-addon.md (C, accurate)
"None of it is built" is correct: no `src/addons/editor`, no `.mvproj`. :36 "Spikes S2–S8" is still
undefined (issue 621, still open).

### 2.9 29-enhance.md
- **:19 (S):** "the sheet … audio files and clips only". This is consistent with :67-71 (photos go through
  the Edit tab's cards, :21). Only the palette word is wrong (2.6).
- **Body vs index:** the body is current. Only the README index row is stale (2.1).

### 2.10 28-driftwire.md
- **:942-943 (C):** "**Phones do not use it yet**: the iPhone app has no VPN ([phone-vpn plan], not built)".
  The phone VPN is built and on TestFlight: `src.ios/App/DriftwireTunnel/PacketTunnelProvider.swift`;
  commits d4a94c4e ("iOS: build 16 … VPN auto-connect"), 7401cd5f (#752 phone-vpn-auto-connect);
  phone-vpn.md:3. **Proposed:** `Phones use it through the iPhone app's built-in VPN ([phone-vpn](../plans/phone-vpn.md)); the pairing code carries no relay route for a host, so …` (keep the rest).

### 2.11 Other open items from issue 621 (checked today)
| Issue 621 item | Today | Evidence |
|---|---|---|
| 17 Not built lists D2–D4 document search | **Fixed** | 17 "Not built" no longer has them |
| 17 "Place names in the query language" | Reworded to "Place names in the index" (S, fine) | 17 Not built list |
| 27 AI fold is Latin-1 | **Fixed** | no "Latin-1" in 27 |
| 27:39 `fold.cpp` | **Still true** (now 27-places.md:26, and 02:54) | `ls src/geo` |
| 06 `meta::read_location` as provider read | **Still true** (06-metadata.md:88) | production uses `meta::cached_or_read` (src/meta/meta_cache.cpp:374; addons_mac.mm:370,2148) |
| 18 Mac finish notification is a beep | **Still true** (18-import.md:302) | `UNUserNotificationCenter` at src/shell/addons_mac.mm:916 |
| 09 / single_instance_win.h multi-window | **Still true** (09:341; src/shell/single_instance_win.h:7) | main.cpp:10622 |
| 14 ABI history 0.8→0.10 | **Moot** | 14:31-32 now points at the comment in mediaviewer.h |
| 04:233 palette commands | **Still true** | commands.h:224 |
| 03:197 optional pixel grid | **Still true** | `pixel_grid` defaults true (src/gfx/blit.h:43-44), no assignment anywhere sets it false |
| 16 five-slot LRU | **Still true** (16:190, :520; 10:78) | abi.cpp:730-732 |
| 10 PR 57–60 Lua not built | **Still true** (10:645) | 10:599-616 |
| 10 tabs duplicate | **Still true** (10:655 vs 16:614) | |
| 10 PR 5c scrub preview | **Still true** (10:113 vs 05:201) | |
| 10 `jpg512.3` | **Still true** (10:567; also .1 at :77, :217) | thumb.h:30 |
| plans/audio-and-documents.md:8-17 "on review branches … retire this file" | **Still true** | file unchanged |
| plans/audio-search.md:13 "A1 is being built" | **Still true** | |
| plans/metadata-search-and-place.md:22 "#291 open with no PR"; :74 geo under meta | **Still true** | |
| plans/enhance.md "Left:" paragraph | **Still true** (enhance.md:36-39 keeps slice-5 text) | |
| 21:251 "S2–S5", 22:36 "S2–S8" | **Still true** | |
| 05:28-31 M4B | **Still true** (05:30) | |
| 05:175-177 half-frame bias | Not rechecked (S) | |
| src/image/pipeline_mac.h:3-6 "JPEG/PNG/BMP only" | **Still true** | |
| src/shell/commands.h pages "TIFF, PDF, DOCX" | **Still true** (now :328) | |
| src/shell/crash_reporter_win.h:10-12 "PR 15 owns the upload path" | **Still true** | |
| UpdateManifest.cs "Prerelease tags are not a v1 channel" | **Still true** (now :12) | |
| tools/clipjob/main.cpp:8-10 | Reworded (S, not rechecked in depth) | |
| CLAUDE.md items | **Fixed** | |

**Summary of 621:** about 6 items are fixed or moot; about 20 are still true. Leave the issue open, or
close it with the edits above.

---

## 3. docs/DEVELOPMENT.md (2,561 lines)

**Path check:** a script resolved every `tools/…`, `src/…`, `cmake/…`, `tests/…` and `src.*` path
(forward and backslash). None is dead. The only misses are gitignored or generated files
(`tools/testmedia/raw/*`, `*.mp4`, `bin/Release/...`, `src.ios/Config/Signing.local.xcconfig`) and
shorthand like `docs/design/12`. Every script in "policy gates" (:1448-1456) exists.

- **:1-4 (C):** three lines of preamble come before the `#` title ("Build, run and test recipes …" at
  :1-2, then `# MediaViewer: development notes` at :4). **Proposed:** move :1-2 below :4.
- **:151 (C):** "The chrome in `src.ios` will link these when it lands." It has landed: `src.ios/App`, the
  `MediaViewer` scheme, :463-549. **Proposed:** `The iPhone app in src.ios links these (see "iPhone and iPad interface" below).`
- **:2487-2542 Layout (C):** misses `player`, `infer`, `nle`, `shellext`, `wire`, `catalogue`, `mobile`,
  `remote`, `src/addons/*` (only `driftwire/cli`), `src.swift/` and `src.ios/`. Its `src/gfx` line says
  D3D11 only.
  - :2538 has the same stale dependency chain as CLAUDE.md.
  - :2541 says "forward plans not built yet".

  **Proposed:** replace the block with a pointer, `See docs/design/02-architecture.md "Module layout".`,
  or add the missing rows from 2.2.
- **:2549-2561 (C):** an orphan "The iPhone app's link" section sits after "Design reference" and
  duplicates part of :463 and :673. Its line :2555 says "until the desktop claim lands", but the desktop
  claim is built (Phone access card; DEVELOPMENT.md:398 section; plans/phone-in-the-app.md:124 "As built").
  **Proposed:** fold it into the :463 section and drop "until the desktop claim lands".
- **iOS and Mac sections exist and match:**
  - Mac: :52 "macOS (Mac PRs 1–6)", :231 MediaViewer.app, :251 runbook.
  - iOS: :149 engines, :463 interface (`src.ios/MobilePackage`, `App/MediaViewer`, `MediaViewerFixtures`,
    all present), :673 phone library, :744 sample library.
  - Commands match CI (ci.yml:249 `run-simulator-smoke.sh … mv_ios_core_smoke`).
  - Minor: :149 builds into `build-ios-sim`, CI uses `build-ios` (S).
- **:25 (S):** `-G "Visual Studio 17 2022"`. See 1.4.

---

## 4. README.md

### 4.1 Feature claims vs code
- **README.md:150 (C):** "and so does the canvas surround unless you pick Grey, White, Checkerboard or
  Dark." That setting was removed (12-decision-log.md:3272-3276, issue #408: "Settings' 'Canvas
  background' row (System, Grey, White, Checkerboard, Dark), `B` and `cycle_background` are gone"). No
  "checker" string exists in any chrome.
  **Proposed:** `- **Light or dark.** Follows the system appearance on Windows and macOS, or the theme you pick; the space around your photos and videos follows it too.`
- **README.md:86 vs :67 (C):**
  - :86 says "No present was dropped, and no video frame was skipped."
  - :67 says "the selector skipped 1 video frame". The report agrees with :67:
    `docs/perf/investigation/video-native-60s.json.video.json` "dropped": 1.

  **Proposed (:86):** `**On screen.** 60 s of 1080p HEVC. Frame time sits on the refresh. No present was dropped; the selector skipped one video frame.`
- **README.md:25 vs :290 (C):** :25 says the server is "The Mac you already have (a Windows PC and a
  Synology NAS next)". :290 says it "hosts on a Mac and on a Windows PC", and Windows hosting has run.
  **Proposed (:25):** `| **Your own server** | The Mac or Windows PC you already have (a Synology NAS next): one guided setup shares your folders with your phones and your other computers, encrypted end to end |`
- **README.md:285 (C):** "On a Mac, **File → Connect to a Server…**". Windows has it too
  (`src.managed/MediaViewer.Chrome/IslandHost.Remote.cs:15` "Connect to a server…"), and the same line
  quotes Windows timings. **Proposed:** `- **Your other computers too.** **Connect to a Server…** (File menu on a Mac, the folder tree on Windows) pins the server's folders …`
- **README.md:325 (C):** "the command palette". See 2.6.
- **README.md:367 (S):** "Search from Final Cut Pro" is under **Coming next**, but 23-nle-search.md:12-16
  says it ships inside MediaViewer.app, off until turned on. Confirm with the owner whether it is
  released. If so, move it into "Find anything" as
  `- **From Final Cut Pro (Mac).** Turn on Final Cut Pro search under Local search and a MediaViewer panel inside Final Cut Pro searches your index; drag the moments into an event or the timeline.`
- **README.md:368 (S):** "More from add-ons … settings pages, commands, keymaps and whole screens, as
  sandboxed scripts" under Coming next. README:333-334 already says themes, key layouts, settings pages
  and script commands are built (10-roadmap.md:599-653). **Proposed:** `| **More from add-ons** | Search providers and file access for other makers' add-ons, still sandboxed and with no network access ([docs/design/25](docs/design/25-open-addons.md)) |`
- **README.md:167 (S):** "`;` plays a Live Photo's motion" with no platform note. Windows only
  (command_coverage.inc:91). **Proposed:** append ` (Windows)`.
- **Built but not in README (C):**
  - **VPN / Tunnel.** `src/addons/vpn`; the phone's built-in VPN (`src.ios/App/DriftwireTunnel`, TestFlight
    build 16); the Driftwire Tunnel (28-driftwire.md:940-964). The words "VPN" and "Tunnel" never appear
    in README.
  - **Places.** Offline place names and a world map (`src/addons/places`, `world_map.h`;
    27-places.md:197). Not mentioned.
  - **Enhance tools in Edit:** Remove, Restore faces, Extend, Restyle (29-enhance.md:274-351). Only
    "more ways to restore them in Edit" is mentioned (README:323).
  - **Photos backup check-library** (plans/check-library.md:83 "built"). Possibly covered by README:254
    "only copies what is new" (S).

### 4.2 Performance numbers vs docs/perf
All Windows numbers come from `docs/perf/*` (machine.json: 2026-11-18, commit 9bff6aae). Mac numbers come
from `docs/perf/mac/*` (2026-11-18, e68e9d73).

| README | Claim | Report value | Match |
|---|---|---|---|
| :19, :63 | RAW preview 47–89 ms Windows; one Nikon 134 ms | first-pixel/*.json + investigation/first-pixel-repeat-{2,3}: ARW 60/63/47, CR2 65/68/65, CR3 56/50/54, NEF **134**/85/89, DNG 67/64/67 ms | ✔ |
| :63 | HEIC 32 ms | first-pixel/libheif-example.heic.json 0.032 s (repeats 33, 33) | ✔ |
| :19, :105 | Mac 28–50 ms | mac/first-pixel: ARW 28, CR3 29, DNG 33, CR2 38, NEF 50 | ✔ |
| :105 | Mac full 0.26–0.49 s | 0.261–0.494 s | ✔ |
| :105 | Mac HEIC 6 ms then 55 ms | 0.006 / 0.055 s | ✔ |
| :19, :65 | next photo 0.2 ms ("under 0.2 ms" at :19) | browse.json warm present_ms 0.173–0.202; ready_ms 0.039–0.074 | ✔ at :65; :19 "under 0.2 ms" is true only for ready_ms. Suggest "in 0.2 ms" (S) |
| :66 | jump 45–68 ms | browse.json cold present_ms 45.34, 67.86 | ✔ |
| :62, :72 | 3,597 frames, p99 17.10 ms, 16.68 ms refresh, 0 dropped | pan-soak-42mp-arw.json and frametime-animated.json: 3597, 17.1, 16.680, 0 | ✔ |
| :64 | idle 0.03 % CPU, 0 presents | frametime-idle.json idle_cpu_percent 0.026, idle_presents 0 | ✔ |
| :67 | 1080p HEVC 3,597 presents, p99 17.25, 0 dropped, selector skipped 1 | investigation/video-native-60s.json 3597 / 17.25 / 0; .video.json dropped 1 | ✔ (but :86 contradicts, see 4.1) |
| :67, :90 | A/V p99 2.2 ms over 120 s; none of 3,600 discarded | investigation/av-after-120s.csv last row: error_p99 2.229, presented 3600, dropped 0 | ✔. **But** top-level `docs/perf/av-drift-120s.csv` is the old pre-fix trace (dropped 135, p99 16.08). investigation/README.md:89 still says "2 discarded / 3,597", which is stale vs the csv. Delete or rename the top-level csv; update investigation/README.md:89 (C) |
| :68 | 0.6 s to window, 0.3 s to photo | boot/windows-empty.json interactive median 566 ms; boot/windows-arw.json first_pixel median 322.9 ms | ✔ |
| :94 | full RAW decode 0.6–2.0 s | first-pixel full 0.557–1.98 s (all three runs) | ✔ (rounded) |
| :94 | one of three NEF opens dropped a frame | repeat-3 nikon: dropped 1, refine_fade_dropped 1 | ✔ |
| :94, :130 | 4K HEVC ~22 fps, worst gap 636 ms | compare/screen.json play hevc_4k mv fps 22.1, **p99** 635.8 | ✔ fps; "worst gap" is actually p99. Say "p99 gap 636 ms" (S) |
| :103 | Mac 3,599 frames, p99 16.85, 16.67 refresh, 0.18 ms CPU | mac/frametime-animated.json 3599, 16.85, 16.667, cpu_mean 0.180 | ✔ |
| :104 | Mac idle 0 presents, 0.6 % CPU | mac/frametime-idle.json 0, 0.635 | ✔ |
| :106 | Mac arrow under 1.1 ms; jump 53–64 ms | mac/browse.json warm present 0.229–1.033; cold 53.48, 64.48 | ✔ |
| :107 | Mac 1 dropped present in 3,595, p99 16.9, worst 83.5 ms, 5 skipped, A/V p99 11.0 | mac/investigation/video-native-60s.json 3595 / dropped 1 / 16.9 / max 83.51; .video.json dropped 5, error_p99 10.95 | ✔ |
| :108 | Mac list 3,000 in 1.7 ms, sort 0.2 ms | mac/bench.json 1.702, 0.223 | ✔ |
| :122 | Sony 1,012 vs 976; Nikon 754 vs 957; HEIC 373 vs 783 | compare/screen.json means: mv ARW 1011.8 / Photos 976.1; NEF Photos 754.1 / mv 956.7; HEIC mv 373.0 / Photos 783.0 | ✔ |
| :126 | pan p50 16.7 ms, slowest 20–85 ms | screen.json pan mv p50 16.8/16.7/16.7, p99 51.2/85.2/20 | ✔ |
| :130 | 27–28 fps both; p99 167 ms each | mv 27.4 fps p99 167; wmp 27.6 fps p99 165.7 | ≈. Say "p99 166–167 ms" (S) |
| :232-234 | search accuracy, 25,000-photo library | search.json towers at 24992, Apple M5, **2026-09-28**, commit 94f195c | ✔ size. Note: README:58 says every number below is from the Ryzen on 2026-11-18; search.json is from a Mac in September (S: add "measured on an M5 Mac, 2026-09-28") |
| :245 | 519 of the first 600 suggestions right, none wrong | **no report in repo** (grep of docs, tools, tests) | unverifiable (S) |
| :268 | Mac conversion 5.6–7.4× real time | only in docs/plans/phone-clip-convert.md:104; no docs/perf report | unverifiable from docs/perf (S) |
| :268 | Windows NVENC first frame 0.6–0.75 s, seek 0.4–0.5 s | windows-convert.jsonl (AVI/MKV): first_frame 568, 584, 633, 752 ms; seek 360, 367, 521, 528 ms | first frame ≈ (0.57–0.75); **seek is 0.36–0.53 s, not 0.4–0.5**. Proposed: `the first frame of a 1080p AVI or MKV in 0.57–0.75 s and a seek in 0.36–0.53 s` (C) |
| :273 | Simulator 128–207 / 110–271 ms; search 4.2 ms | **no report in repo** | unverifiable (S) |
| :285 | Mac loopback 79–88 / 139–155 / 56–60 (78–87) ms | only docs/plans/desktop-client.md:233-236; no docs/perf file | values match the plan, but README:57 promises raw reports in docs/perf. Add `docs/perf/mac/desktop-client.json` (S) |
| :285 | Windows 131–143 / 217–229 / 76–78 (93–96) ms | windows-desktop-client.json: 131.0–142.8 / 216.7–229.1 / 76.5–77.7 / 92.8–95.9 | ✔ |

### 4.3 Where MediaViewer is slower than, or only equal to, a native app or its own target
Targets from 09-build-and-test.md:228-237.

1. **Windows Photos opens the Nikon NEF faster:** 754 ms vs 957 ms (compare/screen.json; README:122).
2. **Sony ARW cold open is a tie or slightly slower:** MediaViewer 1,012 ms vs Photos 976 ms. Photos won
   rep 1 by 194 ms (741.9 vs 935.8).
3. **30 fps clip playback only equals Media Player:** 27.4 vs 27.6 fps, p99 167 vs 165.7 ms. Both are
   below the clip's 30 fps.
4. **Cold-launch first pixel vs the 09 target "< 400 ms with the WinUI shell":** the screen-timed cold
   launches are 936–1,088 ms (ARW), 944–969 ms (NEF) and 357–389 ms (HEIC). Only the HEIC meets it. The
   in-app boot report is 323 ms (warm), which does meet it.
5. **"First pixel, 45 MP RAW < 60 ms" (09:231):** the Windows 42 MP ARW is at 60, 63 and 47 ms; two of
   three runs miss. Other cameras are at 50–134 ms (NEF 134/85/89). The Mac meets it (28 ms).
6. **"Full-res RAW decode complete < 500 ms" (09:232):** Windows misses for every RAW (0.557–1.98 s; ARW
   1.28–1.38 s). The Mac meets it (0.26–0.49 s), but the Mac boot report's ARW full_resolution is 624 ms
   (boot/mac-arw.json).
7. **"Warm folder navigation < 40 ms" (09:230):** met when warm (0.2 ms). A jump to an undecoded RAW
   takes 45–68 ms on Windows and 53–64 ms on the Mac.
8. **"Input → photon ≤ 1 refresh" (09:233):** the mouse-drag pan p99 is 51.2 ms (ARW), 85.2 ms (NEF)
   and 20 ms (HEIC) against a 16.7 ms refresh (screen.json). fps is 52.5–56.4 rather than 60.
9. **Mac playback:** 1 dropped present, 4 missed refreshes, 5 skipped frames and a worst gap of 83.5 ms
   (mac/investigation/video-native-60s.json). A/V error p99 is 11.0 ms vs 2.2 ms on Windows.
10. **4K HEVC on Windows:** 22 fps with a p99 gap of 636 ms. Below real time, though Media Player showed
    nothing at all.
11. **NEF refine:** one of three opens dropped a frame in the preview→full cross-fade, against the
    "0 dropped" rule.
12. **Remote folders:** Windows loopback (131–143 ms) is about 60 % slower than Mac loopback (79–88 ms),
    and both are slower than the local disk (72 ms Windows first pixel).
13. **Boot gates:** boot/README.md says the `gate-*` files "fail on both" (loaded machine).
14. **Mac idle CPU:** 0.6 % (vs Windows 0.03 %). It is within the ≤ 1 % gate.

### 4.4 Other README items
- **:377/:382 (S):** VS 2022 `-G`. See 1.4.
- **:9-10, :256-275 (C, accurate):** the iPhone app is on TestFlight with iOS 18+. Consistent.

---

## 5. docs/plans and the two verify docs

### 5.1 Plans built (wholly or mostly): fold into docs/design, then delete or trim
| Plan | Status evidence | Action |
|---|---|---|
| audio-and-documents.md | :8-17 "All slices are built"; shipped (README:170-179) | Delete; leftovers (§2.2 waveform strip, cover-art poster #572) go to 04/05 "Not built" |
| audio-search.md | :13 says A1 "being built", but 17 describes A1 as built | Trim to A2 only, or delete |
| document-search.md | :12-19 D1, D2 (and D4) built | Fold remainder into 17; delete |
| metadata-search-and-place.md | slices 1, 4, 5 built (:19-26, :56) | Fold into 06/27; delete |
| model-tiers.md | :18 "T1 to T7 are built" | Fold into 17; delete |
| index-watcher.md | :7 "built, not yet verified in the app"; :9 folded into 17 | Delete once verified |
| people-accuracy.md | :3 built, described in 17; keeps a short "Not built" | Move "Not built" into 17; delete |
| enhance.md | slices 1–5 built (:27-34) | Move "Left:" (:36) into 29 "Not built"; delete |
| edit-adjust.md | :6 "Nothing here is built", contradicted by :38 and :72 (both "built 2026-11-02") | Fix :6 at least; fold built parts into 20/07 |
| edit-refresh.md | S1, S2 … built (:95, :110) | Fold into 20; trim |
| video-editor-look.md | slices 1–4 built 2026-11-03 (:44-65) | Fold into 21/08; delete |
| places-world-map.md | built: `src/addons/places/world_map.h`, 27-places.md:155, :197 | Delete (already in 27) |
| driftwire-menu-and-tunnel.md | slices 1–5 built (:3-11); folded into 28 "The Tunnel" | Delete |
| check-library.md | §2.3 built (:83); §2.4 and C not built | Move the remainder to 26 "Not built"; delete |
| phone-clip-convert.md | :3 "Status: built (draft PR)" | Fold into 28/mobile; trim |
| phone-in-the-app.md | superseded (:3-5); "As built" :124 | Delete (history in 12) |
| phone-access-addon.md | built (:154 "Built:") | Becomes the design doc for phone access, or fold into a new design doc |
| desktop-client.md | :162 "built", :213 "As built" | Becomes the design doc for remote folders (`src/remote`) |
| nas-owner-and-guests.md | :3 "Slices 1–3 built"; :356 "As built" | Fold into 28 |
| self-hosted.md | **:7 says "Not built; this maps it out"**, but README:277-290 and DEVELOPMENT.md:599 `driftwire host` show it hosts on Mac and Windows | Fix :7 at once; fold into 28 or a new design doc |
| self-host-drives.md, self-host-drives-keep-a-copy.md, self-host-users-smb.md | built in part (keep-a-copy :7 "most of slices 2 and 3 are built"; users-smb :195 built) | Fold built parts; keep the remainder |
| vpn-addon.md, vpn-stealth.md, phone-vpn.md | built: `src/addons/vpn`; vpn-stealth :122 "Built (2026-11-19)"; phone-vpn S1/S2 built, on TestFlight | Need a design doc (VPN); keep only open calls |
| mobile.md (1,071 lines) | app built, on TestFlight (README:275); backup 1.1 still unbuilt | Split: design doc for the iPhone app + a slimmed plan for 1.1 |
| nas-relay.md | relay/hole punching "Built second" (:92) (S) | Check against 28 "Relays"; likely fold |

### 5.2 Plans still forward (keep)
ai-out-of-process.md (:5 "nothing is built"), linux.md (but fix :19 "has not run yet", see 2.4),
driftwire-volunteer-relay.md (:3 "not built"), driftwire-tor-spike.md (dropped; could be deleted, history
is in 12), driftwire-tls-spike.md (a spike record, S), nas-setup.md (Synology package not built,
28-driftwire.md:609).

### 5.3 docs/settings-navigation-verify.md (51 lines, last touched e0e8e4a0, 2026-09-26)
- Linked from DEVELOPMENT.md:1076, so it is still referenced.
- **:12-13 stale (C):** "change sort, direction and background". The canvas background setting was
  removed (#408, 12-decision-log.md:3272). **Proposed:** `change sort and direction`.
- The rest (Settings tabs, key capture, path bar) still describes existing UI (S). Recommendation: keep,
  but move it into DEVELOPMENT.md under the Settings section (:1062-1076) as a checklist subsection, or
  under `docs/review/`. The repo root `docs/` otherwise holds only DEVELOPMENT.md and ADDONS.md.

### 5.4 docs/system-theme-verify.md (70 lines, last touched 290892e3, 2026-10-15)
- **Not referenced anywhere** (grep for the filename finds only itself).
- Its content matches #408 (no canvas background; theme tokens). It predates the 19 built-in themes and
  grounds (README:332).
- Recommendation: link it from DEVELOPMENT.md next to the settings checklist, or fold its "Implementation"
  half into 03-rendering / 25-open-addons (themes) and delete it (S).
