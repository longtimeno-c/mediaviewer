# 18 — Import (an installable add-on)

How the Import add-on copies a card or folder into a photo library, and how the add-on
mechanism (manifest, signing, host function table, install) works for every first-party add-on.
Import is optional, installed from Settings; the base app does not carry it. One engine
(`src/addons/import`) with a WinUI and a SwiftUI window.

## What it is, honestly

Import does not copy faster than the hardware. It saves time by copying less (duplicates
skipped by content hash, renamed ones too), verifying each file as it copies, deciding
everything up front (no mid-copy prompts), moving pairs and sidecars as one unit into dated
folders, resuming after a crash, writing a backup from the same read, and never deleting from,
formatting or erasing a card.

## Painless by default

Source: the card that just appeared; selection: new only; destination: last used, else
`Pictures\MediaViewer` / `~/Pictures/MediaViewer`; layout `YYYY/YYYY-MM-DD`; names unchanged;
duplicates skipped; full verify; eject when done. The first import asks for the destination once.

## The Import window

A separate window (WinUI `MediaViewer.Import.Chrome`; SwiftUI `Import.bundle`), keyboard-complete
([16](16-commands.md)).

- **Sources** (left): cards, drives, added folders and shares, each with an "N new" count.
- **Contents** (centre): virtualised grid grouped by day; imported files dimmed; pairs one tile.
  `Space` toggles; `Enter` opens the tile in the viewer.
- **Preset** (right): **Where to**, **What to import**, **How to organise** (with the **Where
  files go** preview), backup and eject; the rest under **More options**.
- **Tools** menu: Past imports, Check a folder for damaged files, Find duplicates, Check a folder
  against the library, About Import.
- **Bottom bar:** selection, skips, ETA from measured per-device MB/s. *Import N* is disabled
  until the plan on screen is current.
- **While copying / after:** progress with **Pause** / **Cancel** (closing the window keeps the
  job running); a summary with **Retry failed**, **Eject**, **Open in viewer**, **Show report**.
  A job whose source or destination vanished stops *interrupted* and offers **Resume** next open.

## Configurability

Settings live in **named presets** (`presets` table; `mv::import::preset` in
`src/addons/import/model.h`, strict JSON: unknown keys ignored, a mistyped known key fails). A
preset can be bound to a card by volume id.

| Setting | Options | Default |
|---|---|---|
| Selection | New since last import · all · marked in viewer · date range · type filter (RAW / JPEG / HEIC / video / other) | New only |
| Destination | Any folder or drive | Last used |
| Backup destination | Off · second folder/drive, same read | Off |
| Layout | `YYYY/YYYY-MM-DD` · `YYYY/MM/DD` · `YYYY-MM-DD` · + camera · + type subfolders (`RAW`/`JPEG`/`Video`) · keep card structure · flat | `YYYY/YYYY-MM-DD` |
| Date source | Date taken (EXIF / container) → file time fallback, labelled · file time only | Date taken |
| Rename | Off · template of `{date}` `{time}` `{camera}` `{seq}` `{original}` `{ext}`; `{seq}` per day, survives re-imports | Off |
| Duplicates | Skip · import anyway (safe name) · scope: this destination / whole library index | Skip, this destination |
| Name clash, different bytes | Keep both (safe name) | Keep both |
| Verify | Full read-back · hash-on-read only | Full |
| Companions | Keep camera sidecars with their file | On |
| After import | Eject card · open destination in viewer · notify · nothing | Eject + notify |
| On card insert | Nothing · open Import · auto-import with this preset (per card, opt-in) | Open Import |
| Priority | Background · fast | Background |

**Never offered:** deleting from or formatting a card, overwriting a destination file, uploading.

## The engine

`src/addons/import/engine.*`, behind `mv.import.1` (`mediaviewer_import.h`). One control thread
runs scans, plans and card arrivals in order; each job has its own thread. I/O workers only.

- **Duplicate test:** size first; BLAKE3-256 only when a size matches; never by name. The library
  index (`import.db` table `library`: destination, relative path, size, mtime → hash) makes a
  re-import a size lookup; a row is rechecked when size or mtime differ.
- **Per-card memory** (`card_files`): volume id plus (volume-relative path, size, mtime, hash) per
  file, so "new since last import" is a lookup whether the card is scanned from root or subfolder.
