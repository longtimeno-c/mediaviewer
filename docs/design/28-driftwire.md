# 28 — Driftwire: send files to a MediaViewer nearby

One MediaViewer sends photos, video and audio to another: the two find each other, the receiver
shows a six-digit code the sender types, and nothing moves until the receiver accepts. Protocol 2
adds a post-quantum hybrid Noise handshake and a session that survives its link, moving between
Bluetooth, Wi-Fi and the internet with no new code or Accept ([The wire](#the-wire),
[The session](#the-session-protocol-2-472)).

A first-party add-on (option B of [25 §9](25-open-addons.md#9-code-how-a-strangers-add-on-runs),
like Import): its own code, no chrome, one window through the screens contribution point
([25 §10](25-open-addons.md#10-screens)) drawn by both hosts with native controls. Third-party
script add-ons still have no network. The same engine runs in a terminal as `driftwire`
([From a terminal](#from-a-terminal-driftwire)).

## What a person sees

- **The window**, "Driftwire": from Settings → Add-ons (**Open Driftwire**), its palette command (no
  default key), and the share menu's first entry ([16 "Share"](16-commands.md#share-444)). The
  manifest's **Send with Driftwire…** context entry is left out of right-click menus. AirDrop-like,
  the same on both hosts; each group its own card, never nested:
  - this computer's name, **Keep receiving when this window is closed** (persisted; command bar
    shows **Driftwire: receiving**), **Receive when MediaViewer is closed** ([In the background](#in-the-background-452));
  - **what is happening now**, first: incoming-offer and send cards, each with a **picture of the
    path** (screen `link` node: both computers, the bearer badge — Wi-Fi, Bluetooth, internet,
    DHT-met (`link::punched`), via own computer, via own relay — progress, travelling dots, a
    done/failed mark). A card **shows itself** (screen `reveal`) on appear, start and end;
    progress alone never scrolls;
  - **the incoming offer**: the **code** in large digits and **Decline**; then count, size, first
    names, **Save to**, **Decline** / **Accept**; then "1.2 GB of 4.8 GB · 14 MB/s · about 4 min
    left" (`eta.h`, after 3 s and 0.5 MB) and **Cancel**; every end stays until **OK**, both sides;
  - **Unfinished transfers** with **Discard** ([Resuming a receive that stopped](#resuming-a-receive-that-stopped-530));
  - **Send**: six thumbnails, count, size, **Send a folder**, **Clear**;
  - **Nearby**: round tiles; **one click sends**, then a digits-only code field that submits at the
    sixth digit (three wrong end the link), "Waiting for … to accept", the tile's ring, a result
    and **OK**. Empty: **No one nearby yet** and how to fix it;
  - **Over the internet**: one toggle, then **Receive from anywhere** (Make a code, **Copy code**,
    **Copy link**), **Send to a code**, Remembered devices, **Internet options**.
- **The gallery** ([16](16-commands.md#gallery-range-and-share-413)): Shift-click range, drag-out;
  Share (the bar's button, Driftwire first; a one-click install when absent) sends the range, the
  file, or the outlined folder tile whole ([16 "Share"](16-commands.md#share-444)).

## From a terminal (`driftwire`)

A terminal host of the same engine ([12](12-decision-log.md#driftwire)) on Linux, Windows and
macOS, wire-compatible with the add-on, installed without MediaViewer ([Installing](#installing)),
never in the app's payload. Manual page `packaging/linux/driftwire.1`.

```
driftwire                        the session: visible nearby while it runs; send and receive
driftwire send PATH... [--to NAME]   files or one folder ready; closes when that send ends;
                                 --to sends to NAME once heard (the code is still typed)
driftwire receive [--once]       --once closes after the first offer (a knock does not count)
driftwire code                   makes a code; closes when its receive ends or the code dies
driftwire devices                remembered devices (no terminal needed)
driftwire serve [--data DIR]     the NAS service ([below](#the-nas-service-driftwire-serve))
driftwire host [--data DIR]      this computer as the service and a relay ([below](#on-your-own-computer-driftwire-host))
driftwire update                 update through the install channel (no terminal needed)
driftwire --help | --version
  --save-to DIR, --name NAME     this run only
  --plain, --no-color, --verbose
```

Exit 0 normal (for `send`, `receive --once`, `code`: the transfer went through); 1 that transfer did
not; 2 usage error, no terminal, or failed start.

- **Interactive only**: no `--yes`; refuses when stdin/stdout is not a terminal (`driftwire: this
  needs a terminal (it asks before anything is sent or saved).`, exit 2). Only the never-packaged
  test build `mv_driftwire_cli_test` (`MV_DRIFTWIRE_CLI_TEST_HOOKS`) reads a pipe.
- **No secret on the command line**: codes, checks and relay keys are pasted into `/send-to` or
  `/relay-server`, never arguments (`driftwire code 123456` is a usage error).
- **The look** (after Claude Code's terminal UI): inline, never the alternate screen; finished
  events print once to scrollback (`⏺` sentence, `⎿` size, time, bearer, destination); only the
  live region redraws (current card, Nearby, prompt box and hint, `/` palette). Accent teal; bearer
  colours as the window. Animates ~12 fps only while waiting or moving; idle, the loop blocks.
  Under 40 columns the code is plain digits. `NO_COLOR`/`--no-color` drops colour; `--plain` or
  `TERM=dumb`: lines only, no escapes ("Accept 24 files (412 MB) from Ann's MacBook? [y/N/s]").
- **The code**: receiver shows six box-drawing digits "123 456" (Esc declines); sender types into
  six digit boxes (three wrong end the link). Internet checks are shown to compare. `/copy`,
  `/copy link` use OSC 52.
- **Accept is typed**: **1** Accept, **2** save elsewhere (Tab completes), **3** Decline; `y`/`n`/Esc.
  A card that says yes to someone else answers only once keys have been idle 750 ms with it shown
  (`kAnswerSettleMs`); Esc always answers. Three minutes declines. Esc twice cancels a transfer;
  Ctrl+C twice closes.
- **Clicks**: mouse reporting (CSI ?1000h + ?1006h) is on only while something clickable is shown
  (offer, question, check, Remember, palette, Nearby with files ready). A click is the choice's
  number typed (same settle rule); wheel is ↑↓; Nearby: first click points, second sends. Hit boxes
  from FTXUI `reflect` (`hit_boxes`, `render.h`); the region's row from a CSI 6n report after each
  write (`inline_view`, `up_from_top()`). Off in plain mode, on suspend and on exit.
- **Files**: typed, pasted or dropped; quotes, escapes, `~`, `file://`, globs `* ? [...]` expanded by
  the program. Commands: `/send`, `/folder`, `/files`, `/clear`, `/code`, `/copy`, `/send-to`,
  `/devices`, `/forget N`, `/mine N on|off`, `/unfinished`, `/discard N`, `/save-to`, `/internet`,
  `/dht`, `/handover`, `/relay`, `/relay-server`, `/tunnel [on|off|apps on|off]`, `/updates`,
  `/settings`, `/help`, `/quit`.
- **Its own data folder** (0700): `driftwire.json`, `driftwire_resume.json`, `driftwire_dht.dat`, the
  lock; `${XDG_STATE_HOME:-~/.local/state}/driftwire`, `~/Library/Application Support/Driftwire`,
  `%LOCALAPPDATA%\Driftwire`. Secrets under service "Driftwire CLI" (`secrets::kCliService`), never
  the app's. macOS has no store for a bare program: per-run key, Remember hidden. `--save-to`/`--name`
  are starting state, never saved.
- **Session = window**: `screen_opened("")` while it runs; Keep receiving and background are forced
  off. **One per user** (`net::try_lock_instance` on `<data folder>/driftwire.json.lock`; second:
  "Driftwire is already open in another terminal.", exit 2); independent of the app's lock.
- **No Bluetooth** (`use_bluetooth = false`, `bt_none.cpp`).
- **Threads**: the session thread owns engine and terminal and makes every engine call; `post_tree`
  only wakes it (`cli_host`). `~engine` runs before the terminal is restored so a cut-off receive
  is kept. SIGTERM/SIGHUP quit in order; raw Ctrl+C is a key; Ctrl+Z suspends.
- **Linux packages** (`cmake/driftwire-cli`): `/usr/bin/driftwire` needing only glibc ≥ 2.35;
  libsodium 1.0.22 (ML-KEM-768), BLAKE3, FTXUI, C++ runtime linked in; the build fails on any other
  `NEEDED`. libsecret, libsystemd, libcurl opened at run time (recommended by `.deb`/`.rpm`). Ships
  completions, a firewalld service (`driftwire`, 47613/udp), DEP-5 copyright.

### Updates in the terminal

`src/addons/driftwire/cli/update.h`.

- **The check**: on session open, at most daily, only for an installer-placed copy, a detached
  thread GETs `<site>/latest.json` (`{"version": "0.1.40"}`, from `driftwire_repo.py`); never
  waited on. Kept in `driftwire_update.json` (`check`, `checked`, `latest`). Off with `/updates
  off` or `DRIFTWIRE_NO_UPDATE_CHECK`. Notice: "driftwire 0.1.40 is available · this is 0.1.38" /
  "Run driftwire update to update it (/updates off stops these checks)."
- **`driftwire update`**: up to date → exit 0; else runs the channel: `brew upgrade driftwire`
  (path in a Homebrew Cellar), the site's `install.sh` from a file (`/usr/bin/driftwire`, or beside
  `share/driftwire/install-manifest`), or `install.ps1` via `powershell.exe -NoProfile -Command`
  (`%LOCALAPPDATA%\Programs\Driftwire\driftwire.exe`). Exit 1 on failure or a self-built copy.
- **Site**: `MV_DRIFTWIRE_SITE_URL`; `DRIFTWIRE_REPO_URL` overrides if https (or http on localhost)
  and URL characters only.
- **Request**: User-Agent `driftwire`, no cookie/query/identifier; WinHTTP, NSURLSession
  (ephemeral) or run-time libcurl; https only, 4 KiB, 10 s. Only digits and dots are a version.
  Unsigned: it only says an update exists.

### Installing

Pages site `https://longtimeno-c.github.io/mediaviewer/driftwire/` (Linux, Windows) and tap
`longtimeno-c/homebrew-tap` (macOS); setup in [DEVELOPMENT](../DEVELOPMENT.md#publishing-the-command-line).

```
Linux    curl -fsSL https://longtimeno-c.github.io/mediaviewer/driftwire/install.sh | sh
macOS    brew install longtimeno-c/tap/driftwire
Windows  irm https://longtimeno-c.github.io/mediaviewer/driftwire/install.ps1 | iex
```

| | What it writes | Updates | Removing it |
|---|---|---|---|
| Linux | the package via apt, dnf, yum or zypper with the repo's source and key; else the tarball in `~/.local` (`/usr/local` as root) | the package manager; tarball: the line again | `curl -fsSL <site>/install.sh \| sh -s -- --uninstall` |
| macOS | Homebrew's copy; without Homebrew, the macOS archive via `install.sh` into `~/.local` | `brew upgrade`; archive: the line again | `brew uninstall driftwire`; archive: `install.sh --uninstall` |
| Windows | `%LOCALAPPDATA%\Programs\Driftwire` on the user's `Path` | the line again | `& ([scriptblock]::Create((irm <site>/install.ps1))) -Uninstall` |

None touches the data folder or needs an administrator on Windows/macOS.

- **`install.sh`** (`packaging/linux/repo/install.sh`, POSIX sh, `main` on the last line): checks
  x86_64/arm64, glibc ≥ 2.35 (musl refused), curl/wget; prints each command. apt: key
  `/etc/apt/keyrings/driftwire.gpg`, deb822 `/etc/apt/sources.list.d/driftwire.sources` (`URIs:
  <site>/apt`, `Suites: stable`, `Components: main`, `Signed-By:`). dnf/yum/zypper:
  `/etc/yum.repos.d/driftwire.repo` (zypper repo `driftwire`). Else the tarball, checked against
  signed `SHA256SUMS`. A source that fails to verify is removed with apt's `partial/` files.
  `--uninstall` removes only what it added. On macOS with `brew` it prints the brew line and exits
  1; without, it checks macOS ≥ 14 and arch (`sysctl hw.optional.arm64`) and installs the archive.
- **Homebrew**: `Formula/driftwire.rb`, rewritten per stable release by release.yml's
  `driftwire-homebrew` (`tools/package/homebrew_formula.py`, `packaging/macos/driftwire.rb.in`);
  `url` = `driftwire-<version>-macos-universal.tar.gz` (`-macos-arm64` + `depends_on arch: :arm64`
  without an Intel build). Archive from `tools/mac/macpack.py cli` (`lipo_merge.py`); Developer ID,
  hardened runtime, identifier `io.github.longtimeno-c.driftwire`, no entitlements, notarized.
- **`install.ps1`** (`packaging/windows/install.ps1`): PowerShell 5.1 and 7, ASCII, binary cmdlets
  and .NET only (Restricted policy blocks script modules). 64-bit Windows 10+ x64 (Windows 10 on Arm
  and 32-bit refused). Downloads `driftwire-<version>-windows-x64.zip` from the site, checks the
  embedded SHA-256 and (signed releases) Authenticode before changing anything; swaps the folder,
  adds `Path` via the registry (keeping `REG_EXPAND_SZ`), broadcasts. Cleans only its own
  leftovers (`Driftwire.old-` / `.driftwire-` + 8 hex). `-BaseUrl`/`DRIFTWIRE_REPO_URL` repoint;
  `DRIFTWIRE_INSTALL_TEST=1` runs off Windows for tests.
- **Windows zip** (`tools/package/driftwire-windows.ps1`): `driftwire.exe` plus every DLL found from
  import/delay-load tables: `libsodium.dll`, `blake3.dll`, and the VC++ runtime from VS's
  redistributable folder (never System32). Any other DLL fails the build. Trusted Signing when
  configured.
- **Publishing**: `MV_DRIFTWIRE_REPO=1` makes each stable release upload the macOS archive and
  Windows zip (`tools/package/github-release.py`), deploy the site (`driftwire-repo`) and push the
  formula; preflight refuses it with the repo key or tap token missing. Linux packages are not on
  the release page (`MV_RELEASE_LINUX_CLI` off) so they always come with their update source.
- **The repository** (`tools/package/driftwire_repo.py`): a static tree, newest version only. apt
  pool, `Packages`, `Release` (Origin/Label `Driftwire`, `stable`, `main`) with `InRelease` and
  `Release.gpg`, Valid-Until a year ahead (a stable release must come within the year); rpm signed
  with `rpmsign`, `createrepo_c` (gzip, no sqlite), `repomd.xml.asc`, `.repo` with `gpgcheck=1` and
  `repo_gpgcheck=1`; tarballs and zip with `SHA256SUMS`/`.asc`; `key.asc`/`key.gpg`; the page; both
  installers; `latest.json`. It refuses mismatched versions, missing arches, foreign packages, bad
  paths, a non-x64 exe, non-https URLs; verifies every signature (`gpgv`, `rpmkeys`) and that no
  private key is in the tree before replacing the site. `driftwire_repo.py smoke` installs from it
  on localhost first. Key change: two keys overlap for at least one release.
- **Secrets**: `DRIFTWIRE_REPO_GPG_KEY`, `DRIFTWIRE_REPO_GPG_PASSPHRASE`, `HOMEBREW_TAP_TOKEN`
  (Contents: write on the tap only); the release's Apple and optional Trusted Signing credentials.

## Finding each other

**What stays put.** Presence runs only while the window is open, Keep receiving is on, or a
transfer runs; otherwise no socket, beacon, Bluetooth or thread. The add-on loads after first
pixel; the engine is made at the first open or command unless Keep receiving was on.

**Keep receiving is kept across launches** in `driftwire.json` (`prefs.h`, written on the control
thread). At load `mv_addon_get` reads it and, if on, makes the engine. Each window is its own
process ([18](18-import.md) "One host process"); only the holder of `driftwire.json.lock`
(`net::try_lock_instance`) receives. Clicking **Driftwire: receiving** opens the window.

| Bearer | Announce | Hear | Carry |
|---|---|---|---|
| **Wi-Fi** (`presence.cpp`, `net_*`) | A UDP beacon a second, port 47613, to 239.255.77.83 and the subnet broadcast, TTL 1 | The same socket, shared with other windows on the machine | One TCP connection to the beacon's address and port; the listener's port is the OS's pick |
| **Bluetooth** (`bt_mac.mm`, `bt_win.cpp`) | A GATT service advertised connectable | A scan filtered to that service; each new device read once for INFO (id, name) | The service as a byte pipe (`bt_pipe.h`): C2P written without response, P2C notifying |

- **Beacon**: `MVSP1` + `{"v","p","id","name","port"}`: random session id (16 hex), machine name
  **only while the window is open**, port, `"p"` = highest protocol (`"v"` stays 1; no `"p"` =
  protocol 1). INFO carries id, name, `"p"`. A listed peer's `"p"` is never lowered (anti-downgrade).
  Displayed names come from sealed `ident`.
- **Rotation**: with the window closed the name is `MediaViewer` and the id changes every 10 min
  (`kIdRotateMs`) while idle and whenever presence starts or the window opens/closes; never during
  a transfer, handshake or wait.
- Peers expire 4.5 s after their last beacon; both bearers = one row.
- **Nearby is this link**: only sources on this machine's subnets (`net::local_subnets`, cached,
  re-read on miss ≤ every 2 s or after 30 s), link-local or loopback.
- **Limits**: 64 peers (evict longest-unheard, never the picked or target peer); ≤ 4 ids per address
  (loopback uncapped); Wi-Fi ≤ 48 places. Bluetooth probe 5 s timeout, ≤ 8 at once, ≤ 256 devices;
  Windows ≤ 32 probe threads.
- **Bluetooth LE** Mac↔Windows is GATT only (tens of KB/s). Protocol 2: any set starts on Bluetooth
  and moves to Wi-Fi when heard. Protocol 1: a set over **32 MiB never goes over Bluetooth**; the
  tile waits ("Waiting for … on Wi-Fi") and starts when the beacon arrives.
- Sending prefers Wi-Fi when the beacon is fresh.

## In the background (#452)

**Receive when MediaViewer is closed**: opt-in, implies Keep receiving, in `driftwire.json`. The
add-on asks the host to keep its process (`keep_alive`).

- **On**: closing the window leaves the process running (⌘Q / menu / Dock Quit still quit). The
  viewer returns to welcome (`returnToWelcome` / `return_to_welcome`) and hides; Mac: accessory
  policy and a **menu-bar item**; Windows: a **notification-area icon**. Menu (host table
  `keep_menu`, `build_menu`): "Driftwire: receiving"; current activity ("Sending to Ann's MacBook ·
  42%") with **Cancel Sending**/**Cancel Receiving**; **Send Files…**; checkmarks **Receive from
  Nearby**, **Send and Receive over the Internet**, **Receive when MediaViewer is closed** (ids
  `receiving`, `internet`, `background`); **Open Driftwire**, **Open MediaViewer**, **Quit
  MediaViewer**. Re-sent only on change. While `driftwire host` runs, the server menu is a second item
  (Hosting on/off, usage, phones connected, drives, Quit hosting; [below](#on-your-own-computer-driftwire-host)). Only the lock holder keeps itself. A staged update installs
  once no transfer runs ([13](13-updates-and-telemetry.md)).
- **Notifications** (`notice_post`), background or not, unless the window is frontmost: "Ann's
  MacBook wants to send you files. Open Driftwire to see the code."; then "… wants to send you 24
  files, 412 MB. Open Driftwire to accept." Never the code or a file name. Click opens the window;
  the one button is **Decline**, tagged with its offer. Accept only in the window. ≤ 3 a minute per
  source.
  - **Mac**: `UNUserNotificationCenter` (`AddonNotices.swift`), permission asked when a toggle is
    first turned on (`notice_authorize`); without it the window opens and the Dock bounces.
  - **Windows**: `AppNotificationManager` under AUMID `MediaViewer.Viewer` (`IslandHost.Notices.cs`);
    refused → window opens; `----AppNotificationActivated:` accepted at start.
- **Sign-in start** while on: Mac `SMAppService.mainApp`; Windows
  `HKCU\…\CurrentVersion\Run\MediaViewer` = `"<install>\MediaViewer.exe" --background`
  (`src/shell/login_item_win.*`), registered/removed on the keep-alive list's empty edge; installed
  builds only. A login launch (`keyAELaunchedAsLogInItem`) or `--background` never shows the window;
  a Windows `--background` never forwards to a running viewer. If nothing keeps the process within
  60 s it removes its login item and quits.

## The wire

`wire.h`, `noise.h`, `channel.h`. The bearer only moves bytes. Every TCP link sets `TCP_NODELAY`
(each write is one whole frame, so Nagle would only delay small requests and a reply's last
packet; `link::no_delay`). A client speaks the peer's announced
`"p"`; a receiver answers any hello, so protocol 1 peers still work (no rejoin or bearer moves).

**Protocol 2 and 3**: `Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b`. Protocol 3 adds the server's
commitment (steps 2, 4) and its own prologue; rejoins and internet hellos are the same in both.

1. Client (sender): `MVSP`, 2, `N`, message 1: X25519 + **ML-KEM-768** keys; payload a BLAKE2b
   commitment to a random 32-byte nonce.
2. Server: X25519 key, `ee`, ML-KEM ciphertext (mixed into the chaining key), static, `es`;
   protocol 3 payload: its own nonce commitment.
3. Client: static, `se`, reveals its nonce. **Both statics are per-link keys**; the long-term
   identity ([below](#remembered-devices-472-pr-4)) goes sealed: the sender's in its **offer**
   (`"key"`), the receiver's in its **accept**. Behind CPace the identity is the static.
4. Protocol 3: the server's first sealed message `code_nonce` (unnumbered) reveals its nonce.
5. **The code**: six digits from BLAKE2b of the handshake hash after message 2 and both nonces
   (protocol 2: client's only). Commit-then-reveal with the server last stops a MITM searching for
   or learning the code early. Only the receiver shows it.
6. XChaCha20-Poly1305 secretstreams from Split; Split's third output is the **resume secret**
   (memory only). Ephemerals `sodium_memzero`ed.

Vectors: Cacophony for `Noise_XX`/`Noise_NNpsk0`; own fixed-key vectors for `hfs`
(`tests/test_driftwire_noise.cpp`). **Formal model**: `tools/formal/` (ProVerif 2.05) covers both
handshakes, the code (with a collision oracle), forward secrecy if only one of X25519/ML-KEM breaks,
the internet handshake (`internet.pv`) and rejoin; primitives are ideal.

**Tries, not people.** A nearby link that ends with no offer counts toward a limit (five a minute,
then 30 s of not answering, `note_unconfirmed`): about ten MITM tries per three-minute wait.
Protocols 1–2 lack the server nonce, so for them this limit is all that holds.

**Protocol 1**: client `MVSP`, 1, BLAKE2b of its X25519 key; server `MVSP`, 1, its key; client
reveals. `crypto_kx`; code from a hash of both keys. Other versions are refused at the hello.

Then, on any protocol:

1. `ident` (id, name) each way.
2. **The sender types the code**; checked locally, constant time; three wrong → `cancel`; 60 s idle
   ends the link. Nothing about the files leaves before.
3. **Offer**: count, total, each relative path and size. Too big for one message: `"parts"` then
   `offer_more`, ≤ 128 messages (`kMaxFiles` 100 000).
4. `accept`, or `decline` with `declined`, `busy` or `no_room`; three minutes declines; a busy
   receiver answers `busy` after the handshake.
5. Per file: `file_begin` (index, size, mtime), `data` chunks of 256 KiB, `file_end` with BLAKE3; or
   `file_skip`. Receiver: `file_done` each, `done` at the end.
6. `cancel` either way, ≤ 250 ms (`kCancelWaitMs`); `{"why":"error"}` for a receiver error.
   Protocol 1: ping after 2 s idle, 20 s silence = dropped. Deadlines before Accept (handshake 8 s,
   code 60 s, answer 3 min) ignore pings; after Accept 60 s with no file progress fails.

## The session (protocol 2, #472)

A session above the link (`channel.h`); `transfer.h` just waits through rejoins.

- **Nothing lost, nothing twice**: each message carries a sequence number and an ack; unacked data
  is kept ≤ **16 MiB** (`kReplayBytes`), then the sender waits. Acks at least per 1 MiB; pings
  (≥ 1/s idle) carry them.
- **Rejoin, no second code**: `MVSP`, 2, `R`, a 16-byte tag (8 random bytes + keyed hash under the
  resume secret), then `Noise_NNhfs+psk0_25519+MLKEM768_ChaChaPoly_BLAKE2b` with the resume secret
  as psk. Routed by tag (`engine::route_busy`); unknown tag closes. First message each way
  **`resume_at`** (count received; the other resends from there); its sequence field is the rejoin
  count, both take the larger. Duplicates dropped by sequence.
- **Other links during a transfer**: internet listeners close any non-rejoin hello before a
  handshake; nearby links get `busy`. Each tag opens once (last 256). Hello wait 2 s over the
  internet, pool 8 (nearby 4), ≤ 4 per source (address or IPv6 /64); when full a random in-hello
  link is closed (`source_of`, `internet.h`). The receiver's `.mvtmp`, BLAKE3 state and verifier
  carry on untouched.
- **Moves** (`engine::run_mover`):
  - **Break**: suspect after a failure or **3 s** silence (`kSuspectMs`); every bearer the peer is
    heard on races a **whole rejoin** (`channel::begin_rejoin`), winning only with the answer within
    **3 s**. **Reconnecting to …**. **20 s** (`kRejoinMs`) without a link ends the session.
  - **Make before break**: Bluetooth → Wi-Fi when the peer appears there; next move waits **10 s**
    (`kSwitchHoldMs`).
  - **Flapping**: three rejoins in **10 s** end it.
  - [Wi-Fi → internet](#wi-fi--internet-472-pr-5) adds internet places and DHT meetings to the race.
- A frame that does not open is a dropped link; a sequence gap or a bogus ack ends the session.
- **End**: the receiver waits ≤ 5 s for `done`'s ack.
- **A cancel is final**: `channel::cancel` marks the session, writes `cancel` after the current frame
  (timed mutex), waits for its ack, ≤ 250 ms; afterwards a drop is not a loss and rejoins are
  refused. Messages say who cancelled ("Ann cancelled. 2 files arrived whole; nothing partial was
  kept."). On Windows `IslandHost.Screens.cs` holds rebuilds while a pointer is down so clicks land.

**Who can take what.** Bytes go only to the peer the sender's person picked, after the code and the
peer's Accept; a sender never answers a request for files. A peer heard in the last 4.5 s keeps its
address. ≤ 4 `busy` refusals at once; ≤ 6 receives a minute per source.

## Over the internet (#472 PR 2)

A **code** reaches a receiver anywhere with no server. Toggle **Send and receive over the
internet**, off by default (off: no listener, mapping or socket). (`internet.*`, `invite.*`,
`mapping.*`, `cpace.*`.)

- **Making a code**: a TCP listener (IPv6 + IPv4), then ≤ 4 places: a **router-mapped** port
  (`mapping.h`: PCP, then NAT-PMP, then UPnP-IGD; also external **443** when allowed; a non-public
  router address is reported, not used); the **public IPv6** (IGDv2 pinhole if offered); the LAN
  address. No public place → the window says why. **UPnP talks to the gateway only** (its SSDP
  answer; URLs must name the gateway; service type `urn:` + `[A-Za-z0-9.:-]`); foreign forwards are
  never taken over.
- **Leases 120 s** (`kMapLifetimeS`), renewed at half-life (`mapping::keeper`); ledger
  `driftwire_mappings.dat` / `driftwire_way_in.dat` lets the next code clean crash leftovers
  (`mapping::clean_leftovers`).
- **The code** (`invite.h`): 48-bit secret + places encrypted under a key from it + a two-byte typo
  check. Crockford base32 in groups of four, or `mediaviewer://driftwire?code=…`; 26 to ~90 chars.
- **Once, for 10 minutes**: dies at the first internet hello (right or wrong) or at 10 min, closing
  the listener and mappings unless a transfer runs. A second request keeps the current code.
- **Sender**: races every place, IPv6 first, others +250 ms (`race.h`); retries after 1, 2, 5, 10,
  then every 30 s, saying why. Only public non-own places; the LAN address only when this router
  shares the code's public address. Only the code's own ways in spend it.
- **Handshake** (`channel::open_internet`): `MVSP` 2 `I`, CPace session id and message
  (draft-irtf-cfrg-cpace-18, ristretto255/SHA-512); then `Noise_XXhfs+psk0` keyed by the CPace key,
  transcript in the prologue. A wrong code fails at the receiver's first read; one online guess.
- **Names confirmed**: "Connected to Bea (new device)" + a six-digit **check**, **Send to Bea**; the
  receiver sees the same check and accepts as usual.
- Then protocol 2's session over TCP. Direct codes are TCP only.

## Four words over the DHT (#472 PR 3)

With **Find each other over the internet (public DHT)** (opt-in) the code is **four words** and both
meet on the BitTorrent DHT, only while a code waits or a send looks. (`dht_item.*`, `dht.*`,
`meet.*`, `udp_hub.*`, `bencode.*`, `sha1.*`.)

- **Words**: BIP-39 English (`wordlist.inc`), 44 bits, whole or first four letters. Argon2id
  (256 MiB) → Ed25519 key, content key, salt base, CPace password. Salt includes the UTC hour; in an
  hour's first ten minutes a sender derives both hours.
- **BEP 44 items** under four salts per role: addresses (LAN, DHT-seen public, IPv6, one UDP port),
  XChaCha20-Poly1305-sealed, 512 bytes. Target SHA-1(key ‖ salt). Taken only if key, signature,
  seal and < 10 min age check. Overwritten with "gone" afterwards.
- **DHT client** (`dht.h`): read-only (BEP 43), 3 in flight / 8 closest, BEP 42 public-address votes
  (one per IPv4 /24 or IPv6 /48; secure voters only once ≥ 4), table in `driftwire_dht.dat` (≤ 200
  nodes), four bootstrap routers. Public nodes only.
- **One UDP socket** (`udp_hub.h`) for DHT, punching and stream. Probe every given address plus 16
  ports above the public one, every 100 ms for ≤ 10 s; RFC 1918 only when `one_site` (same public
  address or one /16). The sender offers the first replying path (*choose*) until *chosen*.
- **Authenticated probes** (`prober`, `meet.cpp`): code tag, 16-byte nonce, keyed-BLAKE2b MAC
  (hello 46 bytes, others 63). v0.1.35's 14-byte probes are ignored: no DHT meeting with v0.1.35.
- **Nothing kept before the handshake** (`meet_options::open` / `later`): attempt-numbered
  choose/chosen; on handshake failure the next path, ≤ 3 attempts.
- **Rate** ≤ 200 probes/s each way.
- **Birthday punch** (exactly one hard NAT; "hard" from BEP 42 votes): the hard side opens 256 extra
  sockets (10 at a time) probing the easy side's address; the easy side probes random ports (≤
  100/s). POSIX fd limit raised to 1024 + 256. Two hard NATs do not try.
- **TCP simultaneous open** (`net::tcp_rendezvous`, `SO_REUSEADDR`/`SO_REUSEPORT`): beside the UDP
  punch in word-code and remembered-device meetings; used only if no UDP path in 1.5 s; verified
  hello within 2 s before choose.
- **The stream**: our own UDP transport (shares the socket; the session already seals and
  resumes): 1200-byte packets, cumulative + 256-packet selective acks plus ≤ 16 runs (`kMaxRuns`),
  RACK and tail-loss probe (RFC 8985, simplified), BBR v1 simplified with BBR v2's loss exit and
  inflight cap, RTO floor 200 ms, 16 MiB window (`kStreamMaxRx`); untrusted input bounded. Bench:
  `mv_import_tests "[.driftwire-stream-bench]"`, `tests/driftwire_path.h`,
  `docs/perf/driftwire-stream.jsonl`.
- Then CPace on the words' password and `Noise_XXhfs+psk0`, as a direct code.

## Wi-Fi → internet (#472 PR 5)

Toggle **Continue over the internet if Wi-Fi drops** (opt-in, both sides, plus the internet toggle).

- The offer says `"handover":true`; the receiver opens a way in (`way_in`, `internet.h`: listener,
  mapping, pinhole) and sends `places` (message 14). The sender keeps only the first non-empty list
  of public non-own places (`may_reach`). "If Wi-Fi drops, this continues over the internet."
- On suspect/drop the mover races these beside Wi-Fi and Bluetooth (a normal rejoin), and moves
  back to Wi-Fi when heard. Closed at transfer end.
- **Both sides move**: with the DHT toggle also on both sides, they meet like four words under
  session keys (`dht_handover`, `handover.h`). The offer adds `"dht":true`; the receiver answers
  `places` `"dht":true` at once. Both keep a UDP hub joined to the DHT from accept to end.
  Keys: `channel::handover_secret()` = BLAKE2b(resume secret, `"MediaViewer Driftwire handover
  v1"`), `crypto_kdf` context `MVSPhand` at index `4 × rejoins()`. On a drop (checked every 200 ms):
  re-ask ≤ 8 nodes, publish, look every 500 ms, punch, rejoin over the stream; items "gone" after.
- **Not built**: a handover through a relay ([below](#relays-472)).

## Remembered devices (#472 PR 4)

After a protocol 2 transfer, **Remember Bea** (nearby only if Bea sent its identity). Ticked on both,
Bea is a tile under **Remembered devices** and one click sends over the internet, no code; Accept
still applies. (`secrets.*`, `known.*`.) The question stays until the next send or offer.

- **Identity**: X25519 key in the OS secret store (`secrets.h`):
  - **Mac**: data protection keychain, generic password "MediaViewer Driftwire",
    `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`, access group
    `<team>.io.github.longtimeno-c.mediaviewer.driftwire` (`macpack.py release --team-id`,
    `APPLE_TEAM_ID`), claimed as an app group and, with the Developer ID profile embedded, as
    `keychain-access-groups`: the keychain refuses the app group alone (-34018,
    [12](12-decision-log.md), 2026-11-15). Unsigned builds, and releases without a profile, have no
    working store.
  - **Windows**: Credential Manager, values sealed with DPAPI (`CryptProtectData`, own entropy);
    plain items refused.
  - **Linux**: Secret Service via run-time libsecret (`secrets_linux.cpp`), schema
    `io.github.longtimeno_c.Driftwire`, attributes `service`, `name`; values `crypto_secretbox`-sealed
    (key from "<service> secrets v1"). No bus or > 10 s → no store.

  No store → per-run key, Remember hidden. Tests: `secrets::file_store`.
- **Kept**: name, identity, a **pairing secret** (BLAKE2b over the resume secret); ≤ 16 in one value;
  the 17th replaces the oldest not marked "my computer". **Remove everything** wipes them
  (`mv_addon_forget`). Tiles and Forget use `relay::fingerprint`, never list position.
- **Who proved whom**: nearby, the sender shows a check (`remember_check`) the receiver must type
  ("Code shown on Ann"; three wrong withdraw Remember). Over the internet CPace proves both.
- **Mutual**: the meeting is keyed by the pairing secret; one-sided, "Waiting for Bea to come
  online… Bea must remember this computer too".
- **Meeting**: each UTC hour the secret yields a four-word key set per direction. While the window is
  open or Keep receiving is on (internet + DHT on), the receiver looks for a knock every 15 s,
  publishing nothing; on a knock both publish and punch (UDP + TCP, 15 s each); items "gone" after.
- **Trust on first use**: CPace on the meeting hour's password, `Noise_XXhfs+psk0`, then the static
  must match: "This is not the Bea you remembered: its security key has changed…".
- **Not built**: marks on nearby tiles. Clocks > 1 min apart may miss around the hour.

## Relays (#472)

Only **through a computer of the person's own** or **their own relay server**. Handshake and
session run end to end; the relay sees ciphertext only.

- **Own computer** (`relay.*`, `engine_relay.cpp`): toggle **Use my other computers to relay**;
  devices then show **<name> is my computer** (`known.h`). From 1.5 s into a direct-code round, A
  meets each own computer R (≤ 45 s) and sends `relay` (20) with the code's places. R serves only if
  its toggle is on and it marks A as its own, connects only to those public places outside its own
  networks (`relay::fence`), answers `relay_answer` (21), then carries `relay_data` (22) once the
  bytes start as an internet hello. One at a time, ≤ 256 GiB each way, 120 s idle; **Relaying for
  Ann** / **Through R (your computer)**. Relayed sends do not move; word codes and remembered sends
  are not relayed; `kSilenceMs` 20 s.
- **Own relay server** (`splice.*`, `tools/driftwire-relay`): field **Relay (your own server)**
  `<address or name>:<port>#<key>`; key in the secret store, address in settings, never in the code.
  Each code registers a **waiter** under a random 16-byte token sealed in the code (server slower
  than 3 s is left out); the sender tries it 1.5 s after the direct places.

  **The hop to the server is sealed** (protocol 3): `MVRL`, 3, role, session id, CPace on the server
  key; a sender sends its token's keyed-BLAKE2b id (no waiter → closed); then Noise NNpsk0, server
  first. Sealed server words **registered** and **paired**; then raw splice. Waiters re-register
  every 60 s. Caps: 16 handshakes (4 per source), 64 waiters (4 per IPv6 /64 or IPv4 /24), 8
  splices, 64 GiB each way, optional rate, 120 s idle. Not post-quantum; no state on disk.
- TURN and Tor are not used ([plan](../plans/driftwire-tor-spike.md)).

## Claiming and pairing (phone access)

`src/addons/phone/desk.*`, `wire/advert*`, the owner's half of `wire/serve.*`; plans `docs/plans/mobile.md`
§7.2, §7.4, §7.5, `docs/plans/phone-in-the-app.md`, `docs/plans/phone-access-addon.md`. Phone access is the
`phone-access` add-on's since 2026-11-14 (`src/addons/phone/link.*` via `route_request`, caller "phone"),
items under "MediaViewer Phone access" (`desk::take_items`).

**Claiming.** Only from the service's own network (`net::on_link`) and only while unowned (or for a
hello the access key does not open), the service reads `.mediaviewer/claim.token` (64 hex) under its
named folders; a hello opening under `claim_key` (`channel::hello_opens_under`) is a claim channel.
It then sends a nonce that must appear in `.mediaviewer/claim.proof` (write proof). The device key
becomes `owner`; the answer carries the access key; `<folder>/.mediaviewer` becomes named folder
`Catalogue`. The desktop deletes the token. Owned: only the owner's key may re-claim (same keys);
others are closed after the handshake. `driftwire serve disown` forgets the owner.

**Who may do what.** Phones hold read-only grants. Only the owner's session has `kAdminOp` and may
send `serve_admin` (devices, add/revoke, pairing code, named folders); others get `refused`.

**Pairing.** Single-use code on the service's clock; a new one withdraws the last (`pair_cancel`).
`serve_pair` before `serve_hello`; failure `bad-code`; a paired phone rescanning gets `serve_paired`.
Codes hashed in memory, ≤ `kMaxPairCodes`. The desktop's list is the service's (`phone_devices`). A
revoke while the NAS is away is queued (`phone_revoke_pending`), retried 30 s doubling to an hour.

**The owner's passphrase** (`owner_pass.h`; `docs/plans/nas-owner-and-guests.md` §3). Set at `serve
init` or the first claim; Argon2id (3 passes, 256 MiB) → Ed25519 seed; the service keeps salt, cost,
public key (`owner_passphrase`). The owner signs a nonce bound to `channel::handshake_hash`, the
service identity and its device key; clients refuse challenges under 64 MiB.

- Claim adds `serve_claim_challenge` (38) after the write proof; a NAS with a passphrase refuses an
  older desktop (`passphrase`).
- **Needs it**: adding devices, pairing/guest codes, extending or widening grants, folder changes,
  passphrase change; unlock lasts 10 min (`challenge`, `unlock`, `lock`, `owner_status`). Reading,
  revoking, cancelling and narrowing need only the device key.
- **Lockout** (`owner_lockout`, written before checking): 5 free, then 15 min doubling to a day,
  ≤ 1 wrong answer/s.
- **Recovery**: `driftwire serve passphrase reset` as the service's user.

**Timed and scoped phones** (`grants.h`): in `library_grants`, never the paired list (older services
refuse them); guest codes in `library_guest_codes` (`guest_code`; time starts at redemption). The 2
s watch cuts expired sessions (`expired`) and refreshes changed ones (`folders-changed`). A scoped
phone sees only its folders and the Catalogue. **Folder scope is not offered yet** (needs per-scope
catalogues). `serve_caps` reports `features`, passphrase state and a limited phone's end.

**Nearby.** `desk::start_nearby` runs a second `serve::server` on the Mac over the local copy,
`lan_only`, unclaimable, same access key and devices, advertised `_mediaviewer._tcp` (TXT `v=1`,
`lib=` first 16 hex of SHA-256 of the library id; `advert_mac.cpp`). Windows: `DnsServiceRegister`
(`advert_win.cpp`, instance `MediaViewer-` + 6 hex). Linux: `advert_none.cpp`.

## The NAS service (`driftwire serve`)

`serve.h`, `serve_wire.h`, `serve_client.h`, `channel::open_library`; plan `docs/plans/mobile.md`
§7.2, M2b. Headless: a paired phone lists and reads named folders through one forwarded port. No
DHT, mapping or beacon, except that under `driftwire host` its router keeper (`host_ports.h`) holds
the host's own ports while it runs (decision log, 2026-11-14).

```
driftwire serve [--data DIR]              the service: DIR/serve.json, DIR/secrets
driftwire serve init [--passphrase-stdin | --no-passphrase]   identity + access key (shown once)
driftwire serve status                    identity, owner, passphrase, lockouts
driftwire serve devices                   paired devices, timed ones with their end
driftwire serve add NAME KEY [--for D]    pair by public key (64 hex)
driftwire serve grant NAME-OR-KEY         --for D (24h, 7d, 90m, seconds) or --forever
driftwire serve revoke NAME-OR-KEY        a running session is cut within 2 s
driftwire serve pair-code [S] [--for D]   single-use QR code (default 600 s); --for: guest code
driftwire serve passphrase set|reset
driftwire serve disown
```

`serve.json`: `{"port":N,"name":"...","roots":[{"id":"16 hex digits","label":"Photos","path":"/volume1/photo"}]}`.

**Who gets in**, in order:

1. Silent limits, coarsest first: per IPv6 /48 120/min, 4 in progress; per address (/64) 30/min, 2
   in progress; 4096 sources per table; 200 admitted/s overall; ≤ 16 handshakes (evict an incomplete
   hello after 500 ms, else a handshake stalled > 1 s after reply; full hello gets 2 s).
2. `read_library_hello`: `MVSP`, 2, `L`, a two-byte length equal to Noise message 1; all 1256 bytes
   read before judging.
3. `Noise_XXhfs+psk0` keyed by the **library access key** (32 bytes, in the QR); the ML-KEM key is
   sealed under a psk-mixed key, so without the key: one AEAD open, no reply
   (`noise::kem_encapsulations()` asserted in tests).
4. Replay ring of the last 4096 opened first messages (memory).
5. `serve_pair` with a code (hash + expiry) → added, `serve_paired`; else `{"why":"bad-code"}`.
6. `peer_key` must be paired (read per connection), else `{"why":"not-paired"}`; re-checked every
   2 s, cut with `{"why":"revoked"}`.

Keys and lists in `secrets::file_store` (0600 files, renamed in, refused if group/world-readable or a
link; folder 0700, this user's). An unreadable value is an error, never regenerated. **As root, or
elevated on Windows, it refuses to start** (except the test build).

**The protocol.** Message ids 25–31, 33–34 (`wire.h`) plus ident, ping, cancel. `serve_hello`
(versions, operation names) → `serve_caps` (intersection); unknown operation →
`end_code::unsupported`. Streams with client ids: `roots`, `list` (NDJSON), `stat`, `read_range`
(`file_reader::read_at`). Credit per stream (≤ 8 MiB; the phone opens a viewer's read with 1 MiB and
a whole-file pull with all 8, since one window per round trip caps a download); interactive chunks ≤ 64 KiB, bulk ≤ 256 KiB,
one bulk per four interactive. `stream_cancel`; seek = cancel + new `read_range`. No rejoin or
replay; reads are idempotent.

**Addresses after pairing.** `serve.json` keeps where devices find the service: `local` and `public`
(set by the owner's desktop with `serve_admin set_addresses`, which Phone access sends whenever the
service's caps disagree with what it knows; feature `addresses`) and `relay`. Every session's
`serve_caps` carries them as `"addresses":{"local","public","relay"}`, each `host:port` and present only
when set. `mobile::link_session` replaces its addresses with each one given (an empty one never wipes
a kept one) and races them next time; the desktop's `remote::library` saves them to `servers.json`,
the phone (`mv_link_addresses_now`) to its Keychain record. The handshake pins the service's key, so a
changed address cannot lead to another server. Older clients ignore the field; an older service
sends none and nothing changes. `driftwire host pair` (the CLI's own host path) does not set them yet.

**Paths.** Root id + relative path; absolute paths never sent or logged. `safe_relative`, then
`io::stat_beneath`, `list_beneath`, `file_reader::open_beneath` walk part by part from the root's
descriptor (`openat(O_NOFOLLOW|O_DIRECTORY)`); Windows holds each folder open without share-delete.
Listings omit hidden and link entries.

**Rule 6.** Prints its port and stop; counts totals (`server::stats`); logs no address, path or name.

**Revocation removes a device, not the access key.** A revoked phone can still drive the first
handshake steps. Rotating the access key is not built.

**A folder that is not there** (a drive unplugged, a share not mounted): `roots` lists it with
`"offline":true` (only then); a failed request on it ends `not_found` `{"why":"offline"}`; the session
goes on. Looked at for `roots`, before a write at its top, and after a failure. A `host share` folder
with `"mount":true` (POSIX) or a `"volume"` GUID path (Windows) is also offline while unmounted or
while its letter names another volume (`server::options::root_present`, `cli/volume.*`), checked
before every write. Neither field is sent.

**Not built**: the Synology package; release signing of the NAS update `latest.json` (the updater is
built: `cli/nas_update.*`, `"[nas-update]"`, Ed25519-signed per-arch manifest, `serve self-test`,
`.prev` rollback after three failed starts, `MV_NAS_VAR`, `MV_NAS_UPDATE_KEY`, exit 75); claiming
over SMB with owner operations over the channel; library rejoin; the phone's `remote_file`.

### Phone backup: the write grant (1.1)

`src/wire/serve_backup.*`, `src/wire/grants.*`; plan `docs/plans/mobile.md` §10b, M11. A backup
folder is `"backup":{"limit","reserve"}` in `serve.json` (owner's `add_root` / `set_backup`;
Settings → Add-ons → Phone access). A phone listing `write` gets the op (bit `1u << 6`).

**Who may write is a phone's grant** (`backup`): the paired list holds only unlimited devices with
backup; others live in `library_grants`. Guest codes give backup off. Turning backup on needs the
unlock. Checked per write (`device_may_write`).

A write: `stream_open {"op":"write","root","rel","at","size","hash"}`, `stream_credit`,
`stream_data`. Errors `not-allowed`, `name` (> 200 bytes), `limit`, `full`, `hash`. Folders made
beneath the root, never through a link; an identical file (name, size, BLAKE3) answers at once;
writes hidden `.<name>.<device id>.mvpart`, then flushes, hashes and renames without replacing
(`" (2)"`), one commit per folder at a time. Only the device's own `.mvpart` and
`.mediaviewer-phone-backup/<device id>/ledger.json` (`"meta":true`) are ever replaced. Phone side
`src/mobile/backup.*`.

### Clips the phone can't play: `convert` (2026-11-15)

`src/wire/serve_convert.*`, `cli/convert_run.*`, `src/edit/clip_convert.*`; plan
`docs/plans/phone-clip-convert.md`. Op 7 `convert` (bit `1u << 7`, not in `kAllOps`): listed in
`serve_caps` only while the server has a converter (`server::options::converter`,
`set_converter`); afterwards a request is `unsupported {"why":"off"}`. `{"seg":-1}` is the plan
(`{"duration_ms","segment_ms","segments","width","height"}` in `stream_end`); `{"seg":k}` streams
segment k's MPEG-TS (H.264 + AAC) exactly as a `read_range` once it is made. The path walks
`stat_beneath` first, as a read does.

`convert::service`: one worker for all sessions; asked segments first, then two ahead of a clip's
second (or later) asked segment, only for the clip asked last; a seek drops what was ahead. Cache
`serve/convert-cache/<BLAKE2b(folder id, rel, size, mtime, format)>/plan.json, seg-k.ts`, 2 GiB,
oldest-used segments out; off empties it. Each job is one run of the app's clip helper
(`MediaViewerClipJob --convert-plan|--convert-segment`, exit 0/1/2 not a clip/3 no encoder), which
lowers its own priority and encodes on the hardware encoder only. `driftwire host convert on
[--helper PATH]|off` (host.json `convert`, `convert_helper`; on by default, the Phone access add-on
names the helper); state.json `"convert":{"on","available","encoder","busy"}`; `driftwire serve
--convert-helper PATH` for a bare service. Tests `"[convert]"`; timings `"[.convert-bench]"`.

### Through the person's own relay

Plan [nas-relay.md](../plans/nas-relay.md). No public relay: away from home a phone reaches the NAS
only through the person's `driftwire-relay`.

- **Waiting** (`serve.cpp` `relay_loop` / `wait_at_relay`): with `"relay"` in `serve.json` and
  `relay_key` stored, one waiter per paired device (≤ `kMaxRelayWaiters` 4), re-registering every
  `kReregisterMs`. Token `relay_token` = BLAKE2b-128 keyed by the access key over `"MediaViewer
  relay token v1" || device_pub`.
- **Admission** (`admit()`) keyed `relay:<token hex>`, never local (no claim via relay);
  `stats().relayed`.
- **Set by the owner** (`set_relay {address, key}`, feature `"relay"`); the QR (`"v":1`) may carry
  `"relay":{"address"}`.
- **The phone** (`src/mobile/link.cpp`) joins its waiter (`relay::join`) 1.5 s after direct routes;
  direct always wins; pairing never relays; "Connected through your relay".
- **Then a direct path**: a relayed session may get `kUpgradeOp`; candidates via `serve_upgrade`
  (LAN at a `tcp_rendezvous` port, public as seen by the relay's **`kAsk`** role); both punch 10 s;
  the phone redoes the handshake and swaps in. Retries 1, 5, 15 min, then hourly; on mobile data only
  with **Faster direct connection on mobile data** (`mv_link_set_upgrade`). ≤ 8 candidates
  (`usable_candidate`), one ask per device per 30 s, 4 punches at once. Tests `"[phone-upgrade]"`,
  `"[phone-relay]"`.

### On your own computer: `driftwire host`

[Self-hosted.md](../plans/self-hosted.md) slice 2 (2026-11-14): the job a NAS would have done, on a PC
or a Mac, from one command that runs while its user is logged in.

```
driftwire host [--data DIR]                 the supervisor, no terminal: what starts at login
driftwire host share PATH [--label L] [--backup]   a folder for phones (--backup: they may back up into it)
driftwire host unshare LABEL                takes one back (its files stay)
driftwire host status                       name, ports, folders by label (offline or not), phones,
                                            VPN, staying awake, start at login
driftwire host start-at-login on|off        per user, no administrator
driftwire host stay-awake on|off            the "system required" request while hosting (off for laptops)
driftwire host setup                        the guided run: a name, a folder, the router, a phone, the VPN (slice 5)
driftwire host name duckdns|none            the DuckDNS name; subdomain and token typed or pasted, never arguments
driftwire host name domain NAME|none        your own domain for the VPN (a CNAME to the DuckDNS name)
driftwire host pair                         a phone's pairing QR in the terminal
driftwire host vpn on [--tls-port N|off] | off  the laptop's VPN exit; `on` prints its code (slice 7)
driftwire host pause | resume | quit        the server menu's control: Hosting off / on; quit stops it
driftwire host status --watch               the status again whenever it changes (usage, phones connected)
```

`/host …` in the interactive program runs the same (`/host` alone is `status`): what only prints
(`share`, `unshare`, `status`, `vpn`, `name none`, `start-at-login`, `stay-awake`) says its lines in
the transcript; what asks or draws a QR (`setup`, `name duckdns`, `pair`) gets the terminal as a shell
leaves it (`terminal::run_cooked`: raw mode off, the live region erased) and the session takes it back.
`/host` and `/host child` say the host runs on its own.

- **One process for the services.** `driftwire host` is a supervisor: it runs the same program as
  `host child`, which holds `serve::server` for the shared folders and the relay server (`splice.*`,
  as `mv-driftwire-relay`) together, and the power request. The child's stdin is a pipe the
  supervisor holds: closing it (or the supervisor dying) stops the child. A child that ends unasked
  (a crash, an exit) is started again after 30 s; one that says it cannot run (exit 3: its folder is
  not this user's, its keys cannot be read) ends the supervisor with 3, which the systemd unit does
  not restart (`RestartPreventExitStatus=3`; launchd and Task Scheduler still try every 30 s / 5 min). Inside the child, a service
  whose port is taken is tried again every 30 s without stopping the other.
- **Its folder** is `<data dir>/host` (0700): `host.json` (`relay_port`, `vpn`, `stay_awake`; from
  slice 5 `ddns` and `ddns_stamp`, `tls_port`, `library_id`),
  `serve/` (`serve.json` and the service's secrets, the layout `driftwire serve --data` reads),
  `relay/secrets` (the relay's key, made at the first run, never printed: slice 7 prints the relay
  line), `secrets/` (for the keepers), `state.json` (the running child's ports, counts and pid, for
  `host status`), `host.lock` (one supervisor). The child also holds `serve/serve.lock`.
- **Changes while it runs** come through the files: `serve.json` and `host.json` are read every 2 s
  (one small file each; nothing renders in this process). A `host share` from another terminal is
  served within that; the running service takes the folders with `server::replace_config` (requests
  already open keep the view they opened with); a new port restarts it; no folder stops it, the relay
  stays. The service's own writes (the owner's edits from the desktop) and `host share` take
  `serve/serve-config.lock`, and the service's write is refused when the file changed since this
  process read it (the desktop tries again), so neither loses the other's change.
- **Drives.** Every minute each folder's drive is looked at (`cli/volume.h`): a POSIX folder that was
  a mount point when shared (`st_dev` differs from its parent's) is offline while it is not one; a
  Windows folder on a volume mounted at a drive letter's root is kept by its volume's GUID path too,
  and when the volume is back under another letter the folder is moved there and `serve.json`
  rewritten (one under a folder mount point keeps its path). A desktop's `add_root` is refused,
  like `host share`, for a folder holding the host's own folder.
- **Starting at login** (`cli/autostart.h`, one file per OS): a per-user Scheduled Task at log on,
  least privilege, tried again every 5 minutes while not running (Task Scheduler's own restart is for
  a start that fails, not an exit), made with `schtasks /Create /XML`; a LaunchAgent
  (`io.github.longtimeno-c.driftwire.host`, KeepAlive unless it exited 0, ThrottleInterval 30); a
  `systemd --user` unit (Restart=on-failure, 30 s) with `sudo loginctl enable-linger` printed, never
  run. The entry runs the program's stable path (a Homebrew Cellar path becomes its `opt` link).
  A host started while one runs says so and exits 0.
- **Updates.** The supervisor looks at its program every minute; replaced (an install channel's
  update, `driftwire update`), it stops the child and ends 75, and the entry starts the new one.
- **Sleep.** While hosting it holds the OS's "system required" request through `power::hold_for`
  ("Driftwire is hosting your server"): the display may still sleep. `host stay-awake off` drops it.
- **Elevation.** Refused as root or elevated, as `serve`.
- **The server menu** ([plan "Tray"](../plans/self-hosted.md), issue #686 §4): while the host runs,
  its own menu-bar item / notification-area icon in the app (#452's mechanism; every kept entry now
  has its own item): the head with the DuckDNS name, **Hosting** on/off (off is `host pause`), status,
  drive health (folders offline, PR #692's sets), today's bytes read and written and the rate now, each
  phone connected by its paired name with how (home network, over the internet, through your relay),
  since when, its bytes and whether it is reading or writing; **Quit hosting**, **Open Phone access
  settings**. The app reads `host.lock` and `state.json` (`phone/host_tray.h`, one thread, a stat
  every 5 s, a read only on a change) and runs the verbs; until Phone access is an add-on it is the
  registry's one non-add-on kept entry (`set_shell_keep`, `"shell": true`). The host's start at login
  also wants the app's login item. `pause` writes `host.json`'s `paused`; `quit` its `quit_stamp`
  (a child of a supervisor started before it ends 4; the supervisor stops and clears it).
  `state.json` gained `paused`, `clients` and `usage` (from `serve::server::usage()`: per-session
  atomics, folded at the 2 s pass by `cli/host_usage.h`; today kept in `usage.json`). The card's status
  row and `host status` (`--watch`: again on each change) say the same lines.
- **Rule 6.** Its lines say ports and counts, never a path or an address; `state.json` holds ports,
  counts and a pid. `host status` prints folders by their labels.
- **Keepers** (`cli/host_keepers.*`, slice 5): the DuckDNS keeper (`ddns::keeper`) and the router
  keeper (`host_ports::keeper`), started after the services and stopped before them. Each rebuilds
  on a worker of its own when the context changes (the service's port, the relay's and its TLS port,
  the VPN, the DuckDNS name or its `ddns_stamp`), so neither the host's loop nor `start()` waits on
  the network. The router is asked for the service's port while it serves, and the relay's (and the
  web-site look at outside 443, falling back to its own number) only with the VPN on; a new public
  outside address from the router kicks DuckDNS at once. Each writes a status line into `state.json`
  (`host status` prints it: the name and "updated 3 min ago", or each port open or not, the outside
  kind and `host_ports::advice()`; while nothing is open yet the router is asked again after 1,
  5, then every 15 minutes, a plain refusal three times at most), and the router keeper the outside ports a phone and the laptop
  dial (`service_outside_port`, `relay_outside_port`, `tls_outside_port`). In the test build
  (`loopback_only`) both stay idle and say so, unless built for a stand-in.
- **From the desktop** (slice 6, [plan](../plans/self-hosted.md) §17): Settings → Add-ons → Phone access →
  Set up the service… → **This computer** runs the installed program's `host share` and
  `host start-at-login on` on the phone link's thread, writes the DuckDNS name where slice 5's
  `host name duckdns` does, claims the host over loopback and shows a code; its status row reads `state.json` and
  `host.lock`. The app does not carry the program ([12](12-decision-log.md#driftwire), 2026-11-14).
- **Not yet:** watching for a volume arriving (the minute's recheck only).
- **Tested** (`"[host]"`, loopback only): the arguments, `host.json`, sharing rules, the three entries'
  texts and the files written into a home of the test's own (the OS's tools never run in a test), the
  drive checks (`/dev` as a real mount point), the service process in-process (relay alone, a share
  served, a second share without a restart, offline and a new letter, the VPN restart, a broken
  `serve.json`, unsharing all), and the supervisor as a program on POSIX: a second supervisor exits 0,
  a SIGKILLed child is started again, a replaced program ends it 75 with its child, SIGTERM ends both
  with 0. Windows and Linux run in CI only; the entries were never registered with a real
  launchd, systemd or Task Scheduler.

### Setting it up: `driftwire host setup` (slice 5, 2026-11-14)

One guided run in the order a person needs it (`cli/host_setup.*`, plan §2):

1. **A name.** A DuckDNS subdomain (typed as `home` or `home.duckdns.org`) and its token, read with
   echo off and never an argument; one update (with `clear` first, so an old IPv6 record goes), then
   the name resolved and compared with what DuckDNS said it set, again every 10 s for about a minute
   and a half (the 60 s TTL). Refused, unanswered and mistyped answers each say so; three tries, then
   no name. The token goes in the host's secret store (`ddns_token`), the subdomain in `host.json`
   with a `ddns_stamp`, so a running host takes a new token for the same name too. Skipped: "phones
   reach this computer at home only".
2. **A folder** (`host share`), and optionally a backup folder with a limit in GB (the 1 GiB reserve
   stays).
3. **The router.** With no host running, a probe `host_ports::keeper` for the service's port (and the
   relay's with the VPN on) under its own ledger, its result and `advice()` printed (the one port to
   forward by hand; carrier NAT: a relay on a rented server, §5.4; double NAT), then removed: the
   running host asks for its own. With a host running, its router line from `state.json`. Then the
   honest outside check: the phone on mobile data.
4. **A phone** (`host pair` alone): the pairing QR drawn in the terminal (`cli/qr.*`, the Windows
   card's encoder in C++, Project Nayuki's MIT algorithm, byte mode at ECC M, pinned to the same
   scanner-read matrices; black on white set by the line itself, so it reads on a dark terminal;
   half-block characters, two module rows a line, about 99 columns for a pairing code). The payload is
   `catalog::pair_url`'s, field for field: the service's local address (toward the gateway) and the
   DuckDNS name with the router's outside port as the public route, a single-use code for 15 minutes,
   and the check the phone shows. No relay route: the host's relay is behind the same router.
   **The host library**: a phone pairs with a library, and a host has no desktop library, so `host
   pair` makes its own: an id in `host.json`, a publisher key from a seed in the host's secrets
   (`host_publisher_seed`), and its `library.json` (as the desktop writes one on a NAS share) in
   `<data dir>/library/.mediaviewer`, shared as the "Catalogue" folder. The phone finds that
   library.json and opens the shared folders in file mode. The Catalogue goes with the last of the
   person's folders on `host unshare`. A service a desktop has claimed (its Catalogue is there) is
   refused: its phones pair from the desktop's card, with its library.
5. **The VPN** (`host vpn on`, below), then **start at login**.

### The VPN exit: `driftwire host vpn on|off` (slice 7, 2026-11-14)

`host vpn on` sets `vpn` in `host.json`; the running host restarts its relay with `relay::vpn_host_defaults`:
forwarding to every port (`kAnyPort`, TCP and UDP; all traffic, owner 2026-11-19) apart from
`never_forward`'s (TCP 23, 25 and 445, and 135 and 137-139 both kinds: what gets a home address
blocklisted; mail apps use 587, 465 and 993), 128 TCP forwards and 128 UDP flows apart (a lookup's flow
gives its slot back after 10 s idle), UDP out at most 4 MiB/s for all flows together, never to the
home's outside address (`forward_refused` = `refuse_name_addresses` of the DuckDNS name and the own
domain: the router's address hairpins to its forwarded ports), and never to a private, loopback or
link-local address, nor to any of the host's own networks (`relay::forward_destination_ok`; owner call
5, no VPN into the home network). **The web-site look is on by default** with the VPN
([vpn-stealth](../plans/vpn-stealth.md), 2026-11-19): `look_port` is `tls_port` or the relay's port
+ 1 (any user may listen there, so Linux needs no privilege and a host that serves 443 keeps it), and
the router keeper maps **only it** outside, at 443, else 8443 (`kLookFallbackPort`): the plain
relay's cleartext `MVRL` head never reaches the internet. `--tls-port N` moves it, `--tls-port off`
turns it off (`"look": false`; then the plain port is mapped as before); a bare `vpn on` (the
desktop's VPN code asks it) never changes it. It prints, once, the code
`mediaviewer://relay-tls#NAME:443#KEY` (the plain line `NAME:PORT#KEY` without the look, or when it
could not listen), `NAME` the own domain, else the DuckDNS name, else the LAN address: the only
place the relay's key is ever shown. The look's certificate is Let's Encrypt's when the certificate
keeper has one (below), else self-signed for the name. `off` turns forwarding off, and the router's
port closes.

**The certificate** (`acme.*`, `cert_keeper` in `host_keepers.cpp`): with the look on and a DuckDNS
name and token, an ACME client (RFC 8555; BoringSSL for ES256, the CSR and reading certificates;
`acme_none.cpp` without it) gets Let's Encrypt's certificate for the name by DNS-01, the TXT set and
cleared through DuckDNS (`ddns::set_duckdns_txt`), so no port 80. Files in `<host>/tls`
(`account.pem`, `cert.pem`, `key.pem`; mode 600). Checked at start and daily, renewed 30 days before
the end; after a failure 1, 2, 4 … 24 h (or the CA's Retry-After), at most 3 orders a day. The relay
takes a new one with `relay::server::set_tls_certificate` (new connections; open forwards keep
theirs). HTTPS to the CA is the command line's own (`cli::https_request`: NSURLSession, WinHTTP,
libcurl), never logged. `host status` has a `certificate` line. Tested against a stand-in CA in the
test process that checks every JWS signature, nonce and URL (`"[acme]"`); never Let's Encrypt.

**Your own domain** (`host name domain NAME|none`, `host_settings::domain`; the card's **Your own
domain** row runs the same command): the code, the look's SNI and its certificate carry it instead
of the DuckDNS name, which some networks block. DuckDNS still keeps the address: the person adds
`NAME CNAME SUB.duckdns.org` and `_acme-challenge.NAME CNAME _acme-challenge.SUB.duckdns.org`, which
DNS-01 follows. Without DuckDNS the person points it themselves and the certificate stays
self-signed. One name per certificate (DuckDNS keeps one TXT value). Tested on loopback: a laptop's Tunnel proxy
(`tunnel::proxy`) with the printed line reaches a site through the host's relay, off refuses, and the
shipped options refuse a forward to the home's own addresses (403).

**From the desktop** (Settings → Add-ons → Phone access, once this computer is the server): the
**VPN through this Mac/PC** row has a **Use as VPN** switch that runs the installed program's own
`host vpn on|off` on the phone link's thread (`link.cpp` `host_vpn`; no second relay, forwarder or
`host.json` writer in the app), and its status: what `host.json` asks for (`vpn`, `tls_port`) and what
the running relay does (`state.json` `vpn` with a relay port, `relay_outside_port`, `relay_tls_port`,
`tls_outside_port`, `tls_failed`): off, "Turning on…", "On once the server runs", or "On · looks like a
web site on port 443" (the plain "On · relay port N" without the look) and "home network only" without
a name. The card waits up to 5 s for the running host to take the change. **VPN
code…** asks `vpn on` again and shows the line it printed (`self_host::relay_line_in`) as a code to copy
(below), until Hide, the VPN going off or Settings closing; it is asked once more when the relay came
back on another port than the one printed. Once the library is claimed, this row (or, when a NAS is the server, the NAS's row below) sits in
the card's **Away from home** section. The card's tree is the add-on's own, so the Mac and Windows chromes draw the
row with no code of their own. **Phones do not use it yet**: the iPhone app has no VPN
([phone-vpn plan](../plans/phone-vpn.md), not built), and the pairing code carries no relay route for a
host (its relay is behind the same router), so a phone reaches the library as before.

### The NAS as a VPN host (2026-11-15)

A Synology (or any `driftwire serve`) can be the laptop's VPN exit too, with the same relay. `serve`
offers it (`options::vpn`, set by `serve_cmd.cpp` only; the service inside `driftwire host` does not,
since the host runs its own relay) as feature `vpn`. The owner op **`set_vpn`** `{on, port?}` (unlocked
once there is a passphrase) keeps a key of its own in the service's store (`kVpnKeyName`, apart from the
person's other relay's `kRelayKeyName`) and `vpn_port` in `serve.json` (0: off, 47614 by default), and
runs a `relay::server` inside the service with the host's `vpn_host_defaults`, refusing the
addresses of its outside name as the owner's desktop last set it, public addresses only, and the web-site look on the next port (self-signed; the plain relay alone
when it cannot listen). `set_vpn` answers `tls_port` then, and the card's code is
`mediaviewer://relay-tls#NAME:PORT#KEY`; the SPK's service file names that port too
(`[mediaviewer-nas-vpn-web]`, 47615) so DSM's router page offers it (outside 443 is the person's to map). On answers the port and the key, once, for the owner's line; a service
started with `vpn_port` set starts the relay again; on again for the same port leaves a running relay as
it is (asking for the code never cuts a laptop's forwards). `owner_status` gains `vpn_port`,
`vpn_running`, `vpn_listening`. The desktop card (claimed NAS, the feature there) shows **VPN through
your server** with the same switch and **VPN code…**; the line is the NAS's own name from outside (else
its address on the network) with that port. The SPK's `conf/mediaviewer-nas.sc` gains a
`[mediaviewer-nas-vpn]` section for 47614/tcp, so DSM's firewall and router offer name it. **Never run
on a real Synology** (the owner's unit died 2026-11-14): tested on loopback and by the package tests only.

### Connect to a VPN host (2026-11-15)

The line holds the relay's key and is long to type, so the host's card gives it as a code,
`mediaviewer://relay#host:port#KEY` (`serve::relay_connect_code`), and the laptop's Driftwire add-on has
a **VPN host** card on its settings page (Settings → Add-ons → Driftwire): **Connect to a VPN host…**
opens a secure field; Connect reads the code or a bare line (`serve::parse_relay_connect_code`: spaces
and a newline around it, the scheme in any case; one `#`, a key of at least 16 characters, a host:port
the relay field takes), fills **Your own relay server** as typing it would (the key in the secret store,
never the settings file), and turns the Tunnel on; All traffic (VPN) is then one switch. The card names
the host, never the key. One parser in `src/wire`, used by both add-ons; both chromes draw both cards.
A `mediaviewer://relay-tls#` code (the web-site look's) also turns on the Tunnel's **Look like a web
site** at the code's port; a plain code turns it off (`parse_relay_connect_code(text, &tls)`).
Measured with `"[.vpn-host-bench]"` (a host's relay with the shipped forwarding on this Mac, the
Tunnel's proxy, curl to a public download, alternated with direct): the same throughput as direct, about
35 ms more to the first byte (numbers in PR #711).

### DuckDNS: a name for a home's changing address (self-hosting, 2026-11-14)

For a computer that hosts the service itself ([plan](../plans/self-hosted.md) §4), `ddns.*` keeps
`SUB.duckdns.org` on the home's public address so the phone's QR and the laptop's relay line can
carry a name. Built as a module of its own (slice 3); `driftwire host` (slice 2) runs it, nothing
else does (never the viewer, never the phone).

- **One provider, DuckDNS** (`ddns::provider`, `ddns::duckdns`): one HTTPS GET,
  `…/update?domains=SUB&token=TOKEN&ip=[V4]&ipv6=[V6]&verbose=true`, answered `OK` with the addresses
  set and `UPDATED`/`NOCHANGE`, or `KO`. The HTTPS client is the command line's (`cli::https_get`),
  handed in as a function that returns a status and a body only, never the transport's error text
  (which can hold the URL). DuckDNS keeps an old `ipv6=` until `clear=true`, which clears both
  records, so an update with no IPv6 clears and sets the IPv4 again when one is shown, on the
  keeper's first update, and until an update without one has succeeded. A non-public IPv4 from
  the router (carrier NAT) is not sent: DuckDNS then uses the address it sees.
- **When** (`ddns::schedule`, `ddns::keeper`): at start, every 5 minutes, at once when told the
  address moved (`address_changed`) or the IPv6 the port is open on changed (`set_ipv6`; only a
  public v6 counts, else the record is cleared), never twice within a minute, and 1, 2, 4 … 30
  minutes after failures in a row. `status()` is what `host status` shows: the last outcome and
  time, the last success, failures, the next try, the addresses DuckDNS reported.
- **What the person types**: the subdomain (`parse_subdomain`: one label of `[a-z0-9-]`, a
  trailing `.duckdns.org` dropped, any other dot refused) and the token (`parse_token`: a UUID and
  nothing else, so only hex and `-` reach the URL), typed or pasted, never an argument. The token
  is kept in the secret store as `ddns_token`; the subdomain in the host's config.
- **The check** (`check_name`): resolves the name and compares it with what the update reported,
  for setup; a fresh record may take DuckDNS's 60 s TTL to show.
- **Tested on loopback only** (`tests/test_driftwire_ddns.cpp`, `"[ddns]"`): a stand-in DuckDNS on
  `127.0.0.1` over plain HTTP (the real fetch takes https only) answers OK, KO, an HTTP error, a
  hang past the timeout and nobody there; the stale-IPv6 clear; the schedule with made-up times;
  the keeper's thread updating at start, on its period, at once on a change, backing off and
  recovering, and stopping while a request hangs; no log line holds the token, the subdomain or an
  address. Never the real duckdns.org. Not run: the real `https_get` against DuckDNS, Windows and
  Linux.

## The Tunnel (2026-11-12)

Reach the web through one's own relay server on a filtering network. Plan
[driftwire-menu-and-tunnel](../plans/driftwire-menu-and-tunnel.md).

- **Forwarding** (`splice.*`): role `kForwarder` (2): CPace (`…forwarder-server`) then Noise NNpsk0;
  the client's sealed answer carries kind byte, port and host (`forward_host_ok`). Reply `F`, or `O`
  off, `X` port, `B` busy, `U` unreachable, `N` private, `K` kind. Then **sealed frames**
  (`sealed_link`: 2-byte length + AEAD ≤ 16 KiB). Ports 80/443 unless `--forward-ports` (`all`:
  every port but `never_forward`'s, as the VPN hosts set it; `none` off), `--max-forwards` 64, public destinations only, 10 min idle; the server resolves names and
  logs only counts. Models: `tools/formal/relay.pv`, `relay_leaked_key.pv`.
- **Local proxy** (`tunnel.*`): SOCKS5 CONNECT (names sent as names) and HTTP CONNECT / absolute-form
  on **127.0.0.1**, port kept as `tunnel_port`. ≤ 128 streams, 3 opening at once. No UDP.
- **System proxy** (`sysproxy.*`): **Use for all apps** (`tunnel_all_apps`, on by default) sets the
  OS auto-proxy to `http://127.0.0.1:<port>/driftwire-tunnel.pac` (local DIRECT, else `SOCKS5
  127.0.0.1:<port>; DIRECT`; no `dnsResolve`). `sysproxy_mac.cpp` (SystemConfiguration, admin once
  a run), `sysproxy_win.cpp` (WinINet LAN, per user), `sysproxy_none.cpp` (Linux). The prior setting
  is saved in `driftwire_proxy.json` and restored on off, quit or the next start after a crash, only
  where still ours. Only hosts passing `make_sysproxy` change it.
- **One switch**: node `tunnel` (`kChange`, off by default) in the window (**Tunnel through
  <relay>**), settings page, menu bar, `/tunnel`. `sync_tunnel` on the control thread; `link` node
  bearer `via: "tunnel"`; `tunnel_mode` "proxy" or VPN. Site refusals map to SOCKS5 reply 2/4 or
  HTTP 403/502.
- **Looking like HTTPS** (`tls.*`; [spike](../plans/driftwire-tls-spike.md)): **Look like a web site
  (port 443)** (`tunnel_tls`) uses BoringSSL with Chrome's ClientHello to `mv-driftwire-relay
  --tls-port 443` (JA4 in tests `t13d1516h2_8daaf6152771_d8a2da3f94cd`). Certificate unchecked (the
  inner hop proves the key). First 32 bytes inside TLS are the **knock** (keyed BLAKE2b of exporter
  `EXPORTER-driftwire-knock`, key = Argon2id of the server key); without it the relay acts as a web
  server (`--decoy-dir`) as nginx does: its error pages byte for byte, `Server: nginx`, a `Date`,
  keep-alive (HTTP/1.1, up to 100 requests), and a short request answered at once (it stops waiting
  for a knock as soon as the bytes cannot be one). **Padded knock** (2026-11-19): this build's clients
  knock under `EXPORTER-driftwire-knock-padded` and both sides put a length byte and 0–255 random
  bytes before their first bytes, in the same record, so no connection's first records have fixed
  sizes; a server takes both knocks (an older client still connects), a client of this build needs a
  server of this build. `--tls-cert`/`--tls-key` or self-signed `--tls-name`. A VPN host's
  `relay-tls` code carries the look's port; other codes the plain port.
- **All traffic: Driftwire VPN** (`netstack.*`, `vpn.*`): **All traffic (VPN)** (`tunnel_vpn`,
  `/tunnel vpn`). `netstack.cpp`: own IPv4/IPv6 stack (TCP server side, in-order, go-back-N 400 ms,
  no window scaling; UDP; no ICMP or fragments). TCP → `tunnel::dialer` forwards; UDP → datagram
  flows (`kForwardUdp`, `--forward-udp-ports` default 53, `all` on the VPN hosts). DNS `198.18.0.2` answered by the relay
  (`dns.driftwire`, `--dns`). Addresses `198.18.0.1/30`, `fd5d:7269:6674::1/64`; everything routed
  except the relay's addresses and local networks (`vpn::plan_routes`). **Block when the tunnel is
  down** (`tunnel_block`). `vpn::os_port`: Mac Network Extension packet tunnel (`vpn_mac.mm`,
  `mac_tunnel/`, `-DMV_DRIFTWIRE_PACKET_TUNNEL=ON`, `cmake/darwin-tunnel.cmake`, embedded by
  `macpack.py` in `Contents/Library/SystemExtensions`, offered only when entitled;
  `includeAllNetworks` kill switch); Windows Wintun (`vpn_win.cpp`, routes `0.0.0.0/1`,
  `128.0.0.0/1`, `::/1`, `8000::/1`, WFP kill-switch sublayer that outlives a crash until stop, next
  start or reboot); Linux none.
- **Exposure**: the network sees the relay; sites see the relay; the relay sees hostnames, not
  HTTPS content.

## What moves, and how it lands

`transfer.h`. Originals are only read (rule 5).

- **Offered**: the handed-over files with pair halves (`pair_names`) and XMP sidecars, one listing
  per folder; or a folder (**Send a folder**, or a handed-over set that is one folder: the host's
  Share on a folder tile; looked at on the reader thread) (`walk_files`, depth 32) minus housekeeping (`@eaDir`, `#recycle`,
  `$RECYCLE.BIN`, `System Volume Information`, `lost+found`, `__MACOSX`, `Thumbs.db`, `desktop.ini`,
  dot files, `.mvtmp`).
- **Only the file picked is read**: identity noted at pick (`st_dev`/`st_ino`; `io::identify` on
  Windows, no hydration) and re-checked at open (`file_reader::open_same`, `O_NOFOLLOW`); a swap is
  `file_skip`. Links are not offered.
- **Lands in Downloads** (`net::downloads_folder`) or **Save to** (kept in `driftwire.json`). Every
  path passes `safe_relative` (`/` only, no `..`, root, drive, stream, device name, trailing dot or
  space, leading `.`; UTF-8; ≤ 1024 bytes, 32 levels; no C0/C1, bidi controls, separators, ZWSP,
  BOM). One bad path refuses the offer. Only bare names or one folder are accepted.
- **Never through a link, never merged**: `io::make_directories_beneath`; a taken folder lands as
  `Trip (2)`.
- **Marked as downloaded** (`io::mark_from_elsewhere`: `com.apple.quarantine`, `Zone.Identifier`
  zone 3), 0644. `no_room` if free space less 64 MiB is short.
- **`io::verified_copy`'s rules** ([24](24-transfer.md)): exclusive `<final>.mvtmp`, hash on write,
  match at `file_end`, flush, uncached read-back hash, no-replace rename (`io::unique_name`). Errors
  remove the temporary. mtime clamped to 1970–tomorrow.
- **Pipelined check**: two verifier threads, in order, ≤ 4 waiting; `done` after every check.
- **Staying awake** (`power.h`): `PowerSetRequest(PowerRequestSystemRequired)`, IOKit
  `PreventUserIdleSystemSleep`, logind `sleep` inhibitor (`block`); reason via `power::set_owner`.
- Both sides yield between chunks while the viewer presents (`should_yield`).

## Resuming a receive that stopped (#530)

`resume.h`, `transfer.h`'s `resume_plan`.

- **Kept**: on failure (not cancel/decline) the flushed temporary and a record in
  `driftwire_resume.json`: sender identity, destination, landed folder name, whole files (path,
  size, time, where landed), the partial (temporary, durable bytes). Rewritten ≤ 1/s; partial
  flushed ≤ every 5 s per 32 MiB. One per sender, ≤ 8, expired after 7 days; **Discard** removes.
- **Hidden temporaries** (`io::set_hidden`; Linux leading dot `.IMG_1.JPG.mvtmp`,
  `io::kHasHiddenFlag`, `temp_name`), unhidden before rename.
- **Matching**: the offer carries file times and `"resume": true`; same path, size and time →
  **had** or **taken up**. Accept carries `"have"` (runs `[first, end)`, ≤ 8192) and `"at"`
  (`[{"i","at"}]`).
- **Sender** skips had files and resumes the partial with `file_begin` `"at"`, hashing the kept
  bytes locally so `file_end` is the whole file's BLAKE3. Receiver reopens via
  `io::file_writer::open_resume`; **the read-back checks the whole file**.
- No `"resume"` or no identity → nothing kept. Records never leave the machine.

**Benchmarks**: `mv_import_tests "[.driftwire-bench]"` and `mv-driftwire-bench`
(`tools/driftwire-bench`, per-stage timing from `stages.h`, `disk_bound`, iperf3 share) →
`docs/perf/driftwire-lan.jsonl`. The app passes no stage timers (`hooks::stages` null).

## Threads

Host calls arrive on its worker for the add-on. The engine (`engine.h`) owns: a control thread (the
only one starting/stopping presence; throttled posts), the accept loop, discovery, Bluetooth's
queue, the transfer, a folder reader (replaced by a newer set, never waited on), and during a
protocol 2 send the mover with per-bearer connect threads. Each channel has a pinger (suspect 3 s,
end 20 s). None is UI or render; no lock across I/O; Bluetooth hand-offs pass a gate the bearer
closes before stopping. The window is posted whole (`build_tree`, pure), ≤ ~10/s; posts never
present the canvas.

## Privacy (rule 6)

- No path, file name, pixel, EXIF, address or peer name in logs (`log` is ASCII without them),
  telemetry or crash reports ([13](13-updates-and-telemetry.md)).
- Beacon/INFO: random id, port, protocol; the name only while the window or a `driftwire` session is
  open; otherwise `MediaViewer` and a rotating id. The long-term identity never appears in a nearby
  handshake.
- Internet (opt-in): the code reveals this public address; routers see mappings; with the DHT on,
  bootstrap names are resolved and nodes see participation, never meaningful targets. Relays only
  the person's own.
- Secrets in memory are wiped when they go; only Remember writes (to the OS store).
- Receiving is off until turned on, then persists and is shown in the command bar.
- `driftwire`'s update check sends nothing about the user; never in the add-on.
- Notifications: sender, count and size only; ≤ 3 a minute per source.
- `driftwire host`'s DuckDNS name (opt-in): DuckDNS learns the home's public address, nothing about
  files. The token stays in the secret store, never in a log, a status or an argument.

## Host pieces this added

- **Notices and keep-alive** (host table after `place_matches`, ABI 0.33), held by `slot_registry`
  ([In the background](#in-the-background-452), [25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)).
- **Screens vocabulary** ([25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)):
  `text_field` `"pick": "folder"` (**Choose…**), `tiles`, `stack` `"look": "card"`, `progress`
  `"look": "ring"`, `text_field` `"look": "code"` + `"digits"`, `text` `"display"`; `link` and a
  button's `"copy"` (contribution API 6).
- **Slot add-on commands** become command events (`slot_registry::run_command`, `mv_slots_command`,
  ABI 0.29, [14](14-abi.md)).
- **Settings → Add-ons**: **Open …**; one-click install (`mediaviewer-addon-driftwire-<platform>`);
  Driftwire's settings in place (`"settings"`, contribution API 8, `build_settings_tree`, same node
  ids as the window): Nearby, Received files, Over the internet, Remembered devices, Internet
  options. Opening it makes the engine but starts no presence.
- **The gallery's range** ([16](16-commands.md#gallery-range-and-share-413)).
- **Windows screen windows are MediaViewer windows** (`OpenScreenWindow`, `Shared/AppIcon.cs`,
  `PreferredMinimumWidth`/`Height`, owned by the viewer, `MediaViewer.AddonWindow` so the key
  router ignores them).
- **Permissions**: `NSLocalNetworkUsageDescription`, `NSBluetoothAlwaysUsageDescription`; Windows
  Defender Firewall at the first listen.

## Where it lives

The protocol is `src/wire` (`mv_wire`, `cmake/wire.cmake`), linked by the app's phone feature
([plan](../plans/phone-in-the-app.md)): `wire.*`, `keys.*`, `noise.*`, `cpace.*`, `stages.*`,
`channel.*`, `link.h`, `net*`, `machine_*`, `admission.h`, `secrets*`, `known.*`, `serve*`,
`pair_check.*`, `advert*`, `grants.*`, `owner_pass.h`. The rest is `src/addons/driftwire`
(`mv_driftwire_engine`). Namespace `mv::driftwire`. Hourly DHT keys: `presence_keys.*`.

| Piece | Code |
|---|---|
| Wire, code, path rule, beacon | `src/wire/wire.*` |
| Handshakes, internet codes, mapping, race | `noise.*`, `cpace.*`, `invite.*`, `mapping.*`, `internet.*`, `race.h`, `net_address.cpp` |
| DHT, word codes, UDP stream, punching | `dht_item.*`, `wordlist.inc`, `dht.*`, `bencode.*`, `sha1.*`, `udp_hub.*`, `meet.*` |
| Handover | `internet.*` (`way_in`), `handover.*`, `channel.*` (`handover_secret`) |
| Relays | `splice.*`, `relay.*`, `engine_relay.cpp`, `tools/driftwire-relay/main.cpp` |
| Self-host router keeper | `host_ports.*` (`"[host-ports]"` in `test_driftwire_internet.cpp`) |
| Self-host DuckDNS keeper | `ddns.*` (`tests/test_driftwire_ddns.cpp`, `"[ddns]"`) |
| Identity and remembered devices | `secrets.h`, `secrets_{mac,win,linux,file}.cpp`, `known.*`, `presence_keys.*` |
| Session, transfer, resume | `channel.*`, `link.h`, `transfer.*`, `resume.h` |
| Engine, presence, tree, prefs | `engine.*`, `presence.*`, `tree.cpp`, `prefs.*` |
| Sockets, machine name, Downloads | `net.h`, `net_posix.cpp`, `net_win.cpp`, `machine_mac.cpp` |
| Bluetooth | `bt_pipe.*`, `bt_mac.mm`, `bt_win.cpp`, `bt_none.cpp` |
| The Tunnel | `tunnel.*`, `sysproxy*`, `tls.*`, `netstack.*`, `vpn*`, `mac_tunnel/` |
| Add-on entry and manifest | `addon_entry.cpp`, `manifest.json`, `cmake/driftwire.cmake` |
| The command line | `src/addons/driftwire/cli/` (`term*`, `keys`, `line_editor`, `paths_arg`, `complete`, `theme.h`, `text`, `render`, `inline_view`, `args`, `host`, `session`, `data_dir*`, `update*`, `serve_cmd`, `serve_tty*`, `nas_update`, `main.cpp`); targets `mv_driftwire_cli_core`, `mv_driftwire_cli` (`OUTPUT_NAME driftwire`), `mv_driftwire_cli_test`, `mv_driftwire_cli_gallery` |
| Packaging | `cmake/driftwire-cli/` (`needed.cmake`), `packaging/linux/` (`driftwire.1`, `completions/`, `firewalld/driftwire.xml`, `copyright`, `repo/`), `packaging/windows/install.ps1`, `packaging/macos/driftwire.rb.in`, `tools/package/driftwire_repo.py`, `driftwire-windows.ps1`, `homebrew_formula.py`, `github-release.py`, `tools/mac/macpack.py cli`, `.github/actions/driftwire-cli-linux` |
| Mac chrome | `Driftwire.swift`, `SlotItems.swift`, `ScreenView.swift`, `GalleryView.swift`, `FolderStore.swift`, `AddonNotices.swift` |
| Windows chrome | `IslandHost.Driftwire.cs`, `IslandHost.GalleryRange.cs`, `IslandHost.Slots.cs`, `IslandHost.Screens.cs`, `IslandHost.Notices.cs` |
| Phone access (the `phone-access` add-on) | `src/addons/phone/` (`desk.*`, `link.*`, `service.*`, `screens.*`, `qr.*`, `bundled.*`, `self_host.*`, `host_tray.*`), `wire/advert_{mac,win,none}.cpp` |
| NAS service | `serve_wire.*`, `serve.*`, `serve_client.*`, `serve_backup.*`, `channel.*` (`open_library`, `read_library_hello`) |
| Self-hosting (`driftwire host`) | `cli/host_cmd.*`, `cli/host_keepers.*`, `cli/host_setup.*`, `cli/qr.*`, `cli/host_control.*`, `cli/host_usage.*`, `cli/host_users.*`, `cli/host_drives*`, `cli/autostart*`, `cli/volume*`, `cli/elevate*`, `host_smb*`, `power::hold_for`; tests `test_driftwire_host*.cpp`, `test_host_users.cpp` |
| Native hosts, notices | `src/shell/main_mac.mm`, `src/shell/main.cpp`, `src/shell/login_item_win.*`, `src/shell/addons_mac.mm`, `src/addon/slots.*` (`post_notice`, `keep_alive`, `notices_json`), `src/addon/host.cpp` |
| Formal models, fuzzing | `tools/formal/`, `tools/fuzz/fuzz_driftwire.cpp` |
| Tests | `tests/test_driftwire*.cpp` (`_noise`, `_internet`, `_dht`, `_known`, `_session`, `_serve`, `_relay`, `_addon`, `_cli`, `_cli_ui`, `_cli_update`, `_cli_e2e`), `tests/test_phone_relay.cpp`, `tests/driftwire_{fixture,swarm,path,cli_vt}.h`, `tools/package/test_*.py`, `tools/package/check-driftwire-install.ps1` |

## Not built

- **The Tunnel**: the Mac VPN's Network Extension entitlement; the Windows VPN's Wintun licence and
  elevation (owner's call); a Linux interface; the system proxy on Linux and beyond Windows' LAN
  entry; codes over the relay's TLS port; ICMP and fragments.
- A prompt before an own computer relays (it reveals R's address to the code's maker).
- Bonjour discovery for transfers; IPv6 on the LAN; Bluetooth on Linux.
- Command line: Bluetooth; Remember on macOS; winget; the live site and tap (owner setup).
- Internet: TURN; relaying word codes, remembered sends and handovers; moving a relayed send.
- Deferred from #472: re-gathering on network change; last-known direct addresses; DHT bootstrap
  from peers; a keepalive during waits; S/Kademlia disjoint lookups; UDP port mapping; per-source
  limits on the listener and rejoins; Wi-Fi Direct; AES-256-GCM, larger chunks, parallel streams.
- Resuming protocol 1 transfers, or a protocol 2 session after it ends, beyond what
  [Resuming a receive that stopped](#resuming-a-receive-that-stopped-530) keeps.
- A formal model of protocol 1.
