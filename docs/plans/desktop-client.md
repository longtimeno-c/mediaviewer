# Another desktop opens the host's drives

**Status: plan, 2026-11-14 (issue #686, the addendum "other desktops open the host's drives from
anywhere" and the owner's calls of the same day). Built on `claude/self-host-integration` (#684).
This was slice 8 of `docs/plans/self-hosted.md` ("the laptop as a client of the service").**

## 1. What the owner asked

A phone reaches a self-hosted computer, or a Synology, from any network. Another desktop running
MediaViewer does not: on the home network it has the SMB share (#686 §2); away from home it has
nothing, since the VPN deliberately does not reach the home network (call 5 of `self-hosted.md`)
and SMB is never forwarded.

So **MediaViewer on another desktop becomes a client of the same service, the way the phone is.**
The owner's calls (2026-11-14):

1. **Read-only first.** An admin, in the desktop app or the `driftwire` CLI on the host, can later
   give that desktop write access. The roles are #686 §2's (session `smb-users` builds them).
2. **Pairing is just the code, like a phone.** No admin password. A desktop with no camera pastes
   the code, or opens it as a link.
3. **A remote folder in the viewer itself**, pinned in the folder list like iCloud Photos or
   OneDrive, not only inside the Phone access add-on.

## 2. Shape

Nothing new on the wire. The desktop is one more paired device of `driftwire serve`:

```
host card / `driftwire host pair` ──code──▶ desktop: File → Connect to a Server… (paste)
                                            │ mobile::parse_pair_payload, a new X25519 device key
                                            │ mobile::pair_device: the route race, the code redeemed
                                            ▼
                        src/remote (portable core, D9): servers.json + device key in the OS store
                                            │ one mobile::link_session per server (routes, relay,
                                            │ direct-path upgrade, reconnect, stream slots)
                                            ▼
   folder tree: a pinned row per server ── remote:<server>/<root>/<rel> keys ── the viewer's list
```

- **Reuse, not a fork.** `src/mobile/link.*` (the route race, reconnect, range reads) and
  `link_payload.*` (the code) move into a small static library, `mv_mobile_route`, that both the
  phone's `mv_mobile_link` and the desktop's `mv_remote` link. No line of either changes.
- **The device key** is the desktop's own (X25519, made at pairing), kept in the OS store
  (`secrets::os_store(kAppService)`: Keychain, DPAPI) with the access key, under
  `remote_<server id>`. The server's name, routes and pinned identity are in `servers.json` in the
  app's data folder; no key is in that file. The routes start as the code's and follow what the
  server says at each connect (a DuckDNS name or relay added later; docs/design/28-driftwire.md,
  "Addresses after pairing"). The service lists the desktop with the phones under
  the name it gave ("MacBook Pro", the computer's name) and revokes it the same way.
- **Read-only.** The desktop never calls the service's write op. Every `remote:` item is
  read-only through `shell/write_guard` (Move, Trash, rename, ratings and every metadata edit are
  refused with a notice; rotate and flip stay edits; Copy To and drag-out copy the fetched
  original). Write access waits for §6.

## 3. In the viewer

### Keys

A remote item has no path. It is a key, `remote:<server id>/<root id>/<rel>`, the same shape as
the Photos library's `photos:` keys (docs/design/26 "Virtual items"): listed as given — name, size
and mtime from the service's listing, no stat — and resolved to a local file on a worker when it is
about to be shown. `remote:<server id>/` is the server's top (its named folders, the Catalogue
hidden), `remote:<server id>/<root id>/` a named folder.

### The pinned row

The folder tree gets one row per paired server above the open folder's tree, as the Photos
Library row sits (Mac `FolderTreeView.swift`, Windows `IslandHost.Panels.cs`): a server glyph, its
name, and its state when it is not online (*Connecting…*, *Can't reach it*, *Access ended*). It
expands like any folder: its children are the named folders, theirs the subfolders, listed off the
UI thread through the same call a disk folder uses (`mv_chrome_list_subdirectories`,
`mv_list_subdirectories`), which answers a `remote:` key from the service. Choosing one opens it
through the same call Open Folder uses (`mv_chrome_open_folder`, `mv_folder_open`): a listing of
its files, titled with the folder's name, the gallery, filmstrip, marks and keys of any folder.

**Pinned with the folders too (owner 2026-11-18).** A server just paired is also pinned like a
folder (issue #617's pins, as `remote:<server id>`), named by the server with "Paired server" as
its place. The pins list it before the folder pins, and the welcome card draws it with a server
glyph in the top block, under iCloud Photos / OneDrive and above RECENT FOLDERS:
- **Mac:** after File > Connect to a Server; the welcome card, the Dock menu and File > Open Recent
  list it, and the Folders pane opens if the welcome card is up. Opening the pin opens the
  server's one named folder straight away; with several, their tiles in the gallery.
- **Windows:** after the tree's Connect to a server. The island sends `chrome_cmd_remote_changed`
  whenever `mv_remote_generation` moves; native reads `mv_remote_pair_state_json`'s `key` and
  `seq` to pin each pairing once, and drops the pin of a server no longer listed. The welcome card
  and Open > Recent folders list it; the jump list does not (its items start the exe with a disk
  path). Opening the pin lists the server's named folders on a worker (`mv_list_subdirectories`):
  one opens straight away; with several, the server's top opens with them as gallery tiles.

Forget This Server unpins it; Unpin or the card's x drops the pin and leaves the server paired in
the tree. Never treated as a disk path, so never forgotten for being away.

### First pixel (rule 3)

The ladder is the one docs/design/04 sets, with the network as the slow end:

1. **Memory** (the viewer's LRU) — unchanged, keyed by the `remote:` key.
2. **The disk thumbnail** — the thumbnail cache under (key, mtime, size), like a file's.
3. **A tile from the server**, made here, never by the server (a Synology has no decoders):
   - a still up to 16 MiB is fetched whole into the originals cache and thumbed from those bytes,
     so opening it later reads no network;
   - a larger still: its first 1 MiB, where a camera's embedded preview usually is, thumbed by the
     same `make_thumb_jpeg`; only when that fails the whole file;
   - a clip: its poster through `player::byte_source` over `read_range`, a few 64 KiB reads, never
     the clip.
4. **The original**, fetched into the originals cache as soon as the item is on screen (cancelled
   when the user moves on), then opened as a local file: the local pipeline's own preview → full
   resolution. The ±2 neighbours fetch behind it on the background lane, so an arrow lands on a
   local file.

Until the original is here the viewer shows the tile, as the Photos library shows a preview
(docs/design/26), and replaces it in place.

### Video

A clip's poster is a range read. Playback in this slice opens the fetched file: a clip plays once
it has arrived (the notice shows *Downloading… 40 %*). **Streaming playback through `read_range`
(seek without the whole file) is §7's next slice**: the player opens a path today
(`video_source.cpp`), and a byte-source open on the playing path is a change to the video hot path
that gets its own measurement.

### Caches, keyed by content identity

- **Originals:** `<caches>/MediaViewer/Remote/<server>/<h>/<name>`, `h` the BLAKE2b of
  (root, rel, size, mtime): a changed file is a new entry, a renamed one a miss. The name is kept,
  so kind-by-name (`is_video_name`, the D5 table) works unchanged. Bounded (2 GiB by default),
  least recently used first; a file in use is never removed. A part file is renamed into place
  only when whole, and its size is checked against the listing's.
- **Tiles:** in the app's thumbnail cache, under the key, like a file's.

### Unreachable, offline, ended

- A server that does not answer: its row says *Can't reach it*; opening it says *Can't reach
  NAME right now. Its folders show again when it answers.* What is cached still opens.
- A named folder whose drive is unplugged (`root_info.offline`): listed, greyed, opening it says
  *This folder's drive isn't connected to NAME.*
- Revoked or expired: the row says *Access ended*; **Forget** removes it (the keys go from the
  OS store, the caches with them).

## 4. The core (`src/remote`, portable, D9)

- `remote/key.*` — the key: parse, make, display names.
- `remote/library.*` — the paired servers, one `link_session` each, `pair`, `list`, `tile`,
  `fetch`, `cached`, `state`; every blocking call on a worker. The last listing's size and mtime of
  each file are kept, so a decode job holding only the key need not stat over the link.
- `remote/cache.*` — the originals cache (identity, LRU, part files).
- Hosts: Mac `main_mac.mm` (the Photos pattern: resolve on a worker, ±2 after, a refinement in
  place), the Windows core's session (`abi/abi.cpp`: a remote listing without a stat, a tile job,
  a decode job that fetches first), C ABI `mv_remote_*` for the C# chrome. No `HWND`, no Cocoa in
  `src/remote`.

## 5. Pairing entry

- **File → Connect to a Server…** on both hosts: one field that takes the code or the
  `mediaviewer://pair#…` link, the code's fingerprint shown to compare with the host's card before
  anything is redeemed, then *Connecting…*, then the row appears.
- **Where Phone access lives**: the Phone access add-on (session `phone-addon`) calls the same hook
  (`mv_chrome_remote_pair`, `mv_remote_pair_json`) from its screen. This slice does not edit the
  Phone access screens.
- **Opening the link**: registering `mediaviewer://` on the desktop would also catch a phone's
  link; owed (§7).

## 6. Write access (owner call 1, with #686 §2) — built

A desktop starts "view". When the host's admin gives it "change" (`driftwire host users role` /
`serve grant … --role change`), the next connect's caps say so and the service marks each named
folder `writable` for it (never the Catalogue). Then, on both hosts:

- **What it may do**: add (drops, Paste files here, Copy to), New Folder, move and rename within
  the server, and Delete, which goes to that folder's hidden trash on the server
  (`.mediaviewer-trash/<date>/`), emptied only by its owner. Ratings, metadata and edits of a
  remote original stay refused (`write_guard::change_allowed` lets exactly those five through).
- **Truthful per folder**: `remote::library::can_write(key)` is the service's change ops
  (`changes_files`) and that root's `writable`, read with the server's folders and again after a
  reconnect whose role changed. `write_problem(key)` says why not ("Home lets this computer view
  only…", "needs an update", "isn't answering"); greyed menu items carry it as their tip, a key
  or a drop beeps and shows it.
- **The seam**: every change goes through `shell/file_transfer.h` (`mixed_target` over the host's
  `remote_ops`: the library on the Mac, the core's `mv_remote_*` 0.45 calls on Windows) and the
  drop rule in `shell/file_drop.h`. Up is an upload (the service never replaces: " (2)"; it
  resumes after a dropped link or an offset refusal); down is a fetch then a verified local copy,
  and a move off the server deletes there only after that copy reads back; within one server a
  move is the service's own. A remote folder moves only within its server; nothing goes from one
  server to another.
- **A role changed while linked**: the role comes once, at hello. The service re-reads its lists
  every 2 s and cuts a link whose limit changed; the desktop learns the new role on its next
  request. Taken away: the next change is refused `not_allowed`, and the desktop reconnects and
  re-reads the roots at once (row and `can_write` say "view" from then). Given: the next listing
  (opening or refreshing a folder) reconnects and re-reads them. No timer; neither side restarts.
  A `busy` refusal (a write open there) says "try again in a moment", never a failure.
- **Fresh at once**: each change drops the kept listing and stamps it touched; the open remote
  folder and the folder pane list again, keeping the selection.

Not yet: downloading a whole remote folder, a copy between two servers, cancelling an upload on
Windows (the core's add has no cancel handle there), and the server-side "restore from trash".

## 6a. The share in the Finder (Mac) — built

Owner's ask (2026-11-27): when the Mac reaches the paired server at home, its folders are also an
ordinary SMB volume in the Finder, so other apps reach the files too. The host already makes one
LAN-only SMB share per named folder, called `share_users::share_name(label)`, for the users an
admin adds ([self-host-users-smb.md](self-host-users-smb.md)). The desktop only mounts them.
Owner's calls, the same day:

0. **Its own account (2026-11-27, self-host-users-smb.md §12):** a paired Mac is a user of the
   share in its own right. Once the host's administrator finished it on the card, the server
   hands this Mac its login at each connect from home (`library::share_login`), and the mount
   uses it with no sign-in at all. What follows is for a server from before that, or an account
   still waiting.
1. **Sign-in:** macOS's own dialog the first time, with its "Remember this password in my
   keychain". MediaViewer never sees, keeps or logs the password. The pairing carries no SMB
   account, because an SMB password is a share user's OS account.
2. **On by default, silent:** when a server answers, its shares mount with the sign-in the
   Keychain has (NetFS with `UIOption = NoUI`). Without one, the dialog is never shown on its own.
   A notice says "…need a sign-in once: File > Mount Server Shares in Finder."
3. **Every shared folder**, one volume each. A share already mounted is skipped (matched by host
   and share name, without case; NetFS's `EEXIST` counts as mounted).
4. **Away from home, nothing:** a mount needs the latest connect to have been on the server's LAN
   route (not `over_internet`, not relayed) and the address to be a LAN one (RFC 1918, 169.254/16,
   IPv6 ULA, `.local`). Shares already mounted are left to macOS.

**Pieces.** `remote/lan_share.*` (portable, pure) decides: `lan_host`, `share_url`
(`smb://192.168.0.79/Photos`), `parse_mounted_from` (the `statfs` "mounted from" text),
`plan_mounts` and the notice words. `remote::library` says when a server comes online
(`config::on_online`: at pairing, at the first connect of a run, and after it was unreachable) and how it
was reached (`last_route`). `shell/lan_share_mac.mm` lists the SMB volumes (`getfsstat`,
`MNT_NOWAIT`), mounts through `NetFSMountURLAsync` on its own queue, one share after another, and
unmounts with `NSFileManager`'s asynchronous unmount. Nothing runs on the main thread's time.

**Where.** Settings → Servers (shown once a server is paired): "Mount shares in the Finder at
home" (the `mv.lanShare.autoMount` default), with Mount and Unmount. File → Mount Server Shares in
Finder (the OS sign-in may show) and Unmount Server Shares (every SMB volume of a paired server's
LAN host, nothing else). The server row's context menu has the same pair. Windows: not built
(Explorer maps `\\PC\Photos` itself).

**Tests.** `[lan-share]` (the decision, the URLs, the mount table, the words) and
`[desktop-client]` "a desktop says when a server answers, and by which route" (loopback).

## 7. Not in this slice

- Streaming playback through `read_range` (seek before the clip has arrived).
- The catalogue: searching the host's `.mediaviewer` from the desktop as the phone does.
- `mediaviewer://pair` as a registered link on the desktop; a QR read from a webcam.
- Linux (its host has no folder tree yet).

## 8. Verify

> **Verify:** `mv_tests "[desktop-client]"` (loopback only): a desktop pairs from the code alone
> (no passphrase), lists the host's folders without the Catalogue, opens a still and a clip, reads a
> range, refuses a write locally, says unreachable / offline / revoked plainly, and a second run
> reads nothing from the network for what it cached. `mv_tests "[.perf-bench]"` gains
> *remote first pixel* (tile, cold), *remote full resolution* and *remote arrow-to-next* (cached
> neighbour, cold) over loopback. Local folders are not slower: `--browse-soak` and the local
> `[.perf-bench]` rows, base vs new, alternated, same machine. Both present-loop gates hold.

## 9. As built (2026-11-14, draft PR on `claude/self-host-integration`)

- **Core** `src/remote` as §4; the phone's `link.*` / `link_payload.*` shared through the
  `mv_mobile_route` OBJECT library (its objects go into `libmv_mobile_link.a` too, so the iOS project
  is unchanged; `mv_mobile_link_tests "[mobile-link]"` passes, 51 cases). Each folder's last listing
  is kept on disk (`<cache>/<server>/listings/`), so an unreachable server shows the folder as it
  was and what was fetched still opens. The head-read tile is for RAWs only: a JPEG cut at 1 MiB
  decoded to a wrong picture (found by the bench). The 8 most recently used originals are never
  trimmed (the one on screen and its ±2); tiles are not capped yet (a few tens of KiB each, cleared
  with Forget).
- **Mac**: File → Connect to a Server… (paste, then the fingerprint to compare, then connect on a
  worker), the server rows above the tree in `FolderTreeView.swift` (expand to folders, Forget in
  the context menu), a folder opens as a virtual list (`folder_model` now takes one provider per
  prefix), an item shows its tile then its original, the ±2 fetched behind it (clips over 256 MiB
  wait), `write_guard` refuses every write on a `remote:` key or the cache with its own words.
  Bridge: `mv_chrome_remote_*`; `mv_chrome_remote_pair(code)` is the hook for the Phone access add-on.
- **Windows**: `mv_folder_open` / `mv_list_subdirectories` take a folder key; the session's thumb job
  makes the tile, its decode job shows the tile then fetches the original (neighbours: stills up to
  256 MiB), `mv_remote_*` (ABI 0.44) for the C# tree's rows and its paste-to-connect panel
  (`IslandHost.Remote.cs`). Native `open_folder` skips siblings and the trail for a key.
- **Measured** (Mac, Release, loopback, `mv_tests "[.desktop-client-perf]"`, 12 JPEGs 4032×3024,
  9.9 MiB each, three runs): list 5.0–6.9 ms; **remote first pixel (tile, cold) 79–88 ms**; remote
  full resolution (tile, fetch, full decode, cold) 139–155 ms; **arrow to a prefetched neighbour
  56–60 ms**, to a cold one 78–87 ms; the same files from the local disk: first pixel 52–57 ms, full
  59–64 ms. Local folders, base vs new alternated on the same machine (other sessions were building,
  so these are noisy): `--browse-soak` on 40 local JPEGs, five base and three new runs, no timeouts,
  cold arrow medians base 80–138 ms / new 88–108 ms, warm base 0.27–0.41 ms / new 0.35–0.38 ms
  (overlapping); `mv_tests "[.perf-bench]"` decode and list rows overlapping (base 128–200 ms, new
  153–200 ms for the 24 MP decode). The local paths gain one prefix compare per selection.
- **Measured, Windows** (Ryzen 7 5700X3D, Release, the same test, three runs, 2026-11-18,
  `docs/perf/windows-desktop-client.json`): list 25–29 ms; remote first pixel 131–143 ms; full
  resolution 217–229 ms; arrow to a prefetched neighbour 76–78 ms, to a cold one 93–96 ms; local
  disk: first pixel 72 ms, full 87–88 ms. The core path only: the WinUI paste panel and tree rows
  were not driven on screen.
- **Owed**: either host driven on screen against a real `driftwire host` (the Mac half is built and
  its core tested; the Windows native half is compiled only in CI, the C# compiled here); the
  Windows "Downloading… %" notice (the Mac shows it); streaming playback through `read_range` (a
  clip plays once fetched); write access with #686 §2's roles (`smb-users`' #690 has no per-device
  role yet: proposed `grant_role` in caps); a desktop link for `mediaviewer://pair`; a run against a
  Synology; both present-loop gates (the change does not touch the present loop; not re-run).
