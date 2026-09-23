// SPDX-License-Identifier: GPL-2.0-or-later
// PR 19: the SwiftUI-side mirror of the clip on screen. The render thread owns
// the clip and publishes its status; this polls it (plain integer reads, the same
// cadence FolderStore uses) and posts commands back through the bridge.
import Combine
import Foundation
import MVChromeBridge

@MainActor
final class VideoStore: ObservableObject {
  static let shared = VideoStore()

  /// The 0.25 ... 4x ladder plan/16 defines; rates are x100 to stay integral.
  static let speeds: [Int32] = [25, 50, 100, 150, 200, 400]

  @Published private(set) var active = false
  @Published private(set) var playing = false
  @Published private(set) var muted = false
  @Published private(set) var positionMs: Int64 = 0
  @Published private(set) var durationMs: Int64 = 0
  @Published private(set) var rateX100: Int32 = 100
  @Published private(set) var volume: Float = 1.0
  /// While the slider is held, show (and apply) this rather than the polled value --
  /// the same "local wins over polled" shape scrubMs uses, so a drag does not jitter
  /// against the render thread's own echo of what it just set.
  @Published var draggingVolume: Float?
  /// While the thumb is held, the strip shows (and seeks to) this, not the clip.
  @Published var scrubMs: Int64?

  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
  }

  private func poll() {
    var pos: Int64 = 0, dur: Int64 = 0, rate: Int32 = 100
    var isPlaying = false, isMuted = false
    var vol: Float = 1.0
    let on = mv_chrome_video_status(&pos, &dur, &isPlaying, &rate, &isMuted, &vol)
    if on != active { active = on }
    guard on else { return }
    if scrubMs == nil, pos != positionMs { positionMs = pos }
    if dur != durationMs { durationMs = dur }
    if isPlaying != playing { playing = isPlaying }
    if isMuted != muted { muted = isMuted }
    if rate != rateX100 { rateX100 = rate }
    if draggingVolume == nil, vol != volume { volume = vol }
  }

  func toggle() { mv_chrome_video_toggle() }
  func skip(_ ms: Int64) { mv_chrome_video_skip(ms) }
  func toggleMute() { mv_chrome_video_toggle_mute() }

  func setVolume(_ v: Float) {
    draggingVolume = v
    volume = v
    mv_chrome_video_set_volume(v)
  }
  func endVolumeDrag() { draggingVolume = nil }

  func step(_ frames: Int32) { mv_chrome_video_step(frames) }

  /// Drag: nearest keyframe, instant. Release: decode forward to the frame.
  func scrub(to ms: Int64) {
    scrubMs = ms
    mv_chrome_video_seek(ms, false)
  }
  func endScrub() {
    guard let ms = scrubMs else { return }
    positionMs = ms
    mv_chrome_video_seek(ms, true)
    scrubMs = nil
  }

  func setSpeed(x100: Int32) {
    guard let target = Self.speeds.firstIndex(of: x100),
      let current = Self.speeds.firstIndex(of: rateX100), target != current
    else { return }
    let delta = target - current
    for _ in 0..<abs(delta) { mv_chrome_video_speed_step(delta > 0 ? 1 : -1) }
  }
}
