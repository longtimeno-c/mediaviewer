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
  /// The hosting view main_mac.mm pinned to the window's bottom-right corner:
  /// shown only while there is a hint, so it never takes a click otherwise.
  weak var host: NSView?
  private var timer: Timer?

  private init() {}

  /// The host: show `text` for eight seconds (each show starts the time
  /// again), or hide the card when it is empty.
  func show(_ next: String) {
    timer?.invalidate()
    timer = nil
    let fresh = !next.isEmpty && next != text
    if next != text { text = next }
    host?.isHidden = next.isEmpty
    guard !next.isEmpty else { return }
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
    HStack(spacing: 10) {
      Text(store.text)
        .font(MVTheme.font(13)).foregroundStyle(MVTheme.title)
        .fixedSize(horizontal: false, vertical: true)
      Spacer(minLength: 0)
      Button("Got it") { store.learned() }
        .help("Don't show this again")
    }
    .padding(.horizontal, 14).padding(.vertical, 10)
    .frame(width: HintNoticeView.width, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: 10).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: 10).stroke(MVTheme.hairline, lineWidth: 1))
    .shadow(color: .black.opacity(0.18), radius: 8, y: 2)
    .padding(8)  // room for the shadow inside the hosting view
    .fixedSize()
    .accessibilityElement(children: .contain)
    .accessibilityLabel(store.text)
  }

  static let width: CGFloat = 300
}
