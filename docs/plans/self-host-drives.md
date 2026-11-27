# Self-hosting: two or more drives, kept apart or combined

Issue #686 §3, owner's calls of 2026-11-14. Builds on [self-hosted.md](self-hosted.md) (`driftwire
host`, the "This computer" card). Decision: [12 (2026-11-14)](../design/12-decision-log.md#driftwire),
"MediaViewer may erase and combine drives".

With two or more drives picked for the server, the person either shares each as it is or **erases
them and combines them** into one volume that the host then shares (`host share`) and watches.

## 1. The choices, every one offered equally (owner)

Shown side by side, each with the space it gives (worked out from the picked drives' sizes), what a
dead drive costs, and its speed. Mirror is marked as the one for photos; Stripe carries a warning;
none is hidden. Since 2026-11-27 (owner: the step was hard to follow) the card asks first what the
drives are for, and these come under its third answer, Combine drives; Separate is its second,
Share drives as they are; the first is Back up a drive ([keep a copy](self-host-drives-keep-a-copy.md)).

| Choice | Space you get | If one drive dies | Speed |
|---|---|---|---|
| **Separate** | Each drive is its own folder; nothing is erased | Only that drive's files are lost | Each drive's own |
| **Mirror (RAID 1)** | The smallest drive | Nothing is lost; replace it and it rebuilds. **Recommended for photos** | Reads a little faster, writes as one drive |
| **Span (JBOD)** | All drives added together | That drive's files are lost, often more | As one drive |
| **Stripe (RAID 0)** | All drives added together (the smallest × count) | **Everything** is lost | Fastest |
| **Parity (RAID 5)** | Total minus one drive (the smallest × (count − 1)) | Nothing is lost if only one dies; **3 or more drives** | Slow to write |

Above them, always: **RAID is not a backup.** Deleting a file deletes it on every drive. Keep one copy
somewhere else: a phone's backup folder, a second NAS, a drive kept elsewhere.

Where the OS cannot do one, its row stays and says why instead of offering it:

- **macOS has no parity** (AppleRAID does mirror, stripe and concatenated only): "macOS can't make
  this. Use Mirror, or combine the drives on a Windows PC."
- **Windows**: Storage Spaces is on every Windows 10/11 edition, Home included, but can be turned off
  by policy and is not reachable in S mode (no PowerShell). The row says "Storage Spaces is not
  available on this PC" when the storage subsystem cannot pool.
- **Parity with fewer than 3 drives**: "Needs 3 or more drives."
- Any combined choice with one drive: "Needs 2 or more drives."

Pure: `drives::options_for(sizes, os, pool_ok)` (`cli/drives.h`) gives every row with its numbers and
words; the card and the terminal print the same rows.

## 2. The OS's own tools, behind one port (D9)

No RAID code of ours. `cli/drives.h` is the port; one file per OS:

| | Mirror | Span | Stripe | Parity | Health |
|---|---|---|---|---|---|
| **macOS** `drives_mac.mm` | `diskutil appleRAID create mirror NAME JHFS+ disks…` | `… concat …` | `… stripe …` | not offered | `diskutil appleRAID list -plist`, `diskutil info -plist` (`SMARTStatus`) |
| **Windows** `drives_win.cpp` | Storage Spaces two-way mirror | simple, 1 column | simple, one column per drive | parity | `Get-VirtualDisk` / `Get-PhysicalDisk` (`HealthStatus`, `OperationalStatus`), `Get-StorageJob` |
| **Linux** `drives_linux.cpp` | `mdadm` (later: a stub that says so) | | | | |

Windows builds each through one PowerShell script: `Clear-Disk` each drive, `New-StoragePool`, then
`New-VirtualDisk` (resiliency and columns above, fixed provisioning, maximum size),
`Initialize-Disk` GPT, `New-Partition -AssignDriveLetter`, `Format-Volume` NTFS with the set's name.
The Mac's AppleRAID volumes are Mac OS Extended (Journaled): AppleRAID cannot hold APFS.

The disks' facts come from the same tools, as JSON (`ConvertTo-Json`) or a property list read with
CoreFoundation, and are turned into `drives::disk` by pure parsers that every OS's tests run.

## 3. Erasing: the guards (all mandatory)

1. **What is on it, shown first.** For each drive: its name, size, model, and what is on it (its
   volumes, their used space and folder count).
2. **The person types each drive's name** (unique in the list: two drives both called "Untitled" are
   shown as "Untitled (disk4)" and "Untitled (disk5)"). There is no `--yes`; the card asks too.
3. **Refused**, with the reason, before anything else is asked:
   - a drive that holds this computer's system (Windows `IsSystem`/`IsBoot`; the Mac's boot
     volume's whole disk and the APFS container's physical stores; an internal drive on the Mac);
   - a drive with the MediaViewer library or catalogue on it: driftwire's data folder (the host's
     library), MediaViewer's own data folder, or a `.mediaviewer` folder at the top of a volume;
   - a drive holding a folder the host shares (serve.json's roots): it may be the only copy. The
     person copies it elsewhere and runs `host unshare LABEL` first. The app cannot prove another
     copy exists, so every shared folder is treated as possibly the only one;
   - a drive already in a set or pool, a drive that is read-only, and on Windows one that is not
     online.
4. **An administrator, once, in the OS's own prompt.** `host drives combine` runs as the person; only
   the OS tool itself is elevated: on the Mac `osascript … with administrator privileges` (the
   password sheet), on Windows `ShellExecuteEx "runas"` of PowerShell (UAC). The running host never
   asks for it and still refuses to run elevated. After the names are typed every drive is listed
   again and must be the one confirmed and still pass every guard. Then, inside the elevated step
   itself: on Windows the script checks each disk's `UniqueId` and `IsSystem`/`IsBoot` before
   `Clear-Disk`; on the Mac the one shell line under the password prompt compares each disk's size,
   model, internal flag and partitions' `DiskUUID`s before `appleRAID create` (a renumbered `diskN`
   stops there). The elevated Windows run never runs a file another process could change: a short
   bootstrap on its command line reads the script into memory, checks its SHA-256, and runs those
   bytes; it writes no file and reports through its exit code.
   - A drive with a volume that is not mounted is refused (what is on it cannot be checked), and
     Separate refuses the system's drive. A label a combined drive already has is refused.
5. **Tests touch disk images made for them only.** The test build (`MV_DRIFTWIRE_CLI_TEST_HOOKS`, and
   `drives::guard::images_under`) refuses any disk that is not a disk image whose file is under the
   test's own folder: `hdiutil` images on the Mac (`hdiutil info` maps each `/dev/diskN` to its
   image), VHDs on Windows CI (`Get-DiskImage`). Never this Mac's drives, never another session's
   images.

## 4. Afterwards

- **Health** in `host status`, in the card, and in state.json: per set healthy, degraded (a drive
  missing or failed), rebuilding (with a percent when the OS gives one) or offline; SMART "failing"
  where the OS reports it (`SMARTStatus`, `HealthStatus`/`Predictive Failure`). The running host's
  drives keeper (`host_keepers.h`, a third keeper beside DuckDNS and the router) reads it every five
  minutes on its own thread, no elevation needed.
- **A notification** when a set goes from healthy to degraded or offline (`osascript display
  notification` on the Mac, a toast through PowerShell on Windows), and **to the phone**: the
  service's caps carry `"storage":"degraded"` (or `rebuilding`, `offline`, `failing`) and the phone
  says "Your server's mirror lost a drive" (or the words for the others).
- **Replace a drive**: `host drives replace SET` lists the set's missing or failed member and the
  drives that could take its place (the same guards, the new drive's name typed), then on the Mac
  removes the missing member (`appleRAID remove`) and adds the new disk (`appleRAID add member`, which
  rebuilds; tried on images 2026-11-14: `repairMirror` on a set whose member had vanished left the new
  disk as a spare), on Windows `Add-PhysicalDisk`, `Set-PhysicalDisk -Usage Retired` on the old one,
  `Repair-VirtualDisk`, `Remove-PhysicalDisk`. A stripe or span has nothing to rebuild and says so.
- The combined volume is shared through the existing `host share` (the combine command runs it once
  the volume is mounted, with the set's name as the label).

## 5. Command line

```
driftwire host drives [list] [--json]          drives, what is on each, sets and their health
driftwire host drives choices DRIVE... [--json] the five rows for these drives
driftwire host drives combine CHOICE DRIVE... --label NAME [--confirm-stdin]
driftwire host drives replace SET [--confirm-stdin]
```

`CHOICE` is `separate|mirror|span|stripe|parity`; `DRIVE` is the id `list` prints (`disk4`, `2`).
`--confirm-stdin` takes the typed names one per line from stdin (the card), never as arguments.
`separate` erases nothing: it shares each drive's volume as it is (`host share`).

## 6. The card (Mac and Windows halves)

"This computer" gets a **Drives** step. Set up drives asks "What are the drives for?": Back up a
drive (recommended, nothing erased), Share drives as they are (Separate, nothing erased), or Combine
drives (erases). Combine drives shows the drives to pick, then the erasing rows side by side
(the same `options_for` words, from `host drives choices --json`), the "not a backup" line, then for a
combined choice each drive's facts and a field to type its name; Combine runs `host drives combine …
--confirm-stdin`, which brings up the OS's administrator prompt. The status row shows each set's health.
The card is moving into the Phone access add-on (#686 §1) in parallel; this step touches its files only
where it must.

## 7. Verify

- `mv_tests "[host-drives]"`: the choice table and its words, the space each gives, the guards
  (system, library, catalogue, shared folder, images-only), plans against stand-in disk lists from
  both OSes' JSON, the confirmations, the health parsers.
- By hand on the Mac, `mv_tests "[.host-drives-image]"`: a real AppleRAID mirror on two `hdiutil`
  images (made, combined, one detached → degraded, replaced → healthy), images deleted after.
- Windows: the same on VHDs on CI (owed: CI's Windows runner has no VHD step yet).
- Both present-loop gates still hold (nothing here runs in the viewer).

## 8. Not in this

- Linux `mdadm` (a stub that says "not yet").
- Growing a set, changing its kind, hot spares, scheduled scrubs.
- A second drive for a drive that is already full, without erasing it:
  [self-host-drives-keep-a-copy.md](self-host-drives-keep-a-copy.md).
- A combined volume as the boot or system volume.
