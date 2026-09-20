// SPDX-License-Identifier: GPL-2.0-or-later
// PR 19: the clip transport strip (plan/05, plan/16 "Video": bottom-centre,
// play/pause, scrubber, time, speed, mute). Chrome only: the clock, the seek
// model and the canvas are native; this posts commands over the bridge.
import SwiftUI

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
      Text(Self.clock(store.durationMs)).monospacedDigit().font(.caption)
        .frame(minWidth: 40, alignment: .leading)

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
    }
    .buttonStyle(.plain)
    .padding(.horizontal, 14)
    .frame(height: 44)
    .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
  }
}
