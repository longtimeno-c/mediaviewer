// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A clip tile's length: the one it was listed with, else one learnt since (a folder's clip, from
// the header its poster read), and nothing (never 0:00) while none is known.
import MobileKit
@testable import MobileUI
import XCTest

final class ClipBadgeTests: XCTestCase {
  private func clip(_ duration: TimeInterval?) -> GridEntry {
    GridEntry(item: MediaItem(id: MediaID("file:r/GX010001.MP4"), kind: .clip, capturedAt: Date(timeIntervalSince1970: 0),
                              pixelWidth: 0, pixelHeight: 0, duration: duration, fileName: "GX010001.MP4"))
  }

  func testAFolderClipShowsTheLengthItLearnt() {
    XCTAssertNil(MediaTile.badgeText(clip(nil), learnt: nil), "no length yet: no time, not 0:00")
    XCTAssertEqual(MediaTile.badgeText(clip(nil), learnt: 83.9), "1:23")
  }

  func testAListedLengthIsShownAsListed() {
    XCTAssertEqual(MediaTile.badgeText(clip(5), learnt: nil), "0:05")
    XCTAssertEqual(MediaTile.badgeText(clip(5), learnt: 7), "0:05")
  }
}
