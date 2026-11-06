// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 11 (docs/design/07, docs/design/16): the state the adjust pane reads. It mirrors the
// host's shell::adjust_view -- readiness, slider values, histogram, clipping --
// and re-reads it only when the host's adjust generation moves, which it does
// for everything except the pane's own slider (so a drag is never fought).
import AppKit
import Combine
import MVChromeBridge
import SwiftUI

struct AdjustSpec {
  let name: String
  let min: Double
  let max: Double
  let step: Double
  let format: (Double) -> String
}

/// edit::adjust_section: a group of sliders with its own reset and, but for
/// the vignette, its own Auto. `params` are edit::adjust_param values, in the
/// order the pane shows them.
struct AdjustSection {
  let name: String
  let icon: String
  let params: [Int]
  let hasAuto: Bool
}

@MainActor
final class AdjustStore: ObservableObject {
  static let shared = AdjustStore()

  /// edit::adjust_param order and edit::range_of, the Windows pane's table too.
  static let specs: [AdjustSpec] = {
    let signed: (Double) -> String = { String(format: "%+.0f", $0) }
    func s(_ name: String) -> AdjustSpec { AdjustSpec(name: name, min: -100, max: 100, step: 5, format: signed) }
    return [
      AdjustSpec(name: "Exposure", min: -5, max: 5, step: 0.1) { String(format: "%+.1f EV", $0) },
      s("Contrast"), s("Saturation"), s("Temperature"), s("Tint"),
      s("Highlights"), s("Shadows"), s("Whites"), s("Blacks"), s("Vibrance"),
      AdjustSpec(name: "Intensity", min: 0, max: 100, step: 5) { String(format: "%.0f", $0) },
      s("Filter"), s("Tone"),
      s("Strength"), s("Radius"), s("Softness"),
    ] + (0..<4).flatMap { _ in
      [
        AdjustSpec(name: "Black", min: 0, max: 100, step: 1) { String(format: "%.0f", $0) },
        AdjustSpec(name: "Mid-tones", min: -100, max: 100, step: 2, format: signed),
        AdjustSpec(name: "White", min: -100, max: 0, step: 1) { String(format: "%.0f", $0) },
      ]
    } + (0..<8).flatMap { _ in [s("Hue"), s("Saturation"), s("Luminance")] } + [
      s("Definition"), s("Brilliance"),
      AdjustSpec(name: "Sharpen", min: 0, max: 100, step: 5) { String(format: "%.0f", $0) },
      AdjustSpec(name: "Noise reduction", min: 0, max: 100, step: 5) { String(format: "%.0f", $0) },
    ]
  }()
  /// edit::adjust_section order.
  static let sections: [AdjustSection] = [
    AdjustSection(name: "Light", icon: "sun.max", params: [0, 5, 6, 7, 8, 1], hasAuto: true),
    AdjustSection(name: "Colour", icon: "circle.lefthalf.filled", params: [2, 9, 3, 4], hasAuto: true),
    AdjustSection(name: "Black & White", icon: "circle.righthalf.filled", params: [10, 11, 12], hasAuto: true),
    AdjustSection(name: "Vignette", icon: "circle.dashed", params: [13, 14, 15], hasAuto: false),
    // Slice 2. Levels' sliders are those of the channel picked (levelsParams);
    // Curves has an editor, no slider.
    AdjustSection(name: "Levels", icon: "chart.bar", params: Array(16..<28), hasAuto: true),
    AdjustSection(name: "Curves", icon: "point.topleft.down.to.point.bottomright.curvepath", params: [],
                  hasAuto: false),
    // Slice 3. Selective Colour's sliders are the picked band's (selectiveParams);
    // Red-Eye has a tool, no slider.
    AdjustSection(name: "Selective Colour", icon: "paintpalette", params: Array(28..<52), hasAuto: false),
    AdjustSection(name: "Red-Eye", icon: "eye.slash", params: [], hasAuto: false),
    // Slice 4: a pass over the working image; the canvas follows when it settles.
    AdjustSection(name: "Detail", icon: "triangle", params: [52, 53, 54, 55], hasAuto: false),
    // Slice 6: a button, no slider.
    AdjustSection(name: "Faces", icon: "face.smiling", params: [], hasAuto: false),
    // Slice 7: a brush over the photo; Enhance takes out what it covers.
    AdjustSection(name: "Remove", icon: "eraser", params: [], hasAuto: false),
  ]
  static let facesSection = 9
  static let removeSection = 10
  static let selectiveSection = 6
  static let redEyeSection = 7
  static let bandNames = ["Red", "Orange", "Yellow", "Green", "Aqua", "Blue", "Purple", "Magenta"]
  static let bandColours: [Color] = [.red, .orange, .yellow, .green, .cyan, .blue, .purple, .pink]
  static func selectiveParams(_ band: Int) -> [Int] { [28 + 3 * band, 29 + 3 * band, 30 + 3 * band] }
  static let levelsSection = 4
  static let curvesSection = 5
  static let channelNames = ["Image", "Red", "Green", "Blue"]
  /// edit::adjust_param of channel `ch`'s black, mid-tones and white.
  static func levelsParams(_ ch: Int) -> [Int] { [16 + 3 * ch, 17 + 3 * ch, 18 + 3 * ch] }
  static let monoParam = 10
  static let bins = 64

