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
// Motion rests (owner, 2026-10-15): every display frame while this app is in
// front and a pointer or a key moved in the last five seconds; ten while a
// window is visible, for up to a minute after the last input; then none, and
// none under Reduce Motion. Dots do not move, and a theme with no ground does
// not either.
// A still photo still presents nothing: this timer is the chrome's, and it
// stops. Speed scales velocities and size scales radii; 1 leaves a mark as
// written. Every position is a function of the clock, not of the last frame.
import AppKit
import QuartzCore
import SwiftUI
import MVChromeBridge

@MainActor
final class ThemeMotion: NSObject, ObservableObject {
  static let shared = ThemeMotion()

  @Published private(set) var time: TimeInterval = 0
  @Published private(set) var reduce = false

  private var listeners = 0
  private var timer: Timer?
  /// In use: the display's own frame callback, so marks move on its beat (60
  /// a second, 120 on ProMotion). A fixed 60 Hz timer drifts against it.
  private var link: CADisplayLink?
  private var monitor: Any?
  private var lastInput = CACurrentMediaTime()
  private var lastTick = CACurrentMediaTime()
  private var lastForward: CFTimeInterval = 0
  // The rule (owner, 2026-10-15; the native home runs the same, shell/home_theme.h
  // home_ground_clock): every display frame in front with input in the last 5
  // seconds, 10 a second while a window is visible for up to 60 seconds after
  // the last input, then rest.
  private let slowTick: TimeInterval = 0.1
  private let fastWindow: TimeInterval = 5
  private let slowWindow: TimeInterval = 60

