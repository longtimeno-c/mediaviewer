# Driftwire: a fuller menu-bar menu, then a tunnel through your own relay

Status: **slice 1 under way** (2026-11-12, owner's ask). Slices 2–4 are a proposal awaiting the
owner's sign-off: they add a product surface Driftwire does not have (forwarding a person's
own web traffic), so nothing of them is built until that call is made.

## Why

Two asks from the owner, 2026-11-12:

1. The menu-bar item (Mac) / notification-area icon (Windows) that Receive when MediaViewer is
   closed leaves ([28 "In the background"](../design/28-driftwire.md#in-the-background-452))
   has four fixed lines: the status, Open Driftwire, Open MediaViewer, Quit. Driftwire's
   everyday switches and "send something" are a window away. Make the menu the fast way in.
2. On a public network that filters sites (and blocks the common VPNs), reach the web through
   one's own Driftwire relay server (`tools/driftwire-relay`), with the toggle in that menu.

What a tunnel can and cannot do, so nobody builds the wrong thing: encryption alone does not get
past a filter (HTTPS already hides the content; the filter blocks by DNS name, TLS SNI or
address). Traffic has to *leave* through a machine the filter allows. With only the laptop
there is no such machine, and no Driftwire change helps. With the person's own relay server
(a NAS, a home computer) it is that machine.

## Slice 1: the add-on's own menu rows (this slice)

**The mechanism (shared core, both hosts).** A kept add-on may give the host a short list of
rows for its menu, after its status line and before the host's own Open / Quit rows:

- host table `keep_menu(host, menu_json)`, appended after `still_extend`; `menu_json` is
  `{"items":[…]}`, at most 16 rows, each one of
  - `{"kind":"text","label":…}`: a line that does nothing (what is happening now);
  - `{"kind":"toggle","id":…,"label":…,"value":bool}`: a checkmark row;
  - `{"kind":"button","id":…,"label":…}`;
  - `{"kind":"files","id":…,"label":…}`: the host asks for files (its own open panel) and
    hands the chosen paths back; nothing happens when the person cancels;
  - `{"kind":"separator"}`.
  Labels ≤ 120 bytes, as `keep_alive`'s; ids `[a-z0-9_]`. Never a path or file name (rule 6).
  An empty list or `NULL` removes the rows. Kept per add-on in the slot registry and shown in
  `mv_slots_notices_json`'s `keep` entry as `menu`.
- `mv_slots_menu_action(addon, item, value_json)`: a row was picked. The registry checks the
  row exists with that kind (a toggle's value a boolean, a files row's an array of strings) and
  posts `MV_SLOT_EVENT_MENU` (16) to the add-on, screen = the kept screen, node = the row's id.
- The rows reuse the window's controls: a toggle row's id **is** the window's toggle's node id,
  and the add-on handles it exactly as a change in the window (or its settings page). No
  second code path for what a switch does.

**Driftwire's rows.**

| Row | When | Does |
|---|---|---|
| "Sending to Ann's MacBook · 42%" / "Receiving from … · 42%" / "… wants to send you files" | a transfer or an offer | nothing (text) |
| Cancel | a transfer under way | the window's `out_cancel` / `in_cancel` |
| Send Files… | always | the host's open panel, then the files go to the window's Send card and the window opens |
| ✓ Receive from nearby | always | the window's `receiving` (off also ends the background: the menu goes) |
| ✓ Send and receive over the internet | always | the window's `internet` |
| ✓ Receive when MediaViewer is closed | always | the window's `background` (off: the menu goes) |

The host's rows follow: Open Driftwire, Open MediaViewer, Quit MediaViewer. The rows are
rebuilt only when their text changes (a percentage moves in whole steps), and
the menu is built by a pure function (`build_menu`, `tree.cpp`), tested like the window's tree.

**Verify (slice 1).** Mac: with Receive when MediaViewer is closed on, the menu-bar menu shows the
rows; each toggle row flips the window's switch and back; Send Files… sends the chosen files to
the Send card; during a transfer the progress line and Cancel appear and Cancel ends it.
Windows: the same on the notification-area icon's menu. `mv_tests "[slots]"` and
`mv_import_tests "[driftwire]"` cover the registry's checks and `build_menu`. Both present-loop
gates hold (nothing here touches the render path; the menu is chrome).

## Slices 2–4: Driftwire Tunnel (proposal; owner's sign-off first)

- **2. Forwarding on the relay server.** A key holder may ask `driftwire-relay` to open a TCP
  connection to `host:port` and copy bytes both ways, on the relay's existing sealed hop
  (CPace on the server key, then Noise). Never for anyone without the key: not an open proxy.
  Limits: ports 80/443 only by default, a connection cap, no UDP. ProVerif: the new request
  type in `relay.pv`.
- **3. A local proxy in the add-on.** SOCKS5 and HTTP CONNECT on `127.0.0.1` (a random port,
  shown with a Copy button), each connection one forwarded stream. Browsers, the Claude app and
  CLIs (`HTTPS_PROXY`) use it; no VPN driver, no OS entitlement. The menu gets **✓ Tunnel
  through <relay>**; the path picture a "tunnel" bearer. The `driftwire` terminal program gets
  `/tunnel`.
  **The system proxy, so nothing is pasted** (owner, 2026-11-12: "sort of like a VPN"). While
  the tunnel is on, Driftwire also points the computer's own proxy setting at it, so every app
  that follows that setting (browsers, the Claude app, most desktop apps) goes through the relay
  with nothing to configure:
  - The setting is a **PAC file** that Driftwire serves on the same local port and answers with
    `SOCKS5 127.0.0.1:<port>; DIRECT`. If the proxy is gone (a crash, the relay unreachable),
    apps fall back to the Wi-Fi as it is. The tunnel never leaves a machine with no internet.
  - Mac: the active network services' automatic proxy URL (SystemConfiguration; it may ask for
    an administrator once). Windows: the per-user WinINet proxy (`InternetSetOption`, no
    administrator rights).
  - Driftwire saves what was there before and puts it back when the tunnel goes off and when
    Driftwire quits. If MediaViewer crashed with the tunnel on, it restores the setting at the
    next start. It changes the setting only if the setting is still Driftwire's own.
  - A switch under the tunnel, **Use for all apps** (on by default), turns this off for anyone
    who wants the local address only.
  - Not a VPN: apps that ignore the system proxy (many command-line tools, some games, anything
    over UDP) keep using the network directly. The window says so in one line. The SOCKS5
    hostname is resolved at the relay, so DNS filtering on the Wi-Fi does not see it.
- **4. Looking like HTTPS.** The relay on TCP 443 behind TLS 1.3 with a browser's ClientHello,
  the sealed hop inside; a connection that does not authenticate is answered as an ordinary web
  site. For networks that block what looks like a tunnel. Its own spike first.

What the tunnel exposes: the Wi-Fi sees a TLS connection to the relay's address; the sites see
the relay's address; the relay sees which hosts are visited (never the content, which stays the
browser's TLS). A network that only lets through sites on a list, or breaks TLS with its own
certificate, defeats this as it defeats every tunnel; the plan does not claim otherwise.
