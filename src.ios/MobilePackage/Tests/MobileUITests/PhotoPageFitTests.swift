// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A photo's page fits the picture's own shape to the screen. A folder's file says nothing of its
// shape, and the page once took that for a square: a portrait photo sat small inside it, with black
// all round, and stayed that size with the controls hidden.
import MobileKit
@testable import MobileUI
import UIKit
import XCTest

@MainActor
final class PhotoPageFitTests: XCTestCase {
  private let screen = CGSize(width: 390, height: 844)
  private let gap: CGFloat = 20

  private func photo(width: Int = 0, height: Int = 0, hint: Double? = nil) -> GridEntry {
    GridEntry(item: MediaItem(id: MediaID("file:r/IMG_0001.JPG"), kind: .photo, capturedAt: Date(timeIntervalSince1970: 0),
                              pixelWidth: width, pixelHeight: height, duration: nil, fileName: "IMG_0001.JPG",
                              aspectHint: hint))
  }

  private func picture(_ width: CGFloat, _ height: CGFloat) -> UIImage {
    let format = UIGraphicsImageRendererFormat()
    format.scale = 1
    return UIGraphicsImageRenderer(size: CGSize(width: width, height: height), format: format).image { _ in }
  }

  /// Shows `entry` on a screen-sized page and waits for its ladder to finish.
  private func page(_ entry: GridEntry, cached: UIImage?, ladder: [UIImage]) async -> ZoomPageCell {
    let cell = ZoomPageCell(frame: CGRect(origin: .zero, size: CGSize(width: screen.width + gap, height: screen.height)))
    let done = expectation(description: "ladder finished")
    cell.show(entry: entry, images: Ladder(cached: cached, steps: ladder), trailingGap: gap, fittingPixels: screen,
              onTap: {}, onPlay: {}, onZoomed: { _ in },
              onState: { _, state in if state == .finished { done.fulfill() } },
              holdsRefinement: { false })
    cell.layoutIfNeeded()
    await fulfillment(of: [done], timeout: 5)
    cell.layoutIfNeeded()
    return cell
  }

  func testAPortraitPhotoOfUnknownShapeFillsTheScreenWidth() async {
    let cell = await page(photo(), cached: picture(300, 400), ladder: [])
    XCTAssertEqual(cell.pictureFrame.size.width, screen.width, accuracy: 0.5)
    XCTAssertEqual(cell.pictureFrame.size.height, screen.width * 4 / 3, accuracy: 0.5)
  }

  func testTheOriginalsShapeWinsOverAWrongGuess() async {
    // The interim library lists every photo as 4:3; this one is portrait.
    let cell = await page(photo(width: 4, height: 3), cached: nil, ladder: [picture(1500, 2000)])
    XCTAssertEqual(cell.pictureFrame.size.width, screen.width, accuracy: 0.5)
    XCTAssertEqual(cell.pictureFrame.size.height, screen.width * 4 / 3, accuracy: 0.5)
  }

  func testAKnownShapeHoldsWhileNoPictureHasCome() {
    XCTAssertEqual(ZoomPageCell.shape(of: photo(width: 3000, height: 2000).item, picture: nil), 1.5)
    XCTAssertEqual(ZoomPageCell.fit(shape: 1.5, in: screen), CGSize(width: 390, height: 260))
    XCTAssertEqual(ZoomPageCell.fit(shape: 0.25, in: screen), CGSize(width: 211, height: 844))
  }
}

private final class Ladder: ImageLadder, @unchecked Sendable {
  let cached: UIImage?
  let steps: [UIImage]
  init(cached: UIImage?, steps: [UIImage]) {
    self.cached = cached
    self.steps = steps
  }

  func cachedImage(for id: MediaID) -> UIImage? { cached }
  func thumbnail(for item: MediaItem) async -> UIImage? { cached }
  func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> {
    AsyncThrowingStream { continuation in
      for (i, step) in steps.enumerated() { continuation.yield(.image(step, isOriginal: i == steps.count - 1)) }
      continuation.finish()
    }
  }
  func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {}
  func length(for item: MediaItem) async -> TimeInterval? { nil }
}
