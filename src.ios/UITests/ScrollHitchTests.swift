// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §13: hitch measurements on the 20,000-item fixture.
//
// Two measures. `FrameHitchMetric` counts late display-link frames on the main run loop, in the
// Simulator and on a phone. Apple's scroll metric adds real hitch figures only on a device; in the
// Simulator it reports a duration alone.
//
// A Simulator number shows a change for better or worse on this Mac. It is not a phone number: the
// GPU, the thermals and the display are the Mac's. M6's gate has to be run on a phone.
import XCTest

private struct FrameStats {
  let hitches: Double
  let hitchMs: Double
  let elapsedMs: Double
  let worstMs: Double
  let expectedMs: Double

  init?(_ text: String) {
    var values: [String: Double] = [:]
    for pair in text.split(separator: " ") {
      let parts = pair.split(separator: "=")
      if parts.count == 2 { values[String(parts[0])] = Double(parts[1]) }
    }
    guard let hitches = values["hitches"], let hitchMs = values["hitchMs"], let elapsedMs = values["elapsedMs"],
          let worstMs = values["worstMs"], let expectedMs = values["expectedMs"] else { return nil }
    self.hitches = hitches
    self.hitchMs = hitchMs
    self.elapsedMs = elapsedMs
    self.worstMs = worstMs
    self.expectedMs = expectedMs
  }
}

/// Hitch time per second of swiping, read from the fixtures app's frame meter.
private final class FrameHitchMetric: NSObject, XCTMetric {
  private let app: XCUIApplication
  private var first: FrameStats?
  private var last: FrameStats?

  init(app: XCUIApplication) { self.app = app }

  func copy(with zone: NSZone? = nil) -> Any { FrameHitchMetric(app: app) }

  private func read() -> FrameStats? {
    FrameStats(app.staticTexts["frame-stats"].label)
  }

  func willBeginMeasuring() { first = read() }

  func didStopMeasuring() { last = read() }

  func reportMeasurements(from startTime: XCTPerformanceMeasurementTimestamp,
                          to endTime: XCTPerformanceMeasurementTimestamp) throws -> [XCTPerformanceMeasurement] {
    guard let first, let last, last.elapsedMs > first.elapsedMs else { return [] }
    let seconds = (last.elapsedMs - first.elapsedMs) / 1000
    return [
      XCTPerformanceMeasurement(identifier: "mv.hitch.ratio", displayName: "Hitch time per second",
                                doubleValue: (last.hitchMs - first.hitchMs) / seconds, unitSymbol: "ms/s"),
      XCTPerformanceMeasurement(identifier: "mv.hitch.count", displayName: "Hitches",
                                doubleValue: last.hitches - first.hitches, unitSymbol: "count"),
      XCTPerformanceMeasurement(identifier: "mv.hitch.worst", displayName: "Worst frame so far",
                                doubleValue: last.worstMs, unitSymbol: "ms"),
      XCTPerformanceMeasurement(identifier: "mv.hitch.expected", displayName: "Frame time the display asked for",
                                doubleValue: last.expectedMs, unitSymbol: "ms"),
    ]
  }
}

final class ScrollHitchTests: XCTestCase {
  private func launched(_ state: String) -> XCUIApplication {
    let app = XCUIApplication()
    app.launchArguments = ["-fixtureState", state, "-fixtureChrome", "NO", "-fixtureFrameMeter", "YES"]
    app.launch()
    return app
  }

  /// On a ProMotion phone the gate means nothing unless the app is really running at 120 Hz.
  private func assertRunsAtFullRate(_ app: XCUIApplication) throws {
    #if !targetEnvironment(simulator)
    guard UIScreen.main.maximumFramesPerSecond >= 120 else { return }
    let stats = FrameStats(app.staticTexts["frame-stats"].label)
    let expected = try XCTUnwrap(stats?.expectedMs)
    XCTAssertLessThan(expected, 9.5, "Expected about 8.3 ms a frame; check CADisableMinimumFrameDurationOnPhone.")
    #endif
  }

  private var options: XCTMeasureOptions {
    let options = XCTMeasureOptions()
    options.iterationCount = 5
    return options
  }

  private func metrics(_ app: XCUIApplication) -> [XCTMetric] {
    [FrameHitchMetric(app: app), XCTOSSignpostMetric.scrollingAndDecelerationMetric]
  }

  func testLibraryGridScrollHitches() throws {
    let app = launched("library")
    XCTAssertTrue(app.buttons["All media"].waitForExistence(timeout: 30))
    XCTAssertTrue(app.staticTexts["frame-stats"].waitForExistence(timeout: 10))
    sleep(2)
    try assertRunsAtFullRate(app)
    // The middle of the screen is the grid; the chip row above it scrolls sideways.
    measure(metrics: metrics(app), options: options) {
      for _ in 0..<4 { app.swipeUp(velocity: .fast) }
    }
  }

  func testViewerPagingHitches() throws { try pageThrough("viewer") }

  /// No full-size pictures arrive here, so the difference from the test above is what they cost.
  func testViewerPagingHitchesWithoutOriginals() throws { try pageThrough("viewer.gone") }

  private func pageThrough(_ state: String) throws {
    let app = launched(state)
    XCTAssertTrue(app.buttons["Close"].waitForExistence(timeout: 30))
    XCTAssertTrue(app.staticTexts["frame-stats"].waitForExistence(timeout: 10))
    sleep(2)
    try assertRunsAtFullRate(app)
    measure(metrics: metrics(app), options: options) {
      for _ in 0..<6 { app.swipeLeft(velocity: .fast) }
    }
  }
}
