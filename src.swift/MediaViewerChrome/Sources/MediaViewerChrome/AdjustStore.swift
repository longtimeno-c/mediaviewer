// SPDX-License-Identifier: GPL-2.0-or-later
// PR 11 (plan/07, plan/16): the state the adjust pane reads. It mirrors the
// host's shell::adjust_view -- readiness, slider values, histogram, clipping --
// and re-reads it only when the host's adjust generation moves, which it does
// for everything except the pane's own slider (so a drag is never fought).
import AppKit
import Combine
import MVChromeBridge

struct AdjustSpec {
  let name: String
  let min: Double
  let max: Double
  let step: Double
  let format: (Double) -> String
}

@MainActor
final class AdjustStore: ObservableObject {
  static let shared = AdjustStore()

  /// edit::adjust_param order and edit::range_of, the Windows pane's table too.
  static let specs: [AdjustSpec] = [
    AdjustSpec(name: "Exposure", min: -5, max: 5, step: 0.1) { String(format: "%+.1f EV", $0) },
    AdjustSpec(name: "Contrast", min: -100, max: 100, step: 5) { String(format: "%+.0f", $0) },
    AdjustSpec(name: "Saturation", min: -100, max: 100, step: 5) { String(format: "%+.0f", $0) },
    AdjustSpec(name: "Temperature", min: -100, max: 100, step: 5) { String(format: "%+.0f", $0) },
    AdjustSpec(name: "Tint", min: -100, max: 100, step: 5) { String(format: "%+.0f", $0) },
  ]
  static let bins = 64

  @Published private(set) var visible = false
  @Published private(set) var readiness: Int32 = 0  // 0 none, 1 preparing, 2 ready, 3 failed
  @Published private(set) var fromRaw = false
  @Published var values: [Double] = Array(repeating: 0, count: 5)
  @Published private(set) var histogramValid = false
  @Published private(set) var histogram: [[Double]] = Array(repeating: [], count: 4)  // R, G, B, luma; 0..1
  @Published private(set) var clipHigh: Double = 0
  @Published private(set) var clipLow: Double = 0

  var ready: Bool { readiness == 2 }

  private var generation: UInt64 = .max
  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    poll()
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
  func close() { mv_chrome_adjust_close() }
  func blur() { mv_chrome_adjust_blur() }
}
