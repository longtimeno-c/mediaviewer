// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File mode (docs/plans/phone-in-the-app.md §4): a library with no catalogue yet is the NAS's named
// folders. Its items and folders are named by where they are, not by a catalogue's assets.id:
// "file:<service root id>/<path under it>". A catalogue item's id is a decimal number, so the two
// can never be taken for each other.
import Foundation

public struct FilePath: Hashable, Sendable {
  /// The service's id for a named folder.
  public let root: String
  /// '/'-separated, under the root; "" is the root's top.
  public let rel: String

  public init(root: String, rel: String) {
    self.root = root
    self.rel = rel
  }

  static let prefix = "file:"

  /// The id an item or folder carries.
  public var id: String { Self.prefix + root + "/" + rel }

  /// nil for anything that is not a file-mode id (a catalogue item's "1234", or a malformed one).
  public init?(id: String) {
    guard id.hasPrefix(Self.prefix) else { return nil }
    let body = id.dropFirst(Self.prefix.count)
    guard let slash = body.firstIndex(of: "/") else { return nil }
    let root = String(body[..<slash])
    guard !root.isEmpty, !root.contains("/") else { return nil }
    self.root = root
    self.rel = String(body[body.index(after: slash)...])
  }

  public static func isFile(_ id: String) -> Bool { id.hasPrefix(prefix) }

  /// The last part of the path: the file or folder name ("" at a root's top).
  public var name: String { rel.split(separator: "/").last.map(String.init) ?? "" }

  /// A child of this folder.
  public func appending(_ name: String) -> FilePath {
    FilePath(root: root, rel: rel.isEmpty ? name : rel + "/" + name)
  }

  /// The folder this is in, within its named folder; nil at a root's top.
  public var parent: FilePath? {
    guard !rel.isEmpty else { return nil }
    guard let slash = rel.lastIndex(of: "/") else { return FilePath(root: root, rel: "") }
    return FilePath(root: root, rel: String(rel[..<slash]))
  }

  /// What a cache keys a file's facts by: where it is, its size and its modification time. A
  /// file changed in place has another key.
  public func stamp(size: UInt64, mtime: Int64) -> String { "\(root)/\(rel)|\(size)|\(mtime)" }
}

/// When the library is in file mode, from what the last catalogue check said. Status numbers are
/// mv_mobile_link.h's.
public enum FileModeRule {
  public static let noCatalogue: Int32 = 20
  public static let copyAvailable: Int32 = 16
  public static let installed: Int32 = 0
  public static let upToDate: Int32 = 15

  /// A catalogue on the phone always wins. Otherwise the check's word decides; a check that could
  /// not tell (the NAS unreachable, a copy offered but not yet made) keeps what was known.
  public static func fileMode(afterCheck outcome: Int32, hasCatalogue: Bool, wasFileMode: Bool) -> Bool {
    if hasCatalogue { return false }
    if outcome == noCatalogue { return true }
    if outcome == installed || outcome == upToDate { return false }
    return wasFileMode
  }
}

/// Which picture from a file's head the grid may use. ImageIO, asked for a thumbnail from a head
/// that is not the whole file, gives an embedded preview when there is one, and otherwise decodes
/// what it has of the picture itself (the bottom comes out grey).
public enum HeadPicture {
  /// What the head is asked for: larger than any preview worth keeping, so a preview comes back at
  /// its own size and a decode of the picture at this size (or at the picture's own, if smaller).
  public static let probePixels = 1024
  /// A preview smaller than this looks soft on a grid tile: the whole file is read instead.
  public static let minimumPreviewPixels = 256

  /// `longSide`: of what ImageIO returned; `imageLongSide`: of the picture itself, 0 when unknown.
  public static func isUsablePreview(longSide: Int, imageLongSide: Int) -> Bool {
    let decodeSize = imageLongSide > 0 ? min(imageLongSide, probePixels) : probePixels
    if abs(longSide - decodeSize) <= 1 { return false }  // a decode of the picture, not a preview
    return longSide >= minimumPreviewPixels
  }
}

/// The folders whose listings are kept, most recent last, so what a session has browsed stays
/// bounded. Not thread-safe: the owner locks.
public struct RecentFolders: Sendable {
  public let limit: Int
  public private(set) var order: [String] = []

  public init(limit: Int) { self.limit = limit }

  /// Notes `folder` as just listed; returns the folders to forget, never `keep` (the one on screen).
  public mutating func touch(_ folder: String, keep: String?) -> [String] {
    order.removeAll { $0 == folder }
    order.append(folder)
    var dropped: [String] = []
    var i = 0
    while order.count > limit, i < order.count {
      if order[i] == keep || order[i] == folder {
        i += 1
        continue
      }
      dropped.append(order.remove(at: i))
    }
    return dropped
  }

  public mutating func removeAll() { order.removeAll() }
}
