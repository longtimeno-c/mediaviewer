# The VPN add-on

Owner, 2026-11-19: "One VPN add-on"; the calls below were agreed with the owner the same day. Today the VPN is split between two add-ons:

- **Driftwire** connects: the Tunnel (proxy), All traffic (VPN), Block when the tunnel is
  down, the VPN host card and Connect to a VPN host…, the menu-bar / tray rows, and `/tunnel`
  in the `driftwire` program.
- **Phone access** hosts: VPN through this Mac/PC and VPN through your server, with Use as
  VPN and VPN code….

This plan makes one add-on, **VPN** (id `vpn`), with all of it on one settings page:

1. **Connect**: your VPN server (address and key, or a pasted VPN code), Tunnel, All
   traffic, Block when the tunnel is down, Look like a web site, Use for all apps.
2. **Host**: this computer or your claimed server as the exit, Use as VPN, VPN code….
3. **Menu bar / tray**: the Tunnel and All traffic rows (the host's kept menu, as now).

iOS is unchanged: it has no add-ons (the phone's VPN is built in, `src.ios`).

## Code

`src/addons/vpn/` takes, unchanged where it can: `tunnel.*`, `netstack.*`, `vpn.*`,
`vpn_mac.mm`, `vpn_win.cpp`, `vpn_none.cpp`, `sysproxy*`, `mac_tunnel/`, and the
`tunnel_*` parts of Driftwire's `engine.*`, `prefs.*` and `tree.cpp`, as a small engine of
its own (`service.*`: prefs, the secret, the proxy or VPN, the view) and its tree.

- `cmake/vpn.cmake`: `mv_vpn_engine` (static; the `driftwire` program links it for
  `/tunnel`) and `mv_vpn` (the module, `build/addons/vpn`).
- What the Tunnel needs from Driftwire today (`relay_server_of`, the relay client) moves down
  into `src/wire`, which both add-ons already link. Neither add-on links the other.
- Driftwire keeps transfers, relays for transfers, and its own "Relay (your own server)".

## Calls

**The relay setting and its secret.** The VPN keeps its own server: address in
`vpn.json`, key in the secret store under its own service ("MediaViewer VPN", `relay_key`).
Driftwire keeps "Relay (your own server)" for transfers under "MediaViewer Driftwire".
One server for both is still one paste each (or one VPN code). Neither reads the other's.

**Reaching the host / NAS link.** Phone access owns the claimed server, the host program and
the owner's link; it answers three `addon_request` methods (`mv.request.1`, the only way one
add-on reaches another): `vpn_state` (where hosting can happen: `here`, `server` or `none`,
on/off, port, running, the last problem), `vpn_set {on}` and `vpn_code` (the code, once).
Phone access answers only caller `vpn`. The VPN add-on draws the rows; its control thread
asks (never a slot worker), and refreshes when the page opens and after a change. Without
Phone access the Host section says hosting needs it. Phone access's card keeps one line
where its VPN rows were: "VPN: Settings → Add-ons → VPN" (owner 2026-11-19).

**Moving people over.** On its first start with no `vpn.json`, the VPN add-on takes the
Tunnel's settings from Driftwire's `driftwire.json` (the sibling-folder read
`adopt_old_prefs` already does for Sharepoint) and copies Driftwire's relay key into its own
item. Driftwire keeps reading and writing the old `tunnel*` keys untouched, so they are still
there whenever VPN is installed, and while they say the Tunnel was on and VPN is not
installed, its page says "The Tunnel is now the VPN add-on" with where to get it.

**The Mac packet tunnel.** The system extension stays in the app bundle (macOS loads it only
from there). Its source moves to `src/addons/vpn/mac_tunnel` and the entitlement check moves with
`vpn_mac.mm` to the VPN add-on. It keeps the id `<app>.DriftwireTunnel` and the build option
`MV_DRIFTWIRE_PACKET_TUNNEL`: renaming was agreed first as free, but 0.1.44 already ships it signed
with Developer ID profiles for that App ID, so the owner kept it (2026-11-19). The name the OS shows
for the VPN becomes "MediaViewer VPN". One decision-log row records the owner's call (the #672 row
stays as history).

**The `driftwire` program.** Both stay there (it is one program, not an add-on, and
`host vpn` is the hosting Phone access runs); owner 2026-11-19: `/tunnel` is renamed `/vpn`,
with `/tunnel` kept as an alias. Its help names the VPN add-on.

**Packaging.** `tools/package/addon-pack.py` gets `vpn`; `release.yml` builds, joins, signs
and packs it as it does Driftwire; `github-release.py` adds it to the first-party add-ons;
the add-on catalogue lists it. Driftwire's notice drops the Tunnel's BoringSSL line.

## Tests

The Tunnel and VPN tests move with the code under `[vpn]`; new tests for the adoption from
Driftwire's prefs and secret, the `vpn_*` requests (Phone access answers, other callers are
refused, the VPN page's Host rows follow them), and Driftwire's moved line. `[driftwire]`,
`[phone-addon]`, `[nas-vpn]`, `[phone-pairing]` and `[host]` stay green.