  override private init() {
    super.init()
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
    for name in [NSApplication.didResignActiveNotification, NSApplication.didHideNotification,
                 NSApplication.didUnhideNotification, NSWindow.didChangeOcclusionStateNotification,
                 NSWindow.didMiniaturizeNotification, NSWindow.didDeminiaturizeNotification] {
      note.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
        MainActor.assumeIsolated { self?.consider() }
      }
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
    // The native empty window runs the same rule and hears the canvas itself;
    // the bar and the panes are ours, so pass their input on (a few a second).
    if lastInput - lastForward >= 0.5 {
      lastForward = lastInput
      mv_chrome_ground_input()
    }
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

  /// A window of ours on screen and not covered.
  private var visible: Bool {
    guard let app = NSApp, !app.isHidden else { return false }
    return app.windows.contains { $0.isVisible && !$0.isMiniaturized && $0.occlusionState.contains(.visible) }
  }

  private enum Pace: Equatable { case rest, everyFrame, slow }

  private var pace: Pace {
    guard listeners > 0, !reduce, visible else { return .rest }
    let quiet = CACurrentMediaTime() - lastInput
    if (NSApp?.isActive ?? false) && quiet < fastWindow { return .everyFrame }
    return quiet < slowWindow ? .slow : .rest
  }

  private var running: Pace {
    link != nil ? .everyFrame : timer != nil ? .slow : .rest
  }

  private func consider() {
    let want = pace
    guard want != running else { return }
    let was = running
    link?.invalidate()
    link = nil
    timer?.invalidate()
    timer = nil
    if was == .rest { lastTick = CACurrentMediaTime() }
    switch want {
    case .rest:
      break
    case .everyFrame:
      let screen = NSApp?.mainWindow?.screen ?? NSApp?.windows.first?.screen ?? NSScreen.main
      if let link = screen?.displayLink(target: self, selector: #selector(frame(_:))) {
        link.add(to: .main, forMode: .common)
        self.link = link
      }
    case .slow:
      let timer = Timer(timeInterval: slowTick, repeats: true) { [weak self] _ in
        MainActor.assumeIsolated { self?.tick() }
      }
      timer.tolerance = slowTick * 0.1
      RunLoop.main.add(timer, forMode: .common)
      self.timer = timer
    }
  }

  @objc private func frame(_ link: CADisplayLink) {
    tick()
  }

  private func tick() {
    guard pace != .rest else {
      consider()
      return
    }
    let now = CACurrentMediaTime()
    time += min(0.1, now - lastTick)
    lastTick = now
    if pace != running { consider() }
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
    let pace = item.speed
    let scale = item.size
    switch item.effect {
    case "none":
      break
    case "dots":
      let spacing = max(28, (area / 72).squareRoot())
      let alpha = 0.16 * gain
      var y = spacing * 0.5
      while y < h {
        var x = spacing * 0.5
        while x < w {
          dot(context, x, y, 1.2 * scale, ink, alpha)
          x += spacing
        }
        y += spacing
      }
    case "rain":
      let n = count(area, per: 9000, cap: 64, min: 8)
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let len = (14 + rng.next() * 18) * scale
        let speed = (32 + rng.next() * 56) * pace
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
        let speed = (12 + z * 48) * pace
        let radius = (0.45 + z * 1.15) * scale
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
        let petal = (3 + rng.next() * 4) * scale
        let vy = (16 + rng.next() * 22) * pace
        let amp = (10 + rng.next() * 16) * scale
        let spin = (-0.5 + rng.next()) * pace
        let phase = rng.next() * .pi * 2
        let x = x0 + sin(phase + time * 0.7 * pace) * amp
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
        let freq = (0.6 + depth * 0.9) * pace
        let x = wrap(x0 + sin(time * freq + phase) * (18 + depth * 36) * scale, w)
        let y = wrap(y0 + cos(time * freq * 0.72 + phase) * (10 + depth * 22) * scale, h)
        let pulse = max(0, sin(time * (1.1 + depth) * pace + phase))
        let alpha = pulse * (0.16 + depth * 0.34) * gain
        if alpha > 0.02 {
          dot(context, x, y, (1.1 + depth * 1.5) * scale, ink, alpha)
        }
      }
    case "synapse":
      let spacing = max(24, (w + h) / 120)
      let line = 0.035 * gain
      var row = 0
      while Double(row) * spacing < h {
        context.fill(Path(CGRect(x: 0, y: Double(row) * spacing, width: w, height: 1)),
                     with: .color(color(ink, line)))
        row += 1
      }
      var col = 0
      while Double(col) * spacing < w {
        context.fill(Path(CGRect(x: Double(col) * spacing, y: 0, width: 1, height: h)),
                     with: .color(color(ink, line)))
        col += 1
      }
      let n = count(area, per: 28000, cap: 14, min: 4)
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let vertical = rng.next() >= 0.5
        let speed = (36 + rng.next() * 74) * pace
        let trail = 14 * scale
        let head = 1.3 * scale
        let phase = rng.next()
        let lanes = max(1, Int((vertical ? w : h) / spacing))
        let lane = Double(Int(rng.next() * Double(lanes))) * spacing
        let span = (vertical ? h : w) + trail
        let pos = wrap(phase * span + time * speed, span) - trail
        if vertical {
          context.fill(Path(CGRect(x: lane, y: pos, width: 1, height: trail)),
                       with: .color(color(ink, 0.30 * gain)))
          dot(context, lane + 0.5, pos + trail, head, ink, 0.55 * gain)
        } else {
          context.fill(Path(CGRect(x: pos, y: lane, width: trail, height: 1)),
                       with: .color(color(ink, 0.30 * gain)))
          dot(context, pos + trail, lane + 0.5, head, ink, 0.55 * gain)
        }
      }
    case "embers":
      let n = count(area, per: 14000, cap: 36, min: 8)
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let x0 = rng.next() * w
        let y0 = rng.next() * (h + 24)
        let phase = rng.next() * .pi * 2
        let vy = (18 + rng.next() * 36) * pace
        let amp = (6 + rng.next() * 14) * scale
        let radius = (1.1 + rng.next() * 1.6) * scale
        let rate = (0.18 + rng.next() * 0.28) * pace
        let life = wrap(time * rate + phase / (.pi * 2), 1)
        let fade = sin(life * .pi)
        if fade * gain <= 0.02 { continue }
        let x = wrap(x0 + sin(phase + time * 0.8 * pace) * amp, w)
        let y = wrap(y0 - time * vy, h + 24)
        dot(context, x, y, radius * 3.2, ink, 0.14 * fade * gain)
        dot(context, x, y, radius * 0.65, ink, 0.62 * fade * gain)
      }
    case "sparkles":
      let n = count(area, per: 16000, cap: 28, min: 6)
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let x = rng.next() * w
        let y = rng.next() * h
        let radius = (2.4 + rng.next() * 3.4) * scale
        let phase = rng.next() * .pi * 2
        let freq = (0.9 + rng.next() * 1.8) * pace
        let twinkle = max(0, sin(time * freq + phase))
        let alpha = twinkle * 0.42 * gain
        if alpha > 0.02 {
          star(context, x, y, radius * (0.45 + twinkle * 0.55), ink, alpha)
        }
      }
    case "perlin-flow":
      let n = count(area, per: 18000, cap: 28, min: 6)
      let lags = [0.0, 0.16, 0.32]
      let alphas = [0.22, 0.11, 0.05]
      let radii = [1.15, 0.85, 0.60]
      for i in 0..<n {
        var rng = Rng(seed: item.id, effect: item.effect, i: i)
        let x0 = rng.next() * w
        let y0 = rng.next() * h
        let phase = rng.next() * .pi * 2
        let freq = (0.12 + smoothNoise(x0 * 0.01, y0 * 0.01) * 0.22) * pace
        let amp = (26 + smoothNoise(x0 * 0.008 + 3, y0 * 0.008) * 46) * scale
        for sample in 0..<3 {
          let at = flow(x0, y0, phase, freq, amp, time - lags[sample], pace, w, h)
          dot(context, at.x, at.y, radii[sample] * scale, ink, alphas[sample] * gain)
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

  private static func noise2d(_ x: Double, _ y: Double) -> Double {
    let n = sin(x * 12.9898 + y * 78.233) * 43758.5453
    return n - floor(n)
  }

  private static func smoothNoise(_ x: Double, _ y: Double) -> Double {
    let ix = floor(x)
    let iy = floor(y)
    let fx = x - ix
    let fy = y - iy
    let a = noise2d(ix, iy)
    let b = noise2d(ix + 1, iy)
    let c = noise2d(ix, iy + 1)
    let d = noise2d(ix + 1, iy + 1)
    let ux = fx * fx * (3 - 2 * fx)
    let uy = fy * fy * (3 - 2 * fy)
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy
  }

  /// A closed orbit whose angle comes from the noise at the seed, plus a short wake.
  private static func flow(_ x0: Double, _ y0: Double, _ phase: Double, _ freq: Double,
                           _ amp: Double, _ t: Double, _ pace: Double,
                           _ w: Double, _ h: Double) -> (x: Double, y: Double) {
    let angle = smoothNoise(x0 * 0.004 + t * 0.05 * pace, y0 * 0.004) * .pi * 6
    let x = wrap(x0 + cos(angle) * amp * 0.35 + sin(t * freq + phase) * amp, w)
    let y = wrap(y0 + sin(angle) * amp * 0.35 + cos(t * freq * 0.72 + phase) * amp * 0.62, h)
    return (x, y)
  }

  private static func star(_ context: GraphicsContext, _ x: Double, _ y: Double,
                           _ radius: Double, _ rgb: UInt32, _ alpha: Double) {
    var mark = context
    mark.translateBy(x: x, y: y)
    let r = radius
    var path = Path()
    path.move(to: CGPoint(x: 0, y: -r))
    path.addQuadCurve(to: CGPoint(x: r, y: 0), control: CGPoint(x: r * 0.15, y: -r * 0.15))
    path.addQuadCurve(to: CGPoint(x: 0, y: r), control: CGPoint(x: r * 0.15, y: r * 0.15))
    path.addQuadCurve(to: CGPoint(x: -r, y: 0), control: CGPoint(x: -r * 0.15, y: r * 0.15))
    path.addQuadCurve(to: CGPoint(x: 0, y: -r), control: CGPoint(x: -r * 0.15, y: -r * 0.15))
    mark.fill(path, with: .color(color(rgb, alpha)))
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
