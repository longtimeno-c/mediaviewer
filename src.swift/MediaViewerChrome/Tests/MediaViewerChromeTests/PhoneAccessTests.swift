// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Phone access (PhoneAccessView.swift). This target does not import
// the chrome (its bridge resolves only in the app's link), so the card's QR
// settings are read as text and those same settings are read back by Core
// Image's own detector, at the size the card draws.
import CoreImage
import Foundation
import XCTest

final class PhoneAccessTests: XCTestCase {
  private var source: String {
    get throws {
      var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
      while dir.path != "/" {
        let file = dir.appendingPathComponent("Sources/MediaViewerChrome/PhoneAccessView.swift")
        if FileManager.default.fileExists(atPath: file.path) { return try String(contentsOf: file, encoding: .utf8) }
        dir.deleteLastPathComponent()
      }
      throw XCTSkip("PhoneAccessView.swift not found")
    }
  }

  func testTheCardAsksForCorrectionMAtTheSizeTested() throws {
    let text = try source
    XCTAssertTrue(text.contains(#"filter.setValue("M", forKey: "inputCorrectionLevel")"#))
    XCTAssertTrue(text.contains("PhoneQR.image(for: t, side: 432)"))
    XCTAssertTrue(text.contains(".interpolation(.none)"))
  }

  // Owner, 2026-11-12: no public relay will exist, and the card says so.
  func testTheRelayRowSaysItIsYourOwn() throws {
    let text = try source
    XCTAssertTrue(text.contains(#"SettingsRow(title: "Relay (your own server)""#))
    XCTAssertTrue(text.contains("There is no public one."))
    XCTAssertTrue(text.contains(#"model.send(["do": "set_relay""#))
  }

  func testALongPairingStringReadsBack() throws {
    let payload = "mediaviewer://pair#" + String(repeating: "eyJ2IjoxLCJsaWJyYXJ5X2lkIjoiYWJjIn0", count: 18)
    let filter = try XCTUnwrap(CIFilter(name: "CIQRCodeGenerator"))
    filter.setValue(Data(payload.utf8), forKey: "inputMessage")
    filter.setValue("M", forKey: "inputCorrectionLevel")
    let output = try XCTUnwrap(filter.outputImage)
    let scale = max(1, (432 / output.extent.width).rounded(.down))
    let big = output.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
    let image = try XCTUnwrap(CIContext(options: [.useSoftwareRenderer: true]).createCGImage(big, from: big.extent))
    XCTAssertGreaterThanOrEqual(image.width, 200)  // at least two pixels a module
    let side = image.width + 48  // the white quiet zone the card draws
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
