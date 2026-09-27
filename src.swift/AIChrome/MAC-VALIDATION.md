# Milestone H (PRs 20–24) — Mac validation checklist

For the Mac agent that builds and verifies the Mac half of Local AI search
([plan/17](../../plan/17-local-ai-search.md)). The Mac half was **written on a Windows machine and
has never been compiled**: not the Objective-C++ host, not either Swift package. Treat every
step below as unproven until it has been run on an Apple silicon Mac, and record the numbers
the steps ask for in plan/17 or the PR description, not in memory.

Run everything on **Apple silicon** (the AI pack is arm64 only). One step (§9) also needs an
Intel Mac or an x86_64 build to prove the section is absent there.

Record for each step: pass / fail, the machine (chip, RAM, macOS), and the numbers asked for.

## Results, first Mac run (2026-09-27, Apple M5, 24 GB, macOS 26.6)

Numbers and fixes are in plan/17 "Verified on macOS". In short:

- **§1 Build:** passes after two compile fixes; all three Swift packages build with no warnings.
  `mv_ai_tests` and `ai-bench` now exist on Darwin; AI targets are arm64 only (Intel defines none).
- **§2 / §12b / §12c tests:** pass with the real pack (CPU reference for both towers and CLAP,
  Whisper base and small on the `say` clip, the grey-ICC JPEG, the labelled eval). Whisper base
  missed a sentence until the full-window re-listen fix.
- **§4-5 Core ML:** every CLIP node on Core ML only once the image tower's shapes are pinned
  (B/32 499 img/s, L/14 31; cosine 0.9992 / 0.9983). Opening compiles for 17-64 s even when
  cached, so the pack answers on CPU and swaps Core ML in when it is ready. CLAP and Whisper
  cannot use Core ML (one fails to compile, the other aborts the process): CPU on the Mac.
- **§13:** reviewed; fixed a store race at launch, main-thread Remove, list/folder races, stale
  search results, unload safety and the thumbnail pipeline (plan/17 lists them).

---

## 0. What changed on the Mac side

| Area | Files |
|---|---|
| Host: add-ons | `src/shell/addons_mac.mm`, `src/shell/addons_mac.h` — AI slot (arm64 only), `mv_addon2_*` bridge, host-table v2 pixel services, MVAIChrome selectors, host selectors `openList:` `closeList` `viewerState` `seekTo:` `setMarkers:` `viewerWindow` |
| Host: result lists | `src/shell/folder_model_mac.{h,cpp}` (`open_list`, `snapshot`), `src/shell/main_mac.mm` (list mode, `mv_chrome_open_list` & co., scrub markers, `search_*` routing, folder/item notifications) |
| Host: moment seek | `src/shell/present_lab_mac.{h,mm}` — `open_item(path, moment_ms)`: a clip adopted with a moment is paused and exact-sought, never played |
| Bridge | `src.swift/MediaViewerChrome/Sources/MVChromeBridge/include/mv_chrome_bridge.h` |
| Base Swift | `LocalSearchView.swift` (new), `AddonsView.swift` (AddonChannel refactor), `SettingsView.swift`, `CommandBarView.swift` (pill, list title row), `FolderStore.swift` (list title), `GalleryView.swift`, `VideoStore.swift` + `TransportView.swift` (match markers) |
| AI chrome | `src.swift/AIChrome/**` → `AI.bundle` (principal class `MVAIChrome`) |
| Build | `cmake/darwin.cmake` (AI.bundle target), `cmake/make-import-bundle.sh` (parameterised; Import output unchanged), `.github/workflows/ci.yml` (mac-chrome job), `tools/mac/macpack.py` (+ tests) |

---

## 1. Build

Per [docs/DEVELOPMENT.md](../../docs/DEVELOPMENT.md) § macOS (Apple silicon triplets):

```sh
export VCPKG_ROOT=/path/to/vcpkg
"$VCPKG_ROOT/vcpkg" install --triplet arm64-osx-dynamic \
  --overlay-triplets="$PWD/tools/mac/triplets" \
  --x-manifest-root="$PWD/tools/mac/dependencies" \
  --x-install-root="$PWD/build-darwin/vcpkg_dynamic"
cmake -S . -B build-darwin -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx \
  -DMV_VCPKG_DYNAMIC_PREFIX="$PWD/build-darwin/vcpkg_dynamic/arm64-osx-dynamic"
cmake --build build-darwin
cmake --build build-darwin --target mediaviewer_app
cmake --build build-darwin --target mv_import_chrome mv_ai_chrome
```

