// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The owner's ask (2026-11-14): as the viewer's filmstrip is scrolled, the photo on screen changes,
// as in Photos. Opens a photo of the sample library, drags the strip, and checks that the photo
// changed while the finger was still down, that the strip came to rest with the photo on screen in
// its middle, and that a swipe on the photo still moves the strip. Runs only with
// MV_FILMSTRIP_SCRUB set (and MV_SAMPLE_BASE for a Debug build).
import XCTest

final class FilmstripScrubTests: XCTestCase {
  private let bundle = "io.github.longtimeno-c.mediaviewer.mobile"
  private let gridTile = NSPredicate(format: "label BEGINSWITH 'Photo,' OR label BEGINSWITH 'Clip,'")
  private var env: [String: String] { ProcessInfo.processInfo.environment }

  private func save(_ name: String) {
    guard let dir = env["MV_FILMSTRIP_OUT"] else { return }
    try? XCUIScreen.main.screenshot().pngRepresentation.write(to: URL(fileURLWithPath: dir).appendingPathComponent(name + ".png"))
  }

  private func launch() throws -> XCUIApplication {
    guard env["MV_FILMSTRIP_SCRUB"] != nil else { throw XCTSkip("Set MV_FILMSTRIP_SCRUB to run") }
    let app = XCUIApplication(bundleIdentifier: bundle)
    if let base = env["MV_SAMPLE_BASE"], !base.isEmpty { app.launchEnvironment["MV_SAMPLE_BASE"] = base }
    app.launch()
    return app
  }

  private func ensureLibrary(_ app: XCUIApplication) {
    let tiles = app.buttons.matching(gridTile)
    if tiles.firstMatch.waitForExistence(timeout: 8) { return }
    if app.buttons["Scan code"].waitForExistence(timeout: 5) { app.buttons["Scan code"].tap() }
    app.buttons["Enter a code"].tap()
    let field = app.alerts.textFields.firstMatch
    XCTAssertTrue(field.waitForExistence(timeout: 5))
    field.typeText("sample")
    app.alerts.buttons["Open"].tap()
    let open = app.buttons["Open sample library"]
    XCTAssertTrue(open.waitForExistence(timeout: 30))
    open.tap()
    XCTAssertTrue(tiles.firstMatch.waitForExistence(timeout: 600))
    sleep(3)
  }

  /// The strip's cell for the photo on screen (the selected one): its label and where its middle
  /// is, read now (an element taken by index is looked up again each time it is read).
  private func selected(_ strip: XCUIElement) -> (label: String, midX: CGFloat)? {
    for cell in strip.buttons.allElementsBoundByIndex where cell.exists && cell.isSelected {
      return (cell.label, cell.frame.midX)
    }
    return nil
  }

  func testDraggingTheStripChangesThePhoto() throws {
    let app = try launch()
    ensureLibrary(app)
    app.buttons.matching(gridTile).element(boundBy: 2).tap()
    let strip = app.scrollViews["scrolls-sideways"].firstMatch
    XCTAssertTrue(strip.waitForExistence(timeout: 10), "No filmstrip")
    sleep(1)
    let before = try XCTUnwrap(selected(strip), "No cell is the photo on screen").label
    save("01-open")

    // A slow drag of about four cells to the left, the finger held at its end before lifting.
    let from = strip.coordinate(withNormalizedOffset: CGVector(dx: 0.7, dy: 0.5))
    let to = strip.coordinate(withNormalizedOffset: CGVector(dx: 0.2, dy: 0.5))
    from.press(forDuration: 0.1, thenDragTo: to, withVelocity: .slow, thenHoldForDuration: 1.0)
    sleep(1)
    save("02-after-drag")
    let after = try XCTUnwrap(selected(strip), "No cell is the photo on screen after the drag")
    XCTAssertNotEqual(after.label, before, "The photo did not change as the strip moved")
    // At rest the photo on screen sits in the middle of the strip.
    XCTAssertEqual(after.midX, strip.frame.midX, accuracy: 8, "The strip did not come to rest on the photo")

    // A swipe on the photo still leads the strip.
    app.swipeLeft()
    sleep(2)
    let swiped = try XCTUnwrap(selected(strip))
    XCTAssertNotEqual(swiped.label, after.label, "A swipe on the photo did not move to the next one")
    XCTAssertEqual(swiped.midX, strip.frame.midX, accuracy: 8, "The strip did not follow the swipe")

    // A tap on a cell still goes to that photo.
    // A cell two to the right of the middle, tapped where it is.
    let label = try XCTUnwrap(strip.buttons.allElementsBoundByIndex.lazy
      .filter { $0.exists }
      .map { ($0.label, $0.frame) }
      .first { $0.1.midX > strip.frame.midX + 90 && $0.1.maxX < strip.frame.maxX }).0
    let point = strip.buttons[label].firstMatch.frame
    app.coordinate(withNormalizedOffset: .zero).withOffset(CGVector(dx: point.midX, dy: point.midY)).tap()
    sleep(2)
    XCTAssertEqual(selected(strip)?.label, label, "A tap on a cell did not show its photo")
    save("03-tapped")
  }
}
