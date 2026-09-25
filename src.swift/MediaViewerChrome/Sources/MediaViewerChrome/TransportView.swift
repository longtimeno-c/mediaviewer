// SPDX-License-Identifier: GPL-2.0-or-later
// PR 19: the clip transport strip (plan/05, plan/16 "Video": bottom-centre,
// play/pause, scrubber, time, speed, mute). Chrome only: the clock, the seek
// model and the canvas are native; this posts commands over the bridge.
import SwiftUI

private struct VolumeSliderRow: View {
  @ObservedObject private var store = VideoStore.shared

  var body: some View {
    HStack(spacing: 6) {
      Image(systemName: "speaker.fill").font(.caption)
      Slider(
        value: Binding(
          get: { Double(store.draggingVolume ?? store.volume) },
          set: { store.setVolume(Float($0)) }),
        in: 0...1,
        onEditingChanged: { editing in if !editing { store.endVolumeDrag() } }
      )
      .frame(width: 120)
      Image(systemName: "speaker.wave.3.fill").font(.caption)
    }
    .padding(.horizontal, 10)
    .padding(.vertical, 4)
  }
}

/// Command-table ids (src/shell/commands.h; chrome_host.h pins them).
enum TrimCommand {
  static let keyframe: Int32 = 132
  static let reencode: Int32 = 133
}

/// Trim over the scrubber (plan/08: "show the keyframe grid on the timeline so
/// the snapping is visible and expected rather than surprising").
private struct TrimMarks: View {
  @ObservedObject private var store = VideoStore.shared
  // The system slider's track sits about half a knob in from its frame.
  private let inset: CGFloat = 10

  var body: some View {
    Canvas { ctx, size in
      guard store.trimArmed, store.trimDurationNs > 0 else { return }
      let span = max(size.width - 2 * inset, 1)
      func x(_ t: Int64) -> CGFloat {
        inset + span * CGFloat(min(max(Double(t) / Double(store.trimDurationNs), 0), 1))
      }
      if store.trimCutInNs >= 0, store.trimCutOutNs > store.trimCutInNs {
        let a = x(store.trimCutInNs), b = x(store.trimCutOutNs)
        ctx.fill(Path(CGRect(x: a, y: 0, width: max(1, b - a), height: size.height)),
                 with: .color(Color.orange.opacity(0.25)))
      }
      var last: CGFloat = -10
      for k in store.trimKeyframesNs {
        let px = x(k)
        if px - last < 2 { continue }  // one tick per two points on a long clip
        last = px
        ctx.fill(Path(CGRect(x: px, y: size.height - 5, width: 1, height: 5)),
                 with: .color(Color.secondary.opacity(0.8)))
      }
      for m in [store.trimInNs, store.trimOutNs] where m >= 0 {
        ctx.fill(Path(CGRect(x: x(m) - 1, y: 0, width: 2, height: size.height)), with: .color(.orange))
      }
    }
  }
}

struct TransportView: View {
  @ObservedObject private var store = VideoStore.shared

  private static func clock(_ ms: Int64) -> String {
    let total = max(0, Int(ms / 1000))
    let h = total / 3600, m = (total % 3600) / 60, s = total % 60
    return h > 0 ? String(format: "%d:%02d:%02d", h, m, s) : String(format: "%d:%02d", m, s)
  }

  var body: some View {
    let duration = max(store.durationMs, 1)
    let shown = store.scrubMs ?? store.positionMs
    HStack(spacing: 10) {
      Button(action: { store.skip(-10_000) }) { Image(systemName: "gobackward.10") }
      Button(action: { store.toggle() }) {
        Image(systemName: store.playing ? "pause.fill" : "play.fill").frame(width: 18)
      }
      Button(action: { store.skip(10_000) }) { Image(systemName: "goforward.10") }

      Text(Self.clock(shown)).monospacedDigit().font(.caption).frame(minWidth: 40, alignment: .trailing)
      Slider(
        value: Binding(
          get: { Double(shown) },
          set: { store.scrub(to: Int64($0)) }),
        in: 0...Double(duration),
        onEditingChanged: { editing in if !editing { store.endScrub() } })
        // PR 13: the keyframe grid, the kept range and the markers, drawn over
        // the track (not hit-testable, so the thumb still drags).
        .overlay { TrimMarks().allowsHitTesting(false) }
      Text(Self.clock(store.durationMs)).monospacedDigit().font(.caption)
        .frame(minWidth: 40, alignment: .leading)

      if store.trimArmed {
        Text(store.trimPreviewing ? store.trimLabel + " · previewing" : store.trimLabel)
          .font(.caption).monospacedDigit()
          .foregroundStyle(Color.orange)
          .lineLimit(1)
        // The keyed commands for the mouse: Return / ⇧Return in trim mode.
        Button("Save") { store.run(TrimCommand.keyframe) }
          .help("Keyframe trim: instant, no re-encode (Return)")
        Button("Save exact") { store.run(TrimCommand.reencode) }
          .help("Frame-accurate re-encode on the hardware encoder; slower (⇧Return)")
      }

      Menu {
        ForEach(VideoStore.speeds, id: \.self) { rate in
          Button(action: { store.setSpeed(x100: rate) }) {
            Text(rate == 100 ? "1x" : String(format: "%.2gx", Double(rate) / 100))
          }
        }
      } label: {
        Text(String(format: "%.2gx", Double(store.rateX100) / 100)).font(.caption).monospacedDigit()
      }
      .menuStyle(.borderlessButton).fixedSize()

      Button(action: { store.toggleMute() }) {
        Image(systemName: store.muted ? "speaker.slash.fill" : "speaker.wave.2.fill").frame(width: 18)
      }

      // Windows parity (IslandHost.Video.cs "More" panel): frame step and volume
      // are not one-tap-frequent enough to earn main-bar space, same call Windows made.
      Menu {
        Button(action: { store.step(-1) }) { Text("Previous frame") }
        Button(action: { store.step(1) }) { Text("Next frame") }
        Divider()
        VolumeSliderRow()
      } label: {
        Image(systemName: "ellipsis.circle").frame(width: 18)
      }
      .menuStyle(.borderlessButton).fixedSize()
    }
    .buttonStyle(.plain)
    .padding(.horizontal, 14)
    .frame(height: 44)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
  }
}
