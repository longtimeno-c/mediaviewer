// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/phone-vpn.md §1.2–1.3 (S2): what the app and the VPN extension share.
import Foundation
import MobileLinkSupport
import XCTest

final class VPNTunnelTests: XCTestCase {
  /// The last call wins (LinkVPN): moves run in the order they were called, never two at once, and
  /// a switch-on still waiting on iOS learns that a later move was called.
  func testVPNMovesRunInTurnAndKnowWhenOvertaken() async {
    let moves = VPNMoves()
    let log = Log()
    let gate = Gate()
    // A switch-on that waits on iOS's alert; meanwhile a switch-off, then a remove, are called.
    let on = Task {
      await moves.inTurn { run in
        log.add("on starts")
        await gate.wait()
        log.add(moves.isLatest(run) ? "on starts the tunnel" : "on overtaken")
      }
    }
    while !log.entries.contains("on starts") { await Task.yield() }
    let off = Task { await moves.inTurn { run in log.add("off \(moves.isLatest(run) ? "latest" : "overtaken")") } }
    while moves.latest < 2 { await Task.yield() }  // the off is called first, then the remove
    let remove = Task { await moves.inTurn { run in log.add("remove \(moves.isLatest(run) ? "latest" : "overtaken")") } }
    while moves.latest < 3 { await Task.yield() }
    for _ in 0..<100 { await Task.yield() }
    XCTAssertEqual(log.entries, ["on starts"], "A later move began before the switch-on ended")
    gate.open()
    await on.value
    await off.value
    await remove.value
    // Every move ran, in order; only the last was still the latest when it ran.
    XCTAssertEqual(log.entries, ["on starts", "on overtaken", "off overtaken", "remove latest"])
  }

  func testTheSharedKeychainGroupCarriesTheTeamPrefix() {
    XCTAssertEqual(VPNNames.keychainGroup(prefix: "23FQ4A4Q35."), "23FQ4A4Q35.io.github.longtimeno-c.mediaviewer.mobile.vpn")
    XCTAssertEqual(VPNNames.keychainGroup(prefix: "23FQ4A4Q35"), "23FQ4A4Q35.io.github.longtimeno-c.mediaviewer.mobile.vpn")
    // An unsigned build has no prefix (or the unexpanded variable): no shared group to name.
    XCTAssertNil(VPNNames.keychainGroup(prefix: nil))
    XCTAssertNil(VPNNames.keychainGroup(prefix: ""))
    XCTAssertNil(VPNNames.keychainGroup(prefix: "$(AppIdentifierPrefix)"))
    // Owner call 8 (2026-11-19): these cannot change after release without losing the configuration.
    XCTAssertEqual(VPNNames.extensionBundleID, "io.github.longtimeno-c.mediaviewer.mobile.DriftwireTunnel")
    XCTAssertEqual(VPNNames.appGroup, "group.io.github.longtimeno-c.mediaviewer.mobile")
  }

  func testTheTunnelItemKeepsAWholeItemOnly() throws {
    let item = VPNTunnelItem(relay: "home.duckdns.org:47614", key: "PHONE-VPN-TEST-KEY-0123456789", knock: Data(repeating: 7, count: 32))
    let data = try XCTUnwrap(item.encoded())
    XCTAssertEqual(VPNTunnelItem.decode(data), item)
    XCTAssertFalse(item.tls)  // a relay# code: the relay's plain port
    // A relay-tls code's look and port reach the extension through the Keychain item (LinkVPN
    // makes the item from the parsed code this way).
    let code = VPNCode(relay: "home.duckdns.org:443", key: "PHONE-VPN-TEST-KEY-0123456789", tls: true, tlsPort: 443)
    let look = VPNTunnelItem(code: code, knock: Data(repeating: 7, count: 32))
    XCTAssertEqual(look.relay, "home.duckdns.org:443")
    XCTAssertEqual(look.key, code.key)
    let back = try XCTUnwrap(VPNTunnelItem.decode(XCTUnwrap(look.encoded())))
    XCTAssertTrue(back.tls)
    XCTAssertEqual(back.tlsPort, 443)
    XCTAssertEqual(back, look)
    // A relay# code: plain, whatever port it names.
    let plain = VPNTunnelItem(code: VPNCode(relay: "home.duckdns.org:47614", key: code.key, tls: false, tlsPort: 47614),
                              knock: Data(repeating: 7, count: 32))
    XCTAssertFalse(plain.tls)
    XCTAssertEqual(plain.tlsPort, 0)
    // A knock key that is not 32 bytes, no key, no server: not kept, and not read back.
    XCTAssertNil(VPNTunnelItem(relay: "h:1", key: "k", knock: Data(count: 31)).encoded())
    XCTAssertNil(VPNTunnelItem(relay: "h:1", key: "", knock: Data(count: 32)).encoded())
    XCTAssertNil(VPNTunnelItem(relay: "", key: "k", knock: Data(count: 32)).encoded())
    XCTAssertNil(VPNTunnelItem.decode(Data("{\"relay\":\"h:1\",\"key\":\"k\"}".utf8)))
    XCTAssertNil(VPNTunnelItem.decode(Data("not json".utf8)))
  }

  func testTheSharedStateNeverHoldsTheKey() throws {
    let suite = "vpn-tunnel-tests-\(UUID().uuidString)"
    let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
    defer { defaults.removePersistentDomain(forName: suite) }
    XCTAssertEqual(VPNSharedState.load(defaults), VPNSharedState())  // nothing saved: not set up
    VPNSharedState(host: "home.duckdns.org:47614", consented: true).save(defaults)
    XCTAssertEqual(VPNSharedState.load(defaults), VPNSharedState(host: "home.duckdns.org:47614", consented: true))
    let raw = try XCTUnwrap(defaults.data(forKey: VPNSharedState.defaultsKey))
    XCTAssertFalse(String(decoding: raw, as: UTF8.self).contains("KEY"))
  }

