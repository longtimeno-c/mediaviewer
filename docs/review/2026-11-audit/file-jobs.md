# Audit: file jobs, transfer, write guard, Photos backup, shellext, catalogue

Scope: `src/shell/file_jobs.*`, `file_transfer.*`, `write_guard.*`, `photos_backup*`,
`photos_items_mac.*`, `src/shellext`, `src/catalogue`, and the `src/io` code they call
(`verified_copy.cpp`, `replace_{win,mac}.cpp`, `file_ops_win.cpp`, `file_port_mac.cpp` rename/flush).
Read-only. Open issues checked (`gh issue list --limit 200 --state open`): only #611 (F7/F8 progress and
Stop; this covers `file_jobs` having no cancel or progress) and #155 (Mac collision tests) overlap. Nothing
below is already filed.

"Confirmed" means I read the code path end to end. "Suspected" means the code is as described, but whether
it bites depends on OS or filesystem behaviour I could not run.

---

## Issues by severity

### High

**H1. Windows `replace_atomic` can delete the only copy of the user's file.** Confirmed by reading, plus the
documented `ReplaceFileW` contract.
`src/io/replace_win.cpp:98-101` calls `ReplaceFileW(wide, temp, nullptr /*no backup*/, ...)`. If that fails,
the code runs `DeleteFileW(temp)`. Microsoft documents that with no backup name,
`ERROR_UNABLE_TO_MOVE_REPLACEMENT` means "the replaced file no longer exists and the replacement file exists
under its original name". In that case `temp` holds the only copy, and the code deletes it.
`ERROR_UNABLE_TO_MOVE_REPLACEMENT_2` leaves the original under a different name, which is never restored.
Callers that write users' originals in place:
- `src/shell/edit_session.cpp:696` (lossless rotate)
- `src/meta/write.cpp:1153, 1275, 1384` (metadata)
- `src/remote/library.cpp:86`

Sharing violations from antivirus, the indexer or the thumbnail surrogate are the usual trigger.
Fix: branch on `GetLastError()`. On `..._MOVE_REPLACEMENT`, rename `temp` to the final name instead of
deleting it. Better, pass a backup name and restore from it on any failure.
Test gap: `tests/test_edit.cpp:705` covers only the success path. There is no seam to inject a
`ReplaceFileW` failure.

**H2. A cross-volume folder move deletes files that arrived during the copy.** Confirmed by reading.
`src/shell/file_transfer.cpp:152` lists each folder's children once, when `copy_tree` enters that folder. After
the whole tree is copied and verified, lines 323-327 run `fs::remove_all(source)`. That deletes everything in
the folder at that moment, including files added after their folder was listed. On a NAS move the window lasts
the whole copy, often minutes to hours. Examples: a camera sync, Import, or another app writing into the
folder. Those files are never copied, and the item reports success.
Fix: remove exactly the entries `copy_tree` copied, newest first (the mirror of `undo_made`), then remove each
folder only when it is empty. A folder that is not empty afterwards should stay and be reported.

**H3. A Photos backup can be written to the boot disk under a stale `/Volumes/<share>` mount point.**
Suspected (the code path is confirmed; it depends on macOS leaving the empty mount point behind).
`src/shell/photos_backup.cpp:871-872` checks only that `destination` stats as a directory before
`make_directories(dir)` and `verified_copy`. The comment at 868-870 says this prevents re-creating a vanished
destination. But the same file says at 671-676 that "an offline share's mount point is often left behind
empty". That stale folder passes the check: `/Volumes` is admin-writable, so the YYYY folders and copies land on
the local disk and count as `done`. When the share comes back it mounts at `/Volumes/<share>-1`. The manifest
handle still points at the dead share, so `record()` only logs a warning (line 267).
`destination_answers()` (451-455) has the same weakness, so `fail_file` never stops the run.
Fix: record the destination's device id (`st_dev`), or check that `housekeeping_dir/manifest.sqlite` still
exists, once at start. Compare before each file.
Test gap: the "destination that disappears" test (`tests/test_photos_backup.cpp:436`) removes the folder
entirely. It never leaves an empty one behind.

### Medium

