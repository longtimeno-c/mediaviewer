// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File mode (docs/plans/phone-in-the-app.md §4): path ids, when a library is in file mode, the tags
// ImageIO reads from a file's head, and the tag cache by path, size and mtime.
import ImageIO
import MobileLinkSupport
import UniformTypeIdentifiers
import XCTest

final class FilePathTests: XCTestCase {
  func testAnIDRoundTrips() {
    for (root, rel) in [("a1a1a1a1a1a1a1a1", "Trip/IMG_0001.JPG"), ("r", ""), ("r", "a/b/c d é/x.heic"),
                        ("0123456789abcdef", "2024/Crème brûlée.jpg")] {
      let path = FilePath(root: root, rel: rel)
      XCTAssertTrue(path.id.hasPrefix("file:"))
      XCTAssertEqual(FilePath(id: path.id), path)
      XCTAssertTrue(FilePath.isFile(path.id))
    }
  }

  func testACatalogueIDIsNeverAFile() {
    for raw in ["1234", "", "file:", "file:abc", "file:/x", "files:a/b"] {
      XCTAssertNil(FilePath(id: raw), raw)
    }
    XCTAssertFalse(FilePath.isFile("1234"))
  }

  func testNamesParentsAndChildren() {
    let top = FilePath(root: "r", rel: "")
    let trip = top.appending("Trip")
    let file = trip.appending("a.jpg")
    XCTAssertEqual(trip.rel, "Trip")
    XCTAssertEqual(file.rel, "Trip/a.jpg")
    XCTAssertEqual(file.name, "a.jpg")
    XCTAssertEqual(file.parent, trip)
    XCTAssertEqual(trip.parent, top)
    XCTAssertNil(top.parent)
    XCTAssertEqual(top.name, "")
  }

  func testTheStampChangesWithSizeAndTime() {
    let p = FilePath(root: "r", rel: "a.jpg")
    XCTAssertEqual(p.stamp(size: 10, mtime: 5), p.stamp(size: 10, mtime: 5))
    XCTAssertNotEqual(p.stamp(size: 10, mtime: 5), p.stamp(size: 11, mtime: 5))
    XCTAssertNotEqual(p.stamp(size: 10, mtime: 5), p.stamp(size: 10, mtime: 6))
    XCTAssertNotEqual(p.stamp(size: 10, mtime: 5), FilePath(root: "r", rel: "b.jpg").stamp(size: 10, mtime: 5))
  }
}

final class FileModeRuleTests: XCTestCase {
  func testNoCatalogueOnTheNASIsFileMode() {
    XCTAssertTrue(FileModeRule.fileMode(afterCheck: FileModeRule.noCatalogue, hasCatalogue: false, wasFileMode: false))
  }

  func testACatalogueOnThePhoneAlwaysWins() {
    XCTAssertFalse(FileModeRule.fileMode(afterCheck: FileModeRule.noCatalogue, hasCatalogue: true, wasFileMode: true))
  }

  func testACheckThatCannotTellKeepsWhatWasKnown() {
    for outcome: Int32 in [3, 11, FileModeRule.copyAvailable, 19] {
      XCTAssertTrue(FileModeRule.fileMode(afterCheck: outcome, hasCatalogue: false, wasFileMode: true))
      XCTAssertFalse(FileModeRule.fileMode(afterCheck: outcome, hasCatalogue: false, wasFileMode: false))
    }
  }

  func testAnInstalledCatalogueEndsIt() {
    XCTAssertFalse(FileModeRule.fileMode(afterCheck: FileModeRule.installed, hasCatalogue: false, wasFileMode: true))
  }
}

