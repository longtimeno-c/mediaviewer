// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/design/20 "Craft rules" (2026-11-05): the chrome's one progress
// language. Every bar the Mac chrome draws is a ProgressTrack (a 3 pt rounded
// track, an accent fill that eases forward and never back, a calm sweep when
// there is no fraction), and every bar that says something is a ProgressLine:
// the track, then one line ("Enhancing · 40 % · about 1 min left") with a
// quiet Cancel at its end, in fixed slots so nothing reflows. When the work
// ends, the line's slot carries the outcome (the result and its action, or a
// one-line reason and Try again). Spinners are for waits under a second.
// The words and arithmetic are ProgressWords.swift; the Windows twin is
// IslandHost.Progress.cs.
import QuartzCore
import SwiftUI

/// The bar alone. `fraction` nil: indeterminate.
struct ProgressTrack: View {
  let fraction: Double?
  var height: CGFloat = 3
  /// A finished or failed run keeps its slot with the bar faded out.
  var faded = false
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  static let sweepSeconds = 1.8
  static let bandWidth = 0.3

  var body: some View {
    GeometryReader { geo in
      ZStack(alignment: .leading) {
        Capsule().fill(MVTheme.title.opacity(0.10))
        if let f = fraction {
          Capsule().fill(MVTheme.accent)
            .frame(width: f <= 0 ? 0 : max(height, geo.size.width * min(1, f)))
            .animation(reduceMotion ? nil : .easeOut(duration: 0.35), value: f)
        } else if reduceMotion {
          Capsule().fill(MVTheme.accent.opacity(0.35))
        } else {
          // The calm sweep: a soft band glides across and eases at both ends.
          // TimelineView ticks only while the bar is on screen and unknown.
          TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { context in
            let t = context.date.timeIntervalSinceReferenceDate
              .truncatingRemainder(dividingBy: Self.sweepSeconds) / Self.sweepSeconds
            let band = geo.size.width * Self.bandWidth
            let eased = 0.5 - 0.5 * cos(t * .pi)
            LinearGradient(colors: [MVTheme.accent.opacity(0), MVTheme.accent, MVTheme.accent.opacity(0)],
                           startPoint: .leading, endPoint: .trailing)
              .frame(width: band)
              .offset(x: -band + (geo.size.width + band) * eased)
          }
        }
      }
      .clipShape(Capsule())
    }
    .frame(height: height)
    .opacity(faded ? 0 : 1)
    .animation(.easeOut(duration: 0.25), value: faded)
    .accessibilityHidden(true)
  }
}

/// How a line ends: in place of the progress words, in the same slot.
enum ProgressOutcome: Equatable {
  /// "Saved as IMG_0042 enhanced.jpg", with its action ("Move to Trash").
  case done(String, action: String?)
  /// "Enhance stopped: out of memory", with Try again.
  case failed(String, retry: Bool)
}

/// Remembers each keyed bar's meter across rebuilds (an add-on's tree is
/// rebuilt at every revision), and forgets a bar not seen for 30 s.
@MainActor
final class ProgressMemory {
  static let shared = ProgressMemory()
  private var meters: [String: (meter: ProgressMeter, seen: Double)] = [:]

  static var now: Double { CACurrentMediaTime() }

  func sample(_ key: String, _ fraction: Double?) -> ProgressMeter {
    let now = Self.now
    if meters.count > 64 { meters = meters.filter { now - $0.value.seen < 30 } }
    var m = meters[key].flatMap { now - $0.seen < 30 ? $0.meter : nil } ?? ProgressMeter()
    m.add(fraction, at: now)
    meters[key] = (m, now)
    return m
  }
}

/// The bar and its one line. With a `key`, the bar never goes back and the
/// time left is estimated (Estimating… until there is a rate).
struct ProgressLine: View {
  let verb: String
  let fraction: Double?
  var detail: String? = nil
  /// The bar's identity across rebuilds; nil: no memory, no time left.
  var key: String? = nil
  var timed = true
  var outcome: ProgressOutcome? = nil
  var cancelTitle = "Cancel"
  /// Esc cancels too (only where no other Esc is meant: a sheet, the tool bar).
  var escCancels = false
  var onCancel: (() -> Void)? = nil
  var onAction: (() -> Void)? = nil
  var onRetry: (() -> Void)? = nil

