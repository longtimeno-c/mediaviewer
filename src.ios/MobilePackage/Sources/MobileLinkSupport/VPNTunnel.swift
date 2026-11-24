// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What the app and the VPN extension share (docs/plans/phone-vpn.md §1.2): the names (owner call
// 8, 2026-11-19), the tunnel item in the shared Keychain group, the non-secret state in the App
// Group, the extension's interface settings and the server's problems. No NetworkExtension, so
// it is tested on its own; everything but VPNItemStore (the Keychain) is pure.
import Foundation
import Security

public enum VPNNames {
  /// The extension's bundle ID: the Mac's suffix (#673).
  public static let extensionBundleID = "io.github.longtimeno-c.mediaviewer.mobile.DriftwireTunnel"
  /// The App Group: non-secret state only (§1.2).
  public static let appGroup = "group.io.github.longtimeno-c.mediaviewer.mobile"
  /// The shared Keychain group without its team prefix; `keychainGroup(prefix:)` adds it.
  public static let keychainGroupSuffix = "io.github.longtimeno-c.mediaviewer.mobile.vpn"
  public static let keychainService = "io.github.longtimeno-c.mediaviewer.mobile.vpn"
  public static let keychainAccount = "tunnel"
  /// What iOS shows in Settings → VPN.
  public static let localizedDescription = "MediaViewer — your server"
  /// The Info.plist key both bundles carry, set to $(AppIdentifierPrefix) at build time.
  public static let prefixInfoKey = "MVAppIdentifierPrefix"
  /// The error domain the extension fails a start with; codes are `VPNServerProblem`'s.
  public static let errorDomain = "io.github.longtimeno-c.mediaviewer.vpn"

  /// "TEAMID.io.github.….mobile.vpn" from the bundle's prefix ("TEAMID."); nil without one (an
  /// unsigned build: the default group then, which the extension cannot share).
  public static func keychainGroup(prefix: String?) -> String? {
    guard let prefix, !prefix.isEmpty, !prefix.hasPrefix("$(") else { return nil }
    return (prefix.hasSuffix(".") ? prefix : prefix + ".") + keychainGroupSuffix
  }
}

/// A pasted VPN code as mv_vpn_parse_code read it (the core's parser; none in Swift).
public struct VPNCode: Equatable, Sendable {
  public var relay: String
  public var key: String
  /// A relay-tls code: the server looks like a web site on `tlsPort`, the code's port.
  public var tls: Bool
  public var tlsPort: UInt16
  public init(relay: String, key: String, tls: Bool, tlsPort: UInt16) {
    self.relay = relay
    self.key = key
    self.tls = tls
    self.tlsPort = tlsPort
  }
}

/// The one secret the extension reads (§1.2): never the pairing record, the access key or the
/// device key. JSON in a generic password, AfterFirstUnlockThisDeviceOnly.
public struct VPNTunnelItem: Codable, Equatable, Sendable {
  /// "host:port" of the server.
  public var relay: String
  /// The server's key.
  public var key: String
  /// The 32-byte knock key for the HTTPS look, derived in the app (Argon2id, 64 MiB): the
  /// extension never derives it (§2.1).
  public var knock: Data
  /// The HTTPS look (port `tlsPort`, 0: 443): a relay-tls code's, so the extension dials TLS.
  public var tls: Bool
  public var tlsPort: UInt16

  public init(relay: String, key: String, knock: Data, tls: Bool = false, tlsPort: UInt16 = 0) {
    self.relay = relay
    self.key = key
    self.knock = knock
    self.tls = tls
    self.tlsPort = tlsPort
  }

  /// The item for a pasted code (mv_vpn_parse_code's answer) and its derived knock key.
  public init(code: VPNCode, knock: Data) {
    self.init(relay: code.relay, key: code.key, knock: knock, tls: code.tls, tlsPort: code.tls ? code.tlsPort : 0)
  }