**M1. On macOS, a drag-move within one exFAT card or one SMB share fails outright.** The code is confirmed;
filesystem support for `RENAME_EXCL` is suspected.
`src/shell/file_transfer.cpp:81-84` calls `renamex_np(..., RENAME_EXCL)`. Any errno other than
`EXDEV`/`EEXIST` returns `failed`, and line 289 turns that into an `io` error with no copy fallback.
`io::rename_no_replace` (`src/io/file_port_mac.cpp:425-448`) exists because FAT/exFAT and some shares answer
`ENOTSUP`/`EINVAL`, and it falls back to link-then-rename. The drop path does not use it.
Fix: call `io::rename_no_replace`, or treat `ENOTSUP` as `other_volume`. That runs the verified copy, which is
safe but slower.

**M2. On a server upload, the name the server actually used is thrown away; on a later failure, undo removes
the wrong file.** Confirmed by reading.
`mixed::put_file` (`file_transfer.cpp:211-215`) ignores the key `ops_.add` returns. The header (line 67) says
the service lands `" (2)"` when the name is taken. `copy_tree` still pushes `dest` into `made` (136, 145). If a
later child fails, `undo_made` removes whatever is at `dest`: the other party's file, which goes to the
server's trash. Meanwhile our upload under `" (2)"` stays behind as an orphan. `transfer_item.dest` also
reports the wrong name. This only happens in a race: the planned name was free when checked.
Fix: return the key from `put_file` and record that.
Test gap: the fake `add` (`tests/test_file_transfer.cpp:214-218`) throws on a taken name instead of renaming.

**M3. Starting a Photos backup can block the UI thread on share I/O.** The engine is confirmed; that the host
calls it on the UI thread is suspected.
`finish()` sets `running_ = false` (`photos_backup.cpp:446`) before `record_run()` and `write_report()` (947-948).
Both write to the destination: SQLite with a 10 s busy timeout (line 103), then `fopen` on the share.
`engine::start` (368-369) sees `running_ == false` and calls `join()` on the caller's thread.
`main_mac.mm:16538-16565` calls it from the chrome, so on a slow or dropped NAS, Back Up Now freezes the UI.
That breaks hard rule 1. `snapshot()` also reports `done` before the report exists.
Fix: clear `running_` last. Then `start()` never joins a thread that is still busy.

**M4. Partial `.mvtmp` files are left in the destination after a NAS drop.** Confirmed by reading.
`verified_copy.cpp` (lines 458, 464, 472, 478, 485) ignores `remove_file(w.temp)` failures. When the share
drops mid-write, that remove is exactly the call that fails. None of the callers in this slice sweep
`*.mvtmp`: Photos backup, `run_transfer` and the Windows `file_jobs` move. (Import and Driftwire have their
own sweeps.) On the next run `create_temp` moves on to `.mvtmp2` … `.mvtmp16` (`kTempAttempts`). Partial
videos, visible to the user (no dot prefix and not hidden), pile up in the backup folder.
Fix: Photos backup sweeps `<final>.mvtmp*` in each folder it writes, when it starts writing there.

**M5. The Photos backup manifest is a single point of failure.** Lack of recovery is confirmed; corruption is
suspected.
- No recovery. If the manifest is unreadable or corrupt, the DDL fails, so every run ends with "The backup's
  manifest could not be opened" (`photos_backup.cpp:113-125, 497-499`). The only way out is for the user to
  delete a hidden folder by hand.
- Corruption risk. It is a rollback-journal SQLite database on SMB, and the code expects two Macs to write it
  ("another Mac's backup into the same folder", 89-91, 101-103). SQLite's file locking over SMB/AFP is not
  reliable.
- Lost rows. A row that fails to write is only logged (`record`, 266-267). The next run re-copies that asset as
  `name (2).ext`, so the backup grows duplicates.

Fix: on `SQLITE_CORRUPT`/`NOTADB`, move the bad file aside and rebuild from the destination. The library index
already knows the bytes there. Also keep a lock file per destination.

**M6. A transient error on the share can overwrite the catalogue's `library.json` and its key-move chain.**
Suspected (needs a transient error at that exact moment).
`catalog.cpp:283, 291, 320` call `fs::exists(..., ec)` and never look at `ec`. When the share hiccups, `exists`
returns false with `ec` set. At 283 that skips the identity check. At 320, `put_durable` then writes a fresh
`library.json`, and the rename replaces the existing one. That file has no `epoch` and no `chain`; compare
`catalog_chain.cpp:101-121`. After a key handover, phones still pinned to the old key then fail `follow_chain`
(`at != current`, `catalog_chain.cpp:172`) and must be paired again. It can also overwrite another library's
identity in the same folder.
Fix: treat `ec` as `status::io` and never write `library.json` unless the answer was a real "not found".
`start_library` (`publisher.cpp:111`) has the same pattern.

