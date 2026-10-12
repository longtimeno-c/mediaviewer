// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The five themes Settings → Appearance ships with nothing installed
// (docs/design/25 "Themes", issue #359). Host palettes: compiled in, not an
// open add-on, so a fresh profile shows them and start-up does not read a
// package. The Windows twin is BuiltinThemes.cs. The rows are one literal
// each and identical in both files; tests/test_open_addons.cpp reads them
// and refuses a palette below the contrast floor.
//
// One palette, no font. The chrome keeps that appearance, including
// system-drawn controls, and a choice applies at once. `mediaviewer.` is
// reserved, so this selection key cannot be an add-on's.
import Foundation

enum BuiltinThemes {
  static let addon = "mediaviewer.builtin"

  struct Item: Equatable {
    let id: String
    let name: String
    /// What ThemeTokens and the token cache read: the core's theme JSON shape.
    let json: String
    var key: String { addon + "/" + id }
  }

  static let all: [Item] = rows.map { row in
    guard let item = Item(row: row) else { fatalError("builtin theme row") }
    return item
  }

  static func item(selection key: String) -> Item? {
    all.first { $0.key == key }
  }

  private static let tokens = ["canvas", "surface", "title", "body", "disabled", "hairline", "accent"]

  // id, name, dark|light, then the seven tokens as #rrggbb. Opaque; the hosts append ff.
  private static let rows = [
    "mvbuiltin ember Ember dark #1c1814 #2b241e #f6f1e8 #cbbba8 #7e7468 #433b33 #e39a45",
    "mvbuiltin graphite Graphite dark #1b1c20 #27292f #f3f4f6 #b7bcc6 #6e7480 #3a3d46 #8eb6ea",
    "mvbuiltin harbour Harbour dark #12161c #1c2530 #e7eef6 #a9b8c9 #647484 #2e3b4c #6ec8e6",
    "mvbuiltin linen Linen light #f6f1e7 #fffdf8 #2a2218 #5c5348 #8f867a #e0d5c4 #a34724",
    "mvbuiltin studio Studio light #f2f3f5 #ffffff #1a1c20 #4c525a #8a9098 #d4d7de #1d5cba",
  ]
}

private extension BuiltinThemes.Item {
  init?(row: String) {
    let parts = row.split(separator: " ").map(String.init)
    guard parts.count == 11, parts[0] == "mvbuiltin", parts[3] == "dark" || parts[3] == "light"
    else { return nil }
    guard (4..<11).allSatisfy({ parts[$0].count == 7 && parts[$0].hasPrefix("#")
                                && UInt32(parts[$0].dropFirst(), radix: 16) != nil })
    else { return nil }
    let dark = parts[3] == "dark"
    var palette = "{"
    for (i, name) in BuiltinThemes.tokens.enumerated() {
      if i > 0 { palette += "," }
      palette += "\"\(name)\":\"\(parts[4 + i])ff\""
    }
    palette += "}"
    let id = parts[1]
    let name = parts[2]
    json = "{\"addon\":\"\(BuiltinThemes.addon)\",\"id\":\"\(id)\",\"name\":\"\(name)\","
      + "\"theme\":{\"font\":\"\",\"dark\":\(dark ? palette : "null"),\"light\":\(dark ? "null" : palette)}}"
    self.id = id
    self.name = name
  }
}
