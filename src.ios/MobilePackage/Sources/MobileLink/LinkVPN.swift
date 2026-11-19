// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN through your server, for real (docs/plans/phone-vpn.md S2): the packet tunnel extension
// (App/DriftwireTunnel) runs the whole VPN; this side keeps its one secret and its configuration.
// Built in but opt-in (owner, 2026-11-19): nothing goes to the Keychain or iOS's VPN settings until
// the person pastes the server's VPN code (connect) and switches it on (setOn); remove() takes all
// of it away again. The code is read by the laptop's own parser, behind the C ABI (owner call 1).
import CMobileVPN
import Foundation
import MobileKit
import MobileLinkSupport
import NetworkExtension
import Security

public final class LinkVPN: VPNService, @unchecked Sendable {
  private let lock = NSLock()
  private var snapshot: VPNSnapshot                                    // under lock
  private var listeners: [UUID: AsyncStream<VPNSnapshot>.Continuation] = [:]  // under lock
  private var manager: NETunnelProviderManager?                        // under lock
  /// The person switched it on: a disconnect is then a failure to say, not "Off".
  private var wantsOn = false                                          // under lock
  private let defaults: UserDefaults
  private let item: VPNItemStore
  private var observer: NSObjectProtocol?

  /// `bundle`: the app's, whose Info.plist names its team prefix (the shared Keychain group).
  public init(bundle: Bundle = .main) {
    defaults = UserDefaults(suiteName: VPNNames.appGroup) ?? .standard
    item = VPNItemStore(group: VPNNames.keychainGroup(prefix: bundle.object(forInfoDictionaryKey: VPNNames.prefixInfoKey) as? String))
    let state = VPNSharedState.load(defaults)
    let setUp = item.load() != nil
    snapshot = VPNSnapshot(status: setUp ? .off : .notSetUp, host: setUp ? state.host : nil, consented: state.consented)
    observer = NotificationCenter.default.addObserver(forName: .NEVPNStatusDidChange, object: nil, queue: nil) {
      [weak self] note in
      guard let self, let connection = note.object as? NEVPNConnection,
            connection === self.lock.withLock({ self.manager?.connection }) else { return }
      self.statusChanged(connection)
    }
    // A configuration saved by an earlier run: its state now.
    if setUp { Task { [weak self] in await self?.loadManager() } }
  }

