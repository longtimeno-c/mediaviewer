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

  public init(status: VPNStatus, host: String? = nil, isDemo: Bool = false, consented: Bool = false) {
    self.status = status
    self.host = host
    self.isDemo = isDemo
    self.consented = consented
  }

  /// The switch's position: on while connecting, connected or reconnecting.
  public var isOn: Bool {
    switch status {
    case .connecting, .connected, .reconnecting: return true
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
  /// Stops the tunnel and removes the code, the key and iOS's VPN configuration.
  func remove() async
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
  public func remove() async {}
}

/// The sample library's VPN: the real screens, the real order (consent before the switch), and a
/// tunnel that is never made. No VPN configuration is added and no traffic is routed; the screen
/// says so for as long as it is on. Forgotten with the sample.
public final class DemoVPN: VPNService, @unchecked Sendable {
  /// What the demo calls its server.
  public static let host = "Sample server (demo)"

  private let lock = NSLock()
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
      lock.withLock {
        listeners[id] = continuation
        continuation.yield(snapshot)
      }
      continuation.onTermination = { [weak self] _ in self?.lock.withLock { _ = self?.listeners.removeValue(forKey: id) } }
    }
  }

  /// The demo uses no host or key, but text that isn't a VPN code is refused as the real one
  /// refuses it, so the demo never suggests that anything works. Only the code's scheme is checked:
  /// the one parser is the core's (serve::parse_relay_connect_code). The demo host stays.
  public func connect(code: String) async throws {
    guard Self.looksLikeACode(code) else { throw VPNCodeError.notACode }
    publish { $0.status = .off }
  }

  static func looksLikeACode(_ text: String) -> Bool {
    let t = text.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    guard t.hasPrefix("mediaviewer://relay"), let hash = t.firstIndex(of: "#") else { return false }
    return t.index(after: hash) < t.endIndex
  }

  public func acceptConsent() async { publish { $0.consented = true } }

  public func setOn(_ on: Bool) async {
    let mine: Int? = lock.withLock {
      if on, !snapshot.consented || snapshot.isOn { return nil }
      if !on, !snapshot.isOn { return nil }
      generation += 1
      snapshot.status = on ? .connecting : .off
      for continuation in listeners.values { continuation.yield(snapshot) }
      return on ? generation : nil
    }
    guard let mine else { return }
    if connectingFor > .zero { try? await Task.sleep(for: connectingFor) }
    let since = now()
    lock.withLock {
      guard generation == mine, snapshot.status == .connecting else { return }
      snapshot.status = .connected(since: since)
      for continuation in listeners.values { continuation.yield(snapshot) }
    }
  }

  /// Back to how the sample opened.
  public func remove() async {
    lock.withLock { generation += 1 }
    publish { $0 = VPNSnapshot(status: .off, host: DemoVPN.host, isDemo: true) }
  }

  private func publish(_ change: (inout VPNSnapshot) -> Void) {
    lock.withLock {
      change(&snapshot)
      for continuation in listeners.values { continuation.yield(snapshot) }
    }
  }
}
