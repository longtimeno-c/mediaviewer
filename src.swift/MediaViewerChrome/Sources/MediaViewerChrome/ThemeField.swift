// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The ground of a chrome pane: the canvas colour, and the one effect a
// built-in theme names (BuiltinThemes.swift). The Windows twin is
// ThemeField.cs. The counts, the speeds and the seed match; a change to
// the drawing here is a change to that file.
//
// The effect is not a theme token. An add-on theme has none. High contrast
// clears MVTheme.themed, so this paints the system ground and stops.
// Photo and video live on the swapchain, which is a different view.
//
// Motion rests. Ten frames a second, and only while this app is in front,
// Reduce Motion is off, and a pointer or a key moved in the last five
// seconds. Dots do not move. A still photo still presents nothing: this
// timer is the chrome's, and it stops.
import AppKit
import QuartzCore
import SwiftUI

@MainActor
final class ThemeMotion: ObservableObject {
  static let shared = ThemeMotion()

  @Published private(set) var time: TimeInterval = 0
  @Published private(set) var reduce = false

  private var listeners = 0
  private var timer: Timer?
  private var monitor: Any?
  private var lastInput = CACurrentMediaTime()
  private var lastTick = CACurrentMediaTime()
  private let rest: TimeInterval = 5

  private init() {
    reduce = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
    NSWorkspace.shared.notificationCenter.addObserver(
      forName: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification,
      object: nil, queue: .main
    ) { [weak self] _ in
      MainActor.assumeIsolated {
        guard let self else { return }
        self.reduce = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        self.consider()
      }
    }
    let note = NotificationCenter.default
    note.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
      MainActor.assumeIsolated { self?.bump() }
    }
    note.addObserver(forName: NSApplication.didResignActiveNotification, object: nil, queue: .main) { [weak self] _ in
      MainActor.assumeIsolated { self?.consider() }
    }
    note.addObserver(forName: NSWindow.didBecomeKeyNotification, object: nil, queue: .main) { note in
      MainActor.assumeIsolated { (note.object as? NSWindow)?.acceptsMouseMovedEvents = true }
    }
    monitor = NSEvent.addLocalMonitorForEvents(
      matching: [.mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged,
                 .scrollWheel, .leftMouseDown, .rightMouseDown, .keyDown]
    ) { event in
      MainActor.assumeIsolated { ThemeMotion.shared.bump() }
      return event
    }
    NSApp?.windows.forEach { $0.acceptsMouseMovedEvents = true }
  }

  func bump() {
    lastInput = CACurrentMediaTime()
    consider()
  }

  func retain() {
    listeners += 1
    bump()
  }

  func release() {
    listeners = max(0, listeners - 1)
    consider()
  }

  private var live: Bool {
    listeners > 0 && !reduce && (NSApp?.isActive ?? false)
      && CACurrentMediaTime() - lastInput < rest
  }

  private func consider() {
    if live {
      guard timer == nil else { return }
      lastTick = CACurrentMediaTime()
      let timer = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in
        MainActor.assumeIsolated { self?.tick() }
      }
      RunLoop.main.add(timer, forMode: .common)
      self.timer = timer
    } else {
      timer?.invalidate()
      timer = nil
    }
  }

  private func tick() {
    guard live else {
      consider()
      return
    }
    let now = CACurrentMediaTime()
    time += min(0.1, now - lastTick)
    lastTick = now
  }
}

/// Canvas colour, then the built-in's marks. Menus keep a flat fill: this is
/// the pane ground, not a popover.
struct ThemeField: View {
  @ObservedObject private var theme = ThemeStore.shared
  @ObservedObject private var motion = ThemeMotion.shared
  @State private var holding = false

  private var item: BuiltinThemes.Item? {
    guard MVTheme.themed != nil else { return nil }
    return BuiltinThemes.item(selection: theme.selection)
  }

  private var moves: Bool {
    guard let item else { return false }
    return BuiltinThemes.moves(item.effect)
  }

  var body: some View {
    Canvas { context, size in
      ThemeGround.draw(context, size, item: item,
                       time: moves && !motion.reduce ? motion.time : 0)
    }
    .transaction { $0.animation = nil }
    .accessibilityHidden(true)
    .allowsHitTesting(false)
    .onAppear { syncHold() }
    .onDisappear {
      if holding {
        holding = false
        motion.release()
      }
    }
    .onChange(of: theme.generation) { _, _ in
      syncHold()
      motion.bump()
    }
  }

  private func syncHold() {
    if moves && !holding {
      holding = true
      motion.retain()
    } else if !moves && holding {
      holding = false
      motion.release()
    }
  }
}

