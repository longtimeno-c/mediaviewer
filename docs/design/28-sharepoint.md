# 28 — Sharepoint: send files to a MediaViewer nearby

How one MediaViewer sends photos, video and audio to another, the way a nearby share does: the
two clients find each other, the receiver shows a short code that the sender types, and nothing
moves until the person on the other side accepts. Issues [#410](https://github.com/longtimeno-c/mediaviewer/issues/410)
(the add-on and its window) and [#413](https://github.com/longtimeno-c/mediaviewer/issues/413)
(the gallery's range, drag-out and share icon). Built 2026-10-15; where each part stands is in
[DEVELOPMENT.md](../DEVELOPMENT.md#sharepoint-issues-410-413-2026-10-15). Protocol 2 (issue
[#472](https://github.com/longtimeno-c/mediaviewer/issues/472) PR 1, 2026-10-18): a Noise handshake
with a post-quantum hybrid, and a transfer that survives its link, moving between Bluetooth and
Wi-Fi with no new code and no new Accept ([The wire](#the-wire), [The session](#the-session-protocol-2-472)).

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
  entry is left out of the hosts' right-click menus (#444), so there is one route. Its design is
  AirDrop's (#442), the same on both hosts, top to bottom:
  - this computer's name as others see it, and **Keep receiving when this window is closed** (kept
    across launches; the command bar shows **Sharepoint: receiving** while it is on), then
    **Receive when MediaViewer is closed** (#452, off by default; [In the background](#in-the-background-452));
  - **what is offered to this computer**, in a card of its own: the sender's round tile (its
    monogram, name and how it is connected), "… wants to send you files" and the **code** in large
    digits to type over there, **Decline**; then "… wants to send you 24 files, 412 MB", the first
    names, **Save to** with Choose…, **Decline** / **Accept**; then a progress ring with its
    percentage, the count and **Cancel**; then what arrived and **OK**;
  - **Send**, a card: a strip of the first six files' thumbnails (the viewer's cache), the count
    and size, the first names, **Send a folder** (Choose…), **Clear**;
  - **Nearby**: the MediaViewers it can see as large round tiles (a monogram, the name, "Wi-Fi",
    "Bluetooth" or both). **One click on a tile sends** to it. A card then asks for the code the
    other screen shows, in one large field that takes digits only and goes by itself at the sixth
    (three wrong codes end the link; this screen never shows the code); then "Waiting for … to
    accept"; then the tile's own ring fills while the bytes move, and it ends with a done or
    failed mark on the tile and a line saying what happened.
- **The gallery** ([16](16-commands.md#gallery-range-and-share-413)): Shift-click selects a range,
  dragging a cell drags it out, and one share icon opens the share menu ([16 "Share"](16-commands.md#share-444)),
  whose first entry is Sharepoint with the selection (issue #444: the same menu as `Ctrl+Shift+S`,
  the bar's share button and right-click Share ▸).
  Without Sharepoint the icon offers the same one-click install as the other add-ons.

## Finding each other

Presence runs only while the window is open, while **Keep receiving** is on, or while a transfer
runs. With the window closed and receiving off there is no socket, no beacon, no Bluetooth and no
thread: the idle viewer announces nothing and polls nothing. Nothing in the add-on runs before the
host loads it after first pixel, and the engine is made at the first open or command, not at load,
unless Keep receiving was on.

**Keep receiving is kept across launches** (#442, [12](12-decision-log.md#sharepoint)). The toggle
is written to the add-on's prefs (`prefs.h`, `sharepoint.json` in its data folder) on the engine's
control thread. At load, `mv_addon_get` reads that file (one small read); when the toggle was on it
makes the engine, whose control thread starts presence. The host loads slot add-ons on its add-on
queue (Mac) or a worker (Windows) after first pixel, so presence never starts before first pixel
or on the foreground queue. Each window is its own process ([18](18-import.md) "One host
process"): only the one holding `sharepoint.json.lock` (`net::try_lock_instance`: `flock` on the
Mac, an unshared handle on Windows) starts receiving at load, so one machine shows up once. While
the toggle is on, the command bar shows **Sharepoint: receiving** (a status item, both hosts), so
a person can see this machine is discoverable after a relaunch. Off at load: the read, and nothing
else (no engine, thread, socket or lock).

Idle cost with it on and the window closed (`mv_import_tests "[.sharepoint-idle]"`, Mac M-series,
15 s per phase, the real UDP beacon, listener on loopback, no Bluetooth in the test build): off
0.0003 % CPU and 0.1 context switches a second; on 0.020 % (2.9 ms of CPU in 15 s) and 6.4 switches
a second (the beacon once a second, the listener's 200 ms poll); off again 0.0001 %. No present:
the engine never touches the canvas.

| Bearer | Announce | Hear | Carry |
|---|---|---|---|
| **Wi-Fi** (`presence.cpp`, `net_*`) | A UDP beacon a second, port 47613, to the group 239.255.77.83 and the subnet broadcast, TTL 1 | The same socket, shared with other windows on the machine | One TCP connection to the beacon's address and port; the listener's port is the OS's pick |
| **Bluetooth** (`bt_mac.mm`, `bt_win.cpp`) | A GATT service advertised connectable | A scan filtered to that service; each new device is read once for its INFO (id, name) | The service as a byte pipe (`bt_pipe.h`): C2P written without response, P2C notifying |

- A **beacon** is `MVSP1` and `{"v","p","id","name","port"}`: a random session id (16 hex digits,
  new each run), the machine's name (the Sharing name on the Mac, the computer name on Windows), a
  port, and `"p"`, the highest protocol it speaks (#472; `"v"` stays 1, which is what a protocol 1
  client checks, so old and new still see each other; no `"p"` is protocol 1). Bluetooth's INFO
  carries the same `"p"`. Never a path, a name of a file or anything about one.
- A peer leaves the list 4.5 s after its last beacon or advertisement. One heard on both bearers
  is one row.
- **Bluetooth LE between a Mac and a Windows PC** has one common way to move bytes: GATT. Windows
  gives apps no connection-oriented LE channel. GATT moves tens of kilobytes a second. To a
  **protocol 2** peer (#472) a set of any size starts on Bluetooth when that is all there is, and
  the session moves to Wi-Fi the moment the peer is heard there ([The session](#the-session-protocol-2-472)).
  To a **protocol 1** peer a set over **32 MiB never goes over Bluetooth**
  ([12](12-decision-log.md#sharepoint)); Wi-Fi carries anything. A protocol 1 peer heard only on
  Bluetooth, selected with such a set, shows why and what to do:
  put both computers on the same Wi-Fi network, or, when this computer has no network, turn on
  Wi-Fi here. A click on its tile then waits ("Waiting for … on Wi-Fi", with Cancel); the moment
  that peer's beacon arrives the send starts by itself, code and Accept as always. Presence stays up while it waits, window closed or not.
- Sending prefers Wi-Fi when the peer's beacon is fresh, and falls back to Bluetooth.

## In the background (#452)

**Receive when MediaViewer is closed** is opt-in and off by default, like AirDrop or Nearby Share
being reachable. It implies Keep receiving (turning that off turns this off too) and is kept in
`sharepoint.json` with it. It is the same app, not a helper: the add-on asks the host to keep its
process (`keep_alive`), and the hosts do the rest.

- **Off** (the default) changes nothing: the last window closing quits, ⌘Q / Alt+F4 quit, and
  with Keep receiving off there is no process, socket, beacon or Bluetooth after quitting.
- **On**, closing the last window, ⌘Q or the Dock's Quit on the Mac, and closing the window on
  Windows, leave the process running. The viewer drops what it showed (no clip plays, the folder
  and its watch close; `returnToWelcome` / `return_to_welcome`) and its window is hidden, not
  closed. On the Mac the app then leaves the Dock (accessory policy) and a **menu-bar item**
  stays; on Windows a **notification-area icon**. Both menus: "Sharepoint: receiving", **Open
  Sharepoint**, **Open MediaViewer**, **Quit MediaViewer** (which really quits). Opening the app
  again (Finder, Spotlight, Start, a file) brings the hidden window back. Logout, restart and shut
  down, and an update's restart, quit as before. Only the process that holds
  `sharepoint.json.lock` keeps itself; another window's process quits as usual.
- **A notification when someone sends** (both hosts, background or not). The engine posts a
  notice (`notice_post`) the moment a sender's link is up, while its code shows: "Ann's MacBook
  wants to send you files. Open Sharepoint to see the code." Once the sender has typed the code
  and the offer arrives: "… wants to send you 24 files, 412 MB. Open Sharepoint to accept." It
  never shows the code or a file name (rule 6: a lock screen shows it), only the sender's name, the
  count and the size. **Clicking it opens the Sharepoint window** with the card and its code;
  its one button is **Decline** (the window's own Decline). Accept is only in the window, after
  the code: nothing is accepted from a notification. It is taken back however the offer ends.
- The host skips the notification when the Sharepoint window is the front window of the active
  app (the card is in plain sight), and shows it when the window is closed, behind another window
  or the app is in the background: the Keep receiving case gets it too.
- **Mac:** `UNUserNotificationCenter` (`AddonNotices.swift`). Permission is asked the first time
  either toggle is turned on (`notice_authorize`), never at launch. Without permission, or in the
  unbundled lab binary (no notification centre), the window opens and the Dock bounces, as
  before #452. Import's finished-job notice (`notifyTitle:`) now uses the same centre when
  allowed, and beeps otherwise.
- **Windows:** `AppNotificationManager`, registered once for the unpackaged build under the
  process's AUMID (`MediaViewer.Viewer`, set before the first window), on the first ask or notice
  (`IslandHost.Notices.cs`); before #452 `Show` threw without a registration and Import's notices
  never appeared. Where Windows refuses, the window opens instead. A notification clicked after
  the process ended starts the app normally (`----AppNotificationActivated:` is accepted).
- **Idle cost, windowless:** see [12](12-decision-log.md#sharepoint) and DEVELOPMENT.md. The
  canvas presents nothing while hidden.
- **It starts at login / sign-in** (2026-10-17), so a restart does not leave the machine
  unreachable until someone opens MediaViewer. While the toggle is on the host keeps a login item:
  on the Mac `SMAppService.mainApp` (System Settings → General → Login Items lists MediaViewer);
  on Windows a per-user Run value, `HKCU\…\CurrentVersion\Run\MediaViewer` =
  `"<install>\MediaViewer.exe" --background` (Settings → Apps → Startup lists it;
  `src/shell/login_item_win.*`). The host follows the keep-alive list: it registers when the list
  turns non-empty and removes the item when it turns empty, and only on that edge (a window whose
  add-on never kept the process, or the list emptying while the app quits, leaves it alone). The
  Windows uninstaller hook removes it too. Installed builds and the bundled app only; a dev build
  or the lab binary registers nothing.
- **That start is the background, not a window.** On the Mac a login item's launch (the
  open-application event's `keyAELaunchedAsLogInItem`) and on Windows `--background` make the
  viewer window but never show it: the app is an accessory app (Mac) with no taskbar button
  (Windows), the add-ons load as on any start, and Sharepoint's keep-alive brings up the menu-bar
  item / notification-area icon. Opening MediaViewer, a file or the icon's **Open MediaViewer**
  shows the window as from the background. A Windows `--background` start never forwards to a
  viewer already running; it exits. **Self-cleaning:** if no add-on keeps the process within
  60 s (the add-on was removed, or the toggle was turned off where this process could not see it),
  the start removes its own login item and quits, so an orphaned entry costs one quiet launch.
  `--background` also starts the Mac host this way, for testing.

## The wire

`wire.h`, `noise.h`, `channel.h`. One session, either bearer; the bearer moves bytes and decides
nothing. A client speaks the protocol the peer's beacon or INFO announces (`"p"`); a receiver
answers whichever hello arrives, so protocol 1 peers (from before #472) still send and receive on
the LAN and Bluetooth. They get no rejoin and no move between bearers.

**Protocol 2 (#472)**, `Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b`:

1. The client (the side that connected, the sender) sends `MVSP`, 2, `N` and the first Noise
   message: an X25519 key and an **ML-KEM-768** key made for this link (the "hfs" modifier), and as
   its payload a BLAKE2b commitment to a random 32-byte nonce.
2. The server (the receiver) answers: its X25519 key, `ee`, the ML-KEM ciphertext encapsulated to
   the client's key (its shared secret mixed into the chaining key beside the X25519 ones), its
   static key and `es`. A recording of the link needs both X25519 and ML-KEM-768 broken to read.
3. The client sends its static key, `se`, and reveals the nonce, which must match the commitment.
   Statics are a key the engine makes once a run (the XX shape is there for remembered devices,
   #472 PR 4); nothing on the LAN or Bluetooth trusts them yet.
4. **The code** is six digits from a BLAKE2b hash of the handshake hash after message 2 and the
   nonce. Everything the server contributes is fixed before the client's nonce is seen, and the
   nonce is fixed before the server speaks, so a machine in the middle cannot search for keys that
   give both people the same digits (protocol 1's commit-then-reveal, carried into Noise). Only the
   receiver's window shows it.
5. Both sides open an XChaCha20-Poly1305 secretstream each way keyed from the handshake's Split, and
   keep Split's third output as the session's **resume secret** (memory only). Ephemerals are wiped
   (`sodium_memzero`) as the handshake ends. Every later message is sealed with a sequence number
   and an acknowledgement ([The session](#the-session-protocol-2-472)).

The Noise core is checked against the published Cacophony vectors (`Noise_XX` and `Noise_NNpsk0`
over 25519, ChaChaPoly and BLAKE2b); the hybrid patterns have fixed-key regression vectors and
tests that the ML-KEM secret is in the keys and a tampered key or ciphertext fails
(`tests/test_sharepoint_noise.cpp`). libsodium 1.0.22 (already pinned) has ML-KEM-768.

**Protocol 1** (a peer from before #472): commit, answer, reveal, each with an X25519 key made for
this link alone: the client sends `MVSP`, 1 and a BLAKE2b hash of its public key; the server
answers `MVSP`, 1 and its public key; the client reveals its key, which must hash to what it sent.
Both derive one key per direction (`crypto_kx`) and open the same secretstreams. The code is six
digits from a hash of both public keys. Another version is refused at the hello.

Then, on either protocol:

1. The first sealed message on each side is who it is (`ident`: id, name). Names on screen are
   these, not the beacon's.
2. **The sender's person types the code** the receiver's screen shows (#442, passkey entry rather
   than numeric comparison: typing makes the person read the other screen). The sender checks it
   on its own machine against the code it derived, digits only, in constant time; a partial entry
   is not a try. A wrong one says so and how many tries are left; the third wrong one sends
   `cancel` and ends the link. Until a right one nothing about the files leaves, not even a name:
   a machine that only claimed the peer's id, or one in the middle, shows the receiver no code or
   another one, so what the person types cannot match.
3. The sender's **offer**: the count, the total, and each file's relative path and size. No hash
   up front: the sender would have to read every file twice before the first byte moved.
4. The receiver answers `accept`, or `decline` with `declined`, `busy` or `no_room`. Closing the
   window, or three minutes without an answer, declines; a receiver already in a transfer answers
   `busy` straight after the handshake.
5. Each file: `file_begin` (index, size, mtime), `data` chunks of 256 KiB, `file_end` with the
   BLAKE3 of the bytes as read; or `file_skip` for one that is gone or changed since the offer.
   The receiver answers each with `file_done` (verified or not) and the end with `done`.
6. `cancel` from either side ends it (sent only if the link is free, never behind a chunk).
   Protocol 1: a ping goes out whenever a side has sent nothing for 2 s, 20 s of silence is a
   dropped link, and a frame must arrive whole within 20 s of its first byte. Protocol 2: below.
   Every wait before Accept is a deadline (the handshake 8 s, typing the code and the answer 3
   minutes) that pings never extend, and a handshake still running is ended by Cancel or by the
   add-on stopping.

## The session (protocol 2, #472)

The channel is a session above the link (`channel.h`); `transfer.h` sees none of what follows: a
send or a receive waits through a rejoin.

- **Nothing lost, nothing twice.** Every sealed message carries its sequence number and the count
  the sender of it has received (the acknowledgement). A side keeps what the other has not
  acknowledged, at most **16 MiB** (`kReplayBytes`, a budget: [12](12-decision-log.md#sharepoint));
  past it a sender waits, as a full socket would. The receiver acknowledges at least every 1 MiB
  and at once for anything but file bytes; pings (at least every second while a link is idle)
  carry the acknowledgement too.
- **Rejoin over any bearer, no second code.** The client opens a new link and sends `MVSP`, 2, `R`,
  a 16-byte tag (8 random bytes and a keyed hash of them under the resume secret: only the two
  sides make or recognise one, and no two look alike) and a fresh
  `Noise_NNhfs+psk0_25519+MLKEM768_ChaChaPoly_BLAKE2b` handshake with the resume secret as the psk:
  new X25519 and ML-KEM keys, new link keys. The receiver routes it by the tag to the session it
  names (`engine::route_busy`); a tag that names nothing here closes the link. Each side's first
  sealed message on the new link is **`resume_at`**: how many messages it has received. That
  proves it holds the keys (a recorded rejoin played again opens its first message and stops
  there), and the other side sends again, in order, what is past that point. A message that came
  twice (sent on the old link, and again after the rejoin) is dropped by its sequence number.
  Messages are chunk-sized, so this is the issue's "resume at a chunk boundary": the receiver
  keeps its open `.mvtmp`, its BLAKE3 state and its verifier exactly as they were, and nothing
  that arrived is sent again. Every file is still verified end to end.
- **When it moves** (the sender's mover thread, `engine::run_mover`):
  - **Break.** A link that fails, or that is silent for **3 s** (`kSuspectMs`), is suspect: every
    bearer the peer is heard on is raced (`link_race`: Wi-Fi to its fresh beacon, Bluetooth to its
    handle) and the session rejoins on the first that connects; the slower ones close. The window
    says **Reconnecting to …** and the ring holds where it is. **20 s** (`kRejoinMs`) without a
    link, or without a sign of life, ends the session as a lost link ends it today.
  - **Make before break.** A send on Bluetooth whose peer appears on Wi-Fi opens Wi-Fi alongside,
    rejoins there, and closes Bluetooth; the switch loses nothing (what was in flight on Bluetooth
    is sent again from the replay). Wi-Fi is always the faster by far, so this is also the "too
    slow" rule; after a move the next move for speed waits **10 s** (`kSwitchHoldMs`).
  - **Flapping.** Three rejoins in **10 s** end the session.
- **One bad byte ends the link, not the session**: after the handshake, a frame that does not open
  is treated as a dropped link, and the rejoin sends it again. A gap in the sequence numbers (never
  on an ordered link) or an acknowledgement of what was never sent ends the session.
- **The end.** The receiver answers `done` and waits up to 5 s for it to be acknowledged before it
  closes, so a link that drops at the last moment is rejoined rather than the sender waiting out a
  lost `done`.
- Not here yet (#472 PRs 2–5): rejoining over the internet, the internet bearers themselves, and
  the slower too-slow rule between two fast bearers.

**Who can take what.** Bytes move only from a sender to the peer its person picked, after that
person typed the code the peer's screen showed and the person at that peer accepted. A sender never answers a request for
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
- **Where it lands**: the system **Downloads** folder, as AirDrop (`FOLDERID_Downloads` on Windows,
  `~/Downloads` on the Mac; `net::downloads_folder`, #442), made on Accept if it is missing. A sent
  folder keeps its own name inside it. **Save to** picks another folder, and that choice is kept
  across launches in the add-on's own prefs (`prefs.h`: `sharepoint.json` in its data folder,
  written on the engine's control thread); a kept folder that is gone falls back to Downloads
  until another is picked. Until #442 the default was `<the default photo folder>/Received`.
  Every relative path passes
  `safe_relative` (`/` only, no `..`, no root, no drive or stream, no device name, no trailing dot
  or space, valid UTF-8, at most 1024 bytes and 32 levels): one bad path refuses the whole offer.
  A free-space check (the volume's free bytes less 64 MiB) answers `no_room` before any byte moves.
- **The write follows `io::verified_copy`'s rules** ([24](24-transfer.md)) on bytes that come from
  a link: a sibling `<final>.mvtmp` created exclusively, hashed as written; at `file_end` the hash
  must be the sender's, the file is flushed to stable storage, read back uncached and hashed
  again; only then is it renamed into place with no-replace. A taken name keeps both
  (`name (2).ext`, `io::unique_name`). A mismatch, an error, a cancel or a dropped link removes the
  temporary. Files that finished stay; nothing partial is left. The file's mtime is kept.
- **The next file arrives while the last one is checked** (#442). The thread reading the link
  writes and hashes each file; at `file_end`, once the streamed hash is the sender's, the file
  goes to a verifier (two threads, in order, at most four files waiting, each an open
  temporary) that flushes it, reads it back uncached, hashes again, renames it with no-replace
  and sends its `file_done`; the sender's `done` is answered only after every check. Every rule
  above holds for each file exactly as before; only file N+1 no longer waits for file N's flush
  and read-back. A cancel or a dropped link still drops the file being written; files that had
  arrived whole finish their check, as they did when it ran inline.
- Between chunks both sides wait while the viewer presents (the host table's `should_yield`, as
  Import does), so a pan keeps its frames. A rejoin changes nothing here (`should_yield` is
  unchanged).

**Throughput** (`mv_import_tests "[.sharepoint-bench]"`, #442): two engines in one process over
loopback TCP, timed from Accept to the receiver's `done`, against a plain copy of the same files
through a loopback socket (no hash, seal, flush or read-back). Mac M-series (Mac17,4), Release,
warm cache, base and new alternated three times, load average ~5 (other work on the machine):

| Case | Plain copy | Before | After |
|---|---|---|---|
| 500 × 2 MiB | 2,150–2,950 MB/s | 124–141 MB/s | 158–180 MB/s |
| 1 × 1 GiB | 1,285–2,920 MB/s | 251–373 MB/s | 243–273 MB/s (no change: within noise) |

Many files gained from the pipelined check (each file's flush to stable storage is most of its
cost on loopback, and two checks overlap); a single file cannot, and on loopback it is bound by
the seal (libsodium's portable ChaCha20-Poly1305, about a third of the CPU in a profile). Tried
and dropped, because it measured no faster: reading and hashing a few chunks ahead of the seal on
the sender (1 GiB: 373–389 MB/s against 370–385 before). Not tried: larger chunks (the frame limit
is part of protocol 1; an older receiver would refuse the frame), socket buffer sizes (setting
them turns off the OS's autotuning on a real link, which loopback cannot show), and pausing only
for a pan rather than for every present (`should_yield` is the host table's one signal, shared
with Import; it would need a host change on both platforms and the PR 1 gate measured with a
transfer running). Loopback is not Wi-Fi or Bluetooth: no radio was measured here, so the MB/s a
person sees between two machines is not in this table.

**Protocol 2 (#472, 2026-10-18)**, the same bench, base (protocol 1) and new alternated three times,
Mac M-series, Release, warm cache, load average ~4.6: 1 GiB 395.8–397.6 → 392.6–398.2 MB/s, 500 ×
2 MiB 178.9–183.3 → 178.0–201.1 MB/s. No difference within noise. The first measurement was 3 %
slower on 1 GiB: the receiver's wait for its `done` to be acknowledged read in 100 ms slices; it
now returns on the acknowledgement. Kept messages are sealed where they lie and their buffers
reused (one copy a chunk, as protocol 1 had).

## Threads

The host calls in on its worker for this add-on (open, close, clicks, commands). The engine
(`engine.h`) keeps its own threads for what waits on the network: a control thread (the only one
that starts and stops presence, and sends throttled posts), the listener's accept loop, the
discovery thread, Bluetooth's queue or handlers, the one transfer, a folder reader, and while a
protocol 2 send runs, its mover (#472) with a connect thread per bearer it races, each with its own
timeout and joined before the send's thread ends. A channel has its pinger, which also watches the
session's health (suspect at 3 s, ended at 20 s). The folder
reader is one thread, made at the first set: a newer set replaces one waiting and stops one being
read (its generation goes stale), and no caller waits for it, so handing over a large gallery
selection returns at once (#442; it used to join the previous reader on the caller's thread). On
Windows the share icon then closes the grid, which ends its range, as on the Mac. None is a
UI or render thread; no lock is held across I/O; a Bluetooth callback that hands a link to the
engine goes off the radio's queue and through a gate the bearer closes before it stops. The
window is posted whole (`build_tree`, a pure function of the state, tested), at most ~10 times a
second while bytes move; those posts update chrome and never present the canvas.

## Privacy (rule 6)

- Nothing about a transfer goes to telemetry or a crash report: no path, file name, pixel, EXIF,
  address or peer name is logged (`log` takes ASCII without them) or annotated. The minidump
  scrub ([13](13-updates-and-telemetry.md)) applies unchanged.
- The beacon carries a random id, the machine's name, a port and the protocol it speaks; the offer
  travels sealed, to the peer picked. No account, no cloud, no internet relay: the beacon's TTL is 1
  and nothing resolves a name. Protocol 2 (#472 PR 1) opens no new network surface: the same
  listener, beacon and Bluetooth service carry it.
- Protocol 2's resume secret and keys live in memory only, wiped when the session ends; nothing
  about a session is written down.
- Receiving is off until the window opens or the person turns it on. Turned on, it stays on across
  launches until turned off (#442), and the command bar says so (**Sharepoint: receiving**).
- A notification (#452) names the sender and a count and size; never a file name, a path or the
  code. It is shown by the OS on this machine only; nothing about it leaves the machine.

## Host pieces this added

- **Notices and keep-alive** (#452, host table entries after `place_matches`, ABI 0.33):
  generic, held by `slot_registry` and drawn by both hosts ([In the background](#in-the-background-452),
  [25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)).

- **Screens vocabulary**: a `text_field` may carry `"pick": "folder"`; the hosts add **Choose…**
  (`NSOpenPanel`; WinUI's `FolderPicker`) and report the folder as the field's change
  ([25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)). For the AirDrop-like window
  (#442), generic additions drawn by both hosts, not a Sharepoint-only path: a `tiles` node, a
  `stack` `"look": "card"`, a `progress` `"look": "ring"`, a `text_field` `"look": "code"` with
  `"digits"`, and the `text` style `"display"` ([25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)).
- **Commands of an add-on that fills slots** become command events to it
  (`slot_registry::run_command`, `mv_slots_command`, ABI 0.29, [14](14-abi.md)): Sharepoint's key-less
  "Sharepoint" row carries the marked or current files.
- **Settings → Add-ons**: an add-on that fills slots and has a window gets **Open …**; Sharepoint
  is offered with one click while it is not installed (its release channel,
  `mediaviewer-addon-sharepoint-<platform>`).
  On Windows each add-on there is one card (#442): its name, what it does, its state, the install
  bar while it installs, its buttons (**Open …**, **Update**, **Remove…**), its message and its
  settings page together, Import's status and restart inside Import's card; the Mac's
  `AddonsSection` / `SlotAddonsSection` group the same way.
- **The gallery's range** on both hosts ([16](16-commands.md#gallery-range-and-share-413)).
- **An add-on's screen window on Windows is a MediaViewer window** (#442, `OpenScreenWindow`): the
  app's icon in the title bar, taskbar and Alt+Tab (`Shared/AppIcon.cs`, as Import's windows);
  the manifest's size in DIPs at the viewer's DPI, with a floor (`PreferredMinimumWidth` /
  `Height`); a window or sheet is owned by the viewer (above it, minimised with it) and opens
  centred over it; and every one carries `MediaViewer.AddonWindow`, so the viewer's key router
  leaves its keys alone (a digit typed into the code field is text, not a rating). A docked pane
  stays unowned, as [25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11) says.
- **App permissions**: the Mac's Info.plist names `NSLocalNetworkUsageDescription` and
  `NSBluetoothAlwaysUsageDescription` (macOS asks once each, the first time Sharepoint looks).
  Windows asks through Defender Firewall at the first listen.

## Where it lives

| Piece | Code |
|---|---|
| Wire, code, path rule, beacon | `src/addons/sharepoint/wire.*` |
| Noise (protocol 2's handshakes, #472) | `noise.*` |
| Sealed channel, and protocol 2's session (replay, rejoin, resume) | `channel.*`, `link.h` |
| Sending and the verified receive | `transfer.*` |
| Engine, presence, the window's tree | `engine.*`, `presence.*`, `tree.cpp` |
| What is kept across launches | `prefs.*` |
| Sockets, machine name, Downloads | `net.h`, `net_posix.cpp`, `net_win.cpp`, `machine_mac.cpp` |
| Bluetooth | `bt_pipe.*`, `bt_mac.mm` (CoreBluetooth), `bt_win.cpp` (C++/WinRT), `bt_none.cpp` |
| The add-on's export and manifest | `addon_entry.cpp`, `manifest.json`; `cmake/sharepoint.cmake` |
| Mac chrome | `Sharepoint.swift` (offer, share icon), `SlotItems.swift` (Settings), `ScreenView.swift` (Choose…), `GalleryView.swift` / `FolderStore.swift` (range, drag), `AddonNotices.swift` (notifications, the menu-bar item, #452) |
| Windows chrome | `IslandHost.Sharepoint.cs`, `IslandHost.GalleryRange.cs`, `IslandHost.Slots.cs`, `IslandHost.Screens.cs`, `IslandHost.Notices.cs` (notifications, #452) |
| Native hosts | `src/shell/main_mac.mm` (the background and the login item, #452), `src/shell/main.cpp` (the background, the notification-area icon and `--background`, #452), `src/shell/login_item_win.*` (the Run value), `src/shell/addons_mac.mm` |
| Notices and keep-alive (#452) | `src/addon/slots.*` (`post_notice`, `keep_alive`, `notices_json`), `src/addon/host.cpp` |
| Tests | `tests/test_sharepoint.cpp`, `tests/test_sharepoint_noise.cpp` (the Cacophony vectors, #472), `tests/test_sharepoint_session.cpp` (rejoin, resume, the Bluetooth / Wi-Fi moves, #472), `tests/sharepoint_fixture.h`, `tests/test_sharepoint_addon.cpp` (`mv_import_tests "[sharepoint]"`), `tools/package/test_addon_pack.py` |

## Not built

- Bonjour / DNS-SD discovery (the beacon is one portable socket on both OSes); IPv6.
- A peer that is neither on the LAN nor in Bluetooth range: no relay, by design. #472 plans the
  internet (direct codes, an opt-in DHT, remembered devices, the person's own relay) in PRs 2–5.
- Resuming a protocol 1 transfer that dropped: finished files stay, the rest is sent again.
  Protocol 2 rejoins and resumes within 20 s; a session that ends (20 s without a link, three
  rejoins in 10 s, the app quitting) is not resumed later.
- A formal model of the handshake and rejoin (Tamarin or ProVerif), from #472's assurance list.
  The parsers of what a peer sends before it is authenticated (the beacon, INFO, the hello, the
  Noise messages, a whole receiver's open) and the offer have a libFuzzer harness
  (`tools/fuzz/fuzz_sharepoint.cpp`); `resume_at` is parsed only after its frame opens under the
  rejoin's keys.