- **Destination identity:** `library`, `seq` and `journal` name a destination by a volume key
  `vol:<identity><US><folder inside the volume>`; journal targets are relative to it
  (`dest_locator`, `src/addons/import/host.h`). A remount, moved drive letter or `E:\` vs
  `\\nas\media` is the same destination. Identity: a local volume's `volume_id` (UUID, or Windows
  serial without capacity); a share's `device_key` folded by `share_identity` (Mac mount source
  with user, no Bonjour name; Windows UNC from `WNetGetConnectionW`). A key is issued only if it
  survives a remount and resolves through the volume list; otherwise the row keeps the absolute
  path. Absolute rows migrate to keys only when one of 32 evenly spread indexed files is really at
  its recorded path and size. Two server names or users for one share are two destinations.
- **Offline destinations:** an unmounted destination resolves to nothing: its rows stay, a plan
  cannot confirm a duplicate there, verify-a-folder skips it, Resume / Retry leave the job
  interrupted (also when two disks answer to one key). A row is dropped only when its folder
  answers and the file is not in it.
- **Verify** (`io::verified_copy`): hash while reading the source; write `<final>.mvtmp` (then
  `.mvtmp2` …); flush (`FlushFileBuffers` / `F_FULLFSYNC`); read back uncached
  (`FILE_FLAG_NO_BUFFERING` / `F_NOCACHE`); compare; rename into place without replacing. On
  mismatch delete the temp, retry once, then fail. On a share the read-back proves what the
  server returns (possibly its RAM).
- **Throughput:** one reader per physical source, one writer per physical destination (a
  per-device lock per file), 2–4 large buffers in flight; reads and writes overlap. No parallel
  reads of one card. A network destination uses the deep path ([24](24-transfer.md)): depth 8,
  2 MiB chunks, pipelined read-back.
- **Units:** pairs and sidecars are copied, verified, skipped and sorted together; type folders
  keep them together (RAW+JPEG under `RAW`). All-or-nothing: a failed member removes the unit's
  written members.
- **Backup:** duplicates are decided per destination, so a backup lacking a unit still gets it;
  a backup copy does not count as "in the library". All-or-nothing across destinations.
- **Resume and cancel:** `jobs` / `journal` in `import.db`, written in one transaction before the
  job starts (failure → job fails unstarted). After a crash, verified files are done and the rest
  re-queue; cancel leaves no temp files.
- **`{seq}`** is committed at job start (`seq` table): a cancel leaves a gap, never a repeat.
- **Priority:** background waits between buffers while the present loop presents (Windows
  `mv_present_set_busy`, Mac `g_present_busy`; both held 2 s after a dropped frame). Fast never
  waits.
- **Privacy:** hashes, paths, names and reports stay on the machine.
- **Ports:** `io/volume_{win,mac}.cpp`, `io/file_port_{win,mac}.cpp` (Windows
  `WM_DEVICECHANGE`, `CM_Request_Device_Eject`; Mac DiskArbitration, `DADiskUnmount`). BLAKE3 is
  taken under **CC0**.

`import.db` (the add-on's data folder) tables: `library`, `card_files`, `seq`, `presets`,
`card_presets`, `sources`, `seen_hashes`, `jobs`, `journal`.

**Library tools:** library-wide duplicate scope; import history; **verify a folder** (re-hash
against `import.db`, uncached, to find rot); folders and shares as sources.

**F8 across volumes** (base app): deletes the source only after `io::verified_copy` verifies
(`src/io/file_ops_win.cpp`, `src/shell/file_jobs.cpp`, `src/shell/main_mac.mm`).

## Add-ons: how Import is installed

Every first-party add-on (Import, the AI pack [17](17-local-ai-search.md), Places, Driftwire)
uses this mechanism.

- **Settings → Add-ons** is a short list: Import, Local search, Places, Driftwire, Enhance, Phone access, other own
  add-ons, then **From other makers**. Each row is name + state (*Installed · 1.2.0*, *Update
  available · 1.3.0*, *Restart to finish updating*, *Not installed*, *Needs an update*,
  *Couldn't start*, *Installing…*). A row opens the add-on's page: actions (Install / Update to X
  / Open / Remove… / Remove and install again), the install bar, and its settings (manifest, or a
  screen the add-on draws, [25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)).
  Code: `IslandHost.AddonList.cs`, `AddonListView.swift`; words in
  `src.managed/Shared/AddonListWords.cs` and its Swift copy (Chrome.Tests keeps them equal).
  Install is offered, with the real size, only when the signed manifest verifies for the running
  app. A **one-time** hint offers Import when a card first appears (its network check obeys the
  automatic update-check switch, [13](13-updates-and-telemetry.md)).
- **Installing** is one flow on both platforms: **check** (signed manifest) → **download** →
  **verify** (size, SHA-256) → **install** (store checks every file) → **attach live** or the
  **restart card** (see Updates). Windows: `RunInstall` in `IslandHost.InstallFlow.cs`. A failure
  says what happened in plain words with one next step: **Retry**, or **Remove and install
  again** when the copy on disk is unusable (keeps data). A working copy whose update failed is
  never removed. Details go to the log, never a message that could hold a path. Words:
  `src.managed/Shared/InstallWords.cs`, `InstallWords.swift`.
- **A damaged copy repairs itself** (Windows; slot add-ons). When the load refuses an installed
  copy's files (one missing or changed: antivirus quarantining a bundled `.exe` is the case seen),
  the chrome installs it again over the damaged copy, once per process, only while the automatic
  update check is on, keeping data (`AddonRepair`, `RepairDamagedSlotAddons`). If the fresh copy
  breaks again, or its files do not verify as they land, it does not loop: the card says the copy
  keeps losing its files and that security software is the usual cause. The Mac still offers only
  the Reinstall button.
- **Signed, verified, then loaded.** Two fixed release assets per platform:
  `mediaviewer-addon-<id>-<platform>.json` (+ `.sig`) and the `.zip` it names, fetched by plain GET
  (no identifier, query or cookies). `manifest.json` lists files, SHA-256 and a licence per file,
  signed with the update-manifest **Ed25519** key, checked by the core with libsodium (key pinned
  in `src/addon/manifest.cpp` and `UpdateKeys.cs`; a test checks they agree). Order: manifest
  before the archive is requested; archive size + SHA-256 before it is opened; every file and
  nothing extra before install **and at every load**. Windows binaries Authenticode-signed; the
  Mac bundle Developer ID-signed, notarized, loaded under library validation (same Team ID).
- **Remote files:** a manifest may list `remote` files (URL, path, size, SHA-256, licence):
  pinned `https://huggingface.co/` files the add-on downloads into `data` and checks itself.
