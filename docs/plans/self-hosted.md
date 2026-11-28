# Plan — Self-hosting: your own computer as the NAS and the VPN exit

Owner's ask (2026-11-14): the Synology has died. Make it possible to host everything a Synology was
going to do on one's own PC, from the `driftwire` command line: **host the VPN** (be the Tunnel's exit),
**host a NAS** (open one of the PC's drives to the phone the way the Synology would have), and **build
DuckDNS in** so a home address that changes still has a name the phone and laptop can find, simple
enough for someone who has never forwarded a port. Not built; this maps it out. Owner calls are in §9.
**A Synology stays supported** (owner, 2026-11-14: "synology is good to support"): self-hosting is a
second way to run the same service, beside the NAS, not a replacement for it.
**Not in this plan, owner's asks of 2026-11-14 (#686):** Phone access as an add-on (the Synology path
and self-hosting out of the base app); the shared drive as a LAN-only SMB share with a username and
password; combining two or more drives (mirror, span, stripe, parity) through the OS's own tools, with
each choice explained and erasing guarded; a tray / menu-bar presence while hosting. Each waits for
the owner's calls listed there.

## 1. The good news: almost nothing is Synology code

The recent work (0.1.42: Phone access, the NAS service, the Tunnel and VPN) was *planned around* a
Synology, but the programs are generic:

| Piece | What it is | Synology-specific? |
|---|---|---|
| `driftwire serve` (`src/wire/serve.*`, `cli/serve_cmd.cpp`) | the NAS service: named folders, pairing, grants, backup writes, owner passphrase, relay waiters, the direct-path upgrade | **No.** Portable C++; tested on loopback on a Mac. Never run on Windows or Linux. |
| `mv-driftwire-relay` (`tools/driftwire-relay`, `splice.*`) | the relay server: rendezvous for codes and phones, **forwarding (the Tunnel/VPN exit)**, the TLS look | **No.** Already "a NAS or a home computer" in `docs/design/28`. Built for Windows in `build-release.ps1`. |
| Tunnel / VPN client (`tunnel.*`, `netstack.*`, `vpn*.cpp`) | the laptop's side | **No.** It needs a relay address; it does not care what runs it. |
| Claim, pairing QR, phone link, backup | desktop ↔ service ↔ phone | **No**, except that claim proves the share by writing `.mediaviewer/` over SMB, which on the same PC is a local write. |
| `driftwire_spk.py`, `nas-setup.md`, `probe-synology.sh`, `start-stop-status`, the musl builds | packaging for DSM | **Yes.** |
| The setup card's "Which NAS" step, DSM's words, DSM's DDNS (`mobile.md` §7.3), `/volume1/…` examples, "Synology" in iOS and desktop copy | the guided path | **Yes.** |
| `cli/nas_update.*` (signed self-update) | update core | Generic core; only its supervisor (`start-stop-status`) is DSM's. |

So self-hosting is **a new host for existing programs**, not a rewrite: one command that runs `serve`
and the relay together on the PC, starts with the PC, keeps a name pointed at it, opens the router for
it, and a desktop card that sets it up on the same machine.

## 2. What the person does (target)

On the PC with the drive, in `driftwire` (or Settings → Add-ons → Phone access → **Use this computer**):

```
driftwire host setup          one guided run: name, drive, router, phone, VPN (below)
driftwire host                run it all, no terminal (what starts at login)
driftwire host status         name, address, ports open or not, phones, VPN on/off, last DuckDNS update
driftwire host share PATH [--label L] [--backup] | unshare L
driftwire host name duckdns   asks for the subdomain and token (typed or pasted, never an argument)
driftwire host vpn on|off     the relay forwards for this host's key (the Tunnel/VPN exit)
driftwire host start-at-login on|off
```

and `/host …` the same in the interactive program. `host setup` walks:

1. **A name.** "Do you have a DuckDNS name? (free, duckdns.org)" → subdomain + token. Checked by one
   update; the program then resolves `name.duckdns.org` and compares it with the public address the
   update reported. Skip → LAN only (works at home; says so).
2. **A drive or folder** to give phones (`D:\Photos`, `/Volumes/Photos`), and optionally a backup folder
   with a limit and reserve (the existing `backup` fields).
3. **The router.** Ask it to forward the service's port (and the relay's, if the VPN is on) with the
   existing `mapping.*` (PCP → NAT-PMP → UPnP-IGD, leases renewed). If the router says no or is behind
   carrier NAT, print the one port to forward by hand. Then the outside check (§5.3).
4. **A phone.** Prints the pairing QR in the terminal (the desktop app shows it too), with the DuckDNS
   name as the public route.
