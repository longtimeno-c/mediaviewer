// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The VPN through your server, for real (docs/plans/phone-vpn.md S2): the packet tunnel extension
// (App/DriftwireTunnel) runs the whole VPN; this side keeps its one secret and its configuration.
// Built in but opt-in (owner, 2026-11-19): nothing goes to the Keychain, the App Group or iOS's VPN
// settings until the person has pasted the server's VPN code (connect, kept in memory until then)
// AND switched it on (setOn); remove() takes all of it away again. The code is read by the laptop's
// own parser, behind the C ABI (owner call 1).
import CMobileVPN
import Foundation
import MobileKit
import MobileLinkSupport
import NetworkExtension
import Security

public final class LinkVPN: VPNService, @unchecked Sendable {
  /// Never held while a follower is told: a follower's cancel runs onTermination (which takes it)
  /// while holding its task's status lock, and a yield needs that lock too.
  private let lock = NSLock()
  /// Held while followers are told, so they see the changes in the order they were made.
  private let delivery = NSLock()
  private var snapshot: VPNSnapshot                                    // under lock
  private var listeners: [UUID: AsyncStream<VPNSnapshot>.Continuation] = [:]  // under lock
  private var manager: NETunnelProviderManager?                        // under lock
  /// This switch-on, as the status machine follows it.
  private var attempt = VPNStatusMachine.Attempt()                     // under lock
  /// A pasted code not yet switched on: kept here, written nowhere (opt-in).
  private var pending: VPNTunnelItem?                                  // under lock
  /// Consent given in this run, before the first switch-on writes it down.
  private var consentedNow = false                                     // under lock
  /// Bumped by remove(): a load that started before it must not bring the configuration back.
  private var generation = 0                                           // under lock
  private let defaults: UserDefaults
  private let item: VPNItemStore
  private var observer: NSObjectProtocol?

  /// `bundle`: the app's, whose Info.plist names its team prefix (the shared Keychain group).
  public init(bundle: Bundle = .main) {
    defaults = UserDefaults(suiteName: VPNNames.appGroup) ?? .standard
    item = VPNItemStore(group: VPNNames.keychainGroup(prefix: bundle.object(forInfoDictionaryKey: VPNNames.prefixInfoKey) as? String))
    // Not set up until the Keychain answers, off the main thread (attributes only, no secret).
    snapshot = VPNSnapshot(status: .notSetUp)
    observer = NotificationCenter.default.addObserver(forName: .NEVPNStatusDidChange, object: nil, queue: nil) {
      [weak self] note in
      guard let self, let connection = note.object as? NEVPNConnection,
            connection === self.lock.withLock({ self.manager?.connection }) else { return }
      self.statusChanged(connection)
    }
    let generation = 0
    Task.detached(priority: .utility) { [weak self, item, defaults] in
      guard item.exists() else { return }
      let state = VPNSharedState.load(defaults)
      guard let self, self.lock.withLock({ self.generation == generation && self.pending == nil }) else { return }
      self.publish {
        if !$0.isSetUp {
          $0.status = .off
          $0.host = state.host
          $0.consented = state.consented
        }
      }
      // A configuration saved by an earlier run: its state now.
      await self.loadManager(generation: generation)
    }
  }

  deinit {
    if let observer { NotificationCenter.default.removeObserver(observer) }
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
    guard let knock else {
      publish { $0.status = .failed(.other("This phone couldn't read the VPN code. Try again.")) }
      return
    }
    // An earlier code's tunnel stops: the new code replaces it and leaves the VPN off. Kept in
    // memory only, until the switch is turned on.
    let earlier = lock.withLock { () -> NETunnelProviderManager? in
      attempt = VPNStatusMachine.Attempt()
      pending = VPNTunnelItem(relay: relay, key: key, knock: knock)
      return manager
    }
    earlier?.connection.stopVPNTunnel()
    publish {
      $0.status = .off
      $0.host = relay
    }
  }

  public func acceptConsent() async {
    lock.withLock { consentedNow = true }
    publish { $0.consented = true }
  }