- **Location:** `%LocalAppData%\MediaViewer\addons\import\<version>` /
  `~/Library/Application Support/MediaViewer/Add-ons/Import/<version>`; data in `<addon>/data`;
  downloads verify in `<addons>/.staging/<n>` (`src/addon/store.h`). Removing offers to keep or
  delete data; delete first calls optional `mv_addon_forget` ([14](14-abi.md)).
- **Updates:** **Update to X** when the channel has a newer signed version; nothing downloads
  until clicked. With automatic update checks on, channels are also asked in the background (30 s
  after the first window reads its add-ons, then daily, on a worker) and a newer version shows
  **Add-on update available** in the command bar. The new version installs beside the old; the
  store prunes the old at next start (`prune.pending`). Then:
  - **Reloaded live:** a slot add-on (Places, Driftwire) is unloaded and reloaded; an AI piece is
    picked up by the pack. A posted `busy_post` makes the reload wait.
  - **Restart card:** a running Import or Local search Core takes over only at next start: a card
    with **Later** / **Restart now** (several updates, one card). Never forced.
  - **Restart now never cuts work off:** it waits for an import, any `busy_post`, indexing and
    downloads, offering **Cancel** / **Restart anyway**. Words: `src.managed/Shared/RestartWords.cs`,
    `RestartNoticeView.swift`; `MV_ADDON_RESTART_SELFTEST` shows the card. The restart restores
    folder and file (Windows `--relaunch-after <pid>`, `update_guard.h`; Mac
    `-restartForAddons`).

  `busy_post(host, what)` (host table): `what` (≤ 120 bytes, never a path or name) completes
  "Waiting for … to finish"; "" ends it; listed in `mv_slots_notices_json`'s `busy`. The manifest
  declares its host API range; outside it the add-on is not loaded ("Import needs an update").
  **No downgrades:** an older signed manifest than a working install is refused; the same
  version is a repair. A sideloaded folder is verified the same way.
