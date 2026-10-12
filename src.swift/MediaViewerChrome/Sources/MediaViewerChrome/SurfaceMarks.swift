// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A screen surface's marks (docs/design/25 PR 58, 2026-10-12): names the add-on
// places over its own pixels, drawn by the chrome so the text is native and crisp.
// The Windows twin is in IslandHost.Screens.cs (SurfaceMarks).
import SwiftUI

/// A caption-size name just right of each point, in the body colour with a soft
/// halo of the surface colour, in the add-on's priority order, skipping any
/// that would overlap one already drawn or the pin. Part of the surface's one
/// accessibility element (its label speaks), so the names are hidden from it.
struct SurfaceMarks: View {
  let marks: [[String: Any]]
  let pin: CGPoint?
  let size: CGSize

  var body: some View {
    let placed = layout()
    ZStack(alignment: .topLeading) {
      ForEach(placed.indices, id: \.self) { i in
        Text(placed[i].text)
          .font(MVTheme.font(11))
          .foregroundStyle(MVTheme.body)
          .shadow(color: MVTheme.surface, radius: 1.5)
          .shadow(color: MVTheme.surface, radius: 1.5)
          .fixedSize()
          .position(x: placed[i].rect.midX, y: placed[i].rect.midY)
      }
    }
    .frame(width: size.width, height: size.height, alignment: .topLeading)
    .allowsHitTesting(false)
    .accessibilityHidden(true)
  }

  private func layout() -> [(text: String, rect: CGRect)] {
    var taken: [CGRect] = []
    if let pin {
      taken.append(CGRect(x: pin.x * size.width - 12, y: pin.y * size.height - 24, width: 24, height: 26))
    }
    var out: [(String, CGRect)] = []
    for m in marks {
      guard let x = (m["x"] as? NSNumber)?.doubleValue, let y = (m["y"] as? NSNumber)?.doubleValue,
            let text = m["text"] as? String, !text.isEmpty else { continue }
      let r = CGRect(x: x * size.width + 6, y: y * size.height - 8, width: CGFloat(text.count) * 6.6 + 4, height: 16)
      guard r.maxX <= size.width + 1, r.minY >= -1, r.maxY <= size.height + 1,
            !taken.contains(where: { $0.intersects(r) }) else { continue }
      taken.append(r)
      out.append((text, r))
    }
    return out
  }
}
