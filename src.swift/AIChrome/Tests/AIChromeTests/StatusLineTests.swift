// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The search footer's status line from a status struct (issue #417): with
// iCloud clips still queued, the pictures count and ETA are named as this
// Mac's files and the clips as the rest of the same run; with the queue empty
// the line is the plain pictures / sound / speech line.
//   cd src.swift/AIChrome && swift test -c release -Xswiftc -enable-testing --filter StatusLineTests
import XCTest
@testable import AIChrome
import CAiApi

final class StatusLineTests: XCTestCase {
  /// A Photos library run with the sound and speech passes nearly done, as
  /// the report's screenshot: 24,443 assets, one still to handle on this Mac.
  private func status(done: UInt64 = 24_442, total: UInt64 = 24_443,
                      left: UInt64, fetch: mv_ai_icloud_fetch,
                      progress: Float = 0, eta: Double = 30) -> mv_ai_status {
    var s = mv_ai_status()
    s.state = MV_AI_STATE_INDEXING.rawValue
    s.assets_total = total
    s.assets_done = done
    s.sound_total = 100
    s.sound_done = 99
    s.speech_total = 100
    s.speech_done = 99
    s.eta_low_seconds = eta
    s.eta_high_seconds = eta
    s.icloud_videos_left = left
    s.icloud_fetch = fetch.rawValue
    s.icloud_fetch_progress = progress
    return s
  }

  func testQueuedClipsAreTheRestOfTheRun() {
    let line = StatusLine(status(left: 1_833, fetch: MV_AI_ICLOUD_WAIT_INDEXER))
    XCTAssertEqual(line.text, "Indexing pictures 24,442 of 24,443 on this Mac · under a minute")
    XCTAssertEqual(line.detail,
                   "Then 1,833 clips from iCloud · the next are downloaded and wait for indexing to catch up")
    XCTAssertFalse(line.detail.contains("left to download"))
  }

  func testEveryWaitSaysWhyWhileLocalFilesRun() {
    let cases: [(mv_ai_icloud_fetch, String)] = [
      (MV_AI_ICLOUD_DOWNLOADING, "Then 1,833 clips from iCloud · downloading the next (42%), each indexed as it lands"),
      (MV_AI_ICLOUD_WAIT_NETWORK, "Then 1,833 clips from iCloud · downloads wait for an unmetered network"),
      (MV_AI_ICLOUD_WAIT_POWER, "Then 1,833 clips from iCloud · downloads wait for power"),
      (MV_AI_ICLOUD_LOW_DISK, "Then 1,833 clips from iCloud · downloads wait: under 10 GB free"),
    ]
    for (fetch, expected) in cases {
      let line = StatusLine(status(left: 1_833, fetch: fetch, progress: 0.425))
      XCTAssertTrue(line.text.hasSuffix(" on this Mac · under a minute"), line.text)
      XCTAssertEqual(line.detail, expected)
    }
  }

  func testEmptyQueueIsTodaysLine() {
    for fetch in [MV_AI_ICLOUD_OFF, MV_AI_ICLOUD_DONE] {
      let line = StatusLine(status(left: 0, fetch: fetch))
      XCTAssertEqual(line.text, "Indexing pictures 24,442 of 24,443 · under a minute")
      XCTAssertEqual(line.detail, "")
    }
  }

  func testClipsPutOffToNextLaunchAreNotThisRun() {
    let line = StatusLine(status(left: 12, fetch: MV_AI_ICLOUD_RETRY_LATER))
    XCTAssertEqual(line.text, "Indexing pictures 24,442 of 24,443 · under a minute")
    XCTAssertEqual(line.detail, "iCloud did not send 12 clips; they are tried again next time MediaViewer starts")
  }

  func testNothingLocalLeftHeadlinesTheDownload() {
    var s = status(done: 24_443, left: 1_833, fetch: MV_AI_ICLOUD_WAIT_INDEXER)
    s.sound_done = 100
    s.speech_done = 100
    let line = StatusLine(s)
    XCTAssertEqual(line.text, "Downloading from iCloud · 1,833 clips left")
    XCTAssertEqual(line.detail,
                   "iCloud downloads wait for indexing to catch up · 1,833 clips left to download and index")
  }

  /// #354 D4: the scanned pages' pass runs after every other one.
  func testScannedPagesAreTheLastPass() {
    var s = status(done: 24_443, left: 0, fetch: MV_AI_ICLOUD_OFF)
    s.sound_done = 100
    s.speech_done = 100
    s.docs_total = 5
    s.docs_done = 5
    s.scans_total = 5
    s.scans_done = 3
    XCTAssertEqual(StatusLine(s).text, "Reading scanned pages 3 of 5 documents")
    s.speech_done = 99
    XCTAssertTrue(StatusLine(s).text.hasPrefix("Indexing speech 99 of 100 clips"))
  }

  func testSoundPassIsUntouched() {
    var s = status(done: 24_443, left: 1_833, fetch: MV_AI_ICLOUD_WAIT_INDEXER)
    s.sound_done = 40
    let line = StatusLine(s)
    XCTAssertEqual(line.text, "Indexing sound 40 of 100 clips")
    XCTAssertEqual(line.stages.map(\.name), ["Pictures", "Speech"])
  }
}
