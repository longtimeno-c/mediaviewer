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
  /// What this app last told iOS about on-demand (shown as Paused while the tunnel is down).
  private var armed = false                                            // under lock
  /// A new code, the switch and remove, one at a time; what an overtaken one learns isn't shown.
  private let moves = VPNMoves()
  /// A pasted code not yet switched on: kept here, written nowhere (opt-in).
  private var pending: VPNTunnelItem?                                  // under lock
  /// Consent given in this run, before the first switch-on writes it down.
  private var consentedNow = false                                     // under lock
  /// Bumped by remove(): a load that started before it must not bring the configuration back.
  private var generation = 0                                           // under lock
  private let defaults: UserDefaults
  private let item: VPNItemStore
  private var observer: NSObjectProtocol?
  private var configurationObserver: NSObjectProtocol?

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
    // iOS may change on-demand itself (Settings → VPN): what is armed is read again, in turn.
    configurationObserver = NotificationCenter.default.addObserver(forName: .NEVPNConfigurationChange, object: nil, queue: nil) {
      [weak self] _ in
      guard let self else { return }
      Task { await self.moves.inTurn(overtaking: false) { [weak self] _ in await self?.refreshArmed() } }
    }
    let generation = 0
    Task.detached(priority: .utility) { [weak self, item, defaults] in
      guard item.exists() else { return }
      let state = VPNSharedState.load(defaults)
      guard let self else { return }
      // Checked in the same hold of the lock as the change, so a remove meanwhile stays removed.
      self.publish {
        if self.generation == generation, self.pending == nil, !$0.isSetUp {
          $0.status = .off
          $0.host = state.host
          $0.consented = state.consented
          $0.autoConnect = state.autoConnect
          $0.homeNetworks = state.homeNetworks
        }
      }
      // A configuration saved by an earlier run: its state now, in turn with the moves.
      await self.moves.inTurn(overtaking: false) { [weak self] _ in await self?.loadManager(generation: generation) }
    }
  }

  deinit {
    if let observer { NotificationCenter.default.removeObserver(observer) }
    if let configurationObserver { NotificationCenter.default.removeObserver(configurationObserver) }
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

  // MARK: - Moves: a new code, the switch, remove
  //
  // One at a time, in the order they were called, so the last call wins: each begins only once the
  // one before has ended (a remove can't be undone by a switch-on still saving, nor a new code lost).
  // Each move also begins a run when it is called, so a switch-on still running (iOS's alert open,
  // a save under way) sees a later move waiting and writes, starts and shows nothing more. Status
  // news and the extension's reason for a stop belong to the run they arrived in.

  public func connect(code: String) async throws {
    var relayOut: UnsafeMutablePointer<CChar>?
    var keyOut: UnsafeMutablePointer<CChar>?
    var tls: Int32 = 0
    var tlsPort: UInt16 = 0
    guard mv_vpn_parse_code(code, &relayOut, &keyOut, &tls, &tlsPort) == MV_VPN_OK, let relayOut, let keyOut else {
      mv_vpn_free(relayOut)
      mv_vpn_free(keyOut)
      throw VPNCodeError.notACode  // a wrong paste overtakes nothing
    }
    let relay = String(cString: relayOut)
    let key = String(cString: keyOut)
    mv_vpn_free(relayOut)
    mv_vpn_free(keyOut)
    // A relay-tls code (what a host hands out by default): the HTTPS look on the code's port, the
    // extension dialling it as the desktop's Tunnel does (the core's TLS, a browser's ClientHello).
    let parsed = VPNCode(relay: relay, key: key, tls: tls != 0, tlsPort: tlsPort)
    await inTurn { [self] in await connectMove(parsed) }
  }

  public func acceptConsent() async {
    lock.withLock { consentedNow = true }
    publish { $0.consented = true }
  }

  public func setOn(_ on: Bool) async {
    await inTurn { [self] run in await (on ? switchOn(run: run) : switchOff()) }
  }

  public func setAutoConnect(_ on: Bool, homeNetworks: [String]) async {
    let home = VPNHomeNetworks.cleaned(homeNetworks)
    // Waits its turn but overtakes no switch-on: it changes no switch.
    await moves.inTurn(overtaking: false) { [self] _ in await autoConnectMove(on, home) }
  }

  public func remove() async {
    await inTurn { [self] in await removeMove() }
  }

  private func inTurn(_ move: @escaping @Sendable () async -> Void) async { await moves.inTurn { _ in await move() } }
  private func inTurn(_ move: @escaping @Sendable (Int) async -> Void) async { await moves.inTurn(move) }

  private func isLatest(_ run: Int) -> Bool { moves.isLatest(run) }

  private func connectMove(_ code: VPNCode) async {
    let relay = code.relay, key = code.key
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
      pending = VPNTunnelItem(code: code, knock: knock)
      return manager
    }
    await disarmOnDemand()  // or iOS brings the old code's tunnel back
    earlier?.connection.stopVPNTunnel()
    publish {
      $0.status = .off
      $0.host = relay
    }
  }

  private func switchOff() async {
    let m = lock.withLock { () -> NETunnelProviderManager? in
      attempt = VPNStatusMachine.Attempt()
      return manager
    }
    // On-demand off first, or iOS would start the tunnel again by itself.
    await disarmOnDemand()
    m?.connection.stopVPNTunnel()
    publish { if $0.isSetUp { $0.status = .off } }
  }

  private func switchOn(run: Int) async {
    let now = current
    guard now.isSetUp, now.consented, let host = now.host else { return }
    guard let (toWrite, consent) = lock.withLock({ () -> (VPNTunnelItem?, Bool)? in
      guard moves.isLatest(run) else { return nil }
      attempt = VPNStatusMachine.Attempt(wantsOn: true)
      return (pending, consentedNow)
    }) else { return }
    publish(run: run) { $0.status = .connecting }
    // Switched on: only now is anything written (the item, the host and consent).
    if let toWrite {
      guard item.save(toWrite) else {
        await failed(run: run, .other("This phone couldn't keep the VPN code. Try again."))
        return
      }
      lock.withLock { pending = nil }
    }
    // The item and the host and consent are one write: once the item is saved, so are they (a later
    // move corrects the screen, and a remove deletes both).
    var state = VPNSharedState.load(defaults)
    state.host = host
    state.consented = state.consented || consent
    state.autoConnect = now.autoConnect
    state.homeNetworks = now.homeNetworks
    state.save(defaults)
    let m: NETunnelProviderManager
    do {
      m = try await savedManager(host: host, run: run)
    } catch {
      // Overtaken while iOS asked: the move waiting says what is true.
      if error is CancellationError { return }
      await failed(run: run, .notAllowed)  // the person declined iOS's alert
      return
    }
    guard isLatest(run) else { return }
    do {
      try m.connection.startVPNTunnel()
    } catch {
      await failed(run: run, .other("iOS couldn't start the VPN."))
    }
  }

  /// A switch-on that failed: shown, and nothing left armed (its rules may be saved already).
  private func failed(run: Int, _ problem: VPNProblem) async {
    guard moves.isLatest(run) else { return }  // the move waiting says what is true
    lock.withLock { attempt = VPNStatusMachine.Attempt() }
    await disarmOnDemand()
    publish(run: run) { $0.status = .failed(problem) }
  }

  private func removeMove() async {
    // Not set up from here on: a switch move waiting after this one has nothing to turn on.
    let m = lock.withLock { () -> NETunnelProviderManager? in
      generation += 1
      attempt = VPNStatusMachine.Attempt()
      pending = nil
      consentedNow = false
      defer { manager = nil }
      return manager
    }
    publish { $0 = VPNSnapshot(status: .notSetUp) }
    // Disarmed first: should removing the configuration fail, iOS must not keep retrying it.
    await disarmOnDemand()
    m?.connection.stopVPNTunnel()
    // Ours, whether or not this run loaded it.
    let all = (try? await NETunnelProviderManager.loadAllFromPreferences()) ?? []
    for saved in all where Self.isOurs(saved) { try? await saved.removeFromPreferences() }
    item.remove()
    defaults.removePersistentDomain(forName: VPNNames.appGroup)
    defaults.removeObject(forKey: VPNSharedState.defaultsKey)  // a standard-defaults fallback, too
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
      armed = m.isOnDemandEnabled
      return true
    }) else { return }
    statusChanged(m.connection)
  }

  /// Ours, saved with this host and enabled: the first save shows iOS's "Add VPN Configurations".
  private func savedManager(host: String, run: Int) async throws -> NETunnelProviderManager {
    let all = try await NETunnelProviderManager.loadAllFromPreferences()
    let m = all.first(where: Self.isOurs) ?? NETunnelProviderManager()
    let now = current
    let plan = VPNOnDemandPlan(autoConnect: now.autoConnect, homeNetworks: now.homeNetworks, switchedOn: true)
    Self.apply(plan, to: m)
    let proto = (m.protocolConfiguration as? NETunnelProviderProtocol) ?? NETunnelProviderProtocol()
    proto.providerBundleIdentifier = VPNNames.extensionBundleID
    proto.serverAddress = host
    proto.providerConfiguration = [:]  // nothing secret: the key is in the shared Keychain item
    m.protocolConfiguration = proto
    m.localizedDescription = VPNNames.localizedDescription
    m.isEnabled = true
    try await m.saveToPreferences()
    lock.withLock { armed = plan.enabled }
    try await m.loadFromPreferences()  // as iOS requires before the first start
    // Only the latest run's: an overtaken one's would hide the live tunnel's status news.
    guard lock.withLock({ () -> Bool in
      guard moves.isLatest(run) else { return false }
      manager = m
      return true
    }) else { throw CancellationError() }
    return m
  }

  /// Connect automatically: kept in the App Group once the code is (opt-in: not before the first
  /// switch-on), and given to iOS at once while the VPN is on.
  private func autoConnectMove(_ on: Bool, _ home: [String]) async {
    guard current.isSetUp else { return }
    let (m, saved) = lock.withLock { (manager, pending == nil) }
    publish {
      $0.autoConnect = on
      $0.homeNetworks = home
    }
    if saved {
      var state = VPNSharedState.load(defaults)
      state.autoConnect = on
      state.homeNetworks = home
      state.save(defaults)
    }
    // Switched on (paused at home included): iOS takes it now, on the configuration as iOS has it.
    guard m != nil, current.isOn, let fresh = await savedOurs() else { return }
    let plan = VPNOnDemandPlan(autoConnect: on, homeNetworks: home, switchedOn: true)
    Self.apply(plan, to: fresh)
    guard (try? await fresh.saveToPreferences()) != nil else { return }
    lock.withLock { armed = plan.enabled }
    // Turned off while held down at home: nothing will bring it up now.
    if !plan.enabled { publish { if $0.status == .paused { $0.status = .off } } }
  }

  /// What iOS has armed now (it may have changed it itself); Paused only while it is.
  private func refreshArmed() async {
    let now = await savedOurs()?.isOnDemandEnabled ?? false
    lock.withLock { armed = now }
    if !now { publish { if $0.status == .paused { $0.status = .off } } }
  }

  /// Our configuration as iOS has it now (not a copy that may be stale), if there is one.
  private func savedOurs() async -> NETunnelProviderManager? {
    (try? await NETunnelProviderManager.loadAllFromPreferences())?.first(where: Self.isOurs)
  }

  /// On-demand off on our saved configuration, before any stop, or iOS starts the tunnel again by
  /// itself (also after a switch-on that was overtaken once it had saved its rules).
  private func disarmOnDemand() async {
    lock.withLock { armed = false }
    guard let m = await savedOurs(), m.isOnDemandEnabled else { return }
    m.isOnDemandEnabled = false
    try? await m.saveToPreferences()
  }

  /// The plan as iOS's rules: stay off on the home Wi-Fi first, then connect on any network.
  private static func apply(_ plan: VPNOnDemandPlan, to m: NETunnelProviderManager) {
    var rules: [NEOnDemandRule] = []
    if !plan.stayOffOn.isEmpty {
      let home = NEOnDemandRuleDisconnect()
      home.interfaceTypeMatch = .wiFi
      home.ssidMatch = plan.stayOffOn
      rules.append(home)
    }
    let anywhere = NEOnDemandRuleConnect()
    anywhere.interfaceTypeMatch = .any
    rules.append(anywhere)
    m.onDemandRules = rules
    m.isOnDemandEnabled = plan.enabled
  }

  private func statusChanged(_ connection: NEVPNConnection) {
    guard let status = VPNStatusMachine.Status(rawValue: connection.status.rawValue) else { return }
    let (phase, run, armed) = lock.withLock { (VPNStatusMachine.phase(status, &attempt), moves.latest, self.armed) }
    // Down while Connect automatically is on: iOS holds it off here and brings it up elsewhere.
    let down: VPNStatus = VPNStatusMachine.afterDrop(armed: armed, serverProblem: false).shown == .paused ? .paused : .off
    switch phase {
    case .connecting: publish(run: run) { if $0.isSetUp { $0.status = .connecting } }
    case .connected:
      let since = connection.connectedDate ?? Date()
      publish(run: run) { if $0.isSetUp { $0.status = .connected(since: since) } }
    case .reconnecting: publish(run: run) { if $0.isSetUp { $0.status = .reconnecting } }
    case .off: publish(run: run) { if $0.isSetUp { $0.status = down } }
    case .unchanged: break
    case .failed:
      // It stopped while wanted on: why, in the extension's words (VPNServerProblem). The answer
      // comes later; by then a newer switch move may have said something else.
      connection.fetchLastDisconnectError { [weak self] error in
        guard let self else { return }
        let shown = Self.status(after: error)
        let drop = VPNStatusMachine.afterDrop(armed: armed, serverProblem: shown != .off)
        // A broken server: shown, and iOS stops retrying it (in turn, after any move waiting).
        if drop.disarm {
          Task { await self.moves.inTurn(overtaking: false) { [weak self] _ in await self?.disarmOnDemand() } }
        }
        self.publish(run: run) { if $0.isSetUp { $0.status = drop.shown == .failed ? shown : down } }
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

  /// `run`: the switch move this news belongs to; dropped when a later one has begun.
  private func publish(run: Int? = nil, _ change: (inout VPNSnapshot) -> Void) {
    delivery.withLock {
      let told: (VPNSnapshot, [AsyncStream<VPNSnapshot>.Continuation])? = lock.withLock {
        if let run, !moves.isLatest(run) { return nil }
        change(&snapshot)
        return (snapshot, Array(listeners.values))
      }
      guard let (value, list) = told else { return }
      list.forEach { $0.yield(value) }
    }
  }
}
