// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The gallery search bar's Names filter (plan/17 "Gallery search bar",
// 2026-09-27): case- and diacritic-insensitive substring match on the names
// the tiles show. Pure: no bridge, no UI, so it can be measured on its own.
//
// Cost (plan/17, the spec's "≤ 2 ms for 10,000 items"): the folding is
// Foundation's and is done once per listing, off the main thread
// (GallerySearchStore); a keystroke only runs `matching`, a byte substring
// search (memmem) over one contiguous buffer of already-folded UTF-8.
import Foundation

struct FoldedNames: Sendable {
  /// Every folded name back to back, and where each one ends.
  private let bytes: [UInt8]
  private let ends: [Int]

  init(_ names: [String]) {
    var bytes: [UInt8] = []
    var ends: [Int] = []
    bytes.reserveCapacity(names.count * 16)
    ends.reserveCapacity(names.count)
    for name in names {
      bytes.append(contentsOf: Self.fold(name).utf8)
      ends.append(bytes.count)
    }
    self.bytes = bytes
    self.ends = ends
  }

  var count: Int { ends.count }

  /// What both sides of a match are compared as: no case, no accents, and
  /// one normal form (a name from the file system is often decomposed).
  static func fold(_ s: String) -> String {
    s.folding(options: [.caseInsensitive, .diacriticInsensitive], locale: nil)
      .precomposedStringWithCanonicalMapping
  }

  /// Indices of the names that contain `foldedQuery` (made with `fold`), in
  /// order. An empty query matches everything.
  func matching(_ foldedQuery: String) -> [Int] {
    let needle = Array(foldedQuery.utf8)
    guard !needle.isEmpty else { return Array(0..<ends.count) }
    var out: [Int] = []
    bytes.withUnsafeBufferPointer { hay in
      needle.withUnsafeBufferPointer { pin in
        guard let h = hay.baseAddress, let p = pin.baseAddress else { return }
        var start = 0
        for (i, end) in ends.enumerated() {
          let len = end - start
          if len >= pin.count, memmem(h + start, len, p, pin.count) != nil { out.append(i) }
          start = end
        }
      }
    }
    return out
  }
}