  public func encoded() -> Data? { isUsable ? try? JSONEncoder().encode(self) : nil }

  /// nil for anything not a whole item (a damaged or older one is as good as none).
  public static func decode(_ data: Data) -> VPNTunnelItem? {
    guard let item = try? JSONDecoder().decode(VPNTunnelItem.self, from: data), item.isUsable else { return nil }
    return item
  }

  var isUsable: Bool { !relay.isEmpty && !key.isEmpty && knock.count == 32 }
}

/// Why the server could not be used, as the extension's C ABI says it (mv_mobile_vpn.h).
public enum VPNServerProblem: Int, Sendable, Equatable {
  case keyRejected = 2     // MV_VPN_KEY_REJECTED
  case unreachable = 3     // MV_VPN_UNREACHABLE
  case notForwarding = 4   // MV_VPN_NOT_FORWARDING

  /// The problem a failed start carried (the extension's error, as iOS hands it back); nil for
  /// one that is not the server's.
  public init?(error: Error?) {
    guard let e = error as NSError?, e.domain == VPNNames.errorDomain else { return nil }
    self.init(rawValue: e.code)
  }

  public var error: NSError { NSError(domain: VPNNames.errorDomain, code: rawValue) }
}

/// The non-secret state in the App Group (§1.2), as one value: removed with the VPN.
public struct VPNSharedState: Codable, Equatable, Sendable {
  public static let defaultsKey = "vpn"
  /// "host:port" shown on the screen; never the key.
  public var host: String?
  /// The consent screen (App Review 5.4) was accepted on this phone.
  public var consented: Bool

  public init(host: String? = nil, consented: Bool = false) {
    self.host = host
    self.consented = consented
  }

  public static func load(_ defaults: UserDefaults) -> VPNSharedState {
    guard let data = defaults.data(forKey: defaultsKey),
          let state = try? JSONDecoder().decode(VPNSharedState.self, from: data) else { return VPNSharedState() }
    return state
  }

  public func save(_ defaults: UserDefaults) {
    if let data = try? JSONEncoder().encode(self) { defaults.set(data, forKey: Self.defaultsKey) }
  }
}

/// The interface's settings from the C ABI's mv_vpn_settings_json: every address of both
/// families through the tunnel, the server and the local networks left out (§1.3).
public struct VPNInterfaceSettings: Decodable, Equatable, Sendable {
  public struct Route: Decodable, Equatable, Sendable {
    public var base: String
    public var prefix: Int
    public init(base: String, prefix: Int) {
      self.base = base
      self.prefix = prefix
    }
    /// The default route (the system's own, not a /0 of ours).
    public var isDefault: Bool { prefix == 0 }
  }
  /// The server's first resolved address: NEPacketTunnelNetworkSettings' tunnelRemoteAddress.
  public var remote: String?
  public var address4: String
  public var prefix4: Int
  public var dns4: String
  public var address6: String
  public var prefix6: Int
  public var mtu: Int
  public var include4: [Route]
  public var exclude4: [Route]
  public var include6: [Route]
  public var exclude6: [Route]

  public static func parse(_ json: String) -> VPNInterfaceSettings? {
    try? JSONDecoder().decode(VPNInterfaceSettings.self, from: Data(json.utf8))
  }

  /// An IPv4 prefix length as NEIPv4Settings wants it: "255.255.255.0".
  public static func mask(_ prefix: Int) -> String {
    let bits: UInt32 = prefix <= 0 ? 0 : UInt32.max << (32 - UInt32(min(prefix, 32)))
    return [24, 16, 8, 0].map { String((bits >> UInt32($0)) & 255) }.joined(separator: ".")
  }
}

/// The tunnel item in the shared Keychain group (§1.2): the one secret the extension reads. Never
/// in the app's default group with the pairing record, so the extension cannot read that.
public struct VPNItemStore: Sendable {
  public let group: String?

