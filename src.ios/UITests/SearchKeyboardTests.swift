// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The owner's report (2026-11-23): on the first search after the app opened, the keyboard went
// away and came back as they typed. A backup was running ("Back up when the app is open"), and
// while the keyboard is up iOS shows the tab bar's bottom slot on top of it: each new count in
// that slot put the keyboard away and brought it back. Needs the Simulator's software keyboard
// (I/O > Keyboard > Connect Hardware Keyboard off).
//
// These tests type by tapping the on-screen keys, never with typeText: typeText types through a
// virtual hardware keyboard, and where the keyboard setting "minimise the keyboard when a hardware
// keyboard is used" is on (com.apple.keyboard.preferences AutomaticMinimizationEnabled), iOS puts
// the on-screen keyboard away as it connects and keeps it away for later runs. That is iOS doing
// what it should, not the app taking the keyboard.
import XCTest

final class SearchKeyboardTests: XCTestCase {
  func testABackupCountingDoesNotTakeTheKeyboard() throws {
    let app = XCUIApplication()
    app.launchArguments = ["-fixtureState", "library.backupRunning", "-fixtureChrome", "NO", "-fixtureKeyboardMeter", "YES"]
    app.launch()
    let searchTab = app.tabBars.buttons["Search"].firstMatch
    XCTAssertTrue(searchTab.waitForExistence(timeout: 30))
    searchTab.tap()
    let field = app.searchFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 10))
    let line = app.descendants(matching: .any).matching(NSPredicate(format: "label CONTAINS 'backed up'")).firstMatch
    XCTAssertTrue(line.waitForExistence(timeout: 10), "No backup line above the tab bar")
    field.tap()
    let stats = app.staticTexts["keyboard-stats"].firstMatch
    try requireSoftwareKeyboard(app, stats)
    Self.tapKeys("bo", in: app)
    let shownWhileTyping = line.label

    // The app's own count of the keyboard going: a drop and return within a frame or two is
    // shorter than one query here, so watching for the keyboard would miss it.
    let threeCounts = NSPredicate { _, _ in Self.number("ticksWhileUp", in: stats.label) >= 3 }
    let waited = XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: threeCounts, object: nil)], timeout: 15)
    XCTAssertEqual(waited, .completed, "The backup never counted while the keyboard was up: \(stats.label)")
    XCTAssertEqual(Self.number("hides", in: stats.label), 0, "The keyboard went away as the backup counted: \(stats.label)")
    XCTAssertEqual((field.value as? String)?.lowercased(), "bo")

    // Dragging the page still puts the keyboard away, and then the line catches up.
    dismissKeyboard(app)
    let caughtUp = NSPredicate { _, _ in line.label != shownWhileTyping }
    XCTAssertEqual(XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: caughtUp, object: nil)], timeout: 10),
                   .completed, "The backup line did not catch up once the keyboard went")
    XCTAssertGreaterThanOrEqual(Self.number("hides", in: stats.label), 1, "Dragging did not put the keyboard away")
  }

  /// A new catalogue once the keyboard is up (fixture state) brings the People tab: adding a tab
  /// redrew the tabs under the keyboard and took it. The tab now waits for the keyboard to go.
  func testThePeopleTabArrivingDoesNotTakeTheKeyboard() throws {
    let (app, field, stats) = try typingInSearch("library.backupPeopleArrive")
    let served = NSPredicate { _, _ in Self.number("peopleServed", in: stats.label) >= 1 }
    let waited = XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: served, object: nil)], timeout: 20)
    XCTAssertEqual(waited, .completed, "The people never came: \(stats.label)")
    // Typing waits for the app to settle, so the redraw that follows the people has happened.
    Self.tapKeys("a", in: app)
    XCTAssertEqual(Self.number("hides", in: stats.label), 0, "The keyboard went away as the People tab arrived: \(stats.label)")
    XCTAssertEqual((field.value as? String)?.lowercased(), "boa")
    XCTAssertFalse(app.tabBars.buttons["People"].exists, "The People tab came while the keyboard was up")
    dismissKeyboard(app)
    XCTAssertTrue(app.tabBars.buttons["People"].waitForExistence(timeout: 10), "The People tab never came once the keyboard went")
  }

  private func typingInSearch(_ state: String) throws -> (XCUIApplication, XCUIElement, XCUIElement) {
    let app = XCUIApplication()
    app.launchArguments = ["-fixtureState", state, "-fixtureChrome", "NO", "-fixtureKeyboardMeter", "YES"]
    app.launch()
    let searchTab = app.tabBars.buttons["Search"].firstMatch
    XCTAssertTrue(searchTab.waitForExistence(timeout: 30))
    searchTab.tap()
    let field = app.searchFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 10))
    field.tap()
    let stats = app.staticTexts["keyboard-stats"].firstMatch
    try requireSoftwareKeyboard(app, stats)
    Self.tapKeys("bo", in: app)
    return (app, field, stats)
  }

  /// The on-screen keyboard came up (the app saw keyboardWillShow). With a hardware keyboard
  /// attached it does not, and there is nothing here to test.
  private func requireSoftwareKeyboard(_ app: XCUIApplication, _ stats: XCUIElement) throws {
    guard app.keyboards.firstMatch.waitForExistence(timeout: 5) else {
      throw XCTSkip("No on-screen keyboard: turn off Connect Hardware Keyboard in the Simulator, or restart it if a test typed with typeText where the keyboard minimises for a hardware keyboard")
    }
    // The keyboard is there, so the meter must be too and must have seen it come: a missing
    // meter fails here, it is never a skip.
    XCTAssertTrue(stats.waitForExistence(timeout: 5), "The keyboard meter is not on screen")
    let shown = NSPredicate { _, _ in Self.number("shows", in: stats.label) >= 1 }
    XCTAssertEqual(XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: shown, object: nil)], timeout: 5), .completed,
                   "The app never saw the keyboard come up")
  }

  /// Types by tapping the on-screen keys (see the note at the top).
  private static func tapKeys(_ text: String, in app: XCUIApplication) {
    for character in text {
      app.keyboards.keys.matching(NSPredicate(format: "label ==[c] %@", String(character))).firstMatch.tap()
    }
  }

  private func dismissKeyboard(_ app: XCUIApplication) {
    let top = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3))
    top.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.98)))
  }

  private static func number(_ name: String, in label: String) -> Int {
    guard let range = label.range(of: "\(name)=") else { return -1 }
    return Int(label[range.upperBound...].prefix { $0.isNumber }) ?? -1
  }
}
