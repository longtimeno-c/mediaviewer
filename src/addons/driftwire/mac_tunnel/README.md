# Driftwire Tunnel: the Mac packet tunnel provider (slice 5)

A thin pump for Driftwire's VPN mode (`src/addons/driftwire/vpn.h`, `vpn_mac.mm`). It sets the
interface's address, routes and DNS from the configuration the add-on saves, and passes packets
to and from the add-on over loopback. It holds no relay, key or TCP/IP stack.

**Owed before it can ship:** Apple's Network Extension entitlement
(`packet-tunnel-provider-systemextension`) on the signing team's Developer ID profile. Until
then it is off by default. `-DMV_DRIFTWIRE_PACKET_TUNNEL=ON` builds it (`cmake/darwin-tunnel.cmake`,
swiftc, the app's architectures) and `macpack.py assemble` embeds it in
`MediaViewer.app/Contents/Library/SystemExtensions/<app id>.DriftwireTunnel.systemextension`, signed
ad hoc without the entitlement (macOS will not load that copy, and the add-on, reading its
signature, says so instead of offering the VPN); `macpack.py release --team-id` signs it with
`DriftwireTunnel.entitlements` and gives the app the right to install it. The app also needs `com.apple.developer.system-extension.install`
and the same Network Extension value on its own entitlements once granted.

Why it lives in the app's bundle (bending "add-ons carry their own code"): macOS loads a system
extension only from the app that installs it. See `docs/design/12-decision-log.md`, 2026-11-12.