  var body: some View {
    let meter = key.map { ProgressMemory.shared.sample($0, fraction) }
    let shown = fraction.map { f in meter.map { $0.started ? $0.shown : f } ?? f }
    VStack(alignment: .leading, spacing: MVTheme.space(6)) {
      ProgressTrack(fraction: outcome == nil ? shown : 1, faded: outcome != nil)
      HStack(spacing: MVTheme.space(8)) {
        if let outcome {
          outcomeRow(outcome)
        } else if let meter, timed, let f = shown, f < 1 {
          // Counts down between updates, once a second, only while it runs.
          TimelineView(.periodic(from: .now, by: 1)) { _ in
            words(ProgressWords.parts(verb, fraction: f, detail: detail,
                                      left: meter.secondsLeft(at: ProgressMemory.now), timed: true))
          }
        } else {
          words(ProgressWords.parts(verb, fraction: shown, detail: detail))
        }
        Spacer(minLength: 0)
        // The end of the line: Cancel while it runs, the outcome's action after.
        if let outcome {
          switch outcome {
          case .done(_, let action?) where onAction != nil:
            QuietButton(title: action, action: onAction!)
          case .failed(_, true) where onRetry != nil:
            QuietButton(title: "Try again", action: onRetry!)
          default:
            EmptyView()
          }
        } else if let onCancel {
          QuietButton(title: cancelTitle, action: onCancel)
            .modifier(EscCancels(on: escCancels))
        }
      }
      .frame(minHeight: 22)
    }
    .accessibilityElement(children: .combine)
    .accessibilityLabel(verb)
    .accessibilityValue(shown.map { ProgressWords.percent($0) } ?? "In progress")
  }

  private func words(_ p: (head: String, tail: String)) -> some View {
    HStack(spacing: 0) {
      Text(p.head).lineLimit(1).truncationMode(.tail)
      Text(p.tail).lineLimit(1).fixedSize().layoutPriority(1)
    }
    .font(MVTheme.font(12).monospacedDigit())
    .foregroundStyle(MVTheme.body)
  }

  @ViewBuilder
  private func outcomeRow(_ o: ProgressOutcome) -> some View {
    switch o {
    case .done(let text, _):
      Image(systemName: "checkmark.circle.fill").font(.system(size: 12)).foregroundStyle(MVTheme.accent)
        .accessibilityHidden(true)
      Text(text).font(MVTheme.font(12)).foregroundStyle(MVTheme.title).lineLimit(1).truncationMode(.middle)
        .help(text)
    case .failed(let reason, _):
      Image(systemName: "exclamationmark.circle.fill").font(.system(size: 12)).foregroundStyle(MVTheme.warm)
        .accessibilityHidden(true)
      Text(reason).font(MVTheme.font(12)).foregroundStyle(MVTheme.title).lineLimit(1).truncationMode(.tail)
        .help(reason)
    }
  }
}

private struct EscCancels: ViewModifier {
  let on: Bool
  func body(content: Content) -> some View {
    if on { content.keyboardShortcut(.cancelAction) } else { content }
  }
}

/// A quiet text action: the body colour, a wash on hover, no border. Cancel,
/// Try again, Move to Trash: actions that sit at the end of a line.
struct QuietButton: View {
  let title: String
  let action: () -> Void
  @State private var hover = false
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    Button(action: action) {
      Text(title)
        .font(MVTheme.font(12))
        .foregroundStyle(!enabled ? MVTheme.disabled : (hover ? MVTheme.title : MVTheme.body))
        .padding(.horizontal, MVTheme.space(8))
        .padding(.vertical, MVTheme.space(3))
        .background(RoundedRectangle(cornerRadius: MVTheme.radius(5))
          .fill(hover && enabled ? MVTheme.hoverWash : .clear))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .fixedSize()
    .onHover { hover = $0 }
    .animation(.easeOut(duration: 0.12), value: hover)
  }
}
