// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The host's chrome tokens (docs/design/25 "Themes", issue #204): Import's and
// Duplicates' windows take the colours, accent and corner scale the app's own
// chrome uses, so an add-on theme reaches them too. The twin of the copy in
// AIChrome (this bundle links nothing of the app or of the AI chrome).
//
// The app publishes them as the `tokens` class method of a class named
// MVChromeTheme and posts MVChromeThemeDidChange after a repaint. An app from
// before PR 56 has neither: every token is then the system's, as it was.
import AppKit
import SwiftUI

@MainActor
enum HostTheme {
  private static var tokens: [String: Any] = read()
  static let didChange = Notification.Name("MVChromeThemeDidChange")

  private static func read() -> [String: Any] {
    guard let type = NSClassFromString("MVChromeTheme") as? NSObject.Type else { return [:] }
    let selector = NSSelectorFromString("tokens")
    guard type.responds(to: selector),
          let value = type.perform(selector)?.takeUnretainedValue() as? [String: Any]
    else { return [:] }
    return value
  }

  /// Re-reads after the app repainted; the windows rebuild on HostThemeRoot.
  static func refresh() { tokens = read() }

  private static func colour(_ key: String, _ fallback: NSColor) -> Color {
    Color(nsColor: tokens[key] as? NSColor ?? fallback)
  }
  private static func scale(_ key: String) -> CGFloat {
    CGFloat((tokens[key] as? NSNumber)?.doubleValue ?? 1)
  }

  static var canvas: Color { colour("canvas", .windowBackgroundColor) }
  static var surface: Color { colour("surface", .controlBackgroundColor) }
  static var title: Color { colour("title", .labelColor) }
  static var body: Color { colour("body", .secondaryLabelColor) }
  static var hairline: Color { colour("hairline", .separatorColor) }
  static var accent: Color { colour("accent", .controlAccentColor) }
  /// A corner radius the window would draw as `r` (theme shape).
  static func radius(_ r: CGFloat) -> CGFloat { r * scale("radius") }
}

/// Generation the window roots rebuild on when the app's theme changes.
@MainActor
final class HostThemeWatch: ObservableObject {
  static let shared = HostThemeWatch()
  @Published private(set) var generation = 0
  private init() {
    NotificationCenter.default.addObserver(forName: HostTheme.didChange, object: nil, queue: .main) {
      [weak self] _ in
      MainActor.assumeIsolated {
        HostTheme.refresh()
        self?.generation += 1
      }
    }
  }
}

/// A window's root: rebuilt on a theme change (rare), tinted with the accent.
struct HostThemeRoot<Content: View>: View {
  @ObservedObject private var watch = HostThemeWatch.shared
  let content: Content
  init(@ViewBuilder _ content: () -> Content) { self.content = content() }
  var body: some View {
    content
      .tint(HostTheme.accent)
      .id(watch.generation)
  }
}
