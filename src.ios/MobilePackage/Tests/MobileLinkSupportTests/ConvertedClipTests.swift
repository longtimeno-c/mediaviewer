// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileLinkSupport
import XCTest

final class ConvertedClipTests: XCTestCase {
  func testThePlaylistCoversTheClipInItsSegments() {
    // 10.5 s in 4 s segments: 4, 4, 2.5.
    let plan = ConvertPlan(durationMs: 10_500, segmentMs: 4000, segments: 3, width: 1280, height: 720)
    let text = plan.playlist { "\($0).ts" }
    XCTAssertTrue(text.hasPrefix("#EXTM3U\n"))
    XCTAssertTrue(text.contains("#EXT-X-TARGETDURATION:4\n"))
    XCTAssertTrue(text.contains("#EXT-X-PLAYLIST-TYPE:VOD\n"))
    XCTAssertTrue(text.hasSuffix("#EXT-X-ENDLIST\n"))
    let infs = text.split(separator: "\n").filter { $0.hasPrefix("#EXTINF:") }
    XCTAssertEqual(infs, ["#EXTINF:4.000,", "#EXTINF:4.000,", "#EXTINF:2.500,"])
    XCTAssertTrue(text.contains("\n2.ts\n"))
    XCTAssertFalse(text.contains("3.ts"))
  }

  func testATailJoinedToTheLastSegmentRaisesTheTarget() {
    // 8.2 s, the 0.2 s tail joined: 4 and 4.2, which rounds up to a target of 5.
    let plan = ConvertPlan(durationMs: 8_200, segmentMs: 4000, segments: 2, width: 640, height: 360)
    let text = plan.playlist { "s\($0)" }
    XCTAssertTrue(text.contains("#EXT-X-TARGETDURATION:5\n"))
    XCTAssertTrue(text.contains("#EXTINF:4.200,\ns1\n"))
    XCTAssertEqual(plan.seconds(of: 2), 0)
  }

  func testAPostersPlaylistHoldsTheFirstSegmentAlone() {
    let plan = ConvertPlan(durationMs: 30_000, segmentMs: 4000, segments: 8, width: 1920, height: 1080)
    let text = plan.firstSegment.playlist { "\($0).ts" }
    XCTAssertEqual(text.split(separator: "\n").filter { $0.hasPrefix("#EXTINF:") }, ["#EXTINF:4.000,"])
    XCTAssertFalse(text.contains("1.ts"))
    let short = ConvertPlan(durationMs: 2_500, segmentMs: 4000, segments: 1, width: 1, height: 1).firstSegment
    XCTAssertEqual(short.durationMs, 2_500)
  }
}
