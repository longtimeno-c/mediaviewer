// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #345 (docs/design/06 "The AI badge", docs/design/16 "Settings"): the small "AI" mark in
// the bottom-right of the canvas when the file on screen says it was AI-made.
// Chrome, like the transport strip: an NSHostingView over the CAMetalLayer, never
// drawn into the canvas, so showing, hiding or hovering it costs no present.
// main_mac.mm owns when it shows and where (shell/ai_badge.h decides the text);
// this view only draws what it is given. Positive evidence only: there is no
// "not AI" state to draw.
import AppKit
import SwiftUI

@MainActor
final class AIBadgeStore: ObservableObject {
  static let shared = AIBadgeStore()

  @Published private(set) var title = ""
  @Published private(set) var evidence = ""

  /// The host pushes the badge's text whenever the item or its answer changes.
  func set(title: String, evidence: String) {
    if title != self.title { self.title = title }
    if evidence != self.evidence { self.evidence = evidence }
  }

  /// "Made with ChatGPT. Content Credentials (C2PA): AI-generated (…)" for VoiceOver.
  var spoken: String { evidence.isEmpty ? title : title + ". " + evidence }
}

struct AIBadgeView: View {
  /// The hosting view's size (main_mac.mm pins it): the capsule and nothing else,
  /// so no transparent margin takes the canvas's clicks.
  static let width: CGFloat = 44
  static let height: CGFloat = 20

  @ObservedObject private var store = AIBadgeStore.shared
  @State private var hovering = false
  @FocusState private var focused: Bool

  var body: some View {
    HStack(spacing: 3) {
      Image(systemName: "sparkles").font(.system(size: 10, weight: .semibold))
      Text("AI").font(.system(size: 11, weight: .semibold))
    }
    .foregroundStyle(MVTheme.title)
    .frame(width: AIBadgeView.width, height: AIBadgeView.height)
    .background(.regularMaterial, in: Capsule())
    .overlay(Capsule().strokeBorder(focused ? MVTheme.accent : MVTheme.hairline, lineWidth: focused ? 2 : 0.5))
    .contentShape(Capsule())
    // Tab reaches it in the chrome's key loop; it takes no keys of its own, so
    // every key but Tab and Esc carries on to the window's router.
    .focusable()
    .focusEffectDisabled()
    .focused($focused)
    .onHover { hovering = $0 }
    // The host clears the text when it hides the badge. A hidden view gets no
    // hover-out, so without this the popover would stay up over the next item.
    .onChange(of: store.title) { _, title in if title.isEmpty { hovering = false } }
    .popover(isPresented: Binding(get: { !store.title.isEmpty && (hovering || focused) },
                                  set: { if !$0 { hovering = false } }),
             arrowEdge: .top) {
      VStack(alignment: .leading, spacing: 4) {
        Text(store.title).font(.callout.weight(.semibold)).foregroundStyle(MVTheme.title)
        if !store.evidence.isEmpty {
          Text(store.evidence).font(.caption).foregroundStyle(MVTheme.body)
            .fixedSize(horizontal: false, vertical: true)
        }
      }
      .padding(10)
      .frame(maxWidth: 320, alignment: .leading)
    }
    .accessibilityElement(children: .ignore)
    .accessibilityLabel("AI-generated")
    .accessibilityValue(store.spoken)
    .accessibilityAddTraits(.isStaticText)
  }
}
