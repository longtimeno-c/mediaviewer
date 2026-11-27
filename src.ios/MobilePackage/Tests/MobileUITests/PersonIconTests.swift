// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A person's round icon on the People page is their own face, which the desktop cut as it cuts its
// People covers. It was the middle of their photo: "Nigel" (top right of his cover photo) showed
// as the baby in the middle of it.
import MobileKit
@testable import MobileUI
import UIKit
import XCTest

@MainActor
final class PersonIconTests: XCTestCase {
  private let photo = UIImage()  // the middle of it can be someone else
  private let face = UIImage()

  private func nigel(hasFace: Bool) -> Person {
    Person(id: "7", name: "Nigel", itemCount: 133,
           face: MediaItem(id: MediaID("1/42"), kind: .photo, capturedAt: Date(timeIntervalSince1970: 0),
                           pixelWidth: 512, pixelHeight: 384, duration: nil, fileName: "IMG_0042.JPG"),
           hasFace: hasFace)
  }

  func testTheIconIsTheirFaceWhenTheDesktopSentOne() async {
    let images = Images(photo: photo, face: face)
    let icon = await PersonIcon.image(for: nigel(hasFace: true), in: images)
    XCTAssertTrue(icon === face)
    XCTAssertEqual(images.thumbnailsAsked, 0)
  }

  func testAFaceThatCannotBeReadFallsBackToTheirPhoto() async {
    let images = Images(photo: photo, face: nil)
    let icon = await PersonIcon.image(for: nigel(hasFace: true), in: images)
    XCTAssertTrue(icon === photo)
  }

  func testAnOlderDesktopsCatalogueIsTheirPhotoAsBefore() async {
    let images = Images(photo: photo, face: face)
    let icon = await PersonIcon.image(for: nigel(hasFace: false), in: images)
    XCTAssertTrue(icon === photo)
    XCTAssertEqual(images.facesAsked, 0)
  }

  /// First paint never shows the whole photo while their face is on its way: a frame of the baby
  /// before Nigel is what this fixes.
  func testFirstPaintWaitsForTheFaceRatherThanShowTheirPhoto() {
    let images = Images(photo: photo, face: face)
    XCTAssertNil(PersonIcon.cached(for: nigel(hasFace: true), in: images))
    images.faceInMemory = true
    XCTAssertTrue(PersonIcon.cached(for: nigel(hasFace: true), in: images) === face)
    XCTAssertTrue(PersonIcon.cached(for: nigel(hasFace: false), in: images) === photo)
  }
}

private final class Images: ImageLadder, @unchecked Sendable {
  let photo: UIImage
  let face: UIImage?
  var faceInMemory = false
  var facesAsked = 0
  var thumbnailsAsked = 0

  init(photo: UIImage, face: UIImage?) {
    self.photo = photo
    self.face = face
  }

  func cachedImage(for id: MediaID) -> UIImage? { photo }
  func thumbnail(for item: MediaItem) async -> UIImage? {
    thumbnailsAsked += 1
    return photo
  }
  func face(for person: Person) async -> UIImage? {
    facesAsked += 1
    return face
  }
  func cachedFace(for person: Person) -> UIImage? { faceInMemory ? face : nil }
  func ladder(for item: MediaItem, fittingPixels: CGSize) -> AsyncThrowingStream<LadderEvent, Error> {
    AsyncThrowingStream { $0.finish() }
  }
  func prefetch(_ items: [MediaItem], fittingPixels: CGSize) {}
}
