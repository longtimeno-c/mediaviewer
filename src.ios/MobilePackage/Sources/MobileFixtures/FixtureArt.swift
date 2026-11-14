// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-in photographs, drawn from a seed: the same picture at any size, so a thumbnail really
// does sharpen into its original. No file ships with them.
#if !DEBUG
#error("MobileFixtures is debug only and must not be built into a release")
#endif
import UIKit

struct SeededGenerator {
  private var state: UInt64
  init(seed: UInt64) { state = seed &+ 0x9E37_79B9_7F4A_7C15 }

  mutating func next() -> UInt64 {
    state &+= 0x9E37_79B9_7F4A_7C15
    var z = state
    z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
    z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
    return z ^ (z >> 31)
  }

  mutating func unit() -> CGFloat { CGFloat(next() % 10_000) / 10_000 }
  mutating func range(_ low: CGFloat, _ high: CGFloat) -> CGFloat { low + unit() * (high - low) }
}

enum FixtureScene: Int, CaseIterable {
  case mountains, forest, city, dunes, lake, snowRoad, boats
}

enum FixtureArt {
  private static func color(_ hex: UInt32) -> CGColor {
    UIColor(red: CGFloat((hex >> 16) & 255) / 255, green: CGFloat((hex >> 8) & 255) / 255,
            blue: CGFloat(hex & 255) / 255, alpha: 1).cgColor
  }

  static func scene(for seed: UInt64) -> FixtureScene {
    FixtureScene(rawValue: Int(seed % UInt64(FixtureScene.allCases.count)))!
  }

  static func image(seed: UInt64, size: CGSize) -> UIImage {
    let format = UIGraphicsImageRendererFormat()
    format.scale = 1
    format.opaque = true
    return UIGraphicsImageRenderer(size: size, format: format).image { renderer in
      let c = renderer.cgContext
      var rng = SeededGenerator(seed: seed &* 2_654_435_761)
      // Square drawing units; the short side always shows the whole subject.
      switch scene(for: seed) {
      case .mountains: mountains(c, size, &rng)
      case .forest: forest(c, size, &rng)
      case .city: city(c, size, &rng)
      case .dunes: dunes(c, size, &rng)
      case .lake: lake(c, size, &rng)
      case .snowRoad: snowRoad(c, size, &rng)
      case .boats: boats(c, size, &rng)
      }
    }
  }

  /// A head and shoulders for People.
  static func portrait(seed: UInt64, size: CGSize) -> UIImage {
    let format = UIGraphicsImageRendererFormat()
    format.scale = 1
    format.opaque = true
    return UIGraphicsImageRenderer(size: size, format: format).image { renderer in
      let c = renderer.cgContext
      var rng = SeededGenerator(seed: seed &* 40_503)
      let grounds: [UInt32] = [0x3D5A80, 0x6D597A, 0x4A7C59, 0xB56576, 0x355070, 0x8D6E63]
      let skins: [UInt32] = [0xF2CDA0, 0xD9A074, 0xA9714B, 0x7A4E2D, 0xE8B98F]
      let hairs: [UInt32] = [0x2B2118, 0x4B3621, 0x8C6A3F, 0x1B1B1B, 0xB0A8A0]
      let shirts: [UInt32] = [0xE9C46A, 0xE76F51, 0x2A9D8F, 0xF4F1DE, 0x264653]
      // A square picture centred on the short side, so a head stays round at any shape.
      let u = min(size.width, size.height)
      let x0 = (size.width - u) / 2, y0 = (size.height - u) / 2
      func box(_ x: CGFloat, _ y: CGFloat, _ w: CGFloat, _ h: CGFloat) -> CGRect {
        CGRect(x: x0 + x * u, y: y0 + y * u, width: w * u, height: h * u)
      }
      c.setFillColor(color(grounds[Int(rng.next() % UInt64(grounds.count))]))
      c.fill(CGRect(origin: .zero, size: size))
      c.setFillColor(color(shirts[Int(rng.next() % UInt64(shirts.count))]))
      c.fillEllipse(in: box(0.14, 0.66, 0.72, 0.7))
      let skin = color(skins[Int(rng.next() % UInt64(skins.count))])
      c.setFillColor(skin)
      c.fill(box(0.43, 0.55, 0.14, 0.16))
      c.setFillColor(color(hairs[Int(rng.next() % UInt64(hairs.count))]))
      c.fillEllipse(in: box(0.27, 0.16, 0.46, 0.46))
      c.setFillColor(skin)
      c.fillEllipse(in: box(0.31, 0.25, 0.38, 0.40))
    }
  }

