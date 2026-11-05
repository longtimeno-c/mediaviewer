// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #299 (docs/design/17 "Unfinished roots wait for Resume"): an index left
// unfinished last session (a quit or a kill part-way through the Photos
// library or a large folder) does not carry on by itself. Once the window is
// up, a small notice in the bottom right asks "Resume indexing Photos?".
// Resume loads the pack and continues from the last committed frame; Dismiss
// leaves it paused, and the next launch asks again. Not a modal: the viewer
// works around it. The question comes from the pack's resume.json, read by the
// host at launch without loading the pack (addons_mac.mm).
//
// The twin of the Windows notice (IslandHost.ResumeNotice.cs).
import AppKit
import MVChromeBridge
import SwiftUI

@MainActor
final class ResumeNoticeStore: ObservableObject {
  static let shared = ResumeNoticeStore()

  @Published private(set) var question = ""
  /// Resume or Dismiss was clicked: the card fades out, then goes (NoticeCard).
  @Published private(set) var leaving = false
  /// The hosting view main_mac.mm pinned to the window's bottom-right corner:
  /// shown only while there is a question, so it never takes a click otherwise.
  weak var host: NSView?
  private var timer: Timer?

  private init() {
    // Until the host's launch read has run (it waits for the pack's verify,
    // seconds): then once, and the timer stops. Nothing polls after that.
    timer = Timer.scheduledTimer(withTimeInterval: 0.5, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
  }

  private var ticks = 0

  private func poll() {
    ticks += 1
    // A later window indexes nothing and asks nothing; a host that never
    // checks (the editor soak starts no add-ons) is given up on after 5 min.
    if mv_addon2_elsewhere() || ticks > 600 {
      timer?.invalidate()
      timer = nil
      return
    }
    guard mv_addon2_ai_resume_known() else { return }
    timer?.invalidate()
    timer = nil
    show(AddonStore.readString { mv_addon2_ai_resume_question($0, $1) })
  }

  private func show(_ text: String) {
    leaving = false
    if text != question { question = text }
    syncHost()
    if !text.isEmpty {
      // Said once as it appears, not only when focused.
      NSAccessibility.post(element: NSApp as Any, notification: .announcementRequested,
                           userInfo: [.announcement: text, .priority: NSAccessibilityPriorityLevel.medium.rawValue])
    }
  }

  /// The hosting view holds this card and the restart card (#510) above it:
  /// shown while either is, so it never takes a click otherwise. The hint card
  /// above them is laid out again for the corner's new height.
  func syncHost() {
    let hidden = question.isEmpty && !RestartNoticeStore.shared.shown
    if host?.isHidden != hidden { host?.isHidden = hidden }
    DispatchQueue.main.async { mv_chrome_corner_changed() }
  }

  func resume() {
    let started = mv_addon2_ai_resume()
    leave()
    if started { LocalSearchStore.shared.resumeStarted() }
  }

  func dismiss() {
    mv_addon2_ai_resume_dismiss()
    leave()
  }

  /// Fades the card out, then takes it down (the corner below it closes up then).
  private func leave() {
    guard !question.isEmpty, !leaving else { return }
    withAnimation(NoticeCard.animation) { leaving = true }
    NoticeCard.afterFade { [weak self] in
      guard let self, self.leaving else { return }
      self.show("")
    }
  }
}

/// The corner's hosting view: the restart card (#510, RestartNoticeView.swift)
/// above the resume notice, each shown while it has something to say.
struct ResumeNoticeView: View {
  @ObservedObject private var store = ResumeNoticeStore.shared
  @ObservedObject private var restart = RestartNoticeStore.shared

  var body: some View {
    VStack(alignment: .trailing, spacing: 0) {
      if restart.shown { RestartNoticeCard() }
      if !store.question.isEmpty { resumeCard }
    }
    .fixedSize()
  }

  // The corner cards' look (NoticeCard, NoticeStore.swift).
  private var resumeCard: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(12)) {
      NoticeWords(title: store.question,
                  line: "Local search stopped part-way last time. It stays paused until you resume it.")
      HStack(spacing: MVTheme.space(8)) {
        Spacer(minLength: 0)
        Button("Dismiss") { store.dismiss() }
          .buttonStyle(NoticeButtonStyle())
          .help("Leave it paused. You can resume it from Settings → Add-ons → Local search.")
        Button("Resume") { store.resume() }
          .buttonStyle(NoticeButtonStyle(primary: true))
      }
    }
    .noticeCard(width: ResumeNoticeView.width, visible: !store.leaving)
    .accessibilityElement(children: .contain)
    .accessibilityLabel(store.question)
  }

  static let width: CGFloat = 340
}
