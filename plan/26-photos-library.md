# 26 — The Photos library as a folder, and its backup (macOS)

**Status: built 2026-10-03 on the branch `claude/macos-icloud-photos-improvements`, owner request
of 2026-10-03 ("can we have it as a folder that is opened? instead of just searching? (only if it
has been added in settings)"; "a tool … to export them to a nas … backup all my icloud photos to a
local directory"). Mac-only under D9, like the source it extends (plan/17 "Photos library
source", issue #72). The Windows half is nothing by design: iCloud for Windows syncs to a folder,
which a folder root already is.**

Three things live here. The first two are the owner's Photos asks; the third was found while
doing them and is a viewer fix on both platforms.

1. **The library as a folder.** Once the user has added the Photos library in Settings (Local
   search → *Add Photos Library*), it is one more place to open: a **Photos Library** row at the
   top of the folder tree (`⌘⇧E`), **File → Open Photos Library**, and
   `mv_chrome_open_photos_library` for the chrome. It opens as a listing titled *Photos
   Library* — the gallery, filmstrip, keys and marks of any folder — oldest first (the Photos
   app's Library order), the newest selected. Not before the library was added: the row, the
   menu item and the backup section are absent until then, and absent again after *Remove*.
2. **Backup.** Settings → *Photos Library* → **Back Up Now**: every original of the library,
   copied and verified into a folder of the user's choosing (a NAS share, a drive), under
   Import's layout `YYYY/YYYY-MM-DD/<original name>`. Run again, it copies only what is new or
   missing.
3. **Clips play the right way up.** A portrait phone clip is coded landscape with a 90° display
   matrix in its container. The player ignored it, so every such clip — from Photos or from a
   card — played sideways, and its poster tile was sideways too. Both read the matrix now.

## Virtual items (the folder)

A Photos asset has no path, and a 23 k-asset library cannot be turned into 23 k files to list
it (plan/17 measured 7 ms a resolve, and most originals are only in iCloud). So the folder
model gains **virtual entries**: a `list_entry` whose `is_virtual` is set is listed as given —
name, stamp and size from the provider, no stat — under the pack's own key, `photos:<id>`
(`shell/folder_model_mac.h`). Everything the host does with a listing (gallery, filmstrip, marks,
sort-free list order, duplicate names) works on it unchanged; three things learn the key:

- **Tiles.** `folder_model::set_virtual_items(prefix, thumb_fn)`: a tile for a `photos:` key comes
  from PhotoKit's own cached rendition, network off, and is stored in the thumbnail cache under
  `(key, modificationDate, pixel count)` like a file's. The pack's moment rows for a clip are
  looked up under the same key first.
- **Opening.** The host resolves a key to a file on a worker as it is about to be shown
  (`shell/photos_items_mac.h` `resolve`): the current rendition where Photos keeps it, in place
  and read-only; or, when only iCloud has the original, a `<name> (preview).jpg` written once
  to `~/Library/Caches/MediaViewer/Photos Library/`. The result is memoised, so a second visit is
  as direct as a file's, and the ±2 neighbours resolve in the same job so the next arrow does not
  wait. After 400 ms on a preview the original is fetched from iCloud (the one network request;
  the user's own viewing, owner 2026-09-28) and replaces it in place — the viewer's usual
  first-picture-then-full-resolution refinement, over the network. Navigating away cancels the
  fetch and removes its part file.
- **Writes.** `shell/write_guard.h` protects the key itself and everything under the cache
  folder: Move, Trash, ratings, every metadata edit, rotate / flip, Save copy and trims are
  refused with the plan/17 notice. **Copy To** and drag-out copy the original, fetched from
  iCloud for it if this Mac does not hold it (the user asked for the file).

**Search results use the same path.** The AI chrome used to resolve every Photos result into a
file before opening the list ("Getting N items from your Photos library…", previews written
for the whole result set, its own on-view download). It now passes the keys and the host lists
them as virtual entries (one PhotoKit fetch for the lot gives names and stamps; a deleted asset
drops out). The pack's `itemChanged:` path is the key, so scrub markers, N / Shift+N and Find
Similar ask about the asset with no mapping table. `PhotosLibrary.swift` keeps only tiles,
access and the **added flag**: `mv.photosLibrary.added` (NSUserDefaults, one process), set
while the library is a root of the index and cleared with *Remove*; the host's `available()`
is that flag and PhotoKit access, both.

**Cleared after.** On-view downloads go when the list ends (a folder opens, another list opens,
Back) and at quit; the whole cache folder is emptied at launch. Previews are small and stay
for the session.

**Not done:** the list does not follow the library live (an import in Photos shows after the
folder is reopened; the index rescans on its own); Reveal in Finder, Share and Copy Path act
on the resolved file only when there is one; a Live Photo's video is not shown beside its
still.

## Backup

`shell/photos_backup.h` is the portable engine, over a `source` (PhotoKit in
`photos_backup_mac.mm`; a fake in `tests/test_photos_backup.cpp`) and `io::verified_copy`
(hashed while read, read back from the destination uncached, compared — plan/18's rule, and
the deep network path once plan/24's PR 49 lands, since the copy goes through the same call).

- **What:** every asset's **originals** — the photo or video as shot, a Live Photo's paired
  video, a RAW+JPEG pair's RAW — the Hidden album included (a backup keeps what the user hid).
  Not Photos' edited renditions (edits are non-destructive; the original is what a backup
  keeps), not albums or metadata Photos holds outside the file.
- **Where:** `<destination>/YYYY/YYYY-MM-DD/<original name>`, the day in local time; a second
  asset with the same name on the same day gets ` (2)` (`io/collision_name.h`). Nothing is
  overwritten.
- **Resumable, idempotent:** `<destination>/.mediaviewer-photos-backup/manifest.sqlite` records
  every file copied and verified. The next run skips a recorded file that is still there (one
  stat), copies one the user deleted again, and a cancelled run picks up where it stopped.
  Zero bytes are written for an asset already backed up. `last-run.txt` beside it lists what
  did not make it.
- **iCloud-only originals are downloaded for this** — owner call, this is the user asking for a
  copy of them — into `~/Library/Caches/MediaViewer/Photos Backup/`, copied verified, removed.
  The index's rule (never download) and the viewer's (only on view) are unchanged.
- **One run at a time,** on its own thread; it survives Settings closing; quit cancels it between
  buffers and leaves no part file. Progress (counts, bytes, the file being copied) is polled by
  Settings at 2 Hz; the last run is remembered.

**Not done:** a schedule (run it from Settings); a mirror (a file deleted in Photos stays in
the backup); hashing an existing destination file against the source when the manifest was
lost (such a run copies beside it as ` (2)`); Photos' edited renditions as an option.

## Clips with a display matrix (both platforms)

`video_stream_info::rotation` (0 / 90 / 180 / 270, clockwise) comes from the stream's
`AV_PKT_DATA_DISPLAYMATRIX` (`player::stream_rotation_degrees`, the convention
`edit/clip_common.cpp` already used); every `video_frame` carries it as quarter turns. The
video blitters (HLSL and its MSL twin) take the turn: the camera frames the *displayed* picture
(coded height × width for 90 / 270) and the shader maps back to the coded frame before sampling
the visible rect. Both hosts size the picture after the turn (`picture_size`, `media_width`).
`player::poster_frame` turns the tile the same way, so the thumbnail spec moves
`jpg512.2 → jpg512.3` and old posters regenerate on view (plan/04). Tests:
`tests/test_poster.cpp` on the synthetic fixture's 0°, 90° and 180° clips.

## Verify

- `mv_tests "[poster]"`, `"[write_guard]"`, `"[photos_backup]"`, `"[folder][photos]"` pass on
  the Mac; the first three are in the Windows suite too (the engine is portable; PhotoKit is
  the Mac host's).
- In a Mac build: a 640×360 clip with a 90° matrix shows 360×640, upright, with the fixture's
  flat half on the left; with the library added, the tree shows **Photos Library**, opening it
  lists the library in the gallery, an iCloud-only item opens as a preview and becomes the
  original after a moment, Move / Trash / edits are refused, Copy To copies the original;
  Settings → Photos Library backs up to a chosen folder, a second run writes nothing; the Mac
  PR 1 present-loop gate still holds.

## Owner calls recorded here (plan/12, 2026-10-03)

The library opens as a folder once added in Settings; the backup downloads iCloud-only
originals on the user's own run; the thumbnail spec bump for turned posters.