  /// `group`: VPNNames.keychainGroup(prefix:) of the bundle's team prefix.
  public init(group: String?) { self.group = group }

  private var query: [String: Any] {
    var q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                            kSecAttrService as String: VPNNames.keychainService,
                            kSecAttrAccount as String: VPNNames.keychainAccount]
    if let group { q[kSecAttrAccessGroup as String] = group }
    return q
  }

  public func save(_ item: VPNTunnelItem) -> Bool {
    guard let data = item.encoded() else { return false }
    SecItemDelete(query as CFDictionary)
    var add = query
    add[kSecValueData as String] = data
    add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
    return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
  }

  public func load() -> VPNTunnelItem? {
    var q = query
    q[kSecReturnData as String] = true
    q[kSecMatchLimit as String] = kSecMatchLimitOne
    var out: AnyObject?
    guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
    return VPNTunnelItem.decode(data)
  }

  /// Whether an item is kept, from its attributes alone: the secret is not read.
  public func exists() -> Bool {
    var q = query
    q[kSecReturnAttributes as String] = true
    q[kSecMatchLimit as String] = kSecMatchLimitOne
    var out: AnyObject?
    return SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess
  }

  public func remove() { SecItemDelete(query as CFDictionary) }
}

/// The VPN's moves (a new code, the switch, remove), one at a time in the order they were called, so
/// the last call wins (LinkVPN). Each move begins a run when it is called: a move still running can
/// ask whether a later one is waiting (`isLatest`) and then write, start and show nothing more.
/// What it learns afterwards (iOS's alert answered, the extension's reason for a stop) belongs to
/// its run too. Takes only its own lock, so it may be asked from under the caller's.
public final class VPNMoves: @unchecked Sendable {
  private let lock = NSLock()
  private var latestRun = 0
  private var last: Task<Void, Never>?

  public init() {}

  public var latest: Int { lock.withLock { latestRun } }
  public func isLatest(_ run: Int) -> Bool { lock.withLock { run == latestRun } }

  /// Begins a run now; `move` runs once every earlier move has ended.
  public func inTurn(_ move: @escaping @Sendable (Int) async -> Void) async {
    let turn = lock.withLock { () -> Task<Void, Never> in
      latestRun += 1
      let run = latestRun
      let before = last
      let turn = Task { await before?.value; await move(run) }
      last = turn
      return turn
    }
    await turn.value
  }
}

/// What iOS's VPN status means for the screen (LinkVPN), pure so it is tested without iOS.
public enum VPNStatusMachine {
  /// NEVPNStatus's raw values (NetworkExtension is not imported here).
  public enum Status: Int, Sendable {
    case invalid = 0, disconnected = 1, connecting = 2, connected = 3, reasserting = 4, disconnecting = 5
  }

  /// One switch-on: the person asked for it, and whether iOS has since said "connecting".
  public struct Attempt: Equatable, Sendable {
    public var wantsOn: Bool
    public var sawConnecting: Bool
    public init(wantsOn: Bool = false, sawConnecting: Bool = false) {
      self.wantsOn = wantsOn
      self.sawConnecting = sawConnecting
    }
  }

  public enum Phase: Equatable, Sendable {
    case connecting, connected, reconnecting, off
    /// It stopped after starting while wanted on: the extension's error says why.
    case failed
    /// Nothing to show: disconnecting, or the "disconnected" iOS posts after a save, before the
    /// start it is about to make.
    case unchanged
  }

  public static func phase(_ status: Status, _ attempt: inout Attempt) -> Phase {
    switch status {
    case .connecting:
      if attempt.wantsOn { attempt.sawConnecting = true }
      return .connecting
    case .connected: return .connected
    case .reasserting: return .reconnecting
    case .disconnecting: return .unchanged
    case .disconnected, .invalid:
      guard attempt.wantsOn else { return .off }
      guard attempt.sawConnecting else { return .unchanged }
      attempt = Attempt()
      return .failed
    }
  }
}
