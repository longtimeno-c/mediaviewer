// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN through your server, on the iPhone (docs/plans/phone-vpn.md §1.1): a packet tunnel
// extension that runs the WHOLE VPN, unlike the Mac's thin pump
// (src/addons/driftwire/mac_tunnel/PacketTunnelProvider.swift): iOS suspends the app, so nothing
// else is awake to hand packets to. Driftwire's shared VPN (netstack, vpn::runner, tunnel::dialer,
// the sealed hop, BoringSSL) runs here behind the C ABI in src/mobile/mv_mobile_vpn.h; this file
// only moves packets between NEPacketTunnelFlow and it, and sets the interface.
//
// It reads one secret, the tunnel item in the shared Keychain group, and nothing of the pairing.
// The knock key arrives derived (the app's Argon2id): the extension's memory ceiling is below it.
// Nothing is logged with a host, an address or a key.
import CMobileVPN
import Foundation
import MobileLinkSupport
import Network
import NetworkExtension

final class PacketTunnelProvider: NEPacketTunnelProvider {
  private let queue = DispatchQueue(label: "io.github.longtimeno-c.mediaviewer.vpn")
  /// The running VPN (mv_vpn*). Taken under `vpnLock`, which packet_in holds too, so a packet never
  /// reaches a VPN that is being stopped.
  private var vpn: OpaquePointer?
  private let vpnLock = NSLock()
  private var item: VPNTunnelItem?
  private var monitor: NWPathMonitor?
  private var lastPath: Network.NWPath?

  override func startTunnel(options: [String: NSObject]?, completionHandler: @escaping (Error?) -> Void) {
    let prefix = Bundle.main.object(forInfoDictionaryKey: VPNNames.prefixInfoKey) as? String
    guard let item = VPNItemStore(group: VPNNames.keychainGroup(prefix: prefix)).load() else {
      completionHandler(NEVPNError(.configurationInvalid))  // removed, or never set up
      return
    }
    self.item = item
    queue.async { [self] in
      // The server answers and takes the key, before any app's traffic is routed here.
      let started = start(item)
      guard let vpn = started.vpn else {
        completionHandler(VPNServerProblem(rawValue: Int(started.status.rawValue))?.error ?? NEVPNError(.connectionFailed))
        return
      }
      guard let settings = networkSettings(for: item) else {
        stop()
        completionHandler(VPNServerProblem.unreachable.error)  // its name did not resolve
        return
      }
      vpnLock.withLock { self.vpn = vpn }
      setTunnelNetworkSettings(settings) { [self] error in
        if let error {
          stop()
          completionHandler(error)
          return
        }
        readPackets()
        watchPath()
        completionHandler(nil)
      }
    }
  }

  override func stopTunnel(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
    monitor?.cancel()
    monitor = nil
    queue.async { [self] in
      stop()
      completionHandler()
    }
  }

  // MARK: - the VPN

  private func start(_ item: VPNTunnelItem) -> (vpn: OpaquePointer?, status: mv_vpn_status) {
    let ctx = Unmanaged.passUnretained(self).toOpaque()
    var out: OpaquePointer?
    let status: mv_vpn_status = item.relay.withCString { relay in
      item.key.withCString { key in
        item.knock.withUnsafeBytes { knock in
          var config = mv_vpn_config()
          config.relay = relay
          config.key = key
          config.tls = item.tls ? 1 : 0
          config.tls_port = item.tlsPort
          config.knock_key = item.tls ? knock.bindMemory(to: UInt8.self).baseAddress : nil
          config.packet_out = { ctx, packet, size in
            guard let ctx, let packet else { return }
            let me = Unmanaged<PacketTunnelProvider>.fromOpaque(ctx).takeUnretainedValue()
            let data = Data(bytes: packet, count: size)
            let family = (packet.pointee >> 4) == 6 ? AF_INET6 : AF_INET
            me.packetFlow.writePackets([data], withProtocols: [NSNumber(value: family)])
          }
          config.event = { ctx, problem in
            guard let ctx else { return }
            Unmanaged<PacketTunnelProvider>.fromOpaque(ctx).takeUnretainedValue().serverSaid(problem)
          }
          config.ctx = ctx
          return mv_vpn_start(&config, &out)
        }
      }
    }
    return (status == MV_VPN_OK ? out : nil, status)
  }

  private func stop() {
    let v = vpnLock.withLock { () -> OpaquePointer? in
      defer { vpn = nil }
      return vpn
    }
    mv_vpn_stop(v)
  }

