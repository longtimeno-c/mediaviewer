# Plan — Self-hosting: your own computer as the NAS and the VPN exit

Owner's ask (2026-11-14): the Synology has died. Make it possible to host everything a Synology was
going to do on one's own PC, from the `driftwire` command line: **host the VPN** (be the Tunnel's exit),
**host a NAS** (open one of the PC's drives to the phone the way the Synology would have), and **build
DuckDNS in** so a home address that changes still has a name the phone and laptop can find, simple
enough for someone who has never forwarded a port. Not built; this maps it out. Owner calls are in §9.

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

On the PC with the drive, in `driftwire` (or Settings → Phone access → **Use this computer**):

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
With the TLS look, the relay takes 443 instead (`--tls-port`), unless the router's 443 is someone
else's (`mapping` never takes over a port already forwarded). Nothing else: never SMB, never RDP.

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
on (`--forward-ports 80,443`, `--forward-udp-ports 53` as the relay's defaults) and maps the relay's
port. The laptop puts `name.duckdns.org:PORT#KEY` in **Relay (your own server)** once and turns on the
Tunnel or **All traffic (VPN)**; the sites then see the home's address.

- The relay already refuses to forward to private, loopback and link-local addresses, so the VPN is
  **not a way into the home network**. If the owner wants "VPN home to reach my own LAN" that is a
  different feature (§9 call 5), not this.
- **The TLS look** with a real certificate passes a prober best. DuckDNS supports TXT records, so a
  later slice can get a Let's Encrypt certificate for `name.duckdns.org` by DNS-01 with no port 80
  needed. That is an ACME client (BoringSSL is already linked); out of the first slices, self-signed
  until then (§9 call 4).
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
- **The Synology work is paused, not removed** (§9 call 1): the SPK and its tests stay; S5's real-unit
  checks wait for a unit.

## 9. Owner calls (answered 2026-11-14: "use recommendations")

1. **The Synology path is paused**, not removed: the SPK, its tests and the plan stay; S5's real-unit
   checks wait for a unit.
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
   `status`, autostart per OS, the power assertion. Tests `"[host]"` on loopback.
3. **DuckDNS keeper** (`ddns.*`, `"[ddns]"` against a loopback stand-in) and `host name duckdns`.
4. **Router keeper** for the host's ports (reusing `mapping.*` against its stand-in routers), the
   carrier-NAT message, the decision-log row.
5. **`host setup`** (the guided run) and `/host` in the interactive program; the QR in the terminal.
6. **The desktop card's "This computer"** on Mac and Windows (both halves, D9), and the copy changes
   on the phone and desktop, demoable in SAMPLE.
7. **`host vpn on`** and the relay line; `docs/design/28` gains "Self-hosting", `docs/DEVELOPMENT.md`
   the run recipe.
8. Later: Let's Encrypt via DNS-01; a custom DDNS URL; the laptop as a `serve` client.

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
