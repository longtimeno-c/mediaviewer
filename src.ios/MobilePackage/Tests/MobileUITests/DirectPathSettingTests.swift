// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/nas-relay.md §9.2: "Faster direct connection on mobile data", on unless switched off,
// in Settings for every library (the sample's too), and read by the core through the same key.
import MobileKit
@testable import MobileUI
import XCTest

final class DirectPathSettingTests: XCTestCase {
  private var defaults: UserDefaults!
  private let suite = "DirectPathSettingTests"

  override func setUp() {
    defaults = UserDefaults(suiteName: suite)
    defaults.removePersistentDomain(forName: suite)
  }

  override func tearDown() { defaults.removePersistentDomain(forName: suite) }

  func testItIsOnUntilSwitchedOff() {
    XCTAssertTrue(LinkPreferences.directOnMobileDataEnabled(defaults))
    defaults.set(false, forKey: Preferences.directOnMobileData)
    XCTAssertFalse(LinkPreferences.directOnMobileDataEnabled(defaults))
    defaults.set(true, forKey: Preferences.directOnMobileData)
    XCTAssertTrue(LinkPreferences.directOnMobileDataEnabled(defaults))
  }

  func testTheScreenAndTheCoreShareOneKey() {
    XCTAssertEqual(Preferences.directOnMobileData, LinkPreferences.directOnMobileData)
  }

  func testItSaysPlainlyWhatItDoes() {
    XCTAssertEqual(SettingsCopy.directOnMobileData, "Faster direct connection on mobile data")
    XCTAssertTrue(SettingsCopy.directOnMobileDataDetail.contains("relay"))
    XCTAssertTrue(SettingsCopy.directOnMobileDataDetail.contains("Wi-Fi"))
  }
}
