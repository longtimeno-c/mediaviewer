// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A phone paired at home learns the server's DuckDNS name and relay from its next connect, and
// an address the service leaves out never wipes the one it kept.
import MobileLinkSupport
import XCTest

final class ServiceAddressesTests: XCTestCase {
  func testANameAddedAfterPairingIsKept() {
    let paired = ServiceAddresses(local: "192.168.1.20:7878", publicName: "", relay: "")
    let said = ServiceAddresses(local: "192.168.1.20:7878", publicName: "home.duckdns.org:7878", relay: "relay.example.com:7879")
    XCTAssertEqual(paired.updated(by: said),
                   ServiceAddresses(local: "192.168.1.20:7878", publicName: "home.duckdns.org:7878", relay: "relay.example.com:7879"))
  }

  func testANewLanAddressReplacesTheOld() {
    let paired = ServiceAddresses(local: "192.168.1.20:7878", publicName: "home.duckdns.org:7878", relay: "")
    let said = ServiceAddresses(local: "192.168.1.31:7878", publicName: "", relay: "")
    XCTAssertEqual(paired.updated(by: said)?.local, "192.168.1.31:7878")
    XCTAssertEqual(paired.updated(by: said)?.publicName, "home.duckdns.org:7878")
  }

  func testNothingSaidOrNothingNewSavesNothing() {
    let paired = ServiceAddresses(local: "192.168.1.20:7878", publicName: "home.duckdns.org:7878", relay: "r.example.com:1")
    // An older service says nothing: everything kept, nothing to save.
    XCTAssertNil(paired.updated(by: ServiceAddresses(local: "", publicName: "", relay: "")))
    XCTAssertNil(paired.updated(by: paired))
  }
}
