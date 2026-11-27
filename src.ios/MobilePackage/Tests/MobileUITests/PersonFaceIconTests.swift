// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A person's round icon on the People page is their own face, cut as the desktop cuts its covers.
// It was the middle of the photo: "Nigel" (top right of his cover photo) showed as the baby in the
// middle of it.
import MobileKit
@testable import MobileUI
import SwiftUI
import UIKit
import XCTest

@MainActor
final class PersonFaceIconTests: XCTestCase {
  private let photo = CGSize(width: 512, height: 384)
  private let nigel = CGRect(x: 0.7, y: 0.05, width: 0.2, height: 0.25)  // top right
  private let baby = CGRect(x: 0.4, y: 0.4, width: 0.2, height: 0.2)    // the middle

  func testTheSquareIsTheFaceWithTheDesktopsMargin() {
    let crop = FaceCrop.square(of: photo, box: nigel)
    // As AIChrome's Thumbs.crop: 1.6 times the face's longer side (102.4 px wide), squared.
    XCTAssertEqual(crop.width, 102.4 * 1.6, accuracy: 0.001)
    XCTAssertEqual(crop.height, crop.width)
    XCTAssertEqual(crop.midX, 0.8 * 512, accuracy: 0.001)
    XCTAssertEqual(crop.minY, 0)  // held inside the picture, not centred above it
    let face = CGRect(x: nigel.minX * 512, y: nigel.minY * 384, width: nigel.width * 512, height: nigel.height * 384)
    XCTAssertTrue(crop.contains(face))
    let middle = CGRect(x: baby.minX * 512, y: baby.minY * 384, width: baby.width * 512, height: baby.height * 384)
    XCTAssertFalse(crop.intersects(middle))
  }

  func testWithNoBoxTheMiddleSquareAsBefore() {
    XCTAssertEqual(FaceCrop.square(of: photo, box: nil), CGRect(x: 64, y: 0, width: 384, height: 384))
    XCTAssertEqual(FaceCrop.square(of: photo, box: .zero), CGRect(x: 64, y: 0, width: 384, height: 384))
  }

  func testAFaceTooBigForAMarginIsTheWholeShortSide() {
    let crop = FaceCrop.square(of: photo, box: CGRect(x: 0.3, y: 0.1, width: 0.5, height: 0.8))
    XCTAssertEqual(crop.width, 384)
    XCTAssertGreaterThanOrEqual(crop.minX, 0)
    XCTAssertLessThanOrEqual(crop.maxX, 512)
  }

  /// The drawn icon: the middle of an 84 pt cell is Nigel's red, not the baby's blue.
  func testTheIconShowsTheirFaceNotTheMiddleOfThePhoto() throws {
    let format = UIGraphicsImageRendererFormat()
    format.scale = 1
    let picture = UIGraphicsImageRenderer(size: photo, format: format).image { ctx in
      UIColor.gray.setFill()
      ctx.fill(CGRect(origin: .zero, size: photo))
      UIColor.blue.setFill()
      ctx.fill(CGRect(x: baby.minX * 512, y: baby.minY * 384, width: baby.width * 512, height: baby.height * 384))
      UIColor.red.setFill()
      ctx.fill(CGRect(x: nigel.minX * 512, y: nigel.minY * 384, width: nigel.width * 512, height: nigel.height * 384))
    }
    XCTAssertEqual(try centre(of: FaceIcon(image: picture, box: nigel)), .red)
    XCTAssertEqual(try centre(of: FaceIcon(image: picture, box: nil)), .blue)  // an older desktop's catalogue
  }

  private enum Hue { case red, blue, other }

  private func centre(of icon: FaceIcon) throws -> Hue {
    let renderer = ImageRenderer(content: icon.frame(width: 84, height: 84))
    renderer.scale = 1
    let image = try XCTUnwrap(renderer.cgImage)
    var pixel = [UInt8](repeating: 0, count: 4)
    let ctx = try XCTUnwrap(CGContext(data: &pixel, width: 1, height: 1, bitsPerComponent: 8, bytesPerRow: 4,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    ctx.draw(image, in: CGRect(x: -42, y: -42, width: image.width, height: image.height))
    if pixel[0] > 200 && pixel[2] < 60 { return .red }
    if pixel[2] > 200 && pixel[0] < 60 { return .blue }
    return .other
  }
}