**M7. The key-handover `library.json` and `takeover.request` are written to the share without a flush.**
Suspected (depends on the filesystem).
`catalog_chain.cpp:33-44` (`replace_file`) does `ofstream::flush` then `rename`, with no `fsync`/`_commit`.
`publisher.cpp:41-65` (`write_durable`) does fsync, and `catalog.cpp:262-274` (`put_durable`) goes through a
verified copy. `hand_over` and `restart_publisher` (209, 223) are the one write where a torn or empty file
locks out every phone (`read_library`, 130-133, returns `corrupt`).
Fix: use the publisher's `write_durable`, or route the write through `put_durable`.

**M8. The Explorer thumbnail surrogate reads stills of up to 512 MB whole, outside the cap and the deadline.**
Suspected.
`thumb_provider_win.cpp:209-216` reads the whole still (`kMaxSourceBytes` = 512 MiB) in `Initialize`, on the
shell's thread. That is before `run_bounded` applies `kMaxInFlight` = 4 (`thumb_request.cpp:78`) and
`kDeadline` = 4 s. Explorer runs several extractions at once, so several objects can each hold hundreds of MB
in dllhost. A read from a slow share is not bounded in time at all.
Fix: read stills lazily through a `byte_source`, as clips already are. Or take the in-flight slot before the
full read.

### Low

- **L1. On Linux, a drag-move rename can replace an existing file.** Confirmed by reading.
  `file_transfer.cpp:86-88` checks `exists(dest)` and then calls `std::rename`. A file created between the two
  calls is replaced, and that breaks the "never replace" rule in the header. `renameat2(RENAME_NOREPLACE)`
  already exists in `rename_no_replace` (`file_port_mac.cpp:419`).
- **L2. macOS `replace_atomic` drops the original's metadata.** Confirmed by reading.
  `replace_mac.cpp:65-85` swaps in a new inode, so xattrs (Finder tags, comments, `kMDItemWhereFroms`), ACLs,
  birth time and hard links are lost on the user's original. `stat` follows a symlink, but `rename` replaces
  the link itself. The parent directory is not fsynced after the rename. The Windows path keeps these through
  `ReplaceFileW`.
  Fix: `copyfile(COPYFILE_METADATA)` from the original to the temp before the rename, and refuse symlinks.
- **L3. Two move paths ignore `source_unstable`.** Confirmed by reading.
  `file_transfer.cpp:66-68` and `file_ops_win.cpp:112-116` delete the source after a verified copy even when
  `copy_outcome::source_unstable` says the source changed between attempts. The copy matches one read of a
  file that was still being written. Treat that as a failure for a move.
- **L4. F8 copy on Windows can leave a truncated file under the final name.** Confirmed by reading.
  `file_ops_win.cpp:82-93` uses `CopyFileExW`, which writes straight to the final name. A crash or power loss
  mid-copy leaves a truncated file that looks complete. Being unverified is by design
  (`docs/design/24-transfer.md:58`); this partial-file case is not mentioned there.
- **L5. A Photos backup can hang on a paused Import job.** Suspected.
  `photos_backup_library.cpp:139-159` loops until `state >= DONE && != PAUSED`, with no time limit. If Import
  pauses the check job (for example while its own window needs the disk), the backup's worker polls forever.
  Cancel sends `api_->cancel` once, which may not resume a paused job.
- **L6. An Import check can keep running after the add-on is closed.** Suspected.
  `close_import_library` (253-262) cancels `g.job`, but `check()` stores `g.job` only after `check_library`
  returns (122-130). A close in that window leaves the add-on's job running after unload.
- **L7. Republishing the same generation nearby deletes a folder a phone may be reading.** Suspected.
  `prepare_nearby` (`catalog_chain.cpp:286, 306`) allows the same generation number (the check is `>`, not
  `>=`). It then calls `remove_all(done)` on the folder `current.json` names.
- **L8. `deliver` trusts a shared pack by its size alone.** Confirmed by reading.
  `catalog.cpp:338-339` skips a file whose size matches. A same-size corrupt pack is never repaired, so phones
  keep failing its hash.
