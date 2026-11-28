// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN through your server (docs/plans/phone-vpn.md): every app's traffic out through the
// person's own server, the way the desktop's All traffic (VPN) does. Built in but opt-in (owner,
// 2026-11-19): nothing is set up, saved or started until the person pastes the VPN code from their
// server and switches it on, and Remove takes it all away again. The screens see only this seam.
// `NoVPN` is a build without the tunnel extension; `DemoVPN` is the sample library's labelled demo,
// which routes nothing (plan §8, owner call 5).
import Foundation

/// Where the tunnel is.
public enum VPNStatus: Hashable, Sendable {
  /// This build has no tunnel extension: nothing can be set up yet.
  case notInThisBuild
  /// No VPN code pasted yet.
  case notSetUp
  /// Set up, switched off.
  case off
  case connecting
  case connected(since: Date)
  /// The network changed or the server dropped; trying again on its own.
  case reconnecting
  /// On, with Connect automatically: iOS leaves the tunnel down here (a home Wi-Fi, or no network)
  /// and brings it up by itself elsewhere. The switch stays on; switching off stops that too.
  case paused
  case failed(VPNProblem)
}

/// Why it could not connect, in words the screen shows.
public enum VPNProblem: Hashable, Sendable {
  /// The server did not answer at its address and port.
  case serverUnreachable
  /// The server answered but refused the key (the VPN code is old: Use as VPN was turned off and on).
  case keyRejected
  /// The person declined iOS's "Add VPN Configurations" alert.
  case notAllowed
  /// Anything else iOS reported, already in words.
  case other(String)
}

/// The VPN code (`mediaviewer://relay#host:port#KEY`) was not one.
public enum VPNCodeError: Error, Hashable, Sendable {
  case notACode
}

public struct VPNSnapshot: Hashable, Sendable {
  public var status: VPNStatus
  /// "host:port" of the VPN host, once a code is pasted. Never the key.
  public var host: String?
  /// The sample library's demo: nothing is routed.
  public var isDemo: Bool
  /// The person has seen and accepted the consent screen (App Review 5.4) on this phone.
  public var consented: Bool
  /// Opt-in, off by default (owner call 4): while the VPN is on, iOS connects it by itself on any
  /// network and leaves it off on the home Wi-Fi networks named here.
  public var autoConnect: Bool
  /// The home Wi-Fi networks (SSIDs) the VPN stays off on, in the order they were added.
  public var homeNetworks: [String]

  public init(status: VPNStatus, host: String? = nil, isDemo: Bool = false, consented: Bool = false,
              autoConnect: Bool = false, homeNetworks: [String] = []) {
    self.status = status
    self.host = host
    self.isDemo = isDemo
    self.consented = consented
    self.autoConnect = autoConnect
    self.homeNetworks = homeNetworks
  }

  /// The switch's position: on while connecting, connected or reconnecting.
  public var isOn: Bool {
    switch status {
    case .connecting, .connected, .reconnecting, .paused: return true
    default: return false
    }
  }

  /// A code is in place, so the switch can be shown.
  public var isSetUp: Bool {
    switch status {
    case .notInThisBuild, .notSetUp: return false
    default: return true
    }
  }
}

public protocol VPNService: Sendable {
  /// What is known now, without waiting.
  var current: VPNSnapshot { get }
  /// The current state, then every change.
  func updates() -> AsyncStream<VPNSnapshot>
  /// Reads and keeps the VPN code (the key in the Keychain, never in settings). Throws `VPNCodeError`.
  /// Replaces any earlier code and leaves the VPN off.
  func connect(code: String) async throws
  /// Records that the consent screen was accepted. The screen calls this before the first `setOn(true)`.
  func acceptConsent() async
  /// On: asks iOS to add the configuration the first time (its own alert), then starts the tunnel.
  /// Idempotent: on while already on (or connecting) and off while off change nothing, and when
  /// calls overlap the last one wins; an earlier call never finishes over a later one.
  func setOn(_ on: Bool) async
  /// Connect automatically (except on home Wi-Fi), and the home networks; kept until Remove.
  /// `homeNetworks` is cleaned as `VPNHomeNetworks.cleaned` does. Only once set up.
  func setAutoConnect(_ on: Bool, homeNetworks: [String]) async
  /// Stops the tunnel and removes the code, the key and iOS's VPN configuration.
  func remove() async
}

/// Home Wi-Fi names as iOS matches them (an SSID is 1 to 32 bytes).
public enum VPNHomeNetworks {
  /// Trimmed, without empty, too-long or repeated names, in the order given.
  public static func cleaned(_ names: [String]) -> [String] {
    var seen = Set<String>()
    var out: [String] = []
    for name in names {
      let n = name.trimmingCharacters(in: .whitespacesAndNewlines)
      guard !n.isEmpty, n.utf8.count <= 32, seen.insert(n).inserted else { continue }
      out.append(n)
    }
    return out
  }
}

/// A build with no tunnel extension: says so, does nothing.
public struct NoVPN: VPNService {
  public init() {}
  public var current: VPNSnapshot { VPNSnapshot(status: .notInThisBuild) }
  public func updates() -> AsyncStream<VPNSnapshot> {
    AsyncStream { $0.yield(VPNSnapshot(status: .notInThisBuild)); $0.finish() }
  }
  public func connect(code: String) async throws { throw VPNCodeError.notACode }
  public func acceptConsent() async {}
  public func setOn(_ on: Bool) async {}
  public func setAutoConnect(_ on: Bool, homeNetworks: [String]) async {}
  public func remove() async {}
}