5. **The VPN.** Prints the relay line `name.duckdns.org:PORT#KEY` once, for the laptop's
   **Relay (your own server)** field (and the phone's, when the phone VPN lands).

## 3. `driftwire host`: one supervisor, two services

A new CLI command (`cli/host_cmd.*`) that owns a data folder (`<data dir>/host/`, 0700; on Windows the
user profile's ACL as `file_store` already relies on) and runs:

- **`serve`** in-process, from `host/serve/` (`serve.json`, secrets), exactly the existing
  `serve::server`. The roots are the person's shared folders.
- **the relay** in-process (`splice.*` linked from `mv_driftwire_engine`, as `mv-driftwire-relay` does),
  from `host/relay/` (its key in the secret store, never printed again after setup). It serves the
  phone's relay waiters only when §5.4 applies, and forwarding when `host vpn on`.
- **the DuckDNS keeper** (§4) and **the router keeper** (§5), threads of their own.

One process because a home user should see one thing running and one status line. A crash restarts the
whole host after 30 s (the SPK supervisor's rule, reused).

**Starting with the computer, without an administrator** (D9: one file per OS behind a port,
`cli/autostart_*.cpp`):

| OS | How | Why that one |
|---|---|---|
| Windows | a per-user Scheduled Task "at log on", hidden, restart on failure | no admin; a Windows service would run as LocalSystem, which the keys must not belong to |
| macOS | a LaunchAgent in `~/Library/LaunchAgents`, `KeepAlive` | per user, no admin |
| Linux | a `systemd --user` unit; `loginctl enable-linger` suggested (needs the person's sudo, printed, never run) | runs headless after reboot only with linger |

Consequence to say plainly: on Windows and macOS **the host runs while that user is logged in**. A PC
left at the lock screen works; a PC at the login screen after a reboot does not, until someone logs in
(Windows auto-logon is the person's choice, never ours). That is the honest gap against a NAS.

**Sleep.** A sleeping PC is a dead NAS. While the host runs it holds the OS's "system required"
assertion through the existing `power.h` port (no setting changed; the display may still sleep), and
`host status` says so. A toggle turns it off for laptops.

**Elevation.** POSIX `serve` refuses root; Windows has no such check today
(`serve_cmd.cpp:669` is POSIX only). `host` refuses to run elevated on Windows as well (keys would be
the administrator's, and a non-elevated `host status` could not read them).

**Updates.** The PC already updates `driftwire` through its own install channel (`cli/update.*`). The
host reuses the existing exit-75 path: an installed update ends `host` with 75 and the autostart entry
starts the new binary. The NAS feed (`nas_update`) is not needed on a PC.

## 4. DuckDNS, built in (`src/addons/driftwire/ddns.*`)

DuckDNS's whole API is one HTTPS GET:
`https://www.duckdns.org/update?domains=SUB&token=TOKEN&ip=&ipv6=V6&verbose=true` → `OK`/`KO` (with
`verbose`, the addresses it set). Empty `ip=` means "the address you saw me from".

- **Where it runs:** the host only. Never in the viewer, never on the phone.
- **When:** at start, every 5 minutes while the public address is unchanged (a cheap check), and at once
  when the router keeper or an interface change says the address moved; at most once a minute. IPv6:
  sent only when the PC has a global IPv6 address and the service's port is open on it (§5.2), else
  cleared, so a phone never prefers a v6 address that cannot answer.
- **Secrets:** the token goes in the secret store (`ddns_token`) like `relay_key`; the subdomain in
  `host.json`. Typed or pasted, never an argument (the CLI's existing rule). Never logged, never in a
  crash report.
- **Privacy (rule 6):** DuckDNS learns the home's public address and that the name is used, which is the
  point of it; nothing about files. The setup says so in one line. Opt-in; LAN-only works without it.
- **Not tied to DuckDNS forever:** the keeper is `ddns::provider` with DuckDNS the only one built; a
  "custom update URL" provider (most routers and other DDNS services take a URL) is a later slice if
  asked. A router's own DDNS also works today: the person just types that name instead.
- **Tests** (`"[ddns]"`): against a stand-in HTTPS server on loopback (`OK`, `KO`, timeouts, a changed
  address, backoff), never the real duckdns.org in CI.

The name goes in the pairing QR as the public route (a name, as `mobile.md` §7.3 already requires for
NAT64), and in the relay line for the laptop.

## 5. Being reachable from outside

### 5.1 Ports
Two TCP ports at most: the service's (7878 by default, `serve.json`) and, with the VPN on, the relay's.
With the TLS look (on by default with the VPN, [vpn-stealth](vpn-stealth.md)), only the look's port is
mapped, at outside 443, or 8443 when the router's 443 is someone else's (`mapping` never takes over a
port already forwarded). Nothing else: never SMB, never RDP.

### 5.2 The router
`serve` deliberately "never touches a router's mapping" (`docs/design/28`). The host changes that, for
the host only: it asks the router with `mapping.*` for the ports in §5.1, renewing short leases while it
runs and removing them on stop, so a crash leaves them open minutes, not forever. IPv6: an IGDv2
pinhole where the router offers one. **This is a decision reversal (§9 call 2) and needs a row in
`docs/design/12-decision-log.md` when built.**

### 5.3 The check
After mapping, the host asks its own relay port and service port to be reached from outside. Without a
public helper (there is none and will not be: `nas-relay.md` §6 call 1) the honest checks are:
the router's outside address is public (not RFC 1918, not 100.64/10 carrier NAT) — `mapping::notes()`
already says this; the DuckDNS name resolves to it; and the final answer is the phone on mobile data
(the setup asks the person to try it, as `mobile.md` §2.2 does).

### 5.4 Behind carrier NAT
If the outside address is not public, no forward can help, and **a relay on the same PC cannot help
either** (it is behind the same NAT). The phone then works at home only, unless the person runs
`mv-driftwire-relay` on a small rented server (a VPS) and sets it with the existing `set_relay`; then
the phone→home path is the relay plus the direct-path upgrade already built (`nas-relay.md` §9). The
setup says this in plain words instead of failing quietly. IPv6 often gets around it (§4).

## 6. The NAS part: a drive on the PC as the phone's library

Everything the phone does with a Synology works unchanged against the host, because it is the same
service. What is new is the drive being a PC's drive:

- **Removable and external drives.** A root whose folder is missing (drive unplugged, letter changed)
  answers `roots` with that root marked offline, instead of failing the session; the host rechecks every
  minute and when the OS says a volume arrived. On Windows the root is kept by the volume's GUID path as
  well as the letter, so `E:` becoming `F:` still finds it.
- **File systems.** NTFS, APFS, exFAT, ext4. exFAT has no links or owners: `open_beneath` still refuses
  hidden and link entries; the `0700` checks apply to the host's data folder only, never the drive.
- **Windows `serve` is untested.** `open_beneath` on Windows holds each folder open without
  share-delete (built, never run). The first slice runs `"[driftwire-serve]"` and `"[phone-relay]"` on
  Windows CI, and fixes what breaks.
- **Claiming on the same machine.** The claim proves write access to the share by a token in
  `.mediaviewer/`; on the host the desktop writes it locally. `on_own_network` must count loopback as
  the service's own network (check `serve.cpp:205`; a test either way). The claim still never comes
  through a relay.
- **The viewer on the same PC** reads the drive directly, as any folder. The host never stands between
  the PC's own MediaViewer and its files.
- **Backup** from the phone writes into the backup folder exactly as built (`serve_backup.*`).
- **Your other computers.** A laptop can already open the drive over the OS's own file sharing at home;
  through the host it is a later slice (a desktop client of `serve`, out of scope here).

## 7. The VPN part: the PC as the exit

The Tunnel/VPN client needs only a relay with forwarding. On the host, `host vpn on` turns forwarding
on (every port, TCP and UDP, apart from the blocklisting ones: `relay::vpn_host_defaults`) and maps the relay's
port. The laptop puts `name.duckdns.org:PORT#KEY` in **Relay (your own server)** once and turns on the
Tunnel or **All traffic (VPN)**; the sites then see the home's address.

- The relay already refuses to forward to private, loopback and link-local addresses, so the VPN is
  **not a way into the home network**. If the owner wants "VPN home to reach my own LAN" that is a
  different feature (§9 call 5), not this.
- **The TLS look** is on by default with the VPN and gets a Let's Encrypt certificate by DNS-01
  through DuckDNS's TXT (built 2026-11-19, [vpn-stealth](vpn-stealth.md); self-signed until then).
- **Home upload speed** is the VPN's speed. `host status` shows the relay's measured totals only (never
  hosts, rule 6).

## 8. Copy and the Synology path

- "NAS" in the desktop card and the phone becomes **"your server"** where the text means the service
  (it can be a NAS or this computer); "Synology" stays only inside the Synology guide. Strings in
  `src.managed/Shared/PhoneWords.cs`, `PhoneAccessView.swift`, `src.ios/…/Copy.swift`,
  `SetupScreens.swift`, the App Store listing and the support page.
- The card's **Set up the service…** (`nas-setup.md` §5) gets a first choice: **This computer** (§2,
  done in-app on Mac and Windows, no terminal) · **A Synology** (the existing plan) · **Something else**
  (the manual guide).
- The phone's sample library shows a "server" whose kind is "a computer" so the copy change is
  demoable through SAMPLE (owner's rule, 2026-11-12).
- **A Synology stays a supported choice** (§9 call 1): the setup card, the SPK, its updates and the
  docs keep it; "your server" covers both, and the Synology's own words stay in its guide and wherever
  the text is about a NAS.

## 9. Owner calls (answered 2026-11-14: "use recommendations")

1. **Both are supported** (owner, 2026-11-14, replacing the first answer "paused"): a Synology
   (`nas-setup.md`, the SPK, signed updates) and this computer (`driftwire host`) are equal choices in
   the setup card and the docs. `nas-setup.md` work continues; only S5's real-unit checks wait, until
   the owner has a working unit to try them on.
2. **The host may map router ports** for its own ports only, short leases removed on stop. This
   reverses "serve never touches a router's mapping" for the host; the decision-log row lands with
   slice 4, which builds it.
3. **DuckDNS only, first**; a custom update URL later if asked.
4. **Real TLS certificates via DuckDNS DNS-01: later**, after these slices; self-signed until then.
5. **No VPN into the home LAN** in this work: the relay keeps refusing private addresses.
6. **Start at login** (per user, no administrator); no Windows service or system unit.
7. **Linux after Windows and Mac**, as D9 stands: `host` is written hostable (the CLI already builds
   on Linux) and its systemd user unit follows; Linux's own open calls (`docs/plans/linux.md` §10)
   are not settled by this.

## 10. Order

1. **`serve` on Windows** in CI (`"[driftwire-serve]"`, `"[phone-relay]"`, `"[phone-pairing]"`, which
   `mv_import_tests` already builds on Windows), the elevated-run refusal, loopback as own network for
   the claim, offline roots. No new feature; makes the ground solid. **Built 2026-11-14** (below).
2. **`driftwire host`**: the supervisor, `host.json`, `share`/`unshare`, serve + relay in one process,
   `status`, autostart per OS, the power assertion. Tests `"[host]"` on loopback. **Built 2026-11-14**
   (§12).
3. **DuckDNS keeper** (`ddns.*`, `"[ddns]"` against a loopback stand-in) and `host name duckdns`.
   **The keeper built 2026-11-14** (§12); `host name duckdns` is owed to slice 2's `host` command.
4. **Router keeper** for the host's ports (reusing `mapping.*` against its stand-in routers), the
   carrier-NAT message, the decision-log row. **Built 2026-11-14** as a module (§12); `host` calls it in slice 2.
5. **`host setup`** (the guided run) and `/host` in the interactive program; the QR in the terminal.
   **Built 2026-11-14** with the keepers wired into `host` (§16).
6. **The desktop card's "This computer"** on Mac and Windows (both halves, D9), and the copy changes
   on the phone and desktop, demoable in SAMPLE. **Copy built 2026-11-14 (§15); the card (§16).**
7. **`host vpn on`** and the relay line; `docs/design/28` gains "Self-hosting", `docs/DEVELOPMENT.md`
   the run recipe. **Built 2026-11-14** (§16).
8. Let's Encrypt via DNS-01 and the person's own domain (by CNAME): **built 2026-11-19**
   ([vpn-stealth](vpn-stealth.md)). Later: a custom DDNS update URL; the laptop as a `serve` client.

**Verify (proposed):** on a Windows PC and a Mac, `driftwire host setup` with a DuckDNS name and a
UPnP router makes the name resolve to the home's address, opens the service's port, and pairs a phone
from its QR with no NAS and no password; from mobile data the phone lists, range-reads, seeks and backs
up to a folder on an external drive; unplugging the drive marks its root offline and replugging under a
new letter brings it back; a laptop on another network with the host's relay line runs **All traffic
(VPN)** and sites see the home's address; after a reboot and login the host is back by itself; on stop
the router's mappings are gone. In CI, every new piece on loopback only (no public DHT, no real
duckdns.org, no real router). Both present-loop gates still hold (nothing here touches the viewer).

## 11. Slice 1, as built (2026-11-14)

- **Elevated runs refused on Windows** too: `serve_is_privileged()` in the `serve_tty` port (root on
  POSIX, `TokenElevation` on Windows); `driftwire serve` says to run it from a terminal that is not
  elevated.
- **Same-machine claim:** loopback already counted as the service's own network (`net::on_link`, and
  a dual-stack socket unmaps `::ffff:127.0.0.1`); now tested with the service's own rule rather than
  the tests' switch (`[phone-pairing]` "a desktop on the same computer claims the service over
  loopback").
- **Offline roots:** `roots` marks a named folder whose folder is not there now `"offline":true`
  (sent only then, so an older phone reads the same list), and a `list`, `stat`, `read_range` or
  `write` that fails on such a folder ends `not_found` with `{"why":"offline"}`. The folder is checked
  only for `roots`, before a write at the folder's top (where no walk would fail), and after a request
  has failed. The session goes on and reads
  again once the drive is back (`[driftwire-serve]` "a named folder whose drive is gone …").
- **Known limits (review, 2026-11-14):** on Windows a root that is itself a link or junction to an
  unplugged drive still reads online (`stat_path` uses `GetFileAttributesExW`, which does not follow
  it), so the phone gets a plain `not_found`; an unmounted Linux mount point left as an empty folder
  reads online, and a backup there would write to the system disk (older than this slice; slice 2
  compares the folder's device with its parent's); `roots` now touches each folder, so a hung network
  share can stall that one session's `roots`.
- **Not yet:** the phone saying "offline" (the Swift side reads the flag only once a screen shows it),
  the volume GUID path for a drive letter that changes, and the host's minute recheck (slice 2).

## 12. Slice 2, as built (2026-11-14)

- **`driftwire host`** (`cli/host_cmd.*`): a supervisor that runs `host child` (the same program) and
  starts it again 30 s after it ends unasked; the child holds `serve::server` and the relay server in
  one process, from `<data dir>/host/` (0700: `host.json`, `serve/`, `relay/`, `secrets/`,
  `state.json`, `host.lock`). A service whose port is taken is tried again every 30 s inside the child
  without stopping the other. `serve.json` and `host.json` are read again every 2 s; the running
  service takes new folders with `server::replace_config`; unsharing the last stops it.
- **`host share PATH [--label L] [--backup]`, `unshare LABEL`, `status`, `start-at-login on|off`,
  `stay-awake on|off`.** A share is refused for the host's own folder or one holding it (its keys);
  `--backup` has no limit of its own (the 1 GiB reserve keeps the drive from filling); `--limit GB|none`
  sets one or takes it off, also on a folder shared already. `status` prints labels, ports, counts and switches, never a path or an address.
- **Start at login**, one file per OS (`cli/autostart_win.cpp`, `_mac.cpp`, `_linux.cpp`; texts pure
  in `autostart.cpp`): a per-user Scheduled Task at log on (least privilege, retried every 5 min while
  not running, since Task Scheduler's restart-on-failure does not cover an exit), a LaunchAgent with
  KeepAlive unless exit 0, a systemd user unit with Restart=on-failure and linger printed only.
- **Updates**: the supervisor ends 75 when its program on disk changes; the entry starts the new one.
- **Power**: `power::hold_for(kHostReason)` while hosting, `stay-awake off` to drop it.
- **Offline roots (§11's owed items)**: Windows keeps a folder's volume GUID path and moves it to a new
  letter at the minute's recheck; POSIX keeps `mount` and reads an unmounted mount point offline;
  serve looks at such a folder before every write (`options::root_present`).
- **Hooks for slices 3 and 4**: `cli/host_keepers.h` (`host_keeper`, `keeper_context`,
  `host_keepers()` in `host_cmd.cpp` returns none yet). `ddns.*` and `host_ports.*` are not created
  here.
- **Owed:** running any of it on Windows or Linux (CI builds and runs `"[host]"` there; the
  Windows port files were written without a Windows compiler here), registering a real Scheduled
  Task, LaunchAgent or systemd unit by hand and rebooting into it, the console window that may flash
  at login on Windows (the host frees a console made for it alone), a volume-arrival notice (the
  minute's recheck only), and on Windows the entry reads "on" from its XML file even if the task was
  deleted in Task Scheduler by hand.

## 13. Slice 3, as built (2026-11-14)

- **`src/addons/driftwire/ddns.*`** in `mv_driftwire_engine`, a module of its own with no caller
  yet: `ddns::provider` with `ddns::duckdns` the only one, `ddns::keeper` (its thread, the schedule
  of §4, `status()` for `host status`), `parse_subdomain` / `parse_token`, `save_token` /
  `load_token` / `forget_token` (`ddns_token` in the secret store), and `check_name` (resolve the
  name, compare it with what DuckDNS reported). The header shows the whole use for `host`.
- **The HTTPS client** stays the command line's (`cli::https_get`); the engine cannot link it, so
  `host` hands it in as a `fetch_fn` that keeps the status and body and drops the transport's
  error text (it can hold the URL, and so the token). No new TLS dependency.
- **DuckDNS keeps an old IPv6** until `clear=true`, which clears both records: an update without
  IPv6 clears and sets the IPv4 again (one round trip with no record) when the answer shows one,
  and also on the keeper's first update and until one without IPv6 has succeeded, so it does not
  hang on whether DuckDNS's verbose answer reports a kept record (not checked against the real
  service). One timeout covers the update's requests together.
- **The IPv4 sent** is the router keeper's (slice 4, `address_changed`) when public; none, or a
  carrier-NAT or private one, sends `ip=` empty and DuckDNS uses the address it sees, which it can
  do over IPv4 only (a host that reaches DuckDNS over IPv6 needs slice 4's address; open).
- **Hardening (Opus review, 2026-11-14):** a subdomain or token that `parse_*` would not give back
  unchanged is refused before any request; `stop()` is safe twice and from any thread; the log has
  a line for the first success, a change, a failure and a recovery, not every quiet 5 minutes.
- **Tests** `"[ddns]"` (`tests/test_driftwire_ddns.cpp`), on a Mac: a stand-in DuckDNS on
  `127.0.0.1` over plain HTTP (the real fetch takes https only, so the stand-in's fetch is the
  test's own GET): OK, KO, an HTTP error, a timeout, nobody there, the stale-IPv6 clear, the
  schedule with made-up times, the keeper's thread (start, period, change, IPv6 only when public,
  backoff and recovery, stop while hanging), and no log line with the token, subdomain or an
  address.
- **Owed:** `host name duckdns` and the keeper in `host` (slice 2's session); the router keeper's
  `address_changed` and `set_ipv6` calls (slice 4); the setup check's wording and retry (slice 5).
  Not run: the real `https_get` against duckdns.org (by design, never in CI), Windows and Linux
  builds of the module (portable C++ over `net::resolve`; CI's legs will build it).

## 14. Slice 4, as built (2026-11-14)

- **`host_ports::keeper`** (`src/addons/driftwire/host_ports.*`): given the host's ports (each with
  an optional outside port and a fallback, e.g. the relay's TLS port at 443 falling back to its own),
  it removes a crashed run's leftovers (the ledger), asks the router for each with `mapping::mapper`
  (PCP → NAT-PMP → UPnP-IGD; an IGDv2 IPv6 pinhole at the internal port where this computer has a
  public IPv6 address and PCP did not map it), and hands what it got to `mapping::keeper`, which
  renews each 120 s lease every 60 s and removes everything when the host stops.
- **The report** per port: mapped / refused / not offered (nobody answered), the method, the outside
  port a phone dials; the router's outside IPv4 and its kind: public, double NAT (private),
  carrier-grade NAT (100.64/10, and DS-Lite's 192.0.0/29) or "not public" (a non-public IPv6
  outside address); 0.0.0.0 (a WAN that is down) is no address. The ledger is written as each forward
  is made, so a crash mid-start leaves nothing unlisted. `mapping::mapper::last_outside()` is new for this: PCP's answer now tells the kinds apart.
- **`host_ports::advice()`**: plain lines with no address. Carrier NAT: no forward helps, a relay on a
  rented server is needed (§5.4), IPv6 noted if a pinhole opened; no hand-forward line then. Double
  NAT: bridge mode or forward on both. A port not mapped: the one port to forward by hand (the
  fallback for 443, "outside port 443 instead if nothing else uses it").
- **The outside address is asked every minute** on the keeper's own thread (NAT-PMP, else UPnP); a new
  public one calls `on_outside_changed` at once (the DuckDNS keeper's kick) with no dependency on `ddns.*`.
- **Tests** `"[host-ports]"` (in `tests/test_driftwire_internet.cpp`, against its stand-in PCP,
  NAT-PMP and UPnP routers on loopback): mapped, renewed and removed on stop; a UPnP forward and
  pinhole removed and the ledger gone; someone else's 443 left alone and the fallback mapped; carrier
  NAT from NAT-PMP and from PCP; double NAT; a silent router and its hand-forward lines; a changed
  outside address told once, and a move behind carrier NAT followed. Mac only so far.
- **Owed:** the host and the app's own receive over the internet share UPnP's description
  ("MediaViewer Driftwire"), so on the same outside port one could renew or remove the other's
  forward (unlikely: different ports); the IPv6 pinhole takes the first public IPv6 address, which
  may be a temporary one that rotates; the call from `driftwire host` and `host status` (slice 2's session); a router that stops
  renewing is not shown in the report (`mapping::keeper` retries quietly); a PCP-only router's outside
  address is read once at start, not followed (NAT-PMP and UPnP are asked); moving the PC to another
  network keeps the old gateway's forwards until the host restarts; the outside check (§5.3: the
  DuckDNS name resolves to the outside address) belongs to the DuckDNS keeper; Windows CI runs the
  tests once the branch is in a PR.

## 15. Slice 6's copy, as built (2026-11-14)

- **"Your server"** wherever the phone and the desktop card meant the service in general (owner
  2026-11-14: a Synology stays a supported choice, so what-you-need lists name it, "or your own computer" beside it): the phone's failure,
  backup, setup and settings words (`MobileUI/Copy.swift`, `SetupScreens.swift`, `SettingsScreen.swift`,
  `BackupScreen.swift`, `RootView.swift`), the local-network prompt, the card's rows and lines on both
  hosts (`PhoneWords.cs`, `IslandHost.Phone.cs`, `PhoneAccessView.swift`), the site's iPhone paragraph,
  the support and privacy pages, the review notes and the 1.1 listing draft. Code names, wire fields and
  JSON keys (`nas`, `serve`, `synology`) are unchanged. The App Store name and subtitle (**MediaViewer
  for NAS**) stay until the owner says otherwise; `listing.md`'s 1.0 section stays the record of what was
  entered.
- **The sample** stands in for a server that is a computer (Settings: "Server · A computer"), and its
  backup folder is "Sample server on this iPhone".
- **Offline folders on the phone:** `mv_root_fn` carries `offline`; `mv_files_roots` lists an offline
  folder without looking inside it, and `mv_files_offline_roots` lists only those. The Library's top
  level adds the server's offline folders after its own (a library with a catalogue too), each chip an
  unplugged drive with "Not connected"; a tap says the drive isn't connected and opens nothing. The
  sample's `static_remote::roots()` adds one, "Travel drive" (not in `files.json`; no republish).
- **Not yet (at the time):** the desktop card's **This computer** choice (§16); an offline backup folder is still
  offered in the phone's backup picker; a catalogue folder whose root is offline is not marked (the
  catalogue's folders are not tied to the service's roots on the phone).

## 16. Slices 5 and 7, as built (2026-11-14)

- **The keepers in the host** (`cli/host_keepers.*`): `make_ddns_keeper` around `ddns::keeper` and
  `make_router_keeper` around `host_ports::keeper`, returned by `host_keepers()`. Each rebuilds on a
  worker of its own when the context changes (`keeper_context` gained `relay_tls_port` and
  `ddns_subdomain`; `host.json` gained `ddns`, `ddns_stamp`, `tls_port`, `library_id`), so the
  host's loop never waits on the network. The router gets the service's port, and the relay's and the
  web-site look's (outside 443, falling back to its own) with the VPN on; its new public outside
  address kicks DuckDNS. `host status` prints both keepers' lines from `state.json` ("DuckDNS
  home.duckdns.org, updated 3 min ago"; "router service 7878 open (UPnP), relay 47614 open" and the
  advice), never an address or the token; `state.json` also carries the outside ports.
- **`host setup`** (`cli/host_setup.*`): the five steps of §2 plus start at login. DuckDNS: subdomain
  and token typed (the token with echo off), one update, then the name resolved and compared every
  10 s for about 90 s; skip = LAN only, said plainly. The folder through `host share`, a backup folder
  with a limit in GB. The router: a probe keeper when no host runs (its forwards removed after; the
  host asks for its own), the running host's line when one does; `advice()` gives the one port to
  forward by hand, and carrier NAT's "a relay on a rented server". A phone: the QR in the terminal.
  Then `host vpn on` if wanted.
- **`host name duckdns|none`, `host pair`, `host vpn on [--tls-port N]|off`** run one part alone;
  `/host …` in the interactive program runs them all (`/host` alone is status): printing commands in
  the transcript (`print_capture`), asking ones with the terminal handed over (`terminal::run_cooked`).
- **The QR** (`cli/qr.*`): the Windows card's encoder (`QrCode.cs`, after Project Nayuki's MIT
  library; notice kept, no new dependency) in C++, pinned to the same scanner-read matrices; drawn
  with half blocks, black on white set by each line. A pairing code is version 19 or so: about 99
  columns, so an 80-column window needs widening (setup says so).
- **The host library.** A phone pairs with a library; a host with no desktop makes its own (an id,
  a publisher key from a seed in the host's secrets) and writes its `library.json` in
  `<data dir>/library/.mediaviewer`, shared as "Catalogue", so the phone opens the folders in file
  mode as it does a NAS's before Local search publishes. The payload is `catalog::pair_url`'s (read
  back in the tests by the phone's own `parse_pair_payload`); the public route is
  `name.duckdns.org:<outside port>`; no relay route (the host's relay is behind the same router). A
  service a desktop has claimed (another Catalogue there) is refused: its phones pair from the
  desktop's card and get its library, since a phone that pinned the host library would be refused
  once the desktop published. `host unshare` takes only the host library's Catalogue with the last
  folder, never a desktop's.
- **`host vpn on`**: forwarding on in the running host's relay (every port but 23, 25, 135-139 and 445, TCP and UDP; private,
  loopback and link-local refused, call 5), the relay's port mapped, the relay line
  `name.duckdns.org:PORT#KEY` printed (the LAN address without a name). `--tls-port 443`: the
  relay's TLS look with its own self-signed certificate for the name (call 4); a look that cannot
  listen (443 taken, or not allowed on Linux) leaves the relay running without it, said in `host
  status`; `vpn off` clears it.
- **Hardening (Opus review, 2026-11-14):** the router keeper asks again on a 1, 5, 15-minute backoff
  while nothing is open yet (a start at login before the network is up, no answer; a refusal three
  times at most), never once a port is open, since a new ask closes the old forwards a moment; a
  mix of open and refused ports waits for the ports wanted to change or a restart; the
  DuckDNS keeper keeps a hash of the token it was built from, not a copy; the QR's text and the
  session's captured relay line are wiped after use; `/host unshare` takes a label with spaces; the
  test build's `/host` stays on loopback.
- **Tests** `"[host-setup]"` (`tests/test_driftwire_host_setup.cpp`, two in
  `test_driftwire_internet.cpp` beside the stand-in routers, one in `test_driftwire_cli.cpp`), Mac:
  the QR against the C# pins; the arguments (no token or subdomain as an argument); `host.json`; the
  whole guided run with a stand-in DuckDNS and resolver, then a phone (`serve::client` on `::1`)
  redeeming the QR's code once, finding the host library's `library.json` and reading a photo; the
  name skipped, refused, mistyped, offline, late to resolve, forgotten; the DuckDNS keeper in the
  running host (status line, a new name taken, no token or address in `state.json`), the test build's
  real keepers idle; the router keeper in the running host against a stand-in PCP router (the
  service's port, the relay's with the VPN on, both removed on stop, `host status`); setup's router
  step behind a stand-in carrier NAT; a laptop's Tunnel proxy forwarding through the host's relay
  with the printed line, off refusing, and the shipped options refusing the home's own addresses;
  `/host share|status|vpn on|setup` in a session.
- **Owed:** running it on Windows and Linux (CI's legs build and run the tags; not run here), a real
  router, the real duckdns.org and a real phone scanning the terminal's QR (the matrices match the
  desktop's, which a scanner read; the terminal drawing was looked at, not scanned). On Windows,
  setup reads lines with `fgetc` in the console's own code page (as `serve passphrase set` does), so
  a typed folder outside ASCII arrives garbled: a `ReadConsoleW` line reader in `serve_tty_win.cpp`
  is owed. Ctrl+C during a `/host setup` handed over from the session reaches the session, not the
  setup (Enter through it, or run it in its own terminal). The QR's quiet zone is 2 modules (the
  standard asks 4), and without colour (`--no-color`, `NO_COLOR`) it is ink on the terminal's own
  background, inverted on a dark one. If a desktop claims the service after phones paired from
  `host pair`, those phones meet the desktop's library and must pair again from its card. The TLS
  look's fallback (the relay without it) is not tested (loopback picks its own port). The relay line's port is the
  router's outside port only once the running host has mapped it: `host vpn on` run before that
  prints the relay's own port (the same unless the router picked another; `host status` shows it).
  The QR's size: a shorter payload (the hex keys) would need a new payload version on the phone.


## 17. Slice 6's desktop card, as built (2026-11-14)

- **Set up the service…** in Settings → Add-ons → Phone access (Mac `PhoneAccessView.swift`, Windows
  `IslandHost.Phone.cs`, words in `Shared/PhoneWords.cs` checked against the Swift) when no folder is
  chosen and nothing is claimed: **This computer** · **A Synology** (the Package Center steps in one line,
  then the existing folder and claim rows, unchanged) · **Something else** (`driftwire serve init`, then
  the same rows). No choice is preselected over another beyond the picker's first place.
- **This computer**, one request (`{"do":"host_setup","folders":[…],"duckdns","duckdns_token"}`) to the
  phone link's own thread (`src/phone/link.cpp` `host_setup`, `src/phone/self_host.*`); nothing blocks
  the UI thread, and no new C ABI call is needed (`mv_phone_request` / `mv_phone_state_json`, ABI
  unchanged). In order: the folders checked (one that phones read, absolute paths, none twice; the
  DuckDNS name and token by `ddns`'s own rules, the tests compare both); each folder not yet shared
  through the installed program's `driftwire host share PATH [--label L] [--backup]` (its own rules and
  words: a refusal shows its line; labels made unique first, since it refuses one in use, and folders
  already shared matched as the OS matches paths); the DuckDNS name where slice 5's `host name duckdns`
  puts it (`host.json` `ddns` + `ddns_stamp`, the token as `ddns_token` in `<data>/host/secrets`, 0700),
  because that command asks at a terminal only, and an unreadable `host.json` is left alone; `driftwire host start-at-login on`, which registers the entry and starts the
  host through the OS (never the app's child; up to 150 s for Task Scheduler); up to 30 s for the port
  in `state.json` to answer on loopback (a file left by a crashed host names a dead port);
  the first folder phones read becomes the library's folder (`library.json`); the claim over loopback
  (127.0.0.1, then ::1); the addresses (this computer's on the route to its gateway, and
  `name.duckdns.org:PORT` from outside, PORT being the router keeper's `service_outside_port` once it
  has mapped one, else an address typed before stays); then a code, as **Add a
  phone** makes it. A setup that stopped part way (claimed nothing yet) offers the choice again.
- **The program** is found where its installers put it (Homebrew's `/opt/homebrew/bin` and
  `/usr/local/bin`, `~/.local/bin`; `%LOCALAPPDATA%\Programs\Driftwire`; then PATH), and the data
  folder as the program picks it. With none installed the card gives its one install line and
  **Check again**. The app still does not carry it (decision log, 2026-11-14).
- **The status row** ("This Mac/PC is your server · NAME"): running or not, the service's port, the
  DuckDNS name, drives not connected, and the keepers' own lines from `state.json` (the router's and
  DuckDNS's: CGNAT and hand-forward words come from `host_ports::advice` there), with **Start** when it
  is not running and **Stop** when it is (`host start-at-login off`, then `host quit`: it stays off
  until Start; folders and pairings stay). Read when Settings opens and at the link's 30 s tick,
  never per frame.
- **After setup** (claimed): a **DuckDNS name** row, **Add…** / **Change…** opening the name and token
  (`host_name`: checked and kept as setup does, then `set_addresses` points phones' outside address at
  it); and **Share another folder** picks the folder with the OS's picker (a Windows path, `G:\…`, as
  the service's `add_root` takes it) instead of a typed NAS path.
- **Tests:** `"[phone-self-host]"` in `mv_import_tests` (the places against the command line's own
  constants, state.json and host.json read and written, the inputs; and against the test build of the
  program on loopback: shared, started, claimed, addresses, a code; a DuckDNS name in the code and the
  token in the host's store and nowhere the card reads; refusals before anything runs; no program),
  `PhoneAccessTests` (Swift), `PhoneWordsTests` (C#, against the Swift). `io::child_process` gained an
  opt-in to read the child's stderr with its stdout.
- **Not done:** either card driven on screen (the Mac one built and its tests run; the Windows one
  compiled on the Mac and its words tested, never run); a real `host start-at-login on` from the app
  (the tests start the test build instead); **the DuckDNS name does nothing on this branch alone**: the
  keepers (`host_keepers()` returns none here), `host.json`'s `ddns` key, the `host name duckdns`
  command and a `host_settings_json` that keeps that key come with the host-setup branch (slices 5 and
  7), so until it is merged the card writes a name the host does not read yet and a later
  `host stay-awake` would drop it; the router and DuckDNS status lines likewise; the phone's LAN address is this computer's IPv4 at setup and is not followed
  if DHCP moves it (the card's address field still changes it); a host set up from the terminal shows its
  status row but the card offers no unshare or rename (the terminal does).

## 18. The VPN switch on the desktop card, as built (2026-11-15)

Ask: the VPN that slice 7 built for the command line (`host vpn on|off`), available from Phone access too.
- **Settings → Add-ons → Phone access**, this computer's server: a **VPN through this Mac/PC** row with a
  **Use as VPN** switch, its status and **Relay line…** ([28](../design/28-driftwire.md#the-vpn-exit-driftwire-host-vpn-onoff-slice-7-2026-11-14)).
  The switch runs the installed program's own `host vpn on|off`, so the CLI's forwarding rules, ports and
  words are the only ones (no second implementation); the line is the one `vpn on` prints. Drawn by the
  add-on's own tree: both chromes, no chrome code. Built on PR #706 (the add-on page).
- **Phones**: unchanged. The iPhone app has no VPN yet ([phone-vpn.md](phone-vpn.md), plan only, its owner
  calls open), and the code carries no relay route for a host. Turning the VPN on does not change how a
  phone reaches the library; the test pairs a phone and reads a photo with it on.
- **Tests**: `"[phone-addon]"` (the row's states, the switch's requests, the line copyable and hidden, the
  error), `"[phone-self-host]"` (host.json and state.json read as the program writes them, the line found in
  its output and nothing else), and the switch end to end against the test build of the program
  (on, the running relay taking it, the line, hidden, asked again, a phone paired and reading, off).
- **Measured**: `"[.phone-host-bench]"`, below in DEVELOPMENT.md; numbers in the PR.
- **Not done**: the card driven on screen on either host (the tree is tested, the chromes draw its node
  types already); the web-site look's port from the card (`--tls-port`: the terminal only; the card shows it).
- **Then (owner, 2026-11-15: "do it in desktop app first so my macbook can connect to a vpn host on windows /
  synology")**: the card's line became a code (`mediaviewer://relay#…`), the MacBook's Driftwire add-on
  gained **Connect to a VPN host…**, and a Synology became a VPN host too (`set_vpn`, the relay inside
  `driftwire serve`, the SPK's port section), all in the same PR at the owner's choice
  ([28](../design/28-driftwire.md) "The NAS as a VPN host", "Connect to a VPN host"). The phone's own VPN
  stays [phone-vpn.md](phone-vpn.md)'s, after this.

## Tray: the server menu while hosting (issue #686 §4, owner 2026-11-14)

Owner's calls: **yes, a tray / menu-bar icon while the computer hosts, through the app's existing
background icon code**; then (same day) **its own item, as Driftwire's is, with Hosting on/off,
read/write usage and the clients connected, and the same numbers in the desktop app and the
command line**. Not a second app or a tray process (decision 2026-10-11: add-ons carry their own
code; never a helper app): it is MediaViewer's own background presence (#452,
[28 "In the background"](../design/28-driftwire.md)), the one Driftwire's "Receive when MediaViewer
is closed" uses.

### Its own item

The #452 mechanism is a list of kept entries (host table `keep_alive` + `keep_menu`, rows `text`,
`toggle`, `button`, `files`, [driftwire-menu-and-tunnel.md](driftwire-menu-and-tunnel.md) slice 1).
Until now every kept entry shared one menu-bar item. Now **each kept entry has its own item** on the
Mac (an `NSStatusItem` per entry, `AddonNotices.swift`) and **its own notification-area icon** on
Windows (one `NOTIFYICONDATA` uID per entry, `main.cpp sync_tray`; the icon's uID picks its menu).
Driftwire alone looks as before. The server menu is one more entry: until Phone access is its own
add-on (#686 §1, PR #691) the base app holds it (`slot_registry::set_shell_keep("mediaviewer.host")`,
`"shell": true` in notices_json, its Open row opening Settings → Add-ons → Phone access); then the add-on calls
`keep_alive` / `keep_menu` with the same rows and gets the same item with no host change. The Mac's
icon is `server.rack`, Driftwire's stays the waves; Windows uses the app's icon with the entry's
tooltip ("Hosting as home.duckdns.org").

### How it appears when the host starts at login with no window open

- The host draws nothing (a terminal program; a tray in it would be a second app). The app reads the
  host's files as the This computer card does: `host.lock` held means it runs, `state.json` says what
  it has, `host.json` its DuckDNS name (`phone/host_tray.h`). The item shows **only while the host
  runs** (and for 90 s after it lets go of its lock, "Not running just now: starting again…", so an
  update's restart or a crash the supervisor restarts neither ends the hidden app nor pops its
  window; not after Quit hosting from the menu).
- The app must be running, so the host's start at login (`host start-at-login on`, the card's Start)
  is also a reason for **the app's own login item** (#452's: SMAppService on the Mac, the Run entry on
  Windows, started `--background`, no window): `slot_registry::wants_login_item`, notices_json's
  `"login"`. Nothing new is registered with the OS. A login start that finds the host not running
  quits after its 60 s grace and keeps the login item while the host's entry is there.
- With the app open, while the host runs, closing the window hides it to the item (as a kept add-on
  does). When the host's entry was all that kept a hidden app and it goes (Quit hosting, past the
  grace), the app quits on both hosts (Windows no longer shows the window for that case). Quit
  MediaViewer quits the app only: the host goes on.

### What it shows (`host_tray::build`, sentence case, said once)

1. The head: "Hosting as home.duckdns.org" (or "Hosting from this computer").
2. **Hosting** (a checkmark toggle). On: serving. **Off = `host pause`**: the service and the relay
   stop, the router keeper lets its mappings go, the stay-awake request is dropped; the host process
   keeps running (so the item stays and says "Off: phones can't reach this computer"), and start at
   login stays as set. Kept across restarts and logins until turned on again (`host resume`).
3. One status line ("Serving phones", "Not serving: its port is taken", "Nothing shared yet") and a
   line per problem (the relay's port taken, the web-site look).
4. Drive health: "All 3 folders online" / "1 of 3 folders offline", then combined drives from PR
   #692's keeper (its `state.json` keeper line, a set per line, the advice after ";" left to the card;
   `drives_health` degraded, offline or failing marks the item's status as a problem), two lines at
   most.
5. Usage: "Today: 3.4 GB read, 120 MB written · reading 12 MB/s" (both rates: "now 12 MB/s and
   2.0 MB/s"). Today is the computer's local date.
6. The phones connected, four at most then "and 2 more phones": "Ann's iPhone · home network since
   14:02 · 1.2 GB read, 3.0 MB written · reading". How: **home network** (a direct session from this
   computer's own network), **over the internet** (direct from away, a punched path included),
   **through your relay**. Since: the earliest session up now, local time. Reading / writing: bytes
   moved in the last 2 s. By the name the phone was paired under ("A phone" for one not on the list);
   never an address, a key or a path (rule 6). The owner's own sessions (the desktop that claimed the
   service) are not a phone and not counted.
7. **Quit hosting** (`host quit`), then the hosts' **Open Phone access settings**, Open MediaViewer,
   Quit MediaViewer.

### One source for the numbers: the host's `state.json`

- **The service counts** (`serve::server::usage()`): per session, file bytes sent (`read_range`) and
  written (backup uploads), one relaxed atomic add per chunk on the conn, no lock and no syscall;
  how and since when are set once at admission. A finished session's bytes are folded per device
  when it is reaped. `usage()` sums ended and live sessions per paired device.
- **The host folds** at its existing 2 s pass (`cli/host_usage.h` `usage_book`): what is new since
  the last pass is added to today's totals, per device and in all, and divided by the time for the
  rate; a service started again (a new port, Hosting turned on) counts from 0 and nothing is lost.
  Today's totals are kept in `<host>/usage.json` (device keys in hex, never a name), written only when
  they moved, so a restart or an update keeps them; a new local day starts from 0.
- `state.json` gains `"paused"`, `"clients": [{"name","via","since","read","written","read_rate",
  "write_rate"}]` (connected now) and `"usage": {"day","read","written","read_rate","write_rate",
  "devices": [{"name","read","written"}]}` (today, every device that moved bytes). It is rewritten only
  when it changed, as before: idle, nothing is written.
- **Read by three:** the menu (above); the This computer card's status row in Settings → Add-ons → Phone access
  (Mac and Windows: "Running, hosting off" when off, then the same usage and phone lines from the
  core, `host_tray::usage_lines` through `self_host::host_status::stats`); and the command line:
  `driftwire host status` (`hosting on|off`, `today`, a line per device today, `connected` and a line
  per phone) and **`driftwire host status --watch`**, which prints it again whenever it changed,
  looked at every 2 s (the host's own cadence), redrawn in place in a terminal, until Ctrl-C.

### Control through the host's own verbs

`driftwire host pause | resume | quit` (`cli/host_control.*`; one `else if` in `cli/args.cpp`'s host
chain, `--watch` one flag), owner-only: each writes `host.json` in the host's 0700 folder, which the
running host already reads every 2 s. No socket listens for them.
- **Pause / resume**: `"paused"`, as Hosting off / on above.
- **Quit**: `"quit_stamp"` (unix ms). The supervisor gives its own start to its child
  (`DRIFTWIRE_HOST_SINCE_MS`); a child whose supervisor started before the stamp ends with exit 4
  (kHostChildQuit), which the supervisor takes as a stop and then clears the stamp (so a clock set
  back at the next boot stops nothing); a supervisor between restarts checks it before the next
  child. The verb waits up to 10 s for `host.lock` to go. Start at login stays as set.

### Cost (CLAUDE.md "Performance is the product")

One thread in the app (the first window's process; the Mac skips it in a soak or harness run):
every 5 s a stat of `state.json` and, when it exists, a moment's try of `host.lock` (30 s when there is
no host folder at all; 1 s for three looks after a verb, as the host takes it within 2 s); the files
are read only when the stat changed; the start-at-login entry every minute. The registry, and so the
hosts' items, hear only a changed model. Nothing presents: the viewer is hidden as for any
background. The verbs run on that thread through `io::child_process` (rule 1), up to 30 s; the app's
quit does not wait for one. In the service, one relaxed atomic add per chunk sent or written; in the
host, `usage()` walks at most 32 sessions at each 2 s pass. Not measured against the PR 1 gates on
this branch (nothing on the render path changed).

### Later, and not done

- Started on its own (Windows `mv_host_tray_start` / `mv_host_tray_stop` from the chrome's first-window
  branch and the shell's quit; the Mac beside its add-on host claim), not by Phone access's resume,
  which PR #691 removes. The verbs run whichever driftwire `self_host::find()` returns first (PR
  #691's own copy in the add-on's folder once it lands).
- When Phone access is its own add-on (PR #691), the watcher moves into it and it calls `keep_alive`
  and `keep_menu`; `set_shell_keep`, `abi/host_tray_glue.*` and the hosts' `"shell"` cases go. An
  add-on keep has no "login" reason yet: carry `wants_login_item`'s over (a flag on `keep_alive`).
- "Later desktop": a laptop connected as a client of the service (slice 8) shows as a phone does.
- Linux draws it once its host has a tray. A host before anyone logs in stays out (owner call 6).
- Windows: the watcher is not skipped in a soak run (the Mac's is); only matters on a soak machine
  that hosts.

**Tests:** `"[host-tray]"` in `mv_import_tests`: the model (not running, start at login, the head,
the toggle on and off, each problem, nothing shared, usage and its rates, each way a phone is
connected, the cut after four, a long name cut, the drives keeper's lines, the 16-row limit, no path
or port in a label), the bytes words, `state.json`'s clients / usage / drives read, the usage book
(rates, a service started again, kept and read back, a new day), the registry's shell keep (drawn,
kept, the login flag, picks routed, refused rows), the watcher over a folder of the test's own (a held
`host.lock`, `state.json` written, the toggle's pick run as `resume`, a made-up one refused, the login
entry, the grace then gone), `host pause` / `resume` taken by the service in-process on loopback with
the quit stamp asked, and the service's `usage()` with a paired session reading a file (counted,
connected, then kept once it ended). C#: `PhoneWordsTests` (the card's line with the stats, off).
