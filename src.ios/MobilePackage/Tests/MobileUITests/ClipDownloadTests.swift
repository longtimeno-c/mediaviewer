// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A clip too big for the connection plays once it is downloaded, from the copy on the phone. The
// server still says "too big" for it (the connection has not changed), so asking it again after
// the download put the page back where it started.
import AVFoundation
import MobileKit
@testable import MobileUI
import UIKit
import XCTest

private final class SlowLinkClips: ClipService, @unchecked Sendable {
  let file: URL
  private let lock = NSLock()
  private var routes = 0
  var routesAsked: Int { lock.withLock { routes } }

  init(file: URL) { self.file = file }

  func route(for item: MediaItem) async -> ClipRoute {
    lock.withLock { routes += 1 }
    return .tooBigToStream(neededMbps: 100, availableMbps: 5, fileBytes: 1_000)
  }

  func moments(for item: MediaItem) async -> [Moment] { [] }
  func still(of item: MediaItem, at seconds: TimeInterval) async -> UIImage? { nil }

  func download(_ item: MediaItem) -> AsyncThrowingStream<DownloadEvent, Error> {
    AsyncThrowingStream { continuation in
      continuation.yield(.progress(TransferProgress(doneBytes: 1_000, totalBytes: 1_000)))
      continuation.yield(.finished(file))
      continuation.finish()
    }
  }
}

@MainActor
final class ClipDownloadTests: XCTestCase {
  private let item = MediaItem(id: MediaID("42"), kind: .clip, capturedAt: Date(timeIntervalSince1970: 0),
                               pixelWidth: 0, pixelHeight: 0, duration: 60, fileName: "BIG.MP4")

  private func settled(until done: @escaping () -> Bool) async {
    for _ in 0..<1_000 where !done() { await Task.yield() }
  }

  func testATooBigClipPlaysFromItsDownload() async {
    let file = FileManager.default.temporaryDirectory.appendingPathComponent("BIG.MP4")
    let clips = SlowLinkClips(file: file)
    let clip = ClipModel(item: item, clips: clips)
    await clip.load(startAt: nil)
    guard case .tooBig = clip.phase else { return XCTFail("expected too big, got \(clip.phase)") }

    clip.startDownload()
    await settled { clip.phase == .playable }
    XCTAssertEqual(clip.phase, .playable, "the download plays, not back to \"too big\"")
    XCTAssertNil(clip.download)
    XCTAssertEqual((clip.player.currentItem?.asset as? AVURLAsset)?.url, file, "played from the copy on the phone")
    XCTAssertEqual(clips.routesAsked, 1, "the server is not asked again")

    // Try again after a stop also plays the copy.
    clip.retry()
    await settled { clip.phase == .playable }
    XCTAssertEqual(clip.phase, .playable)
    XCTAssertEqual(clips.routesAsked, 1)
    clip.stop()
  }
}
