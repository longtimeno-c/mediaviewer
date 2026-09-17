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
    // No .keyboardShortcut here: MvMetalView's keyDown: (main_mac.mm) already
    // binds plain `0`/`1` globally for the window. Registering the same keys
    // here too let a single keypress double-dispatch through both paths
    // depending on AppKit's key-equivalent resolution; these buttons are
    // click-only, matching their role as chrome, not a second key router.
    HStack(spacing: 12) {
      Button("Fit") { mv_chrome_fit() }
      Button("1:1") { mv_chrome_one_to_one() }
      Spacer()
    }
    .padding(.horizontal, 12)
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(.regularMaterial)
  }
}
