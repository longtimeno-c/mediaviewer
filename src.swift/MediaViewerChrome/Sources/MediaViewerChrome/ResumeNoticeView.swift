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
    if text != question { question = text }
    host?.isHidden = text.isEmpty
    if !text.isEmpty {
      // Said once as it appears, not only when focused.
      NSAccessibility.post(element: NSApp as Any, notification: .announcementRequested,
                           userInfo: [.announcement: text, .priority: NSAccessibilityPriorityLevel.medium.rawValue])
    }
  }

  func resume() {
    let started = mv_addon2_ai_resume()
    show("")
    if started { LocalSearchStore.shared.resumeStarted() }
  }

  func dismiss() {
    mv_addon2_ai_resume_dismiss()
    show("")
  }
}

struct ResumeNoticeView: View {
  @ObservedObject private var store = ResumeNoticeStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      Text(store.question)
        .font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
        .fixedSize(horizontal: false, vertical: true)
      Text("Local search stopped part-way last time. It stays paused until you resume it.")
        .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
      HStack(spacing: 8) {
        Spacer(minLength: 0)
        Button("Dismiss") { store.dismiss() }
          .help("Leave it paused. You can resume it from Settings → Local search.")
        Button("Resume") { store.resume() }
      }
    }
    .padding(14)
    .frame(width: ResumeNoticeView.width, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: 10).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: 10).stroke(MVTheme.hairline, lineWidth: 1))
    .shadow(color: .black.opacity(0.18), radius: 8, y: 2)
    .padding(8)  // room for the shadow inside the hosting view
    .fixedSize()
    .accessibilityElement(children: .contain)
    .accessibilityLabel(store.question)
  }

  static let width: CGFloat = 340
}
