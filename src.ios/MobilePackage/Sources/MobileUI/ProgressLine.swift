// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "One progress language", on the phone: the same 3 pt track, fill, calm sweep and
// one line under it as src.swift's ProgressLine.swift / ProgressWords.swift, whose words and
// arithmetic this follows. The line wraps instead of shortening so it holds at large type.
import QuartzCore
import SwiftUI

struct ProgressMeter {
  static let tauS = 4.0
  static let warmS = 5.0
  static let readyS = 3.0
  static let readyFraction = 0.02
  static let restartDrop = 0.25

  private(set) var started = false
  private var startT = 0.0, lastT = 0.0
  private var startF = 0.0, lastF = 0.0
  private var rate = 0.0
  private(set) var shown = 0.0

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
    if dt < 0.2 || f <= lastF { return }
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

  func secondsLeft(at now: Double) -> Double? {
    guard ready else { return nil }
    return max(0, (1 - lastF) / rate - max(0, now - lastT))
  }
}

public enum ProgressWords {
  static func timeLeft(_ seconds: Double) -> String {
    if seconds < 10 { return "a few seconds left" }
    if seconds < 55 { return "about \(Int((seconds / 5).rounded()) * 5) seconds left" }
    if seconds < 90 { return "about a minute left" }
    let minutes = Int((seconds / 60).rounded())
    if minutes < 60 { return "about \(minutes) min left" }
    let rounded = Int((seconds / 300).rounded()) * 5
    let h = rounded / 60, m = rounded % 60
    return "about \(h) h" + (m > 0 ? " \(m) min" : "") + " left"
  }

  static func percent(_ fraction: Double) -> String {
    "\(Int((min(1, max(0, fraction)) * 100).rounded(.down))) %"
  }

  static func line(_ verb: String, fraction: Double?, detail: String? = nil, left: Double? = nil,
                   timed: Bool = false) -> String {
    guard let fraction else {
      if let detail, !detail.isEmpty { return verb + " · " + detail }
      return verb.hasSuffix("…") ? verb : verb + "…"
    }
    var tail = [(detail?.isEmpty == false) ? detail! : percent(fraction)]
    if fraction < 1 {
      if let left { tail.append(timeLeft(left)) } else if timed { tail.append("Estimating…") }
    }
    return ([verb] + tail).joined(separator: " · ")
  }

  /// "48 of 161 MB": both in the larger's unit.
  public static func amounts(_ done: Int64, of total: Int64) -> String {
    if total >= 1_000_000_000 {
      return String(format: "%.2f of %.2f GB", Double(max(0, done)) / 1e9, Double(total) / 1e9)
    }
    return "\(max(0, Int((Double(done) / 1e6).rounded()))) of \(max(1, Int((Double(total) / 1e6).rounded()))) MB"
  }

  public static func size(_ bytes: Int64) -> String {
    if bytes >= 1_000_000_000 { return String(format: "%.1f GB", Double(bytes) / 1e9) }
    return "\(max(1, Int((Double(bytes) / 1e6).rounded()))) MB"
  }
}

/// The bar alone. `fraction` nil: the calm sweep.
struct ProgressTrack: View {
  let fraction: Double?
  var faded = false
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  private static let sweepSeconds = 1.8
  private static let bandWidth = 0.3

  var body: some View {
    GeometryReader { geo in
      ZStack(alignment: .leading) {
        Capsule().fill(Theme.title.opacity(0.12))
        if let f = fraction {
          Capsule().fill(Theme.accent)
            .frame(width: f <= 0 ? 0 : max(3, geo.size.width * min(1, f)))
            .animation(reduceMotion ? nil : .easeOut(duration: 0.35), value: f)
        } else if reduceMotion {
          Capsule().fill(Theme.accent.opacity(0.35))
        } else {
          TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { context in
            let t = context.date.timeIntervalSinceReferenceDate
              .truncatingRemainder(dividingBy: Self.sweepSeconds) / Self.sweepSeconds
            let band = geo.size.width * Self.bandWidth
            let eased = 0.5 - 0.5 * cos(t * .pi)
            LinearGradient(colors: [Theme.accent.opacity(0), Theme.accent, Theme.accent.opacity(0)],
                           startPoint: .leading, endPoint: .trailing)
              .frame(width: band)
              .offset(x: -band + (geo.size.width + band) * eased)
          }
        }
      }
      .clipShape(Capsule())
    }
    .frame(height: 3)
    .opacity(faded ? 0 : 1)
    .animation(reduceMotion ? nil : .easeOut(duration: 0.25), value: faded)
    .accessibilityHidden(true)
  }
}

/// How a line ends, in the same slot.
public enum ProgressOutcome: Equatable {
  case done(String)
  case failed(String, canRetry: Bool)
}