- **What an add-on is, technically:**
  - **Native:** one shared library (`mv_import.dll` / `libmv_import.dylib`) exporting
    `mv_addon_get(uint32_t host_api, const mv_host_api* host, mv_addon_api* out)`. The host passes
    a **function table** (`mediaviewer_addon.h`): jobs, folder model, thumbnails, metadata read,
    pairing, `io` ports, verified copy, completion queue. The add-on
    does not link the core statically and does not reach into it — add-ons link nothing of the core (`mv_import` links
    SQLite only). Flat C, POD, status codes, correlation ids ([14](14-abi.md)). Table versions only
    append; the host serves every layout from 1 up.
  - **Windows chrome:** `MediaViewer.Import.Chrome.dll` in its own `AssemblyLoadContext`, given
    `IAddonHost` (`IslandHost.Addons.cs`, `abi/addon_abi.cpp`).
  - **Mac chrome:** `Import.bundle` loaded with `NSBundle`; its principal class returns the
    SwiftUI root view (`src/shell/addons_mac.mm`, `AddonsView.swift`). The Mac add-on is
    universal (`tools/mac/lipo_merge.py`).
- **Screens and slots (host table v3):** an add-on whose manifest names screens or slots answers
  through `mv.slots.1` and posts back through the v3 rows (`screen_post`, `screen_diff`,
  `surface_post`, `search_post`, `search_progress`, `is_stale`, `badges_post`, `status_post`,
  `screen_show`, `location_of`, `locations_in`); `loaded_addon::load` attaches its contributions
  and its destructor detaches them. It needs no chrome of its own and carries its own code and
  data. Windows loads it with `mv_addon_load(…, MV_SLOTS_INTERFACE)`. Details:
  [25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11).