  private static func gradient(_ c: CGContext, _ size: CGSize, top: UInt32, bottom: UInt32, to fraction: CGFloat = 1) {
    let g = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: [color(top), color(bottom)] as CFArray,
                       locations: [0, 1])!
    c.saveGState()
    c.clip(to: CGRect(x: 0, y: 0, width: size.width, height: size.height * fraction))
    c.drawLinearGradient(g, start: .zero, end: CGPoint(x: 0, y: size.height * fraction), options: [])
    c.restoreGState()
  }

  private static func ridge(_ c: CGContext, _ size: CGSize, base: CGFloat, height: CGFloat, teeth: Int,
                            color fill: UInt32, _ rng: inout SeededGenerator) {
    let path = CGMutablePath()
    path.move(to: CGPoint(x: 0, y: size.height))
    path.addLine(to: CGPoint(x: 0, y: size.height * base))
    for i in 0...teeth {
      let x = size.width * CGFloat(i) / CGFloat(teeth)
      let up = i % 2 == 1
      let y = size.height * (base - (up ? rng.range(height * 0.55, height) : rng.range(0, height * 0.2)))
      path.addLine(to: CGPoint(x: x, y: y))
    }
    path.addLine(to: CGPoint(x: size.width, y: size.height))
    path.closeSubpath()
    c.addPath(path)
    c.setFillColor(color(fill))
    c.fillPath()
  }

  private static func waves(_ c: CGContext, _ size: CGSize, base: CGFloat, amplitude: CGFloat, color fill: UInt32,
                            _ rng: inout SeededGenerator) {
    let path = CGMutablePath()
    let phase = rng.range(0, 6.28), length = rng.range(0.7, 1.4)
    path.move(to: CGPoint(x: 0, y: size.height))
    for step in 0...48 {
      let t = CGFloat(step) / 48
      path.addLine(to: CGPoint(x: size.width * t, y: size.height * (base + amplitude * sin(t * 6.28 / length + phase))))
    }
    path.addLine(to: CGPoint(x: size.width, y: size.height))
    path.closeSubpath()
    c.addPath(path)
    c.setFillColor(color(fill))
    c.fillPath()
  }

  private static func mountains(_ c: CGContext, _ s: CGSize, _ rng: inout SeededGenerator) {
    gradient(c, s, top: 0x5C7BA8, bottom: 0xC3D3E6)
    // A single peak with a snow cap, then the ranges in front of it.
    let peakX = s.width * rng.range(0.38, 0.62), peakY = s.height * rng.range(0.18, 0.28)
    let base = s.height * 0.72
    let spread = s.width * 0.55
    let peak = CGMutablePath()
    peak.move(to: CGPoint(x: peakX - spread, y: base))
    peak.addLine(to: CGPoint(x: peakX, y: peakY))
    peak.addLine(to: CGPoint(x: peakX + spread, y: base))
    peak.closeSubpath()
    c.addPath(peak)
    c.setFillColor(color(0x4A5468))
    c.fillPath()
    let capDepth = (base - peakY) * 0.26
    let capSpread = spread * 0.26
    let cap = CGMutablePath()
    cap.move(to: CGPoint(x: peakX, y: peakY))
    cap.addLine(to: CGPoint(x: peakX + capSpread, y: peakY + capDepth))
    cap.addLine(to: CGPoint(x: peakX + capSpread * 0.3, y: peakY + capDepth * 0.72))
    cap.addLine(to: CGPoint(x: peakX - capSpread * 0.25, y: peakY + capDepth))
    cap.addLine(to: CGPoint(x: peakX - capSpread, y: peakY + capDepth))
    cap.closeSubpath()
    c.addPath(cap)
    c.setFillColor(color(0xF4F6F8))
    c.fillPath()
    ridge(c, s, base: 0.82, height: 0.2, teeth: 7, color: 0x343C4D, &rng)
    ridge(c, s, base: 0.95, height: 0.14, teeth: 9, color: 0x232A38, &rng)
  }

  private static func forest(_ c: CGContext, _ s: CGSize, _ rng: inout SeededGenerator) {
    gradient(c, s, top: 0xB6CBA8, bottom: 0xDDE5C7, to: 0.8)
    c.setFillColor(color(0x2F4A36))
    c.fill(CGRect(x: 0, y: s.height * 0.78, width: s.width, height: s.height * 0.22))
    for row in 0..<2 {
      let count = row == 0 ? 11 : 8
      let baseY = s.height * (row == 0 ? 0.74 : 0.86)
      let tall = s.height * (row == 0 ? 0.32 : 0.46)
      c.setFillColor(color(row == 0 ? 0x54795E : 0x2A4A34))
      for i in 0..<count {
        let x = s.width * (CGFloat(i) + 0.5 + rng.range(-0.2, 0.2)) / CGFloat(count)
        let h = tall * rng.range(0.75, 1.05), half = s.width / CGFloat(count) * 0.62
        let tree = CGMutablePath()
        tree.move(to: CGPoint(x: x, y: baseY - h))
        tree.addLine(to: CGPoint(x: x + half, y: baseY))
        tree.addLine(to: CGPoint(x: x - half, y: baseY))
        tree.closeSubpath()
        c.addPath(tree)
        c.fillPath()
      }
    }
  }

  private static func city(_ c: CGContext, _ s: CGSize, _ rng: inout SeededGenerator) {
    gradient(c, s, top: 0x10183A, bottom: 0x28345F)
    var x: CGFloat = 0
    while x < s.width {
      let w = s.width * rng.range(0.14, 0.26)
      let top = s.height * rng.range(0.22, 0.6)
      c.setFillColor(color(0x161B2E))
      c.fill(CGRect(x: x, y: top, width: w - 2, height: s.height - top))
      let columns = max(2, Int(w / max(1, s.width * 0.05)))
      let cell = w / CGFloat(columns)
      var y = top + cell * 0.5
      while y < s.height {
        for col in 0..<columns where rng.unit() > 0.42 {
          c.setFillColor(color(0xF2C46B))
          c.fill(CGRect(x: x + CGFloat(col) * cell + cell * 0.25, y: y, width: cell * 0.42, height: cell * 0.55))
        }
        y += cell * 1.15
      }
      x += w
    }
  }

  private static func dunes(_ c: CGContext, _ s: CGSize, _ rng: inout SeededGenerator) {
    gradient(c, s, top: 0xD9C7A8, bottom: 0xEBDCC0, to: 0.55)
    waves(c, s, base: 0.5, amplitude: 0.05, color: 0xE0B16E, &rng)
    waves(c, s, base: 0.66, amplitude: 0.05, color: 0xC98F4F, &rng)
    waves(c, s, base: 0.84, amplitude: 0.04, color: 0xA9713B, &rng)
  }

  private static func lake(_ c: CGContext, _ s: CGSize, _ rng: inout SeededGenerator) {
    gradient(c, s, top: 0x9DBCC2, bottom: 0xCBDAD7, to: 0.5)
    let waterline: CGFloat = 0.5
    ridge(c, s, base: waterline, height: 0.26, teeth: 5, color: 0x5B7F7E, &rng)
    gradient2(c, s, from: waterline, top: 0x5A8A92, bottom: 0x36606E)
    var mirror = SeededGenerator(seed: rng.next())
    c.saveGState()
    c.clip(to: CGRect(x: 0, y: s.height * waterline, width: s.width, height: s.height * (1 - waterline)))
    c.translateBy(x: 0, y: s.height * waterline * 2)
    c.scaleBy(x: 1, y: -1)
    c.setAlpha(0.55)
    ridge(c, s, base: waterline, height: 0.2, teeth: 5, color: 0x3E6A6E, &mirror)
    c.restoreGState()
  }

  private static func gradient2(_ c: CGContext, _ s: CGSize, from: CGFloat, top: UInt32, bottom: UInt32) {
    let g = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: [color(top), color(bottom)] as CFArray,
                       locations: [0, 1])!
    c.saveGState()
    c.clip(to: CGRect(x: 0, y: s.height * from, width: s.width, height: s.height * (1 - from)))
    c.drawLinearGradient(g, start: CGPoint(x: 0, y: s.height * from), end: CGPoint(x: 0, y: s.height), options: [])
    c.restoreGState()
  }

  private static func snowRoad(_ c: CGContext, _ s: CGSize, _ rng: inout SeededGenerator) {
    gradient(c, s, top: 0xD6DEE6, bottom: 0xF2F4F6, to: 0.42)
    c.setFillColor(color(0xF7F9FA))
    c.fill(CGRect(x: 0, y: s.height * 0.42, width: s.width, height: s.height * 0.58))
    let center = s.width * rng.range(0.42, 0.58)
    let road = CGMutablePath()
    road.move(to: CGPoint(x: center - s.width * 0.02, y: s.height * 0.42))
    road.addLine(to: CGPoint(x: center + s.width * 0.02, y: s.height * 0.42))
    road.addLine(to: CGPoint(x: center + s.width * 0.26, y: s.height))
    road.addLine(to: CGPoint(x: center - s.width * 0.26, y: s.height))
    road.closeSubpath()
    c.addPath(road)
    c.setFillColor(color(0x8F98A3))
    c.fillPath()
    c.setFillColor(color(0x4A5058))
    for side: CGFloat in [-1, 1] {
      for depth in 0..<3 {
        let t = CGFloat(depth) * 0.27 + 0.2
        let x = center + side * s.width * (0.05 + t * 0.34)
        let top = s.height * (0.42 + t * 0.32), h = s.height * (0.06 + t * 0.12)
        c.fill(CGRect(x: x, y: top, width: max(1, s.width * 0.012), height: h))
      }
    }
  }

  private static func boats(_ c: CGContext, _ s: CGSize, _ rng: inout SeededGenerator) {
    gradient(c, s, top: 0x41306B, bottom: 0xF0875A, to: 0.56)
    let horizon: CGFloat = 0.56
    let sunX = s.width * rng.range(0.55, 0.75), sunR = min(s.width, s.height) * 0.075
    c.setFillColor(color(0xFFD18A))
    c.fillEllipse(in: CGRect(x: sunX - sunR, y: s.height * horizon - sunR * 1.9, width: sunR * 2, height: sunR * 2))
    gradient2(c, s, from: horizon, top: 0x7F4A6F, bottom: 0x2B2548)
    c.setFillColor(color(0xFFD18A))
    for i in 0..<5 {
      let y = s.height * (horizon + 0.03 + CGFloat(i) * 0.04)
      let half = sunR * (1.0 - CGFloat(i) * 0.14)
      c.fill(CGRect(x: sunX - half, y: y, width: half * 2, height: max(1, s.height * 0.008)))
    }
    let boatX = s.width * rng.range(0.22, 0.42), boatY = s.height * (horizon + 0.03), unit = min(s.width, s.height)
    c.setFillColor(color(0x17132B))
    let hull = CGMutablePath()
    hull.move(to: CGPoint(x: boatX - unit * 0.1, y: boatY))
    hull.addLine(to: CGPoint(x: boatX + unit * 0.1, y: boatY))
    hull.addLine(to: CGPoint(x: boatX + unit * 0.07, y: boatY + unit * 0.035))
    hull.addLine(to: CGPoint(x: boatX - unit * 0.07, y: boatY + unit * 0.035))
    hull.closeSubpath()
    c.addPath(hull)
    c.fillPath()
    let sail = CGMutablePath()
    sail.move(to: CGPoint(x: boatX - unit * 0.01, y: boatY - unit * 0.18))
    sail.addLine(to: CGPoint(x: boatX + unit * 0.08, y: boatY - unit * 0.02))
    sail.addLine(to: CGPoint(x: boatX - unit * 0.01, y: boatY - unit * 0.02))
    sail.closeSubpath()
    c.addPath(sail)
    c.fillPath()
  }
}
