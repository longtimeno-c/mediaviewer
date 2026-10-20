# 28 — Driftwire: send files to a MediaViewer nearby

How one MediaViewer sends photos, video and audio to another, the way a nearby share does: the
two clients find each other, the receiver shows a short code that the sender types, and nothing
moves until the person on the other side accepts. Issues [#410](https://github.com/longtimeno-c/mediaviewer/issues/410)
(the add-on and its window) and [#413](https://github.com/longtimeno-c/mediaviewer/issues/413)
(the gallery's range, drag-out and share icon). Built 2026-10-15; where each part stands is in
[DEVELOPMENT.md](../DEVELOPMENT.md#driftwire-issues-410-413-2026-10-15). Protocol 2 (issue
[#472](https://github.com/longtimeno-c/mediaviewer/issues/472) PR 1, 2026-10-18): a Noise handshake
with a post-quantum hybrid, and a transfer that survives its link, moving between Bluetooth and
Wi-Fi with no new code and no new Accept ([The wire](#the-wire), [The session](#the-session-protocol-2-472)).

Driftwire is a first-party add-on (option B of [25 §9](25-open-addons.md#9-code-how-a-strangers-add-on-runs),
the same kind as Import and Places). It carries its own code, has no chrome of its own, and
fills one window through the screens contribution point ([25 §10](25-open-addons.md#10-screens)):
both hosts draw it with their own controls, so it follows the theme, text size, VoiceOver and
Narrator. Other makers' script add-ons still have no network permission (owner, 2026-10-03).

## What a person sees

- **The window**, "Driftwire", opened from its row in Settings → Add-ons (**Open Driftwire**),
  from its command (the palette; no default key, issue #410), and from the share menu's first
  entry ([16 "Share"](16-commands.md#share-444)): the gallery's share icon, the bar's share button,
  `Ctrl+Shift+S` and right-click Share ▸. Its manifest's own **Send with Driftwire…** context
  entry is left out of the hosts' right-click menus (#444), so there is one route. Its design is
  AirDrop's (#442), the same on both hosts, top to bottom:
  - this computer's name as others see it, and **Keep receiving when this window is closed** (kept
    across launches; the command bar shows **Driftwire: receiving** while it is on), then
    **Receive when MediaViewer is closed** (#452, off by default; [In the background](#in-the-background-452));
  - **what is happening now**, first: a card for what is offered to this computer and one for a
    send under way, each opening with a **picture of the path** (2026-10-19, the screen `link`
    node): this computer, the other, and between them the bearer's badge, Wi-Fi, Bluetooth,
    internet straight to each other, internet met through the public DHT, through one of the
    person's computers (named), or through their relay server, each its own shape and colour, with
    the part moved so far and dots that travel the way the bytes go while they do (still with
    Reduce Motion; paused while it reconnects; a done or failed mark at the end). Whether an
    internet link was met on the DHT comes from the link (`link::punched`, `udp_hub`'s stream);
  - **what is offered to this computer**: "… wants to send you files" and the **code** in large
    digits to type over there, **Decline**; then "… wants to send you 24 files, 412 MB", the first
    names, **Save to** with Choose…, **Decline** / **Accept**; then the path filling with its
    percentage, the count and **Cancel**; then what arrived and **OK**;
  - **Send**, a card: a strip of the first six files' thumbnails (the viewer's cache), the count
    and size, the first names, **Send a folder** (Choose…), **Clear**;
  - **Nearby**: the MediaViewers it can see as large round tiles (a monogram, the name, "Wi-Fi",
    "Bluetooth" or both). **One click on a tile sends** to it. A card then asks for the code the
    other screen shows, in one large field that takes digits only and goes by itself at the sixth
    (three wrong codes end the link; this screen never shows the code); then "Waiting for … to
    accept"; then the tile's own ring fills while the bytes move (and the path card above shows
    how), and it ends with a done or failed mark on the tile and a line saying what happened;
  - **Over the internet**, one toggle, then in the order a person uses it: **Receive from
    anywhere** (Make a code; the code in large type, selectable, with **Copy code** and **Copy
    link**), **Send to a code**, Remembered devices, and **Internet options** (the DHT, Wi-Fi drops,
    relaying, and the optional own relay server). Nothing asks for an address: a code finds its
    own (2026-10-19).
- **The gallery** ([16](16-commands.md#gallery-range-and-share-413)): Shift-click selects a range,
  dragging a cell drags it out, and one share icon opens the share menu ([16 "Share"](16-commands.md#share-444)),
  whose first entry is Driftwire with the selection (issue #444: the same menu as `Ctrl+Shift+S`,
  the bar's share button and right-click Share ▸).
  Without Driftwire the icon offers the same one-click install as the other add-ons.

## Finding each other

Presence runs only while the window is open, while **Keep receiving** is on, or while a transfer
runs. With the window closed and receiving off there is no socket, no beacon, no Bluetooth and no
thread: the idle viewer announces nothing and polls nothing. Nothing in the add-on runs before the
host loads it after first pixel, and the engine is made at the first open or command, not at load,
unless Keep receiving was on.

**Keep receiving is kept across launches** (#442, [12](12-decision-log.md#driftwire)). The toggle
is written to the add-on's prefs (`prefs.h`, `driftwire.json` in its data folder) on the engine's
control thread. At load, `mv_addon_get` reads that file (one small read); when the toggle was on it
makes the engine, whose control thread starts presence. The host loads slot add-ons on its add-on
queue (Mac) or a worker (Windows) after first pixel, so presence never starts before first pixel
or on the foreground queue. Each window is its own process ([18](18-import.md) "One host
process"): only the one holding `driftwire.json.lock` (`net::try_lock_instance`: `flock` on the
Mac, an unshared handle on Windows) starts receiving at load, so one machine shows up once. While
the toggle is on, the command bar shows **Driftwire: receiving** (a status item, both hosts), so
a person can see this machine is discoverable after a relaunch. Off at load: the read, and nothing
else (no engine, thread, socket or lock).

Idle cost with it on and the window closed (`mv_import_tests "[.driftwire-idle]"`, Mac M-series,
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
- **Nearby is this machine's link** (#502). The listener and the beacon take a source only when it
  is on one of this machine's networks (an interface's subnet, `net::local_subnets`), link-local
  or loopback: not another subnet, a VPN's far side or anything routable that reaches the port.
  The networks are cached and looked up again on a miss (at most every 2 s) or after 30 s.
- **The list cannot be filled or starved** (#502). It holds 64 peers; a new one takes the place
  of the one heard from longest ago (never the picked peer, nor the one a send is for), one
  address lists at most 4 ids (loopback, this computer's own windows, is not capped), and Wi-Fi
  fills at most 48 places, so 16 stay for peers heard only on Bluetooth. A Bluetooth probe
  (connect, read INFO) gives its place up after 5 s, at most 8 run at a time, and at most 256
  devices are remembered (`bt_pipe.h`); on Windows at most 32 probe threads may still be waiting
  on the OS, and none start past that.
- **Bluetooth LE between a Mac and a Windows PC** has one common way to move bytes: GATT. Windows
  gives apps no connection-oriented LE channel. GATT moves tens of kilobytes a second. To a
  **protocol 2** peer (#472) a set of any size starts on Bluetooth when that is all there is, and
  the session moves to Wi-Fi the moment the peer is heard there ([The session](#the-session-protocol-2-472)).
  To a **protocol 1** peer a set over **32 MiB never goes over Bluetooth**
  ([12](12-decision-log.md#driftwire)); Wi-Fi carries anything. A protocol 1 peer heard only on
  Bluetooth, selected with such a set, shows why and what to do:
  put both computers on the same Wi-Fi network, or, when this computer has no network, turn on
  Wi-Fi here. A click on its tile then waits ("Waiting for … on Wi-Fi", with Cancel); the moment
  that peer's beacon arrives the send starts by itself, code and Accept as always. Presence stays up while it waits, window closed or not.
- Sending prefers Wi-Fi when the peer's beacon is fresh, and falls back to Bluetooth.

## In the background (#452)

**Receive when MediaViewer is closed** is opt-in and off by default, like AirDrop or Nearby Share
being reachable. It implies Keep receiving (turning that off turns this off too) and is kept in
`driftwire.json` with it. It is the same app, not a helper: the add-on asks the host to keep its
process (`keep_alive`), and the hosts do the rest.

- **Off** (the default) changes nothing: the last window closing quits, ⌘Q / Alt+F4 quit, and
  with Keep receiving off there is no process, socket, beacon or Bluetooth after quitting.
- **On**, closing the last window, ⌘Q or the Dock's Quit on the Mac, and closing the window on
  Windows, leave the process running. The viewer drops what it showed (no clip plays, the folder
  and its watch close; `returnToWelcome` / `return_to_welcome`) and its window is hidden, not
  closed. On the Mac the app then leaves the Dock (accessory policy) and a **menu-bar item**
  stays; on Windows a **notification-area icon**. Both menus: "Driftwire: receiving", **Open
  Driftwire**, **Open MediaViewer**, **Quit MediaViewer** (which really quits). Opening the app
  again (Finder, Spotlight, Start, a file) brings the hidden window back. Logout, restart and shut
  down, and an update's restart, quit as before. Only the process that holds
  `driftwire.json.lock` keeps itself; another window's process quits as usual.
- **A notification when someone sends** (both hosts, background or not). The engine posts a
  notice (`notice_post`) the moment a sender's link is up, while its code shows: "Ann's MacBook
  wants to send you files. Open Driftwire to see the code." Once the sender has typed the code
  and the offer arrives: "… wants to send you 24 files, 412 MB. Open Driftwire to accept." It
  never shows the code or a file name (rule 6: a lock screen shows it), only the sender's name, the
  count and the size. **Clicking it opens the Driftwire window** with the card and its code;
  its one button is **Decline** (the window's own Decline). Accept is only in the window, after
  the code: nothing is accepted from a notification. It is taken back however the offer ends.
  One source (a LAN address, or Bluetooth) gets at most three notifications a minute, the window
  brought forward counted with them; past that its offers wait in the window unannounced (#502:
  a sender that connects as "IT Support", idles and connects again all night).
- The host skips the notification when the Driftwire window is the front window of the active
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
- **Idle cost, windowless:** see [12](12-decision-log.md#driftwire) and DEVELOPMENT.md. The
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
  (Windows), the add-ons load as on any start, and Driftwire's keep-alive brings up the menu-bar
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
   The static is this machine's identity, kept in the OS's secret store (#472 PR 4,
   [below](#remembered-devices-472-pr-4)); without a store, a key made once a run. Nothing on the
   LAN or Bluetooth trusts it: only a remembered device's meeting checks it.
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
(`tests/test_driftwire_noise.cpp`). libsodium 1.0.22 (already pinned) has ML-KEM-768.

**The formal model** (`tools/formal/`, ProVerif 2.05, #472's assurance list) writes both handshakes
out token by token from `noise.cpp` and `channel.cpp`: h and ck as hash and HKDF chains, the
commitment, the code, the tag and `resume_at`. The attacker owns the network, replays and crosses
messages between any number of sessions and rejoins, and for the code holds a **collision oracle**:
given one side's code inputs and the other side's h2 it gets a nonce that gives both the same six
digits, the power a real search over 10^6 gives. What it shows:

- **The code.** When the sender's person typed the code the receiver showed, both sides ran the
  same handshake (same h2, nonce, link keys and resume secret, one for one), and the files and the
  resume secret stay secret, oracle and all. The oracle never helps because of the order: the
  server's nonce is bound by its commitment before the server's ephemeral (so its h2) exists, and
  the client's nonce is secret until its h2 is fixed. So each try of the attacker in the middle
  matches with probability 10^-6 and three wrong codes end the link: at most 3 × 10^-6 per
  attempt. On the handshake's skeleton with the commitment check removed, ProVerif finds that
  attack (the full first link without the check does not finish).
- **Broken later.** A first link recorded now stays secret if X25519 is broken later (every
  ephemeral and static handed over), or ML-KEM-768 is; with both broken it is read (the check that
  the model can lose).
- **The rejoin.** A link is adopted only by a side holding the session's resume secret, each
  adoption matches one rejoin the other side really ran (a replayed first message is never adopted,
  either way round), no two adoptions share link keys, and what a rejoined link carries stays
  secret even if the resume secret leaks after the session, and then also X25519 or ML-KEM-768
  is broken.

Simplifications: the primitives are ideal (symbolic hashes, HKDF, AEAD, the Diffie-Hellman
equation, ML-KEM as an ideal KEM), so this says nothing about side channels or implementation
bugs (the tests, the vectors and the fuzz harness do). The first link's handshake ciphertexts carry
a constant associated data instead of h; that only helps the attacker, so what holds there holds for
the code, and with the real h ProVerif does not finish. The code's 10^-6 is outside the symbolic
model: the oracle gives the attacker a perfect search, and the model shows no order of messages lets
it use one. Not modelled: protocol 1, `ident` (sent sealed before the code, so a machine in the
middle learns the sender's id and name before a code is typed), timeouts and the flapping rule.

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
   `cancel` and ends the link. Nobody typing it for 60 s ends the link on both sides (#502: the
   receiver's one slot is held meanwhile, under a name the sender chose). Until a right one nothing about the files leaves, not even a name:
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
   Every wait before Accept is a deadline (the handshake 8 s, typing the code 60 s, the answer 3
   minutes) that pings never extend, and a handshake still running is ended by Cancel or by the
   add-on stopping. After Accept, a receive where nothing of the files moves for 60 s (no message,
   and no file being read back) ends as failed (#502): pings alone do not hold the slot.

## The session (protocol 2, #472)

The channel is a session above the link (`channel.h`); `transfer.h` sees none of what follows: a
send or a receive waits through a rejoin.

- **Nothing lost, nothing twice.** Every sealed message carries its sequence number and the count
  the sender of it has received (the acknowledgement). A side keeps what the other has not
  acknowledged, at most **16 MiB** (`kReplayBytes`, a budget: [12](12-decision-log.md#driftwire));
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
  - **Wi-Fi → internet** (#472 PR 5, [below](#wi-fi--internet-472-pr-5)): with the toggle on, the
    receiver's internet places are raced too; with the DHT toggle on as well, both sides also meet
    on the DHT under the session's keys when those places are gone.
- **One bad byte ends the link, not the session**: after the handshake, a frame that does not open
  is treated as a dropped link, and the rejoin sends it again. A gap in the sequence numbers (never
  on an ordered link) or an acknowledgement of what was never sent ends the session.
- **The end.** The receiver answers `done` and waits up to 5 s for it to be acknowledged before it
  closes, so a link that drops at the last moment is rejoined rather than the sender waiting out a
  lost `done`.
- Not here yet: the slower too-slow rule between two fast bearers (over the internet a send
  moves back to Wi-Fi when the peer is heard there again, as Bluetooth does).

**Who can take what.** Bytes move only from a sender to the peer its person picked, after that
person typed the code the peer's screen showed and the person at that peer accepted. A sender never answers a request for
files, so a third machine that hears the beacon learns a name and a port, and nothing else. A
peer heard in the last 4.5 s keeps its address and device: a beacon claiming its id from another
address is ignored, and the list holds at most 64 ([Finding each other](#finding-each-other)).
A receiver already in a transfer answers `busy`; at most four such refusals run at once. One
source starts at most six receives a minute (#502); a link past that closes before its handshake.

## Over the internet (#472 PR 2)

A sender anywhere reaches a receiver with a **code**, and no server of anyone's is involved. One
toggle, **Send and receive over the internet**, is off by default; with it off nothing below runs or
shows: no listener, no router mapping, no socket. (`internet.*`, `invite.*`, `mapping.*`,
`cpace.*`.)

- **The receiver makes a code** (one at a time). It opens a TCP listener on IPv6 and IPv4, then
  gathers the places a sender could reach, in this order, at most four:
  - a port the **router maps** (`mapping.h`): PCP (RFC 6887), then NAT-PMP (RFC 6886), then
    UPnP-IGD (WANIPConnection 1/2, WANPPPConnection), the first that answers; also external
    **443** when the router allows, for a sender whose network lets only 443 out. A router whose
    own outside address is not public (double NAT, carrier NAT) is reported, not used;
  - this machine's **public IPv6** address (with an IGDv2 pinhole on the router when it offers
    one);
  - its address on this network, for a sender on the same network or behind the same router.

  With no public place at all, the window says why (no mapping and no IPv6, or the router behind
  another NAT) and what to try (UPnP / NAT-PMP on the router, the DHT toggle, a phone hotspot
  with IPv6). A UPnP description is fetched only from an address on this network, never a public
  one, so a stray SSDP answer cannot point it anywhere else.
- **The code** (`invite.h`) is a 48-bit secret and the places, encrypted under a key from the
  secret (a link preview shows no address), and a two-byte check that catches a typo on the
  sender before the code is spent. Crockford base32 in groups of four, case and O/0, I/1 forgiven,
  or a link (`mediaviewer://driftwire?code=…`). An IPv4-only code is 26 characters; with IPv6
  and several places up to about 90. The issue's "about 16 characters" cannot hold an address and
  a secret: the link carries the long ones.
- **It works once, for 10 minutes.** It dies at the first internet hello that reaches it, right or
  wrong, and at 10 minutes; then the listener closes and every mapping is removed (before the
  window shows it over), unless a transfer over it runs, whose rejoins come back the same way.
  A wrong attempt is said on the receiver ("Someone tried this code and it was wrong, so it no
  longer works").
- **The sender pastes the code** and tries every place at once, IPv6 first and the rest 250 ms
  later (happy eyeballs, `race.h`). The first TCP connection wins; the others close before a byte
  goes, so one attempt reaches the receiver. A round with no connection is tried again after 1, 2,
  5, 10, then every 30 seconds until Cancel, the window saying "Trying to reach the other computer
  (attempt 4)…" and, from what each place answered, why: nothing answered (a firewall, either
  side), refused (the code expired or MediaViewer closed), or only IPv6 in the code and none
  here.
- **The handshake** (`channel::open_internet`): the hello `MVSP` 2 `I` carries a CPace session id
  and the sender's CPace message (draft-irtf-cfrg-cpace-18, ristretto255 / SHA-512, checked
  against the draft's vectors); the receiver answers with its own. Then
  `Noise_XXhfs+psk0_25519+MLKEM768_ChaChaPoly_BLAKE2b` keyed by the CPace key, with the CPace
  transcript in the prologue: a wrong code fails at the receiver's first read, before it says
  anything of itself (not even its static key), and a machine in the middle gets one live guess
  per code and nothing to test guesses against offline.
- **Both confirm the other's name before any byte moves.** The sender sees "Connected to Bea
  (new device)" and a six-digit **check** (from the whole handshake hash), and clicks **Send to
  Bea**; the receiver sees "Ann (new device) wants to send you files" with the same check, and
  still accepts or declines as always. The check is optional to compare and differs whenever
  someone else is in the middle. Names are self-declared for a first-time peer: "new device".
- Then the transfer is protocol 2's session ([above](#the-session-protocol-2-472)) over the TCP
  link ("over the internet" on the window): a drop rejoins at the place the sender reached, and if
  the peer turns up on this network the send moves there.
- **TCP only, in PR 2.** With a direct code only the receiver's addresses are known, so the
  paths that need both sides to send (UDP hole punching, birthday punch, port prediction, IPv6
  simultaneous sends) wait for the DHT (#472 PR 3), and with them the QUIC-or-own-UDP transport
  spike. The keepalive is the session's ping (every second while a link is up).

## Four words over the DHT (#472 PR 3)

When the receiver has no public address of its own (no mapping, no IPv6), a
second opt-in toggle, **Find each other over the internet (public DHT)**, off by default, lets the
code be **four words** instead, and both computers meet on the public BitTorrent DHT. The DHT runs
only while a code waits or a send looks for its receiver; never at idle. (`dht_item.*`, `dht.*`,
`meet.*`, `udp_hub.*`, `bencode.*`, `sha1.*`.)

- **The words** are BIP-39's English list (2048 words, MIT; `wordlist.inc`), 44 bits; typed whole
  or as their first four letters. Argon2id (libsodium, moderate: 256 MiB, about a second) turns
  them into an Ed25519 key, a key for the items' contents, a salt base and CPace's password.
- **Each side publishes a BEP 44 mutable item** under four salts of its role (receiver, sender):
  its addresses (on this network, its public address as the DHT saw it, its public IPv6, all at
  its one UDP port), XChaCha20-Poly1305-sealed, padded to 512 bytes, the timestamp inside. The
  target is SHA-1(key ‖ salt): without the words, 20 random bytes nobody can find, read or forge.
  Each side looks up the other's four targets in parallel; an item is taken only if its key
  hashes to the target, its signature verifies, it opens under the code and it is under ten
  minutes old. After the link is up each side overwrites its items with "gone".
- **The DHT client** (`dht.h`) is the spike's "small KRPC + BEP 44/42 client in the add-on", not
  libtorrent (Boost, a torrent engine, sockets of its own): read-only (BEP 43, it answers no
  queries and stays out of others' tables), iterative lookups (three in flight, the eight closest),
  BEP 42 votes for this machine's public address from every answer (no STUN), nodes whose id
  matches their address preferred, a routing table kept between runs (`driftwire_dht.dat` beside
  the prefs), and the four routers of #472 to start from the first time.
- **One UDP socket** (`udp_hub.h`) carries the DHT, the punching and the transfer: the hole a probe
  punches is the one the transfer uses. **Punching**: each side probes every address the other
  gave, and 16 ports above the other's public one (a NAT that hands ports out in sequence), every
  100 ms for up to 10 s. A hello from a path the item did not name (a NAT's fresh mapping) is
  answered and probed back at once. The sender takes the first path a reply came back on and
  offers it (*choose*) until the receiver answers (*chosen*); the receiver answers only the first
  path it is offered, so both end on the same one, whichever transport it is on.
- **Every probe**, of every kind, is at most 200 a second each way (20 a 100 ms tick): the paths
  learned from hellos first (at most 8), the birthday's share (below), then the item's addresses
  and predicted ports in turn with what is left.
- **The birthday punch** (one hard NAT, #472): each item says whether its side's NAT maps each
  destination apart, from BEP 42 votes: each node's latest answer counts once, for ten minutes,
  and "hard" needs at least three ports seen for the most-seen address with no port seen by half
  the voters, so one node's lie (or two's) does not set it. A client with fewer than four voters
  looks up once more before it decides, and a side the puts show to be hard says so again. When
  exactly one side is hard, it opens 256 more UDP sockets on the hub (64 KiB buffers; one reader
  thread polls them all, `net::wait_readable`) and probes the easy side's public address from
  each, every one a new mapping in its NAT; the easy side probes random ports of the hard side's
  address. The birthday's probes are at most 100 a second each way, until the punch's deadline.
  The hard side's mappings open 10 at a time (all 256 by 2.6 s), so the ~1000 random probes of a
  10 s punch meet about 97 times in 100 on a real NAT (the arithmetic, not a measurement). The
  stream runs on the socket that met (its buffers grown to the main socket's); every other one is
  closed when the punch ends, and the stream's own when the stream ends, so the hub is back to its
  one socket and its idle wait. On POSIX the soft descriptor limit is raised to 1024 + 256 for
  them, for the rest of the process (lowered again it could fail another part's next open). Two
  hard NATs do not birthday-punch each other (256 × 256 of 64k ports would not meet in time).
- **TCP simultaneous open** (#472, `net::tcp_rendezvous`, POSIX and Winsock): each side binds one
  TCP port (`SO_REUSEADDR`, and `SO_REUSEPORT` where the OS has it), publishes it, and while
  listening on it connects from it to the other's port at each of its addresses, again every
  250 ms, so SYNs that cross in two NATs open one connection and one that arrives after ours
  failed is accepted. It runs beside the UDP punch, only in a word code's meeting (the window is
  open), never in a remembered device's background meetings, since it listens on every interface
  (Windows asks its firewall at the first). The sender picks a TCP connection only if no UDP path
  came back in the first 1.5 s, and the same choose / chosen settles it; once it has offered a TCP
  connection it waits for the answer there until the punch ends and never falls back to UDP, so
  the two sides cannot end on different transports. It meets where a NAT keeps a TCP
  connection's port (most home routers), whatever it does to UDP: two hard UDP NATs can still
  meet this way. On Windows the port is shared with `SO_REUSEADDR` (there is no `SO_REUSEPORT`),
  not `SO_EXCLUSIVEADDRUSE`, so another process of the same user could bind it too and take a
  connection; CPace on the code still refuses it. A connection closed after its choose leaves the
  4-tuple in TIME_WAIT on this side, which blocks a retry to the same peer from the same port:
  the sender does not retry that way.
- **The stream** over it is the spike's third option, "our own simple UDP transport", not QUIC: it
  must share the socket on both OSes (msquic owns its sockets and has no macOS; ngtcp2 needs TLS
  beside Noise), and the session above already seals, orders and resumes. 1200-byte packets,
  cumulative and 256-packet selective acknowledgements, RACK loss detection and a tail-loss probe
  (RFC 8985, simplified), NewReno growth, the receiver's buffer as the window; no pacing yet.
  Loopback, Mac (`"[driftwire]"`'s stream case): about 60 MB/s clean, 17–26 MB/s at 1 % loss each
  way, 6–10 MB/s at 5 %; no real path measured.
- Then the **internet handshake** runs over the stream as with a direct code (CPace on the words'
  password, `Noise_XXhfs+psk0`), the names are confirmed and the check is shown, and the transfer
  is protocol 2's session ("over the internet").
- **Measured on loopback only** (Mac, stand-in NATs in the tests: a symmetric one and a
  port-restricted cone that wrap every socket a hub opens): a hard receiver or a hard sender
  meets an easy one by the birthday punch, the stream on the one extra socket left open; two hard
  NATs time out over UDP with no extra socket opened, and meet over TCP when TCP is allowed. On
  loopback the kernel answers a SYN to a port nobody connects from with a reset, so the test's
  connection is one side's connect accepted by the other's listener: SYNs crossing in flight (the
  simultaneous open proper) happen only where a NAT drops the first SYN, which no test here makes.
  No real NAT, router or public path has been tried (the public DHT and real networks are never
  contacted by a test).
- **Not built**: port mapping for the UDP socket, and disjoint lookup paths in the S/Kademlia
  sense (each item has four targets, looked up apart, which spreads them over the keyspace). Two
  hard NATs that also rewrite TCP ports, or a network with no UDP, do not meet this way, and the
  window says what to try.

## Wi-Fi → internet (#472 PR 5)

A third opt-in toggle in the internet section, **Continue over the internet if Wi-Fi drops**, off
by default, lets a transfer nearby carry on over the internet when its Wi-Fi goes. Both computers
need it on (and "Send and receive over the internet").

- **Places swapped over the sealed link, no lookup.** A sender with the toggle on says so in its
  offer (`"handover":true`). A receiver with it on, receiving over Wi-Fi or Bluetooth in protocol 2,
  then opens a way in for as long as the transfer runs (`way_in`, `internet.h`): a listener, the
  router asked for a port (PCP, NAT-PMP, UPnP-IGD) and an IPv6 pinhole, as a code's places
  ([above](#over-the-internet-472-pr-2)) less the LAN address. It sends those places to the sender
  as `places` (message 14, protocol 2; only to a sender whose offer asked, since a channel before
  this PR ends the link on a type it does not know). The sender's window says "If Wi-Fi drops,
  this continues over the internet."
- **The move.** When the link is suspect or drops, the mover races the receiver's places beside
  Wi-Fi and Bluetooth; a TCP connection that reaches the way in is a rejoin (`NNhfs+psk0` and
  `resume_at`, as any), and the window says "over the internet". When the peer is heard on Wi-Fi
  again, the send moves back there (make before break). On loopback in the tests the move takes
  10–14 ms after the cut.
- When the transfer ends the listener closes and every mapping is removed. With the toggle off on
  either side nothing is offered and a Wi-Fi drop with no Bluetooth ends the transfer as before.
- What the sender learns: the receiver's public address, which the receiver gives to a peer its
  person already accepted from.
- **When both sides move (#472, after PR 5).** The places above go with the receiver's own
  network (its mapped port, its pinhole; only a public IPv6 on another interface survives). With
  **Find each other over the internet** (the DHT, [above](#four-words-over-the-dht-472-pr-3)) on as
  well, on both sides, the two meet on the DHT the way four words do, under keys from the session
  itself (`dht_handover`, `handover.h`):
  - The sender's offer says `"dht":true` beside `"handover"`; a receiver with the DHT toggle on
    answers at once with `places` carrying `"dht":true` (before its way in has opened, which may
    take the router seconds), and again with its places.
  - **Pre-started.** The receiver once it has accepted and the offer asked, the sender once those
    `places` arrive (so a receiver that never says so costs the sender nothing): from then until the
    files are in, each side keeps its UDP hub open and has joined the DHT (the bootstrap and saved
    table, as a word code's), so a drop costs no join. With nothing to rescue a side sends nothing
    more on the DHT than the join; the window shows nothing new. When the transfer ends the engine
    stops it without waiting (a join may be in a DNS lookup) and joins its thread later.
  - **The keys.** `channel::handover_secret()`: BLAKE2b over the resume secret under its own context
    (`"MediaViewer Driftwire handover v1"`, apart from the pairing secret's), then `crypto_kdf`
    (context `MVSPhand`) into the four parts of a word code's set (item key, box, salt base, CPace
    password), at index `4 × rejoins()`: the resume secret is fixed for the session (decision log,
    2026-10-18), so the count of rejoins both ends have adopted is what makes each drop's targets
    and stream id new. Only the two ends of the session hold the secret. Should the counts ever
    differ (a rejoin that completed on one end only), the meeting finds nobody and the session
    ends after 20 s, as with the toggle off. The sender publishes and looks as the sender, the
    receiver as the receiver, so neither meets its own item.
  - **On a drop** (the link suspect or down, looked at every 200 ms): each side asks up to eight
    known nodes again how they see it (its network may have changed), publishes its candidates,
    looks up the other's every 500 ms (a word code waits 2 s between looks), and punches. The
    sender rejoins over the punched stream (`NNhfs+psk0`, `resume_at`, as any rejoin); the receiver
    hands it to the engine's rejoin path. A meeting stops as soon as the link is up again some
    other way (Wi-Fi back, the swapped places); the sender drops a stream met after that. Every
    meeting, met or not, ends with this side's items saying "gone" (at most 2 s, at a sequence
    number above any it used under those keys). It runs beside the swapped places, not after them,
    so a drop the places rescue in milliseconds may still cost each side one publish.
  - **Measured** on the stand-in swarm (16 loopback nodes; 10 runs, 2026-10-18): the stream is up
    0.14–0.69 s after the drop is seen, the send is over the internet 0.87–1.88 s after the cut.
    The bimodal tail is the stream's first retransmit (1 s): when the receiver's punch completes
    ~100 ms after the sender's, the sender's first stream packet arrives before the receiver's
    stream exists. A second drop after a move over the DHT meets again under the next keys in
    ~5.5 s: the sender sees its cut at once, the receiver only as silence (`kSuspectMs`, 3 s).
    Well inside `kRejoinMs` (20 s); the public DHT's publish and lookup during a drop and real NATs
    are not measured.
  - With the DHT toggle off on either side (both cases tested), neither joins the DHT, and a drop
    that takes the receiver's places ends the transfer after 20 s, as before.
- **Not built** (said so in #472's PR 5): a handover does not go through a relay ([below](#relays-472)); with the DHT toggle off on either side, the receiver's own network going still ends the transfer after 20 s.

## Remembered devices (#472 PR 4)

After a transfer over protocol 2 that went through, either window shows **Remember Bea** under the
result. Ticked on both computers, a later send needs no code: Bea is a tile under **Remembered
devices** (in the internet section), and one click sends to it over the internet, whenever both
are online. The receiver still accepts or declines every transfer. (`secrets.*`, `known.*`.)

- **Identity.** An X25519 key made once and kept in the OS's secret store (`secrets.h`, a port, D9):
  the login Keychain on the Mac (a generic password under "MediaViewer Driftwire", this device
  only, after first unlock), Credential Manager on Windows (generic credentials, this machine only,
  not roamed). It is the static of every protocol 2 handshake. Where there is no store (or it does
  not keep what it is given) the key is this run's and nothing can be remembered; the toggle does
  not show. The tests use a folder (`secrets::file_store`).
- **What is remembered**: the peer's name, its static key from that session, and a **pairing
  secret** only the two ends of that session hold (BLAKE2b over the session's resume secret). At
  most 16 devices, in one value of the store; Forget removes one.
- **Mutual by construction.** Nothing is sent to say "I remembered you": the meeting is keyed by
  the pairing secret, so a device is found only when both kept it. One-sided, the sender waits
  ("Waiting for Bea to come online… Bea must remember this computer too") until Cancel.
- **Meeting.** Each hour (UTC) the secret gives the pair a fresh set of the four-word code's keys
  ([above](#four-words-over-the-dht-472-pr-3)) per direction: toward the receiving device's
  identity, so each side's wait and send are apart and a device never meets its own item. While
  the window is open (or Keep receiving is on), the internet and DHT toggles are on and something
  is remembered, one UDP socket and DHT client publish this computer's item for each remembered
  device (again every 4 minutes; an item is believed for 10) and look for a sender's every 15
  seconds, one target per look. A sender's item found, the two meet and punch as with four words
  (the sender punches 15 s longer, as the other side looks only that often). The sender's rounds
  end on the hour, when both change keys; it keeps going until it meets or Cancel.
- **Trust on first use.** The handshake is the internet one (CPace on this hour's password, then
  `Noise_XXhfs+psk0`), so only a holder of the pairing secret gets through; then the peer's static
  must be the one remembered. Another key ends the link on the side that sees it, loudly: "This is
  not the Bea you remembered: its security key has changed… If Bea was set up again, forget it
  here and send once nearby or with a code to remember it again. If not, someone may be pretending
  to be Bea." Nothing is sent or received. A device set up again with a new key is not found at all
  (its wait is keyed by its new identity), which is the same advice.
- What DHT nodes see: this computer's address taking part, and while something is remembered, an
  item per remembered device each hour under targets that change every hour and mean nothing
  without the pair's secret.
- **Not built**: remembered devices are not marked on the nearby tiles (the beacon carries an id, not the identity). Two clocks more than
  a minute apart around the hour may miss each other for that minute.

## Relays (#472)

When nothing answers directly, a transfer may go **through a computer of the person's own** or
**through a relay server they run**, never anyone else's. Either way the two MediaViewers' handshake
(CPace on the code, then Noise XX) and their session run end to end through it: the relay passes
ciphertext, holds no key of theirs, and cannot read or change what they say; at most it drops the
link. The tests check that no 32-byte run of the file passes through either relay in the clear.

- **Through your own computer** (option 9; `relay.*`, `engine_relay.cpp`). A toggle in the internet
  section, **Use my other computers to relay**, off by default; with it on, each remembered device
  shows **<name> is my computer** (kept in its record, `known.h`, and keyed in the window by a hash
  of the device's key, not its place in the list; a record with no mark is written as before). A
  sends to a direct code; beside the code's places, 1.5 s in, each round also meets each computer
  marked as its own in turn, as any remembered device (the DHT, the pair's hourly keys, its key
  checked; up to 45 s each, given up as soon as the round is decided), and over that sealed channel
  sends `relay` (20) with the code's places. R serves only if its toggle is on and it marks A as
  its own too, now: then it connects onward to those places and only those, and only to public
  addresses that are not its own and not in an IPv6 /64 it has an address in (R's own public IPv4
  reached through its router is not known to R; what the router forwards there is open to the
  whole internet anyway), answers `relay_answer` (21), and from then on the bytes of A's link to B
  travel as `relay_data` (22), sealed again between A and R. A runs the internet handshake with B
  through it; B sees R's address as the sender's, and both see the same check. One relay at a time
  (it is R's one transfer; while R is busy it meets nobody, so A's meeting runs out rather than
  hearing "busy"); 256 GiB each way at most, no rate cap (it is the person's own line); 120 s of
  silence ends it. R's window says **Relaying for Ann**, sealed, with **Stop relaying**; A's says
  **Through R (your computer)**, and when R says no, why (after the round's own reason). Nothing is
  logged about either peer; R logs one fixed line that it relayed. A relayed send does not move
  between links (its rejoins would need R again), and R must be reachable over the DHT (Keep
  receiving on, the internet and DHT toggles on). Word codes and sends to a remembered device are
  not relayed: R would need the code's DHT meeting. R reads a remembered sender's first message
  within 20 s (`kSilenceMs`): the request or the offer comes at once, nothing waits on a person.
- **Through your own relay server** (option 10; `splice.*`, `tools/driftwire-relay`). An advanced
  field, **Relay (your own server)**, `<address or name>:<port>#<key>`; the server prints the key
  (or takes `--key`). The name is the one Driftwire resolves, because the person typed it; the
  code carries the address, never the key. With it set, each code also registers at that server as
  a **waiter** under a fresh random 16-byte token sealed in the code with its places, proving the
  key (keyed BLAKE2b over the token); this runs beside the router's mapping, and a server slower
  than 3 s is left out of that code and the window says so. A code with a relay is about 115
  characters, and only a build with relays opens it. A sender races the code's places as before
  and the relay 1.5 s later, so a direct path that works wins; it says the token only. The server
  pairs a sender with the waiter under its token and copies bytes between them; it relays only for
  key holders' codes, so it is not an open relay. A sender with no waiter is closed at once
  (senders never wait); a waiter registering again under its token replaces its old wait, which
  the receiver does every 60 s, new connection before closing the old, ahead of NATs that drop an
  idle TCP connection. Anything that is not a hello, a waiter whose proof is wrong, or a byte
  before the pair, is closed. The receiver hands a paired link on only once the sender has spoken,
  so a sender that lost its race to a direct place costs nothing. The token is not the code's
  secret and says nothing about it; the server learns two addresses, the time and the size, and
  prints counts only. Caps: 16 hellos being read at once and 4 from one address; 64 waiters, 4 from
  one address; 8 splices; per splice 64 GiB each way, an optional rate as a token bucket (a quarter
  second of burst), 120 s idle. The program is about 300 KB (Mac, Release), the splice alone,
  portable (D9), no state on disk.
- **TURN is left out**: allocations, permissions, STUN's message format and long-term credentials
  are a server of their own for the same job the splice does in a few hundred lines.
- **Tor** (option 11) was evaluated, not built, and dropped for now
  ([plan](../plans/driftwire-tor-spike.md), [12](12-decision-log.md#driftwire)).

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
  across launches in the add-on's own prefs (`prefs.h`: `driftwire.json` in its data folder,
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

**Throughput** (`mv_import_tests "[.driftwire-bench]"`, #442): two engines in one process over
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
  travels sealed, to the peer picked. No account, no cloud, no server of ours: the beacon's TTL is
  1 and nothing resolves a name. Protocol 2 (#472 PR 1) opens no new network surface: the same
  listener, beacon and Bluetooth service carry it.
- **Over the internet** (#472 PR 2, opt-in, off by default; [12](12-decision-log.md#driftwire)
  amends "no internet relay, by design"): a sealed link to the peer the person picked, local or
  over the internet, with no server of ours. A relay only ever through the person's own devices or
  one they configure ([Relays](#relays-472)), and it sees only ciphertext. What cannot be hidden: the code gives its holder this machine's
  public address; the router learns of a mapping (PCP, NAT-PMP, UPnP); a network observer sees
  encrypted traffic and roughly how much moved. With only the internet toggle on, nothing goes
  anywhere else: no lookup, no STUN, no name resolution. With **Find each other over the internet**
  (the DHT) on, the client resolves the four bootstrap routers' names (`dht.h`) unless its saved
  table answers, and DHT nodes see this machine's address taking part in the DHT (never what it
  looks up: the targets mean nothing without the code or the pair's secret). A relay server the
  person typed is resolved too. Codes, addresses and peer names are never logged. While four words meet
  (a word code, not a remembered device), a TCP port listens on every interface (its number is in
  the sealed item), and a birthday punch sends its 14-byte probes to random ports of the other
  side's public address only.
- Protocol 2's resume secret and keys live in memory only, wiped when the session ends; nothing
  about a session is written down, except what Remember keeps (#472 PR 4): the peer's name, key
  and pairing secret, in the OS's secret store (Keychain, Credential Manager), never a plain file,
  until Forget.
- Receiving is off until the window opens or the person turns it on. Turned on, it stays on across
  launches until turned off (#442), and the command bar says so (**Driftwire: receiving**).
- A notification (#452) names the sender and a count and size; never a file name, a path or the
  code. It is shown by the OS on this machine only; nothing about it leaves the machine. At most
  three a minute from one source (#502).
- "Nearby" is this machine's link (#502): the LAN listener and the beacon ignore sources that are
  not on one of its networks, so a public address with the firewall off, or a VPN's far side, is
  not taken for a computer in the room.

## Host pieces this added

- **Notices and keep-alive** (#452, host table entries after `place_matches`, ABI 0.33):
  generic, held by `slot_registry` and drawn by both hosts ([In the background](#in-the-background-452),
  [25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)).

- **Screens vocabulary**: a `text_field` may carry `"pick": "folder"`; the hosts add **Choose…**
  (`NSOpenPanel`; WinUI's `FolderPicker`) and report the folder as the field's change
  ([25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)). For the AirDrop-like window
  (#442), generic additions drawn by both hosts, not a Driftwire-only path: a `tiles` node, a
  `stack` `"look": "card"`, a `progress` `"look": "ring"`, a `text_field` `"look": "code"` with
  `"digits"`, and the `text` style `"display"` ([25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)).
  For the path picture and the code's copy (2026-10-19): a `link` node and a button's `"copy"`,
  contribution API 6; Driftwire's manifest asks for 6.
- **Commands of an add-on that fills slots** become command events to it
  (`slot_registry::run_command`, `mv_slots_command`, ABI 0.29, [14](14-abi.md)): Driftwire's key-less
  "Driftwire" row carries the marked or current files.
- **Settings → Add-ons**: an add-on that fills slots and has a window gets **Open …**; Driftwire
  is offered with one click while it is not installed (its release channel,
  `mediaviewer-addon-driftwire-<platform>`).
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
  `NSBluetoothAlwaysUsageDescription` (macOS asks once each, the first time Driftwire looks).
  Windows asks through Defender Firewall at the first listen.

## Where it lives

| Piece | Code |
|---|---|
| Wire, code, path rule, beacon | `src/addons/driftwire/wire.*` |
| Noise (protocol 2's handshakes, #472) | `noise.*` |
| CPace, internet codes, port mapping, the code's life and the sender's race (#472 PR 2) | `cpace.*`, `invite.*`, `mapping.*`, `internet.*`, `race.h`, `net_address.cpp` |
| Word codes, the DHT, one UDP socket and its stream, punching (#472 PR 3) | `dht_item.*`, `wordlist.inc` (BIP-39, MIT), `dht.*`, `bencode.*`, `sha1.*`, `udp_hub.*`, `meet.*` |
| The way in for Wi-Fi → internet (#472 PR 5) | `internet.*` (`way_in`), `transfer.*` (`places`) |
| Relays: the splice and rendezvous, the own-device relay, the relay server program (#472) | `splice.*`, `relay.*`, `engine_relay.cpp`, `tools/driftwire-relay/main.cpp` |
| The DHT meeting when both sides move (#472) | `handover.*` (`dht_handover`), `channel.*` (`handover_secret`) |
| The identity, remembered devices and their hourly keys (#472 PR 4) | `secrets.h`, `secrets_mac.cpp` (Keychain), `secrets_win.cpp` (Credential Manager), `secrets_file.cpp`, `known.*` |
| Sealed channel, and protocol 2's session (replay, rejoin, resume) | `channel.*`, `link.h` |
| Sending and the verified receive | `transfer.*` |
| Engine, presence, the window's tree | `engine.*`, `presence.*`, `tree.cpp` |
| What is kept across launches | `prefs.*` |
| Sockets, machine name, Downloads | `net.h`, `net_posix.cpp`, `net_win.cpp`, `machine_mac.cpp` |
| Bluetooth | `bt_pipe.*`, `bt_mac.mm` (CoreBluetooth), `bt_win.cpp` (C++/WinRT), `bt_none.cpp` |
| The add-on's export and manifest | `addon_entry.cpp`, `manifest.json`; `cmake/driftwire.cmake` |
| Mac chrome | `Driftwire.swift` (offer, share icon), `SlotItems.swift` (Settings), `ScreenView.swift` (Choose…), `GalleryView.swift` / `FolderStore.swift` (range, drag), `AddonNotices.swift` (notifications, the menu-bar item, #452) |
| Windows chrome | `IslandHost.Driftwire.cs`, `IslandHost.GalleryRange.cs`, `IslandHost.Slots.cs`, `IslandHost.Screens.cs`, `IslandHost.Notices.cs` (notifications, #452) |
| Native hosts | `src/shell/main_mac.mm` (the background and the login item, #452), `src/shell/main.cpp` (the background, the notification-area icon and `--background`, #452), `src/shell/login_item_win.*` (the Run value), `src/shell/addons_mac.mm` |
| Notices and keep-alive (#452) | `src/addon/slots.*` (`post_notice`, `keep_alive`, `notices_json`), `src/addon/host.cpp` |
| Tests | `tests/test_driftwire.cpp`, `tests/test_driftwire_noise.cpp` (the Cacophony and CPace vectors, #472), `tests/test_driftwire_internet.cpp` (codes, mapping against stand-in routers, two engines over a code, #472 PR 2), `tests/test_driftwire_dht.cpp` (BEP 44 / 42 vectors, the stream under loss, a stand-in DHT swarm, two engines over four words, remembered devices and an impostor with the pair's secret, #472 PR 3–4), `tests/test_driftwire_known.cpp` (the store's file form, the identity, the records, the hourly keys, #472 PR 4), `tests/test_driftwire_session.cpp` (rejoin, resume, the Bluetooth / Wi-Fi moves, the Wi-Fi → internet handovers, #472), `tests/test_driftwire_relay.cpp` (the rendezvous, the splice's caps, a relay code, R's answers, two engines through a relay server; the three-engine own-device relay is in `test_driftwire_dht.cpp`, #472), `tests/driftwire_fixture.h`, `tests/driftwire_swarm.h` (the stand-in DHT swarm), `tests/test_driftwire_addon.cpp` (`mv_import_tests "[driftwire]"`), `tools/package/test_addon_pack.py` |

## Not built

- Bonjour / DNS-SD discovery (the beacon is one portable socket on both OSes); IPv6 on the LAN.
- Over the internet, after the relays: Tor (dropped for now, [plan](../plans/driftwire-tor-spike.md)),
  TURN, relaying word codes, remembered-device sends and the handover, a relayed send moving.
- The rest of #472, deferred by the owner (2026-10-18, [12](12-decision-log.md#driftwire)):
  re-gathering and republishing on an OS network change; a remembered device's last-known direct
  addresses; DHT bootstrap from a LAN peer or a remembered device; a 15 s keepalive during a wait;
  S/Kademlia disjoint lookups; the 2x-faster switch between two fast bearers; port mapping for the
  UDP socket; per-source rate limits on the add-on's listener and on rejoins; the Wi-Fi Direct
  soft-AP spike; AES-256-GCM, larger chunks and parallel streams; a word code's failure naming its
  reason.
- Resuming a protocol 1 transfer that dropped: finished files stay, the rest is sent again.
  Protocol 2 rejoins and resumes within 20 s; a session that ends (20 s without a link, three
  rejoins in 10 s, the app quitting) is not resumed later.
- A formal model of protocol 1, and of CPace and the internet bearers (#472 PRs 2–5). Protocol 2's
  handshake and rejoin are modelled ([The wire](#the-wire)). The parsers of what a peer sends before it is authenticated (the beacon, INFO, the hello, the
  Noise messages, a whole receiver's open) and the offer have a libFuzzer harness
  (`tools/fuzz/fuzz_driftwire.cpp`); `resume_at` is parsed only after its frame opens under the
  rejoin's keys.
