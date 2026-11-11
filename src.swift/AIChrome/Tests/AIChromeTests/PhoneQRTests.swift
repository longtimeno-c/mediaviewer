// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The QR a phone scans survives being read back (docs/plans/mobile.md §7.4).
import CoreImage
import XCTest
@testable import AIChrome

final class PhoneQRTests: XCTestCase {
  func testANewFolderIsNamedAfterItsShare() {
    XCTAssertEqual(PhoneLinkModel.shareLabel(forPath: "/volume1/Photos"), "Photos")
    XCTAssertEqual(PhoneLinkModel.shareLabel(forPath: "/volume2/Video/2024/"), "Video")
    XCTAssertEqual(PhoneLinkModel.shareLabel(forPath: "/Users/me/Library"), "Library")
    XCTAssertEqual(PhoneLinkModel.shareLabel(forPath: "/"), "")
  }

  func testALongPairingStringReadsBack() throws {
    let payload = "mediaviewer://pair#" + String(repeating: "eyJ2IjoxLCJsaWJyYXJ5X2lkIjoiYWJjIn0", count: 18)
    let image = try XCTUnwrap(PhoneQR.image(for: payload, side: 368))
    XCTAssertGreaterThanOrEqual(image.width, 200)  // at least two pixels a module
    // The white quiet zone the card draws around it.
    let side = image.width + 48
    let ctx = try XCTUnwrap(CGContext(data: nil, width: side, height: side, bitsPerComponent: 8, bytesPerRow: 0,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    ctx.setFillColor(.white)
    ctx.fill(CGRect(x: 0, y: 0, width: side, height: side))
    ctx.draw(image, in: CGRect(x: 24, y: 24, width: image.width, height: image.height))
    let drawn = try XCTUnwrap(ctx.makeImage())
    let detector = try XCTUnwrap(CIDetector(ofType: CIDetectorTypeQRCode, context: nil,
                                            options: [CIDetectorAccuracy: CIDetectorAccuracyHigh]))
    let found = detector.features(in: CIImage(cgImage: drawn)).compactMap { ($0 as? CIQRCodeFeature)?.messageString }
    XCTAssertEqual(found, [payload])
  }
}
