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

  // Its offer is asked for when its page shows, never at launch.
  func testTheOfferIsAskedWhenItShows() throws {
    let text = try source("PhoneAccess.swift")
    XCTAssertTrue(text.contains(#"static let addonID = "phone-access""#))
    XCTAssertTrue(text.contains(#"AddonChannel(name: "mediaviewer-addon-phone-access-macos")"#))
    XCTAssertTrue(text.contains(".onAppear { if store.offer == .unknown"))
    XCTAssertTrue(text.contains("Your paired phones keep working through your server."))
  }

  // Phone access is a row of Settings → Add-ons, its settings and offer on its own
  // page like every other add-on's, and no section of its own in Settings.
  func testItLivesOnItsAddOnPage() throws {
    let list = try source("AddonListView.swift")
    XCTAssertTrue(list.contains(#"list.append(Entry(id: PhoneAccessStore.addonID, name: "Phone access","#))
    XCTAssertFalse(list.contains("Its settings are in Settings"))
    let page = try source("SlotItems.swift")
    XCTAssertTrue(page.contains("if only == PhoneAccessStore.addonID"))
    let settings = try source("SettingsView.swift")
    XCTAssertFalse(settings.contains(#"section("Phone access")"#))
    // The tray's Open Phone access settings opens its page.
    let notices = try source("AddonNotices.swift")
    XCTAssertTrue(notices.contains("if k.shell { return PhoneAccessStore.addonID }"))
    let host = try source("ChromeHost.swift")
    XCTAssertTrue(host.contains("AddonNav.shared.show(addon)"))
  }

  // A kept settings-place screen (the VPN's, docs/plans/vpn-addon.md): Open
  // shows Settings at its add-on's page, never a window of its own; its
  // menu-bar item is the VPN's shield, not Driftwire's waves.
  func testAKeptSettingsPageOpensInSettings() throws {
    let notices = try source("AddonNotices.swift")
    XCTAssertTrue(notices.contains("return settingsScreens[k.addon] == k.screen ? k.addon : nil"))
    XCTAssertTrue(notices.contains("if let page = Self.settingsPage(k, SlotStore.shared.settingsScreens) {"))
    XCTAssertTrue(notices.contains("mv_chrome_open_addon_settings($0)"))
    XCTAssertTrue(notices.contains(#"k.addon == "vpn" ? "lock.shield" : "dot.radiowaves.left.and.right""#))
    XCTAssertFalse(notices.contains("mv_chrome_open_phone_settings"))
  }

  // Contribution API 15: a secure field draws dots and never takes a value from the tree.
  func testScreensDrawASecureField() throws {
    let text = try source("ScreenView.swift")
    XCTAssertTrue(text.contains(#"if node.bool("secure") {"#))
    XCTAssertTrue(text.contains("SecureField("))
    XCTAssertTrue(text.contains(#"!focused && !node.bool("secure")"#))
  }
}