/// The sample library's VPN: the real screens, the real order (consent before the switch), and a
/// tunnel that is never made. No VPN configuration is added and no traffic is routed; the screen
/// says so for as long as it is on. Forgotten with the sample.
public final class DemoVPN: VPNService, @unchecked Sendable {
  /// What the demo calls its server.
  public static let host = "Sample server (demo)"

  /// Guards the state. Never held while a follower is told: a follower's cancel runs onTermination
  /// (which takes `lock`) while holding its task's status lock, and a yield needs that lock too.
  private let lock = NSLock()
  /// Held while followers are told, so they see the changes in the order they were made.
  private let delivery = NSLock()
  private var snapshot = VPNSnapshot(status: .off, host: DemoVPN.host, isDemo: true)
  private var listeners: [UUID: AsyncStream<VPNSnapshot>.Continuation] = [:]
  /// Counts switch moves and removes, so a connect that slept through a later one gives up.
  private var generation = 0
  private let connectingFor: Duration
  private let now: @Sendable () -> Date

  /// `connectingFor`: how long "Connecting…" shows before "Connected" (0 in tests).
  public init(connectingFor: Duration = .milliseconds(800), now: @escaping @Sendable () -> Date = { Date() }) {
    self.connectingFor = connectingFor
    self.now = now
  }

  public var current: VPNSnapshot { lock.withLock { snapshot } }

  public func updates() -> AsyncStream<VPNSnapshot> {
    AsyncStream { continuation in
      let id = UUID()
      delivery.withLock {
        let first = lock.withLock {
          listeners[id] = continuation
          return snapshot
        }
        continuation.yield(first)
      }
      continuation.onTermination = { [weak self] _ in self?.lock.withLock { _ = self?.listeners.removeValue(forKey: id) } }
    }
  }

  /// The demo uses no host or key, but text that isn't a VPN code is refused, so the demo never
  /// suggests that anything works. The code's shape is checked by the rules of the core's
  /// serve::parse_relay_connect_code (`[mediaviewer://relay#|relay-tls#]host:port#KEY`), which stays
  /// the one that reads codes. The demo host stays.
  public func connect(code: String) async throws {
    guard Self.looksLikeACode(code) else { throw VPNCodeError.notACode }
    publish { s, _ in s.status = .off; return true }
  }

  static func looksLikeACode(_ text: String) -> Bool {
    var t = Substring(text.trimmingCharacters(in: .whitespacesAndNewlines))
    for prefix in ["mediaviewer://relay#", "mediaviewer://relay-tls#"] where t.lowercased().hasPrefix(prefix) {
      t = t.dropFirst(prefix.count)
      break
    }
    let parts = t.split(separator: "#", omittingEmptySubsequences: false)
    guard parts.count == 2 else { return false }
    let address = parts[0], key = parts[1]
    let visible = { (c: Character) in c.unicodeScalars.allSatisfy { $0.value > 0x20 && $0.value != 0x7f } }
    guard (16...4096).contains(key.utf8.count), key.allSatisfy(visible),
          !address.isEmpty, address.utf8.count <= 255, address.allSatisfy(visible),
          let colon = address.lastIndex(of: ":"), colon > address.startIndex else { return false }
    let host = address[..<colon], port = address[address.index(after: colon)...]
    guard (1...5).contains(port.count), port.allSatisfy({ ("0"..."9").contains($0) }),
          let number = Int(port), (1...65535).contains(number) else { return false }
    // An IPv6 address needs its brackets.
    if host.first == "[" { return host.count >= 3 && host.last == "]" }
    return !host.contains(":")
  }

  public func acceptConsent() async { publish { s, _ in s.consented = true; return true } }

  /// Kept and shown; the demo asks iOS for nothing.
  public func setAutoConnect(_ on: Bool, homeNetworks: [String]) async {
    let home = VPNHomeNetworks.cleaned(homeNetworks)
    publish { s, _ in
      s.autoConnect = on
      s.homeNetworks = home
      return true
    }
  }

  public func setOn(_ on: Bool) async {
    var mine: Int?
    publish { s, generation in
      if on, !s.consented || s.isOn { return false }
      if !on, !s.isOn { return false }
      generation += 1
      s.status = on ? .connecting : .off
      if on { mine = generation }
      return true
    }
    guard let mine else { return }
    if connectingFor > .zero { try? await Task.sleep(for: connectingFor) }
    let since = now()
    publish { s, generation in
      guard generation == mine, s.status == .connecting else { return false }
      s.status = .connected(since: since)
      return true
    }
  }

  /// Back to how the sample opened.
  public func remove() async {
    publish { s, generation in
      generation += 1
      s = VPNSnapshot(status: .off, host: DemoVPN.host, isDemo: true)
      return true
    }
  }

  /// `change` edits the snapshot and the generation under `lock` and says whether it changed
  /// anything; the followers are told after `lock` is let go.
  private func publish(_ change: (inout VPNSnapshot, inout Int) -> Bool) {
    delivery.withLock {
      let told: (VPNSnapshot, [AsyncStream<VPNSnapshot>.Continuation])? = lock.withLock {
        guard change(&snapshot, &generation) else { return nil }
        return (snapshot, Array(listeners.values))
      }
      guard let (value, list) = told else { return }
      list.forEach { $0.yield(value) }
    }
  }
}