  public func setOn(_ on: Bool) async {
    guard on else {
      let m = lock.withLock { () -> NETunnelProviderManager? in
        attempt = VPNStatusMachine.Attempt()
        return manager
      }
      m?.connection.stopVPNTunnel()
      publish { $0.status = .off }
      return
    }
    let now = current
    guard now.isSetUp, now.consented, let host = now.host else { return }
    let (toWrite, consent) = lock.withLock { () -> (VPNTunnelItem?, Bool) in
      attempt = VPNStatusMachine.Attempt(wantsOn: true)
      return (pending, consentedNow)
    }
    publish { $0.status = .connecting }
    // Switched on: only now is anything written (the item, the host and consent).
    if let toWrite {
      guard item.save(toWrite) else {
        lock.withLock { attempt = VPNStatusMachine.Attempt() }
        publish { $0.status = .failed(.other("This phone couldn't keep the VPN code. Try again.")) }
        return
      }
      lock.withLock { pending = nil }
    }
    var state = VPNSharedState.load(defaults)
    state.host = host
    state.consented = state.consented || consent
    state.save(defaults)
    let m: NETunnelProviderManager
    do {
      m = try await savedManager(host: host)
    } catch {
      lock.withLock { attempt = VPNStatusMachine.Attempt() }
      publish { $0.status = .failed(.notAllowed) }  // the person declined iOS's alert
      return
    }
    do {
      try m.connection.startVPNTunnel()
    } catch {
      lock.withLock { attempt = VPNStatusMachine.Attempt() }
      publish { $0.status = .failed(.other("iOS couldn't start the VPN.")) }
    }
  }

  public func remove() async {
    let m = lock.withLock { () -> NETunnelProviderManager? in
      generation += 1
      attempt = VPNStatusMachine.Attempt()
      pending = nil
      consentedNow = false
      defer { manager = nil }
      return manager
    }
    m?.connection.stopVPNTunnel()
    // Ours, whether or not this run loaded it.
    let all = (try? await NETunnelProviderManager.loadAllFromPreferences()) ?? []
    for saved in all where Self.isOurs(saved) { try? await saved.removeFromPreferences() }
    item.remove()
    defaults.removePersistentDomain(forName: VPNNames.appGroup)
    defaults.removeObject(forKey: VPNSharedState.defaultsKey)  // a standard-defaults fallback, too
    publish { $0 = VPNSnapshot(status: .notSetUp) }
  }

  // MARK: - iOS's VPN configuration

  private static func isOurs(_ m: NETunnelProviderManager) -> Bool {
    (m.protocolConfiguration as? NETunnelProviderProtocol)?.providerBundleIdentifier == VPNNames.extensionBundleID
  }

  private func loadManager(generation: Int) async {
    guard let all = try? await NETunnelProviderManager.loadAllFromPreferences(),
          let m = all.first(where: Self.isOurs) else { return }
    // remove() ran meanwhile: what was loaded is gone, and stays gone.
    guard lock.withLock({ () -> Bool in
      guard self.generation == generation else { return false }
      manager = m
      return true
    }) else { return }
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
    guard let status = VPNStatusMachine.Status(rawValue: connection.status.rawValue) else { return }
    let phase = lock.withLock { VPNStatusMachine.phase(status, &attempt) }
    switch phase {
    case .connecting: publish { $0.status = .connecting }
    case .connected:
      let since = connection.connectedDate ?? Date()
      publish { $0.status = .connected(since: since) }
    case .reconnecting: publish { $0.status = .reconnecting }
    case .off: publish { if $0.isSetUp { $0.status = .off } }
    case .unchanged: break
    case .failed:
      // It stopped while wanted on: why, in the extension's words (VPNServerProblem).
      connection.fetchLastDisconnectError { [weak self] error in
        self?.publish { $0.status = Self.status(after: error) }
      }
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
    delivery.withLock {
      let (value, list) = lock.withLock {
        change(&snapshot)
        return (snapshot, Array(listeners.values))
      }
      list.forEach { $0.yield(value) }
    }
  }
}