  /// The server's state changed (any of the VPN's threads).
  private func serverSaid(_ problem: mv_vpn_status) {
    switch problem {
    case MV_VPN_OK: reasserting = false
    case MV_VPN_KEY_REJECTED:
      // An old code (Use as VPN was turned off and on): trying again cannot help.
      cancelTunnelWithError(VPNServerProblem.keyRejected.error)
    case MV_VPN_UNREACHABLE:
      // A forward failed at the server: ask it once more, off the VPN's thread, to learn why (the
      // event tells only a change, so this does not repeat while nothing changes).
      reasserting = true
      queue.async { [weak self] in
        guard let self, let v = self.vpnLock.withLock({ self.vpn }) else { return }
        _ = mv_vpn_check(v)
      }
    default: reasserting = true
    }
  }

  // Interface -> VPN, for as long as it runs.
  private func readPackets() {
    packetFlow.readPackets { [weak self] packets, _ in
      guard let self else { return }
      let running = self.vpnLock.withLock { () -> Bool in
        guard let vpn = self.vpn else { return false }
        for p in packets {
          p.withUnsafeBytes { raw in
            _ = mv_vpn_packet_in(vpn, raw.bindMemory(to: UInt8.self).baseAddress, raw.count)
          }
        }
        return true
      }
      if running { self.readPackets() }
    }
  }

  // MARK: - the interface

  /// IPv4 and IPv6 both (§1.3: without IPv6 an app's IPv6 goes around the tunnel), every address
  /// through it, the server's own addresses and the local networks left out, DNS to the server's
  /// resolver. nil when the server's name does not resolve on this network.
  private func networkSettings(for item: VPNTunnelItem) -> NEPacketTunnelNetworkSettings? {
    guard let raw = mv_vpn_settings_json(item.relay) else { return nil }
    let json = String(cString: raw)
    mv_vpn_free(raw)
    guard let s = VPNInterfaceSettings.parse(json) else { return nil }
    let host = item.relay.split(separator: ":").dropLast().joined(separator: ":")
    let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: host.trimmingCharacters(in: CharacterSet(charactersIn: "[]")))
    let v4 = NEIPv4Settings(addresses: [s.address4], subnetMasks: [VPNInterfaceSettings.mask(s.prefix4)])
    v4.includedRoutes = s.include4.map(Self.route4)
    v4.excludedRoutes = s.exclude4.map(Self.route4)
    settings.ipv4Settings = v4
    let v6 = NEIPv6Settings(addresses: [s.address6], networkPrefixLengths: [NSNumber(value: s.prefix6)])
    v6.includedRoutes = s.include6.map(Self.route6)
    v6.excludedRoutes = s.exclude6.map(Self.route6)
    settings.ipv6Settings = v6
    let dns = NEDNSSettings(servers: [s.dns4])
    dns.matchDomains = [""]  // every name, through the server's resolver
    settings.dnsSettings = dns
    settings.mtu = NSNumber(value: s.mtu)
    return settings
  }

  private static func route4(_ r: VPNInterfaceSettings.Route) -> NEIPv4Route {
    r.isDefault ? NEIPv4Route.default() : NEIPv4Route(destinationAddress: r.base, subnetMask: VPNInterfaceSettings.mask(r.prefix))
  }

  private static func route6(_ r: VPNInterfaceSettings.Route) -> NEIPv6Route {
    r.isDefault ? NEIPv6Route.default() : NEIPv6Route(destinationAddress: r.base, networkPrefixLength: NSNumber(value: r.prefix))
  }

  /// Wi-Fi ↔ cellular: the forwards' sockets belong to the old path. Reassert, set the routes again
  /// for the new network (its local networks, the server's addresses on it), and ask the server.
  private func watchPath() {
    let m = NWPathMonitor()
    m.pathUpdateHandler = { [weak self] path in
      guard let self, path.status == .satisfied else { return }
      defer { self.lastPath = path }
      guard let last = self.lastPath, !path.isSameNetwork(as: last) else { return }
      self.reasserting = true
      // On `queue` already (the monitor's), as stop() is: the VPN cannot go while this runs.
      guard let item = self.item, let settings = self.networkSettings(for: item) else { return }
      self.setTunnelNetworkSettings(settings) { [weak self] _ in
        guard let self else { return }
        self.queue.async {
          guard let v = self.vpnLock.withLock({ self.vpn }) else { return }
          self.reasserting = mv_vpn_check(v) != MV_VPN_OK
        }
      }
    }
    m.start(queue: queue)
    monitor = m
  }
}

private extension Network.NWPath {
  /// The same interfaces as before (a new address on the same Wi-Fi is not a move).
  func isSameNetwork(as other: Network.NWPath) -> Bool {
    availableInterfaces.map(\.name) == other.availableInterfaces.map(\.name)
  }
}
