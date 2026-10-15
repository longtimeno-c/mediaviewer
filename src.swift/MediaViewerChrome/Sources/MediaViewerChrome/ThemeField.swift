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
// The marks are Core Animation layers moved on a thread of their own
// (ThemeTicker), so neither SwiftUI nor the main run loop does anything per
// frame. A still photo still presents nothing: the canvas is a different
// view. Speed scales velocities and size scales radii; 1 leaves a mark as
// written. Every position is a function of the clock, not of the last frame.
import AppKit
import QuartzCore
import SwiftUI
import MVChromeBridge

/// The motion rule, decided on the main thread where input and window state
/// live. It does not tick: ThemeTicker does, on its own thread. The main run
/// loop wakes only when the rule changes pace (5 s and 60 s after input),
/// never per frame: each pass of the main loop polls every chrome store
/// (ChromePulse), so a 60 Hz main-thread tick re-laid out the window 60 times
/// a second (profiled 2026-10-15).
@MainActor
final class ThemeMotion: NSObject {
  static let shared = ThemeMotion()

  private(set) var reduce = false
  private var listeners = 0
  private var monitor: Any?
  private var lastInput = CACurrentMediaTime()
  private var lastForward: CFTimeInterval = 0
  private var running: ThemeTicker.Pace = .rest
  private var boundary: Timer?
  // The rule (owner, 2026-10-15; the native home runs the same, shell/home_theme.h
  // home_ground_clock): every display frame in front with input in the last 5
  // seconds, 10 a second while a window is visible for up to 60 seconds after
  // the last input, then rest.
  private let fastWindow: TimeInterval = 5
  private let slowWindow: TimeInterval = 60