- [ ] Configure succeeds; `mv_ai` exists (cmake/ai.cmake) so `mv_ai_chrome` is part of `all`.
- [ ] `build-darwin/addons/ai/` holds `libmv_ai.dylib`, the ONNX Runtime dylib and `AI.bundle`.
- [ ] `build-darwin/addons/import/Import.bundle/Contents/Info.plist` is **byte-identical** to the
      one a pre-Milestone-H build produced (the bundle script was parameterised; defaults must
      reproduce Import exactly): `diff` it against a checkout of `main`'s build.
- [ ] `plutil -p build-darwin/addons/ai/AI.bundle/Contents/Info.plist` shows
      `NSPrincipalClass = MVAIChrome`, `CFBundleExecutable = AI`,
      `CFBundleIdentifier = org.mediaviewer.addon.ai`.

Swift packages on their own (what CI's `mac-chrome` job runs):

```sh
swift build --package-path src.swift/MediaViewerChrome -c release
swift build --package-path src.swift/ImportChrome -c release
swift build --package-path src.swift/AIChrome -c release
```

- [ ] All three build with **no errors**. Record every warning from AIChrome and
      MediaViewerChrome (concurrency warnings especially — see §13) and fix what is cheap.

---

## 2. Unit tests

```sh
ctest --test-dir build-darwin --output-on-failure -R "ai_|infer_|import_|addon"
ctest --test-dir build-darwin --output-on-failure          # the whole suite still passes
python3 tools/mac/test_macpack.py -v
python3 tools/mac/check_plists.py
```

- [ ] `mv_ai_tests` / `mv_infer_tests` (the lead's targets) pass.
- [ ] Import and add-on store tests still pass.
- [ ] `test_macpack.py`: the new `test_nested_runtime_dylib_is_signed_before_the_bundle` and
      `test_code_free_piece_signs_nothing_and_skips_notarization` pass. (On Windows,
      `CrashpadHandlerTests.test_handler_lands_in_helpers_and_is_signed_before_the_app` fails on
      the executable bit; it should pass on the Mac — confirm.)

---

## 3. Pack and sideload the AI pack (test key)

Exact commands (arm64 Mac, from the repo root; `pip3 install cryptography` first):

```sh
# 1. A dev key (never commit it) and a build that trusts it. MV_ADDON_DEV_PUBLIC_KEY
#    replaces the pinned release key in this build only; CMake prints a warning and
#    refuses it under GitHub Actions.
python3 -c "from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey as K; from cryptography.hazmat.primitives import serialization as s; k=K.generate(); open('/tmp/dev.key','w').write(k.private_bytes(s.Encoding.Raw,s.PrivateFormat.Raw,s.NoEncryption()).hex()); print(k.public_key().public_bytes(s.Encoding.Raw,s.PublicFormat.Raw).hex())" > /tmp/dev.pub
cmake -S . -B build-darwin -G Ninja -DMV_ADDON_DEV_PUBLIC_KEY=$(cat /tmp/dev.pub)   # plus your usual darwin flags
cmake --build build-darwin --target mediaviewer_app mv_ai mv_ai_chrome mv_ai_tests mv_import_tests

# 2. Models (pinned revisions + SHA-256, ~1.2 GB Core, ~40 MB faces).
python3 tools/package/ai-models.py check
python3 tools/package/ai-models.py stage --piece ai       --out build-darwin/addons/ai
python3 tools/package/ai-models.py stage --piece ai-faces --out build-darwin/addons/ai-faces
python3 tools/package/ai-models.py stage --piece ai-audio --out build-darwin/addons/ai-audio   # ~1.03 GB (CLAP + Whisper)

# 3. Sign (ad-hoc is fine for a local run; use your Developer ID to test library validation).
python3 tools/mac/macpack.py addon --dir build-darwin/addons/ai --identity "$MV_SIGN_IDENTITY" --skip-notarize
python3 tools/mac/macpack.py addon --dir build-darwin/addons/ai-faces --piece --identity "$MV_SIGN_IDENTITY" --skip-notarize
python3 tools/mac/macpack.py addon --dir build-darwin/addons/ai-audio --piece --identity "$MV_SIGN_IDENTITY" --skip-notarize

# 4. Pack + sign the manifests with the dev key.
python3 tools/package/addon-pack.py pack --addon ai       --platform macos --src build-darwin/addons/ai       --version 1.0.0 --key /tmp/dev.key --out dist/addon
python3 tools/package/addon-pack.py pack --addon ai-faces --platform macos --src build-darwin/addons/ai-faces --version 1.0.0 --key /tmp/dev.key --out dist/addon
python3 tools/package/addon-pack.py pack --addon ai-audio --platform macos --src build-darwin/addons/ai-audio --version 1.0.0 --key /tmp/dev.key --out dist/addon
python3 tools/package/addon-pack.py ceiling dist/addon/mediaviewer-addon-ai*-macos.json   # must print <= 3.0 GB
```

The model-dependent tests use the staged folders directly:

```sh
MV_AI_PACK_DIR=$PWD/build-darwin/addons/ai MV_AI_EVAL_DIR=<labelled folder: img/ + labels.json> \
  ./build-darwin/mv_ai_tests          # reference tolerance on CPU, tokenizer golden, labelled ranking
```

(`MV_AI_EVAL_DIR`: any folder of photos with a COCO-style `labels.json`
`[{"file": "...", "sentences": ["..."]}]`; the Windows run used 1,225 COCO Karpathy-test images.
Core ML tolerance: temporarily run the reference case with `session_options.on = coreml` or
use the app's Settings → Compute = Core ML and compare search results with CPU.)

A release build (no `MV_ADDON_DEV_PUBLIC_KEY`) must REFUSE the same dev-signed pack as
`invalid` / `bad_signature` — that refusal is itself a check (§5).

Sideload (offline install, plan/17 "Offline / sideload"):

- [ ] Unzip the pack into `~/Library/Application Support/MediaViewer/Add-ons/AI/<version>/`
      with `manifest.json` + `manifest.json.sig` beside the files, and `AI Faces/<version>/`
      for People. Launch: Local search loads (Settings → Local search shows the management view).
- [ ] The dylibs and `AI.bundle` must carry the app's Team ID (library validation). An ad-hoc
      signed app and an ad-hoc signed pack also load together; a pack signed by another team is
      refused at `NSBundle -loadAndReturnError:` — Settings says it "did not pass verification".

Release channel (once the lead's release.yml publishes it): `mediaviewer-addon-ai-macos`
and `mediaviewer-addon-ai-faces-macos` (`.json`, `.json.sig`, `.zip`) on the same
`releases/latest/download/` base as Import.

---

## 4. PR 20 spike — Core ML throughput and operator coverage

Measure on the dev Mac (record chip, cores, RAM, macOS, ORT version from the `runtime` field of
`settings_json`, and model precision):

| Model | Provider | img/s (batch as shipped) | ms / text query | ops on CPU (fallback) | notes |
|---|---|---|---|---|---|
| CLIP ViT-B/32 | Core ML (Auto) | | | | |
| CLIP ViT-B/32 | CPU only | | | | |
| CLIP ViT-L/14 | Core ML (Auto) | | | | |
| CLIP ViT-L/14 | CPU only | | | | |

- [ ] Throughput: the lead's bench (`mv_infer_tests` bench or `tools/…`, whichever lands) over
      ≥ 500 fixed 224×224 images, warm (discard the first 20). Record img/s.
- [ ] Operator coverage: run ORT with verbose logging
      (`ORT_LOGGING_LEVEL=0` / session log severity 0) and count nodes assigned to
      `CoreMLExecutionProvider` vs `CPUExecutionProvider`. Record the unsupported op types.
- [ ] Settings → Compute → Core ML then CPU only: the status badge changes ("Neural Engine" /
      "CPU") without a re-index (`frames_indexed` unchanged).
- [ ] Force a fallback (e.g. a build without the Core ML EP, or the self-test marking it slower):
      the footer shows the reason line ("Core ML was slower than the CPU here — using CPU").

---

## 5. PR 20 verify (Mac)

- [ ] **Embedding tolerance.** The fixed test image set embeds on **CPU** and on **Core ML**
      within the recorded tolerance of the reference (PyTorch / ORT-Python) vectors
      (the lead's `infer_` tests; record max abs error and min cosine for both providers).
- [ ] **Text query ranks the small labelled set correctly** (both providers).
- [ ] **App bundle unchanged with the pack absent.** Build `MediaViewer.app` from this branch and
      from the PR 8 baseline; `diff -r` the two bundles (after stripping signatures:
      `codesign --remove-signature` on copies) shows no ORT, no model, no `AI.bundle`; the
      packaging assert still fails if an ORT dylib is copied into `Contents/Frameworks`.
- [ ] **Base app unchanged with no piece installed.** Fresh user (empty
      `~/Library/Application Support/MediaViewer/Add-ons`): no ⌘F / ⌘⇧F / N binding in `?` or in
      Settings → Keyboard shortcuts; ⌘F does nothing; no pill in the command bar; the only
      surface is Settings → Local search offering Install.
- [ ] **Nothing downloads before the click.** With a proxy (mitmproxy, system proxy + CA trusted)
      open Settings: only the two manifest GETs (`…-ai-macos.json`, `.json.sig`, and the People
      pair) appear; the archive is requested only after "Install local search — downloads ~N GB,
      uses ~N GB" is clicked. N matches the manifest's archive size / installed_size.
- [ ] **No identifier in the download request.** In the proxy: the URLs are the fixed
      `…/releases/latest/download/mediaviewer-addon-ai-macos*` with no query string; headers
      are `User-Agent: MediaViewer` plus the transport's own; no cookies; nothing from the user
      (paths, names, machine id).
- [ ] **Tamper refusal.** Flip one byte in an installed model file → next launch Settings says the
      installed copy did not verify and Local search is not started (no ⌘F). Same for an edited
      `manifest.json`, a removed file, and an extra file in the version folder. A pack signed
      with a non-pinned key is refused before the archive is fetched.
- [ ] **3 GB ceiling.** With Core installed, fake a People manifest whose `installed_size` would
      exceed 3 GB in total: the Install button is disabled with the sentence
      "People needs about … Remove another piece first." and nothing is downloaded; forcing an
      install through the store is refused too (`mv_addons_install` false).
- [ ] **Install / Remove per piece** works: Core, then People (People is disabled until Core is
      installed); the budget bar animates to the new total; installing or removing People while
      Core runs is picked up at once (`set_setting("reload")`, no restart);
      Remove Core asks "Also delete the search index?" — the default keeps it (check
      `~/Library/Application Support/MediaViewer/Add-ons/AI/data/` still has `index.db`), the
      other choice deletes it. Removing Core also removes People.
- [ ] **Mac PR 1 present-loop gate with the pack installed and idle:**
      `./build-darwin/bin/frametime --seconds 60 --lab ./build-darwin/bin/mediaviewer_lab`
      → 0 dropped, p99 within 10 % of the baseline, idle CPU as before, 0 presents at rest.
      (`frametime` on Darwin must report `drop_source` `Metal display-link`.)
- [ ] **"guy on a skateboard"** retrieves the PR 20 labelled examples at the recorded target.

---

## 6. PR 21 verify (Mac) — sampler and index

- [ ] **1-hour 4K HEVC clip** (camera or iPhone) indexes to completion. Record wall time,
      assets/s and frames/s from the footer, and whether sampling ran through VideoToolbox or
      software (F3 is the viewer's decoder; for the sampler, the lead's log line / counters).
      Record the **software-sampling** time as well (force it, e.g. a build flag or an env var the
      lead provides) — plan/17 asks for both paths.
- [ ] **Kill / resume.** `kill -9` the app mid-clip; relaunch: indexing resumes at the last
      committed frame (frames_indexed never goes backwards, no committed frame re-embedded —
      the lead's counter or log shows the resume point).
- [ ] Editing / replacing a file re-queues it; reopening a remembered tree queues only its delta.
- [ ] A fresh and an incremental run publish measured assets/s, frames/s and an ETA range in
      the footer ("Indexing 1,204 of 5,300 · about 6–9 min").
- [ ] **Present loop WHILE INDEXING.** Start indexing a large folder, then run
      `frametime --seconds 60` while playing a *different* 4K clip and panning photos: 0 dropped
      frames. The footer / pill shows "Paused while a video plays" during playback (yield visibly
      triggers) and indexing resumes when idle. **Also check cadence, not only drops** (see the
      team note: a 0-drop soak can hide a stalled animation — watch F3 and a GIF/clip cadence).
- [ ] **HDR clip not washed out.** Index an iPhone HLG/Dolby Vision clip; search for something
      in it; the result tile and the found moment look like the SDR-tone-mapped viewer image
      (not grey / flat).
- [ ] **Crash report privacy.** Put a clip in a canary folder (`crash_canary.py make`), start
      indexing it, force a crash mid-index (`kill -SEGV`, or `MV_CRASH_TEST` if the lead adds an
      index-worker mode), relaunch, then
      `python3 tools/mac/crash_canary.py scan ~/Library/Application\ Support/MediaViewer/Crashes/completed/*.dmp --forbid <canary folder> --forbid <canary file> --forbid "$USER"`
      → PASS; also confirm no embedding vector (scan for a known 16-byte fp16 run from the
      index) and no decoded frame bytes are in the dump.
- [ ] Battery: on battery below the chosen threshold the footer says "Paused on battery".

---

## 7. PR 22 verify (Mac) — search and results

- [ ] **< 100 ms over 100 k indexed frames:** time from the debounced `search_text` call to
      `MV_ADDON_EVENT_AI_SEARCH_DONE` (Instruments' os_signpost or the lead's log timing) — record
      p50 / p95 over 20 queries.
- [ ] recall@10 on the labelled eval set meets the target recorded from PR 20.
- [ ] "Nothing matches “qwxzv”. Try describing what's in the picture: “dog on a beach”." for a
      nonsense query.
- [ ] **Enter lands on the moment:** Enter on a clip tile → the viewer shows the result list
      (path row shows the query and "N results"), the clip opens **paused** on the stored
      moment, within one GOP-decode of the PTS (compare against a burned-in timecode clip); the
      path row reads "Search: <query>" and "N results", like the Windows breadcrumb. No
      audio blip, no frames of the clip's head shown first.
- [ ] Cmd+Enter opens the same list as the gallery grid; "Back to folder" returns to the folder.
- [ ] Two results named `IMG_0001.JPG` from different folders show distinct names
      ("IMG_0001.JPG — DCIM") and distinct thumbnails in the gallery and filmstrip.
- [ ] **Match markers:** on a clip from the results, accent dots sit above the scrub bar at every
      matching moment, the current one larger, fading in (~200 ms). Opening a clip that is not in
      the search shows none.
- [ ] **Keyboard-only flow (no mouse at all):** ⌘F → type → ↓ into the grid → arrows →
      Return (opens paused on the moment) → N / ⇧N step next / previous match (exact seek,
      stays paused if paused; the larger dot moves) → ⌘F → Esc (grid → field) → Esc (closes).
      Previous query is pre-selected on reopen. Also: scope and kind chips reachable with Tab.
- [ ] **Folder offer:** open an un-indexed folder, ⌘F: "This folder is not indexed yet" with
      "Index this folder" / "Index this folder and subfolders"; choosing one starts indexing and
      results appear as the index grows (the panel re-runs every ~4 s while indexing without
      re-animating tiles already on screen).
- [ ] The panel never blocks the viewer: with it open, clicking the canvas and using the arrow
      keys navigates the folder; ⌘F brings the panel back to key.
- [ ] Command-bar pill: visible only while indexing / waiting ("Indexing 1,204 of 5,300"),
      hidden when idle; clicking it opens the panel.
- [ ] Look: light, dark and Increase Contrast; accent colour follows System Settings;
      12 pt panel corners, 8 pt tile corners; hover lifts a tile; the selection ring glides
      between tiles; with **Reduce motion** on, only fades remain.

---

## 8. PR 23 verify (Mac) — find similar, management, hardening

- [ ] ⌘⇧F on a still → "Similar to <name>" chip; results are visually related. On a paused clip
      frame (never sampled) → related frames from the same and other clips. ✕ on the chip returns
      to the text query.
- [ ] **Clear index keeps thumbnails:** note `du -sh ~/Library/Caches/MediaViewer/thumbs` and the
      index size; Settings → Local search → Clear index… → confirm: index size drops to ~0, the
      thumbnail cache size is unchanged and the gallery still shows thumbnails instantly.
- [ ] Per-folder Pause / Resume / Rescan / Remove work; Remove leaves the user's files untouched.
- [ ] **Model-upgrade migration:** switch Search quality Fast → High: the footer shows
      "Upgrading the index: X of Y…", queries keep answering from the old model until it
      finishes, and a query mid-migration never mixes vector spaces (the lead's test + a manual
      query whose results do not change character mid-way).
- [ ] Uninstall removes the pack, and the index only if chosen.
- [ ] Manifest / index fuzz corpus (the lead's targets) runs clean on the Mac too.

---

## 9. Intel Mac

- [ ] On an Intel Mac (or an x86_64 build): Settings has **no** Local search section at all;
      `mv_addon2_supported("ai")` is false; no AI slot is ever loaded even with a pack folder
      present; Import still installs, loads and works.

---

## 10. PR 24 verify (Mac) — People

- [ ] Off by default. Turning on "Find people in your photos" shows the privacy line
      "Face data stays on this computer, is never shared, and can be deleted at any time".
- [ ] Without the People piece: the section says to install People; nothing about faces runs.
- [ ] With People installed and faces on: circular covers appear, also for RAW / HEIC / clip
      covers (`face_thumb(cover_face)`, cropped in memory with `cover_box`; confirm with `fs_usage -w -f filesys MediaViewer | grep -i crop` or by checking
      no new image files appear anywhere); rename persists across relaunch; "Merge into…",
      "Not this person" (hover ✕ and the Delete key) and multi-select "Split into new person"
      persist and re-apply after a rescan.
- [ ] Search "photos of <name>" returns that person; "Show photos" from a person opens the panel.
- [ ] **Turning faces off deletes every face vector:** confirm the prompt, then scan the pack's
      data folder and the index (`sqlite3 …/index.db .tables`, and `faces.db` gone or empty) —
      no face vectors, crops or names remain on disk; the frame index is intact (searches still
      work).
- [ ] A forced crash while faces index: the minidump contains no face vectors or crops (§6 scan).
- [ ] Mac PR 1 present-loop holds while faces are indexing.

---

## 11. Import must not regress (Milestone G)

- [ ] An **Import 1.0.0** pack built from `main` (before this branch) installs and loads beside
      the AI pack; ⌘⇧I opens Import; a card import runs; its status line shows in the command
      bar; the one-time card hint still appears when Import is absent.
- [ ] Settings → Add-ons → Import install / remove / reinstall behave exactly as before
      (the channel code moved into `AddonChannel`).

---

## 12. Crash / privacy spot checks

- [ ] `log stream --predicate 'process == "MediaViewer"'` while searching, indexing and using
      People: no path, file name, query text or person name is logged.
- [ ] The search panel and the management view never block the main thread: Instruments' Time
      Profiler shows `roots_json`, `result_thumb`, `people_json`, `person_faces_json`,
      `clip_matches` and all thumbnail / face decodes off the main thread.

---

## 12b. Audio: sounds and speech (2026-09-27)

The **Sound** piece (`ai-audio`, name `AI Audio`, arm64, ~1.03 GB, part_of `ai`, channel
`mediaviewer-addon-ai-audio-macos`) adds CLAP (what a clip sounds like) and Whisper (what is
said). Videos are indexed for **Pictures / Sound / Both** (Settings, and per folder).

### Build and tests

```sh
cmake --build build-darwin --target mv_ai_tests
# A speech clip with known words at 5 / 15 / 28 s (the Mac twin of
# tools/ai-reference/make_speech_clip.ps1). Any ffmpeg on PATH (a test asset,
# never shipped); h264_videotoolbox keeps it free of GPL encoders.
say -o /tmp/s1.aiff "Happy birthday Anna, happy birthday to you."
say -o /tmp/s2.aiff "Ladies and gentlemen, we are now landing in Lisbon."
say -o /tmp/s3.aiff "Come here Max, good boy. Fetch the ball."
ffmpeg -y -f lavfi -i color=c=black:s=640x360:r=30:d=40 \
  -i /tmp/s1.aiff -i /tmp/s2.aiff -i /tmp/s3.aiff \
  -filter_complex "[1]adelay=5000:all=1[a];[2]adelay=15000:all=1[b];[3]adelay=28000:all=1[c];[a][b][c]amix=inputs=3:normalize=0,apad[out]" \
  -map 0:v -map "[out]" -t 40 -c:v h264_videotoolbox -b:v 1M -c:a aac /tmp/speech.mp4
MV_AI_PACK_DIR=build-darwin/addons/ai MV_AI_AUDIO_DIR=build-darwin/addons/ai-audio \
MV_AI_SPEECH_CLIP=/tmp/speech.mp4 \
  ./build-darwin/bin/mv_ai_tests "[audio]"
```

- [ ] `mv_ai_tests "[audio]"`: FFT, feature extractors, CLAP tokenizer/towers within tolerance,
      and Whisper finds "birthday" (4.0–6.5 s), "lisbon" (14–16.5 s) and the 28 s line, for
      whisper-base and whisper-small. None may SKIP on the Mac run (a skip means an env var or
      the host decoders were missing).

### Core ML throughput (record like §4)

| Model | Provider | throughput | notes (ops on CPU) |
|---|---|---|---|
| CLAP audio tower (10 s windows) | Core ML / CPU | windows/s | |
| CLAP text tower | Core ML / CPU | ms / query | |
| Whisper base (30 s windows) | Core ML / CPU | × real time | |
| Whisper small (30 s windows) | Core ML / CPU | × real time | |

### Verify

- [ ] **Settings:** a *Sound* row with its size beside Core and People; the budget bar adds its
      ~1.03 GB; a family over 3 GB is refused before download. Install while Core runs is picked
      up at once (`set_setting("reload")`); Remove asks, then sound search stops.
- [ ] **"Index videos for: Pictures · Sound · Both"** (an `NSSegmentedControl`: a `.segmented`
      `Picker` ignored per-item `.disabled`, owner report 2026-09-27): Sound and Both cannot be
      clicked, with the hint "install Sound above", until the piece is loaded; enabled after. Each indexed
      folder's menu offers Default / Pictures / Sound / Both (→ `root_set_media`; Sound/Both
      disabled without the piece) and shows its choice ("Videos: Sound").
- [ ] **Status:** the management view and the panel footer show
      "Sound: 12 of 40 clips · Speech: 8 of 40" while it runs; the command-bar pill says
      "Indexing sound 12 of 40 clips" once pictures are done.
- [ ] **Sound search:** a clip with a dog barking at a known time; with Sound indexed, ⌘F
      "dog barking" (Sounds chip on, or no chip) finds that clip; the tile carries the
      `speaker.wave.2` badge; Enter opens it paused at the moment, within 2 s of the bark.
- [ ] **Speech search:** index `/tmp/speech.mp4`; ⌘F "landing in Lisbon" (Speech chip) returns
      it with the `text.bubble` badge and the quoted snippet under the tile; Enter lands
      **within 2 s of 15 s**; N / ⇧N walk the other speech matches.
- [ ] Chips: Pictures · Sounds · Speech combine (none on = all three); Sounds / Speech are
      disabled with a tooltip while the piece is absent. Keyboard: Tab reaches them.
- [ ] **Both present-loop gates WHILE INDEXING SOUND:** Mac PR 1 (`frametime --seconds 60`,
      0 dropped, p99 ±10 %, and watch cadence) while a folder of clips indexes for Sound, and
      again while playing a different clip (the footer shows "Paused while a video plays").
      The Windows PR 1 gate is the Windows agent's.
- [ ] Privacy: the minidump scan (§6) also forbids a transcript phrase ("landing in Lisbon");
      `log stream` shows no transcript text.

## 12c. After the first real Windows runs (2026-09-27)

Found by running the real pack on Windows; each changed shared code the Mac also runs.

- [ ] **`io::file_stat::mtime_ns`** is new and filled in `src/io/file_port_mac.cpp` from
      `st_mtimespec` (not compiled on Windows). `mv_import_tests "[store]"`: the same-size,
      same-second tamper after a cached verify must still be refused.
- [ ] **Add-on files are hashed once per process** (`verify_files`): time a cold launch with the
      whole pack before and after a relaunch; Settings → Add-ons, the load, and each piece no
      longer re-hash (~6 GB per launch before).
- [ ] **Model loading waits for a quiet viewer.** Launch with the pack while a clip plays (or
      run the Mac PR 1 soak from launch): the management view says "Loading the search model
      when the viewer is idle" with a still ring, the pill does not appear, and the soak shows
      0 dropped frames through the pack's load. Stop playback: it loads, pictures first, then
      Sound. Record the Mac PR 1 gate with the pack installed, from a cold launch, three times.
- [ ] **Self-test kept:** first launch on Core ML writes `selftest.txt` in the AI data folder;
      record the load time (status LOADING → IDLE / INDEXING) on the first and second launch.
      The second must skip the CPU session (expect roughly the CPU tower's open + 3 batches less).
- [ ] **Settings before load:** open Settings → Local search right at launch: it must not beach
      ball (no ONNX Runtime load on the main thread); models and compute fill in when loading
      finishes (`MV_ADDON_EVENT_AI_COMPUTE` → `reloadSettings`).
- [ ] **"Nothing found" for one-word queries:** `mv_ai_tests "[eval]"` now includes "a dog",
      "a cat" and "dog" and checks nonsense against both tests (margin 0.04, or top-ten z 2.5).
      By hand: "dog", "cat", "car" on a real folder return results; "xyzzy plugh" returns none.
- [ ] **Grey JPEGs with a grey ICC profile** (scans, some archives) now open instead of failing
      as corrupt: `mv_tests "[colour]"`, then open one in the viewer and index a folder with one
      (Settings shows 0 failed).

---

## 13. APIs used that were NOT compile-checked (confirm on the Mac)

Objective-C++ (host):

- `std::unique_ptr` handed through two `dispatch_async` blocks as a raw pointer
  (`start_ai_load`); C++ `std::string` captured by value in blocks.
- `id` members in a C++ struct under ARC (`addon_slot`), `(id<MVAIChrome>)` casts,
  `[chrome respondsToSelector:@selector(runCommand:)]` then a `BOOL` return from a Swift
  `@objc(runCommand:) func … -> Bool`.
- `(__bridge void*)` of the chrome's `NSView` returned to Swift and read back with
  `Unmanaged<NSView>.fromOpaque(_:).takeUnretainedValue()`.
- ObjC methods with C++ parameter / return types (`-openListTitled:paths:moments:select:gallery:`,
  `-displayNameAt:` returning `const std::string&`) used before their definition in the same
  `@implementation`.
- `"—"` in a narrow C++ string literal (expects UTF-8 execution charset).
- `mv::status_name` (core/status.h) in addons_mac.mm; `mv::addon::media::*` assigned to the
  `host_services` std::functions.
- `present_lab_mac`: `media_->pause()` then `media_->seek(ns, true)` on a freshly adopted clip —
  confirm the preview path presents the sought frame while paused and then idles (no
  every-vblank present after it: the PR 1 idle gate).

Swift (base app, `MediaViewerChrome`):

- `NSViewRepresentable` returning another bundle's `NSHostingView` directly, sized by its
  intrinsic content size (`sizingOptions = [.intrinsicContentSize]`) inside a `ScrollView`.
  If the height does not follow the content, wrap it and implement `sizeThatFits`.
- `.contentTransition(.numericText())`, `@ViewBuilder` on the `PathBar.trail` property,
  `Menu`/`Button(role:)`, `mv_chrome_ai_status()` zero-init of a C struct, `mv_addon2_*`
  C functions taking Swift `String` for `const char*`.
- Swift 5 concurrency: values captured into `MainActor.run` from `Task.detached` (`[String: Any]`
  is not Sendable → warnings expected, errors not).

Swift (AI chrome, `AIChrome`):

- C import of `mediaviewer_ai.h` via the relative-include module map (enum constants such as
  `MV_AI_STATE_INDEXING.rawValue`, `MV_ADDON_EVENT_AI_SEARCH_DONE.rawValue`, macro constants
  `MV_AI_STATUS_FACES_READY` as `UInt32`), optional C function pointers called as
  `a.search_text?(ctx, String, …, &id)`, `withOptionalCString` for `NULL` scopes.
- `NSObject.perform(_:with:)` to host selectors and `takeUnretainedValue()` of `NSDictionary`,
  `NSNumber`, `NSWindow` results; `perform` of a `void` selector with the result ignored.
- `NSPanel` subclass (`canBecomeKey` override), `.borderless + .fullSizeContentView`,
  clear background, `addChildWindow(_:ordered:)`, `hidesOnDeactivate`,
  `collectionBehavior = [.fullScreenAuxiliary, .moveToActiveSpace]`; click-through of the
  transparent shadow margin.
- SwiftUI (macOS 14): `onKeyPress(_:)`, `onKeyPress(keys:)`, `onKeyPress(characters:)`,
  `.focusable()`, `.focused(_:equals:)`, `.focusEffectDisabled()`, `onExitCommand`,
  `matchedGeometryEffect` for the selection ring, `ScrollViewReader.scrollTo`,
  `.task(id:)`, `.sheet(item:)`, `accessibilityReduceMotion`, `NSText.selectAll(_:)` via
  `NSApp.sendAction` to pre-select the previous query.
- ImageIO `CGImageSourceCreateThumbnailAtIndex` (tiles, and the `face_thumb` JPEG);
  `CGImage.cropping(to:)` with a top-left origin for `cover_box` / `box` (assumes 0..1 of the
  image `face_thumb` returns, origin top-left — confirm against a known face).
- `MemoryLayout<mv_ai_api>.offset(of: \mv_ai_api.face_thumb)` checked against `struct_size`, so
  a pack whose table predates `face_thumb` shows monograms instead of reading past its table.
- `NSCache` as the LRU; `NumberFormatter`, `ByteCountFormatter`.
- Audio (2026-09-27): `MemoryLayout<mv_ai_api>.offset(of:)` with a `PartialKeyPath` (`has(_:)`)
  guarding `root_set_media` / `result_snippet`; `mv_ai_result.match` and the appended
  `mv_ai_status` sound/speech fields read through the C import; `result_snippet` called on the
  worker inside the result loop; `NSSegmentedControl.setEnabled(_:forSegment:)` in a
  representable ("Index videos for"); `Menu` items with a
  `Label` checkmark; the chip `Set<Find>` OR'ed into `kinds`.
