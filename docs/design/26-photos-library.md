# 26 — The Photos library as a folder, and its backup (macOS)

**Status: built 2026-10-03 on the branch `claude/macos-icloud-photos-improvements`, owner request
of 2026-10-03 ("can we have it as a folder that is opened? instead of just searching? (only if it
has been added in settings)"; "a tool … to export them to a nas … backup all my icloud photos to a
local directory"). Mac-only under D9, like the source it extends (docs/design/17 "Photos library
source", issue #72). The Windows half is nothing by design: iCloud for Windows syncs to a folder,
which a folder root already is.**

Three things live here. The first two are the owner's Photos asks; the third was found while
doing them and is a viewer fix on both platforms.

1. **The library as a folder.** Once the user has added the Photos library in Settings (Local
   search → *Add Photos Library*), it is one more place to open: a **Photos Library** row at the
   top of the folder tree (`⌘⇧E`), an **iCloud Photos** row with a cloud at the top of
   **File → Open Recent** / the Dock menu (owner, 2026-10-05; it has no remove button and
   follows Settings when the window is key again), **File → Open Photos Library**, and
   `mv_chrome_open_photos_library` for the chrome. It opens as a listing titled *Photos
   Library* — the gallery, filmstrip, keys and marks of any folder — oldest first (the Photos
   app's Library order), the newest selected. Opening says *Opening the Photos library…* at
   once, keeps it up until the listing ends, and ignores another click while the listing runs;
   the listing takes each name from the fetched asset — PhotoKit's private `originalFilename`,
   its property set (`PHAssetPropertySetOriginalMetadata`) fetched with the assets in one query
   — and falls back to the original's resource only when that is missing. Names are cached by
   asset and modification date. It costs about what the AI pack's walk does (23 k assets:
   +~90 ms over the bare walk), not one resource lookup per asset (that took 15 minutes on
   24 k assets, #420). The name is the file as shot (`IMG_0001.HEIC`) for an edited item too;
   the resource lookup used to name 8,462 of 23,277 edited items `FullSizeRender.*`. The time is in the unified log,
   `photos: listed N assets in M ms` (count and time only). Not before the library was added:
   the row, the menu item and the backup section are absent until then, and absent again after *Remove*.
   The welcome card also shows an **iCloud Photos** row, on its own above *Recent folders*.
   It opens Settings at *Photos Library* (the backup), not the listing (owner, 2026-10-05):
   from the empty window, the library's settings are what the row is for.
2. **Backup.** Settings → *Photos Library* → **Back Up Now**: every original of the library,
   copied and verified into a folder of the user's choosing (a NAS share, a drive), in the
   folders the user picks — Import's `YYYY/YYYY-MM-DD/<original name>` unless changed (issue
   #414). Run again, it copies only what is new or missing.
3. **Clips play the right way up.** A portrait phone clip is coded landscape with a 90° display
   matrix in its container. The player ignored it, so every such clip — from Photos or from a
   card — played sideways, and its poster tile was sideways too. Both read the matrix now.

## Virtual items (the folder)

A Photos asset has no path, and a 23 k-asset library cannot be turned into 23 k files to list
it (docs/design/17 measured 7 ms a resolve, and most originals are only in iCloud). So the folder
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
  folder: Move, Trash, ratings, every metadata edit and trims are refused with the
  docs/design/17 notice; rotate / flip stay edits (never written into the library). **Copy To**
  and drag-out copy the original, fetched from iCloud for it if this Mac does not hold it (the
  user asked for the file). A saved edit is a new photo in the library: see below.

## Edits of a library photo (owner 2026-11-04; docs/plans/edit-refresh.md S0)

Save copy, Enhance (Detail, Restyle, Remove, Extend), Open closed eyes and Improve faces work on a
library item, and what they make goes **into the library as a new photo**; nothing already there
changes (rule 5). One sink, `abi/addon_media.h`, which the Mac host feeds through two hooks set at
launch (`set_photos_hooks`):

- **resolve(key, ctx)** gives the file to read: the current rendition in place, or the original
  fetched from iCloud into the cache folder. Every writer reads through it (`readable_path`):
  `full_still` (Enhance's `open_still`, so Detail, Restyle, Remove and Extend), the faces jobs
  (target and donors) and Save copy (`save_copy_to_library`).
- **add(file, source)** hands a file staged in `staging_dir()` (`$TMPDIR/MediaViewer Saves/`, never
  beside a library file) to PhotoKit (`creationRequestForAssetFromImageAtFileURL`) and returns the
  new asset's key, `"photos:" + placeholderForCreatedAsset.localIdentifier`. `add_to_library`
  removes the staged file whatever the outcome, so the staging folder is empty afterwards.

**Where the result goes.** The add hook tells the host (`-photosAdded:source:`): when the list on
screen holds the source, the new photo is listed right after it (`folder_model::insert_virtual`,
named and stamped by one PhotoKit `describe`), so → reaches it, and the notice says *Added to your
Photos library, next to this photo*. Otherwise the notice is *Added to your Photos library as a
new photo*. The Enhance sheet says the same instead of a file name, the faces notes say
*added to your Photos library*, and a key is never shown (rule 6). The Edit pane's result row
(docs/plans/edit-refresh.md S5) is to select the new key in that listing; until it lands, the notice and the arrow are how to reach it.

**Permission.** Adding needs read-write access (full or limited) or add-only access
(`photos::can_add`). A library item is only listed with read access, so this holds in practice;
when neither was ever asked, Save copy asks for add-only access on the click
(`NSPhotoLibraryAddUsageDescription`). A refusal is a notice, never a silent failure; Enhance's
sheet says how to allow it.

**iCloud-only originals.** The fetch reports progress (`PHAssetResourceRequestOptions.progressHandler`)
to the notice, *Downloading from iCloud… 40 %*, for the viewer's own refinement and for a
writer's read alike, and clears it when the fetch ends. A job's cancel stops a writer's fetch at
its next progress report when the writer passes its `job_context` to the hook (Open closed eyes,
Improve faces, Save copy); Enhance's reads go through `full_still`, whose host-table row carries no
context, so its Cancel does not stop the download yet. Navigating away stops the viewer's fetch.
Two fetches of one original never write at once: the second waits for the first's file (a writer
waiting behind the viewer's fetch cannot be cancelled until that fetch ends). The Edit pane's sliders wait for the
original as they wait for a RAW's develop (*Preparing…*): a preview never becomes the working
image; when the fetch fails the pane says it could not be prepared and the notice says the
picture is a preview.

**Windows** has no library. A OneDrive or iCloud-for-Windows placeholder
(`FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS`, `io::file_stat::cloud_placeholder`) is hydrated by the
worker's read for Save copy and the working image, the status line saying *Downloading this file
from the cloud…* meanwhile; the result is written beside the original and the sync client uploads
it. The Mac shows the same notice for an evicted iCloud Drive file (`SF_DATALESS`).

Tests: `mv_ai_tests "[photos]"`, whose writer audit (`[audit]`) runs every writer above on a
`photos:` key with fake hooks and checks that each calls add exactly once, from the source's key,
with its file staged outside the library, that nothing is written beside the source (bytes and
stamp unchanged) and that the staging folder is left empty; and `mv_tests "[photos]"` (Save copy
into another folder writes nothing beside the source; a new photo is listed right after its
source, never twice).

**Search results use the same path.** The AI chrome used to resolve every Photos result into a
file before opening the list ("Getting N items from your Photos library…", previews written
for the whole result set, its own on-view download). It now passes the keys and the host lists
them as virtual entries (one PhotoKit fetch for the lot gives names and stamps; a deleted asset
drops out). The pack's `itemChanged:` path is the key, so scrub markers, N / Shift+N and Find
Similar ask about the asset with no mapping table. `PhotosLibrary.swift` keeps only tiles,
access and the **added flag**: `mv.photosLibrary.added` (NSUserDefaults, one process), set
while the library is a root of the index and cleared with *Remove*; the host's `available()`
is that flag and PhotoKit access, both.

**Result order.** A Photos search keeps the order the search panel handed it: best match when
the search can answer, else newest by date created (`creationDate`, never the edit time) first
(issue #412, docs/design/17 "Order"). The grid and the gallery and filmstrip opened from it are
the same sequence, and the item opened stays current. Opening the library as a folder is not a
search: oldest first, the newest selected.

**Cleared after.** On-view downloads go when the list ends (a folder opens, another list opens,
Back) and at quit; the whole cache folder is emptied at launch. Previews are small and stay
for the session.

**Show in Photos.** `⌘E` on a library item (a search result or the library opened as a folder)
— **Open in Photos** in the Open flyout, `?` and Settings while such an item is current (issue
#412; *Show in Finder* for a file) — opens it in the Photos app, selected, instead of revealing its rendition inside the library
bundle or the cache folder in Finder (owner, 2026-10-05). Photos has no API for this; its
scripting `spotlight` command does it, sent as an Apple Event built from the key's
`localIdentifier` (Photos' scripting id for a media item), never as script source
(`photos::show_in_photos`). Photos is launched and brought forward first, then the event goes
from a private queue; the main thread never waits on it. macOS asks once whether MediaViewer may
control Photos (`NSAppleEventsUsageDescription`; the hardened runtime needs
`com.apple.security.automation.apple-events`). Refused, or an asset Photos does not show (the
Hidden album), Photos still comes forward, without the selection. Opening a result stays in the
viewer; only `⌘E` changes.

**Not done:** the list does not follow the library live (an import in Photos shows after the
folder is reopened; the index rescans on its own); Share and Copy Path act
on the resolved file only when there is one; a Live Photo's video is not shown beside its
still.

## Backup

`shell/photos_backup.h` is the portable engine, over a `source` (PhotoKit in
`photos_backup_mac.mm`; a fake in `tests/test_photos_backup.cpp`) and `io::verified_copy`
(hashed while read, read back from the destination uncached, compared — docs/design/18's rule, and
the deep network path once docs/design/24's PR 49 lands, since the copy goes through the same call).

- **What:** every asset's **originals** — the photo or video as shot, a Live Photo's paired
  video, a RAW+JPEG pair's RAW — the Hidden album included (a backup keeps what the user hid).
  Not Photos' edited renditions (edits are non-destructive; the original is what a backup
  keeps), not albums or metadata Photos holds outside the file.
- **Where:** Settings → *Photos Library* → *Folders* (`backup::layout`, remembered as
  `mv.photosBackup.layout` beside the destination; issue #414), from the asset's creation date in
  local time:

  | Layout | A file lands in |
  |---|---|
  | Year | `<destination>/YYYY/<original name>` (Import has no such layout) |
  | Year and day (default) | `<destination>/YYYY/YYYY-MM-DD/<original name>` |
  | Year, month and day | `<destination>/YYYY/MM/DD/<original name>` |
  | Day | `<destination>/YYYY-MM-DD/<original name>` |
  | Flat | `<destination>/<original name>` |

  A second asset with the same name in the same folder gets ` (2)` (`io/collision_name.h`).
  Nothing is overwritten. A layout change moves nothing: a file already verified stays where the
  manifest says, and only a new or missing file is written in the layout chosen now.
- **Resumable, idempotent:** `<destination>/.mediaviewer-photos-backup/manifest.sqlite` records
  every file copied and verified, by its place under the destination at whatever depth its
  layout gave it (`2024/2024-05-17/name`, `2024/name`, `name`), not its absolute path, so a share
  that remounts elsewhere (`/Volumes/photos-1`) or a drive whose letter moved is still the same
  backup (rows from before 2026-10-05 held absolute paths, all in the year/day layout; their last
  three components rebase the same way). The next run skips a recorded file that is still there (one
  stat), copies one the user deleted again, and a cancelled run picks up where it stopped.
  Zero bytes are written for an asset already backed up. `last-run.txt` beside it lists what
  did not make it. If the destination itself disappears mid-run (the NAS drops off the
  network, the drive is unplugged), the run stops as failed rather than failing — and
  possibly downloading from iCloud — every remaining file.
- **An edited photo's original:** the editing input's full-size image is the original only
  while a photo has no edits; for an edited one PhotoKit hands back the render. Such a photo
  (a full-size render or adjustment data among its resources) is streamed from its original
  resource instead.
- **iCloud-only originals are downloaded for this** — owner call, this is the user asking for a
  copy of them — into `~/Library/Caches/MediaViewer/Photos Backup/`, copied verified, removed.
  The index's rule (never download) and the viewer's (only on view) are unchanged.
- **One run at a time,** on its own thread; it survives Settings closing; quit cancels it between
  buffers and leaves no part file. Progress (counts, bytes, the file being copied) is polled by
  Settings at 2 Hz; the last run is remembered.

**Not done:** a schedule (run it from Settings); moving an existing backup into a newly chosen
layout; a mirror (a file deleted in Photos stays in
the backup); hashing an existing destination file against the source when the manifest was
lost (such a run copies beside it as ` (2)`); Photos' edited renditions as an option.

## Clips with a display matrix (both platforms)

`video_stream_info::rotation` (0 / 90 / 180 / 270, clockwise) comes from the stream's
`AV_PKT_DATA_DISPLAYMATRIX` (`player::stream_rotation_degrees`, the convention
`edit/clip_common.cpp` already used); every `video_frame` carries it as quarter turns. The
video blitters (HLSL and its MSL twin) take the turn: the camera frames the *displayed* picture
(coded height × width for 90 / 270) and the shader maps back to the coded frame before sampling
the visible rect. Both hosts size the picture after the turn (`picture_size`, `media_width`),
including the Mac's fit on a clip's first frame (until 2026-10-05 it fitted the coded size, so an
iPhone portrait clip sat pinned left and ran off the bottom).
`player::poster_frame` turns the tile the same way, so the thumbnail spec moves
`jpg512.2 → jpg512.3` and old posters regenerate on view (docs/design/04). Tests:
`tests/test_poster.cpp` on the synthetic fixture's 0°, 90° and 180° clips.

## Verify

- `mv_tests "[poster]"`, `"[write_guard]"`, `"[photos_backup]"`, `"[folder][photos]"` pass on
  the Mac; the first three are in the Windows suite too (the engine is portable; PhotoKit is
  the Mac host's).
- In a Mac build: a 640×360 clip with a 90° matrix shows 360×640, upright, with the fixture's
  flat half on the left; with the library added, the tree shows **Photos Library**, opening it
  lists the library in the gallery, an iCloud-only item opens as a preview and becomes the
  original after a moment, Move / Trash / metadata edits are refused, Copy To copies the original;
  Save copy, Detail and Open eyes on an iCloud-only photo each add one asset (the source asset's
  modification date unchanged, the staging folder empty), the progress notice shows while the
  original comes, and the new photo is listed next to its source;
  Settings → Photos Library backs up to a chosen folder, a second run writes nothing; the Mac
  PR 1 present-loop gate still holds.

## Owner calls recorded here (docs/design/12, 2026-10-03)

The library opens as a folder once added in Settings; the backup downloads iCloud-only
originals on the user's own run; the thumbnail spec bump for turned posters.