  @Published private(set) var visible = false
  @Published private(set) var readiness: Int32 = 0  // 0 none, 1 preparing, 2 ready, 3 failed
  @Published private(set) var fromRaw = false
  @Published var values: [Double] = Array(repeating: 0, count: AdjustStore.specs.count)
  /// The section whose Auto is running, or -1.
  @Published private(set) var autoSection: Int = -1
  /// Slice 3: 0 nothing, 1 the white-balance picker, 2 the red-eye tool; the fixes made.
  @Published private(set) var picking: Int32 = 0
  @Published private(set) var spotCount = 0
  /// Slice 7: the Remove brush's strokes, and whether it can take no more.
  @Published private(set) var brushStrokes = 0
  @Published private(set) var brushFull = false
  /// Slice 5b: what the last typed request did.
  @Published private(set) var promptNote = ""
  /// Slice 2: the four curves' points (image, red, green, blue); empty is the diagonal.
  @Published var curves: [[CGPoint]] = Array(repeating: [], count: 4)
  @Published private(set) var histogramValid = false
  @Published private(set) var histogram: [[Double]] = Array(repeating: [], count: 4)  // R, G, B, luma; 0..1
  @Published private(set) var clipHigh: Double = 0
  @Published private(set) var clipLow: Double = 0

  var ready: Bool { readiness == 2 }

  private var generation: UInt64 = .max

  private init() {
    // Issue #178: polled when the main run loop has run (ChromePulse), not on a timer.
    ChromePulse.shared.add { [weak self] in
      self?.poll()
      return false
    }
  }

  private func poll() {
    let nowVisible = mv_chrome_adjust_visible()
    if nowVisible != visible { visible = nowVisible }
    guard nowVisible else { return }  // closed: no work
    let g = mv_chrome_adjust_generation()
    guard g != generation else { return }
    generation = g
    reload()
  }

  private func reload() {
    var v = mv_adjust_view()
    guard mv_chrome_adjust_view(&v) else { return }
    readiness = v.readiness
    fromRaw = v.from_raw != 0
    values = withUnsafeBytes(of: v.values) { raw in raw.bindMemory(to: Float.self).map { Double($0) } }
    histogramValid = v.histogram_valid != 0
    autoSection = Int(v.auto_section)
    picking = v.picking
    spotCount = Int(v.spot_count)
    brushStrokes = Int(v.brush_strokes)
    brushFull = v.brush_full != 0
    promptNote = withUnsafeBytes(of: v.prompt_note) { raw in
      String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
    }
    let ns: [Int32] = withUnsafeBytes(of: v.curve_n) { Array($0.bindMemory(to: Int32.self)) }
    let xs: [Float] = withUnsafeBytes(of: v.curve_x) { Array($0.bindMemory(to: Float.self)) }
    let ys: [Float] = withUnsafeBytes(of: v.curve_y) { Array($0.bindMemory(to: Float.self)) }
    curves = (0..<4).map { ch in
      (0..<Int(ns[ch])).map { i in CGPoint(x: Double(xs[ch * 8 + i]), y: Double(ys[ch * 8 + i])) }
    }
    clipHigh = Double(v.clip_high)
    clipLow = Double(v.clip_low)
    let flat: [UInt16] = withUnsafeBytes(of: v.bins) { raw in Array(raw.bindMemory(to: UInt16.self)) }
    histogram = (0..<4).map { c in
      (0..<Self.bins).map { i in Double(min(flat[c * Self.bins + i], 1000)) / 1000.0 }
    }
  }