private enum ThemeGround {
  static func draw(_ context: GraphicsContext, _ size: CGSize, item: BuiltinThemes.Item?, time: Double) {
    context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(MVTheme.canvas))
    guard let item, size.width >= 8, size.height >= 8 else { return }
    let w = size.width
    let h = size.height
    let area = w * h
    let ink = item.effectRGB
    let gain = item.intensity
    switch item.effect {
    case "dots":
      let spacing = max(28, (area / 72).squareRoot())
      let alpha = 0.16 * gain
      var y = spacing * 0.5
      while y < h {
        var x = spacing * 0.5
        while x < w {
          dot(context, x, y, 1.2, ink, alpha)
          x += spacing
        }
        y += spacing
      }
    case "rain":
      let n = count(area, per: 9000, cap: 64, min: 8)
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let len = 14 + rng.next() * 18
        let speed = 32 + rng.next() * 56
        let alpha = (0.14 + rng.next() * 0.14) * gain
        let x = rng.next() * w
        let y0 = rng.next() * (h + len)
        let y = wrap(y0 + time * speed, h + len) - len
        context.fill(Path(CGRect(x: x, y: y, width: 1, height: len)),
                     with: .color(color(ink, alpha)))
      }
    case "starfield":
      let n = count(area, per: 7000, cap: 90, min: 10)
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let x = rng.next() * w
        let y0 = rng.next() * h
        let z = 0.18 + rng.next() * 0.82
        let speed = 12 + z * 48
        let radius = 0.45 + z * 1.15
        let alpha = (0.10 + z * 0.38) * gain
        let y = wrap(y0 + time * speed, h)
        dot(context, x, y, radius, ink, alpha)
      }
    case "petals":
      let n = count(area, per: 26000, cap: 18, min: 6)
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let x0 = rng.next() * w
        let y0 = rng.next() * (h + 16)
        let petal = 3 + rng.next() * 4
        let vy = 16 + rng.next() * 22
        let amp = 10 + rng.next() * 16
        let spin = -0.5 + rng.next()
        let phase = rng.next() * .pi * 2
        let x = x0 + sin(phase + time * 0.7) * amp
        let y = wrap(y0 + time * vy, h + 16) - 8
        let rot = phase + time * spin
        petalMark(context, x, y, petal, rot, ink, 0.20 * gain, 0.13 * gain)
      }
    case "fireflies":
      let n = count(area, per: 15000, cap: 32, min: 7)
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let x0 = rng.next() * w
        let y0 = rng.next() * h
        let depth = rng.next()
        let phase = rng.next() * .pi * 2
        let freq = 0.6 + depth * 0.9
        let x = wrap(x0 + sin(time * freq + phase) * (18 + depth * 36), w)
        let y = wrap(y0 + cos(time * freq * 0.72 + phase) * (10 + depth * 22), h)
        let pulse = max(0, sin(time * (1.1 + depth) + phase))
        let alpha = pulse * (0.16 + depth * 0.34) * gain
        if alpha > 0.02 {
          dot(context, x, y, 1.1 + depth * 1.5, ink, alpha)
        }
      }
    default:
      break
    }
  }

  private static func count(_ area: Double, per: Double, cap: Int, min: Int) -> Int {
    guard area >= 800 else { return 0 }
    return Swift.min(cap, Swift.max(min, Int(area / per)))
  }

  private static func wrap(_ value: Double, _ span: Double) -> Double {
    guard span > 1 else { return 0 }
    let m = value.truncatingRemainder(dividingBy: span)
    return m < 0 ? m + span : m
  }

  private static func color(_ rgb: UInt32, _ alpha: Double) -> Color {
    Color(.sRGB,
          red: Double((rgb >> 16) & 0xFF) / 255,
          green: Double((rgb >> 8) & 0xFF) / 255,
          blue: Double(rgb & 0xFF) / 255,
          opacity: alpha)
  }

  private static func dot(_ context: GraphicsContext, _ x: Double, _ y: Double,
                          _ radius: Double, _ rgb: UInt32, _ alpha: Double) {
    let d = radius * 2
    context.fill(Path(ellipseIn: CGRect(x: x - radius, y: y - radius, width: d, height: d)),
                 with: .color(color(rgb, alpha)))
  }

  private static func petalMark(_ context: GraphicsContext, _ x: Double, _ y: Double,
                                _ size: Double, _ rot: Double, _ rgb: UInt32,
                                _ alpha: Double, _ alpha2: Double) {
    var mark = context
    mark.translateBy(x: x, y: y)
    mark.rotate(by: .radians(rot))
    let w = size * 0.7
    let h = size * 0.32
    mark.fill(Path(ellipseIn: CGRect(x: -w - size * 0.15, y: -h, width: w * 2, height: h * 2)),
              with: .color(color(rgb, alpha)))
    mark.fill(Path(ellipseIn: CGRect(x: -w + size * 0.15, y: -h, width: w * 2, height: h * 2)),
              with: .color(color(rgb, alpha2)))
  }
}

private struct Rng {
  var state: UInt64

  init(seed id: String, effect: String, i: Int) {
    state = Self.hash(id + ":" + effect) ^ (UInt64(i) &* 0x9E3779B97F4A7C15)
  }

  mutating func next() -> Double {
    state &+= 0x9E3779B97F4A7C15
    var z = state
    z = (z ^ (z >> 30)) &* 0xBF58476D1CE4E5B9
    z = (z ^ (z >> 27)) &* 0x94D049BB133111EB
    z ^= z >> 31
    return Double(z >> 11) / Double(1 << 53)
  }

  private static func hash(_ text: String) -> UInt64 {
    var h: UInt64 = 0x6a09e667f3bcc909
    for byte in text.utf8 {
      h = (h ^ UInt64(byte)) &* 0x100000001b3
    }
    return h
  }
}
