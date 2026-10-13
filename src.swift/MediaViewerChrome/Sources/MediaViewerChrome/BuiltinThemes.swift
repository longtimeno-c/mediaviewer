// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The themes Settings → Appearance ships with nothing installed
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
    /// 1 leaves a ground's speeds as written. Organs' rain is far below that.
    let speed: Double
    /// 1 leaves radii and streak lengths as written.
    let size: Double
    var key: String { addon + "/" + id }
  }

  static func moves(_ effect: String) -> Bool { effect != "dots" && effect != "none" }

  static let all: [Item] = rows.map { row in
    guard let item = Item(row: row) else { fatalError("builtin theme row") }
    return item
  }

  static func item(selection key: String) -> Item? {
    all.first { $0.key == key }
  }

  private static let tokens = ["canvas", "surface", "title", "body", "disabled", "hairline", "accent"]

  // id, name, dark|light, seven #rrggbb tokens, then the ground effect, its
  // #rrggbb, an intensity, a speed and a size, each 0.01–1.00. The effect
  // stays in the chrome. The JSON the token cache stores is the seven colours
  // only. Opaque; the hosts append ff. tests/test_open_addons.cpp reads the
  // literals. Retrowave's title is lifted from #e94560 so it clears 4.5 : 1.
  private static let rows = [
    "mvbuiltin dark Dark dark #282c34 #111111 #9cdef2 #7aa9ba #618491 #355a66 #e06c75 none #9cdef2 1.00 1.00 1.00",
    "mvbuiltin light Light light #f3f5f7 #ffffff #26313a #5c646b #7d858b #cbd3da #b34d5c dots #26313a 1.00 1.00 1.00",
    "mvbuiltin midnight Midnight dark #0d1117 #161b22 #c9d1d9 #8b949e #6e7681 #30363d #f85149 rain #ffffff 0.50 1.00 1.00",
    "mvbuiltin cyberpunk Cyberpunk dark #10121a #171a26 #9bdde0 #6c989d #53747a #514b78 #d45aa8 synapse #62c4c7 0.55 1.00 1.00",
    "mvbuiltin retrowave Retrowave dark #1a1a2e #16213e #ed5e76 #c65169 #9e455b #533483 #e94560 embers #e94560 1.00 1.00 1.00",
    "mvbuiltin forest Forest dark #1b2a1b #142414 #a8d5a2 #81a67d #62805f #3d6b3d #7cb871 petals #a8d5a2 1.00 1.00 1.00",
    "mvbuiltin starfield Starfield dark #0b1a2c #091422 #64d2ff #6aa3b6 #5e8090 #1e5074 #4facfe starfield #64d2ff 1.00 1.00 1.00",
    "mvbuiltin terminal Terminal dark #0b1110 #080d0c #b8e6c1 #5d8f6e #3f6a4c #28553a #6bd38b fireflies #b8e6c1 0.80 1.00 1.00",
    "mvbuiltin organs Organs dark #0a0406 #15080a #efe1c8 #918779 #6f665c #3a1519 #c83240 rain #451616 0.75 0.05 0.75",
    "mvbuiltin ume Ume dark #271d2b #1d1721 #f1d8e9 #b894a8 #7d6472 #66475f #d98bb7 petals #f5a0c0 1.00 1.00 1.00",
    "mvbuiltin gpt GPT dark #212121 #171717 #ececec #969696 #747474 #424242 #949494 none #ececec 1.00 1.00 1.00",
    "mvbuiltin claude Claude dark #1f1e1b #2b2926 #f5f1e8 #a29f99 #7e7b76 #514b43 #d97757 none #f5f1e8 1.00 1.00 1.00",
    "mvbuiltin cute Cute light #fff4f7 #fffafd #63394d #7e5a6b #9b7d8a #edc7d5 #d65f88 sparkles #ff8cb8 1.00 1.00 1.00",
    "mvbuiltin eclipse Eclipse dark #17191c #0f1113 #e8e4dc #91908c #6e6e6c #3c4248 #46c2b3 starfield #46c2b3 1.00 1.00 1.00",
    "mvbuiltin porcelain Porcelain light #edf0f2 #ffffff #252a30 #5c6770 #8b939c #c7cdd2 #3478c9 dots #252a30 1.00 1.00 1.00",
    "mvbuiltin arcade Arcade dark #15131d #0b1720 #f4e85c #978f42 #746d38 #305f68 #ff4f91 synapse #ff4f91 1.00 1.00 1.00",
    "mvbuiltin blueprint Blueprint dark #10263b #091a29 #e8f1f5 #8e9da8 #697988 #48758d #e6b84a dots #e6b84a 1.00 1.00 1.00",
    "mvbuiltin monolith Monolith dark #202020 #121212 #d8d8d2 #969693 #747471 #50504b #b6e05c perlin-flow #b6e05c 1.00 1.00 1.00",
    "mvbuiltin yoyo Yoyo dark #211f23 #141415 #dfdbd7 #999594 #757273 #3e5146 #e09d6c fireflies #b8e6c1 0.80 1.00 1.00",
  ]

  private static let effects: Set<String> = [
    "none", "dots", "rain", "starfield", "fireflies", "petals",
    "synapse", "embers", "sparkles", "perlin-flow",
  ]

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
    guard parts.count == 16, parts[0] == "mvbuiltin", parts[3] == "dark" || parts[3] == "light"
    else { return nil }
    guard (4..<11).allSatisfy({ parts[$0].count == 7 && parts[$0].hasPrefix("#")
                                && UInt32(parts[$0].dropFirst(), radix: 16) != nil }),
          BuiltinThemes.effects.contains(parts[11]),
          parts[12].count == 7, parts[12].hasPrefix("#"),
          let effectRGB = UInt32(parts[12].dropFirst(), radix: 16),
          let intensity = BuiltinThemes.unit(parts[13]),
          let speed = BuiltinThemes.unit(parts[14]),
          let size = BuiltinThemes.unit(parts[15])
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
    self.speed = speed
    self.size = size
  }
}