- **L9. Windows F8 uses one copy profile for marks from mixed folders.** Suspected.
  `file_jobs.cpp:61-62` picks the profile from `items.front()` only. Marks from a card plus a share, or from
  several folders, all get the first file's profile. For example, a card could be read deep.

### Adjacent (outside the slice, found while tracing callers)
- `src/shell/main_mac.mm:7824-7830`: a Photos item whose `resolve()` fails is silently left out of `local`, so
  the drop's summary says "Copied N items" without it. `resolve` is also called without `&job->cancel`, so a
  large iCloud fetch inside a drop cannot be stopped.

---

## Improvements ranked

1. **Use the network copy profile for Photos backup and drag-drop copies to a NAS.**
   `photos_backup.cpp:888-894` and `file_transfer.cpp:59-64` build `copy_options` without
   `copy_profile_for` / `batch_copy_profile`. Both use the sequential path: one 4 MiB request at a time, one
   file at a time. That is the slow case `docs/design/24-transfer.md:50-62` was built to fix; its "Where it is
   used" list names only F8 and Import. Apply `profile.apply(options)` per file and use `files_in_flight` for
   the backup loop. Measure with `tools/copybench --rtt-us`.
2. **Stop paying ~15 minutes of PhotoKit lookups on every Back Up Now.** `enumerate_files`
   (`photos_items_mac.mm:554`) calls `+assetResourcesForAsset:` per asset on every run. The file's own note
   (72-76, #420) measured that at about 15 minutes for 24k assets. Most runs have nothing new. Cache the
   resource list per (localIdentifier, modificationDate), as `name_cache` does, or keep it in the manifest.
   `local_original` (618) and `backup_resource` (396) make the same call again for each new file.
3. **Hash each file once.** With an index, a file whose size is in the library is read three times:
   `hash_file` (817), then `verified_copy` (source read plus read-back). The server path also hashes (846) and
   then `put` reads the file again. Return the hash from the first pass and skip the second, or hash during the
   copy and look up afterwards.
4. **Walk each folder once in `run_transfer`.** It calls `size_of` (a recursive walk) twice per step (257, 268).
   On a NAS folder of thousands of files that doubles the metadata round trips. Keep the first total per step.
5. **Batch the manifest writes.** Each row is its own autocommit transaction with `synchronous=FULL` on a share
   (`photos_backup.cpp:114, 250-269`): several SMB fsyncs per file. Commit every N files or every few seconds.
   A lost tail costs only a re-check, and the found-on-disk logic already handles that.
6. **Read Explorer stills lazily** (see M8). RAW and HEIC previews sit near the head of the file, so most
   requests would read a few MB instead of the whole file.
7. **Use libsodium's SHA-256** (`crypto_hash_sha256`, already linked) instead of the scalar one in
   `src/catalogue/sha256.cpp` for manifests and model files of hundreds of MB. Minor.

---

## Test gaps

- `replace_atomic`: failure paths. Windows needs a seam that fails `ReplaceFileW` with each documented error
  (H1). macOS needs xattr and symlink preservation (L2).
- `run_transfer`:
  - a cross-volume folder move where a file appears in an already-listed subfolder mid-copy (H2);
  - a fake `add` that answers `" (2)"`, followed by a later failure in the same folder (M2);
  - a move on a filesystem without `RENAME_EXCL`: an exFAT disk image via `hdiutil` (M1);
  - Linux no-replace rename (L1);
  - a move whose source changes between attempts (L3).
- `photos_backup`:
  - a stale empty mount point (H3);
  - a corrupt or locked manifest (M5);
  - a failed `record()`;
  - `start()` right after a run ends does not wait on report I/O (M3);
  - leftover `.mvtmp` files are swept after a failed remove (M4);
  - a paused or never-ending Import job (L5).
- `catalogue`: `deliver` when `fs::exists` errors (M6); a same-size corrupt shared pack (L8); `prepare_nearby`
  republishing the same generation (L7).
- `shellext`: no test of `thumb_provider_win.cpp` itself: COM refcount, `DllCanUnloadNow` while a decode is
  abandoned, and the clip stream marshalling. `test_thumb_request`/`test_thumb_poster` cover only the portable
  halves.
- `file_jobs` (Windows): no cancel or progress to test (#611). Mixed-folder profile choice (L9).
