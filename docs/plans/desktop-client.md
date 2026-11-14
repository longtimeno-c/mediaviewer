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
  app's data folder; no key is in that file. The service lists the desktop with the phones under
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

## 6. Write access (owner call 1, with #686 §2)

The service already says, per folder, whether this device may write there (`root_info.writable`),
and the write op is the backup write path's, with its checks. The role (*can change files*) that
an admin grants a desktop, from the app or `driftwire` on the host, is #686 §2's work
(`smb-users`, through `src/wire/grants.*`). When it lands, the desktop reads `writable` and lets
Move, rename, Trash and metadata through to a service write op; until then it is read-only whatever
the service says.

## 7. Not in this slice

- Streaming playback through `read_range` (seek before the clip has arrived).
- The catalogue: searching the host's `.mediaviewer` from the desktop as the phone does.
- Write access (§6).
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
