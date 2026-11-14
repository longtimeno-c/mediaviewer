# Plan — Check library: the Photos backup knows what the library already holds

Written 2026-11-14 from main at `09db6255` (0.1.42). Owner's question: does the Mac's Photos
Library backup recognise photos that are already in the family library on the NAS, for example
by hash? Today it does not. Dual-track (D9): one shared change in the Import add-on's engine, a
Mac half (Settings → Photos Library, and Import's Tools) and a Windows half (Import's Tools).

## 1. What happens today

Three tools write into a library folder, each with its own duplicate rule:

| Tool | Knows | A photo already there under another name or folder | A file deleted from the folder |
|---|---|---|---|
| **Import** (cards, folders) | `import.db` `library`: every file it copied or hashed, by destination, relative path, size, mtime → BLAKE3-256 | Skipped by content (size, then hash), at "this destination" or "the whole library index" scope | Not copied again unless the card still has it and the index forgot it |
| **Mac Photos backup** (`shell/photos_backup.*`, [26](../design/26-photos-library.md) "Backup") | Its own manifest, `<destination>/.mediaviewer-photos-backup/manifest.sqlite`, by Photos asset id | **Copied again** under a safe name (` (2)`) | **Copied again** |
| **Phone backup** (iPhone 1.1, [mobile §10b](mobile.md)) | Its ledger, by `PHCloudIdentifier`, plus a same-folder name + size + hash check | Copied again unless it is the same name in the same folder | Copied again |

So a family library that already holds someone's photos (imported from a card, sent by the phone,
copied by hand) gets a second copy of each from the Photos backup, and every photo someone
deliberately deleted from the library comes back on the next Back Up Now.

## 2. What to build

### 2.1 Check library

A run that compares a **source** with a **destination folder and every folder under it**, and
matches by content only: size first, BLAKE3-256 only where a size matches, never the name. The
same test Import uses ([18](../design/18-import.md) "Duplicate test").

- **Sources:** the Mac's Photos library (Settings → Photos Library → *Check library*), or any
  folder (Import → Tools → *Check a folder against the library…*, on both platforms).