  override private init() {
    super.init()
    reduce = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
    ThemeTicker.shared.setReduce(reduce)
    NSWorkspace.shared.notificationCenter.addObserver(
      forName: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification,
      object: nil, queue: .main
    ) { [weak self] _ in
      MainActor.assumeIsolated {
        guard let self else { return }
        self.reduce = NSWorkspace.shared.accessibilityDisplayShouldReduceMotion
        ThemeTicker.shared.setReduce(self.reduce)
        self.consider()
      }
    }
    let note = NotificationCenter.default
    note.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { [weak self] _ in
      MainActor.assumeIsolated { self?.bump() }
    }
    for name in [NSApplication.didResignActiveNotification, NSApplication.didHideNotification,
                 NSApplication.didUnhideNotification, NSWindow.didChangeOcclusionStateNotification,
                 NSWindow.didMiniaturizeNotification, NSWindow.didDeminiaturizeNotification,
                 NSWindow.didChangeScreenNotification] {
      note.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
        MainActor.assumeIsolated { self?.consider(screenChanged: name == NSWindow.didChangeScreenNotification) }
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

  func hold() {
    listeners += 1
    bump()
  }

  func letGo() {
    listeners = max(0, listeners - 1)
    consider()
  }

  /// A window of ours on screen and not covered.
  private var visible: Bool {
    guard let app = NSApp, !app.isHidden else { return false }
    return app.windows.contains { $0.isVisible && !$0.isMiniaturized && $0.occlusionState.contains(.visible) }
  }

  private var pace: ThemeTicker.Pace {
    guard listeners > 0, !reduce, visible else { return .rest }
    let quiet = CACurrentMediaTime() - lastInput
    if (NSApp?.isActive ?? false) && quiet < fastWindow { return .everyFrame }
    return quiet < slowWindow ? .slow : .rest
  }

  private func consider(screenChanged: Bool = false) {
    let want = pace
    if want != running || (screenChanged && want == .everyFrame) {
      running = want
      let screen = NSApp?.mainWindow?.screen ?? NSApp?.windows.first?.screen ?? NSScreen.main
      ThemeTicker.shared.setPace(want, screen: screen)
    }
    // One main-thread wake at the rule's next boundary, not one per frame.
    guard want != .rest else {
      boundary?.invalidate()
      boundary = nil
      return
    }
    let quiet = CACurrentMediaTime() - lastInput
    let next = (want == .everyFrame ? fastWindow : slowWindow) - quiet
    if let boundary, boundary.isValid,
       abs(boundary.fireDate.timeIntervalSinceNow - next) < 0.25 { return }
    boundary?.invalidate()
    let timer = Timer(timeInterval: max(0.05, next), repeats: false) { [weak self] _ in
      MainActor.assumeIsolated {
        self?.boundary = nil
        self?.consider()
      }
    }
    timer.tolerance = 0.1
    RunLoop.main.add(timer, forMode: .common)
    boundary = timer
  }
}

/// The pane grounds' clock and layer updates, on a thread of their own with
/// its own run loop: the display link (every frame) or a 10 Hz timer fires
/// there, and the marks' layers move in an explicit Core Animation
/// transaction, which Core Animation allows off the main thread. Everything
/// here after `init` runs on that thread; the main thread reaches it only
/// through `run`.
final class ThemeTicker: NSObject, @unchecked Sendable {
  static let shared = ThemeTicker()
  enum Pace: Equatable { case rest, everyFrame, slow }

  private var loop: CFRunLoop?
  private let ready = DispatchSemaphore(value: 0)
  // Ticker thread only:
  private var link: CADisplayLink?
  private var timer: Timer?
  private var time: Double = 0
  private var lastTick: Double = 0
  private var moving = false
  private var reduce = false
  private var fields: [ThemeFieldLayers] = []

  override private init() {
    super.init()
    let thread = Thread { [unowned self] in self.threadMain() }
    thread.name = "mv.theme-ground"
    thread.qualityOfService = .userInteractive
    thread.start()
    ready.wait()
  }

  private func threadMain() {
    loop = CFRunLoopGetCurrent()
    RunLoop.current.add(NSMachPort(), forMode: .default)  // keeps the loop alive
    ready.signal()
    while true { _ = RunLoop.current.run(mode: .default, before: .distantFuture) }
  }

  /// Runs `block` on the ticker thread.
  func run(_ block: @escaping () -> Void) {
    guard let loop else { return }
    CFRunLoopPerformBlock(loop, CFRunLoopMode.defaultMode.rawValue, block)
    CFRunLoopWakeUp(loop)
  }

  func add(_ field: ThemeFieldLayers) {
    run { [self] in
      if !fields.contains(where: { $0 === field }) { fields.append(field) }
      draw(field)
    }
  }

  func remove(_ field: ThemeFieldLayers) {
    run { [self] in fields.removeAll { $0 === field } }
  }

  /// A field's size, item or visibility changed: take them and draw it once.
  func update(_ field: ThemeFieldLayers, item: BuiltinThemes.Item?, size: CGSize, visible: Bool,
              scale: CGFloat) {
    run { [self] in
      if field.item != item {
        field.item = item
        field.resetPools()
      }
      field.size = size
      field.visible = visible
      field.scale = scale
      draw(field)
    }
  }

  func setReduce(_ on: Bool) {
    run { [self] in
      reduce = on
      fields.forEach(draw)
    }
  }

  /// The display link is made on the main thread (it needs the screen) and
  /// added to this thread's loop, so its callback fires here.
  func setPace(_ pace: Pace, screen: NSScreen?) {
    let newLink = pace == .everyFrame ? screen?.displayLink(target: self, selector: #selector(frame(_:))) : nil
    run { [self] in
      link?.invalidate()
      link = nil
      timer?.invalidate()
      timer = nil
      if !moving { lastTick = CACurrentMediaTime() }
      moving = pace != .rest
      switch pace {
      case .rest:
        break
      case .everyFrame:
        if let newLink {
          newLink.add(to: .current, forMode: .default)
          link = newLink
        }
      case .slow:
        let timer = Timer(timeInterval: 0.1, repeats: true) { [weak self] _ in self?.tick() }
        timer.tolerance = 0.01
        RunLoop.current.add(timer, forMode: .default)
        self.timer = timer
      }
    }
  }

  @objc private func frame(_ link: CADisplayLink) { tick() }

  private func tick() {
    let now = CACurrentMediaTime()
    time += min(0.1, now - lastTick)
    lastTick = now
    fields.forEach(draw)
  }

  private func draw(_ field: ThemeFieldLayers) {
    let moves = field.item.map { BuiltinThemes.moves($0.effect) } ?? false
    field.render(time: moves && !reduce ? time : 0)
  }
}

/// Canvas colour, then the built-in's marks. Menus keep a flat fill: this is
/// the pane ground, not a popover. The colour is SwiftUI's and never changes
/// on a tick; the marks are Core Animation layers in an AppKit view behind the
/// pane's content, moved on each tick without SwiftUI (as the Windows twin
/// moves XAML shapes).
struct ThemeField: View {
  @ObservedObject private var theme = ThemeStore.shared

  private var item: BuiltinThemes.Item? {
    guard MVTheme.themed != nil else { return nil }
    return BuiltinThemes.item(selection: theme.selection)
  }

  var body: some View {
    ZStack {
      MVTheme.canvas
      ThemeMarks(item: item)
    }
    .transaction { $0.animation = nil }
    .accessibilityHidden(true)
    .allowsHitTesting(false)
  }
}

private struct ThemeMarks: NSViewRepresentable {
  let item: BuiltinThemes.Item?
  func makeNSView(context: Context) -> ThemeFieldNSView { ThemeFieldNSView() }
  func updateNSView(_ view: ThemeFieldNSView, context: Context) { view.item = item }
}

/// One mark of a ground, in points, top-left origin, as ThemeGround computes it.
struct ThemeMark {
  enum Kind { case rect, dot, ellipse, star }
  var kind: Kind
  var x: Double, y: Double  // centre
  var w: Double, h: Double
  var rotation: Double = 0
  var alpha: Double
}

/// The marks' layers of one field. Touched only on the ticker thread, except
/// `container`, which the view adds and sizes on the main thread.
final class ThemeFieldLayers: @unchecked Sendable {
  let container = CALayer()
  var item: BuiltinThemes.Item?
  var size: CGSize = .zero
  var visible = false
  var scale: CGFloat = 2
  private var pools: [ThemeMark.Kind: [CALayer]] = [:]
  private var marks: [ThemeMark] = []

  init() {
    container.actions = ["sublayers": NSNull(), "bounds": NSNull(), "position": NSNull()]
  }

  func resetPools() {
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    for layer in pools.values.flatMap({ $0 }) { layer.removeFromSuperlayer() }
    CATransaction.commit()
    pools = [:]
  }

  /// Recompute the marks for the clock and move the layers: maths for at most
  /// ~180 marks, and layer properties set with actions off. A hidden field
  /// moves nothing and catches up when it is shown.
  func render(time: Double) {
    marks.removeAll(keepingCapacity: true)
    if let item, visible {
      ThemeGround.marks(Double(size.width), Double(size.height), item: item, time: time, into: &marks)
    } else if !visible {
      return
    }
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    var used: [ThemeMark.Kind: Int] = [:]
    let h = Double(size.height)
    for mark in marks {
      let i = used[mark.kind, default: 0]
      used[mark.kind] = i + 1
      let l = layer(kind: mark.kind, index: i)
      l.isHidden = false
      l.opacity = Float(max(0, min(1, mark.alpha)))
      let size = CGSize(width: mark.w, height: mark.h)
      if l.bounds.size != size {
        l.bounds = CGRect(origin: .zero, size: size)
        switch mark.kind {
        case .dot: l.cornerRadius = min(size.width, size.height) / 2
        case .ellipse: (l as? CAShapeLayer)?.path = CGPath(ellipseIn: l.bounds, transform: nil)
        case .star: (l as? CAShapeLayer)?.path = ThemeGround.starPath(radius: size.width / 2)
        case .rect: break
        }
      }
      // AppKit's layer space is y-up: flip the centre and the turn.
      l.position = CGPoint(x: mark.x, y: h - mark.y)
      l.setAffineTransform(mark.rotation == 0 ? .identity : CGAffineTransform(rotationAngle: -mark.rotation))
    }
    for (kind, pool) in pools {
      for l in pool.dropFirst(used[kind, default: 0]) where !l.isHidden { l.isHidden = true }
    }
    CATransaction.commit()
  }

  private func layer(kind: ThemeMark.Kind, index: Int) -> CALayer {
    if let pool = pools[kind], index < pool.count { return pool[index] }
    let ink = ThemeGround.cgColor(item?.effectRGB ?? 0xFFFFFF)
    let l: CALayer
    switch kind {
    case .rect, .dot:
      l = CALayer()
      l.backgroundColor = ink
    case .ellipse, .star:
      let shape = CAShapeLayer()
      shape.fillColor = ink
      shape.strokeColor = nil
      l = shape
    }
    l.contentsScale = scale
    l.actions = ["position": NSNull(), "bounds": NSNull(), "opacity": NSNull(),
                 "transform": NSNull(), "hidden": NSNull(), "path": NSNull()]
    container.addSublayer(l)
    pools[kind, default: []].append(l)
    return l
  }
}

/// The AppKit view behind a pane's content that carries its field's layers.
/// Main thread: it holds the clock while its theme moves and tells the ticker
/// what to draw; it never draws.
@MainActor
final class ThemeFieldNSView: NSView {
  var item: BuiltinThemes.Item? {
    didSet {
      guard item != oldValue else { return }
      syncHold()
      push()
    }
  }

  private let layers = ThemeFieldLayers()
  private var holding = false
  private var registered = false

  override init(frame: NSRect) {
    super.init(frame: frame)
    wantsLayer = true
    layerContentsRedrawPolicy = .never
    layer?.addSublayer(layers.container)
  }

  required init?(coder: NSCoder) { fatalError("ThemeFieldNSView is code-only") }

  override func hitTest(_ point: NSPoint) -> NSView? { nil }
  override func isAccessibilityElement() -> Bool { false }

  override func viewDidMoveToWindow() {
    super.viewDidMoveToWindow()
    if window != nil, !registered {
      registered = true
      ThemeTicker.shared.add(layers)
    } else if window == nil, registered {
      registered = false
      ThemeTicker.shared.remove(layers)
    }
    syncHold()
    push()
  }

  override func viewDidHide() { super.viewDidHide(); push() }
  override func viewDidUnhide() { super.viewDidUnhide(); push() }
  override func viewDidChangeBackingProperties() { super.viewDidChangeBackingProperties(); push() }

  override func setFrameSize(_ newSize: NSSize) {
    super.setFrameSize(newSize)
    push()
  }

  private var moves: Bool {
    guard let item else { return false }
    return BuiltinThemes.moves(item.effect)
  }

  private func syncHold() {
    let want = moves && window != nil
    if want && !holding {
      holding = true
      ThemeMotion.shared.hold()
    } else if !want && holding {
      holding = false
      ThemeMotion.shared.letGo()
    }
  }

  private func push() {
    if layers.container.superlayer == nil { layer?.addSublayer(layers.container) }
    CATransaction.begin()
    CATransaction.setDisableActions(true)
    layers.container.frame = bounds
    CATransaction.commit()
    ThemeTicker.shared.update(layers, item: item, size: bounds.size,
                              visible: window != nil && !isHiddenOrHasHiddenAncestor,
                              scale: window?.backingScaleFactor ?? 2)
  }
}

/// The marks of each built-in ground. The Windows twin is ThemeField.cs: the
/// counts, the speeds and the seed match. Points, top-left origin.
enum ThemeGround {
  static func cgColor(_ rgb: UInt32) -> CGColor {
    CGColor(srgbRed: CGFloat((rgb >> 16) & 0xFF) / 255, green: CGFloat((rgb >> 8) & 0xFF) / 255,
            blue: CGFloat(rgb & 0xFF) / 255, alpha: 1)
  }

  /// A four-point star of `radius`, centred in its 2r square, y-up.
  static func starPath(radius r: Double) -> CGPath {
    let path = CGMutablePath()
    let c = CGPoint(x: r, y: r)
    func p(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: c.x + x, y: c.y + y) }
    path.move(to: p(0, -r))
    path.addQuadCurve(to: p(r, 0), control: p(r * 0.15, -r * 0.15))
    path.addQuadCurve(to: p(0, r), control: p(r * 0.15, r * 0.15))
    path.addQuadCurve(to: p(-r, 0), control: p(-r * 0.15, r * 0.15))
    path.addQuadCurve(to: p(0, -r), control: p(-r * 0.15, -r * 0.15))
    path.closeSubpath()
    return path
  }

  static func marks(_ w: Double, _ h: Double, item: BuiltinThemes.Item, time: Double,
                    into out: inout [ThemeMark]) {
    guard w >= 8, h >= 8 else { return }
    let area = w * h
    let gain = item.intensity
    let pace = item.speed
    let scale = item.size
    func rect(_ x: Double, _ y: Double, _ rw: Double, _ rh: Double, _ alpha: Double) {
      out.append(ThemeMark(kind: .rect, x: x + rw / 2, y: y + rh / 2, w: rw, h: rh, alpha: alpha))
    }
    func dot(_ x: Double, _ y: Double, _ radius: Double, _ alpha: Double) {
      out.append(ThemeMark(kind: .dot, x: x, y: y, w: radius * 2, h: radius * 2, alpha: alpha))
    }
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
          dot(x, y, 1.2 * scale, alpha)
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
        rect(x, y, 1, len, alpha)
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
        dot(x, wrap(y0 + time * speed, h), radius, alpha)
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
        let off = petal * 0.15
        let pw = petal * 1.4
        let ph = petal * 0.64
        out.append(ThemeMark(kind: .ellipse, x: x - off * cos(rot), y: y - off * sin(rot),
                             w: pw, h: ph, rotation: rot, alpha: 0.20 * gain))
        out.append(ThemeMark(kind: .ellipse, x: x + off * cos(rot), y: y + off * sin(rot),
                             w: pw, h: ph, rotation: rot, alpha: 0.13 * gain))
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
          dot(x, y, (1.1 + depth * 1.5) * scale, alpha)
        }
      }
    case "synapse":
      let spacing = max(24, (w + h) / 120)
      let line = 0.035 * gain
      var row = 0
      while Double(row) * spacing < h {
        rect(0, Double(row) * spacing, w, 1, line)
        row += 1
      }
      var col = 0
      while Double(col) * spacing < w {
        rect(Double(col) * spacing, 0, 1, h, line)
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
          rect(lane, pos, 1, trail, 0.30 * gain)
          dot(lane + 0.5, pos + trail, head, 0.55 * gain)
        } else {
          rect(pos, lane, trail, 1, 0.30 * gain)
          dot(pos + trail, lane + 0.5, head, 0.55 * gain)
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
        dot(x, y, radius * 3.2, 0.14 * fade * gain)
        dot(x, y, radius * 0.65, 0.62 * fade * gain)
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
          let r = radius * (0.45 + twinkle * 0.55)
          out.append(ThemeMark(kind: .star, x: x, y: y, w: r * 2, h: r * 2, alpha: alpha))
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
          dot(at.x, at.y, radii[sample] * scale, alphas[sample] * gain)
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
