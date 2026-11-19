// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/phone-vpn.md §1.2–1.3 (S2): what the app and the VPN extension share.
import Foundation
import MobileLinkSupport
import XCTest

final class VPNTunnelTests: XCTestCase {
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
    XCTAssertFalse(item.tls)  // the HTTPS look waits for S4's switch
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
  }
}
