// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Driftwire VPN's packet tunnel provider (slice 5; src/addons/driftwire/vpn.h,
// vpn_mac.mm): a thin pump. The system starts it only when the Driftwire
// add-on asks (NETunnelProviderManager in vpn_mac.mm); it sets the
// interface from that configuration and passes packets both ways over a
// loopback TCP connection to the add-on, each as a 2-byte length and the
// packet, after the add-on's token. It has no relay, no key and no stack:
// those are the add-on's. docs/design/12 (2026-11-12) records why it lives
// in the app's bundle.
import Foundation
import Network
import NetworkExtension

final class PacketTunnelProvider: NEPacketTunnelProvider {
  private var link: NWConnection?
  private var buffer = Data()

  override func startTunnel(options: [String: NSObject]?, completionHandler: @escaping (Error?) -> Void) {
    guard let proto = protocolConfiguration as? NETunnelProviderProtocol,
          let config = proto.providerConfiguration,
          let port = config["port"] as? Int, let token = config["token"] as? String,
          let address4 = config["address4"] as? String, let prefix4 = config["prefix4"] as? Int,
          let dns4 = config["dns4"] as? String, let mtu = config["mtu"] as? Int
    else {
      completionHandler(NEVPNError(.configurationInvalid))
      return
    }
    let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: proto.serverAddress ?? "127.0.0.1")
    let v4 = NEIPv4Settings(addresses: [address4], subnetMasks: [Self.mask(prefix4)])
    if let include4 = Self.routes(config["include4"]), let exclude4 = Self.routes(config["exclude4"]) {
      v4.includedRoutes = include4.map(Self.route4)
      v4.excludedRoutes = exclude4.map(Self.route4)
    } else {
      // An add-on from before IPv6 (vpn_mac.mm sends both).
      v4.includedRoutes = [NEIPv4Route.default()]
      var excluded: [NEIPv4Route] = []
      for a in (config["exclude"] as? [String]) ?? [] where !a.contains(":") {
        excluded.append(NEIPv4Route(destinationAddress: a, subnetMask: "255.255.255.255"))
      }
      for n in Self.routes(config["excludeNets"]) ?? [] where !n.base.contains(":") && n.prefix > 0 {
        excluded.append(Self.route4(n))
      }
      v4.excludedRoutes = excluded
    }
    settings.ipv4Settings = v4
    // IPv6 into the tunnel too: without it an app's IPv6 goes around the
    // tunnel (the relay carries it, vpn.h settings). An older add-on sends no
    // IPv6 routes: the default route still, with its relay and networks left
    // out, never IPv6 left alone.
    let address6 = (config["address6"] as? String) ?? "fd5d:7269:6674::1"
    let prefix6 = (config["prefix6"] as? Int) ?? 64
    let v6 = NEIPv6Settings(addresses: [address6], networkPrefixLengths: [NSNumber(value: prefix6)])
    if let include6 = Self.routes(config["include6"]), let exclude6 = Self.routes(config["exclude6"]) {
      v6.includedRoutes = include6.map(Self.route6)
      v6.excludedRoutes = exclude6.map(Self.route6)
    } else {
      v6.includedRoutes = [NEIPv6Route.default()]
      var excluded: [NEIPv6Route] = []
      for a in (config["exclude"] as? [String]) ?? [] where a.contains(":") {
        excluded.append(NEIPv6Route(destinationAddress: a, networkPrefixLength: 128))
      }
      for n in Self.routes(config["excludeNets"]) ?? [] where n.base.contains(":") && n.prefix > 0 {
        excluded.append(Self.route6(n))
      }
      v6.excludedRoutes = excluded
    }
    settings.ipv6Settings = v6
    let dns = NEDNSSettings(servers: [dns4])
    dns.matchDomains = [""]  // every name
    settings.dnsSettings = dns
    settings.mtu = NSNumber(value: mtu)
    setTunnelNetworkSettings(settings) { [weak self] error in
      guard let self, error == nil else {
        completionHandler(error)
        return
      }
      let c = NWConnection(host: "127.0.0.1", port: NWEndpoint.Port(rawValue: UInt16(port))!, using: .tcp)
      self.link = c
      c.stateUpdateHandler = { state in
        switch state {
        case .ready:
          c.send(content: Data((token + "\n").utf8), completion: .contentProcessed { _ in })
          self.readFromApp(c)
          self.readFromInterface(c)
          completionHandler(nil)
        case .failed(let e):
          completionHandler(e)
        default:
          break
        }
      }
      c.start(queue: .global(qos: .userInitiated))
    }
  }

  override func stopTunnel(with reason: NEProviderStopReason, completionHandler: @escaping () -> Void) {
    link?.cancel()
    link = nil
    completionHandler()
  }

  // A route list from the configuration ([{base, prefix}]); nil when absent.
  private static func routes(_ value: Any?) -> [(base: String, prefix: Int)]? {
    guard let all = value as? [[String: Any]] else { return nil }
    return all.compactMap { n in
      guard let base = n["base"] as? String, let prefix = n["prefix"] as? Int else { return nil }
      return (base, prefix)
    }
  }

  // A prefix of 0 is the default route (the system's own, not a /0 of ours).
  private static func route4(_ n: (base: String, prefix: Int)) -> NEIPv4Route {
    n.prefix == 0 ? NEIPv4Route.default() : NEIPv4Route(destinationAddress: n.base, subnetMask: mask(n.prefix))
  }

  private static func route6(_ n: (base: String, prefix: Int)) -> NEIPv6Route {
    n.prefix == 0
      ? NEIPv6Route.default() : NEIPv6Route(destinationAddress: n.base, networkPrefixLength: NSNumber(value: n.prefix))
  }

  private static func mask(_ prefix: Int) -> String {
    let bits: UInt32 = prefix <= 0 ? 0 : UInt32.max << (32 - UInt32(min(prefix, 32)))
    return [24, 16, 8, 0].map { String((bits >> UInt32($0)) & 255) }.joined(separator: ".")
  }

  // Interface -> add-on: each packet framed.
  private func readFromInterface(_ c: NWConnection) {
    packetFlow.readPackets { [weak self] packets, _ in
      var out = Data()
      for p in packets where p.count <= 0xffff {
        out.append(UInt8(p.count >> 8))
        out.append(UInt8(p.count & 0xff))
        out.append(p)
      }
      c.send(content: out, completion: .contentProcessed { error in
        if error == nil { self?.readFromInterface(c) }
      })
    }
  }

  // Add-on -> interface: whole frames as they come.
  private func readFromApp(_ c: NWConnection) {
    c.receive(minimumIncompleteLength: 1, maximumLength: 65536) { [weak self] data, _, done, error in
      guard let self else { return }
      if let data { self.buffer.append(data) }
      var packets: [Data] = []
      var protocols: [NSNumber] = []
      while self.buffer.count >= 2 {
        let n = Int(self.buffer[self.buffer.startIndex]) << 8 | Int(self.buffer[self.buffer.startIndex + 1])
        guard self.buffer.count >= 2 + n else { break }
        let p = self.buffer.subdata(in: self.buffer.startIndex + 2 ..< self.buffer.startIndex + 2 + n)
        self.buffer.removeFirst(2 + n)
        packets.append(p)
        protocols.append(NSNumber(value: (p.first ?? 0) >> 4 == 6 ? AF_INET6 : AF_INET))
      }
      if !packets.isEmpty { self.packetFlow.writePackets(packets, withProtocols: protocols) }
      if done || error != nil {
        self.cancelTunnelWithError(error)
        return
      }
      self.readFromApp(c)
    }
  }
}
