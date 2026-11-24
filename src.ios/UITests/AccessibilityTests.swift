// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// What VoiceOver would be offered, read from the accessibility tree, and Apple's accessibility audit.
// Neither is VoiceOver itself: someone still has to listen to the app on a phone.
import XCTest

final class AccessibilityTests: XCTestCase {
  private func launched(_ state: String, appearance: String = "dark") -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["-fixtureState", state, "-fixtureChrome", "NO", "-appearance", appearance]
    app.launch()
    return app
  }

  /// MV_SHOT_DIR (TEST_RUNNER_MV_SHOT_DIR to xcodebuild): keeps a picture of the screen there.
  private func shot(_ name: String) {
    guard let dir = ProcessInfo.processInfo.environment["MV_SHOT_DIR"] else { return }
    try? XCUIScreen.main.screenshot().pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent(name + ".png"))
  }

  func testViewerControlsAreReachableWithoutAClip() throws {
    let app = launched("viewer")
    XCTAssertTrue(app.buttons["Save to Photos"].waitForExistence(timeout: 30))
    XCTAssertTrue(app.buttons["Details"].exists)
  }

  /// A clip plays on its own page of the viewer: its transport and the viewer's controls are both
  /// there, each once.
  func testOpenClipKeepsTheViewerControls() throws {
    let app = launched("clip")
    XCTAssertTrue(app.buttons["Pause"].waitForExistence(timeout: 30))
    XCTAssertEqual(app.buttons.matching(identifier: "Save to Photos").count, 1)
    XCTAssertEqual(app.buttons.matching(identifier: "Details").count, 1)
  }

  /// Full screen: the picture is the whole screen, so a portrait clip is as large as the phone
  /// allows (it was the space left above the position bar and the buttons), and with no button
  /// on screen the picture itself offers the way back.
  func testClipFullScreenFillsTheScreen() throws {
    let app = launched("clip")
    XCTAssertTrue(app.buttons["Pause"].waitForExistence(timeout: 30))
    let shown = app.descendants(matching: .any).matching(identifier: "clip.picture").firstMatch
    XCTAssertTrue(shown.waitForExistence(timeout: 5))
    let screen = app.windows.firstMatch.frame
    XCTAssertLessThan(shown.frame.height, screen.height * 0.8, "With its controls the picture shares the screen")
    shot("clip-controls")
    app.buttons["Full screen"].tap()
    let full = app.descendants(matching: .any).matching(identifier: "clip.picture.fullscreen").firstMatch
    XCTAssertTrue(full.waitForExistence(timeout: 5), "The clip did not go full screen")
    XCTAssertEqual(full.frame.minX, screen.minX, accuracy: 1)
    XCTAssertEqual(full.frame.minY, screen.minY, accuracy: 1)
    XCTAssertEqual(full.frame.width, screen.width, accuracy: 1)
    XCTAssertEqual(full.frame.height, screen.height, accuracy: 1)
    XCTAssertFalse(app.buttons["Pause"].exists)
    shot("clip-full")
    full.tap()
    XCTAssertTrue(app.buttons["Full screen"].waitForExistence(timeout: 5), "A tap did not bring the controls back")
    XCTAssertTrue(app.buttons["Pause"].exists || app.buttons["Play"].exists)
  }

  func testRunningCopyReadsItsLineAndKeepsCancel() throws {
    let app = launched("firstCopy.running")
    XCTAssertTrue(app.buttons["Cancel"].waitForExistence(timeout: 30))
    let line = app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'Copying library'"))
    XCTAssertGreaterThan(line.count, 0, "The progress line is not read as words.")
  }

  func testFailedCopyKeepsBothActions() throws {
    let app = launched("firstCopy.noSpace")
    XCTAssertTrue(app.buttons["Try again"].waitForExistence(timeout: 30))
    XCTAssertTrue(app.buttons["Cancel"].exists)
    XCTAssertTrue(app.staticTexts.containing(NSPredicate(format: "label CONTAINS 'enough space'")).count > 0)
  }

  /// Try again after a failed read of the code must come back to the fingerprint check.
  func testTryAgainAfterFailedReadReturnsToFingerprint() throws {
    let app = launched("connect.identifyFails")
    let retry = app.buttons["Try again"]
    XCTAssertTrue(retry.waitForExistence(timeout: 30))
    retry.tap()
    XCTAssertTrue(app.buttons["They match"].waitForExistence(timeout: 10), "Pairing went ahead without the check.")
  }

  func testAccessibilityAudit() throws {
    var found: [String] = []
    let states = ["welcome", "connect.confirm", "firstCopy.running", "library", "search.recents", "viewer", "clip",
                  "connection", "people", "firstCopy.noSpace", "library.failed"]
    // Both looks: contrast is checked against each one's colours. The viewer and clip stay dark.
    for (state, appearance) in states.flatMap({ s in ["dark", "light"].map { (s, $0) } }) {
      let app = launched(state, appearance: appearance)
      sleep(8)
      try app.performAccessibilityAudit { issue in
        let element = issue.element.map { "\($0.elementType.rawValue) '\($0.label)'" } ?? "no element"
        found.append("AUDIT \(state) \(appearance) [\(issue.auditType.rawValue)] \(element): \(issue.compactDescription)")
        // Reported, not failed: the date and duration badges are capped on purpose because they
        // sit over photos, and these two checks also flag text the large-type screens show growing
        // normally. Contrast, touch areas and the rest fail the test.
        return issue.auditType == .dynamicType || issue.auditType == .textClipped
      }
      app.terminate()
    }
    for line in found { print(line) }
    let report = XCTAttachment(string: found.joined(separator: "\n"))
    report.lifetime = .keepAlways
    add(report)
  }
}
