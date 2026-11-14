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

  /// The Users section (PhoneUsersView.swift, docs/plans/self-host-users-smb.md §8): admin
  /// unlock first, and a typed password is handed on once and cleared, never kept.
  func testUsersAskForTheAdminFirstAndKeepNoPassword() throws {
    var dir = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
    var text = ""
    while dir.path != "/" {
      let file = dir.appendingPathComponent("Sources/MediaViewerChrome/PhoneUsersView.swift")
      if FileManager.default.fileExists(atPath: file.path) {
        text = try String(contentsOf: file, encoding: .utf8)
        break
      }
      dir.deleteLastPathComponent()
    }
    if text.isEmpty { throw XCTSkip("PhoneUsersView.swift not found") }
    XCTAssertTrue(text.contains("if locked {\n        unlockRow"))
    XCTAssertTrue(text.contains(#""do": "user_set", "name": newName, "role": newRole, "password": newPassword"#))
    XCTAssertTrue(text.contains("newPassword = \"\""))
    XCTAssertFalse(text.contains("AppStorage"))
    XCTAssertFalse(text.contains("UserDefaults"))
    XCTAssertTrue(try source.contains("PhoneUsersSection()"))
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

  // docs/plans/self-hosted.md §8 (owner 2026-11-14): three equal choices, a Synology kept.
  func testSetUpTheServiceOffersThreeChoices() throws {
    let text = try source
    XCTAssertTrue(text.contains(#"SettingsButton(title: "Set up the service…")"#))
    XCTAssertTrue(text.contains(#"Text("This computer").tag(SetupKind.thisComputer)"#))
    XCTAssertTrue(text.contains(#"Text("A Synology").tag(SetupKind.synology)"#))
    XCTAssertTrue(text.contains(#"Text("Something else").tag(SetupKind.other)"#))
    XCTAssertTrue(text.contains("Package Center → Manual Install"))
  }

  // This computer: done through the installed driftwire, on the link's thread; the token never stays.
  func testThisComputerSendsOneRequestAndForgetsTheToken() throws {
    let text = try source
    XCTAssertTrue(text.contains(#""do": "host_setup""#))
    XCTAssertTrue(text.contains(#"request["duckdns_token"] = duckToken"#))
    XCTAssertTrue(text.contains(#"duckToken = ""  // handed on"#))
    XCTAssertTrue(text.contains(#"secure("DuckDNS token""#))
    XCTAssertTrue(text.contains(#"model.send(["do": "host_start"])"#))
    XCTAssertTrue(text.contains("brew install longtimeno-c/tap/driftwire"))
    // Every new error the link can give has words.
    for code in ["no_driftwire_program", "host_no_folders", "host_no_library", "host_twice", "host_bad_name",
                 "host_bad_token", "host_share", "host_name", "host_start", "host_not_listening", "host_no_network",
                 "host_failed"] {
      XCTAssertTrue(text.contains("case \"\(code)\":"), code)
    }
    // The status row reads the host's object from the state, and never a path or an address.
    XCTAssertTrue(text.contains(#"if let h = o["host"] as? [String: Any]"#))
    XCTAssertTrue(text.contains("static func hostDetail(_ h: Host) -> String"))
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
