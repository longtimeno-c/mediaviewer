// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → VPN through your server on the fixtures app (docs/plans/phone-vpn.md): a build with no
// tunnel says so and has no switch; a code sets it up and a wrong one says so; the first switch-on
// asks first; Remove asks, then it is not set up; the sample's demo says it is one throughout.
// With MV_VPN_SHOTS set (TEST_RUNNER_MV_VPN_SHOTS for xcodebuild), each state is saved there in
// light and dark.
import XCTest

final class VPNSettingsTests: XCTestCase {
  private let goodCode = "mediaviewer://relay#home.example.net:51820#Zml4dHVyZS12cG4ta2V5LTEyMzQ1Njc4"

  override func setUp() { continueAfterFailure = false }

  private func launched(_ state: String, appearance: String = "dark") -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["-fixtureState", state, "-fixtureChrome", "NO", "-appearance", appearance]
    app.launch()
    return app
  }

  private func save(_ name: String, _ appearance: String) {
    guard let dir = ProcessInfo.processInfo.environment["MV_VPN_SHOTS"] else { return }
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    try? XCUIScreen.main.screenshot().pngRepresentation
      .write(to: URL(fileURLWithPath: dir).appendingPathComponent("\(name)-\(appearance).png"))
  }

  private func element(_ app: XCUIApplication, _ id: String) -> XCUIElement {
    app.descendants(matching: .any).matching(identifier: id).firstMatch
  }

  private func isOn(_ toggle: XCUIElement) -> Bool {
    let value = toggle.value as? String
    return value == "On" || value == "1"
  }

  /// The switch itself, at the row's trailing edge: the words beside it do not move it.
  private func flip(_ toggle: XCUIElement) {
    toggle.coordinate(withNormalizedOffset: CGVector(dx: 0.92, dy: 0.5)).tap()
  }

  private func status(_ app: XCUIApplication) -> String { element(app, "vpn.status").label }

  private func waitFor(_ app: XCUIApplication, status words: String, timeout: TimeInterval = 10) -> Bool {
    let row = element(app, "vpn.status")
    let p = NSPredicate(format: "label CONTAINS %@", words)
    return XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: p, object: row)], timeout: timeout) == .completed
  }

  func testABuildWithoutTheTunnelSaysSoAndHasNoSwitch() {
    for appearance in ["light", "dark"] {
      let app = launched("vpn.notInThisBuild", appearance: appearance)
      let words = element(app, "vpn.notInThisBuild")
      XCTAssertTrue(words.waitForExistence(timeout: 30))
      XCTAssertTrue(words.label.contains("Not in this version yet"), words.label)
      XCTAssertTrue(app.navigationBars["VPN through your server"].exists)
      XCTAssertFalse(element(app, "vpn.toggle").exists, "A switch in a build with no tunnel.")
      XCTAssertFalse(element(app, "vpn.connectHost").exists, "A code field in a build with no tunnel.")
      save("not-in-this-build", appearance)
      // Settings' row says the same.
      app.navigationBars.buttons.firstMatch.tap()
      let row = app.buttons["settings.vpn"]
      XCTAssertTrue(row.waitForExistence(timeout: 10))
      XCTAssertTrue(row.label.contains("All traffic (VPN)"), row.label)
      XCTAssertTrue(row.label.contains("Not in this version yet"), row.label)
      save("settings-row", appearance)
      app.terminate()
    }
  }

  func testAWrongCodeSaysSoAndAGoodOneShowsTheSwitch() {
    for appearance in ["light", "dark"] {
      let app = launched("vpn.notSetUp", appearance: appearance)
      let connectHost = app.buttons["vpn.connectHost"]
      XCTAssertTrue(connectHost.waitForExistence(timeout: 30))
      XCTAssertEqual(connectHost.label, "Connect to a VPN host…")
      XCTAssertFalse(element(app, "vpn.toggle").exists)
      save("not-set-up", appearance)
      connectHost.tap()
      let field = app.secureTextFields["vpn.code"]
      XCTAssertTrue(field.waitForExistence(timeout: 10))
      field.tap()
      XCTAssertTrue(app.buttons["vpn.connect"].exists)
      // Return connects; the good code below goes through the Connect button.
      field.typeText("not a code\n")
      let error = element(app, "vpn.codeError")
      XCTAssertTrue(error.waitForExistence(timeout: 10), "A wrong code said nothing.")
      XCTAssertTrue(error.label.hasPrefix("That isn't a VPN code"), error.label)
      save("code-error", appearance)
      field.tap()
      field.typeText(String(repeating: XCUIKeyboardKey.delete.rawValue, count: 12))
      field.typeText(goodCode)
      let connect = app.buttons["vpn.connect"]
      XCTAssertTrue(connect.waitForExistence(timeout: 5))
      XCTAssertTrue(XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: NSPredicate(format: "isHittable == true"), object: connect)],
                                     timeout: 10) == .completed, "Connect is not where a finger can reach it.")
      connect.tap()
      let toggle = element(app, "vpn.toggle")
      XCTAssertTrue(toggle.waitForExistence(timeout: 10), "A good code did not set it up.")
      XCTAssertFalse(isOn(toggle), "Pasting a code switched it on.")
      XCTAssertTrue(waitFor(app, status: "Off"))
      XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'home.example.net:51820'")).count > 0)
      app.terminate()
    }
  }

  func testTheFirstSwitchOnAsksFirst() {
    for appearance in ["light", "dark"] {
      let app = launched("vpn.off", appearance: appearance)
      let toggle = element(app, "vpn.toggle")
      XCTAssertTrue(toggle.waitForExistence(timeout: 30))
      XCTAssertFalse(isOn(toggle))
      flip(toggle)
      let turnOn = app.buttons["vpn.consent.turnOn"]
      XCTAssertTrue(turnOn.waitForExistence(timeout: 10), "Switched on without asking.")
      XCTAssertTrue(app.staticTexts["Send all traffic through your server?"].exists)
      XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'home.example.net:51820'")).count > 0)
      save("consent", appearance)
      // Not Now leaves it off.
      app.buttons["vpn.consent.notNow"].tap()
      XCTAssertTrue(turnOn.waitForNonExistence(timeout: 10))
      XCTAssertFalse(isOn(toggle))
      XCTAssertTrue(waitFor(app, status: "Off"))
      // Asked again, Turn On reaches Connected.
      flip(toggle)
      XCTAssertTrue(turnOn.waitForExistence(timeout: 10), "The second try did not ask: Not Now is not a yes.")
      turnOn.tap()
      XCTAssertTrue(waitFor(app, status: "Connected"), status(app))
      XCTAssertTrue(isOn(toggle))
      save("connected", appearance)
      // Off again needs no question; on again, already agreed, needs none either.
      flip(toggle)
      XCTAssertTrue(waitFor(app, status: "Off"))
      flip(toggle)
      XCTAssertTrue(waitFor(app, status: "Connected"), status(app))
      XCTAssertFalse(turnOn.exists)
      app.terminate()
    }
  }

  func testRemoveAsksThenItIsNotSetUp() {
    let app = launched("vpn.connected")
    XCTAssertTrue(element(app, "vpn.toggle").waitForExistence(timeout: 30))
    XCTAssertTrue(waitFor(app, status: "Connected"))
    app.buttons["vpn.remove"].tap()
    let confirm = app.buttons.matching(identifier: "vpn.remove.confirm").firstMatch
    XCTAssertTrue(confirm.waitForExistence(timeout: 10), "Removed without asking.")
    XCTAssertTrue(app.staticTexts["Remove the VPN?"].exists)
    confirm.tap()
    XCTAssertTrue(app.buttons["vpn.connectHost"].waitForExistence(timeout: 10))
    XCTAssertEqual(app.buttons["vpn.connectHost"].label, "Connect to a VPN host…")
    XCTAssertFalse(element(app, "vpn.toggle").exists)
  }

  func testAFailedVPNSaysWhy() {
    let app = launched("vpn.failed")
    XCTAssertTrue(element(app, "vpn.toggle").waitForExistence(timeout: 30))
    XCTAssertTrue(waitFor(app, status: "Your server didn't answer"), status(app))
    XCTAssertFalse(isOn(element(app, "vpn.toggle")))
  }

  func testTheSampleIsALabelledDemoThroughout() {
    for appearance in ["light", "dark"] {
      let app = launched("vpn.sample", appearance: appearance)
      let banner = element(app, "vpn.demoBanner")
      let toggle = element(app, "vpn.toggle")
      XCTAssertTrue(banner.waitForExistence(timeout: 30), "The demo does not say it is one.")
      XCTAssertTrue(toggle.exists)
      XCTAssertTrue(toggle.label.contains("demo"), toggle.label)
      XCTAssertTrue(waitFor(app, status: "Off"))
      flip(toggle)
      let turnOn = app.buttons["vpn.consent.turnOn"]
      XCTAssertTrue(turnOn.waitForExistence(timeout: 10))
      XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'Nothing is routed'")).count > 0)
      save("sample-consent", appearance)
      turnOn.tap()
      XCTAssertTrue(waitFor(app, status: "Connected"), status(app))
      XCTAssertTrue(banner.exists, "The demo's banner went away once it was on.")
      XCTAssertTrue(isOn(toggle))
      save("sample-connected", appearance)
      // Settings' row says demo too.
      app.navigationBars.buttons.firstMatch.tap()
      let row = app.buttons["settings.vpn"]
      XCTAssertTrue(row.waitForExistence(timeout: 10))
      XCTAssertTrue(row.label.contains("Connected · demo"), row.label)
      app.terminate()
    }
  }
}
