// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File search's tags on the phone (docs/plans/phone-in-the-app.md §4, docs/design/06 "Search text"):
// ImageIO reads the file's head, and the values go to the core named as Exiv2 names them, so the
// desktop's own search_facts_of turns them into fields (mv_files_fields). Pure: no network, no core.
import Foundation
import ImageIO

public struct FileTag: Hashable, Sendable {
  /// Exiv2's key: "Exif.Image.Software", "Iptc.Application2.Keywords", "Xmp.dc.subject".
  public let rawTag: String
  /// A list (an XMP bag or sequence) as "a, b", as Exiv2 prints it.
  public let value: String

  public init(_ rawTag: String, _ value: String) {
    self.rawTag = rawTag
    self.value = value
  }
}

public struct FileTagValues: Hashable, Sendable {
  public var make = ""
  public var model = ""
  public var lens = ""
  /// EXIF UserComment.
  public var comment = ""
  public var tags: [FileTag] = []

  public init() {}

  public var isEmpty: Bool { make.isEmpty && model.isEmpty && lens.isEmpty && comment.isEmpty && tags.isEmpty }
}

public enum FileTags {
  /// How much of a file the phone reads for its tags and grid picture: where JPEG APPn segments,
  /// the HEIF meta box and a TIFF's first IFD live.
  public static let headBytes = 256 * 1024

  /// The tags in `head`, the first bytes of a file (or all of it). nil when ImageIO cannot read it.
  public static func read(head: Data) -> FileTagValues? {
    guard let source = CGImageSourceCreateWithData(head as CFData, [kCGImageSourceShouldCache: false] as CFDictionary),
          CGImageSourceGetCount(source) > 0 else { return nil }
    let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any] ?? [:]
    let metadata = CGImageSourceCopyMetadataAtIndex(source, 0, nil)
    return values(properties: properties, metadata: metadata)
  }

  /// The values the desktop reads, from ImageIO's properties and XMP. Exiv2's order: EXIF, IPTC, XMP.
  public static func values(properties: [CFString: Any], metadata: CGImageMetadata?) -> FileTagValues {
    var out = FileTagValues()
    let tiff = properties[kCGImagePropertyTIFFDictionary] as? [CFString: Any] ?? [:]
    let exif = properties[kCGImagePropertyExifDictionary] as? [CFString: Any] ?? [:]
    let aux = properties[kCGImagePropertyExifAuxDictionary] as? [CFString: Any] ?? [:]
    let iptc = properties[kCGImagePropertyIPTCDictionary] as? [CFString: Any] ?? [:]

    out.make = text(tiff[kCGImagePropertyTIFFMake])
    out.model = text(tiff[kCGImagePropertyTIFFModel])
    out.lens = text(exif[kCGImagePropertyExifLensModel])
    if out.lens.isEmpty { out.lens = text(aux[kCGImagePropertyExifAuxLensModel]) }
    out.comment = text(exif[kCGImagePropertyExifUserComment])

    var tags: [FileTag] = []
    func add(_ key: String, _ value: Any?) {
      let v = text(value)
      if !v.isEmpty { tags.append(FileTag(key, v)) }
    }
    add("Exif.Image.Make", tiff[kCGImagePropertyTIFFMake])
    add("Exif.Image.Software", tiff[kCGImagePropertyTIFFSoftware])
    add("Exif.Image.ImageDescription", tiff[kCGImagePropertyTIFFImageDescription])
    add("Exif.Image.Artist", tiff[kCGImagePropertyTIFFArtist])

    add("Iptc.Application2.ObjectName", iptc[kCGImagePropertyIPTCObjectName])
    add("Iptc.Application2.Headline", iptc[kCGImagePropertyIPTCHeadline])
    add("Iptc.Application2.Caption", iptc[kCGImagePropertyIPTCCaptionAbstract])
    // IPTC repeats a tag: one row each, as Exiv2 lists them. The desktop reads the first by-line.
    for keyword in list(iptc[kCGImagePropertyIPTCKeywords]) { add("Iptc.Application2.Keywords", keyword) }
    add("Iptc.Application2.Byline", list(iptc[kCGImagePropertyIPTCByline]).first)

    if let metadata {
      add("Xmp.dc.title", xmp(metadata, "dc:title"))
      add("Xmp.photoshop.Headline", xmp(metadata, "photoshop:Headline"))
      add("Xmp.dc.description", xmp(metadata, "dc:description"))
      add("Xmp.xmp.CreatorTool", xmp(metadata, "xmp:CreatorTool"))
      add("Xmp.dc.subject", xmp(metadata, "dc:subject"))
      add("Xmp.dc.creator", xmp(metadata, "dc:creator"))
    }
    out.tags = tags
    return out
  }

  /// A value as text: a string, a number, or the first of an array; trimmed, with the NULs some
  /// cameras pad with removed.
  static func text(_ value: Any?) -> String {
    let raw: String
    switch value {
    case let s as String: raw = s
    case let n as NSNumber: raw = n.stringValue
    case let a as [Any]: return a.first.map { text($0) } ?? ""
    default: return ""
    }
    return raw.replacingOccurrences(of: "\0", with: "").trimmingCharacters(in: .whitespacesAndNewlines)
  }

  static func list(_ value: Any?) -> [String] {
    switch value {
    case let a as [Any]: return a.map { text($0) }.filter { !$0.isEmpty }
    case let s as String: return [text(s)].filter { !$0.isEmpty }
    default: return []
    }
  }

  /// An XMP property as Exiv2 prints it: a simple value as is, a language alternative as its
  /// first (x-default) text, a bag or sequence as "a, b".
  static func xmp(_ metadata: CGImageMetadata, _ path: String) -> String? {
    guard let tag = CGImageMetadataCopyTagWithPath(metadata, nil, path as CFString) else { return nil }
    return flatten(tag)
  }

  private static func flatten(_ tag: CGImageMetadataTag) -> String? {
    guard let value = CGImageMetadataTagCopyValue(tag) else { return nil }
    if let s = value as? String { return s }
    guard CFGetTypeID(value) == CFArrayGetTypeID(), let array = value as? NSArray else { return nil }
    let items: [CGImageMetadataTag] = array.compactMap { element in
      let object = element as AnyObject
      guard CFGetTypeID(object) == CGImageMetadataTagGetTypeID() else { return nil }
      return (object as! CGImageMetadataTag)
    }
    let texts = items.compactMap { flatten($0) }.map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
    if CGImageMetadataTagGetType(tag) == .alternateText { return texts.first }
    return texts.isEmpty ? nil : texts.joined(separator: ", ")
  }
}