  func testTheServersProblemComesBackFromTheExtensionsError() {
    XCTAssertEqual(VPNServerProblem(error: VPNServerProblem.keyRejected.error), .keyRejected)
    XCTAssertEqual(VPNServerProblem(error: NSError(domain: VPNNames.errorDomain, code: 3)), .unreachable)
    XCTAssertEqual(VPNServerProblem(error: NSError(domain: VPNNames.errorDomain, code: 4)), .notForwarding)
    // Codes match mv_mobile_vpn.h; anything else, or another domain, is not the server's.
    XCTAssertNil(VPNServerProblem(error: NSError(domain: VPNNames.errorDomain, code: 6)))
    XCTAssertNil(VPNServerProblem(error: NSError(domain: NSURLErrorDomain, code: 2)))
    XCTAssertNil(VPNServerProblem(error: nil))
  }

  func testTheInterfaceSettingsFromTheCore() throws {
    // As mv_vpn_settings_json writes them (tests/test_phone_vpn.cpp checks that side).
    let json = """
    {"address4":"198.18.0.1","prefix4":30,"dns4":"198.18.0.2","address6":"fd5d:7269:6674::1","prefix6":64,"mtu":1400,
     "include4":[{"base":"0.0.0.0","prefix":0}],"exclude4":[{"base":"203.0.113.9","prefix":32},{"base":"192.168.1.0","prefix":24}],
     "include6":[{"base":"::","prefix":0}],"exclude6":[{"base":"2001:db8::9","prefix":128}]}
    """
    let s = try XCTUnwrap(VPNInterfaceSettings.parse(json))
    XCTAssertEqual(s.address4, "198.18.0.1")
    XCTAssertEqual(s.mtu, 1400)
    XCTAssertTrue(s.include4.first?.isDefault == true)
    XCTAssertTrue(s.include6.first?.isDefault == true)  // IPv6 claimed too: no leak around the tunnel
    XCTAssertEqual(s.exclude4, [.init(base: "203.0.113.9", prefix: 32), .init(base: "192.168.1.0", prefix: 24)])
    XCTAssertEqual(s.exclude6, [.init(base: "2001:db8::9", prefix: 128)])
    XCTAssertNil(VPNInterfaceSettings.parse("{}"))
    XCTAssertEqual(VPNInterfaceSettings.mask(30), "255.255.255.252")
    XCTAssertEqual(VPNInterfaceSettings.mask(24), "255.255.255.0")
    XCTAssertEqual(VPNInterfaceSettings.mask(32), "255.255.255.255")
    XCTAssertEqual(VPNInterfaceSettings.mask(0), "0.0.0.0")
    XCTAssertNil(s.remote)  // optional: the server's address when the core gives it
    let withRemote = try XCTUnwrap(VPNInterfaceSettings.parse(json.replacingOccurrences(of: "{\"address4\"", with: "{\"remote\":\"203.0.113.9\",\"address4\"")))
    XCTAssertEqual(withRemote.remote, "203.0.113.9")
  }
}

final class VPNStatusMachineTests: XCTestCase {
  typealias M = VPNStatusMachine

  func testAnEarlyDisconnectAfterTheSaveIsNotAFailure() {
    // Switched on: iOS posts "disconnected" for the configuration it just saved, then connects.
    var a = M.Attempt(wantsOn: true)
    XCTAssertEqual(M.phase(.disconnected, &a), .unchanged)
    XCTAssertTrue(a.wantsOn)  // still wanted: the real failure, if any, is still to come
    XCTAssertEqual(M.phase(.connecting, &a), .connecting)
    XCTAssertEqual(M.phase(.connected, &a), .connected)
    XCTAssertEqual(M.phase(.reasserting, &a), .reconnecting)
    XCTAssertEqual(M.phase(.disconnecting, &a), .unchanged)
    // It stopped on its own after starting: a failure, said once.
    XCTAssertEqual(M.phase(.disconnected, &a), .failed)
    XCTAssertEqual(a, M.Attempt())
    XCTAssertEqual(M.phase(.disconnected, &a), .off)
  }

  func testAStartThatFailsIsAFailure() {
    var a = M.Attempt(wantsOn: true)
    XCTAssertEqual(M.phase(.connecting, &a), .connecting)
    XCTAssertEqual(M.phase(.invalid, &a), .failed)  // the extension refused the start
  }

  func testSwitchedOffIsOff() {
    var a = M.Attempt()
    XCTAssertEqual(M.phase(.connecting, &a), .connecting)
    XCTAssertFalse(a.sawConnecting)  // not an attempt of the person's
    XCTAssertEqual(M.phase(.disconnected, &a), .off)
    XCTAssertEqual(M.Status(rawValue: 3), .connected)  // NEVPNStatus.connected
  }
}

private final class Log: @unchecked Sendable {
  private let lock = NSLock()
  private var list: [String] = []
  var entries: [String] { lock.withLock { list } }
  func add(_ entry: String) { lock.withLock { list.append(entry) } }
}

/// Holds a move until the test opens it.
private final class Gate: @unchecked Sendable {
  private let lock = NSLock()
  private var isOpen = false
  private var waiter: CheckedContinuation<Void, Never>?
  func wait() async {
    await withCheckedContinuation { c in
      let now = lock.withLock { () -> Bool in
        if isOpen { return true }
        waiter = c
        return false
      }
      if now { c.resume() }
    }
  }
  func open() {
    let c = lock.withLock { () -> CheckedContinuation<Void, Never>? in
      isOpen = true
      defer { waiter = nil }
      return waiter
    }
    c?.resume()
  }
}
