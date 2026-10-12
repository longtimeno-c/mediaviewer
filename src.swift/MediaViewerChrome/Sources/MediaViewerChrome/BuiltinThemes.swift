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
// system-drawn controls, and a choice applies at once. Each palette also
// names one ground effect (ThemeField.swift). That effect is not a token:
// it is drawn in the chrome only, and the photo canvas never sees it.
// `mediaviewer.` is reserved, so this selection key cannot be an add-on's.
import Foundation

enum BuiltinThemes {
  static let addon = "mediaviewer.builtin"

  struct Item: Equatable {
    let id: String
    let name: String
    /// What ThemeTokens and the token cache read: the core's theme JSON shape.
    /// The ground effect is not in here. It is not a token.
    let json: String
    /// Host-only ground, drawn behind chrome and never on a photo.
    let effect: String
    let effectRGB: UInt32
    let intensity: Double
    var key: String { addon + "/" + id }
  }

  static func moves(_ effect: String) -> Bool { effect != "dots" }

  static let all: [Item] = rows.map { row in
    guard let item = Item(row: row) else { fatalError("builtin theme row") }
    return item
  }

  static func item(selection key: String) -> Item? {
    all.first { $0.key == key }
  }

  private static let tokens = ["canvas", "surface", "title", "body", "disabled", "hairline", "accent"]

  // id, name, dark|light, seven #rrggbb tokens, then the ground effect, its
  // #rrggbb and an intensity 0.01–1.00. The effect stays in the chrome. The
  // JSON the token cache stores is the seven colours only. Opaque; the hosts
  // append ff. tests/test_open_addons.cpp reads the literals.
  private static let rows = [
    "mvbuiltin midnight Midnight dark #0d1117 #161b22 #c9d1d9 #8b949e #6e7681 #30363d #f85149 rain #ffffff 0.50",
    "mvbuiltin starfield Starfield dark #0b1a2c #091422 #64d2ff #6aa3b6 #5e8090 #1e5074 #4facfe starfield #64d2ff 1.00",
    "mvbuiltin terminal Terminal dark #0b1110 #080d0c #b8e6c1 #5d8f6e #3f6a4c #28553a #6bd38b fireflies #b8e6c1 0.80",
    "mvbuiltin ume Ume dark #271d2b #1d1721 #f1d8e9 #b894a8 #7d6472 #66475f #d98bb7 petals #f5a0c0 1.00",
    "mvbuiltin porcelain Porcelain light #edf0f2 #ffffff #252a30 #5c6770 #8b939c #c7cdd2 #3478c9 dots #252a30 1.00",
  ]

  private static let effects: Set<String> = ["rain", "starfield", "fireflies", "petals", "dots"]

  /// "0.50" / "1.00". The dot is the separator on every machine.
  private static func unit(_ text: String) -> Double? {
    let parts = text.split(separator: ".", omittingEmptySubsequences: false)
    guard parts.count == 2, parts[1].count == 2,
          let whole = Int(parts[0]), let frac = Int(parts[1]),
          (0...1).contains(whole), (0...99).contains(frac) else { return nil }
    let value = Double(whole) + Double(frac) / 100
    return value > 0 && value <= 1 ? value : nil
  }
}

private extension BuiltinThemes.Item {
  init?(row: String) {
    let parts = row.split(separator: " ").map(String.init)
    guard parts.count == 14, parts[0] == "mvbuiltin", parts[3] == "dark" || parts[3] == "light"
    else { return nil }
    guard (4..<11).allSatisfy({ parts[$0].count == 7 && parts[$0].hasPrefix("#")
                                && UInt32(parts[$0].dropFirst(), radix: 16) != nil }),
          BuiltinThemes.effects.contains(parts[11]),
          parts[12].count == 7, parts[12].hasPrefix("#"),
          let effectRGB = UInt32(parts[12].dropFirst(), radix: 16),
          let intensity = BuiltinThemes.unit(parts[13])
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
    self.effect = parts[11]
    self.effectRGB = effectRGB
    self.intensity = intensity
  }
}
