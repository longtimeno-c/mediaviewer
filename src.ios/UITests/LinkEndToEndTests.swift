// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The real app against `driftwire serve` on loopback (docs/plans/mobile.md M3, M4, M6–M8). Needs a
// real published catalogue and the driver in tools/ios: it runs only when MV_E2E_PAYLOAD is set.
import XCTest

final class LinkEndToEndTests: XCTestCase {
  private let bundle = "io.github.longtimeno-c.mediaviewer.mobile"

  private func save(_ name: String) {
    guard let dir = ProcessInfo.processInfo.environment["MV_E2E_OUT"] else { return }
    let url = URL(fileURLWithPath: dir).appendingPathComponent(name + ".png")
    try? XCUIScreen.main.screenshot().pngRepresentation.write(to: url)
  }

  private func requirePayload() throws -> String {
    guard let code = ProcessInfo.processInfo.environment["MV_E2E_PAYLOAD"] else {
      throw XCTSkip("Run through tools/ios/link-e2e.sh")
    }
    return code
  }

  /// Pairs, confirms the fingerprint, sees the copy interrupted by the driver killing the service,
  /// retries it and ends on the grid.
  func test1_PairThenCopyThroughAnInterruption() throws {
    let code = try requirePayload()
    let app = XCUIApplication(bundleIdentifier: bundle)
    app.launchEnvironment["MV_TEST_SCAN_CODE"] = code
    app.launch()

    let match = app.buttons["They match"]
    XCTAssertTrue(match.waitForExistence(timeout: 30), "The code did not reach the check screen")
    XCTAssertTrue(app.staticTexts["E2E Library"].exists)
    save("1-check")
    match.tap()

    // The driver kills the service mid-copy; "Try again" appears; the driver has restarted it.
    let again = app.buttons["Try again"]
    let grid = app.collectionViews.firstMatch
    let deadline = Date().addingTimeInterval(120)
    var sawRetry = false
    let carryOn = app.buttons["Continue"]  // the Simulator has no mobile data: the outside test is skipped
    while Date() < deadline {
      if carryOn.exists {
        save("1b-outside-skipped")
        carryOn.tap()
      }
      if again.exists {
        sawRetry = true
        save("2-interrupted")
        sleep(5)  // the driver brings it back within this
        again.tap()
        sleep(1)
        save("3-resuming")
        break
      }
      if grid.exists && app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Photo,' OR label BEGINSWITH 'Clip,'")).count > 0 { break }
      usleep(100_000)
    }
    XCTAssertTrue(sawRetry, "The copy was not interrupted")
    let tile = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Clip,'")).firstMatch
    XCTAssertTrue(tile.waitForExistence(timeout: 120), "The grid did not appear after the retry")
    save("4-grid")
    sleep(3)  // JPEG-512s arrive
    save("5-grid-thumbs")
  }

  /// The viewer: a photo refines to its original; the clip plays, seeks and jumps to a moment.
  func test2_ViewerAndClip() throws {
    _ = try requirePayload()
    let app = XCUIApplication(bundleIdentifier: bundle)
    app.launch()

    let clip = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Clip,'")).firstMatch
    XCTAssertTrue(clip.waitForExistence(timeout: 30), "No library after relaunch")
    let photo = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Photo,'")).firstMatch
    XCTAssertTrue(photo.exists)
    photo.tap()
    XCTAssertTrue(app.buttons["viewer-close"].waitForExistence(timeout: 15), "The photo did not open")
    sleep(6)  // the original over the link, downscaled
    save("6-photo-original")
    app.buttons["viewer-close"].tap()
    sleep(1)

    clip.tap()
    let position = app.descendants(matching: .any).matching(identifier: "Position").firstMatch
    let seconds: () -> Int? = {
      guard let text = position.value as? String else { return nil }
      let parts = text.split(separator: ":").compactMap { Int($0) }
      return parts.count == 2 ? parts[0] * 60 + parts[1] : nil
    }
    XCTAssertTrue(position.waitForExistence(timeout: 30), "The clip did not open")
    XCTAssertTrue(app.buttons["Pause"].waitForExistence(timeout: 30), "The clip did not start")
    app.buttons["Pause"].tap()
    XCTAssertTrue(app.buttons["Play"].waitForExistence(timeout: 10))
    save("7-clip-paused")
    let before = try XCTUnwrap(seconds())
    app.buttons["Forward 10 seconds"].tap()
    sleep(3)
    let after = try XCTUnwrap(seconds())
    XCTAssertEqual(after, min(before + 10, 30), accuracy: 2, "The seek did not land")
    save("8-clip-after-seek")

    let moment = app.buttons.matching(NSPredicate(format: "label CONTAINS 'moment to jump'")).firstMatch
    XCTAssertTrue(moment.waitForExistence(timeout: 10), "The speech moment was not listed")
    moment.tap()
    sleep(3)
    let landed = try XCTUnwrap(seconds())
    XCTAssertEqual(landed, 12, accuracy: 1, "The moment jump missed")
    save("9-clip-after-moment")
  }
}
