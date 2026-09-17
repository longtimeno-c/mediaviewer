// SPDX-License-Identifier: GPL-2.0-or-later
// PR 18's command-bar chrome (plan/10-roadmap.md, plan/15-platforms.md):
// "command bar and window chrome only — panes come with the same features
// they have on Windows, not earlier." Filmstrip, gallery, and the
// keyboard-complete-browse key router are a follow-up once this hosting
// scaffold and the folder/dir-watch backend both exist; this view only
// proves the SwiftUI-in-AppKit hosting path with the two commands PR 17's
// lab already understands (`0` fit, `1` one-to-one).
import SwiftUI
import MVChromeBridge

public struct CommandBarView: View {
  public init() {}

  public var body: some View {
    HStack(spacing: 12) {
      Button("Fit") { mv_chrome_fit() }
        .keyboardShortcut("0", modifiers: [])
      Button("1:1") { mv_chrome_one_to_one() }
        .keyboardShortcut("1", modifiers: [])
      Spacer()
    }
    .padding(.horizontal, 12)
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(.regularMaterial)
  }
}
