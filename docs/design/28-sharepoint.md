# 28 — Sharepoint: send files to a MediaViewer nearby

How one MediaViewer sends photos, video and audio to another, the way a nearby share does: the
two clients find each other, both show the same short code, and nothing moves until the person
on the other side accepts. Issues [#410](https://github.com/longtimeno-c/mediaviewer/issues/410)
(the add-on and its window) and [#413](https://github.com/longtimeno-c/mediaviewer/issues/413)
(the gallery's range, drag-out and share icon). Built 2026-10-15; where each part stands is in
[DEVELOPMENT.md](../DEVELOPMENT.md#sharepoint-issues-410-413-2026-10-15).

Sharepoint is a first-party add-on (option B of [25 §9](25-open-addons.md#9-code-how-a-strangers-add-on-runs),
the same kind as Import and Places). It carries its own code, has no chrome of its own, and
fills one window through the screens contribution point ([25 §10](25-open-addons.md#10-screens)):
both hosts draw it with their own controls, so it follows the theme, text size, VoiceOver and
Narrator. Other makers' script add-ons still have no network permission (owner, 2026-10-03).

## What a person sees

- **The window**, "Sharepoint", opened from its row in Settings → Add-ons (**Open Sharepoint**),
  from its command (the palette; no default key, issue #410), and from the share menu's first
  entry ([16 "Share"](16-commands.md#share-444)): the gallery's share icon, the bar's share button,
  `Ctrl+Shift+S` and right-click Share ▸. Its manifest's own **Send with Sharepoint…** context
  entry is left out of the hosts' right-click menus (#444), so there is one route. It lists:
  - this computer's name as others see it, and **Keep receiving when this window is closed**;
  - someone connecting (their name and the **code**, **Decline**), then their offer (the files,
    **Save to** with Choose…, **Accept** / **Decline**), then progress and **Cancel**, then what
    arrived;
  - the files to send, **Send a folder** (Choose…), **Clear**;
  - **Nearby**: the MediaViewers it can see, each with how ("Wi-Fi", "Bluetooth" or both);
  - **Send to …**, then the code and **Same code, send** / **Cancel**: the person checks that the
    other screen shows the same code before anything about the files leaves; then it waits for
    Accept, then progress and **Cancel**.
- **The gallery** ([16](16-commands.md#gallery-range-and-share-413)): Shift-click selects a range,
  dragging a cell drags it out, and one share icon opens the share menu ([16 "Share"](16-commands.md#share-444)),
  whose first entry is Sharepoint with the selection (issue #444: the same menu as `Ctrl+Shift+S`,
  the bar's share button and right-click Share ▸).
  Without Sharepoint the icon offers the same one-click install as the other add-ons.

## Finding each other

Presence runs only while the window is open, while **Keep receiving** is on, or while a transfer
runs. With the window closed and receiving off there is no socket, no beacon, no Bluetooth and no
thread: the idle viewer announces nothing and polls nothing. Nothing in the add-on runs before the
host loads it after first pixel, and the engine is made at the first open or command, not at load.

| Bearer | Announce | Hear | Carry |
|---|---|---|---|
| **Wi-Fi** (`presence.cpp`, `net_*`) | A UDP beacon a second, port 47613, to the group 239.255.77.83 and the subnet broadcast, TTL 1 | The same socket, shared with other windows on the machine | One TCP connection to the beacon's address and port; the listener's port is the OS's pick |
| **Bluetooth** (`bt_mac.mm`, `bt_win.cpp`) | A GATT service advertised connectable | A scan filtered to that service; each new device is read once for its INFO (id, name) | The service as a byte pipe (`bt_pipe.h`): C2P written without response, P2C notifying |

- A **beacon** is `MVSP1` and `{"v","id","name","port"}`: a random session id (16 hex digits, new
  each run), the machine's name (the Sharing name on the Mac, the computer name on Windows) and a
  port. Never a path, a name of a file or anything about one.
- A peer leaves the list 4.5 s after its last beacon or advertisement. One heard on both bearers
  is one row.
- **Bluetooth LE between a Mac and a Windows PC** has one common way to move bytes: GATT. Windows
  gives apps no connection-oriented LE channel. GATT moves tens of kilobytes a second, so a set
  over **32 MiB never goes over Bluetooth** ([12](12-decision-log.md#sharepoint)); Wi-Fi carries
  anything. Picking a peer heard only on Bluetooth with such a set shows, before Send, why and
  what to do: put both computers on the same Wi-Fi network, or, when this computer has no
  network, turn on Wi-Fi here. Send then reads **Send to … over Wi-Fi** and waits ("Waiting for
  … on Wi-Fi", with Cancel); the moment that peer's beacon arrives the send starts by itself, code
  check and Accept as always. Presence stays up while it waits, window closed or not.
- Sending prefers Wi-Fi when the peer's beacon is fresh, and falls back to Bluetooth.

## The wire

`wire.h`, `channel.h`. One session, either bearer; the bearer moves bytes and decides nothing.

1. **Commit, answer, reveal**, each with an X25519 key made for this link alone: the client (the
   side that connected, the sender) sends `MVSP`, the version and a BLAKE2b hash of its public
   key; the server (the receiver) answers `MVSP`, the version and its public key; the client then
   reveals its key, which must hash to what it sent. The client is bound to its key before it
   sees the server's, and the server shows its key only after that, so a machine in the middle
   cannot search for keys that give both people the same digits (the ZRTP / Bluetooth numeric
   comparison pattern). Another version is refused at the hello.
2. Both derive one key per direction (libsodium `crypto_kx`) and open an XChaCha20-Poly1305
   secretstream each way. Every later message is encrypted, authenticated and in order: one
   altered, dropped, replayed or reordered byte ends the link.
3. **The code** is six digits from a BLAKE2b hash of both public keys, the client's first. Both
   windows show it from here on: a machine in the middle shows each side a different one.
4. The first sealed message on each side is who it is (`ident`: id, name). Names on screen are
   these, not the beacon's.
5. **The sender's person confirms** that the other screen shows the same code (**Same code,
   send**). Until then nothing about the files leaves, not even a name: a machine that only
   claimed the peer's id, or one in the middle, shows the receiver no code or another one.
6. The sender's **offer**: the count, the total, and each file's relative path and size. No hash
   up front: the sender would have to read every file twice before the first byte moved.
7. The receiver answers `accept`, or `decline` with `declined`, `busy` or `no_room`. Closing the
   window, or three minutes without an answer, declines; a receiver already in a transfer answers
   `busy` straight after the handshake.
8. Each file: `file_begin` (index, size, mtime), `data` chunks of 256 KiB, `file_end` with the
   BLAKE3 of the bytes as read; or `file_skip` for one that is gone or changed since the offer.
   The receiver answers each with `file_done` (verified or not) and the end with `done`.
9. `cancel` from either side ends it (sent only if the link is free, never behind a chunk). A ping
   goes out whenever a side has sent nothing for 2 s and 20 s of silence is a dropped link; a
   frame must arrive whole within 20 s of its first byte. Every wait before Accept is a deadline
   (the handshake 8 s, the code and the answer 3 minutes) that pings never extend, and a
   handshake still running is ended by Cancel or by the add-on stopping.

**Who can take what.** Bytes move only from a sender to the peer its person picked, after both
people saw the same code and the one at that peer accepted. A sender never answers a request for
files, so a third machine that hears the beacon learns a name and a port, and nothing else. A
peer heard in the last 4.5 s keeps its address and device: a beacon claiming its id from another
address is ignored, and the list holds at most 64. A receiver already in a transfer answers
`busy`; at most four such refusals run at once.

## What moves, and how it lands

`transfer.h`. Copies only: an original is only ever read (rule 5).

- **What is offered**: the files a command, the context entry or the share icon hands over (the
  gallery range, the marks, or the current file), each with its pair half (RAW + JPEG, Live Photo,
  through the host's `pair_names`) and its XMP sidecars (`IMG_1.xmp`, `IMG_1.CR2.xmp`), one listing
  per folder. Or a folder the person picks, recursively (`walk_files`, depth 32), keeping its
  shape under its own name, without housekeeping (`@eaDir`, `#recycle`, `$RECYCLE.BIN`, `System
  Volume Information`, `lost+found`, `__MACOSX`, `Thumbs.db`, `desktop.ini`, dot files, our own
  `.mvtmp`).
- **Where it lands**: the folder the person picks under **Save to**, else
  `<the default photo folder>/Received`, made on Accept. Every relative path passes
  `safe_relative` (`/` only, no `..`, no root, no drive or stream, no device name, no trailing dot
  or space, valid UTF-8, at most 1024 bytes and 32 levels): one bad path refuses the whole offer.
  A free-space check (the volume's free bytes less 64 MiB) answers `no_room` before any byte moves.
- **The write follows `io::verified_copy`'s rules** ([24](24-transfer.md)) on bytes that come from
  a link: a sibling `<final>.mvtmp` created exclusively, hashed as written; at `file_end` the hash
  must be the sender's, the file is flushed to stable storage, read back uncached and hashed
  again; only then is it renamed into place with no-replace. A taken name keeps both
  (`name (2).ext`, `io::unique_name`). A mismatch, an error, a cancel or a dropped link removes the
  temporary. Files that finished stay; nothing partial is left. The file's mtime is kept.
- Between chunks both sides wait while the viewer presents (the host table's `should_yield`, as
  Import does), so a pan keeps its frames.

## Threads

The host calls in on its worker for this add-on (open, close, clicks, commands). The engine
(`engine.h`) keeps its own threads for what waits on the network: a control thread (the only one
that starts and stops presence, and sends throttled posts), the listener's accept loop, the
discovery thread, Bluetooth's queue or handlers, the one transfer, and a folder reader. None is a
UI or render thread; no lock is held across I/O; a Bluetooth callback that hands a link to the
engine goes off the radio's queue and through a gate the bearer closes before it stops. The
window is posted whole (`build_tree`, a pure function of the state, tested), at most ~10 times a
second while bytes move; those posts update chrome and never present the canvas.

## Privacy (rule 6)

- Nothing about a transfer goes to telemetry or a crash report: no path, file name, pixel, EXIF,
  address or peer name is logged (`log` takes ASCII without them) or annotated. The minidump
  scrub ([13](13-updates-and-telemetry.md)) applies unchanged.
- The beacon carries a random id, the machine's name and a port; the offer travels sealed, to the
  peer picked. No account, no cloud, no internet relay: the beacon's TTL is 1 and nothing resolves
  a name.
- Receiving is off until the window opens or the person turns it on, for this run of the app.

## Host pieces this added

- **Screens vocabulary**: a `text_field` may carry `"pick": "folder"`; the hosts add **Choose…**
  (`NSOpenPanel`; WinUI's `FolderPicker`) and report the folder as the field's change
  ([25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)).
- **Commands of an add-on that fills slots** become command events to it
  (`slot_registry::run_command`, `mv_slots_command`, ABI 0.29, [14](14-abi.md)): Sharepoint's key-less
  "Sharepoint" row carries the marked or current files.
- **Settings → Add-ons**: an add-on that fills slots and has a window gets **Open …**; Sharepoint
  is offered with one click while it is not installed (its release channel,
  `mediaviewer-addon-sharepoint-<platform>`).
- **The gallery's range** on both hosts ([16](16-commands.md#gallery-range-and-share-413)).
- **App permissions**: the Mac's Info.plist names `NSLocalNetworkUsageDescription` and
  `NSBluetoothAlwaysUsageDescription` (macOS asks once each, the first time Sharepoint looks).
  Windows asks through Defender Firewall at the first listen.

## Where it lives

| Piece | Code |
|---|---|
| Wire, code, path rule, beacon | `src/addons/sharepoint/wire.*` |
| Sealed channel | `channel.*`, `link.h` |
| Sending and the verified receive | `transfer.*` |
| Engine, presence, the window's tree | `engine.*`, `presence.*`, `tree.cpp` |
| Sockets, machine name | `net.h`, `net_posix.cpp`, `net_win.cpp`, `machine_mac.cpp` |
| Bluetooth | `bt_pipe.*`, `bt_mac.mm` (CoreBluetooth), `bt_win.cpp` (C++/WinRT), `bt_none.cpp` |
| The add-on's export and manifest | `addon_entry.cpp`, `manifest.json`; `cmake/sharepoint.cmake` |
| Mac chrome | `Sharepoint.swift` (offer, share icon), `SlotItems.swift` (Settings), `ScreenView.swift` (Choose…), `GalleryView.swift` / `FolderStore.swift` (range, drag) |
| Windows chrome | `IslandHost.Sharepoint.cs`, `IslandHost.GalleryRange.cs`, `IslandHost.Slots.cs`, `IslandHost.Screens.cs` |
| Native hosts | `src/shell/main_mac.mm`, `src/shell/main.cpp`, `src/shell/addons_mac.mm` |
| Tests | `tests/test_sharepoint.cpp`, `tests/test_sharepoint_addon.cpp` (`mv_import_tests "[sharepoint]"`), `tools/package/test_addon_pack.py` |

## Not built

- Bonjour / DNS-SD discovery (the beacon is one portable socket on both OSes); IPv6.
- Receiving across launches (the toggle is for this run).
- A peer that is neither on the LAN nor in Bluetooth range: no relay, by design.
- Resuming a transfer that dropped: finished files stay, the rest is sent again.
