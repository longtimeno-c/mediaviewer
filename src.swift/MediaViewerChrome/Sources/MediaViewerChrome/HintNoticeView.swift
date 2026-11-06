// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The "Press T" filmstrip hint (shell/filmstrip_hint.h, docs/design/16
// "Filmstrip hint") as a small card in the window's bottom-right corner, above
// the filmstrip: the resume notice's place and look (ResumeNoticeView.swift),
// so it is seen, where the command bar's notice line was not. The host decides
// when it is due and sends the text (MVChromeHost.showHint); the card fades
// after eight seconds, and "Got it" learns the hint for good, as T does.
//
// The twin of the Windows card (IslandHost.HintNotice.cs).
import AppKit
import MVChromeBridge
import SwiftUI

@MainActor
final class HintNoticeStore: ObservableObject {
  static let shared = HintNoticeStore()

  @Published private(set) var text = ""
  /// The card is in (faded in); false while it fades out and once it has gone.
  @Published private(set) var visible = false
  /// The hosting view main_mac.mm pinned to the window's bottom-right corner:
  /// shown only while there is a hint, so it never takes a click otherwise.
  weak var host: NSView?
  private var timer: Timer?

  private init() {}

  /// The host: show `text` for eight seconds (each show starts the time
  /// again), or fade the card out when it is empty (NoticeCard).
  func show(_ next: String) {
    timer?.invalidate()
    timer = nil
    guard !next.isEmpty else {
      guard visible else {
        // Not in (yet): gone at once, so a pending entrance finds nothing to show.
        host?.isHidden = true
        if !text.isEmpty { text = "" }
        return
      }
      withAnimation(NoticeCard.animation) { visible = false }
      NoticeCard.afterFade { [weak self] in
        // Shown again meanwhile: it stays.
        guard let self, !self.visible else { return }
        self.host?.isHidden = true
        self.text = ""
      }
      return
    }
    let fresh = next != text || !visible
    if next != text { text = next }
    if !visible {
      // Unhidden while faded out, then in on the next pass: an entrance, not a pop.
      host?.isHidden = false
      DispatchQueue.main.async { [weak self] in
        MainActor.assumeIsolated {
          guard let self, !self.text.isEmpty else { return }
          withAnimation(NoticeCard.animation) { self.visible = true }
        }
      }
    }
    if fresh {
      // Said once as it appears, not only when focused.
      NSAccessibility.post(element: NSApp as Any, notification: .announcementRequested,
                           userInfo: [.announcement: next, .priority: NSAccessibilityPriorityLevel.medium.rawValue])
    }
    timer = Timer.scheduledTimer(withTimeInterval: 8, repeats: false) { [weak self] _ in
      MainActor.assumeIsolated { self?.show("") }
    }
  }

  func learned() {
    show("")
    mv_chrome_hint_learned()
  }
}

struct HintNoticeView: View {
  @ObservedObject private var store = HintNoticeStore.shared

  var body: some View {
    // The corner cards' look (NoticeCard, NoticeStore.swift): the words and
    // the button 12 pt apart, even padding all round.
    HStack(spacing: MVTheme.space(12)) {
      Text(store.text)
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
        .lineSpacing(3)
        .contentTransition(.opacity)
        .fixedSize(horizontal: false, vertical: true)
        .frame(maxWidth: .infinity, alignment: .leading)
      Button("Got it") { store.learned() }
        .buttonStyle(NoticeButtonStyle())
        .help("Don't show this again")
    }
    .noticeCard(width: HintNoticeView.width, visible: store.visible)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(store.text)
  }

  static let width: CGFloat = 300
}
