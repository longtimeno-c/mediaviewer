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
The same engine also runs in a terminal: `driftwire`, a program of its own on Linux, Windows and
macOS (owner, 2026-10-23; [From a terminal](#from-a-terminal-driftwire)).

## What a person sees

- **The window**, "Driftwire", opened from its row in Settings → Add-ons (**Open Driftwire**),
  from its command (the palette; no default key, issue #410), and from the share menu's first
  entry ([16 "Share"](16-commands.md#share-444)): the gallery's share icon, the bar's share button,
  `Ctrl+Shift+S` and right-click Share ▸. Its manifest's own **Send with Driftwire…** context
  entry is left out of the hosts' right-click menus (#444), so there is one route. Its design is
  AirDrop's (#442), the same on both hosts, top to bottom. Every group is a card of its own
  (#508, 2026-10-20), never a card in a card:
  - a card with this computer's name as others see it, and **Keep receiving when this window is closed** (kept
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
    percentage, the count, the speed and the time left ("1.2 GB of 4.8 GB · 14 MB/s · about 4 min
    left", #529, both sides: `eta.h`, shown after 3 s and half a megabyte, hidden while it
    reconnects) and **Cancel**; then what arrived and **OK**. Every end, delivered, cancelled or
    lost, stays on screen with **OK** on both sides until it is dismissed (#531; until 2026-10-21 a
    cancel or a loss cleared itself after 6 s, #511);
  - **Unfinished transfers** (#530), a card while any is kept: from whom, how much is here, and
    **Discard** for each ([Resuming a receive that stopped](#resuming-a-receive-that-stopped-530));
  - **Send**, a card: a strip of the first six files' thumbnails (the viewer's cache), the count
    and size, the first names, **Send a folder** (Choose…), **Clear**;
  - **Nearby**: the MediaViewers it can see as large round tiles (a monogram, the name, "Wi-Fi",
    "Bluetooth" or both). **One click on a tile sends** to it. A card then asks for the code the
    other screen shows, in one large field that takes digits only and goes by itself at the sixth
    (three wrong codes end the link; this screen never shows the code); then "Waiting for … to
    accept"; then the tile's own ring fills while the bytes move (and the path card above shows
    how), and it ends with a done or failed mark on the tile and a line saying what happened, with
    **OK** to go back (#531).
    With no one in sight it says **No one nearby yet** and how to fix that, not a status line;
    what this computer cannot search (no network, Bluetooth off or missing) is one quiet line at
    the card's foot, and nothing when Wi-Fi and Bluetooth both work (#508);
  - **Over the internet**, a card with one toggle, then, each a card, in the order a person uses it: **Receive from
    anywhere** (Make a code; the code in large type, selectable, with **Copy code** and **Copy
    link**), **Send to a code**, Remembered devices, and **Internet options** (the DHT, Wi-Fi drops,
    relaying, and the optional own relay server). Nothing asks for an address: a code finds its
    own (2026-10-19).
- **The gallery** ([16](16-commands.md#gallery-range-and-share-413)): Shift-click selects a range,
  dragging a cell drags it out, and one share icon opens the share menu ([16 "Share"](16-commands.md#share-444)),
  whose first entry is Driftwire with the selection (issue #444: the same menu as `Ctrl+Shift+S`,
  the bar's share button and right-click Share ▸).
  Without Driftwire the icon offers the same one-click install as the other add-ons.

## From a terminal (`driftwire`)

`driftwire` is a terminal host of the same engine (owner, 2026-10-23,
[12](12-decision-log.md#driftwire)): a program of its own, outside MediaViewer, on Linux, Windows
and macOS. It speaks the same wire, so it sends to and receives from the add-on and another
`driftwire`, and it installs on its own, with no MediaViewer: on Linux from signed apt and dnf
repositories (one line, or apt, dnf or zypper), on macOS from a Homebrew tap, on Windows with a
PowerShell one-liner, each of which also updates it; stable releases publish all three
([Installing](#installing), [12](12-decision-log.md#driftwire)). It is built and tested with the
app on Windows and macOS and is never in the app's payload. Its manual page is
`packaging/linux/driftwire.1`.

```
driftwire                        the session: visible nearby while it runs; send and receive
driftwire send PATH... [--to NAME]
                                 the session with these files (or one folder) ready, closing
                                 when that send ends (sent, declined, cancelled or failed);
                                 --to sends to NAME as soon as it is heard (the code is typed)
driftwire receive [--once]       the session for receiving; --once closes after the first offer,
                                 accepted or declined (a knock that offers nothing does not count)
driftwire code                   the session, making a code to receive from anywhere, closing
                                 when the receive it lets in ends, or the code fails or runs out
driftwire devices                the remembered devices (no terminal needed)
driftwire --help | --version     (no terminal needed)
  --save-to DIR, --name NAME     this run only
  --plain, --no-color, --verbose
```

Exit 0 when it ends normally (for `send`, `receive --once` and `code`, the transfer went through);
1 when the transfer of one of those did not go through (declined, cancelled, failed); 2 for a
usage error, no terminal, or a start that fails (another session of the same user). With no
network at all it says so and waits, as the window does: a network can come up.

- **Interactive only** (owner). Nothing is accepted or sent without the person at that terminal
  answering: there is no `--yes` and no auto-accept, and the program refuses to start the session
  when stdin or stdout is not a terminal (`driftwire: this needs a terminal (it asks before
  anything is sent or saved).`, exit 2). Only the test build, `mv_driftwire_cli_test`
  (`MV_DRIFTWIRE_CLI_TEST_HOOKS`, never packaged), reads keys from a pipe, keeps to loopback and
  prints machine-readable lines, for the end-to-end test.
- **No secret on the command line.** No code, check digits or relay key is ever an argument:
  it would sit in the shell's history and in `ps`. `driftwire code 123456` is a usage error; a
  code someone gave you is pasted into `/to-code`, a relay server's key typed into
  `/relay-server`'s field. `--to NAME` names a computer, never a code.
- **The look**, after Claude Code's terminal UI, in Driftwire's own colours: an inline UI, never
  the alternate screen. What finished is printed once, above, as a transcript that stays in the
  scrollback (`⏺` and the engine's own sentence, `⎿` and the size, the time, the bearer and where
  it saved), so the terminal keeps a log after it closes. Only a live region at the bottom is
  redrawn: the card under way (a rounded frame with its title in the border), Nearby, a prompt
  box with a hint line under it, and the palette of `/` commands, filtered as it is typed. One
  accent, teal (Wi-Fi's tint); the bearers in the window's colours (Bluetooth blue, internet
  indigo, the DHT orange, a relay purple). A wait is one shimmering line ("Waiting for Ann's
  MacBook to accept… (12s · esc to cancel)"); a transfer is the window's path picture in
  characters, its dots travelling toward the receiver while bytes move and frozen with
  "reconnecting…", over a gradient bar in eighth blocks and the window's stats line (`eta.h`, the
  bytes and times formatted as `tree.cpp` does). It animates only while something waits or moves
  (about 12 frames a second); idle, its loop blocks. Names are cut by display width with `…`;
  under 40 columns the code is plain digits. `NO_COLOR` or `--no-color` drops colour; `--plain`,
  or `TERM=dumb`, is lines only with no escape sequence at all and questions as lines ("Accept 24
  files (412 MB) from Ann's MacBook? [y/N/s]"), for screen readers and dumb terminals.
- **The code.** The receiver's card shows the six digits in large box-drawing digits, grouped
  "123 456" as the window groups them, to type on the other screen; Esc there declines. The sender
  gets six boxes that take digits only and send by themselves at the sixth, with the engine's note
  and the tries left (three wrong entries end the link, as in the window). Over the internet the
  check is shown to compare ("Make sure Ann's MacBook shows 482 917"), never as a code to type.
  `/code`'s card shows the code large with its link and life; `/copy` copies the code and
  `/copy link` the link (OSC 52, where the terminal allows it): commands, not keys, since any
  letter at the prompt may begin a path.
- **Accept is typed** (owner, [12](12-decision-log.md#driftwire)): the offer is a numbered choice
  under the card, **1** Accept and save to the folder shown, **2** save somewhere else (a folder
  typed, Tab completing), **3** Decline; `y` accepts, `n` or Esc declines. A card that says yes to
  someone else (the offer, the check to compare, Remember) answers only the card drawn, and only
  once keys have stopped for 750 ms with it on screen (`kAnswerSettleMs`), so typing under way at
  the prompt or in a field as it appears ("y" of "~/my-trip") is not an answer; Esc always is.
  Three minutes with no answer, the engine declines, as in the window. Esc twice cancels a transfer under way; Ctrl+C
  twice closes Driftwire (during a transfer it says it will cancel it first).
- **Files** are typed, pasted or dropped (a terminal pastes a dropped file as its path): quotes,
  backslash escapes, `~`, `file://` and the globs `* ? [...]`, expanded by the program, not a
  shell. A folder alone is a folder send. Nearby is the engine's list; ↑↓ choose and Enter on an
  empty prompt sends. The commands are the window's: `/send`, `/folder`, `/files`, `/clear`,
  `/code`, `/copy`, `/to-code`, `/devices`, `/forget N`, `/mine N on|off`, `/unfinished`, `/discard N`,
  `/save-to`, `/internet`, `/dht`, `/handover`, `/relay`, `/relay-server`, `/settings`, `/help`,
  `/quit`. An internet option says what it means the first time it is turned on in a session (the
  window's words).
- **Its own identity and data folder.** Its settings (`driftwire.json`), what it keeps of a
  receive that stopped (`driftwire_resume.json`), the DHT's table (`driftwire_dht.dat`) and the
  session's lock live in a folder of its own, made 0700: `${XDG_STATE_HOME:-~/.local/state}/driftwire` on Linux,
  `~/Library/Application Support/Driftwire` on macOS, `%LOCALAPPDATA%\Driftwire` on Windows; never
  the add-on's data folder. Its identity and remembered devices are under the secret-store service
  "Driftwire CLI" (`secrets::kCliService`), never the app's "MediaViewer Driftwire"
  ([12](12-decision-log.md#driftwire)): the Secret Service on Linux, Credential Manager on Windows.
  On macOS there is none (the data protection keychain needs an access group, which a bare program
  cannot be entitled to), so each run has a key of its own and Remember does not show there.
  `--save-to` and `--name` apply to this run only: they go to the engine as its starting state and
  `default_destination`, never as an event (which would save them), and the kept Save to is put
  back when the session closes, since the engine saves its whole view at any change.
- **The name on the air while the session runs.** Opening the session is opening the window
  (`screen_opened("")`): presence starts and this computer's name is in the beacon (#501) until it
  closes. There is no receiving with the session closed: Keep receiving and Receive when
  MediaViewer is closed do not exist in a terminal (both are forced off at start).
- **One session per user**: `net::try_lock_instance` on `<data folder>/driftwire.json.lock`; a
  second prints "Driftwire is already open in another terminal." and exits 2. The app's lock is in
  its own folder, so MediaViewer and `driftwire` can be open on one computer at once: two devices
  to the others, each with its own identity (not measured).
- **No Bluetooth** on any OS (`use_bluetooth = false`, `bt_none.cpp`): CoreBluetooth needs a
  bundle's usage string, the WinRT GATT server in an unpackaged console is unverified, and Linux
  has no bearer. Wi-Fi and the internet only.
- **Threads.** The session thread owns the engine and the terminal and makes every engine call;
  the host's `post_tree` only counts and wakes it (`cli_host`, the host table's rest: a free-space
  check, a log ring shown with `--verbose`, notices taken, no yielding). `~engine` runs on that
  thread before the terminal is restored, so a receive cut off by closing keeps what arrived (#530)
  and the next session lists it under `/unfinished`. SIGTERM, SIGHUP and the console closing quit
  in order; in raw mode Ctrl+C is a key; Ctrl+Z suspends and draws again on resume. While a
  transfer keeps the machine awake the reason names Driftwire ("Driftwire is receiving files",
  `power::set_owner`).
- **Idle cost**, measured 2026-10-23 in a Linux container (a pty, nothing on screen moving):
  60 s idle took 10 ms of CPU (0.017 %) and 6.0 context switches a second, and wrote 0 bytes to the
  terminal; the engine's own `[.driftwire-idle]` "on" phase is 7.4–8.2 ms in 20 s at 6.0–6.1 a
  second. Nothing above the engine's own cost could be measured.
- **The Linux packages** (`cmake/driftwire-cli`, [12](12-decision-log.md#platforms)): one program,
  `/usr/bin/driftwire`, that needs only glibc (2.35 or newer, the build machine's): libsodium 1.0.22
  (a distro's 1.0.18 has no ML-KEM-768), BLAKE3, FTXUI and the C++ runtime are linked in, and the
  build fails if anything else is `NEEDED`. libsecret and libsystemd are opened at run time when
  they are there; the `.deb` recommends `libsecret-1-0`, the `.rpm` `libsecret`. With it: the manual
  page, bash and zsh completions, a firewalld service for the beacon (`driftwire`, 47613/udp; the
  manual page says what ufw needs, and that receiving nearby also needs incoming TCP from the local
  network, on a port the system picks) and the DEP-5 copyright file with every notice.

### Installing

The owner asked (2026-10-24) to install `driftwire` on its own, as `sudo apt install` does, with
no MediaViewer present, and then for the macOS and Windows command line, choosing a Homebrew tap
and a PowerShell one-liner ([12](12-decision-log.md#driftwire)). It needs no MediaViewer: it has
its own identity and data folder, and sends to and receives from MediaViewer's add-on or another
`driftwire`. The Linux and Windows installers come from a site on this repository's GitHub Pages,
`https://longtimeno-c.github.io/mediaviewer/driftwire/`, and the Mac's from the tap
`longtimeno-c/homebrew-tap`; until the owner has set them up
([DEVELOPMENT](../DEVELOPMENT.md#publishing-the-command-line)) neither serves anything.

```
Linux    curl -fsSL https://longtimeno-c.github.io/mediaviewer/driftwire/install.sh | sh
macOS    brew install longtimeno-c/tap/driftwire
Windows  irm https://longtimeno-c.github.io/mediaviewer/driftwire/install.ps1 | iex
```

| | What it writes | Updates | Removing it |
|---|---|---|---|
| Linux | the package, through apt, dnf, yum or zypper, with the repository's source and key; with none of them, the tarball's files in `~/.local` (`/usr/local` as root) | the package manager (`apt upgrade`, `dnf upgrade`, `zypper update`); the tarball: the line again | `curl -fsSL <site>/install.sh \| sh -s -- --uninstall` |
| macOS | Homebrew's copy, `$(brew --prefix)/bin/driftwire` and its manual page | `brew upgrade` | `brew uninstall driftwire` |
| Windows | `%LOCALAPPDATA%\Programs\Driftwire` (`driftwire.exe`, the DLLs it loads, the notices) and that folder on the user's `Path` | the line again | `& ([scriptblock]::Create((irm <site>/install.ps1))) -Uninstall` |

None of them touches the data folder; none needs an administrator on Windows or macOS.

- **Linux: the one line** (`packaging/linux/repo/install.sh`, POSIX sh). It checks Linux, x86_64 or
  arm64, glibc 2.35 or newer (musl is refused, with Alpine named) and curl or wget, says what it is
  about to do, and prints each command as it runs it, through sudo when it is not root. On apt
  systems (Debian, Ubuntu and their derivatives) it saves the key as
  `/etc/apt/keyrings/driftwire.gpg`, adds `/etc/apt/sources.list.d/driftwire.sources` (deb822:
  `URIs: <site>/apt`, `Suites: stable`, `Components: main`, the machine's architecture,
  `Signed-By:` that key), runs `apt-get update` for that source alone and installs. With dnf or yum
  (Fedora; a RHEL-like system from 10, since 9 has glibc 2.34) it imports the key into rpm, writes
  `/etc/yum.repos.d/driftwire.repo` and installs or upgrades; with zypper (openSUSE) it adds the
  same file as the repository `driftwire`. With none of them it unpacks the tarball into
  `~/.local`, or `/usr/local` as root, after checking it against `SHA256SUMS` (and that file's
  signature when `gpgv` is there), and records what it put where; that copy does not update
  itself, and the installer says so. A source the package manager cannot read or verify is taken
  away again, with what apt fetched of it (a refused `InRelease` stays in
  `/var/lib/apt/lists/partial/` otherwise). Running it again upgrades. `--uninstall` removes the
  package, the source, the key and apt's files of that source (`partial/` too), or the tarball's
  files and the folders it made, and nothing else. Everything is in functions and `main` runs on
  the last line, so a download cut off part way runs nothing. The site's address, key fingerprints,
  Homebrew tap and release page are written in when the site is built; `DRIFTWIRE_REPO_URL`
  overrides the address (https only, or http on 127.0.0.1 or localhost for the tests). On macOS it
  installs nothing: it prints the `brew install` line (and, without Homebrew, brew.sh and the
  release page) and exits 1.
- **Linux by hand.** The site's page has the same as commands: the keyring and the deb822 source,
  then `sudo apt install driftwire`; `driftwire.repo` into `/etc/yum.repos.d/` and `sudo dnf install
  driftwire` (dnf asks once to trust the key; the page shows its fingerprint); `sudo zypper
  addrepo --refresh <site>/driftwire.repo`; or the tarball and its signed checksums.
- **macOS: the Homebrew tap.** `brew install longtimeno-c/tap/driftwire` reads
  `Formula/driftwire.rb` in the public repository `longtimeno-c/homebrew-tap`, which release.yml's
  `driftwire-homebrew` job rewrites for each stable release (`tools/package/homebrew_formula.py`
  from `packaging/macos/driftwire.rb.in`): a binary formula whose `url` is the release asset
  `driftwire-<version>-macos-universal.tar.gz` and whose `sha256` is that file's as published;
  macOS 14 or later; the program into `bin`, its manual page into `man1`; a caveat on the
  firewall's question and on Driftwire being open on the other side; a test of `--version`. The
  archive (`tools/mac/macpack.py cli`) is one folder holding the program (the arm64 and x86_64
  builds joined by `lipo_merge.py`), `driftwire.1`, `LICENSE`, `NOTICE` and `COPYRIGHT.txt`. The
  program is signed with the Developer ID identity, the hardened runtime, a secure timestamp and
  the identifier `io.github.longtimeno-c.driftwire`, with no entitlements (it keeps nothing in the
  keychain: Remember does not show on macOS, as above), and notarized in a zip. A bare program
  cannot be stapled, so Gatekeeper looks the ticket up online the first time a quarantined copy
  runs; Homebrew does not quarantine what a formula installs. When the release's Intel leg did not
  build, the archive is `-macos-arm64.tar.gz` and the formula adds `depends_on arch: :arm64`, so an
  Intel Mac is told instead of given a program it cannot run.
- **Windows: the one-liner** (`packaging/windows/install.ps1`). Windows PowerShell 5.1 and
  PowerShell 7; ASCII; one function called on the last line, so a cut-off download runs nothing
  and only that function stays in the session. It calls only binary cmdlets and .NET: Windows
  PowerShell 5.1's `Get-FileHash` and `Expand-Archive` live in script modules, which a Windows
  client's default execution policy (Restricted) will not load, though it runs `irm | iex`. It
  checks 64-bit Windows 10 or later (x64: Windows 11 on Arm runs it too; Windows 10 on Arm, which
  emulates 32-bit x86 only, and 32-bit Windows are refused before anything changes), downloads
  `driftwire-<version>-windows-x64.zip` from the site to a temporary file, checks it against the
  SHA-256 written into the script and, when the release was signed, that every `.exe` and `.dll`
  in it has a valid Authenticode signature, all before anything changes. It unpacks next to
  `%LOCALAPPDATA%\Programs\Driftwire`, moves the old copy aside (a running `driftwire` stops that,
  and it says to close it), the new one in and the old one away, then adds the folder to the
  user's `Path` once. It writes `Path` through the registry so the value keeps its kind and its
  `%VARIABLES%` (`SetEnvironmentVariable` would write it back expanded, as `REG_SZ`), lets
  Explorer know so new terminals have it, puts it on this session's `PATH`, and prints
  `driftwire --version`. `-Uninstall` removes the folder and the `Path` entry. Installing and
  `-Uninstall` also take away an earlier run's leftovers beside the folder (an old copy that was
  still running, a download cut off while unpacking), matched by the names it gives them:
  `Driftwire.old-` or `.driftwire-` and eight hex digits; any other folder there stays.
  A folder of that name without `driftwire.exe` in it is not its own and is left alone. The zip
  comes from the site, not the release page, so the installer and the file it checks go live
  together and a copy of the site served elsewhere is whole; `DRIFTWIRE_REPO_URL` or `-BaseUrl`
  point it at one (the written SHA-256 must still match). Off Windows it refuses, naming the brew
  and curl lines; with `DRIFTWIRE_INSTALL_TEST=1` it runs there for the tests, with the user's
  `Path` in a file and no registry or Authenticode. winget may come later.
- **The Windows zip** (`tools/package/driftwire-windows.ps1`): `driftwire.exe` and every DLL it
  loads, found by reading the import and delay-load tables of the program and of each DLL it finds:
  ours from the build (`libsodium.dll`, `blake3.dll`; x64-windows is a dynamic triplet, FTXUI's
  port static only) and the Visual C++ runtime (`vcruntime140.dll`, `vcruntime140_1.dll`,
  `msvcp140.dll`, whichever it imports) from Visual Studio's redistributable folder, never
  System32: the build uses the dynamic C runtime, a per-user install cannot run `vc_redist`, and a
  clean Windows lacks it. The Universal CRT and the API sets are Windows'; any other DLL fails the
  build. Release signing signs ours with Trusted Signing when it is configured (optional, as for
  the installer; without it the zip is unsigned); Microsoft's runtime keeps Microsoft's
  signature. With `LICENSE`, `NOTICE` and `COPYRIGHT.txt`.
- **Updates** come through each channel: the package manager on Linux (this is the updater a build
  needs before it leaves this machine, CLAUDE.md), `brew upgrade` on macOS, the one line again on
  Windows (it installs the site's version over what is there). The Linux packages are not attached
  to the release page (`MV_RELEASE_LINUX_CLI` stays off): a `.deb` from there would install without
  the source that updates it. The macOS archive and the Windows zip are, since the tap and the
  site take them from the release.
- **One switch.** The repository variable `MV_DRIFTWIRE_REPO=1` publishes the command line from
  each stable release: publish then requires and uploads the macOS archive and the Windows zip
  (`tools/package/github-release.py`), `driftwire-repo` deploys the site (the apt and dnf
  repositories, `install.sh`, the Windows zip and `install.ps1`), and `driftwire-homebrew` pushes
  the formula. The preflight refuses to start a stable release with it on and the repository key
  or the tap's token missing. Previews leave all three alone, and a release with it off is the
  installers alone, as before.
- **The repository** (`tools/package/driftwire_repo.py`) is a static tree built by release.yml's
  `driftwire-repo` job from the release's packages and the Windows zip as published, after the
  release is published. Each run replaces the Pages site, so it holds the newest version only.
  apt: the pool, `Packages` and `Packages.gz` for each architecture present, a `Release` (Origin
  and Label `Driftwire`, Suite and Codename `stable`, component `main`) with `InRelease` and
  `Release.gpg`. rpm: the packages signed with `rpmsign`, `createrepo_c` metadata (gzip and no
  sqlite, which older dnf and zypper read) with `repomd.xml.asc`, and a `.repo` with `gpgcheck=1`
  and `repo_gpgcheck=1`. The tarballs and the Windows zip with `SHA256SUMS` and `SHA256SUMS.asc`,
  the public key as `key.asc` and `key.gpg`, the page (Linux, macOS and Windows), and both
  installers; `install.ps1` gets the zip's version, name and SHA-256, and whether `driftwire.exe`
  carries an Authenticode signature (read from the file; a release with Trusted Signing configured
  refuses an unsigned one). No external asset or script on the page.
- **Valid-Until a year ahead.** apt refuses a `Release` past it, so a mirror or a network in the
  way cannot keep serving an old, correctly signed index to hold back a fix for longer than that.
  The cost: a stable release must come within the year, or `apt update` on every installed
  machine fails ("Release file … is expired", exit 100, checked here) and takes nothing new from
  the repository until the next release re-signs it.
- **Nothing unsigned goes out.** The builder refuses packages whose versions disagree, an
  architecture without all three packages, anything that is not driftwire, a tarball or zip path
  outside its folder, a Windows `driftwire.exe` that is not x64, and a base URL that is not https.
  It signs with the key named (two during a key change: every index then carries both signatures
  and `key.asc` both keys; an `.rpm` carries the first), checks every signature against the
  exported key with `gpgv` and `rpmkeys`, checks that `install.ps1` has the zip's SHA-256 and that
  no private key material is in the tree, and only then replaces the site; a failure leaves the old
  one. The release job then serves the result on localhost and installs from it before deploying
  (`driftwire_repo.py smoke`): apt installs, refuses an `InRelease` changed by one byte, and
  `--uninstall` leaves nothing, `partial/` included; then the tarball route with no package manager.
- **The keys** are the owner's: the repository's OpenPGP key, made once, its private half in the
  Actions secret `DRIFTWIRE_REPO_GPG_KEY` with an offline backup, the passphrase (if any) in
  `DRIFTWIRE_REPO_GPG_PASSPHRASE`; apt keeps the key the installer saved, so a new key reaches an
  apt machine only when the installer runs again (or the key is fetched by hand), and a key change
  overlaps the two keys across at least one release. The tap is written with `HOMEBREW_TAP_TOKEN`,
  a fine-grained token with Contents: write on the tap repository alone. The Apple credentials the
  release already uses sign and notarize the Mac program. The Windows programs are signed only
  when the release's optional Trusted Signing credentials are set, as the installer is; without
  them `driftwire.exe` ships unsigned and `install.ps1` checks the zip's SHA-256 alone.
- Run here only (2026-10-24, Linux): the Linux repository in an Ubuntu 24.04 container (apt 2.8,
  dnf 4.14 with repo_gpgcheck, rpm 4.18: install, upgrade by apt and by the installer again,
  refusal of a changed `InRelease` and of a changed `repomd.xml`, uninstall with `partial/`
  cleared, the tarball route as root and as a user); `install.ps1` in its test mode under
  PowerShell 7.6 (`tools/package/check-driftwire-install.ps1`: install, again, an update to a newer
  version and back, a changed zip refused, a cut-off script doing nothing, uninstall keeping the
  other `Path` entries, only its own leftovers removed, Windows 10 on Arm refused, only binary
  cmdlets called); the formula under Ruby 3.3 against a stand-in for Homebrew's `Formula`;
  `macpack.py cli` and `driftwire-windows.ps1` on tiny stand-in programs. CI's Windows leg runs
  the one-liner for real under the Restricted execution policy (installs in both PowerShells,
  signed and unsigned, and uninstalls in both; the re-run, the update and a changed zip in
  PowerShell 7), and has not run yet; nor have
  the release's macOS and Windows jobs, Homebrew, the tap or Pages, nor a real Fedora, openSUSE or
  arm64 machine.

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
a person can see this machine is discoverable after a relaunch. Clicking it opens the Driftwire
window, or brings it forward (#512: Driftwire's one screen, [25](25-open-addons.md)). Off at load: the read, and nothing
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

- A **beacon** is `MVSP1` and `{"v","p","id","name","port"}`: a random session id (16 hex digits),
  the machine's name (the Sharing name on the Mac, the computer name on Windows) **only while the
  window is open**, a port, and `"p"`, the highest protocol it speaks (#472; `"v"` stays 1, which is
  what a protocol 1 client checks, so old and new still see each other; no `"p"` is protocol 1).
  Bluetooth's INFO carries the same id, name and `"p"`. Never a path, a name of a file or anything
  about one. A peer's `"p"` is never lowered while it is listed (#497): anyone on the network can
  send a beacon in its name, and one with no `"p"` would talk a send down to protocol 1. The name a
  send shows and remembers is the one the peer gives inside the sealed channel (`ident`), not the
  beacon's.
- **The id and the name change (#501).** With the window closed (Keep receiving, or Receive when
  MediaViewer is closed) the name field says `MediaViewer`, which every version reads, and the id is
  replaced every 10 minutes (`kIdRotateMs`) while nothing is under way; it is replaced too whenever
  presence starts and whenever the window opens or closes, so the named and unnamed ids are not one
  machine. With the window open the id holds, so a tile does not change under the pointer. Nothing
  changes while a transfer, a handshake or a wait for a peer runs (the peer knows this side by the
  id `ident` gave); the change waits for the end. A sender learns a background receiver's name from
  `ident`, sealed, once it connects (and the receiver posts its notification). A protocol 1 send waiting to hear its peer on Wi-Fi follows the id, so a
  receiver that changes its id meanwhile is lost to that wait.
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
  `driftwire.json.lock` keeps itself; another window's process quits as usual. Hidden, a staged
  app update installs once no transfer is under way and the process comes back in the
  background; "Update ready — restart" clicked during a transfer waits for it to end, and a
  copy asked to quit for another's update finishes its transfer first (#532,
  [13](13-updates-and-telemetry.md)).
- **A notification when someone sends** (both hosts, background or not). The engine posts a
  notice (`notice_post`) the moment a sender's link is up, while its code shows: "Ann's MacBook
  wants to send you files. Open Driftwire to see the code." Once the sender has typed the code
  and the offer arrives: "… wants to send you 24 files, 412 MB. Open Driftwire to accept." It
  never shows the code or a file name (rule 6: a lock screen shows it), only the sender's name, the
  count and the size. **Clicking it opens the Driftwire window** with the card and its code;
  its one button is **Decline** (the window's own Decline), tagged with the offer it is about, so a
  notification left from an earlier offer declines nothing newer (#502). Accept is only in the
  window, after the code: nothing is accepted from a notification. It is taken back however the
  offer ends.
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

**Protocol 2 (#472) and 3 (#497)**, `Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b`. Protocol 3 is
protocol 2 with the server's commitment (steps 2 and 4, and its own prologue, so the version is
bound into the handshake); a peer that announces 2 (v0.1.34, v0.1.35) is spoken to in 2, a
receiver answers either, and rejoins and internet hellos are the same in both:

1. The client (the side that connected, the sender) sends `MVSP`, 2, `N` and the first Noise
   message: an X25519 key and an **ML-KEM-768** key made for this link (the "hfs" modifier), and as
   its payload a BLAKE2b commitment to a random 32-byte nonce.
2. The server (the receiver) answers: its X25519 key, `ee`, the ML-KEM ciphertext encapsulated to
   the client's key (its shared secret mixed into the chaining key beside the X25519 ones), its
   static key and `es`, and (protocol 3) as its payload its own commitment to a random 32-byte
   nonce. A
   recording of the link needs both X25519 and ML-KEM-768 broken to read.
3. The client sends its static key, `se`, and reveals the nonce, which must match the commitment.
   **Both statics are keys made for this link alone (#501).** Anyone in range can connect and read
   message 2, and a sender sends message 3 to whatever answers, before any code is typed, so the
   machine's long-term identity ([below](#remembered-devices-472-pr-4)) is never a nearby static.
   It goes sealed instead, once the link is authenticated: the sender's in its **offer** (`"key"`,
   sent only after its person typed the right code, so only to the receiver that code names), the
   receiver's in its **accept** (only after its person accepted). Remember keeps those. A handshake
   behind CPace (an internet code, a remembered device's meeting) carries the identity as its
   static, where the CPace key hides it from anyone without the code or the pairing secret and the
   meeting checks it.
4. Protocol 3: the server's first sealed message (`code_nonce`, unnumbered, like `resume_at`)
   reveals its nonce, which must match its commitment.
5. **The code** is six digits from a BLAKE2b hash of the handshake hash after message 2 and both
   nonces (protocol 2: the client's only). Each side's part is fixed before it sees the other's, and the server reveals last, so
   neither side knows the code before the other has finished its part: a machine in the middle can
   neither search for keys that give both people the same digits nor learn the server's code and
   drop the link before the server shows it (#497; before it, a client learned the server's code
   from message 2 and could reconnect until one matched, unseen). Only the receiver's window shows
   it.
6. Both sides open an XChaCha20-Poly1305 secretstream each way keyed from the handshake's Split, and
   keep Split's third output as the session's **resume secret** (memory only). Ephemerals are wiped
   (`sodium_memzero`) as the handshake ends. Every later message is sealed with a sequence number
   and an acknowledgement ([The session](#the-session-protocol-2-472)).

The Noise core is checked against the published Cacophony vectors (`Noise_XX` and `Noise_NNpsk0`
over 25519, ChaChaPoly and BLAKE2b); the hybrid patterns have fixed-key regression vectors and
tests that the ML-KEM secret is in the keys and a tampered key or ciphertext fails
(`tests/test_driftwire_noise.cpp`). Those are our own: no other implementation of the `hfs`
patterns with ML-KEM-768 publishes vectors, so the hybrid half is checked against Cacophony only
where it shares code with plain XX, and against itself (#501 names the gap; it closes when one
does). libsodium 1.0.22 (already pinned) has ML-KEM-768.

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
  the client's nonce is secret until its h2 is fixed. On the handshake's skeleton with the commitment check removed, ProVerif finds that
  attack (the full first link without the check does not finish).
- **Broken later.** A first link recorded now stays secret if X25519 is broken later (every
  ephemeral and static handed over), or ML-KEM-768 is; with both broken it is read (the check that
  the model can lose).
- **Over the internet** (#501, `internet.pv`): CPace on the code (or a remembered pair's hourly
  password), then `XXhfs+psk0` keyed by it. The password resists offline guessing from any number
  of recorded and active runs (`weaksecret`: a 48-bit code or a word code is not given away by a
  transcript; online, one try per code, which then dies). Neither side's identity, the statics
  here, reaches anyone without the password, and what is sent after the handshake stays secret. A
  sender that finished, its pin checked, met a receiver holding the identity it pinned that answered
  its message 1, one for one; a receiver that finished shares its keys with a sender that saw its
  identity (asked without "one for one", which does not finish; the keys hold a fresh ephemeral of
  each run, so no two runs share them). About 9 minutes.
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
it use one. The statics are modelled as fixed keys; since #501 a nearby static is a key of that link,
which only shrinks what a recording or a stolen key gives. The identity the sender says in its offer
is part of what goes after the code (`OFFER` in the model), so it is secret against a machine in the
middle; the receiver's in its accept goes to whoever its person accepted, which no code can prove
(see "Who can take what"). CPace's generator is an abstract group element with the Diffie-Hellman
equation, and the internet handshake's ciphertexts carry a constant associated data too.


**Tries, not people** (#497). The symbolic model cannot count: an attacker in the middle can run
the server's half many times, and each run is a try at matching the code its victim will type.
Before #497 the client learned the server's code from message 2 and could drop the link there,
unseen and unlimited (about 10^5 tries in the three minutes a person has to type). Now the server's
nonce comes after the client's, so a client learns the code only once the server has finished and
shows it; and a nearby link that ends with no offer counts toward a limit: five in a minute, and
new nearby links are not answered for 30 s (`note_unconfirmed`). So a machine in the middle gets
about ten tries in those three minutes, 10^-5, and the receiver's window shows each as a code
nobody used. Protocols 1 and 2 have no server nonce (their code is fixed by the server's answer),
so to a peer from before #497 the limit is all that holds; a receiver still answers them, so an
attacker can choose 2, and the limit is what bounds it. The model's skeleton is unchanged; the server's commitment adds
a check it does not need. Not modelled: protocol 1, `ident` (sent sealed before the code, so a machine in the
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
6. `cancel` from either side ends it (`channel::cancel`, [The session](#the-session-protocol-2-472)
   has the details): it goes out after the chunk being written, waiting at most 250 ms
   (`kCancelWaitMs`) in all, then the link closes. A receiver that stops on an error sends it with
   `{"why":"error"}`, so the sender does not say the person there cancelled.
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
  there), and the other side sends again, in order, what is past that point. Its sequence field
  (0 before #502, ignored by older peers) says which rejoin this is to that side, and both take
  the larger: a rejoin one side adopted and the other did not (an on-path cut between the two
  `resume_at`s) cannot leave the DHT handover's keys, which mix in the rejoin count, out of step.
  A message that came
  twice (sent on the old link, and again after the rejoin) is dropped by its sequence number.
  Messages are chunk-sized, so this is the issue's "resume at a chunk boundary": the receiver
  keeps its open `.mvtmp`, its BLAKE3 state and its verifier exactly as they were, and nothing
  that arrived is sent again. Every file is still verified end to end.
- **When it moves** (the sender's mover thread, `engine::run_mover`):
  - **Break.** A link that fails, or that is silent for **3 s** (`kSuspectMs`), is suspect: every
    bearer the peer is heard on is raced (Wi-Fi to its fresh beacon, Bluetooth to its handle).
    **The race is of whole rejoins, not connects** (#502, `channel::begin_rejoin`): each bearer
    connects and sends the rejoin's first message, and wins only with the answer, proof the other
    end holds the session's secret, within **3 s** of connecting. Only the winner goes on to
    `resume_at` and moves the session; the others close before anything is adopted on either side.
    A machine that beacons the peer's id and connects at once (when the peer's Wi-Fi is stale, or
    it is on Bluetooth only) cannot win or stall the rescue, and a Wi-Fi place first heard during
    the session is used only once it has answered so (owner, 2026-10-20). The window
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
- **A cancel is final** (#511). `channel::cancel` marks the session cancelled first, then writes
  `cancel` as soon as the frame being written (if any) is done (the send lock is a timed mutex),
  waits for the peer to acknowledge it so it is read before the link closes, and shuts down: 250 ms
  at most in all. Nothing is sent after it. The side that reads `cancel` marks its session
  cancelled too. From then on, on either side, a link that drops is not a loss: no
  `link_event::down`, no suspect, no "Reconnecting", no mover race, no wait for `kRejoinMs`, and
  a rejoin of the session is refused. The window then says who cancelled: "Cancelled. 2 of 5
  files arrived whole on Bea." on the side that cancelled, "Ann cancelled. 2 files arrived whole;
  nothing partial was kept." on the other ("You cancelled. …" and "Bea cancelled. …" when the
  receiver did). A link lost with no cancel is unchanged: "Reconnecting to …", then "The
  connection to … was lost". If the frame being written cannot finish within 250 ms (a slow
  Bluetooth link), the cancel is not said, and the other side sees a lost link.
  On Windows a screen window is rebuilt whenever its tree moves, ten times a second during a
  transfer, and a rebuild between a press and its release lost the click, so Cancel there could
  do nothing. `IslandHost.Screens.cs` now holds a window's rebuild while a pointer is down in it
  and catches up on release ([25](25-open-addons.md)).
- Not here yet: the slower too-slow rule between two fast bearers (over the internet a send
  moves back to Wi-Fi when the peer is heard there again, as Bluetooth does).

**Who can take what.** Bytes move only from a sender to the peer its person picked, after that
person typed the code the peer's screen showed and the person at that peer accepted. A sender never answers a request for
files, so a third machine that hears the beacon learns a port (and a name, while the window is open),
and nothing else. A
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
  with IPv6). **UPnP talks to the gateway only** (#499): only an SSDP answer from the default
  gateway is read, and the description's LOCATION, URLBase and every control URL must name the
  gateway itself (never loopback, 0/8, link-local, multicast or a public address); a service type
  must be `urn:` and `[A-Za-z0-9.:-]` only, since it goes into the `SOAPAction` header (a CR/LF
  there was a second, attacker-written request to whatever the control URL named). A port the
  router already forwards (`GetSpecificPortMappingEntry`), unless to this machine by an earlier
  run of this app, is not taken over, so the person's own 443 forward survives; a 443 mapping the
  router put on another port is removed at once. When PCP mapped the port, UPnP is not searched
  for an IPv6 pinhole besides.
- **Leases are 120 s** (`kMapLifetimeS`, #499; was an hour), renewed at half their life while the
  code or transfer holds them (`mapping::keeper`: PCP and NAT-PMP ask again with the same nonce
  and ports, UPnP repeats `AddPortMapping`, a pinhole `UpdatePinhole`; one the router refuses is
  asked again within 10 s). A crash or force-quit leaves a forward or pinhole open two minutes at
  most, and a transfer longer than an hour keeps its way back in. While held they are listed in
  a ledger beside the prefs (`driftwire_mappings.dat`, the handover's way in
  `driftwire_way_in.dat`; deleted once removed): the next launch's first code removes what a crash
  left, on the same gateway only, a UPnP forward only if the router still names this machine and
  this app (`mapping::clean_leftovers`).
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
  here. **#499:** only public places that are not this machine's own are tried (never loopback,
  multicast or 0.0.0.0); the receiver's address on its own network only after this network's
  router (NAT-PMP or UPnP) says it faces the internet from the code's public address, so a host
  at that private address on the sender's network cannot answer for the receiver.
- **The code is spent only by its own ways in** (#499): its listener, its relay and its word
  meeting. A code's hello over the Wi-Fi → internet way in (or any other internet link) is closed
  without spending the code shown.
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
  them into an Ed25519 key, a key for the items' contents, a salt base and CPace's password. Its
  salt holds the hour (UTC) the code was made in (#499: with a fixed salt one table of all 2^44
  codes, a one-off cost, broke every code; now such a table serves one hour). The receiver's keys
  are its hour's; a sender whose code may have been made last hour (the first ten minutes of an
  hour) works out both and meets under the one the receiver's item is found under.
- **Each side publishes a BEP 44 mutable item** under four salts of its role (receiver, sender):
  its addresses (on this network, its public address as the DHT saw it, its public IPv6, all at
  its one UDP port), XChaCha20-Poly1305-sealed, padded to 512 bytes, the timestamp inside. The
  target is SHA-1(key ‖ salt): without the words, 20 random bytes nobody can find, read or forge.
  Each side looks up the other's four targets in parallel; an item is taken only if its key
  hashes to the target, its signature verifies, it opens under the code and it is under ten
  minutes old. After the link is up each side overwrites its items with "gone", and a sender of
  words does so too when its meeting or handshake fails (#499).
- **The DHT client** (`dht.h`) is the spike's "small KRPC + BEP 44/42 client in the add-on", not
  libtorrent (Boost, a torrent engine, sockets of its own): read-only (BEP 43, it answers no
  queries and stays out of others' tables), iterative lookups (three in flight, the eight closest),
  BEP 42 votes for this machine's public address from every answer (no STUN), nodes whose id
  matches their address preferred, a routing table kept between runs (`driftwire_dht.dat` beside
  the prefs, up to 200 nodes, all of which load again), and the four routers of #472 to start from
  the first time. **#499:** one vote per voter's network (an IPv4 /24, an IPv6 /48), and with at
  least four BEP 42-secure voters theirs alone, so one host on many ports or addresses cannot set
  this side's public address (which its item publishes) or call its NAT hard (256 sockets). Only
  public nodes are learned, asked or kept: a node list naming this network, loopback or multicast
  no longer makes this machine send there, nor survives in the saved table.
- **One UDP socket** (`udp_hub.h`) carries the DHT, the punching and the transfer: the hole a probe
  punches is the one the transfer uses. **Punching**: each side probes every address the other
  gave, and 16 ports above the other's public one (a NAT that hands ports out in sequence), every
  100 ms for up to 10 s. Its addresses on its own network (RFC 1918) are probed only when both
  sides' public addresses are the same (behind one router, #499): elsewhere such an address is a
  stranger's on this side's network. A hello from a path the item did not name (a NAT's fresh
  mapping) is answered and probed back at once. The sender takes the first path a reply came back
  on and offers it (*choose*) until the receiver answers (*chosen*); the receiver answers only the
  first path it is offered, so both end on the same one, whichever transport it is on.
- **Probes are authenticated** (#499). Each carries a tag from the code (a quick filter, shown in
  the clear) and this side's random 16-byte nonce for the meeting; a hello adds a 16-byte MAC
  over (kind, nonce), and a reply, choose and chosen add the nonce of the side they answer and a
  MAC over (kind, nonce, that nonce), keyed BLAKE2b under a key from the code (`prober`,
  `meet.cpp`). A probe that does not verify is dropped before anything else, so a stranger who saw
  the tag (a host at the other side's private address on this network, a carrier-NAT neighbour)
  cannot answer a hello, fill the 8 learned-path slots, pick the path or take it, and an answer
  recorded in one meeting fails in the next (new nonces). The stream's id comes from both nonces
  under the same key, not from the tag. Hellos are 46 bytes, the rest 63; v0.1.35's 14-byte
  tag-only probes are ignored, so **this side cannot meet a v0.1.35 peer over the DHT** (words or
  a remembered device; direct codes and Wi-Fi are unchanged).
- **Nothing is kept before the handshake** (#499, `meet_options::open` / `later`). A choose and
  its chosen carry an attempt number. The sender runs the internet handshake (CPace, Noise) over
  the path agreed on before it keeps it; when that fails on a path that did not carry it, it
  agrees on the next path a reply came back on under the next attempt (fresh hellos meanwhile,
  8 s at most to agree), up to three, and the receiver, whose meeting has already handed on the
  first link, hands each later pick on the same way for a minute (`later`, on the hub's reader
  thread). The stream id holds the attempt. Wrong words, or a key that is not the one
  remembered, end it at once: no other path mends those. A word code is still spent by the first
  hello that reaches the receiver, so a later pick after a handshake that got that far fails
  too; the fallback mends paths that answered probes but did not carry the stream.
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
  open) and in a remembered device's once a sender has knocked, never while it waits, since it
  listens on every interface (Windows asks its firewall at the first). The sender picks a TCP
  connection only if no UDP path came back in the first 1.5 s, and the same choose / chosen
  settles it. On each connection both sides first write a hello (an answer to it within 2 s or the
  connection is dropped), and the sender offers its choose only on a connection whose hello
  verifies (#499). Once it has offered a TCP connection it waits for the verified chosen there
  until the punch ends; a connection that closes or answers with anything else was not taken by
  the other side (TCP loses nothing), so the sender falls back to the next path that answers. The
  two sides cannot end on different transports. It meets where a NAT keeps a TCP
  connection's port (most home routers), whatever it does to UDP: two hard UDP NATs can still
  meet this way. On Windows the port is shared with `SO_REUSEADDR` (there is no `SO_REUSEPORT`),
  not `SO_EXCLUSIVEADDRUSE`. Each OS shares a bound port only within one user account (Windows'
  socket security, the BSD and Linux uid check), so only a process of the same user, which can
  read Driftwire's keys anyway, could bind it and take a connection. It gains nothing (#499):
  its hello fails the code's MAC and the sender passes it over, and CPace would refuse it after. A connection closed after its choose leaves the
  4-tuple in TIME_WAIT on this side, which blocks a retry to the same peer from the same port:
  the sender does not retry that way.
- **The stream** over it is the spike's third option, "our own simple UDP transport", not QUIC: it
  must share the socket on both OSes (msquic owns its sockets and has no macOS; ngtcp2 needs TLS
  beside Noise), and the session above already seals, orders and resumes. 1200-byte packets,
  cumulative and 256-packet selective acknowledgements, RACK loss detection and a tail-loss probe
  (RFC 8985, simplified), the receiver's buffer as the window. Since #529 (2026-10-21) the sender
  follows a model of the path, BBR v1 simplified (draft-cardwell-iccrg-bbr-congestion-control),
  rather than NewReno: it paces at the bandwidth it measures (per-packet delivery-rate samples,
  draft-cheng-iccrg-delivery-rate-estimation; a max filter over 10 round trips), keeps two
  bandwidth-delay products in flight, starts up at 2.885 × until three round trips stop growing
  the rate by a quarter, drains, then cycles its pace 1.25 / 0.75 / 1 × six, and probes the
  smallest RTT again after 10 s without one. Two pieces come from BBR v2: startup also ends on a
  round that loses more than 2 %, and such a round caps the packets in flight near what it held
  (raised 2 % a clean round), so a router queue shallower than one bandwidth-delay product is not
  overrun round after round. Lost packets are marked and sent again as the pace and window allow,
  never in one burst; a sample the sender itself limited (a paused writer, a repair, a pipe still
  refilling) may raise the estimate, never lower it; the RTO floor (200 ms) sits under the
  variance, not the sum, so a steady long path does not time out on a little queue. Past the
  256-packet bitmap an ack adds up to 16 runs `[start, end)` of packets held (`kMaxRuns`),
  appended after the fixed part, so an older peer reads what it always did and ignores them; the
  sender visits only packets still in flight for them, each visit forgetting one.
  The stream exists before CPace, so its peer is not yet trusted (#499): a data packet over
  1200 bytes is dropped, what waits to be read and what is held out of order together stay within
  the 16 MiB window it advertises (`kStreamMaxRx`; the advertised window subtracts both, so an
  honest sender never hits the bound), an ack of a packet never sent is ignored, and an ack visits
  only the 256 packets its bitmap covers (and the runs past it only touch packets in flight).
  Loopback, Mac (`"[driftwire]"`'s stream case, before #529): about 60 MB/s clean, 17–26 MB/s at
  1 % loss each way, 6–10 MB/s at 5 %.
  **Emulated paths** (#529, `mv_import_tests "[.driftwire-stream-bench]"`, `tests/driftwire_path.h`:
  a bottleneck with a drop-tail queue, delay, in-order jitter and random loss behind the hub's
  `socket_wrap`), base and new alternated three times, 15 s each, Linux container (4 vCPU),
  RelWithDebInfo (`docs/perf/driftwire-stream.jsonl`):

  | Profile (RTT, bottleneck, queue, loss) | Before | After | Share of the line, before → after |
  |---|---|---|---|
  | lan-ish (4 ms, 300 Mbit, 20 ms) | 33.6–34.9 MB/s | 35.7–36.2 MB/s | 0.95 → 0.99 |
  | home-fibre (25 ms, 100 Mbit, 50 ms) | 10.4–10.8 | 11.8 | 0.87 → 0.98 |
  | cross-country (80 ms, 50 Mbit, 40 ms, 0.1 %) | 0.82–0.84 | 5.39–5.59 | 0.14 → 0.91 |
  | intercontinental (160 ms, 30 Mbit, 30 ms, 0.3 %) | 0.23–0.25 | 2.89–3.23 | 0.07 → 0.84 |
  | wifi-jitter (30 ms ± 15, 80 Mbit, 60 ms, 0.5 %) | 0.42 | 9.12–9.22 | 0.04 → 0.95 |
  | bufferbloat (40 ms, 20 Mbit, 500 ms) | 2.05–2.12 | 2.36–2.39 | 0.87 → 0.98; queue 500 → 82 ms |

  Timeouts in the three runs fell from 7, 9, 7, 22, 0, 13 to 0, 0, 1, 0, 0, 0. No real path has
  been measured: what a person sees between two machines is also bounded by the sender's upload
  and the receiver's download. BBR is not fair to a loss-based flow sharing a shallow queue; on
  a person's own uplink the other flow is usually their own.
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
  this continues over the internet." **#499:** the sender takes the first list that names any
  place and no later one, and keeps only public places that are not its own (the relay's
  `may_reach`), so a receiver cannot make it connect to loopback or its network on every rescue.
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
result (nearby, only when Bea said its identity after the code: a MediaViewer from before #501 says
none, and would remember this side by the link's key, so Remember waits until it updates). Ticked on both computers, a later send needs no code: Bea is a tile under **Remembered
devices** (in the internet section), and one click sends to it over the internet, whenever both
are online. The receiver still accepts or declines every transfer. (`secrets.*`, `known.*`.)

- **Identity.** An X25519 key made once and kept in the OS's secret store (`secrets.h`, a port, D9):
  - **Mac (#501):** the *data protection* keychain, in the app's own keychain access group (a
    generic password under "MediaViewer Driftwire", `kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`,
    not synchronised). There "this device only" holds: it is never in a backup or a migration to
    another Mac. Every query names the group, so another app of the same user can neither read
    these items nor plant one before Driftwire's first use (the OS lets only apps signed with the
    group touch it). The group is the app group `<team>.io.github.longtimeno-c.mediaviewer.driftwire`
    (owner, 2026-10-20: a team-prefixed app group needs no provisioning profile), added to the app's
    entitlements when the release is signed (`macpack.py release --team-id`, from the `APPLE_TEAM_ID`
    secret). The store reads it from the app's own entitlements; a build signed without it (a dev
    build, the lab binary) has no store. Items in the login keychain from before #501 (where
    the accessibility attribute is ignored, and a same-user app could have planted one) are
    removed without a prompt and never read: a Mac remembers its devices again after the update.
  - **Windows:** Credential Manager (generic credentials, this machine only, not roamed). Any
    process of the same user can read, enumerate or plant these with `CredRead` / `CredEnumerate` /
    `CredWrite`, without a prompt. So since #501 each value is sealed with DPAPI first
    (`CryptProtectData` for this user, Driftwire's own entropy, no UI): enumerating the store no
    longer shows the key or the pairing secrets, and an item written in plain (by another program,
    or by a version before #501) does not open and is refused, never believed. That is not a
    boundary against a program of the same user that calls DPAPI the same way (the entropy is in
    the source). Items from before #501 are refused, so a PC also remembers its devices again after
    the update, as a Mac does; the new ones overwrite them.
  - **Linux:** the freedesktop Secret Service (GNOME Keyring, KWallet, KeePassXC), through
    libsecret loaded at run time (`secrets_linux.cpp`; nothing to build against). Items have the
    schema `io.github.longtimeno_c.Driftwire`, a `service` ("MediaViewer Driftwire", or the
    command line's "Driftwire CLI") and a `name`, in the default collection (the login keyring,
    which the login unlocks). The Secret Service has no per-app access: any process of the same
    user on the session bus can read, list or plant an item. So each value goes in sealed
    (`crypto_secretbox`, a key from "<service> secrets v1"), and one that does not open is
    refused, as on Windows; it is no boundary against a program of the same user that does the
    same. Without a session bus nothing is loaded; a service that does not answer a probe within
    10 s, or a call that does not end in 10 s (an unlock prompt nobody answers), is no store.

  It is the static of the handshakes behind CPace (an internet code, a remembered device's
  meeting); nearby it is said sealed after the code and Accept ([The wire](#the-wire)). Where there is no store (or it does
  not keep what it is given) the key is this run's and nothing can be remembered; the toggle does
  not show. The tests use a folder (`secrets::file_store`).
- **What is remembered**: the peer's name, its identity (the static of an internet session; the
  `"key"` of its offer or accept nearby, #501), and a **pairing
  secret** only the two ends of that session hold (BLAKE2b over the session's resume secret). At
  most 16 devices, in one value of the store; Forget removes one. Removing Driftwire without
  keeping its data (**Remove everything**) erases the identity and every remembered device from
  the store too (`mv_addon_forget`, #507): installed again, it is a new device nobody remembers.
  The 17th takes the oldest's place, never one marked as this person's own; with all 16 so marked it is not remembered and
  the result says why (#502). Ticking Remember again for a device already remembered (or
  unticking and ticking it) keeps the secret it had: this session's would be on one side only,
  and the pair would stop meeting.
- **Tiles and Forget name a device by its fingerprint** (`relay::fingerprint` of its key, as the
  "own" toggles), never its place in the list (#502): a click from a window drawn before a Forget
  reaches the device it showed, or nothing. A device forgotten while it was meeting is not
  received from.
- **Who proved whom** (#497). A nearby code proves the receiver to the sender (its person typed
  what the receiver shows), not the sender to the receiver: the sender's name is what it says it
  is. So after a nearby transfer the sender's window shows a six-digit check
  (`remember_check`, keyed by the session's secret), and the receiver keeps the sender only once
  its person types that check ("Code shown on Ann"); ticking is not enough. Three wrong ones and
  Remember is not offered again for that transfer. Over the internet the receiver made the code
  and gave it to one person (CPace), so both sides are proved and Remember is a tick on both.
- **Mutual by construction.** Nothing is sent to say "I remembered you": the meeting is keyed by
  the pairing secret, so a device is found only when both kept it. One-sided, the sender waits
  ("Waiting for Bea to come online… Bea must remember this computer too") until Cancel.
- **Meeting.** Each hour (UTC) the secret gives the pair a fresh set of the four-word code's keys
  ([above](#four-words-over-the-dht-472-pr-3)) per direction: toward the receiving device's
  identity, so each side's wait and send are apart and a device never meets its own item. While
  the window is open (or Keep receiving is on), the internet and DHT toggles are on and something
  is remembered, one UDP socket and DHT client look for a sender's item (its knock) every 15
  seconds, one target per look, and publish nothing. A knock found, the waiting side publishes its
  item and the two meet and punch as with four words, UDP and TCP simultaneous open together, 15 s
  each; the first item the sender finds is that meeting's, with its live TCP port, so both punch at
  once. When the meeting ends, met or not, its item says "gone", so a later send does not punch
  toward it. (Until 2026-10-20 the waiting side kept an item up and met over UDP only: two
  computers that had met on four words over TCP never met again, [12](12-decision-log.md).) The
  sender's rounds end on the hour, when both change keys; it keeps going until it meets or Cancel.
- **Trust on first use.** The handshake is the internet one (CPace on the password of the hour the
  two met in, #499: not the hour the receiver's handshake starts in, which may have turned; then
  `Noise_XXhfs+psk0`), so only a holder of the pairing secret gets through; then the peer's static
  must be the one remembered. Another key ends the link on the side that sees it, loudly: "This is
  not the Bea you remembered: its security key has changed… If Bea was set up again, forget it
  here and send once nearby or with a code to remember it again. If not, someone may be pretending
  to be Bea." Nothing is sent or received. A device set up again with a new key is not found at all
  (its wait is keyed by its new identity), which is the same advice.
- What DHT nodes see: this computer's address taking part and looking up targets that change every
  hour and mean nothing without the pair's secret, and an item under them only while a remembered
  device is sending to it (the knock, then the meeting's item).
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
  or space, valid UTF-8, at most 1024 bytes and 32 levels; since #498 also no part starting with
  `.` and none of the characters that make text read as something else: C0 and C1 controls, the
  bidirectional overrides, embeddings, isolates and marks, line and paragraph separators, the zero
  width space and byte order mark; ZWJ and ZWNJ stay): one bad path refuses the whole offer.
- **Only the shapes a sender makes** (#498): files picked one by one (every path a bare name), or
  one folder and what is in it (every path under the same first part). Anything else (photos
  beside `Library/LaunchAgents/…`) is refused whole, so a sender cannot plant files around the
  ones the card names. The card names the files, or says "In the folder Trip".
- **Never through a link, never merged** (#498): the destination is the person's choice and is
  followed; every folder below it is made one part at a time by `io::make_directories_beneath`,
  which refuses a symlink or junction already there. A sent folder whose name is taken lands
  beside it as `Trip (2)`, never inside the one already there.
- **Marked as from another computer** (#498): each temporary gets the OS's download mark before
  its first byte, `com.apple.quarantine` on the Mac and the Mark of the Web (`Zone.Identifier`,
  zone 3) on Windows (`io::mark_from_elsewhere`), so Gatekeeper, SmartScreen and Office check it
  before it opens or runs, and an app inside a received zip inherits it when extracted. Best
  effort, as a browser's: a FAT or exFAT card on Windows keeps no stream. Received files are
  written 0644 (no execute bit).
  A free-space check (the volume's free bytes less 64 MiB) answers `no_room` before any byte moves.
- **The write follows `io::verified_copy`'s rules** ([24](24-transfer.md)) on bytes that come from
  a link: a sibling `<final>.mvtmp` created exclusively, hashed as written; at `file_end` the hash
  must be the sender's, the file is flushed to stable storage, read back uncached and hashed
  again; only then is it renamed into place with no-replace. A taken name keeps both
  (`name (2).ext`, `io::unique_name`). A mismatch, an error, a cancel or a dropped link removes the
  temporary. Files that finished stay; nothing partial is left. The file's mtime is kept,
  clamped to between 1970 and tomorrow (#498).
- **The next file arrives while the last one is checked** (#442). The thread reading the link
  writes and hashes each file; at `file_end`, once the streamed hash is the sender's, the file
  goes to a verifier (two threads, in order, at most four files waiting, each an open
  temporary) that flushes it, reads it back uncached, hashes again, renames it with no-replace
  and sends its `file_done`; the sender's `done` is answered only after every check. Every rule
  above holds for each file exactly as before; only file N+1 no longer waits for file N's flush
  and read-back. A cancel drops the file being written; a dropped link keeps it to resume
  (below); files that had arrived whole finish their check, as they did when it ran inline.
- **Staying awake** (#530). From the accept to the end, each side holds a request that the system
  not sleep from idleness (`power.h`: `PowerCreateRequest` / `PowerSetRequest(
  PowerRequestSystemRequired)` with a reason on Windows, `powercfg /requests`; an IOKit
  `PreventUserIdleSystemSleep` assertion on the Mac, `pmset -g assertions`; a systemd-logind
  `sleep` inhibitor in `block` mode on Linux, `systemd-inhibit --list`, through libsystemd or
  libelogind loaded at run time, none without a system bus or when polkit refuses; asking takes
  at most 2 s, even of a bus that never answers). The reason names the app ("MediaViewer is
  receiving files (Driftwire)"; the command line's, "Driftwire is receiving files":
  `power::set_owner`). The display may turn off and the screen lock. A closed lid, a shutdown or a
  flat battery is not held off: resume is for those.

## Resuming a receive that stopped (#530)

Until 2026-10-21 nothing partial stayed: a receive that lost its link deleted the file being
written, and the next send started from zero (and sent again what had arrived, landing as
`name (2).ext`). Now (`resume.h`, `transfer.h`'s `resume_plan`, decision log 2026-10-21):

- **What is kept.** A receive that fails (the link lost, either machine asleep or off, MediaViewer
  quitting) keeps the file it was writing as its temporary, flushed to stable storage, and a
  record in the add-on's data folder (`driftwire_resume.json`, written whole and crash-safe as
  the prefs are): the sender's identity (#501), the destination, the folder's landed name, the
  files that arrived whole (the sender's path, size and time; where it landed and the time it was
  stamped with) and the partial (its temporary and the bytes known durable). While a receive
  runs, the record is rewritten at most once a second, and the partial is flushed at most every
  5 s once 32 MiB more has arrived, so a power cut costs at most that much again. A cancel or a
  decline keeps nothing, as before. One record per sender, at most 8; one unused for 7 days goes
  with its temporary (on the control thread at start); **Discard** removes one.
- **Never taken for the finished file.** The temporary keeps its `.mvtmp` name (never the final
  name or extension), is hidden (`io::set_hidden`: `FILE_ATTRIBUTE_HIDDEN`, `UF_HIDDEN`), and is
  shown again just before its rename, since the flag travels with it. Linux has no such flag, so
  there the temporary's name starts with a dot (`.IMG_1.JPG.mvtmp`), which Files, Dolphin and
  `ls` leave out (`io::kHasHiddenFlag`, `transfer.h`'s `temp_name`). MediaViewer's own folder
  listing never lists `.mvtmp`.
- **The same sender, the same files.** The offer now says each file's time and `"resume": true`.
  When an offer comes from a sender that says its identity and has a record into the same
  folder, a file with the same path, size and time is matched: one whole and still there at its
  size and stamped time is **had**; the partial, still at least its kept length, is **taken up**.
  The accept says them: `"have"`, runs `[first, end)` of indexes (at most 8192 runs, so the accept
  fits a message whatever the offer; what does not fit is sent again), and `"at"`, `[{"i","at"}]`.
  The free-space check counts only what is still to come.
- **The sender** sends nothing of a had file, and for the partial a `file_begin` with `"at"`: it
  reads and hashes the kept bytes locally, so `file_end` is still the whole file's BLAKE3, and
  sends the rest. Both sides count what was there as moved (the progress and the time left are
  of what remains). The receiver accepts `"at"` only where it said so, exactly there, reopens the
  temporary (`io::file_writer::open_resume`: cut to that length, never through a link) and writes
  on. **The read-back after `file_end` checks the whole file**, as for every file: a damaged
  partial is refused and dropped, never renamed into place, and the next send sends it whole.
- **Older peers.** An offer without `"resume"` gets no `"have"` or `"at"`, and the receive is as
  before. A sender without an identity (no secret store) cannot be told apart from another, so
  nothing is kept for it.
- **Trust.** The identity in the offer is the sender's claim inside the sealed channel. A sender
  that claims another's key gains nothing: it can only skip files (nothing is written) or write
  on a partial whose earlier bytes it would have to know for the read-back to pass, and the
  person accepted that transfer anyway. Records and temporaries never leave the machine (rule 6).
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
- The beacon and Bluetooth INFO carry a random id, a port, the protocol spoken, and the machine's
  name only while the Driftwire window is open, or a `driftwire` session runs (#501). A machine receiving in the background, maybe
  for weeks, says `MediaViewer` and changes its id every 10 minutes while idle and whenever the
  window opens or closes, so a listener on a café network or a Bluetooth observer cannot follow it
  or read its owner's name off the air (on the Mac this keeps Bluetooth's private-address rotation
  meaningful). What stays: a machine that connects learns the name from `ident` (sealed, before any
  code), and that connection shows the receiver's notification; and the port and protocol are
  constant while presence runs. The offer travels sealed, to the peer picked.
- The long-term identity key is never in a nearby handshake (#501): each link has keys of its own,
  and the identity goes sealed, the sender's after the right code, the receiver's after Accept. A
  connector that reads message 2 and hangs up learns a key that is never used again. No account, no cloud, no server of ours: the beacon's TTL is
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
  (a word code, or a remembered device once it is sent to), a TCP port listens on every interface (its number is in
  the sealed item), and a birthday punch sends its 14-byte probes to random ports of the other
  side's public address only (46-byte hellos, #499).
- Protocol 2's resume secret and keys live in memory only, wiped when the session ends; nothing
  about a session is written down, except what Remember keeps (#472 PR 4): the peer's name, key
  and pairing secret, in the OS's secret store, never a plain file, until Forget. On the Mac that is
  the data protection keychain in the app's own access group, this device only (#501); on Windows
  Credential Manager, each value sealed with DPAPI; on Linux the Secret Service, each value sealed
  the same way ([Remembered devices](#remembered-devices-472-pr-4)).
  Key material in memory is wiped when it goes (#501): the session's resume secret with the
  channel, the identity and the code with every copy of a channel's options, and CPace's buffers
  are sized once, so no reallocation leaves a copy of the code or its key behind.
- Receiving is off until the window opens or the person turns it on. Turned on, it stays on across
  launches until turned off (#442), and the command bar says so (**Driftwire: receiving**). In a
  terminal it is on only while the session runs ([From a terminal](#from-a-terminal-driftwire)).
- What `driftwire` prints stays in that terminal. Its `--verbose` lines are the engine's `log`
  text, which never holds a path, a file name, an address, a peer's name or a code.
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
  `mediaviewer-addon-driftwire-<platform>`). **Driftwire's page there carries its settings**
  (2026-10-21; a screen in place `"settings"`, contribution API 8, so Driftwire's manifest asks
  for 8 and an older app says it needs a newer MediaViewer): under its card, in the window's
  cards and controls, **Nearby** (Keep receiving when the Driftwire window is closed, Receive when
  MediaViewer is closed), **Received files** (Save to, the folder an offer's Save to starts at),
  **Over the internet**, and while that is on, **Remembered devices** (Forget, "is my computer")
  and **Internet options** (DHT, handover, relays, your own relay server). The same node ids as
  the window, so one engine answers both and the two never disagree (`build_settings_tree`).
  Nothing that sends, makes a code or lists who is nearby. Opening it makes the engine (its
  control thread) but is not the window: no presence, beacon or Bluetooth starts unless Keep
  receiving is on, as at launch. Its tree is posted only when it changed, not with every
  progress post.
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
| The identity, remembered devices and their hourly keys (#472 PR 4) | `secrets.h`, `secrets_mac.cpp` (Keychain), `secrets_win.cpp` (Credential Manager), `secrets_linux.cpp` (Secret Service), `secrets_file.cpp`, `known.*` |
| Sealed channel, and protocol 2's session (replay, rejoin, resume) | `channel.*`, `link.h` |
| Sending and the verified receive | `transfer.*` |
| Engine, presence, the window's tree | `engine.*`, `presence.*`, `tree.cpp` |
| What is kept across launches | `prefs.*` |
| Sockets, machine name, Downloads | `net.h`, `net_posix.cpp`, `net_win.cpp`, `machine_mac.cpp` |
| Bluetooth | `bt_pipe.*`, `bt_mac.mm` (CoreBluetooth), `bt_win.cpp` (C++/WinRT), `bt_none.cpp` |
| The add-on's export and manifest | `addon_entry.cpp`, `manifest.json`; `cmake/driftwire.cmake` |
| The command line, `driftwire` (2026-10-23): the terminal port, keys, the line editor, paths, completion, the look and the live region, the arguments, the host, the session, the data folder, `main()` | `cli/term.h`, `cli/term_posix.cpp`, `cli/term_win.cpp`, `cli/keys.*`, `cli/line_editor.*`, `cli/paths_arg.*`, `cli/complete.*`, `cli/theme.h`, `cli/text.*`, `cli/render.*`, `cli/inline_view.*`, `cli/args.*`, `cli/host.*`, `cli/session.*`, `cli/data_dir.h`, `cli/data_dir_posix.cpp`, `cli/data_dir_win.cpp`, `cli/main.cpp`; `cmake/driftwire.cmake` (`mv_driftwire_cli_core`, `mv_driftwire_cli` with `OUTPUT_NAME driftwire`, the test build `mv_driftwire_cli_test`, the gallery `mv_driftwire_cli_gallery` from `tools/driftwire-cli-gallery`) |
| Its Linux packages | `cmake/driftwire-cli/` (the project, CPack, the `NEEDED` gate `needed.cmake`), `packaging/linux/` (`driftwire.1`, `completions/`, `firewalld/driftwire.xml`, `copyright`), `.github/actions/driftwire-cli-linux` |
| Its package repository and installers (2026-10-24) | `tools/package/driftwire_repo.py` (`build`: the site, its signatures and checks; `smoke`: installing from it on localhost; `windows-site`: `install.ps1` and the zip alone), `packaging/linux/repo/` (`install.sh`, the page `index.html` and its Windows part `index-windows.html`), `packaging/windows/install.ps1`, release.yml's `driftwire-repo` job, the repository step of ci.yml's `driftwire-cli-linux`; tests `tools/package/test_driftwire_repo.py`, `tools/package/check-driftwire-install.ps1` (the Windows installer end to end, on Build's msvc Release leg) |
| Its Windows zip and macOS archive (2026-10-24) | `tools/package/driftwire-windows.ps1` (`pe_fixture.py`, `test_driftwire_windows.py`), `tools/mac/macpack.py cli` (`test_macpack.py`), release.yml's `windows` and `macos` jobs, `tools/package/github-release.py` (`MV_RELEASE_CLI`) |
| Its Homebrew tap (2026-10-24) | `packaging/macos/driftwire.rb.in`, `packaging/macos/homebrew-tap-README.md`, `tools/package/homebrew_formula.py` (`test_homebrew_formula.py`), release.yml's `driftwire-homebrew` job |
| Mac chrome | `Driftwire.swift` (offer, share icon), `SlotItems.swift` (Settings), `ScreenView.swift` (Choose…), `GalleryView.swift` / `FolderStore.swift` (range, drag), `AddonNotices.swift` (notifications, the menu-bar item, #452) |
| Windows chrome | `IslandHost.Driftwire.cs`, `IslandHost.GalleryRange.cs`, `IslandHost.Slots.cs`, `IslandHost.Screens.cs`, `IslandHost.Notices.cs` (notifications, #452) |
| Native hosts | `src/shell/main_mac.mm` (the background and the login item, #452), `src/shell/main.cpp` (the background, the notification-area icon and `--background`, #452), `src/shell/login_item_win.*` (the Run value), `src/shell/addons_mac.mm` |
| Notices and keep-alive (#452) | `src/addon/slots.*` (`post_notice`, `keep_alive`, `notices_json`), `src/addon/host.cpp` |
| Tests | `tests/test_driftwire.cpp`, `tests/test_driftwire_noise.cpp` (the Cacophony and CPace vectors, #472), `tests/test_driftwire_internet.cpp` (codes, mapping against stand-in routers, two engines over a code, #472 PR 2), `tests/test_driftwire_dht.cpp` (BEP 44 / 42 vectors, the stream under loss, a stand-in DHT swarm, two engines over four words, remembered devices, behind hard NATs too, and an impostor with the pair's secret, #472 PR 3–4), `tests/test_driftwire_known.cpp` (the store's file form, the identity, the records, the hourly keys, #472 PR 4), `tests/test_driftwire_session.cpp` (rejoin, resume, the Bluetooth / Wi-Fi moves, the Wi-Fi → internet handovers, #472), `tests/test_driftwire_relay.cpp` (the rendezvous, the splice's caps, a relay code, R's answers, two engines through a relay server; the three-engine own-device relay is in `test_driftwire_dht.cpp`, #472), `tests/driftwire_fixture.h`, `tests/driftwire_swarm.h` (the stand-in DHT swarm), `tests/test_driftwire_addon.cpp` (`mv_import_tests "[driftwire]"`), `tools/package/test_addon_pack.py`; the command line's `tests/test_driftwire_cli_ui.cpp` (keys, the line editor, paths, completion, render goldens at 80 and 40 columns, the live region against a fake terminal), `tests/test_driftwire_cli.cpp` (arguments, the host, sessions over two engines on the fixture bus, the program refusing a pipe), `tests/test_driftwire_cli_e2e.cpp` (two test programs in ptys, POSIX), `tests/driftwire_cli_vt.h` (`"[cli]"`) |

## Not built

- Bonjour / DNS-SD discovery (the beacon is one portable socket on both OSes); IPv6 on the LAN.
- Bluetooth on Linux: there is no BlueZ bearer (`bt_none.cpp`), so Linux sends over Wi-Fi and the
  internet only.
- In the command line: Bluetooth on any OS; Remember on macOS (no keychain identity for a bare
  program); winget; the site and the tap live (they wait for the owner's setup: the tap
  repository and its token, Pages, the key, `MV_DRIFTWIRE_REPO`); "the first time" an internet
  option is explained is per session, not per computer. Not run: a transfer between two machines
  over Wi-Fi with the command line on one side.
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
