# Plan — Local search notices new files without a rescan

Written 2026-11-02 from the code at `201590ad`. One slice of Local search
([17](../design/17-local-ai-search.md)), dual-track (D9): one shared change, a Windows half and a
Mac half. The number is assigned when it is scheduled.

## Status (2026-11-02): built, not yet verified in the app

What was built is folded into [17](../design/17-local-ai-search.md) ("The host watches each
folder root", and the Photos library's iCloud-only assets), [12](../design/12-decision-log.md) and
[14](../design/14-abi.md). Where the build differs from the plan below:

- The watcher is its own header, `src/io/tree_watch.h`, with `tree_watch_win.cpp` and
  `tree_watch_mac.cpp`; `dir.h` is unchanged. Linux (the portable core) gets `tree_watch_none.cpp`,
  which watches nothing.
- On Windows a new name (`ADDED`, `RENAMED_NEW_NAME`) is asked whether it is a folder, one
  attribute read each. A folder moved in from outside the tree is a single `ADDED` with nothing
  for its contents, so a parent walk alone would miss them.
- Whole-root walks: more than 64 changed folders, or more than 4,096 names that may have been
  folders (mostly deleted files, one stat each), or lost events.
- §2.4 needs no schema step: "first seen this session" is a set in memory. The library's first
  walk notes nothing, and assets from earlier sessions keep the launch check.
- The Settings row shows only the exception: "new files found every 15 minutes" when a root is
  not watched.
- §6's calls were taken as recommended: no watch before the pack loads; the backstop is 60
  minutes on watched roots; shares are watched but keep 15 minutes.

**Run (Mac, Release):** `mv_tests "[tree_watch]"` (4 cases) and `mv_ai_tests "[watch]"` (3 cases)
pass, each repeated; the whole `mv_ai_tests "[ai]"` (175 passed, 12 skipped) and `mv_import_tests`
pass. The new Photos case fails with the fix taken out. The Swift AI chrome builds, and the WinUI AI
chrome's C# compiles.
**Not run:** the Windows native half (`tree_watch_win.cpp`, the host table on Windows) is
compiled and tested only by CI. Nothing was tried in the app on either platform. The §4 timings
in the app, idle CPU with watched roots, and both PR 1 gates during a large copy were not
measured.

## 1. What happens today

- **Folder roots (OneDrive included) have no watcher.** [17](../design/17-local-ai-search.md):342
  says "a watcher plus a startup delta scan", but the pack never watches a folder. New files are
  found by:
  - the startup delta, when the pack loads;
  - a full walk of every root every 15 minutes (`engine.cpp:2311`);
  - `note_folder_opened`, which rescans a root when the viewer opens a folder inside it
    (`engine.cpp:4803`).

  So a photo copied into an indexed folder can wait up to 15 minutes, unless the person browses to
  that folder. Each 15-minute pass walks every root in full, even when nothing changed.
- **A OneDrive file made available offline.** Hydrating a file changes its attributes, not its
  `(mtime, size)`. The next walk refreshes `assets.cloud` (`index_db.cpp`, `see_assets`), so the
  file is indexed then: up to 15 minutes late.
- **The Photos library is watched.** A `PHPhotoLibraryChangeObserver` (`photos_mac.mm:20`) sets a
  flag, and the control loop re-enumerates at most once every 10 s (`engine.cpp:2295`). One gap
  remains: an asset that arrives only in iCloud is stored as *unavailable*. It is checked again
  only once per launch (`engine.cpp:2797`). When iCloud sends it later in the session, its
  `(modificationDate, pixel count)` is unchanged, so the rescan does not re-queue it.
- **The core already has a watcher.** `io::directory_watcher` (`src/io/dir.h:89`) watches the
  viewer's open folder. On Windows it is `ReadDirectoryChangesW`, **not recursive**
  (`bWatchSubtree = FALSE`), and it does not watch attributes. On the Mac it is FSEvents, which is
  recursive. Its callback carries no path ("relist on any change"). It is in `mv_io`, which the
  pack does not link. The pack reaches the core only through the host table
  (`mediaviewer_addon.h`), which has `watch_volumes` (one per add-on) but no folder watch.

## 2. What this slice builds

### 2.1 `io::tree_watcher` (shared header, one implementation per OS)

A new class beside `directory_watcher`. The viewer's watcher is left as it is.

- `start(roots[], cb, user)` watches several folder trees. `cb(user, root_index, dir_utf8, flags)`
  runs on the watch thread, must not block, and names the **directory** that changed (not the
  file). `flags`:
  - `subtree`: rescan everything below `dir`;
  - `overflow`: rescan the whole root (events were lost).
- **Windows (`dir_win.cpp`).** One thread with an I/O completion port for all roots.
  `ReadDirectoryChangesW` with `bWatchSubtree = TRUE` and the filter: file name, dir name, size,
  last write, **attributes**. Attributes is the one that sees OneDrive hydrating or dehydrating a
  file. The `FILE_NOTIFY_INFORMATION` records are parsed down to their parent directories, with
  duplicates removed per buffer. A zero-byte completion (buffer overflow) sends `overflow`. A
  directory rename or removal sends `subtree` for its parent. A root that cannot be opened (offline
  share, pulled card) fails `start` for that root only.
- **Mac (`dir_mac.cpp`).** One `FSEventStream` over all the roots, on the same private serial
  queue pattern as `directory_watcher`, with a 0.5 s latency. Each event path is a directory.
  `kFSEventStreamEventFlagMustScanSubDirs` maps to `subtree`. `UserDropped` / `KernelDropped` map
  to `overflow`.
- **Linux.** inotify is per directory, with no recursion, and is limited by
  `max_user_watches`. The Linux half waits for the order of the Linux halves
  ([linux.md](linux.md) §10). The header stays portable. Until then a Linux build gets a stub
  that fails `start`, and the pack falls back to polling (§2.3).

### 2.2 Host table: `watch_tree` (appended)

```c
/* [worker-thread] Watches folder trees for the add-on. `on_change` runs on the
 * host's watcher thread and must not block. Each call replaces the add-on's
 * previous set; count 0 stops watching. out_ok[i] is 0 for a root that could
 * not be watched (offline, no access). Appended: read it only when
 * `struct_size` covers it and the pointer is non-NULL. */
mv_status (MV_CALL* watch_tree)(void* host, const char* const* roots_utf8, uint32_t count,
                                void (MV_CALL* on_change)(void* user, uint32_t root,
                                                          const char* dir_utf8, uint32_t flags),
                                void* user, uint8_t* out_ok);
```

It follows the pattern of `t_watch` (`src/addon/host.cpp:245`): a control mutex, a callback
cleared under its lock before stopping, and a stop when the add-on unloads (`stop_watch`). The paths
stay in the process. The index already holds them, and none of them reaches telemetry or crash
reports (rule 6). The Import add-on does not use this entry.

### 2.3 The engine

- **What is watched.** Every enabled folder root that is not held. A held root (#299) is not
  watched, which matches "held means not walked". The set is sent again whenever roots change
  (add, remove, enable, pause, resume). A root that `out_ok` reports as unwatched keeps today's
  15-minute walk, and `roots_json` gains `"watching": false`.
- **The callback** puts `(root, dir, flags)` into a `dirty_` map under `control_m_` and wakes the
  control thread. Nothing else runs on the watch thread.
- **Debounce.** The control loop takes a root's dirty dirs once that root has been quiet for 2 s,
  or after 10 s at most during a long copy (a camera dump). The 10 s matches the Photos rescan gap.
- **`scan_dirs(root, dirs)`.** It walks each dirty directory with `walk_files2` at depth 0, or at
  the root's depth for `subtree`. The per-directory pairing, `see_batch`, the generation and the
  cloud flag work as they do in `scan_root`. A **scoped end** then removes only the rows that were
  under the walked dirs and were not seen this generation:
  - `index_db::end_scan_dirs(root, gen, dirs)` does a range query on `assets.path` (already
    `UNIQUE`, so indexed) and filters to direct children in C++ (or keeps the whole subtree for
    `subtree`);
  - the rest of `end_root_scan` (vectors, faces, speech and cue rows dropped) is shared.

  `overflow` runs a full `scan_root`.
- **What it ignores.** The index's own data folder, housekeeping names (`housekeeping`), and
  non-media files. The walk already filters these, so an XMP sidecar or a `.tmp` from the
  viewer's atomic writes costs one directory listing and finds nothing.
- **The 15-minute walk stays**, as a backstop for events the OS dropped without saying so. It
  runs only for roots that are not watched, and once an hour for watched roots. That cadence
  change is a budget change and gets a decision-log row (§5).
- **Priority.** Unchanged: the control thread is background work, and the walk yields through
  `should_yield` as it does now. A directory the viewer has open still goes first
  (`prefer_next_`).

### 2.4 Photos: iCloud arrivals in the same session

When the observer fires, the rescan also asks PhotoKit about the unavailable assets **added in the
last 24 h**, not all of them (4,082 on the owner's library). These are the ones iCloud is still
sending down. The owner's rule "not on every change" (the comment at `engine.cpp:2800`) still
holds for the old ones. To find them, the rows need an `added` time: one more column, so one
schema step. The launch-time check of every unavailable asset is unchanged.

### 2.5 Hosts

- **Windows half:** `tree_watcher` in `dir_win.cpp`, `watch_tree` in the host table, and a Settings
  folder row reading "Watching for new files" or "Checked every 15 minutes" (`ManagePanel.cs`).
- **Mac half:** `tree_watcher` in `dir_mac.cpp`, the same row in `ManagementView.swift`, and §2.4.

## 3. What does not change

- **Nothing is indexed before search is first opened in a session** (#292). A watcher in the base
  app could record dirty folders without loading the pack, but that is its own decision (§6,
  call 1). This slice leaves it alone.
- Held roots wait for Resume, as #299 decided. Resume walks them in full, which finds what
  arrived.
- **Index online-only files** stays off by default. With it off, a new OneDrive placeholder is
  listed and counted as "only in OneDrive", but not read. The watcher only makes that happen
  sooner.

## 4. Verify line

> **Windows:**
> - A JPEG copied into a subfolder three levels deep in an indexed root is found by a search for
>   its content within 15 s, without that folder being opened.
> - A OneDrive online-only photo set to *Always keep on this device*, with **Index online-only
>   files** off, is indexed within 15 s.
> - A 2,000-file copy into a watched root causes one walk per debounce window, not one per file.
> - Idle with three watched roots: 0 presents and ~0 % CPU over 5 minutes.
> - PR 1's present-loop gate holds while the 2,000-file copy runs.
>
> **Mac:** the same, with iCloud Drive in place of OneDrive, plus: a photo taken on the phone
> arrives in the Photos library as iCloud-only, is downloaded by Photos, and is indexed within 15 s
> in the same session. Mac PR 1's gate holds.

**Tests:**
- `mv_tests` for `tree_watcher`: a nested create reports its own directory, a rename reports
  `subtree`, and a forced overflow reports `overflow`.
- `index_db` for `end_scan_dirs`: rows outside the dirs survive.
- The engine with a fake `watch_tree` in the pack's test host: dirty dirs are debounced, and a
  held root is not watched.

## 5. Docs on landing

- [17](../design/17-local-ai-search.md) "Folder roots persist": describe the watcher, the
  debounce and the backstop as built, replacing today's inaccurate line.
- [12](../design/12-decision-log.md): a row for the backstop cadence (15 min → 60 min on watched
  roots) and for the 24 h re-check window.
- [14](../design/14-abi.md): the `watch_tree` entry.
- `docs/DEVELOPMENT.md`: the new tests.

## 6. Owner calls

1. **Track changes before the pack loads?** The base app could watch the roots and remember dirty
   folders without loading the pack, so the first search after launch does not start with
   "Indexing new files…". It would make the base app hold a watch per indexed root in every
   session, so it is a #292 decision. **Recommend: no, for now.**
2. **How often the backstop walks watched roots:** 60 minutes is proposed; it is 15 today.
3. **Network shares.** `ReadDirectoryChangesW` on SMB works, but a server can stop sending
   without an error. **Recommend:** watch shares, but keep the 15-minute walk for them.
