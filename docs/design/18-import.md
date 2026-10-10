# 18 — Import (an installable add-on)

How the Import add-on copies a card or folder into a photo library, and how the add-on
mechanism it introduced installs, verifies and loads add-ons on Windows and macOS.

Import skips what is already in the library, checks every copy, sorts by date on the way in,
and never loses a file. It is an optional add-on installed from Settings; the base viewer, its
installer and its updates do not carry it. Both platforms share one engine (`src/addons/import`)
with a WinUI and a SwiftUI window. Roadmap labels: PRs 16–19 (engine, window, presets,
library tools) and PR 54 (Find duplicates).

## What it is, honestly

Import does not move bytes faster than the hardware: the card reader, the USB link and the SSD
set the speed, and Explorer and Finder already copy close to it. Import saves time where it is
actually lost:

- **copying less:** files already in the library are skipped by content, not by name;
- **no second pass:** each file is checked as it is copied, not in a separate re-read of the card;
- **no babysitting:** every decision is made before the copy starts, so it never stops
  halfway to ask "Replace or skip?";
- **no sorting afterwards:** files land in dated folders, with pairs kept together;
- **no guessing:** what is new on the card is shown before copying, and a report comes after.

## Compared with Explorer and Finder

| | Explorer / Finder copy | Import |
|---|---|---|
| Already-copied files | Name clash dialog, mid-copy; a renamed duplicate is copied again | Skipped by **content hash** (size, then BLAKE3). Renamed duplicates are caught. Same name with different bytes is kept under a safe name. Decided up front |
| "What's new on this card?" | You work it out | **New since last import** per file and per day; default selection is "new only" |
| Is the copy good? | Not checked | **Every file verified**: hashed while read, read back from the destination uncached, compared. A bad copy is retried, then reported, never counted as done |
| Move to another drive | Deletes the source after copying, unchecked | Deletes the source **only after verify**. Import never deletes from, formats or erases a card |
| Card pulled / crash mid-copy | Partial file left; restart from scratch | No partial files (temp + rename). **Resumes**: already verified files are not re-copied |
| Backup at the same time | A second copy job, reading the card twice | **Second destination** from the same read, both verified |
| Where files go | One folder you picked | **Layouts** (date taken, camera, type, keep card structure, flat) with a live preview of the resulting folders |
| Names | Camera names (`IMG_0001` repeats every 10 000 shots) | Optional **rename templates** with a live preview |
| RAW+JPEG, Live Photos, camera sidecars | Copied as loose files; may be split | Moved as **one unit**: pairs, `.xmp`, `.THM`, `.LRV`, Sony `.XML` travel with their file |
| Preview before copying | Slow thumbnails; RAW needs codecs on Windows | The viewer's own thumbnails: embedded RAW previews, HEIC, no codec packs |
| Two cards at once | Two jobs that fight over one disk | One reader per card, one writer per disk. Cards in different readers run together |
| Viewer while copying | n/a | Keeps panning at refresh: Import yields to the render thread |
| Afterwards | Nothing | Summary (copied / skipped duplicates / failed), **Eject**, **Open in viewer**, a saved local report |

## Painless by default

Plug in a card, press **Import**. The defaults:

- source: the card that just appeared; selection: **new files only**;
- destination: the last one used, else `Pictures\MediaViewer` / `~/Pictures/MediaViewer`;
- layout: `YYYY/YYYY-MM-DD`; names unchanged; duplicates skipped; full verify; eject when done.

One screen, one button, no prompts while copying. The first import asks for the destination
once.

## The Import window

A separate window on both platforms (WinUI 3 `MediaViewer.Import.Chrome`; SwiftUI
`Import.bundle`), so viewing carries on while it copies. It is keyboard-complete
([16-commands.md](16-commands.md)).

