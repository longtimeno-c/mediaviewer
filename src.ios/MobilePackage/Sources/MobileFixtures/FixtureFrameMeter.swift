// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A frame meter for the hitch tests, on only with -fixtureFrameMeter YES. Apple's scroll metric gives
// hitch figures only on a device; this counts late frames in the Simulator too, from the main run
// loop's display link, and publishes them on an accessibility element the test reads.
#if !DEBUG
#error("MobileFixtures is debug only and must not be built into a release")
#endif
import QuartzCore
import UIKit

@MainActor
final class FixtureFrameMeter: NSObject {
  private static var shared: FixtureFrameMeter?

  private var link: CADisplayLink?
  private var last: CFTimeInterval = 0
  private var started: CFTimeInterval = 0
  private var frames = 0
  private var hitches = 0
  private var hitchSeconds: CFTimeInterval = 0
  private var worst: CFTimeInterval = 0
  private var window: UIWindow?
  private let label = UILabel()

  static func startIfAsked(in scene: UIWindowScene) {
    guard UserDefaults.standard.bool(forKey: "fixtureFrameMeter"), shared == nil else { return }
    let meter = FixtureFrameMeter()
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
    label.accessibilityIdentifier = "frame-stats"
    label.text = "."
    window.rootViewController?.view.addSubview(label)
    window.isHidden = false
    self.window = window

    let link = CADisplayLink(target: self, selector: #selector(tick(_:)))
    link.preferredFrameRateRange = CAFrameRateRange(minimum: 80, maximum: 120, preferred: 120)
    link.add(to: .main, forMode: .common)
    self.link = link
  }

  @objc private func tick(_ link: CADisplayLink) {
    defer { last = link.timestamp }
    guard last > 0 else {
      started = link.timestamp
      return
    }
    frames += 1
    let interval = link.timestamp - last
    let expected = link.targetTimestamp - link.timestamp
    // The usual definition: a frame later than one and a half refresh intervals is a hitch, and
    // the time past one interval is what it cost.
    if expected > 0, interval > expected * 1.5 {
      hitches += 1
      hitchSeconds += interval - expected
      worst = max(worst, interval)
    }
    if frames % 12 == 0 {
      let ms = { (t: CFTimeInterval) in String(format: "%.2f", t * 1000) }
      label.accessibilityLabel = "frames=\(frames) hitches=\(hitches) hitchMs=\(ms(hitchSeconds)) elapsedMs=\(ms(link.timestamp - started)) worstMs=\(ms(worst)) expectedMs=\(ms(expected))"
    }
  }
}