/// Keeps each keyed bar's meter across SwiftUI updates.
@MainActor
final class ProgressMemory {
  static let shared = ProgressMemory()
  private var meters: [String: ProgressMeter] = [:]

  func sample(_ key: String, _ fraction: Double?) -> ProgressMeter {
    var m = meters[key] ?? ProgressMeter()
    m.add(fraction, at: CACurrentMediaTime())
    meters[key] = m
    return m
  }

  func forget(_ key: String) { meters[key] = nil }
}

private extension View {
  /// The quiet button's side padding is only touch area. Taken back out on the outer edges only,
  /// the words line up with the track's end (the last button) and with the words above at the
  /// largest text sizes (every button's leading edge), and neighbours' touch areas never overlap.
  func alignedWithLine(leading: Bool, trailing: Bool) -> some View {
    padding(.leading, leading ? -8 : 0).padding(.trailing, trailing ? -8 : 0)
  }
}

/// The bar and its one line.
struct ProgressLine: View {
  let verb: String
  let fraction: Double?
  var detail: String? = nil
  /// The bar's identity across updates; with one, the bar never goes back and the time left shows.
  var key: String? = nil
  var outcome: ProgressOutcome? = nil
  var onCancel: (() -> Void)? = nil
  var onRetry: (() -> Void)? = nil
  /// One more way out of a failure, such as "Open Settings" when Photos access was refused.
  var extra: (title: String, action: () -> Void)? = nil

  @Environment(\.dynamicTypeSize) private var typeSize

  private var lineLayout: AnyLayout {
    typeSize.isAccessibilitySize
      ? AnyLayout(VStackLayout(alignment: .leading, spacing: 4))
      : AnyLayout(HStackLayout(alignment: .firstTextBaseline, spacing: 8))
  }

  var body: some View {
    let meter = key.map { ProgressMemory.shared.sample($0, fraction) }
    let shown = fraction.map { f in meter.map { $0.started ? $0.shown : f } ?? f }
    VStack(alignment: .leading, spacing: 6) {
      ProgressTrack(fraction: outcome == nil ? shown : 1, faded: outcome != nil)
      // At the largest text the words take the whole width and the actions go under them.
      lineLayout {
        if let outcome {
          outcomeWords(outcome)
        } else if let meter, let f = shown, f < 1 {
          TimelineView(.periodic(from: .now, by: 1)) { _ in
            words(ProgressWords.line(verb, fraction: f, detail: detail,
                                     left: meter.secondsLeft(at: CACurrentMediaTime()), timed: true))
          }
        } else {
          words(ProgressWords.line(verb, fraction: shown, detail: detail))
        }
        if !typeSize.isAccessibilitySize { Spacer(minLength: 0) }
        let stacked = typeSize.isAccessibilitySize
        let showsExtra = outcomeIsFailure && extra != nil
        let showsRetry = outcomeCanRetry && onRetry != nil
        let showsCancel = outcomeAllowsCancel && onCancel != nil
        if showsExtra, let extra {
          Button(extra.title, action: extra.action).buttonStyle(QuietButtonStyle(color: Theme.title))
            .alignedWithLine(leading: stacked, trailing: stacked || !(showsRetry || showsCancel))
        }
        if showsRetry, let onRetry {
          Button("Try again", action: onRetry).buttonStyle(QuietButtonStyle(color: Theme.title))
            .alignedWithLine(leading: stacked, trailing: stacked || !showsCancel)
        }
        if showsCancel, let onCancel {
          Button("Cancel", action: onCancel).buttonStyle(QuietButtonStyle())
            .alignedWithLine(leading: stacked, trailing: true)
        }
      }
      .frame(minHeight: 22)
    }
    // The words are read as written, and Cancel and Try again stay reachable on their own.
    .accessibilityElement(children: .contain)
  }

  private var outcomeIsFailure: Bool {
    if case .failed? = outcome { return true }
    return false
  }

  private var outcomeCanRetry: Bool {
    if case .failed(_, true)? = outcome { return true }
    return false
  }

  private var outcomeAllowsCancel: Bool {
    switch outcome {
    case nil, .failed: return true
    case .done: return false
    }
  }

  private func words(_ text: String) -> some View {
    Text(text).font(.footnote.monospacedDigit()).foregroundStyle(Theme.body)
  }

  @ViewBuilder
  private func outcomeWords(_ outcome: ProgressOutcome) -> some View {
    switch outcome {
    case .done(let text):
      Label {
        Text(text).font(.footnote).foregroundStyle(Theme.title)
      } icon: {
        Image(systemName: "checkmark.circle.fill").font(.footnote).foregroundStyle(Theme.accent)
      }
    case .failed(let reason, _):
      Label {
        Text(reason).font(.footnote).foregroundStyle(Theme.title)
      } icon: {
        Image(systemName: "exclamationmark.circle.fill").font(.footnote).foregroundStyle(Theme.warm)
      }
    }
  }
}