  deinit {
    if let observer { NotificationCenter.default.removeObserver(observer) }
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

  public func connect(code: String) async throws {
    var relayOut: UnsafeMutablePointer<CChar>?
    var keyOut: UnsafeMutablePointer<CChar>?
    guard mv_vpn_parse_code(code, &relayOut, &keyOut) == MV_VPN_OK, let relayOut, let keyOut else {
      mv_vpn_free(relayOut)
      mv_vpn_free(keyOut)
      throw VPNCodeError.notACode
    }
    let relay = String(cString: relayOut)
    let key = String(cString: keyOut)
    mv_vpn_free(relayOut)
    mv_vpn_free(keyOut)
    // The knock key for the HTTPS look: Argon2id at 64 MiB, here and never in the extension (§2.1).
    let knock = await Task.detached(priority: .userInitiated) { () -> Data? in
      var out = [UInt8](repeating: 0, count: 32)
      return mv_vpn_derive_knock_key(key, &out) == MV_VPN_OK ? Data(out) : nil
    }.value
    // An earlier code's tunnel stops first: the new code replaces it and leaves the VPN off.
    let earlier = lock.withLock { () -> NETunnelProviderManager? in
      wantsOn = false
      return manager
    }
    earlier?.connection.stopVPNTunnel()
    guard let knock, item.save(VPNTunnelItem(relay: relay, key: key, knock: knock)) else {
      publish { $0.status = .failed(.other("This phone couldn't keep the VPN code. Try again.")) }
      return
    }
    var state = VPNSharedState.load(defaults)
    state.host = relay
    state.save(defaults)
    if let earlier, let proto = earlier.protocolConfiguration as? NETunnelProviderProtocol, proto.serverAddress != relay {
      proto.serverAddress = relay
      try? await earlier.saveToPreferences()
    }
    publish {
      $0.status = .off
      $0.host = relay
    }
  }

  public func acceptConsent() async {
    var state = VPNSharedState.load(defaults)
    state.consented = true
    state.save(defaults)
    publish { $0.consented = true }
  }

  public func setOn(_ on: Bool) async {
    guard on else {
      let m = lock.withLock { () -> NETunnelProviderManager? in
        wantsOn = false
        return manager
      }
      m?.connection.stopVPNTunnel()
      publish { $0.status = .off }
      return
    }
    let now = current
    guard now.isSetUp, now.consented, let host = now.host else { return }
    lock.withLock { wantsOn = true }
    publish { $0.status = .connecting }
    let m: NETunnelProviderManager
    do {
      m = try await savedManager(host: host)
    } catch {
      lock.withLock { wantsOn = false }
      publish { $0.status = .failed(.notAllowed) }  // the person declined iOS's alert
      return
    }
    do {
      try m.connection.startVPNTunnel()
    } catch {
      lock.withLock { wantsOn = false }
      publish { $0.status = .failed(.other("iOS couldn't start the VPN.")) }
    }
  }

  public func remove() async {
    let m = lock.withLock { () -> NETunnelProviderManager? in
      wantsOn = false
      return manager
    }
    m?.connection.stopVPNTunnel()
    // Ours, whether or not this run loaded it.
    let all = (try? await NETunnelProviderManager.loadAllFromPreferences()) ?? []
    for saved in all where Self.isOurs(saved) { try? await saved.removeFromPreferences() }
    item.remove()
    defaults.removePersistentDomain(forName: VPNNames.appGroup)
    defaults.removeObject(forKey: VPNSharedState.defaultsKey)  // a standard-defaults fallback, too
    lock.withLock { manager = nil }
    publish { $0 = VPNSnapshot(status: .notSetUp) }
  }

  // MARK: - iOS's VPN configuration

  private static func isOurs(_ m: NETunnelProviderManager) -> Bool {
    (m.protocolConfiguration as? NETunnelProviderProtocol)?.providerBundleIdentifier == VPNNames.extensionBundleID
  }

  private func loadManager() async {
    guard let all = try? await NETunnelProviderManager.loadAllFromPreferences(),
          let m = all.first(where: Self.isOurs) else { return }
    lock.withLock { manager = m }
    statusChanged(m.connection)
  }

  /// Ours, saved with this host and enabled: the first save shows iOS's "Add VPN Configurations".
  private func savedManager(host: String) async throws -> NETunnelProviderManager {
    let all = try await NETunnelProviderManager.loadAllFromPreferences()
    let m = all.first(where: Self.isOurs) ?? NETunnelProviderManager()
    let proto = (m.protocolConfiguration as? NETunnelProviderProtocol) ?? NETunnelProviderProtocol()
    proto.providerBundleIdentifier = VPNNames.extensionBundleID
    proto.serverAddress = host
    proto.providerConfiguration = [:]  // nothing secret: the key is in the shared Keychain item
    m.protocolConfiguration = proto
    m.localizedDescription = VPNNames.localizedDescription
    m.isEnabled = true
    try await m.saveToPreferences()
    try await m.loadFromPreferences()  // as iOS requires before the first start
    lock.withLock { manager = m }
    return m
  }

  private func statusChanged(_ connection: NEVPNConnection) {
    switch connection.status {
    case .connecting: publish { $0.status = .connecting }
    case .connected:
      let since = connection.connectedDate ?? Date()
      publish { $0.status = .connected(since: since) }
    case .reasserting: publish { $0.status = .reconnecting }
    case .disconnecting: break
    case .disconnected, .invalid:
      guard lock.withLock({ wantsOn }) else {
        publish { if $0.isSetUp { $0.status = .off } }
        return
      }
      // It stopped while wanted on: why, in the extension's words (VPNServerProblem).
      lock.withLock { wantsOn = false }
      connection.fetchLastDisconnectError { [weak self] error in
        self?.publish { $0.status = Self.status(after: error) }
      }
    @unknown default: break
    }
  }

  static func status(after error: Error?) -> VPNStatus {
    switch VPNServerProblem(error: error) {
    case .keyRejected: return .failed(.keyRejected)
    case .unreachable: return .failed(.serverUnreachable)
    case .notForwarding: return .failed(.other("Your server isn't sharing its VPN. Turn on Use as VPN there."))
    case nil: return error == nil ? .off : .failed(.serverUnreachable)
    }
  }

  private func publish(_ change: (inout VPNSnapshot) -> Void) {
    lock.withLock {
      change(&snapshot)
      for continuation in listeners.values { continuation.yield(snapshot) }
    }
  }
}