- **Add-ons from other makers** have their own package, key, folder and store
  ([25](25-open-addons.md)); neither kind loads through the other's path. Their scripts never see
  the host function table: they run in the Lua sandbox
  ([25 §9](25-open-addons.md#9-code-how-a-strangers-add-on-runs)) in every window's process.
- **Absent means absent.** With no add-on installed nothing under the add-ons folder is written,
  the base install carries no add-on payload, and no add-on command, menu or key appears.
- **Packing:** `tools/package/addon-pack.py` (cross-checked by `tools/addon-verify`); the release
  workflow builds, signs and packs both platforms (`--require-pinned-key`; Mac via
  `macpack.py addon`), and `publish` refuses a stable release without Import
  (`MV_RELEASE_ADDONS=1`).

### One host process

Each window is its own process. Add-ons (Import's volume watch, the AI pack and indexer, Final
Cut search) load only in the process owning `Add-ons/.host.lock` (`flock`, Mac) or the
`Local\MediaViewer.AddonHost` mutex (Windows), held until exit; the next window to start takes it
over, not one already open. Slot-only add-ons (Places) write nothing, so every window loads them;
install and removal stay with the first window. Later windows load the AI pack's read-only
**reader** (`MV_AI_READER_ENTRY_SYMBOL`; `mv_addon_load_reader`) at first search or Settings
([23](23-nle-search.md) "The reader"); indexing, People edits, Import and add-on management stay
in the first window. The first window loads the full pack on demand
([17](17-local-ai-search.md) "Starting on demand"); Import loads at launch.

## Add-ons calling add-ons

Add-ons link nothing of each other. `addon_request(id, method, json, reply)` (host table) lands on
the other add-on's slot worker through `mv.request.1`, with the caller's id filled in by the host;
the reply comes back on the caller's thread. `MV_ERR_NOT_FOUND` means not installed (offer to
install). Served today (2026-11-19, [VPN add-on](../plans/vpn-addon.md)):

| Add-on | Methods | Who may ask |
|---|---|---|
| `phone-access` | `vpn_state`, `vpn_set {on}`, `vpn_code`, `vpn_code_hide`, `vpn_domain {domain}`: this computer or the claimed server as the VPN's exit | `vpn` only |
| `vpn` | `status` → `{"tunnel": bool}`: it is installed and loaded | anyone (Driftwire asks) |

## Commands

Rows in `src/shell/command_table.cpp`. They exist only while Import is installed.

| Command | Windows | Mac |
|---|---|---|
| Open Import | `Ctrl+Shift+I` | `⌘⇧I` |
| Import marked now (last preset) | `Ctrl+Shift+F7` | `⌘⇧F7` |
| Start (Import window) | `Enter` (no tile focused) · `Ctrl+Enter` | `Return` · `⌘Return` |
| Pause / resume while copying | `Space` | `Space` |
| Open focused tile in viewer | `Enter` | `Return` |
| Toggle file / day | `Space` / `Shift+Space` | same |
| Next / previous source | `Ctrl+Tab` / `Ctrl+Shift+Tab` | `⌃Tab` / `⌃⇧Tab` |
| Eject after summary | `Ctrl+J` | `⌘J` |

## Where it lives

| Piece | Code |
|---|---|
| Verified copy, BLAKE3, file / volume ports | `src/io/verified_copy.*`, `content_hash.*`, `file_port*.cpp`, `volume_{win,mac}.cpp` |
| Add-on host: manifest, store, loader, host table | `src/addon/`, `mediaviewer_addon.h` |
| Import add-on (`mv_import`) | `src/addons/import/` (scanner, planner, naming, library index, engine), `mediaviewer_import.h` |
| Windows chrome | `IslandHost.Addons.cs`, `src.managed/MediaViewer.Import.Chrome`, `src/abi/addon_abi.cpp` |
| Mac chrome | `AddonsView.swift`, `src.swift/ImportChrome` → `Import.bundle`, `src/shell/addons_mac.mm` |
| Packing | `tools/package/addon-pack.py`, `tools/addon-verify` |
| Tests | `tests/test_import_engine.cpp`, `test_verified_copy.cpp`, `test_addon_manifest.cpp`, `test_import_naming.cpp`, `test_json.cpp`, `test_content_hash.cpp`, `tools/package/test_addon_pack.py`; headless build `cmake/portable` |

## Check library (2026-11-14)

A Tools entry (`CheckLibraryWindow.cs`, `CheckLibraryView.swift`; engine `library_check.{h,cpp}`,
job kind 3), also the index behind the Mac's Photos backup ([26](26-photos-library.md) "Backup").
Given a folder and a library folder it reports which files are already in the library (any path,
any name), which are not, and the library's own duplicates. Report only.

- Same test as an import (size, then BLAKE3). Library files are read only when their size matches.
- Every hash goes into `library` under the destination's volume key, so a re-check of an unchanged
  library reads nothing and Import's duplicate skip learns those files. Source hashes go into
  `seen_hashes`.
- **Scope:** this library folder, or every library Import knows (offline ones confirm nothing).
- **Offline:** an unreachable library is reported; nothing is dropped from the index.
- **Host source** (Photos library, Mac): the host calls `library_find` per file and
  `library_record` after writing and verifying (`mv.import.1`). Background priority.

## Find duplicates (PR 54)

A Tools entry (`DuplicatesWindow.cs`, `DuplicatesView.swift`; engine `find_duplicates` /
`trash_duplicate`, job kind 2): every set of byte-identical files under a folder.

- Size first, BLAKE3-256 where sizes match; never names or look-alikes. Every file type; hidden,
  system and package entries are not walked, links not followed, empty files skipped.
- Hashes cached in `seen_hashes` (path, size, mtime); an unchanged rescan reads no contents.
- Runs on the add-on's I/O thread, one reader per device, yielding like a background import;
  cached reads.
- Groups by largest waste; files can be opened, revealed, or moved to the Recycle Bin / Trash.
  **Deleting is safe by construction:** Trash only; the file must be unchanged and another copy
  re-read to confirm ("the last copy is always kept"); a pick of every copy is not sent.
- **Keys:** `Enter` opens; `Delete` / `⌘⌫` trashes; `Ctrl+E` / `⌘R` reveals; `Esc` closes.

## Known gaps

- Clip tiles show ▶ without a poster unless the viewer's thumbnail cache has one.
- The Mac finish notification is a beep (`NSBeep`); Windows uses an app notification where the
  app has an AUMID.

## Not in Import

Cloud or upload; a catalogue/albums/keywords database; near-duplicate or burst detection; writing
metadata on import; deleting from or formatting cards; tethered capture; transcoding.