  /// A slider (or an arrow key) moved `index` to `value`, clamped and snapped.
  func set(_ index: Int, _ value: Double) {
    guard ready, index >= 0, index < Self.specs.count else { return }
    let spec = Self.specs[index]
    let snapped = (min(spec.max, max(spec.min, value)) / spec.step).rounded() * spec.step
    guard snapped != values[index] else { return }
    values[index] = snapped
    mv_chrome_adjust_set(Int32(index), Float(snapped))
  }

  func reset() { mv_chrome_adjust_reset() }
  /// Slice 5b: a typed request; the host moves the sliders it names.
  func prompt(_ text: String) { if ready { mv_chrome_adjust_prompt(text) } }
  func openEyes() { if ready { mv_chrome_adjust_open_eyes() } }
  /// Slice 7: 0 the last stroke off, 1 every stroke off, 2 Remove (Enhance's sheet).
  func brush(_ action: Int32) { if ready { mv_chrome_adjust_brush(action) } }
  /// Slice 3: what the next click on the photo does (see `picking`).
  func pick(_ mode: Int32, size: Double = 0) { if ready { mv_chrome_adjust_pick(mode, Float(size)) } }
  func auto(_ section: Int) { if ready { mv_chrome_adjust_auto(Int32(section)) } }
  func reset(section: Int) {
    guard ready else { return }
    if section == Self.removeSection { brush(1) } else { mv_chrome_adjust_reset_section(Int32(section)) }
  }
  /// A slider of `section` is away from 0 (Curves: a curve off the diagonal).
  func changed(_ section: Int) -> Bool {
    if section == Self.curvesSection { return curves.contains { !Self.diagonal($0) } }
    if section == Self.redEyeSection { return spotCount > 0 }
    if section == Self.removeSection { return brushStrokes > 0 }
    return Self.sections[section].params.contains { values[$0] != 0 }
  }

  static func diagonal(_ pts: [CGPoint]) -> Bool { pts.allSatisfy { $0.x == $0.y } }

  /// Curve `ch` is now `pts` (sorted by x, at most 8); the core sanitises.
  func setCurve(_ ch: Int, _ pts: [CGPoint]) {
    guard ready, ch >= 0, ch < 4 else { return }
    let sorted = Array(pts.sorted { $0.x < $1.x }.prefix(8))
    curves[ch] = sorted
    let xs = sorted.map { Float($0.x) }, ys = sorted.map { Float($0.y) }
    xs.withUnsafeBufferPointer { x in
      ys.withUnsafeBufferPointer { y in
        mv_chrome_adjust_curve(Int32(ch), Int32(sorted.count), x.baseAddress, y.baseAddress)
      }
    }
  }

  /// edit::evaluate: a monotone cubic (Fritsch–Carlson) through `pts`, flat past the ends.
  static func evaluate(_ pts: [CGPoint], _ p: Double) -> Double {
    let n = pts.count
    guard n >= 2 else { return p }
    if p <= pts[0].x { return pts[0].y }
    if p >= pts[n - 1].x { return pts[n - 1].y }
    var d = [Double](repeating: 0, count: n - 1)
    for i in 0..<(n - 1) { d[i] = (pts[i + 1].y - pts[i].y) / (pts[i + 1].x - pts[i].x) }
    var m = [Double](repeating: 0, count: n)
    m[0] = d[0]
    m[n - 1] = d[n - 2]
    if n > 2 { for i in 1..<(n - 1) { m[i] = d[i - 1] * d[i] <= 0 ? 0 : (d[i - 1] + d[i]) / 2 } }
    for i in 0..<(n - 1) {
      if d[i] == 0 { m[i] = 0; m[i + 1] = 0; continue }
      let a = m[i] / d[i], b = m[i + 1] / d[i], h = a * a + b * b
      if h > 9 { let t = 3 / h.squareRoot(); m[i] = t * a * d[i]; m[i + 1] = t * b * d[i] }
    }
    var k = 0
    while k + 2 < n && p > pts[k + 1].x { k += 1 }
    let h = pts[k + 1].x - pts[k].x, t = (p - pts[k].x) / h, t2 = t * t, t3 = t2 * t
    return (2 * t3 - 3 * t2 + 1) * pts[k].y + (t3 - 2 * t2 + t) * h * m[k]
      + (-2 * t3 + 3 * t2) * pts[k + 1].y + (t3 - t2) * h * m[k + 1]
  }
  func close() { mv_chrome_adjust_close() }
  func blur() { mv_chrome_adjust_blur() }
}
