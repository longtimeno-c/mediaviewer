// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Phone access (PhoneAccess.swift, docs/plans/phone-access-addon.md).
// The card itself is the add-on's view tree, tested with the add-on
// (tests/test_phone_addon.cpp). This target does not import the chrome (its
// bridge resolves only in the app's link), so the section is read as text.
import Foundation
import XCTest

final class PhoneAccessTests: XCTestCase {
  private func source(_ name: String) throws -> String {
    var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    while dir.path != "/" {
      let file = dir.appendingPathComponent("Sources/MediaViewerChrome/\(name)")
      if FileManager.default.fileExists(atPath: file.path) { return try String(contentsOf: file, encoding: .utf8) }
      dir.deleteLastPathComponent()
    }
    throw XCTSkip("\(name) not found")
  }

  // Owner, 2026-11-14: offered the first time someone opens Phone access, never at launch.
  func testTheSectionIsTheAddOnsScreenOrItsOffer() throws {
    let text = try source("PhoneAccess.swift")
    XCTAssertTrue(text.contains(#"static let addonID = "phone-access""#))
    XCTAssertTrue(text.contains("AddonSettingsScreen(address: address)"))
    XCTAssertTrue(text.contains(#"AddonChannel(name: "mediaviewer-addon-phone-access-macos")"#))
    XCTAssertTrue(text.contains(".onAppear { if store.offer == .unknown"))
    XCTAssertTrue(text.contains("Your paired phones keep working through your server."))
  }

  // One view per screen: its page under Add-ons does not draw it a second time.
  func testTheAddOnsPageDoesNotDrawItTwice() throws {
    let text = try source("AddonListView.swift")
    XCTAssertTrue(text.contains("if id == PhoneAccessStore.addonID {"))
  }

  // Contribution API 15: a secure field draws dots and never takes a value from the tree.
  func testScreensDrawASecureField() throws {
    let text = try source("ScreenView.swift")
    XCTAssertTrue(text.contains(#"if node.bool("secure") {"#))
    XCTAssertTrue(text.contains("SecureField("))
    XCTAssertTrue(text.contains(#"!focused && !node.bool("secure")"#))
  }
}
