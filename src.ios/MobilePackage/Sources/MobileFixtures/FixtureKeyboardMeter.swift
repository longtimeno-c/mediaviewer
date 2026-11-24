// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Counts the keyboard going and coming, on only with -fixtureKeyboardMeter YES, for the Search
// keyboard tests. A keyboard that goes and comes back within a frame or two is gone for less time
// than one XCUITest query takes, so a test polling for the keyboard misses it; the app's own
// notifications do not. Published on an accessibility element the test reads, with the fixture
// backup's counts that went by while the keyboard was up, so a test waits on those, not a clock.
#if !DEBUG
#error("MobileFixtures is debug only and must not be built into a release")
#endif
import UIKit

@MainActor
final class FixtureKeyboardMeter: NSObject {
  private static var shared: FixtureKeyboardMeter?

  private var shows = 0
  private var hides = 0
  private var heights: [Int] = []
  private var up = false
  private var ticksWhileUp = 0
  private var window: UIWindow?
  private let label = UILabel()

  static func startIfAsked(in scene: UIWindowScene) {
    guard UserDefaults.standard.bool(forKey: "fixtureKeyboardMeter"), shared == nil else { return }
    let meter = FixtureKeyboardMeter()
    shared = meter
    meter.start(scene)
  }

  private func start(_ scene: UIWindowScene) {
    let window = UIWindow(windowScene: scene)
    window.windowLevel = .alert + 2
    window.frame = CGRect(x: 0, y: 0, width: 2, height: 2)
    window.backgroundColor = .clear
    window.isUserInteractionEnabled = false
    window.rootViewController = UIViewController()
    label.frame = CGRect(x: 0, y: 0, width: 2, height: 2)
    label.textColor = .clear
    label.isAccessibilityElement = true
    label.accessibilityIdentifier = "keyboard-stats"
    label.text = "."
    window.rootViewController?.view.addSubview(label)
    window.isHidden = false
    self.window = window
    publish()

    let centre = NotificationCenter.default
    centre.addObserver(self, selector: #selector(willShow(_:)), name: UIResponder.keyboardWillShowNotification, object: nil)
    centre.addObserver(self, selector: #selector(willHide(_:)), name: UIResponder.keyboardWillHideNotification, object: nil)
  }

  /// The fixture backup counted one more photo.
  static func noteBackupTick() {
    guard let meter = shared, meter.up else { return }
    meter.ticksWhileUp += 1
    meter.publish()
  }

  @objc private func willShow(_ note: Notification) {
    shows += 1
    up = true
    if let frame = note.userInfo?[UIResponder.keyboardFrameEndUserInfoKey] as? CGRect {
      heights.append(Int(frame.height.rounded()))
    }
    publish()
  }

  @objc private func willHide(_ note: Notification) {
    hides += 1
    up = false
    publish()
  }

  private func publish() {
    label.accessibilityLabel = "shows=\(shows) hides=\(hides) ticksWhileUp=\(ticksWhileUp) heights=\(heights.map(String.init).joined(separator: ","))"
  }
}
