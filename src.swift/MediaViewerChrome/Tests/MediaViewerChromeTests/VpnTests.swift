// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Add-ons → VPN (docs/plans/vpn-addon.md): a row whether or not the
// add-on is installed, its page offering the install while it is not. This
// target does not import the chrome (its bridge resolves only in the app's
// link), so the wiring is read as text. The Windows twin is VpnPlaceTests.cs.
import Foundation
import XCTest

final class VpnTests: XCTestCase {
  private func source(_ name: String) throws -> String {
    var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    while dir.path != "/" {
      let file = dir.appendingPathComponent("Sources/MediaViewerChrome/\(name)")
      if FileManager.default.fileExists(atPath: file.path) { return try String(contentsOf: file, encoding: .utf8) }
      dir.deleteLastPathComponent()
    }
    throw XCTSkip("\(name) not found")
  }

  func testItIsARowOfTheAddOnsListEvenBeforeItIsInstalled() throws {
    let list = try source("AddonListView.swift")
    XCTAssertTrue(list.contains(#"list.append(Entry(id: SlotAddonOffer.vpnID, name: "VPN","#))
    XCTAssertTrue(list.contains("$0.id != SlotAddonOffer.vpnID"))  // not listed twice once installed
    XCTAssertTrue(list.contains(#"case SlotAddonOffer.vpnID: return "VPN""#))
    XCTAssertTrue(list.contains(#"SlotAddonOffer.channel(id: SlotAddonOffer.vpnID, name: "VPN").probe()"#))
  }

  func testItsPageOffersTheInstall() throws {
    let page = try source("SlotItems.swift")
    XCTAssertTrue(page.contains(#"static let vpnID = "vpn""#))
    XCTAssertTrue(page.contains("if shows(SlotAddonOffer.vpnID)"))
    XCTAssertTrue(page.contains(#"SlotAddonInstallOffer(offer: SlotAddonOffer.channel(id: SlotAddonOffer.vpnID, name: "VPN")"#))
  }
}
