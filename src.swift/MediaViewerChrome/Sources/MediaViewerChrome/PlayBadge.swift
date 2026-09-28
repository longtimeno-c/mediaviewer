// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The "this item is a video" glyph over gallery and filmstrip cells, so clips
// read apart from stills without opening them. Centred by the caller's overlay.
import SwiftUI

struct PlayBadge: View {
  let diameter: CGFloat

  var body: some View {
    Image(systemName: "play.fill")
      .font(.system(size: diameter * 0.4, weight: .semibold))
      .foregroundStyle(.white)
      .offset(x: diameter * 0.04)  // the triangle's optical centre sits left of its box
      .frame(width: diameter, height: diameter)
      .background(Circle().fill(.black.opacity(0.6)))
      .overlay(Circle().strokeBorder(.white.opacity(0.8), lineWidth: 1.5))
      .shadow(radius: 1)
      .allowsHitTesting(false)
      .accessibilityLabel("Video")
  }
}
