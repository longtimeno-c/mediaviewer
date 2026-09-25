// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The "this item is marked" glyph on filmstrip and gallery cells (plan/16
// "Marks, copy, move"): what F7 / F8 / Delete will act on has to be visible.
import SwiftUI

struct MarkBadge: View {
  var body: some View {
    Image(systemName: "checkmark.circle.fill")
      .symbolRenderingMode(.palette)
      .foregroundStyle(.white, Color.accentColor)
      .font(.system(size: 16))
      .shadow(radius: 1)
      .padding(3)
  }
}
