// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Craft rules" (2026-11-05): the one progress language's words
// and arithmetic, apart from any view so the tests can drive it. A run's line
// reads "Enhancing · 40 % · about 1 min left"; a download's "Downloading the
// detail model · 48 of 161 MB · about 25 seconds left". The time left follows
// Driftwire's meter (src/addons/driftwire/eta.h): the average since the start
// for the first seconds, then a moving average with a 4 s time constant, no
// estimate before 3 s and 2 % moved ("Estimating…" until then), and the same
// rounded words, so it never flickers. The bar never goes back: a smaller
// fraction is held at the largest seen, unless it falls by more than a
// quarter (a new run under the same name). The Windows twin is
// src.managed/Shared/ProgressWords.cs; the C# tests read this file for the
// same constants and words.
import Foundation

struct ProgressMeter {
  static let tauS = 4.0  // the moving average's time constant
  static let warmS = 5.0  // the average since the start until then
  static let readyS = 3.0  // no estimate before this long...
  static let readyFraction = 0.02  // ...or this much moved
  static let restartDrop = 0.25  // a fall this large is a new run

  private(set) var started = false
  private var startT = 0.0, lastT = 0.0
  private var startF = 0.0, lastF = 0.0
  private var rate = 0.0
  /// The largest fraction seen this run: what the bar shows.
  private(set) var shown = 0.0

  /// The run's fraction (0...1) at `now` seconds; nil (unknown) changes nothing.
  mutating func add(_ fraction: Double?, at now: Double) {
    guard let raw = fraction, raw.isFinite else { return }
    let f = min(1, max(0, raw))
    if !started || f < shown - Self.restartDrop {
      started = true
      startT = now; lastT = now
      startF = f; lastF = f
      rate = 0
      shown = f
      return
    }
    shown = max(shown, f)
    let dt = now - lastT
    if dt < 0.2 || f <= lastF { return }  // updates bunch up; a still bar teaches nothing
    let since = now - startT
    if since <= Self.warmS {
      rate = (f - startF) / since
    } else {
      let inst = (f - lastF) / dt
      rate += (1 - exp(-dt / Self.tauS)) * (inst - rate)
    }
    lastT = now
    lastF = f
  }

  var ready: Bool {
    started && rate > 0 && lastT - startT >= Self.readyS && lastF - startF >= Self.readyFraction
  }

  /// Seconds to the end at `now`, counting down between updates; nil before ready.
  func secondsLeft(at now: Double) -> Double? {
    guard ready else { return nil }
    return max(0, (1 - lastF) / rate - max(0, now - lastT))
  }
}

enum ProgressWords {
  /// "about 4 min left", rounded so it does not flicker (Driftwire's words).
  static func timeLeft(_ seconds: Double) -> String {
    if seconds < 10 { return "a few seconds left" }
    if seconds < 55 { return "about \(Int((seconds / 5).rounded()) * 5) seconds left" }
    if seconds < 90 { return "about a minute left" }
    let minutes = Int((seconds / 60).rounded())
    if minutes < 60 { return "about \(minutes) min left" }
    let rounded = Int((seconds / 300).rounded()) * 5  // to five minutes past the hour
    let h = rounded / 60, m = rounded % 60
    return "about \(h) h" + (m > 0 ? " \(m) min" : "") + " left"
  }

  /// The percentage as the line writes it: whole, rounded down, so 100 % means done.
  static func percent(_ fraction: Double) -> String {
    "\(Int((min(1, max(0, fraction)) * 100).rounded(.down))) %"
  }

  /// The one line under a bar. `detail` ("48 of 161 MB") takes the place of
  /// the percentage; `timed` lines say "Estimating…" until there is a rate.
  /// Unknown (nil fraction): the verb with an ellipsis, and the detail.
  static func line(_ verb: String, fraction: Double?, detail: String? = nil, left: Double? = nil,
                   timed: Bool = false) -> String {
    let p = parts(verb, fraction: fraction, detail: detail, left: left, timed: timed)
    return p.head + p.tail
  }

  /// The line in two: the verb, which may shorten in a narrow pane, and the
  /// numbers after it (" · 40 % · about 1 min left"), which never do.
  static func parts(_ verb: String, fraction: Double?, detail: String? = nil, left: Double? = nil,
                    timed: Bool = false) -> (head: String, tail: String) {
    guard let fraction else {
      if let detail, !detail.isEmpty { return (verb, " · " + detail) }
      return (verb.hasSuffix("…") ? verb : verb + "…", "")
    }
    var tail: [String] = []
    if let detail, !detail.isEmpty { tail.append(detail) } else { tail.append(percent(fraction)) }
    if fraction < 1 {
      if let left { tail.append(timeLeft(left)) } else if timed { tail.append("Estimating…") }
    }
    return (verb, tail.map { " · " + $0 }.joined())
  }

  /// "48 of 161 MB": both in the larger's unit, as a download is read.
  static func amounts(_ done: Int64, of total: Int64) -> String {
    guard total > 0 else { return AddonChannel.mbText(done) }
    if total >= 1_000_000_000 {
      return String(format: "%.2f of %.2f GB", Double(max(0, done)) / 1e9, Double(total) / 1e9)
    }
    return "\(max(0, Int((Double(done) / 1e6).rounded()))) of \(max(1, Int((Double(total) / 1e6).rounded()))) MB"
  }
}

/// docs/design/20 "Craft rules": how a slider says its value, beside its
/// label. A slider with a whole-number step of 1 or more counts ("3"); any
/// other is said as how far along its range it sits ("70 %"), so Strength
/// 0–1 reads "70 %" and Restyle's Keep the photo 1–2.5 reads "33 %", never
/// "1.50". The Windows twin is ProgressWords.SliderValue.
enum SliderWords {
  static func value(_ v: Double, min lo: Double, max hi: Double, step: Double?) -> String {
    if let step, step >= 1, step == step.rounded(), lo == lo.rounded() { return String(Int(v.rounded())) }
    guard hi > lo else { return "0 %" }
    return "\(Int(((v - lo) / (hi - lo) * 100).rounded())) %"
  }
}
