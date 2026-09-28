// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The management view as the base Settings list embeds it (MVAIChrome
// -settingsView). An NSHostingView's intrinsic size is its unwrapped ideal
// size, and a width constraint does not change its fittingSize, so a SwiftUI
// parent gave it a one-line height while its text wrapped at the real width:
// "Nothing indexed yet" drew over "People installed" (owner report,
// 2026-09-27). This view answers the height its content needs at a width
// (-fittingHeightForWidth:, NSHostingController.sizeThatFits) and posts
// MVAddonSettingsViewHeightChanged when that height changes (a status line
// wraps onto another line, a confirmation opens), so the base lays it out
// again. A base that predates the selector still sees an intrinsic size.
import AppKit
import SwiftUI

/// The height the content reports at its current width.
private struct ContentHeightKey: PreferenceKey {
  static let defaultValue: CGFloat = 0
  static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}

final class SettingsHostView: NSView {
  /// Posted (object: the view) when the content's height at its width changes.
  static let heightChanged = Notification.Name("MVAddonSettingsViewHeightChanged")

  private let controller: NSHostingController<AnyView>
  private var contentHeight: CGFloat = 0

  @MainActor
  init<Content: View>(_ content: Content) {
    controller = NSHostingController(rootView: AnyView(EmptyView()))
    super.init(frame: .zero)
    // The hosted view fills this one; this one's height comes from the base
    // (sizeThatFits -> -fittingHeightForWidth:), not from the hosted view's
    // unwrapped ideal size.
    controller.sizingOptions = []
    controller.rootView = AnyView(
      content
        .fixedSize(horizontal: false, vertical: true)
        .background(GeometryReader { g in Color.clear.preference(key: ContentHeightKey.self, value: g.size.height) })
        .onPreferenceChange(ContentHeightKey.self) { [weak self] h in
          MainActor.assumeIsolated { self?.contentHeightChanged(h) }
        })
    let hosted = controller.view
    hosted.translatesAutoresizingMaskIntoConstraints = false
    addSubview(hosted)
    NSLayoutConstraint.activate([
      hosted.leadingAnchor.constraint(equalTo: leadingAnchor),
      hosted.trailingAnchor.constraint(equalTo: trailingAnchor),
      hosted.topAnchor.constraint(equalTo: topAnchor),
      hosted.bottomAnchor.constraint(equalTo: bottomAnchor),
    ])
    setContentHuggingPriority(.defaultLow, for: .horizontal)
    setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
  }

  @available(*, unavailable)
  required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

  /// The height the content needs at `width` (the base's sizeThatFits).
  @objc(fittingHeightForWidth:)
  func fittingHeight(forWidth width: CGFloat) -> CGFloat {
    MainActor.assumeIsolated {
      guard width.isFinite, width > 0 else { return intrinsicContentSize.height }
      return ceil(controller.sizeThatFits(in: CGSize(width: width, height: .greatestFiniteMagnitude)).height)
    }
  }

  override var intrinsicContentSize: NSSize {
    NSSize(width: NSView.noIntrinsicMetric,
           height: contentHeight > 0 ? contentHeight : controller.view.intrinsicContentSize.height)
  }

  @MainActor
  private func contentHeightChanged(_ h: CGFloat) {
    guard abs(h - contentHeight) >= 0.5 else { return }
    contentHeight = h
    invalidateIntrinsicContentSize()
    NotificationCenter.default.post(name: Self.heightChanged, object: self)
  }
}
