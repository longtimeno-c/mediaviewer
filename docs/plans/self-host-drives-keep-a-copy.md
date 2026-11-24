# Self-hosting: a second drive that keeps a copy, nothing erased

Written 2026-11-15 from the code at `49147a79`. It builds on [self-host-drives.md](self-host-drives.md)
(`driftwire host drives`) and the host's keepers (`cli/host_keepers.h`).

**Status (2026-11-23):** slice 1 (the pure planner and the bin's rules, `cli/drive_copy.h`,
`"[drive-copy]"`) and most of slices 2 and 3 are built:

- The copier, `cli/drive_copier.h` (`"[drive-copier]"`): `.mvpart` then rename, the source's
  time kept, replaced and deleted files moved to the bin, expiry, trimming for room, the marker.
  An empty source beside a non-empty copy is treated as a missing drive, and a folder that fails
  to list moves nothing to the bin as deleted.
- The copy keeper and `copies.json`, `cli/host_copy.h`: a fourth host keeper that runs a pass on
  each copy every 15 minutes (every 2 while something is held back or failed), and at once when
  `copies.json` changes. It writes `copies-state.json`, which `host drives list --json` reads.
- `host drives copy FROM TO` and `copy-stop FROM` (`"[host-drives]"`). Nothing is erased, so
  no name is typed. A drive with a marker is refused by `refusal_of()`.
- The card's row (`phone/screens.cpp`, `"[phone-addon]"`): with two drives picked, "Keep a copy"
  comes first and is marked Recommended, with Copy from, whether it fits, and Start copying.
  Each copy kept gets a status row with Stop keeping a copy.

Not built yet: changes are found by the 15-minute compare, not `io::tree_watch`; there is no
background I/O priority; the monthly SHA-256 check; the Recycle Bin list, Restore and Empty;
the first copy's time estimate; erasing an unformatted or FAT32 copy drive (the copy refuses
nothing there yet, and a FAT32 drive can't hold a file over 4 GB); `use-copy` (slice 4); and the
phone's bin (slice 5).

## The problem

A person has one drive full of photos and buys a second one so that losing a drive doesn't lose
anything. Every combined choice in `host drives combine` starts by erasing **both** drives (Storage
Spaces' `Clear-Disk`, AppleRAID `create`). The only choice that keeps the data, Separate, keeps no
second copy. So the one thing they bought the drive for means copying everything somewhere else
first, and most people have nowhere else.

## Ways to do it, and the pick

| Way | Full drive erased? | Windows Home | Verdict |
|---|---|---|---|
| Storage Spaces mirror (what `combine` does) | Yes, always: a pool can't take a drive that holds data | Yes | Can't do this |
| Dynamic-disk "Add Mirror" (`diskpart` `convert dynamic`, `add disk=N`) | No: converts the disk, copies onto the new one | Reported missing on Home; dynamic disks are deprecated | Not this plan (see §8) |
| macOS `diskutil appleRAID enable mirror` + `add member` | No, but HFS+ only, never APFS | n/a | A later Mac slice, maybe |
| **Keep a copy**: the host copies the full drive onto the new one and keeps it in step | **No. The full drive is only ever read.** | Yes, and the Mac too | **The pick** |

Keep a copy is not RAID. Here's how it compares with a mirror, and it should say so in the card:

| | Mirror (RAID 1) | Keep a copy |
|---|---|---|
| Works with a drive that is already full | No (both erased) | Yes |
| New drive must be | At least as big as the old one | Big enough for what is on it |
| A drive dies | Keeps running | One click shares the copy instead; files from the last few minutes may not be copied yet |
| A file deleted by mistake | Gone from both | In the copy's Recycle Bin for 30 days, one click to put back (§5) |
| Copy drive plugged into another PC | Unreadable alone (Storage Spaces) | Ordinary folders |
| Silent damage (bit rot) | Not checked | A monthly check reads both and reports any mismatch |

## 1. What the person sees

On the Drives step, when one picked drive has data and another has room, a new row comes first and
is marked **Recommended**: **Keep a copy (nothing on "Photos" is erased)**. It shows:

- what is on the full drive, the new drive's free space, and whether the copy fits;
- "First copy: about N hours" (the size divided by a speed measured from the first 30 seconds of
  copying, then updated as it runs);
- the "not the same as a mirror" lines from the table above, and the existing "RAID is not a backup" line.

Then **Start copying**. The new drive is erased only if it can't be used as it is: it isn't formatted,
or it's FAT32 (FAT32 can't hold a file over 4 GB). That erase goes through the existing guards in
§3 of self-host-drives.md, with the drive's name typed. A drive that is already NTFS or exFAT and
has room is used as it is, and anything already on it is left alone.