```
┌ Import ─────────────────────────────────────────────────────────────────────────┐
│ SOURCES            │ EOS_DIGITAL · 812 new of 1,204 · 38.4 GB     [New only ▾] │ PRESET  Daily ▾   │
│ ▣ EOS_DIGITAL      │ ☑ Sat 21 Sep · 142 (all new)                             │ To  D:\Photos   … │
│   64 GB · 812 new  │  [▣][▣][▣][▣][▣][▣][▣][▣]                                │ Backup  E:\Bak  … │
│ ▢ SD Card 2        │ ☑ Sun 22 Sep · 670 (670 new)                             │ Layout  Date  ▾   │
│   32 GB · 0 new    │  [▣][▣][▣][▣][▣][▣][▣][▣]                                │ Rename  Off   ▾   │
│ ＋ Folder…          │ ☐ Mon 16 Sep · 392 (already imported)                    │ ─ Where files go ─│
│                    │  [◌][◌][◌][◌]  dimmed: already in library                │ 2026/2026-09-21 142│
│ RECENT             │                                                          │ 2026/2026-09-22 670│
│  Imports…          │  RAW+JPEG = one tile · LIVE badge · ▶ video              │                    │
├────────────────────┴──────────────────────────────────────────────────────────┴────────────────────┤
│ 812 files · 38.4 GB · 392 duplicates skipped · ≈ 6 min at 110 MB/s        [ Import 812 ]          │
└─────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **Sources** (left): cards and drives as they appear, each with an "N new" count. Folders can
  be added (remembered in `import.db`). A network share works and is labelled slower.
- **Contents** (centre): a virtualised grid (GridView / LazyVGrid) grouped by day, with a
  checkbox per day and per file. Files already imported are dimmed. RAW+JPEG and Live Photo
  pairs are one tile. `Space` toggles; `Enter` on a focused tile opens it in the main viewer
  (culling before copying); viewer marks carry over.
- **Preset** (right), simplified 2026-10-05 (owner: "its really complicated to use"): three plain
  steps — **Where to** (the destination, or one *Choose a folder…* button while none is set),
  **What to import** (New / All / Marked / Dates) and **How to organise** (the layout in words,
  with the first folders of the **Where files go** preview under it) — then *Also copy to a
  backup drive*, *Eject the card when done* (removable sources only), and everything else
  (file types, camera / type folders, date source, rename, duplicate skipping and scope, full
  verify, notify, fast, saving settings, card binding) under a collapsed **More options**. The
  preset keys and the engine are unchanged. The saved-settings picker shows only once there
  is more than one.
- **Empty states:** no source ("Insert a memory card, or choose a folder"), reading, unreadable,
  everything already imported (a strip over the grid, or *Show all N files* when the grid is
  empty), and nothing matching. **Select all / Select none** sit over the grid.
- **Tools** (Past imports, Check a folder for damaged files, Find duplicates, About Import) are
  one menu under the sources instead of four buttons.
- **Bottom bar:** what is selected and its size, then a quieter line with what will be skipped
  and an ETA from measured throughput per device. With no destination the button reads
  *Choose where to import…* (and Return opens the folder picker) instead of confirming an
  import to nowhere. *Import N* waits for the plan on screen: it is off during a rescan or a
  replan, and while a typed field (rename, dates) has not been planned yet (that happens when
  typing pauses, or on Return).
- **While copying:** the window becomes a progress view: per-destination bars, the current file,
  MB/s, verified so far, **Pause** and **Cancel**. Closing the window keeps the job running, with
  a progress indicator in the main command bar. A notification arrives when it finishes.
- **Summary:** copied / skipped (with what each matched) / failed (with the reason, and **Retry
  failed**). **Eject**, **Open in viewer** (opens the destination folder), **Show report**.
  Import history (what came from which card, when) is listed under Recent.
- **Interrupted jobs:** when a copy fails because the source or destination root is gone, the
  job stops as *interrupted* with the rest pending; the app lists it at next open with
  **Resume**.

## Configurability

All settings are saved in **named presets** (`presets` table). A preset can be bound to a card
(by volume id), so that card opens with it selected.

| Setting | Options | Default |
|---|---|---|
| Selection | New since last import · all · marked in viewer · date range · type filter (RAW / JPEG / HEIC / video / other) | New only |
| Destination | Any folder or drive; network shares labelled slower | Last used |
| Backup destination | Off · a second folder or drive, written from the same read | Off |
| Layout | `YYYY/YYYY-MM-DD` · `YYYY/MM/DD` · `YYYY-MM-DD` · + camera model · + type subfolders (`RAW`/`JPEG`/`Video`) · keep card structure · flat | `YYYY/YYYY-MM-DD` |
| Date source | Date taken (EXIF / container) → file time fallback, labelled · file time only | Date taken |
| Rename | Off · template of `{date}` `{time}` `{camera}` `{seq}` `{original}` `{ext}` with a live preview; `{seq}` is per-day and survives re-imports | Off |
| Duplicates | Skip content duplicates · import anyway (safe name) · scope: this destination / the whole library index | Skip, this destination |
| Name clash, different bytes | Keep both (safe name). Overwrite is not offered | Keep both |
| Verify | Full read-back · hash-on-read only (network targets) | Full |
| Companions | Keep camera sidecars with their file (`.xmp` `.THM` `.LRV` `.XML`) | On |
| After import | Eject card · open destination in viewer · notify · nothing | Eject + notify |
| On card insert | Do nothing · open Import · **auto-import with this preset** (per card, opt-in, remembered by volume id) | Open Import |
| Priority | Background (viewer stays smooth) · fast | Background |

**Never offered:** deleting from the card, formatting a card, overwriting a destination file,
or any upload.

The preset model is `mv::import::preset` (`src/addons/import/model.h`); its JSON parse is strict
(unknown keys ignored, a mistyped known key fails).

## The engine

`src/addons/import/engine.*`, behind `mv.import.1` (`mediaviewer_import.h`). One control thread
runs scans, plans and card arrivals in order; each job runs on its own thread.

- **Duplicate test:** size first; BLAKE3-256 only when a size matches. Never by name. The
  library index (`import.db`, SQLite, table `library`: root, relative path, size, mtime → hash)
  makes a second import cost a size lookup. A row is rechecked when size or mtime differ.
- **Per-card memory:** after each import the card's volume id plus each file's (path, size,
  mtime, hash) are recorded (`card_files`). "New since last import" is a lookup, not a re-hash.
  Card memory and the library index key on the **volume-relative path**, so it holds whether
  the card is scanned from its root or a subfolder.
- **Verify** (`io::verified_copy`): hash while reading the source (one pass over the card).
  Write to a sibling temporary `<final>.mvtmp` (then `.mvtmp2` …), flush (`FlushFileBuffers` /
  `F_FULLFSYNC`), read back uncached (`FILE_FLAG_NO_BUFFERING` / `F_NOCACHE`), compare, then
  rename into place without replacing anything. On mismatch: delete the temporary, retry once,
  then fail that file. On a share, the uncached read-back proves the bytes the server returns,
  which may come from the NAS's RAM rather than its disks.
- **Throughput:** one reader per physical source and one writer per physical destination
  (a per-device lock held for each file, so two cards to one disk alternate file by file), with
  2–4 large buffers in flight between them. Reads and writes overlap. **No parallel reads of one
  card.** Two sources on different devices run concurrently. Measured MB/s per device drives the
  ETA. **A network destination** uses the deep path ([24](24-transfer.md)): several writes in
  flight at their offsets (depth 8, 2 MiB chunks) and a pipelined read-back. The card side stays
  one request at a time.
- **Units:** RAW+JPEG and Live Photo pairs (the viewer's pairing) and camera sidecars are one
  unit, copied, verified, skipped and sorted together, never split across dated folders. Type
  folders keep units together (a RAW+JPEG pair goes under `RAW`, a clip under `Video`). A unit is
  all-or-nothing: if one member fails, the members already written for it are removed and the
  whole unit is reported.
- **Backup:** duplicates are decided per destination. A unit the main destination (or, at
  library scope, the library) already holds is skipped there, but a backup destination that
  lacks it still gets it, so the backup mirrors the card. A copy on the backup drive does not
  count as "in the library". A member is all-or-nothing across destinations too: if one
  destination fails, the copy the other verified is removed.
- **Resume and cancel:** the job journal (`jobs`, `journal` tables) lives in `import.db`. After
  a crash or unplug, verified files are done and the rest re-queue; resume after a crash between
  two destinations' renames keeps the destination that holds matching bytes and copies only the
  missing one. Cancel leaves no temporary files.
- **`{seq}`** is committed when a job starts (`seq` table), so a cancelled import leaves a gap,
  never a repeat.
- **Threads:** I/O workers only, never the UI or render thread.
- **Priority:** a background import waits between buffers while the present loop is presenting
  (Windows: `mv_present_set_busy` from the lab; Mac: `g_present_busy` from the Metal loop; both
  stay set for 2 s after a dropped frame). Fast never waits.
- **Privacy:** hashes, paths, names and reports stay on the machine.
- **Ports:** volume arrival, eject and uncached read-back live in `io/volume_{win,mac}.cpp` and
  `io/file_port_{win,mac}.cpp` behind portable headers. Windows uses `WM_DEVICECHANGE` and
  `CM_Request_Device_Eject`; the Mac card watch uses DiskArbitration's mount callbacks (the C
  API NSWorkspace sits on, so `io/` stays free of Objective-C) and `DADiskUnmount` + eject. The
  base app's one-time card hint uses NSWorkspace itself.
- BLAKE3 is taken under **CC0** (Apache-2.0 alone does not combine with GPL-2.0).

`import.db` lives in the add-on's data folder and holds: `library`, `card_files`, `seq`,
`presets`, `card_presets`, `sources`, `seen_hashes` (Find duplicates), `jobs` and `journal`.

**Library tools (PR 19):** library-wide duplicate scope; import history; **verify a folder**
(re-hash against `import.db` to find silent corruption on an old drive, uncached reads); import
from a folder or network share as a first-class source.

**F8 across volumes** (base app, not the add-on): a move to another volume deletes the source
only after the copy is verified, through the same `io::verified_copy`
(`src/io/file_ops_win.cpp`, `src/shell/file_jobs.cpp`, `src/shell/main_mac.mm`).

## Add-ons: how Import is installed

Import was the first add-on, and its mechanism is the one every add-on uses: the AI pack
([17](17-local-ai-search.md)) installs the same way.

- **Settings → Add-ons** lists each add-on with its size, version and **Install / Remove**.
  Installing is one click, with the size shown ("Install Import, 3 MB"). The first time a card
  appears without Import installed, a **one-time**, dismissible hint offers it; after
  "Not now" it does not return. Settings reads the signed manifest when it opens and offers
  Install, with the archive's real size, only when it verifies for the running app; otherwise it
  says Import is not published yet, or needs a newer MediaViewer. The card hint appears only
  when there is something to install, and its network check waits on the automatic
  update-check switch ([13](13-updates-and-telemetry.md)).
- **Signed, verified, then loaded.** Downloads are two fixed release assets per platform,
  `mediaviewer-addon-<id>-<platform>.json` (+ `.sig`) and the `.zip` it names, fetched by the
  chrome with a plain GET: no identifier, no query, no cookies. `manifest.json` lists files,
  SHA-256 and a licence per file, and is signed with the update-manifest **Ed25519** key, which
  the core checks with libsodium (one verifier for both platforms; the key is pinned in
  `src/addon/manifest.cpp` and `UpdateKeys.cs`, and a test checks they agree). The core verifies
  the manifest before the archive is requested, the archive's size and SHA-256 before it is
  opened, and every file (and that there is nothing extra) before install **and at every load**.
  Windows binaries are Authenticode-signed when the release is. The Mac bundle is Developer
  ID-signed and notarized and loads only under library validation (same Team ID).
- **Location:** `%LocalAppData%\MediaViewer\addons\import\<version>` on Windows and
  `~/Library/Application Support/MediaViewer/Add-ons/Import/<version>` on Mac; the add-on's own
  data (`import.db`) is in `<addon>/data`, and downloads verify in `<addons>/.staging/<n>`
  (`src/addon/store.h`). Uninstalling the app removes add-ons; removing the add-on offers to
  keep or delete its data.
- **Updates:** Settings offers **Update to X** when the release channel has a newer signed
  version than the installed one; nothing downloads until it is clicked. The new version
  installs beside the running one and the store removes the old one at the next start
  (`prune.pending`), so a running add-on never loses its files. An update of a running Import or
  Local search Core therefore takes over only at the next start, so the app offers one
  (2026-10-05, owner): Settings says "Restart MediaViewer to use it" with **Restart now**, and the
  command bar shows **Add-on updated — restart** until then. Never a forced restart. The restart
  puts back the folder and file (as an app update's does); with an app update also staged it is
  that update's restart, which loads the new add-on too. Otherwise Windows starts the exe again
  with `--relaunch-after <pid>`, which waits for the old process to exit before claiming the
  single instance (`update_guard.h`), and the Mac opens the app again from a small waiter once
  the old process is gone (`-restartForAddons`). A People or Sound piece needs none: the loaded
  pack reloads it. The manifest declares the host
  API range it supports; an add-on outside that range is not loaded and the app says "Import
  needs an update". **No downgrades:** install refuses a signed manifest older than a working
  installed version; the same version again is a repair. Offline sideloading works: a folder
  dropped in is verified the same way.
- **What an add-on is, technically:**
  - **Native:** one shared library (`mv_import.dll` / `libmv_import.dylib`) exporting
    `mv_addon_get(uint32_t host_api, const mv_host_api* host, mv_addon_api* out)`. The host
    passes a **function table** (`mediaviewer_addon.h`): jobs, the folder model, thumbnails,
    metadata read, pairing, `io` ports, the verified copy, and the completion queue. The add-on
    does not link the core statically and does not reach into it (`mv_import` links SQLite
    only). Flat C, POD, status codes, correlation ids ([14](14-abi.md)). Table versions only
    append; the host serves every layout from 1 up.
  - **Windows chrome:** `MediaViewer.Import.Chrome.dll`, loaded into its own
    `AssemblyLoadContext` and given the chrome's `IAddonHost` (`IslandHost.Addons.cs`,
    `abi/addon_abi.cpp`).
  - **Mac chrome:** `Import.bundle` loaded with `NSBundle`, whose principal class returns the
    SwiftUI root view for an `NSHostingView` window (`src/shell/addons_mac.mm`,
    `AddonsView.swift`). Its `-shutdown` closes the table before the host unloads the pack:
    later calls from its detached tasks fail, and it waits (at most 2 s) for those already
    inside (a RAW thumbnail, an eject); at quit `-shutdownForQuit` does not wait, and a call
    still inside leaves the pack running for the exit, as the AI chrome's does (issue #193).
  - **The Mac add-on is universal** (arm64 + x86_64, platform `macos`): both architectures'
    trees are joined with `tools/mac/lipo_merge.py` before signing and packing.
- **Add-ons from other makers** are a second kind, with their own package, key, folder and
  store: [25-open-addons.md](25-open-addons.md). Nothing in this section changes for
  MediaViewer's own add-ons, and neither kind loads through the other's path.
- **Absent means absent.** With no add-on installed nothing under the add-ons folder is
  written, the base install tree carries none of the add-on payloads, and no Import command,
  menu or key appears. `F7`/`F8` behave as in the base app.
- **Packing:** `tools/package/addon-pack.py` (cross-checked by `tools/addon-verify`); the
  release workflow builds, signs and packs both platforms' add-on at the release version
  (`--require-pinned-key`; Mac via `macpack.py addon`, Developer ID + notarization), and
  `publish` refuses a stable release without it (`MV_RELEASE_ADDONS=1`).

### One host process

Each window is its own process (`Ctrl+N`, [16](16-commands.md) "Window"). The add-ons —
Import's volume watch, the AI pack and its indexer, Final Cut search on the Mac — load in
the first window's process only: the one that owns `Add-ons/.host.lock` (an `flock` on the
Mac) or the `Local\MediaViewer.AddonHost` mutex (Windows). Two indexers writing one index
would fight over it. The lock is the process's until it exits, crash or not. Later windows
view and browse but show no add-on features. Closing the first window does not hand the
add-ons to a window that is already open; the next window to start takes them.

## Commands

Rows in the shared command table (`src/shell/command_table.cpp`), present only while Import is
installed:

| Command | Windows | Mac |
|---|---|---|
| Open Import | `Ctrl+Shift+I` | `⌘⇧I` |
| Import marked now (last preset) | `Ctrl+Shift+F7` | `⌘⇧F7` |
| Start (Import window) | `Enter` (when no tile has focus) · `Ctrl+Enter` always | `Return` · `⌘Return` always |
| Pause / resume while copying | `Space` | `Space` |
| Open the focused tile in the viewer | `Enter` | `Return` |
| Toggle file or day | `Space` / `Shift+Space` | same |
| Next / previous source (inside the Import window only) | `Ctrl+Tab` / `Ctrl+Shift+Tab` | `⌃Tab` / `⌃⇧Tab` |
| Eject after summary | `Ctrl+J` | `⌘J` |

## Where it lives

| Piece | Code |
|---|---|
| Verified copy, BLAKE3, file / volume ports (both OSes) | `src/io/verified_copy.*`, `content_hash.*`, `file_port*.cpp`, `volume_{win,mac}.cpp` |
| F8 across volumes, verify-before-delete | `src/io/file_ops_win.cpp`, `src/shell/file_jobs.cpp`, `src/shell/main_mac.mm` |
| Add-on host: manifest check, store, loader, host table | `src/addon/`, C header `mediaviewer_addon.h` |
| The Import add-on (`mv_import`) | `src/addons/import/` (scanner, planner, naming, library index, engine), C header `mediaviewer_import.h` (`mv.import.1`) |
| Windows chrome | `IslandHost.Addons.cs` (Settings, download, hint, loading), `src.managed/MediaViewer.Import.Chrome`, `src/abi/addon_abi.cpp` |
| Mac chrome | `AddonsView.swift` (Settings, download, hint), `src.swift/ImportChrome` → `Import.bundle`, `src/shell/addons_mac.mm` |
| Packing and signing | `tools/package/addon-pack.py`, `tools/addon-verify` |
| Tests | `tests/test_import_engine.cpp`, `test_verified_copy.cpp`, `test_addon_manifest.cpp`, `test_import_naming.cpp`, `test_json.cpp`, `test_content_hash.cpp`, `tools/package/test_addon_pack.py`; the headless build is `cmake/portable` |

## Find duplicates (PR 54)

A tool in the Import window on both platforms (`DuplicatesWindow.cs`, `DuplicatesView.swift`;
engine `find_duplicates` / `trash_duplicate`, job kind 2): pick a folder, and Import finds every
set of files with identical bytes in it and every folder under it, wherever they sit and
whatever they are called.

- **What counts as a duplicate:** the same bytes. Size first; BLAKE3-256 only where two files
  share a size, the same test an import uses. Never the name, never a near-duplicate picture.
- **Every file, not only media.** Hidden and system entries and macOS packages are not walked (a
  Photos library is never opened up), links are not followed, and empty files are not compared.
- **Fast the second time.** Each hash is remembered in `import.db` (`seen_hashes`: path, size,
  mtime) and trusted while size and mtime match. A second scan of an unchanged folder reads no
  file contents; a finished scan forgets files that have gone.
- **Where it runs:** the add-on's own I/O thread, one reader per physical device (it waits
  behind an import from the same disk), yielding to the viewer like a background import. Cached
  reads: this is a comparison, not verify-a-folder's check for rot.
- **What you see:** groups, largest waste first, with the space that could be freed. Each file
  can be **opened in the viewer**, **shown in Explorer / the Finder**, or **moved to the Recycle
  Bin / Trash**. Several copies can be picked (Ctrl / Shift or ⌘ / ⇧ click) and moved in one go,
  with the count and space freed shown beside the button. A local report lists every group.
- **Deleting is safe by construction:**
  - only ever to the Recycle Bin / Trash; where a location has none (a network share, some
    removable drives), nothing is removed and the file says so. There is no permanent delete;
  - **a group is never emptied:** before the move, the engine checks the file is unchanged since
    it was hashed and re-reads another copy in the group to confirm the same bytes are still
    there. If none is, the request is refused ("the last copy is always kept");
  - only the files the person picked. A pick that takes **every** copy of some file is not sent
    at all, and says so. There is no "delete all duplicates" and no keep rule.
- **Keys:** arrows move through files (`Shift`+arrows / `Ctrl+Space`, `⇧`-arrows on the Mac,
  pick several), `Enter` opens one in the viewer, `Delete` / `⌘⌫` moves the picked copies to the
  bin, `Ctrl+E` / `⌘R` shows it in Explorer / the Finder, `Esc` closes (a running scan carries
  on, with a line in the command bar).
- **Privacy:** paths and hashes stay in `import.db` and the local report.

## Known gaps

- **Clip tiles show ▶ without a poster** in the Import grid unless the viewer's thumbnail cache
  already holds one: the host's thumbnail service makes stills only (a poster needs the player).
- **The Mac finish notification is a beep** (`NSBeep` in `addons_mac.mm`); the window's
  summary is the notification. Windows uses an app notification where the unpackaged app has an
  AUMID, else the window's summary.

## Not in Import

Any cloud or upload; a catalogue, albums or keywords database; near-duplicate or burst
detection; writing copyright or other metadata on import; deleting from or formatting cards;
tethered capture; video transcoding on import.
