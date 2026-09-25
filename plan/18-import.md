# 18 — Import (an installable add-on)

Copy a card or a folder into a photo library: skip what is already there, check every copy,
sort by date on the way in, and never lose a file. **An optional add-on, installed from
Settings.** The base viewer, its installer and its updates never carry it. Built on
**Windows and macOS together**, like every PR from 9 (D9, amended 2026-09-24).

Roadmap slices: **PR 16–19** ([10-roadmap.md](10-roadmap.md#milestone-g--import-add-on-pr-1619-both-platforms)).
Planned 2026-09-24 ([12](12-decision-log.md)). This is not a D-decision.

## What it is, honestly

**It does not move bytes faster than the hardware.** A card reader, a USB link and an SSD
set the speed, and Explorer and Finder already copy close to it. Import is quicker where the
time is actually lost:

- **copying less:** files already in the library are skipped by content, not by name;
- **no second pass:** each file is checked as it is copied, not in a separate re-read of the card;
- **no babysitting:** every decision is made before the copy starts, so it never stops
  halfway to ask "Replace or skip?";
- **no sorting afterwards:** files land in dated folders, with pairs kept together;
- **no guessing:** you see what is new on the card before copying, and get a report after.

## Compared with Explorer and Finder

| | Explorer / Finder copy | Import |
|---|---|---|
| Already-copied files | Name clash dialog, mid-copy; a renamed duplicate is copied again | Skipped by **content hash** (size, then BLAKE3). Renamed duplicates are caught. Same name with different bytes is kept under a safe name. Decided up front, never mid-copy |
| "What's new on this card?" | You work it out | **New since last import** badge per file and per day; default selection is "new only" |
| Is the copy good? | Not checked | **Every file verified**: hashed while read, read back from the destination uncached, compared. A bad copy is retried, then reported, never counted as done |
| Move to another drive | Deletes the source after copying, unchecked | Deletes the source **only after verify**. Import never deletes from, formats or erases a card |
| Card pulled / crash mid-copy | Partial file left; restart from scratch | No partial files (temp + rename). **Resumes**: already verified files are not re-copied |
| Backup at the same time | A second copy job, reading the card twice | **Second destination** from the same read, both verified |
| Where files go | One folder you picked | **Layouts** (date taken, camera, type, keep card structure, flat) with a live preview of the resulting folders |
| Names | Camera names (`IMG_0001` repeats every 10 000 shots) | Optional **rename templates** with a live preview |
| RAW+JPEG, Live Photos, camera sidecars | Copied as loose files; may be split | Moved as **one unit**: pairs, `.xmp`, `.THM`, `.LRV`, Sony `.XML` travel with their file |
| Preview before copying | Slow thumbnails; RAW needs codecs on Windows | The viewer's own thumbnails: embedded RAW previews, HEIC, video posters, no codec packs |
| Two cards at once | Two jobs that fight over one disk | One reader per card, one writer per disk, overlapped. Cards in different readers run together |
| Viewer while copying | n/a | Keeps panning at refresh: Import yields to the render thread (both present-loop gates hold while importing) |
| Afterwards | Nothing | Summary (copied / skipped duplicates / failed), **Eject**, **Open in viewer**, a saved local report |

## Painless by default

Plug in a card, press **Import**. The defaults, which a new user never needs to change:

- source: the card that just appeared; selection: **new files only**;
- destination: the last one used, else `Pictures\MediaViewer` / `~/Pictures/MediaViewer`;
- layout: `YYYY/YYYY-MM-DD`; names unchanged; duplicates skipped; full verify; eject when done.

One screen, one button, no prompts while copying. The first import asks for the destination
once. Everything else is optional.

## The Import window

A separate window on both platforms, so viewing carries on while it copies: WinUI 3 on
Windows, SwiftUI on Mac. **Keyboard-complete**, like the rest of the app
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
│  Imports…          │  RAW+JPEG = one tile · LIVE badge · ▶ video poster       │                    │
├────────────────────┴──────────────────────────────────────────────────────────┴────────────────────┤
│ 812 files · 38.4 GB · 392 duplicates skipped · ≈ 6 min at 110 MB/s        [ Import 812 ]          │
└─────────────────────────────────────────────────────────────────────────────────────────────────────┘
```

- **Sources** (left): cards and drives as they appear, each with a "N new" count. Folders can
  be added. A network share works but is labelled slower.
- **Contents** (centre): the viewer's gallery grid, grouped by day, with a checkbox per day and
  per file. Files already imported are dimmed. RAW+JPEG and Live Photo pairs are one tile. `Space`
  toggles, `Enter` opens the file in the main viewer (culling before copying), and viewer marks
  carry over.
- **Preset** (right): destination, backup, layout, rename, and a **Where files go** preview that
  updates as settings change. Nothing is hidden behind an Advanced button that decides where
  files land.
- **Bottom bar:** count, size, duplicates skipped, and an ETA from measured throughput, not a
  guess. One primary button.
- **While copying:** the same window becomes a progress view. It shows per-destination bars,
  the current file, MB/s, verified so far, **Pause** and **Cancel**. Closing the window keeps the
  job running, with a small progress indicator in the main command bar. A system notification
  arrives when it finishes.
- **Summary:** copied / skipped (with *what each matched*) / failed (with the reason, and **Retry
  failed**). **Eject**, **Open in viewer** (opens the destination folder), **Show report**.

## Configurability

All of it is saved in **named presets** ("Daily", "Wedding: to NAS + backup"). A preset can be
bound to a card, so that card opens with it selected.

| Setting | Options | Default |
|---|---|---|
| Selection | New since last import · all · marked in viewer · date range · type filter (RAW / JPEG / HEIC / video / other) | New only |
| Destination | Any folder or drive; network shares labelled slower | Last used |
| Backup destination | Off · a second folder or drive, written from the same read | Off |
| Layout | `YYYY/YYYY-MM-DD` · `YYYY/MM/DD` · `YYYY-MM-DD` · + camera model · + type subfolders (`RAW`/`JPEG`/`Video`) · keep card structure · flat | `YYYY/YYYY-MM-DD` |
| Date source | Date taken (EXIF / container) → file time fallback, labelled · file time only | Date taken |
| Rename | Off · template of `{date}` `{time}` `{camera}` `{seq}` `{original}` `{ext}` with a live preview; `{seq}` is per-day and survives re-imports | Off |
| Duplicates | Skip content duplicates · import anyway (safe name) · scope: this destination / the whole library index | Skip, this destination |
| Name clash, different bytes | Keep both (safe name). **Overwrite is not offered** | Keep both |
| Verify | Full read-back · hash-on-read only (network targets) | Full |
| Companions | Keep camera sidecars with their file (`.xmp` `.THM` `.LRV` `.XML`) | On |
| After import | Eject card · open destination in viewer · notify · nothing | Eject + notify |
| On card insert | Do nothing · open Import · **auto-import with this preset** (per card, opt-in, remembered by volume id) | Open Import |
| Priority | Background (viewer stays smooth) · fast (while the viewer is idle) | Background |

Never offered at any setting: deleting from the card, formatting a card, overwriting a
destination file, or any upload.

## The engine (shared core)

- **Duplicate test:** size first; hash (BLAKE3-256) only when a size matches. **Never by name.**
  The library index (`import.db`, SQLite: volume, path, size, mtime → hash) makes a second
  import cost a size lookup. Stale rows are rechecked when size or mtime differ.
- **Per-card memory:** after each import, the card's volume id plus each file's (path, size,
  mtime, hash) are recorded. "New since last import" is a lookup, not a re-hash of the card.
- **Verify:** hash while reading the source (one pass over the card). Write to `name.tmp`,
  flush (`FlushFileBuffers` / `F_FULLFSYNC`), read back uncached (`FILE_FLAG_NO_BUFFERING`
  / `F_NOCACHE`), compare, then rename into place. On mismatch: delete the temp file, retry
  once, then fail that file.
- **Throughput:** one reader thread per physical source, one writer per physical destination,
  and 2–4 large buffers in flight between them. Reads and writes overlap. **No parallel reads
  of one card** (it slows a card down). Two sources on different devices run concurrently.
  Measured MB/s drives the ETA.
- **Units:** RAW+JPEG and Live Photo pairs (PR 7 pairing) and camera sidecars are one unit,
  copied, verified, skipped and sorted together, never split across dated folders.
- **Resume and cancel:** the job journal lives in `import.db`. After a crash or unplug, verified
  files are done and the rest re-queue. Cancel leaves no temp files.
- **Threads:** I/O workers only. **Never the UI or render thread** (rule 1). Background
  priority yields between buffers whenever the present loop needs the CPU or the disk.
- **Privacy:** hashes, paths, names and reports stay on the machine (rule 6). If telemetry is
  on at all, Import reports only counts and throughput, never names.
- **Ports:** volume arrival (`WM_DEVICECHANGE` on Windows, `NSWorkspace` mount notifications
  on Mac), eject (`CM_Request_Device_Eject` / `DADiskUnmount` + eject), and uncached read-back
  live in `io/*_win.cpp` / `io/*_mac.cpp` behind a portable header (D9).
- BLAKE3 is taken under **CC0** (Apache-2.0 alone does not combine with GPL-2.0).

## Add-ons: how Import is installed

Import is the **first add-on**, so it builds the add-on mechanism that later add-ons reuse:
the AI pack ([17](17-local-ai-search.md), PR 20) and Voice ([19](19-voice.md), PR 27).
The design follows the AI pack's delivery rules:

- **Settings → Add-ons** lists each add-on with its size, version and **Install / Remove**.
  Installing is one click, with the size shown ("Install Import, 3 MB"). The first time a card
  appears without Import installed, a **one-time**, dismissible hint offers it. It never
  appears again after "Not now".
- **Signed, verified, then loaded.** The download is a plain GET of a fixed URL on the same
  release channel as updates ([13](13-updates-and-telemetry.md)): no identifier, no path, no
  telemetry. `manifest.json` lists files, SHA-256 and a licence per file. It is signed with
  the update-manifest key. Windows binaries are Authenticode-signed when the release is. The
  Mac bundle is Developer ID-signed and notarized, and loads only under library validation
  (same Team ID).
- **Location:** `%LocalAppData%\MediaViewer\addons\import\<version>` on Windows and
  `~/Library/Application Support/MediaViewer/Add-ons/Import/<version>` on Mac. Per-user,
  versioned folders, like the app itself. Uninstalling the app removes add-ons; removing the
  add-on offers to keep or delete `import.db`.
- **Updates:** an installed add-on updates silently with the app. The manifest declares the
  host API range it supports. An add-on outside that range is not loaded, and the app says
  "Import needs an update" rather than crashing. Offline sideloading works: drop the folder
  in, and it is verified the same way.
- **What an add-on is, technically:**
  - **Native:** one shared library (`mv_import.dll` / `libmv_import.dylib`) exporting
    `mv_addon_get(uint32_t host_api, const mv_host_api* host, mv_addon_api* out)`. The host
    passes a **function table**: jobs, the folder model, thumbnails, metadata read, pairing,
    `io` ports, and the completion queue. The add-on does not link the core statically and does
    not reach into it. Flat C, POD, status codes, correlation ids ([14](14-abi.md)).
  - **Windows chrome:** `MediaViewer.Import.Chrome.dll`, loaded into its own
    `AssemblyLoadContext` and given the chrome's `IAddonHost`.
  - **Mac chrome:** `Import.bundle` loaded with `NSBundle`, whose principal class returns the
    SwiftUI root view for an `NSHostingView` window.
- **Absent means absent.** With no add-on installed, the base install tree is
  **byte-identical** to one built without the add-on system's payloads. No Import command,
  menu or key appears. `F7`/`F8` behave as in the base app.

**Stays in the base app regardless** (it is a safety fix, not a feature): `F8` across volumes
deletes the source only after the copy is verified.

## Commands

New rows in the shared command table, with a Mac default map in the same PR
([16-commands.md](16-commands.md)). They exist only while Import is installed:

| Command | Windows | Mac |
|---|---|---|
| Open Import | `Ctrl+Shift+I` | `⌘⇧I` |
| Import selected / marked now (last preset) | `Ctrl+Shift+F7` | `⌘⇧F7` |
| Start / pause / resume (Import window) | `Enter` / `Space` | `Return` / `Space` |
| Toggle file or day | `Space` / `Shift+Space` | same |
| Next / previous source (inside the Import window only; `Ctrl+Tab` stays "next tab" elsewhere) | `Ctrl+Tab` / `Ctrl+Shift+Tab` | `⌃Tab` / `⌃⇧Tab` |
| Eject after summary | `Ctrl+J` | `⌘J` |

Key assignment is checked against the live table when the PR lands. If a key is taken, the
table wins and this list is updated.

## Roadmap slices (both platforms each)

### PR 16 — Add-on mechanism + import engine
Add-on manifest, signing, download, verify, install/remove, host function table, and
Settings → Add-ons, on both platforms. The engine: duplicate test, library index, verify,
overlap, units, resume, cancel, and the `io` ports. The base-app `F8` verify-before-delete.
UI: a **minimal** Import sheet (source, destination, Go, result). The full window is PR 17.

**Verify (both platforms):**
- The base install tree is byte-identical with the add-on absent, and no Import command
  exists. A tampered add-on file or manifest is refused. The download request carries no
  identifier.
- A 64 GB mixed card imported to an empty folder: every destination hash matches its source,
  and the time is within 10 % of the OS copy of the same set to the same drive.
- Re-import: **zero bytes written**, and the destination is not re-read. Renamed files on the
  card are still skipped. One byte changed in a same-named JPEG: it is copied under a safe
  name.
- Fault injection in the writer is detected, retried, and reported. Pulling the card or the
  destination mid-job, then resuming, re-copies only unverified files. `F8` never removes an
  unverified source.
- **Both present-loop gates hold while importing.**

### PR 17 — The Import window
The full window above: sources with new counts, the day-grouped grid with dimmed already
imported files, the preset panel with the **Where files go** preview, ETA, progress,
pause/cancel, background running with a command-bar indicator, the summary with Retry /
Eject / Open, the report, the notification, the one-time card hint, and keyboard-complete
commands.

**Verify (both platforms):** insert a card → the window opens with only new files selected
→ one key imports them → eject. No mouse, no prompt during the copy. The grid scrolls a
2,000-file card without a hitch. Closing the window mid-import keeps it running, and the
indicator shows progress. The ETA after 10 s is within 20 % of the actual time.

### PR 18 — Presets, backup, layouts, rename
Named presets, per-card presets, auto-import on insert (opt-in per card), second
destination from one read, all layouts, date-source fallback, rename templates with `{seq}`,
camera sidecars as units, type filters and date ranges.

**Verify (both platforms):** one card read produces two verified copies (the card is read once).
A rename template yields identical names on Windows and Mac for the same card. Auto-import
fires only for the card it was enabled on, and never deletes. The layout preview matches what
lands on disk, file for file.

### PR 19 — Library tools
Library-wide duplicate scope, import history (what came from which card, when), and **verify a
folder**: re-hash against `import.db` to find silent corruption on an old drive. Import from a
folder or network share treated as a first-class source.

**Verify (both platforms):** importing a card into a library that holds its files in other
folders skips them with library scope on. Verify-a-folder flags a file flipped by one bit on
disk and passes an untouched one.

## Implementation notes (2026-09-24)

Built on a branch ahead of PRs 11–15 (roadmap state in [10](10-roadmap.md#milestone-g--import-add-on-pr-1619-both-platforms)).
Where it lives:

| Piece | Code |
|---|---|
| Verified copy, BLAKE3, file / volume ports (both OSes) | `src/io/verified_copy.*`, `content_hash.*`, `file_port*.cpp`, `volume_{win,mac}.cpp` |
| F8 across volumes, verify-before-delete | `src/io/file_ops_win.cpp`, `src/shell/main_mac.mm` |
| Add-on host: manifest check, store, loader, host table | `src/addon/`, C header `mediaviewer_addon.h` |
| The Import add-on (`mv_import`, links SQLite only) | `src/addons/import/`, C header `mediaviewer_import.h` (`mv.import.1`) |
| Windows chrome | `IslandHost.Addons.cs` (Settings, download, hint, loading), `MediaViewer.Import.Chrome` (the window), `abi/addon_abi.cpp` |
| Mac chrome | `AddonsView.swift` (Settings, download, hint), `src.swift/ImportChrome` → `Import.bundle`, `src/shell/addons_mac.mm` |
| Packing and signing | `tools/package/addon-pack.py`, cross-checked by `tools/addon-verify` |
| Tests | `tests/test_import_engine.cpp`, `test_verified_copy.cpp`, `test_addon_manifest.cpp`, `test_import_naming.cpp`, `test_json.cpp`, `test_content_hash.cpp`, `tools/package/test_addon_pack.py`; the headless build is `cmake/portable` |

Calls made while building it (none reverses a D-decision; logged in [12](12-decision-log.md)):

- **One signing key for add-ons on both platforms: the update-manifest Ed25519 key.** The C++ core
  checks it (libsodium), so Windows and Mac share one verifier; the Mac bundle is additionally
  Developer ID-signed for library validation. The same key is pinned in `src/addon/manifest.cpp`
  and `UpdateKeys.cs`; a test checks they agree.
- **Downloads are two fixed release assets per platform** — `mediaviewer-addon-import-<platform>.json`
  (+ `.sig`) and the `.zip` it names — fetched by the chrome; the core verifies the manifest before
  the archive is requested, the archive's size and SHA-256 before it is opened, and every file (and
  that there is nothing extra) before install **and at every load**.
- **Card memory and the library index key on the volume-relative path**, so "new since last import"
  holds whether the card is scanned from its root or a subfolder.
- **A unit is all-or-nothing.** If one member fails, the members already written for that unit are
  removed and the whole unit is reported. A crash between members is resumable: verified members
  stay done.
- **Type folders keep units together**: a RAW+JPEG pair goes under `RAW`, a clip under `Video`.
- **`{seq}` is committed when a job starts**, so a cancelled import leaves a gap, never a repeat.
- **"One writer per physical destination"** is a per-device lock held for each file; two cards to one
  disk alternate file by file instead of interleaving writes.
- **Interrupted, not failed**: when a copy fails and the source or destination root is gone, the job
  stops as *interrupted* with the rest pending; the app lists it at next open with **Resume**.
- **Temporaries are `<final>.mvtmp`** (then `.mvtmp2` …), removed on cancel, failure and resume.
- **Duplicates are decided per destination.** A unit whose content the main destination (or, at
  library scope, the library) already holds is skipped there and reported with what it matched,
  but a backup destination that lacks it still gets it, so the backup mirrors the card. It follows
  the selection rules as if it were not a duplicate. A copy on the backup drive does not count as
  "in the library" for the main destination.
- **A member is all-or-nothing across destinations too.** If one destination fails, the copy the
  other destination verified is removed; resume after a crash between the two renames keeps the
  destination that holds matching bytes and copies only the missing one.
- **No add-on downgrades.** Install refuses a signed manifest older than a working installed
  version; the same version again is a repair. An installed copy that is tampered or needs a newer
  app does not block an older one.
- **The Mac card watch uses DiskArbitration's mount callbacks** (the C API NSWorkspace sits on) so
  `io/` stays free of Objective-C; the base app's one-time hint uses NSWorkspace itself.
- **Key clash in the window:** plan text gives Enter to both "start" and "open in viewer". Enter
  starts unless a tile has focus (then it opens that file); Ctrl+Enter / ⌘Return always starts.
- **The Mac add-on is universal** (arm64 + x86_64, platform `macos`), following D9's 2026-09-24
  amendment for the app: both architectures' `addons/import` trees are joined with
  `tools/mac/lipo_merge.py` before signing and packing. Like the app on Intel, it builds but is
  unmeasured there.
- **Priority:** background waits between buffers while the present loop is presenting (Windows
  `mv_present_set_busy` from the lab, Mac `g_present_busy` from the Metal loop); fast never waits.

Known gaps against this spec, owed before it merges:

- **Clip tiles show ▶ without a poster** in the Import grid (thumbnails come from the stills cache;
  posters need the player, which the add-on host does not reach yet).
- **Mac finish notification is a beep** until the app asks for notification permission
  (UNUserNotificationCenter); Windows uses an app notification where the unpackaged app has an
  AUMID, else the window's summary.
- **The Import window is not yet measured** at 2,000 files; the grid uses virtualising containers
  (GridView / LazyVGrid) and lazy thumbnails, but "scrolls without a hitch" is a hardware line.

PR 16–19 verify lines, where each stands:

| Verify line | State |
|---|---|
| Base install byte-identical with the add-on absent; no Import command | By construction (the add-on is a separate artefact; commands gated) and unit-tested (store writes nothing, commands absent from the table); the install-tree diff is a release-machine check |
| Tampered file or manifest refused; request carries no identifier | Tested in C++ and against the Python packer; the GET is fixed URLs with no query or cookies |
| Every destination hash matches; time within 10 % of the OS copy (64 GB) | Hash: tested. Timing: **hardware** |
| Re-import writes zero bytes, destination not re-read; renamed files skipped; one changed byte → safe name | Tested (copy and hash counters) |
| Fault injection detected, retried, reported; unplug + resume re-copies only unverified; F8 never removes an unverified source | Tested (fault hook, card pulled mid-job); F8 path is the same verified copy. Real unplug: **hardware** |
| Both present-loop gates hold while importing | **Hardware**, both platforms |
| Insert → window → one key imports → eject; closing keeps it running; ETA within 20 % at 10 s | Written on both hosts; **hardware** |
| One card read → two verified copies | Tested (one copy call and one read per file) |
| Rename identical on Windows and Mac; auto-import only for its card, never deletes; preview matches disk | Tested (pure naming functions; auto-import; preview vs files on disk) |
| Library scope skips files kept elsewhere; verify-a-folder flags a one-bit flip | Tested |

## Not in Import

Any cloud or upload; a catalogue, albums or keywords database; near-duplicate or burst
detection (the backlog's burst-stack grouping); writing copyright or other metadata on
import (metadata writes stay PR 12's narrow set); deleting from or formatting cards;
tethered capture; video transcoding on import.
