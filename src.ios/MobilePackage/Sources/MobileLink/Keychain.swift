// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Device identity, pairing secrets and the pinned keys (docs/plans/mobile.md §8.3): this device
// only, available after first unlock so a sync can run in the background.
import Foundation
import Security

enum Keychain {
  private static let service = "io.github.longtimeno-c.mediaviewer.mobile.link"

  static func put(_ value: Data, account: String) -> Bool {
    let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecAttrAccount as String: account]
    SecItemDelete(query as CFDictionary)
    var add = query
    add[kSecValueData as String] = value
    add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
    return SecItemAdd(add as CFDictionary, nil) == errSecSuccess
  }

  static func get(account: String) -> Data? {
    let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecAttrAccount as String: account, kSecReturnData as String: true,
                                kSecMatchLimit as String: kSecMatchLimitOne]
    var out: AnyObject?
    guard SecItemCopyMatching(query as CFDictionary, &out) == errSecSuccess else { return nil }
    return out as? Data
  }

  static func remove(account: String) {
    let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecAttrAccount as String: account]
    SecItemDelete(query as CFDictionary)
  }
}

/// What pairing leaves behind, in the Keychain as JSON. Keys stay in hex so nothing else is on disk.
struct PairingRecord: Codable, Sendable {
  var libraryID: String
  var libraryName: String
  var fingerprint: String
  var publisherKey: String
  var local: String
  var publicName: String
  var identity: String
  var accessKey: String
  var deviceName: String
  /// The sample library (§11.1): read from its static folder, with no service. nil in a record
  /// written before it existed.
  var sample: Bool? = nil
  var isSample: Bool { sample == true }
  /// A time-limited phone's end, unix seconds on the service's clock, as it last said (nil: none).
  /// Kept so the phone can say the access ended even with no network.
  var accessUntil: Double? = nil
  static let account = "pairing"

  static func load() -> PairingRecord? {
    guard let data = Keychain.get(account: account) else { return nil }
    return try? JSONDecoder().decode(PairingRecord.self, from: data)
  }

  func save() -> Bool {
    guard let data = try? JSONEncoder().encode(self) else { return false }
    return Keychain.put(data, account: PairingRecord.account)
  }

  static func clear() { Keychain.remove(account: account) }
}

enum DeviceKey {
  private static let account = "device-key"

  /// This phone's X25519 private key, made once and never leaving the Keychain.
  static func loadOrMake(new: (UnsafeMutablePointer<UInt8>, UnsafeMutablePointer<UInt8>) -> Int32) -> Data? {
    if let existing = Keychain.get(account: account), existing.count == 32 { return existing }
    var priv = [UInt8](repeating: 0, count: 32)
    var pub = [UInt8](repeating: 0, count: 32)
    guard new(&priv, &pub) == 0 else { return nil }
    let data = Data(priv)
    return Keychain.put(data, account: account) ? data : nil
  }

  static func clear() { Keychain.remove(account: account) }
}

extension Data {
  init?(hex: String) {
    guard hex.count % 2 == 0 else { return nil }
    var out = Data(capacity: hex.count / 2)
    var index = hex.startIndex
    while index < hex.endIndex {
      let next = hex.index(index, offsetBy: 2)
      guard let byte = UInt8(hex[index..<next], radix: 16) else { return nil }
      out.append(byte)
      index = next
    }
    self = out
  }

  var hexString: String { map { String(format: "%02x", $0) }.joined() }
}