## 2. Command line

```
driftwire host drives copy FROM TO [--confirm-stdin]   start keeping a copy of FROM's volume on TO
driftwire host drives copy-stop FROM                    stop (the copy stays on TO)
driftwire host drives use-copy FROM                     share the copy in FROM's place (FROM died)
driftwire host drives bin FROM [--json]                 what is in the copy's Recycle Bin
driftwire host drives restore FROM ITEM...              put items back on FROM
driftwire host drives empty-bin FROM [ITEM...]          delete them for good (all, or these)
```

`host status` and `host drives list` show each copy as: up to date · copying (N files, X GB left)
· behind (why) · copy drive missing · copy drive full · copy drive failing (SMART) · source
missing ("your copy on 'Backup' has everything up to 14:02, share it: driftwire host drives use-copy …").

## 3. The copy on the new drive

- Everything goes under one folder at the top, `MediaViewer copy of <label>`, beside a
  `.mediaviewer-copy.json` that names the source volume (its lasting id from `drives::disk::identity`)
  and the last time it was complete. A drive carrying this marker is protected by `refusal_of()` like
  a shared folder: it may be the only copy.
- A file is written to `name.mvpart` and renamed into place only once it is complete and flushed.
  So a crash or an unplugged drive never leaves half a file under the real name, and a restart
  resumes the copy. Modified times are kept, and a file counts as unchanged when its size and
  modified time match.
- **Deletes and overwrites never take a file away straight off.** The old version goes to the
  copy's Recycle Bin (§5). That's the protection a mirror can't give.
