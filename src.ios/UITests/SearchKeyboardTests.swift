// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The owner's report (2026-11-23): on the first search after the app opened, the keyboard went
// away and came back as they typed. A backup was running ("Back up when the app is open"), and
// while the keyboard is up iOS shows the tab bar's bottom slot on top of it: each new count in
// that slot put the keyboard away and brought it back. Needs the Simulator's software keyboard
// (I/O > Keyboard > Connect Hardware Keyboard off).
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
    guard app.keyboards.firstMatch.waitForExistence(timeout: 5) else {
      throw XCTSkip("No software keyboard: turn off Connect Hardware Keyboard in the Simulator")
    }
    field.typeText("bo")
    let shownWhileTyping = line.label

    // The app's own count of the keyboard going: a drop and return within a frame or two is
    // shorter than one query here, so watching for the keyboard would miss it.
    let stats = app.staticTexts["keyboard-stats"].firstMatch
    let threeCounts = NSPredicate { _, _ in Self.number("ticksWhileUp", in: stats.label) >= 3 }
    let waited = XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: threeCounts, object: nil)], timeout: 15)
    XCTAssertEqual(waited, .completed, "The backup never counted while the keyboard was up: \(stats.label)")
    XCTAssertEqual(Self.number("hides", in: stats.label), 0, "The keyboard went away as the backup counted: \(stats.label)")
    XCTAssertEqual(field.value as? String, "bo")

    // Dragging the page still puts the keyboard away, and then the line catches up.
    let top = app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.3))
    top.press(forDuration: 0.05, thenDragTo: app.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.98)))
    let caughtUp = NSPredicate { _, _ in line.label != shownWhileTyping }
    XCTAssertEqual(XCTWaiter().wait(for: [XCTNSPredicateExpectation(predicate: caughtUp, object: nil)], timeout: 10),
                   .completed, "The backup line did not catch up once the keyboard went")
    XCTAssertGreaterThanOrEqual(Self.number("hides", in: stats.label), 1, "Dragging did not put the keyboard away")
  }

  private static func number(_ name: String, in label: String) -> Int {
    guard let range = label.range(of: "\(name)=") else { return -1 }
    return Int(label[range.upperBound...].prefix { $0.isNumber }) ?? -1
  }
}
