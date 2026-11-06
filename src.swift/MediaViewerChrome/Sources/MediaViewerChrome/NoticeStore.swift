// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 12: the one-line notice in the command bar -- what a rating key or a click
// just did ("★★★★☆  saved", "Could not save the rating"). The host owns the
// text and clears it after a few seconds; this only mirrors it.
//
// Also the corner cards' one look (NoticeCard, below).
import AppKit
import Combine
import Foundation
import MVChromeBridge
import SwiftUI

@MainActor
final class NoticeStore: ObservableObject {
  static let shared = NoticeStore()

  @Published private(set) var text = ""
  private var generation: UInt64 = .max

  private init() {
    // Issue #178: polled when the main run loop has run (ChromePulse), not on a timer.
    ChromePulse.shared.add { [weak self] in
      self?.poll()
      return false
    }
  }

  private func poll() {
    let g = mv_chrome_notice_generation()
    guard g != generation else { return }
    generation = g
    let needed = Int(mv_chrome_notice_text(nil, 0))
    guard needed > 0 else {
      text = ""
      return
    }
    var buf = [CChar](repeating: 0, count: needed + 1)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_notice_text($0.baseAddress, Int32($0.count)) }
    text = String(cString: buf)
  }

  /// An iCloud fetch's notice (shell/photos_items_mac.mm fetch_notice:
  /// "Downloading from iCloud… 40 %"): its fraction, nil while it has none
  /// yet; nil-nil (no fetch) for any other notice.
  var fetch: Double?? {
    let prefix = "Downloading from iCloud"
    // shell/edit_session.h kCloudFetchNotice: a file's own fetch, no fraction.
    if text.hasPrefix("Downloading this file from the cloud") { return .some(nil) }
    guard text.hasPrefix(prefix) else { return nil }
    let digits = text.dropFirst(prefix.count).filter(\.isNumber)
    guard let pct = Int(digits) else { return .some(nil) }
    return .some(Double(min(100, pct)) / 100)
  }
}

// ---- the corner cards' one look ----------------------------------------------
// The hint, resume and restart cards in the window's bottom-right corner
// (HintNoticeView, ResumeNoticeView, RestartNoticeView), after docs/design/20
// "Craft rules", and the twin of Windows' NoticeCard (IslandHost.HintNotice.cs):
// one surface with a hairline where it meets the photo (depth is the lighter
// surface, not a shadow), words 12 pt in from every edge, a title 4 pt over its
// line, the buttons 12 pt under it (one neutral, the action filled with the
// accent), and a short entrance and exit (150 ms, opacity and 6 pt of travel)
// instead of a pop. Reduce Motion cuts.

@MainActor
enum NoticeCard {
  static let radius: CGFloat = 10
  static let fade: TimeInterval = 0.15
  static let rise: CGFloat = 6

  static var reduceMotion: Bool { NSWorkspace.shared.accessibilityDisplayShouldReduceMotion }
  /// The entrance and exit's curve; nil (a cut) with Reduce Motion.
  static var animation: Animation? { reduceMotion ? nil : .easeOut(duration: fade) }

  /// Runs `then` once a card that started fading has gone (at once with
  /// Reduce Motion): what hides the hosting view or drops the card.
  static func afterFade(_ then: @escaping @MainActor () -> Void) {
    guard !reduceMotion else { return then() }
    DispatchQueue.main.asyncAfter(deadline: .now() + fade) { MainActor.assumeIsolated { then() } }
  }
}

/// The card's surface, padding and motion. It fades in when it first appears
/// and whenever `visible` turns true; `visible` false fades it out.
struct NoticeCardFrame: ViewModifier {
  let width: CGFloat
  var visible = true
  @State private var appeared = false

  func body(content: Content) -> some View {
    let shown = appeared && visible
    content
      .padding(MVTheme.space(12))
      .frame(width: width, alignment: .leading)
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(NoticeCard.radius)).fill(MVTheme.surface))
      .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(NoticeCard.radius))
        .strokeBorder(MVTheme.hairline, lineWidth: 1))
      .opacity(shown ? 1 : 0)
      .offset(y: shown ? 0 : NoticeCard.rise)
      .padding(MVTheme.space(8))  // its place in the hosting view, inside the corner
      .fixedSize()
      .onAppear { withAnimation(NoticeCard.animation) { appeared = true } }
  }
}

extension View {
  func noticeCard(width: CGFloat, visible: Bool = true) -> some View {
    modifier(NoticeCardFrame(width: width, visible: visible))
  }
}

/// A card's title over its line: 14 and 12 pt, 4 pt apart (closer to its own
/// line than to the buttons). Words that change crossfade in place.
struct NoticeWords: View {
  let title: String
  var line: String? = nil

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(4)) {
      Text(title)
        .font(MVTheme.font(14)).foregroundStyle(MVTheme.title)
        .contentTransition(.opacity)
        .fixedSize(horizontal: false, vertical: true)
      if let line {
        Text(line)
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .lineSpacing(3)  // small text keeps a line height of at least 1.3
          .contentTransition(.opacity)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
  }
}

/// A card's button: a control with a surface at rest. The neutral one is a
/// faint fill of the title colour; the action is filled with the accent and
/// keeps that colour under the pointer, a step deeper (Windows' SettingsButton
/// fills, IslandHost.Settings.cs).
struct NoticeButtonStyle: ButtonStyle {
  var primary = false

  func makeBody(configuration: Configuration) -> some View {
    NoticeButtonBody(configuration: configuration, primary: primary)
  }

  private struct NoticeButtonBody: View {
    let configuration: Configuration
    let primary: Bool
    @State private var hover = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
      let pressed = configuration.isPressed
      let fill = primary
        ? MVTheme.accent.opacity(pressed ? 0.78 : (hover ? 0.88 : 1))
        : Color.primary.opacity(pressed ? 0.16 : (hover ? 0.12 : 0.08))
      configuration.label
        .font(MVTheme.font(13))
        .foregroundStyle(primary ? MVTheme.onAccent : MVTheme.title)
        .padding(.horizontal, MVTheme.space(12)).padding(.vertical, MVTheme.space(5))
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(6)).fill(fill))
        .contentShape(Rectangle())
        .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: hover)
        .onHover { hover = $0 }
    }
  }
}