final class FileTagsTests: XCTestCase {
  /// A real JPEG with EXIF, IPTC and XMP written by ImageIO, so the read is checked against what
  /// another writer put there.
  private func taggedJPEG(side: Int = 1200) throws -> Data {
    let context = CGContext(data: nil, width: side, height: side, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    for y in stride(from: 0, to: side, by: 16) {
      context.setFillColor(red: CGFloat(y % 256) / 255, green: 0.4, blue: 0.7, alpha: 1)
      context.fill(CGRect(x: 0, y: y, width: side, height: 8))
    }
    let image = context.makeImage()!
    let data = NSMutableData()
    let destination = try XCTUnwrap(CGImageDestinationCreateWithData(data, UTType.jpeg.identifier as CFString, 1, nil))
    let metadata = CGImageMetadataCreateMutable()
    for (path, value) in [("dc:title", "Lisbon at dusk")] {
      XCTAssertTrue(CGImageMetadataSetValueWithPath(metadata, nil, path as CFString, value as CFString))
    }
    XCTAssertTrue(CGImageMetadataSetValueWithPath(metadata, nil, "dc:subject" as CFString, ["Portugal", "Trams"] as CFArray))
    let properties: [CFString: Any] = [
      kCGImagePropertyTIFFDictionary: [kCGImagePropertyTIFFMake: "Canon", kCGImagePropertyTIFFModel: "Canon EOS R5",
                                       kCGImagePropertyTIFFSoftware: "ChatGPT", kCGImagePropertyTIFFArtist: "Ana Sá"],
      kCGImagePropertyExifDictionary: [kCGImagePropertyExifLensModel: "RF24-70mm F2.8 L IS USM",
                                       kCGImagePropertyExifUserComment: "by the cathedral"],
      kCGImagePropertyIPTCDictionary: [kCGImagePropertyIPTCCaptionAbstract: "Tram 28 at the Sé"],
    ]
    CGImageDestinationAddImageAndMetadata(destination, image, metadata, properties as CFDictionary)
    XCTAssertTrue(CGImageDestinationFinalize(destination))
    return data as Data
  }

  func testTheTagsAreReadUnderTheDesktopsNames() throws {
    let values = try XCTUnwrap(FileTags.read(head: taggedJPEG()))
    XCTAssertEqual(values.make, "Canon")
    XCTAssertEqual(values.model, "Canon EOS R5")
    XCTAssertEqual(values.lens, "RF24-70mm F2.8 L IS USM")
    XCTAssertEqual(values.comment, "by the cathedral")
    let tags = Dictionary(grouping: values.tags, by: \.rawTag).mapValues { $0.map(\.value) }
    XCTAssertEqual(tags["Exif.Image.Make"], ["Canon"])
    XCTAssertEqual(tags["Exif.Image.Software"], ["ChatGPT"])
    XCTAssertEqual(tags["Exif.Image.Artist"], ["Ana Sá"])
    // ImageIO's writer keeps IPTC and XMP in step (dc:subject is also the IPTC keywords, Software
    // also xmp:CreatorTool): each is read under its own name, and the desktop's builder drops repeats.
    XCTAssertEqual(tags["Iptc.Application2.Keywords"] ?? ["Portugal", "Trams"], ["Portugal", "Trams"])
    XCTAssertEqual(tags["Iptc.Application2.Caption"], ["Tram 28 at the Sé"])
    XCTAssertEqual(tags["Xmp.dc.title"], ["Lisbon at dusk"])
    XCTAssertEqual(tags["Xmp.xmp.CreatorTool"] ?? ["ChatGPT"], ["ChatGPT"])
    XCTAssertEqual(tags["Xmp.dc.subject"], ["Portugal, Trams"])
  }

  func testTheHeadAloneIsEnough() throws {
    let whole = try taggedJPEG(side: 2400)
    let head = whole.prefix(min(whole.count, 32 * 1024))
    XCTAssertLessThan(head.count, whole.count)
    let values = try XCTUnwrap(FileTags.read(head: Data(head)))
    XCTAssertEqual(values.model, "Canon EOS R5")
    XCTAssertTrue(values.tags.contains(FileTag("Xmp.dc.title", "Lisbon at dusk")))
  }

  func testNotAPictureIsNothing() {
    XCTAssertNil(FileTags.read(head: Data("not a picture at all".utf8)))
  }

  func testPaddedAndEmptyValuesAreCleaned() {
    let values = FileTags.values(properties: [kCGImagePropertyTIFFDictionary: [kCGImagePropertyTIFFMake: "SONY\0\0 ",
                                                                               kCGImagePropertyTIFFSoftware: "   "]],
                                 metadata: nil)
    XCTAssertEqual(values.make, "SONY")
    XCTAssertEqual(values.tags, [FileTag("Exif.Image.Make", "SONY")])
  }
}

final class FileFactCacheTests: XCTestCase {
  private var dir: URL!

  override func setUp() {
    dir = FileManager.default.temporaryDirectory.appendingPathComponent("facts-\(UUID().uuidString)")
  }

  override func tearDown() { try? FileManager.default.removeItem(at: dir) }

  func testAFolderSeenBeforeIsKnownAfterARestart() {
    let file = dir.appendingPathComponent("facts.json")
    let path = FilePath(root: "r", rel: "a.jpg")
    let facts = FileFacts(fields: ["Canon EOS R5", "Lisboa"], camera: "Canon EOS R5", lens: "")
    let first = FileFactCache(file: file)
    first.load()
    first.set(facts, for: path.stamp(size: 10, mtime: 5))
    first.save()
    let again = FileFactCache(file: file)
    again.load()
    XCTAssertEqual(again.facts(for: path.stamp(size: 10, mtime: 5)), facts)
    XCTAssertNil(again.facts(for: path.stamp(size: 10, mtime: 6)), "a file changed in place is read again")
  }

  func testARefusedWriteStaysToBeWritten() {
    let file = dir.appendingPathComponent("facts.json")
    let cache = FileFactCache(file: file)
    cache.set(FileFacts(fields: [], camera: "", lens: ""), for: "k")
    cache.save { _ in false }  // the library went meanwhile
    XCTAssertFalse(FileManager.default.fileExists(atPath: file.path))
    cache.save()
    XCTAssertTrue(FileManager.default.fileExists(atPath: file.path))
  }

  func testPastItsLimitItStartsAgain() {
    let cache = FileFactCache(file: dir.appendingPathComponent("facts.json"), limit: 3)
    for i in 0..<3 { cache.set(FileFacts(fields: [], camera: "", lens: ""), for: "\(i)") }
    XCTAssertEqual(cache.count, 3)
    cache.set(FileFacts(fields: [], camera: "", lens: ""), for: "new")
    XCTAssertEqual(cache.count, 1)
  }
}