- Skipped and listed in status, never failing the copy: symlinks, junctions, files open for exclusive
  use (retried on the next pass). Long paths use `\\?\`. ACLs and alternate streams aren't copied.
- **The source is only ever opened for reading.** That meets the hard rule against changing a user's
  original file without needing anything special.

## 4. Keeping it in step

A fourth host keeper (`host_keepers.h`, beside DuckDNS, the router and drives) has its own worker
thread, uses background I/O priority (`THREAD_MODE_BACKGROUND_BEGIN` on Windows, `IOPOL_THROTTLE`
on the Mac) and needs no administrator.

1. **First copy**: walk the source and copy whatever is missing or changed, largest folders last
   so that most files are safe soonest.
2. **Changes**: `io::tree_watch` (`src/io/tree_watch.h`, already used by Local search) on the
   source's root. A changed file is copied once its size and time stop changing between two passes,
   so a file still being written isn't copied half-done. A lost-events signal or a big batch falls
   back to a walk, the same rule tree_watch's other user follows.
3. **Backstop**: a size-and-time compare of the whole tree every 60 minutes.
4. **Monthly check**: read both sides and compare SHA-256s. A mismatch is reported (with the path,
   on this PC only, never in telemetry) and isn't "fixed" in either direction, because which side
   is right can't be known.

Health goes through the drives keeper's existing news (`keeper_context`): the notification, the
card's status row and the phone's `"storage"` cap get `copy-behind`, `copy-missing` and
`copy-full`, with "Your server's copy drive is full" and similar words for each.

## 5. The copy's Recycle Bin

When a file is deleted from the full drive, or saved over, the copy doesn't drop the old version.
It moves it into a bin on the copy drive that works like the Recycle Bin. On the Mac it's called
the Trash, the same word the viewer already uses for its "Move to Trash/Recycle Bin"
(`io::recycle_file`).

**Why the copy has a bin of its own:** the OS's Recycle Bin on a removable drive is missing or
empties itself when it runs out of room, and a deletion on a NAS share by a phone or guest never
reaches the PC's bin at all. The copy's bin catches every deletion, whoever made it.

- **Where it lives:** `.mediaviewer-copy/bin/` on the copy drive. Each item is the file under a
  short id, plus one line in `bin.json` giving its original path, size, when it was removed and why:
  `deleted` (gone from the full drive) or `replaced` (a newer version was copied over it). A
  folder deleted as one is one item, so it can be put back as one. Nothing in the bin is ever
  shared to the phone or guests as if it were a live file.
- **What you see:** the card's status row says "Recycle Bin: 214 items, 3.2 GB". **Open** shows a
  list, newest first, with name, original folder, size, removed date and Deleted/Replaced, and a
  search box. Photos and videos show thumbnails through the viewer's own decoders. Every row and
  button can be reached from the keyboard.
- **Restore** puts the item back at its original place on the full drive. If something is already
  at that path, it is restored beside it as `name (restored).ext`, never written over. This is the
  one time the feature writes to the full drive: it only ever creates new files, never changes an
  existing one. The next pass sees the file and keeps its copy as usual.
- **Delete for good** and **Empty Recycle Bin** each ask once. They are the only things that remove
  anything from the copy drive before its time is up.
- **Kept for:** 30 days by default, with a setting offering 7, 30, 90 days or "until I empty it".
  Items older than that are removed in the keeper's hourly pass. If the copy drive runs low on room,
  the oldest items are removed first, but only once the bin is over a set share of the drive (10% by
  default); then the card says "Recycle Bin trimmed to make room" with how much was removed. The
  live copy is never trimmed.
- **The phone:** `serve` caps get `"bin": {items, bytes}`, and a later slice lets the phone list and
  restore. Not in the first build.
- **Telemetry and crash reports** carry only counts and bytes, never a name or path.

## 6. When a drive dies

- **The source dies:** `use-copy` (Share the copy instead) makes the share point at the copy
  folder (`host unshare`, then `host share` with the same label) so the phone and guests carry on.
  Adding a new drive later starts a copy in the other direction: the old copy becomes the source.
- **The copy drive dies:** pick a new drive; the first copy runs again.

## 7. Slices and tests

1. **Pure planner** (`cli/drive_copy.h`): source listing + target listing + marker → actions
   (copy, move to the bin, skip, report). `mv_tests "[drive-copy]"`: new, changed, renamed, deleted
   and same-size-different-time files; a file still growing (two listings) is held back; a full
   target; a symlink is skipped; the marker's guard in `refusal_of()`; the first card row's words and
   the fits/doesn't-fit numbers.
   **Bin rules, pure:** a deleted folder becomes one item; time-based expiry for each setting, using
   an injected clock (no sleeps); trimming takes oldest first and stops at the share limit; restore
   to a taken path picks `name (restored).ext`, then `(restored 2)`; `bin.json` survives a torn
   write (written to a temporary file and renamed).
2. **The copier and keeper** on temp folders: resume after an injected failure mid-file leaves no
   file under its real name; an overwrite keeps the old version; the source's bytes and times are
   unchanged after a run; a locked file is retried on the next pass; the monthly check finds one
   flipped byte; restore puts back the exact bytes and time, and never writes over an existing
   file on the source.
3. **The card row and the Recycle Bin list** on Windows (WinUI) and the Mac (SwiftUI), every action
   keyboard-reachable, checked in light and dark.
4. **`use-copy` and changing direction**, with host-share tests.
5. **The phone's bin** (list, restore).

The Mac gets the whole thing from the same code; only I/O priority and the watcher are per OS, and
both exist.

## 8. Not in this, and the owner's calls

- **Dynamic-disk mirror on Windows.** It would be a real RAID 1 of a full drive, but Microsoft has
  deprecated dynamic disks, Home editions are reported not to offer it (to be checked on a Home VM
  with VHDs before ever offering it), turning a disk back to basic erases it, and its health would
  need a third parser. *Call:* leave it out unless Keep a copy proves not enough.
- **Mac `appleRAID enable mirror`** for HFS+ drives: a possible later slice, tried on `hdiutil`
  images first.
- Copying to another computer or the cloud: the same planner could do it later, but that's a
  backup feature, not this.
- *Call:* the bin's defaults: 30 days, and trimming only once the bin is over 10% of the copy drive.
- Keeping several older versions of one file (a version history). The bin holds each replaced
  version as its own item, but there's no "versions of this file" view yet.
