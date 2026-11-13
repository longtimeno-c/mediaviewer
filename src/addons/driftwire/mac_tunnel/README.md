# Driftwire Tunnel: the Mac packet tunnel provider (slice 5)

A thin pump for Driftwire's VPN mode (`src/addons/driftwire/vpn.h`, `vpn_mac.mm`). It sets the
interface's address, routes and DNS from the configuration the add-on saves, and passes packets
to and from the add-on over loopback. It holds no relay, key or TCP/IP stack.

**Owed before it can ship:** Apple's Network Extension entitlement
(`packet-tunnel-provider-systemextension`) on the signing team's Developer ID profile. Until
then it is not built (`-DMV_DRIFTWIRE_PACKET_TUNNEL=ON` builds it but cannot sign it), it is not
embedded in `MediaViewer.app/Contents/Library/SystemExtensions/`, and the add-on's VPN switch
says the build does not carry it. The app also needs `com.apple.developer.system-extension.install`
and the same Network Extension value on its own entitlements once granted.

Why it lives in the app's bundle (bending "add-ons carry their own code"): macOS loads a system
extension only from the app that installs it. See `docs/design/12-decision-log.md`, 2026-11-12.