- **The library side is Import's index.** The walk of the destination reads sizes and mtimes only.
  A file is hashed only when its size is one a source file has, or one another library file has
  (for the duplicates list). Each hash goes into `import.db` `library` under the destination's
  volume key (`dest_locator`), so a remount, a moved drive letter or `E:\` versus `\\nas\photos`
  is the same library, and **a second check is a walk plus lookups: no file is read again** while
  its size and mtime are unchanged. A row is dropped only when its folder answers and the file is
  not in it (the 2026-11-09 rule).
- **Scope:** *this destination* (the folder and its subfolders) or *the whole library index*
  (every destination Import knows, resolved where they are mounted now; an offline one confirms
  nothing and keeps its rows). Import's default, "this destination", is the default here.
- **The report** (on screen, and `check-<n>.txt` beside the job reports, local only):
  1. **Already in the library:** how many source files, and for each where its bytes are (every
     place, if more than one).
  2. **Not in the library:** how many, and their size, so would be copied.
  3. **Deleted since:** files the Photos backup's manifest lists that are no longer in the folder.
     Only when the manifest has rows (so the folder is the real backup, not an empty mount point)
     and the file's folder or the destination answers. A listed file whose bytes are now somewhere
     else in the library is **moved**, not deleted, and is listed as such.
  4. **Duplicates already in the library:** sets of files with identical bytes inside the folder,
     largest waste first. Report only: nothing is moved or deleted. Find duplicates is where a copy
     can be put in the Bin.
- **iCloud-only originals:** their size comes from PhotoKit without a download (`PHAssetResource`
  `fileSize`; 0 when Photos does not say, then treated as unknown). One whose size matches a
  library file cannot be confirmed without its bytes, so the check counts it as **in iCloud, size
  matches; checked when it is backed up** rather than downloading gigabytes to answer a question.
  The backup downloads it anyway (it does today) and then skips the write if the bytes match.

### 2.2 The backup uses it

Back Up Now, with Import installed:

1. Lists the Photos originals (with their sizes, as above).
2. Runs the library side of a check over the destination at the chosen scope (walk, hash only the
   sizes the Photos library has; remembered from the last check).
3. For each asset not already in its manifest: if the index has a file of its size, it gets the
   bytes (local, or fetched from iCloud as today), hashes them, and asks the index. **A match is
   not copied**: the manifest records it as **found** (where, its size and hash) and the progress
   line counts it ("found in the library"). Otherwise it is copied and verified as today, and the
   copy is added to the index, so Import from a card later skips it too.
4. A found row is re-confirmed next run through the index (a lookup), not by a stat of the place,
   so a file reorganised inside the library stays found.

Scope setting in Settings → Photos Library, beside the layout: **Look for photos already in:
This folder · Every library Import knows** (remembered as `mv.photosBackup.scope`; default This
folder). Without Import installed, the backup behaves as today and the setting says "Install
Import to find photos already in the library": Import owns `import.db`, and the add-on carries its
own code (decision log 2026-10-11).

The cost of the extra read: a local original whose size matches is read once to hash it before
the copy reads it again. Only size matches pay it; a size match with different bytes is rare.

### 2.3 Deleted on purpose — decided 2026-11-14 (owner: "A+B (Recommended)"), built

**Decision (owner, 2026-11-14): A and B, as recommended; C later.** Check library lists the files
deleted since they were backed up with *Copy again* and *Leave deleted*, and Back Up Now holds them
(does not copy them) until someone chooses. *Leave deleted* is remembered by hash in the backup's
manifest. A missing file counts as deleted only when its folder answers (for a file inside the
backup, the destination answering with its manifest is that proof; for a file found in another
library, the file's own folder must answer). "Delete from library" (C) comes later.

The options as they were put to the owner:

Today a manifest file missing from the folder is copied again. That brings back photos someone
removed from the family library on purpose. Never decide from a missing file alone: an offline
share is not a deletion ([18](../design/18-import.md) "Offline destinations", 2026-11-09).

| Option | What it does | For | Against |
|---|---|---|---|
| **A. Ask in Check library** | The deleted-since list has **Copy again** and **Leave deleted** per file and for all. Until someone chooses, Back Up Now does **not** copy a deleted-since file; its line says "12 photos were removed from the folder since they were backed up · Review". | Nothing comes back by surprise; nothing is lost (Photos still has the original); one place to decide | A new file in the list each time someone deletes; a run that never gets reviewed never copies those |
| **B. A remembered "left deleted" list, by hash** | Leave deleted writes the file's hash to the manifest (`left_deleted`: hash, size, when). The backup never copies those bytes again to that destination, whatever asset or name they come back under (a photo re-downloaded into Photos gets a new asset id). | Survives a new asset id and a lost manifest row; the hash is already known | Needs a way to undo (the list in Check library with *Copy again*) |
| **C. "Delete from library" in MediaViewer** | A command on a file in the viewer that moves it to the Bin and, when the file is under a backup destination, records its hash as left deleted. | Records intent at the moment of deletion, no review needed | Only covers deletions made in MediaViewer, not the Finder, Synology Photos or another computer; a new destructive command |

**Recommendation: A and B together, C later.** A is the decision, B is how it is remembered:
by content, so it holds when the photo comes back under another id. A file missing while its
folder (or the destination with a non-empty manifest) answers is *held*: not copied, listed,
waiting for a choice. Choosing *Copy again* copies it on the next run (or now). C is useful but
covers only one way of deleting, so it waits until A and B have been used.

### 2.4 The phone backup later (not built now)

The phone backup's service (on the NAS) writes into the same folders. It could use the same
index:

- The service already hashes every file it receives, so it can **write rows into the same
  `library` shape** (its destination folder, relative path, size, mtime, hash), or better, keep
  one shared index on the NAS beside the folder that the desktop Import reads when its destination
  is that share. Today `import.db` lives on the desktop; two machines writing one SQLite file on a
  share is not safe, so the NAS index would be the service's own, read-only to the desktop.
- Its ledger keys on `PHCloudIdentifier`. Before sending a file, the phone could send **size and
  then hash** (the phone hashes while it reads the original anyway) and the service answers "have
  it at <place>": no upload, the ledger records it as found, the same as 2.2.
- The same "left deleted" list by hash (2.3 B) would then hold for the phone too, if it lives in
  the folder (`.mediaviewer-…/left-deleted`) rather than in each tool's own manifest.

## 3. Where the code goes

- **Import add-on (`src/addons/import`):** a new job kind 3, *library check*, in
  `library_check.{h,cpp}`: the walk, the size-first hashing at background priority (the present-
  loop busy rule, [18](../design/18-import.md) "Priority"), the index rows, duplicates, the
  report. Exposed on `mv.import.1` (appended; `struct_size` guards older chromes):
  - `check_library(request_json) → job` [no-block]: destination, scope, and either a source folder
    (the add-on hashes it) or a list of source sizes (a host source such as Photos), plus the
    manifest's places for the deleted-since list. Progress, cancel and `summary_json` as every job.
  - `library_find(request_json, out)` [worker]: by size, how many indexed files; by size and
    hash, where they are, each confirmed on disk (a stat; a changed file is re-hashed once).
  - `library_record(request_json)` [worker]: a file the backup just verified, into the index.
- **Mac host:** `backup::library` (an interface in `shell/photos_backup.h`, portable) implemented
  over that table in the Mac host; the backup engine gains a check-only mode and the found path.
  PhotoKit gains the resource size. Settings → Photos Library gains *Check library*, the report,
  and the scope picker.
- **Both Import chromes:** Tools → *Check a folder against the library…* (pick a source folder,
  then the destination; the report view).

## 4. Rules this keeps

- Nothing that can block on the UI or render thread: the check is an add-on job; the backup
  thread calls the blocking find/record.
- Hashing at background priority (waits while the present loop is busy); *Fast* is not offered.
- Never modifies an original, never deletes from the library. The tool only reports; a copy
  happens only from Back Up Now (or a *Copy again* the person chose).
- Rule 6: paths, names and hashes stay in `import.db`, the manifest and the local reports.
  Nothing is logged with a path.

## 5. Verify

- `mv_tests "[photos_backup]"` and `mv_import_tests "[import]"` pass, with new cases:
  a fake Photos source and a library with **planted duplicates, renamed copies, files in other
  folders, files deleted since the backup**, at both scopes; a second check reads no file; an
  offline destination confirms nothing and drops nothing.
- Cost, measured on this Mac (Release): a first check and a second check of a large synthetic
  library (tens of thousands of files), the bytes hashed and the wall time of each.
- Both present-loop gates are untouched (no render-thread code changes); the check yields like a
  background import.

## 6. Status (2026-11-14)

- [x] 2.1 Check library: the add-on job (`library_check.{h,cpp}`), Import → Tools on both chromes,
  Mac Settings → Photos Library → *Check library*. Folded into [18](../design/18-import.md) and
  [26](../design/26-photos-library.md).
- [x] 2.2 The backup uses it, with the scope setting (*Skip photos already in*).
- [x] 2.3 A+B (owner, 2026-11-14): held files, Copy again / Leave deleted in Check library
  (`backup::decide_deleted`), `left_deleted` by hash in the manifest. C not built.
- [x] Measured cost, Mac, Release, local SSD, warm cache (the files were just written), the source
  holding every library size (the worst first check): 20,000 files, 2.56 GB, three runs:
  **first check 2.2 s, 3.7 s, 3.6 s**, every file hashed; **second check 91, 63, 70 ms** (a third
  check 101, 70, 69 ms), nothing read (`mv_import_tests "[.perf-bench][check_library]"`). Not measured: a cold
  cache, and a NAS, where the first check is bound by the link (about 23 s for these 2.56 GB at
  110 MB/s) and the walk by the share's directory listing.
- Not run: the Windows native build and the WinUI window (C# compile-checked on the Mac only), the
  live Settings and Import windows on either platform, the real Photos library (not used as test
  data).
