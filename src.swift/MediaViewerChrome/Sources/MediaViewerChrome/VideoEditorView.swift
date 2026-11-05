// SPDX-License-Identifier: GPL-3.0-or-later
// PR 30 (docs/design/21, issue #40; owner 2026-09-26): the Video Editor window's
// timeline. The preview above it is the viewer's own canvas, moved into the
// window (main_mac.mm); this is the SwiftUI under it: transport, the cut
// tools, a thumbnail track and a waveform over the edited program, a playhead
// you can drag, piece edges you can drag, a marked range, and Export. The cut
// list lives in the host (shell/video_timeline.h); this polls it and posts
// edits back.
//
// Keyboard-complete (docs/design/16): Space play · ← → frame (⇧ ten) · J K L shuttle
// (J skims back, L plays faster on each press) · I O mark in / out · X clear
// the marks · [ ] trim start / end · ⌘B split · ⌫ delete the marked range or
// the selected piece · ⌘Z / ⇧⌘Z undo / redo · ⌘E export · ⌘W close.
//
// Two observed objects so the timeline is not redrawn on every tick: the
// store is the edit (pieces, strip, marks), which changes when you edit; the
// clock is the playhead, which moves 30 times a second while playing and
// redraws only the playhead and the timecode.

import AppKit
import Combine
import MVChromeBridge
import SwiftUI

struct EditorThumb: Identifiable {
  let id: Int
  let image: CGImage
  let sourceNs: Int64
}

@MainActor
final class EditorClock: ObservableObject {
  static let shared = EditorClock()
  @Published fileprivate(set) var playheadNs: Int64 = 0
  @Published fileprivate(set) var playing = false
  /// While the playhead is dragged it shows the pointer, not the player.
  @Published var scrubNs: Int64?
  var shown: Int64 { scrubNs ?? playheadNs }
}

@MainActor
final class VideoEditorStore: ObservableObject {
  static let shared = VideoEditorStore()

  @Published private(set) var open = false
  @Published private(set) var ready = false
  @Published private(set) var name = ""
  @Published private(set) var lengthNs: Int64 = 0
  @Published private(set) var sourceNs: Int64 = 0
  @Published private(set) var pieces: [(inNs: Int64, outNs: Int64)] = []
  @Published private(set) var selected = -1
  @Published private(set) var canUndo = false
  @Published private(set) var canRedo = false
  @Published private(set) var edited = false
  @Published private(set) var markIn: Int64 = -1
  @Published private(set) var markOut: Int64 = -1
  @Published private(set) var fps: Double = 0
  // The look (docs/plans/video-editor-look.md): turn, mirror, crop.
  @Published private(set) var lookTurns = 0
  @Published private(set) var lookMirror = 0
  @Published private(set) var lookCrop = 0
  @Published private(set) var lookCopyable = true
  /// Light and colour: exposure (EV), contrast, saturation, temperature, tint.
  @Published private(set) var light: [Float] = [0, 0, 0, 0, 0]
  /// Sound: mute, volume (0..2), fades in seconds.
  @Published private(set) var muted = false
  @Published private(set) var volume: Float = 1
  @Published private(set) var fadeIn: Float = 0
  @Published private(set) var fadeOut: Float = 0
  /// The edit's speed: 0.5, 1 or 2.
  @Published private(set) var speed: Float = 1
  @Published private(set) var thumbs: [EditorThumb] = []
  @Published private(set) var peaks: [Float] = []

  private let clock = EditorClock.shared
  private var generation: UInt64 = .max
  private var stripCount: Int32 = -1
  private var started = false
  private var timer: Timer?

  private init() {
    ChromePulse.shared.add { [weak self] in
      self?.pulse()
      return false
    }
  }

  /// The window is up: poll the host. Stops again when the window goes, so a
  /// closed editor costs nothing (it used to poll for the rest of the session).
  /// Issue #178: polled when the main run loop has run (ChromePulse); the
  /// 30 Hz playhead timer runs only while the clip plays, not while paused.
  func start() {
    guard !started else { return }
    started = true
    generation = .max
    stripCount = -1
    poll()
  }

  func stop() {
    started = false
    follow(false)
    open = false
  }

  private func pulse() {
    if started { poll() }
  }

  private func follow(_ playing: Bool) {
    if playing, timer == nil {
      timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
        MainActor.assumeIsolated { self?.poll() }
      }
    } else if !playing, let t = timer {
      t.invalidate()
      timer = nil
    }
  }

  private func poll() {
    var v = mv_editor_view()
    guard mv_chrome_editor_view(&v) else { return }
    let isOpen = v.open != 0
    if isOpen != open { open = isOpen }
    follow(isOpen && v.playing != 0)
    guard isOpen else { return }
    if clock.scrubNs == nil, v.playhead_ns != clock.playheadNs { clock.playheadNs = v.playhead_ns }
    if (v.playing != 0) != clock.playing { clock.playing = v.playing != 0 }
    let g = mv_chrome_editor_generation()
    guard g != generation else { return }
    generation = g
    ready = v.ready != 0
    lengthNs = v.length_ns
    sourceNs = v.source_ns
    selected = Int(v.selected)
    canUndo = v.can_undo != 0
    canRedo = v.can_redo != 0
    edited = v.edited != 0
    if markIn != v.mark_in_ns { markIn = v.mark_in_ns }
    if markOut != v.mark_out_ns { markOut = v.mark_out_ns }
    if fps != v.frame_rate { fps = v.frame_rate }
    if lookTurns != Int(v.look_turns) { lookTurns = Int(v.look_turns) }
    if lookMirror != Int(v.look_mirror) { lookMirror = Int(v.look_mirror) }
    if lookCrop != Int(v.look_crop) { lookCrop = Int(v.look_crop) }
    if lookCopyable != (v.look_copyable != 0) { lookCopyable = v.look_copyable != 0 }
    let l = withUnsafeBytes(of: v.look_light) { Array($0.bindMemory(to: Float.self)) }
    if light != l { light = l }
    if muted != (v.look_mute != 0) { muted = v.look_mute != 0 }
    if volume != v.look_volume { volume = v.look_volume }
    if fadeIn != Float(v.look_fade_in_ms) / 1000 { fadeIn = Float(v.look_fade_in_ms) / 1000 }
    if fadeOut != Float(v.look_fade_out_ms) / 1000 { fadeOut = Float(v.look_fade_out_ms) / 1000 }
    if speed != v.look_speed { speed = v.look_speed }
    var buf = [CChar](repeating: 0, count: 1024)
    _ = mv_chrome_editor_name(&buf, Int32(buf.count))
    let newName = String(cString: buf)
    if newName != name { name = newName }
    let n = Int(mv_chrome_editor_pieces(nil, 0))
    var flat = [Int64](repeating: 0, count: max(n, 1) * 2)
    _ = flat.withUnsafeMutableBufferPointer { mv_chrome_editor_pieces($0.baseAddress, Int32(n)) }
    pieces = (0..<n).map { (flat[2 * $0], flat[2 * $0 + 1]) }
    if v.strip_count != stripCount {
      stripCount = v.strip_count
      loadStrip(count: Int(v.strip_count))
      let pc = Int(v.peak_count)
      var p = [Float](repeating: 0, count: pc)
      _ = p.withUnsafeMutableBufferPointer { mv_chrome_editor_peaks($0.baseAddress, Int32(pc)) }
      peaks = p
    }
  }

  private func loadStrip(count: Int) {
    var out: [EditorThumb] = []
    for i in 0..<count {
      var w: Int32 = 0, h: Int32 = 0, t: Int64 = 0
      _ = mv_chrome_editor_thumb(Int32(i), nil, 0, &w, &h, &t)
      guard w > 0, h > 0 else { continue }
      var px = [UInt8](repeating: 0, count: Int(w) * Int(h) * 4)
      let got = px.withUnsafeMutableBufferPointer {
        mv_chrome_editor_thumb(Int32(i), $0.baseAddress, Int32($0.count), &w, &h, &t)
      }
      guard got > 0, let image = Self.image(px, Int(w), Int(h)) else { continue }
      out.append(EditorThumb(id: i, image: image, sourceNs: t))
    }
    thumbs = out
  }

  private static func image(_ px: [UInt8], _ w: Int, _ h: Int) -> CGImage? {
    guard let provider = CGDataProvider(data: Data(px) as CFData) else { return nil }
    return CGImage(
      width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4,
      space: CGColorSpace(name: CGColorSpace.sRGB)!,
      bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
      provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
  }

  /// Where piece `i` starts on the program.
  func pieceStart(_ i: Int) -> Int64 {
    pieces.prefix(i).reduce(0) { $0 + ($1.outNs - $1.inNs) }
  }

  var hasMarks: Bool { markIn >= 0 || markOut >= 0 }
  /// The marked range on the program, open ends filled in.
  var markedRange: (Int64, Int64)? {
    hasMarks ? (max(0, markIn), markOut >= 0 ? markOut : lengthNs) : nil
  }

  func seek(_ t: Int64) { mv_chrome_editor_seek(max(0, min(t, lengthNs))) }
  func togglePlay() { mv_chrome_editor_toggle_play() }
  func step(_ frames: Int32) { mv_chrome_editor_step(frames) }
  func split() { mv_chrome_editor_edit(1) }
  func delete() { mv_chrome_editor_edit(2) }
  func setIn() { mv_chrome_editor_edit(3) }
  func setOut() { mv_chrome_editor_edit(4) }
  func undo() { mv_chrome_editor_edit(5) }
  func redo() { mv_chrome_editor_edit(6) }
  func setMarkIn() { mv_chrome_editor_edit(14) }
  func setMarkOut() { mv_chrome_editor_edit(15) }
  func clearMarks() { mv_chrome_editor_edit(16) }
  func turnLeft() { mv_chrome_editor_edit(17) }
  func turnRight() { mv_chrome_editor_edit(18) }
  func mirrorLeftRight() { mv_chrome_editor_edit(19) }
  func mirrorUpsideDown() { mv_chrome_editor_edit(20) }
  /// 0 original, 1 16:9, 2 9:16, 3 1:1, 4 4:3, 5 4:5.
  func crop(_ ratio: Int) { mv_chrome_editor_edit(Int32(21 + ratio)) }
  func saveFrame() { mv_chrome_editor_edit(27) }
  func autoLight() { mv_chrome_editor_edit(28) }
  func toggleMute() { mv_chrome_editor_edit(30) }
  /// 0.5, 1 or 2.
  func setSpeed(_ s: Float) { mv_chrome_editor_edit(s < 0.75 ? 31 : s < 1.5 ? 32 : 33) }
  static func speedName(_ s: Float) -> String { s < 0.75 ? "0.5×" : s < 1.5 ? "1×" : "2×" }
  /// param 0 volume (0..2), 1 fade in, 2 fade out (seconds); phase as setLight.
  func setSound(_ param: Int, _ value: Float, phase: Int32) {
    switch param {
    case 0: volume = value
    case 1: fadeIn = value
    default: fadeOut = value
    }
    mv_chrome_editor_set_sound(Int32(param), value, phase)
  }
  func resetLight() { mv_chrome_editor_edit(29) }
  /// phase 0 starts a drag (one undo step), 1 moves it, 2 ends it.
  func setLight(_ param: Int, _ value: Float, phase: Int32) {
    if param < light.count { light[param] = value }
    mv_chrome_editor_set_light(Int32(param), value, phase)
  }
  /// The five sliders: PR 11's names, ranges and steps (edit/adjust.cpp range_of).
  static let lightSpecs: [(name: String, min: Float, max: Float, step: Float)] = [
    ("Exposure", -5, 5, 0.1), ("Contrast", -100, 100, 5), ("Saturation", -100, 100, 5),
    ("Temperature", -100, 100, 5), ("Tint", -100, 100, 5),
  ]
  static let cropNames = ["Original", "16:9", "9:16", "1:1", "4:3", "4:5"]
  func export(exact: Bool) { mv_chrome_editor_export(exact ? 1 : 0) }
  /// An add-on's button in the bar (editor_items: Enhance…), on the edit: the
  /// clip on screen with the kept pieces, or the whole clip when nothing is cut.
  func runAddon(_ item: SlotItem) {
    guard let path = SlotTargets.path(at: FolderStore.shared.currentIndex) else { return }
    SlotStore.shared.invokeEditor(item, path: path, ranges: edited ? pieces : [])
  }
  func close() { mv_chrome_editor_close() }
  func shuttle(_ key: Int32, _ phase: Int32) { mv_chrome_editor_shuttle(key, phase) }
  func trimBegin(_ index: Int, inEdge: Bool) -> Bool { mv_chrome_editor_trim_begin(Int32(index), inEdge ? 0 : 1) }
  func trimTo(_ sourceNs: Int64) { _ = mv_chrome_editor_trim_to(sourceNs) }
  func trimEnd() { mv_chrome_editor_trim_end() }
}

/// "MM:SS:FF" (or "H:MM:SS:FF"), the frame within the second at the clip's
/// rate; "M:SS.cc" hundredths when the rate is unknown.
func editorTimecode(_ ns: Int64, fps: Double = 0) -> String {
  let t = max(0, ns)
  guard fps > 0 else {
    let cs = t / 10_000_000
    let s = cs / 100
    return s >= 3600
      ? String(format: "%d:%02d:%02d.%02d", s / 3600, (s / 60) % 60, s % 60, cs % 100)
      : String(format: "%d:%02d.%02d", s / 60, s % 60, cs % 100)
  }
  let s = t / 1_000_000_000
  let perSecond = max(1, Int(fps.rounded(.up)))
  let frame = min(perSecond - 1, Int(Double(t % 1_000_000_000) / 1e9 * fps + 1e-6))
  return s >= 3600
    ? String(format: "%d:%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60, frame)
    : String(format: "%02d:%02d:%02d", s / 60, s % 60, frame)
}

/// The ruler's labels: whole seconds.
func editorRulerLabel(_ ns: Int64) -> String {
  let s = max(0, ns) / 1_000_000_000
  return s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60)
                   : String(format: "%d:%02d", s / 60, s % 60)
}

struct VideoEditorView: View {
  @ObservedObject private var store = VideoEditorStore.shared
  @FocusState private var focused: Bool
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    VStack(spacing: 0) {
      // The one hairline: where the preview meets the timeline. The bar and
      // the tracks share a surface, so spacing (not a rule) parts them.
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      // The bar pulls back by its buttons' inset, so the first icon's ink
      // lines up with the tracks and the key line (12 from the edge).
      toolbar
        .padding(.horizontal, MVTheme.space(12) - MVTheme.space(8))
        .padding(.vertical, MVTheme.space(6))
      // Reading the clip (its strip and envelope) can take seconds: a quiet
      // skeleton at the timeline's own geometry, not a spinner, so nothing
      // moves when the tracks cross-fade in.
      ZStack(alignment: .topLeading) {
        if store.ready {
          TimelineArea().transition(fade)
        } else {
          TimelineSkeleton().transition(fade)
        }
      }
      .padding(.horizontal, MVTheme.space(12))
      .padding(.vertical, MVTheme.space(10))
      HStack {
        Text("Space play · ← → frame · J K L shuttle · I O mark · X clear · [ ] trim · ⌘B split · ⌫ delete · ⌘Z undo · ⇧⌘S save frame · ⌘E export")
          .font(MVTheme.font(12))
          .foregroundStyle(MVTheme.body)
        Spacer()
      }
      .padding(.horizontal, MVTheme.space(12))
      .padding(.bottom, MVTheme.space(10))
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(ThemeField())
    .focusable()
    .focused($focused)
    .focusEffectDisabled()
    .onAppear {
      store.start()
      focused = true
    }
    .onDisappear { store.stop() }
    .onKeyPress(.space) { store.togglePlay(); return .handled }
    .onKeyPress(keys: [.leftArrow, .rightArrow]) { press in
      let n: Int32 = press.modifiers.contains(.shift) ? 10 : 1
      store.step(press.key == .leftArrow ? -n : n)
      return .handled
    }
    .onKeyPress(keys: [.delete, .deleteForward]) { _ in store.delete(); return .handled }
    // J K L need the key's release (J's skim settles on the exact frame).
    .onKeyPress(characters: .init(charactersIn: "jklJKL"), phases: [.down, .repeat, .up]) { press in
      guard !press.modifiers.contains(.command) else { return .ignored }
      let key: Int32 = switch press.characters.lowercased() { case "j": 0; case "k": 1; default: 2 }
      let phase: Int32 = switch press.phase { case .down: 0; case .repeat: 1; default: 2 }
      store.shuttle(key, phase)
      return .handled
    }
    .onKeyPress(characters: .init(charactersIn: "iobezxsIOBEZXS[]")) { press in
      let cmd = press.modifiers.contains(.command)
      switch press.characters.lowercased() {
      case "i" where !cmd: store.setMarkIn()
      case "o" where !cmd: store.setMarkOut()
      case "x" where !cmd: store.clearMarks()
      case "[" where !cmd: store.setIn()
      case "]" where !cmd: store.setOut()
      case "b" where cmd: store.split()
      case "e" where cmd: store.export(exact: press.modifiers.contains(.shift))
      case "z" where cmd: press.modifiers.contains(.shift) ? store.redo() : store.undo()
      case "s" where cmd && press.modifiers.contains(.shift): store.saveFrame()
      default: return .ignored
      }
      return .handled
    }
  }

  private var fade: AnyTransition {
    reduceMotion ? .identity : .opacity.animation(.easeOut(duration: 0.15))
  }

  private var toolbar: some View {
    HStack(spacing: MVTheme.space(2)) {
      Button { store.step(-1) } label: { Image(systemName: "backward.frame.fill") }
        .help("Previous frame (←)").accessibilityLabel("Previous frame")
      EditorPlayButton()
      Button { store.step(1) } label: { Image(systemName: "forward.frame.fill") }
        .help("Next frame (→)").accessibilityLabel("Next frame")
      EditorTimecode()
        .padding(.horizontal, MVTheme.space(10))
      divider
      Button("Split") { store.split() }.help("Split at the playhead (⌘B)")
      Button("Delete") { store.delete() }
        .help(store.hasMarks ? "Delete the marked range (⌫)" : "Delete the selected piece (⌫)")
        .disabled(!store.hasMarks && store.pieces.count < 2)
      divider
      Button("Mark in") { store.setMarkIn() }.help("Mark the start of a range to delete (I)")
      Button("Mark out") { store.setMarkOut() }.help("Mark the end of a range to delete (O)")
      divider
      Button("Trim start") { store.setIn() }.help("Cut everything before the playhead ([)")
      Button("Trim end") { store.setOut() }.help("Cut everything after the playhead (])")
      divider
      Button("Undo") { store.undo() }.disabled(!store.canUndo).help("Undo (⌘Z)")
      Button("Redo") { store.redo() }.disabled(!store.canRedo).help("Redo (⇧⌘Z)")
      divider
      Button { store.turnLeft() } label: { Image(systemName: "rotate.left.fill") }
        .help("Turn left").accessibilityLabel("Turn left")
      Button { store.turnRight() } label: { Image(systemName: "rotate.right.fill") }
        .help("Turn right").accessibilityLabel("Turn right")
      Menu {
        Button("Mirror left to right") { store.mirrorLeftRight() }
        Button("Mirror upside down") { store.mirrorUpsideDown() }
        Divider()
        ForEach(0..<VideoEditorStore.cropNames.count, id: \.self) { i in
          Button {
            store.crop(i)
          } label: {
            if store.lookCrop == i {
              Label(VideoEditorStore.cropNames[i], systemImage: "checkmark")
            } else {
              Text(VideoEditorStore.cropNames[i])
            }
          }
        }
      } label: {
        Image(systemName: "crop")
      }
      .menuStyle(.borderlessButton)
      .fixedSize()
      .help("Mirror and crop the whole clip")
      .accessibilityLabel("Mirror and crop")
      EditorLightButton(store: store)
      EditorSoundButton(store: store)
      Menu {
        ForEach([Float(0.5), 1, 2], id: \.self) { s in
          Button {
            store.setSpeed(s)
          } label: {
            if store.speed == s {
              Label(VideoEditorStore.speedName(s), systemImage: "checkmark")
            } else {
              Text(VideoEditorStore.speedName(s))
            }
          }
        }
      } label: {
        Text(VideoEditorStore.speedName(store.speed))
      }
      .menuStyle(.borderlessButton)
      .fixedSize()
      .help("Speed of the whole clip: playback and Export exact")
      .accessibilityLabel("Speed")
      Button { store.saveFrame() } label: { Image(systemName: "camera.fill") }
        .help("Save this frame as a photo (⇧⌘S)").accessibilityLabel("Save frame")
      Spacer(minLength: MVTheme.space(8))
      EditorAddonItems(store: store)
      Button("Export") { store.export(exact: false) }
        .disabled(!store.edited || !store.lookCopyable)
        .help(store.lookCopyable
          ? "Write the edit as a new file, cut on keyframes: instant, no quality loss (⌘E)"
          : "Crop, mirror, colour, sound and speed need Export exact")
      Button("Export exact") { store.export(exact: true) }
        .disabled(!store.edited)
        .help("Frame-accurate: re-encoded on the hardware encoder, slower (⇧⌘E)")
      divider
      Button("Done") { store.close() }.help("Close the editor (⌘W)")
    }
    .buttonStyle(FlatButtonStyle(compact: true))
  }

  private var divider: some View {
    Rectangle().fill(MVTheme.hairline).frame(width: 1, height: 18).padding(.horizontal, MVTheme.space(6))
  }
}

/// Add-ons' buttons in the bar (editor_items, contribution API 11): Enhance….
/// Each runs on the edit, never the clip itself; none shows without one.
struct EditorAddonItems: View {
  @ObservedObject var store: VideoEditorStore
  @ObservedObject private var slots = SlotStore.shared

  var body: some View {
    let items = slots.items(in: "editor")
    if !items.isEmpty {
      ForEach(items) { item in
        Button(item.label) { store.runAddon(item) }
          .disabled(!store.ready)
          // "The original is not changed" is the result notice's to say, once.
          .help(store.edited ? "\(item.label.replacingOccurrences(of: "…", with: "")) the edit as a new clip"
                             : "\(item.label.replacingOccurrences(of: "…", with: "")) the whole clip as a new clip")
      }
      Rectangle().fill(MVTheme.hairline).frame(width: 1, height: 18).padding(.horizontal, MVTheme.space(6))
    }
  }
}

/// Play / pause: follows the clock, so it is the only toolbar view that moves
/// while playing.
private struct EditorPlayButton: View {
  @ObservedObject private var clock = EditorClock.shared

  var body: some View {
    Button { VideoEditorStore.shared.togglePlay() } label: {
      Image(systemName: clock.playing ? "pause.fill" : "play.fill")
    }
    .help("Play / pause (Space)").accessibilityLabel(clock.playing ? "Pause" : "Play")
  }
}

private struct EditorTimecode: View {
  @ObservedObject private var clock = EditorClock.shared
  @ObservedObject private var store = VideoEditorStore.shared

  var body: some View {
    Text("\(editorTimecode(clock.shown, fps: store.fps)) / \(editorTimecode(store.lengthNs, fps: store.fps))")
      .font(MVTheme.font(14))
      .foregroundStyle(MVTheme.title)
      .monospacedDigit()
      .accessibilityLabel(
        "Playhead \(editorTimecode(clock.shown, fps: store.fps)) of \(editorTimecode(store.lengthNs, fps: store.fps))")
  }
}

/// The ruler, the video track (thumbnails per piece), the audio track
/// (waveform per piece), the marked range and the playhead, over the whole
/// program. Everything but the playhead is one canvas that redraws when the
/// edit changes; the playhead is its own layer that follows the clock.
private struct TimelineArea: View {
  @ObservedObject private var store = VideoEditorStore.shared
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var drag: Drag?
  @State private var overEdge = false

  private enum Drag {
    case scrub
    // A piece's edge: where it was in source time, where the pointer went
    // down, and the scale then (the program re-fits as the piece changes).
    case trim(originNs: Int64, originX: CGFloat, nsPerPoint: Double)
  }

  static let rulerH: CGFloat = 18
  static let videoH: CGFloat = 76
  static let audioH: CGFloat = 52
  static let gap: CGFloat = 6
  static var height: CGFloat { rulerH + gap + videoH + gap + audioH + 4 }
  private let edgeSlop: CGFloat = 6
  private let snapPoints: CGFloat = 8

  var body: some View {
    GeometryReader { geo in
      let width = max(1, geo.size.width)
      let length = max(Int64(1), store.lengthNs)
      let x = { (t: Int64) -> CGFloat in CGFloat(Double(t) / Double(length)) * width }
      ZStack(alignment: .topLeading) {
        Canvas { ctx, size in
          drawRuler(ctx, width: size.width, length: length)
          var start: Int64 = 0
          var quiet: CGRect?  // where "No audio" is said, once
          for p in store.pieces {
            let len = p.outNs - p.inNs
            let rect = CGRect(x: x(start), y: Self.rulerH + Self.gap, width: max(1, x(start + len) - x(start)) - 2,
                              height: Self.videoH)
            drawPiece(ctx, piece: p, rect: rect)
            let arect = CGRect(x: rect.minX, y: rect.maxY + Self.gap, width: rect.width, height: Self.audioH)
            drawWave(ctx, piece: p, rect: arect)
            if quiet == nil, arect.width >= 88 { quiet = arect }
            start += len
          }
          if let quiet, store.peaks.isEmpty || store.sourceNs <= 0 {
            ctx.draw(Text("No audio").font(MVTheme.font(11)).foregroundColor(MVTheme.body),
                     at: CGPoint(x: quiet.minX + MVTheme.space(12), y: quiet.midY), anchor: .leading)
          }
        }
        // Lift, don't ring: with a piece chosen (Delete's target), the others
        // step back under a wash of the field, fading as the choice moves.
        let spans = pieceSpans(x)
        ForEach(spans, id: \.index) { span in
          let x0 = span.x
          let w = span.width
          let back = spans.count > 1 && store.selected >= 0 && span.index != store.selected
          VStack(spacing: Self.gap) {
            RoundedRectangle(cornerRadius: MVTheme.radius(5)).frame(height: Self.videoH)
            RoundedRectangle(cornerRadius: MVTheme.radius(4)).frame(height: Self.audioH)
          }
          .foregroundStyle(MVTheme.canvas.opacity(0.45))
          .opacity(back ? 1 : 0)
          .animation(reduceMotion ? nil : .easeOut(duration: 0.15), value: store.selected)
          .frame(width: w)
          .offset(x: x0, y: Self.rulerH + Self.gap)
          .allowsHitTesting(false)
        }
        // Marks fade in and out (the transition carries its own animation, so
        // an edit that clears them does not also slide the rest).
        if let range = store.markedRange {
          EditorMarks(range: range, markIn: store.markIn, markOut: store.markOut, width: width, length: length)
            .transition(reduceMotion ? .identity : .opacity.animation(.easeOut(duration: 0.15)))
        }
        EditorPlayhead(width: width, length: length)
      }
      .contentShape(Rectangle())
      .onContinuousHover { phase in
        switch phase {
        case .active(let at):
          let near = at.y >= Self.rulerH && edge(at: at.x, width: width, length: length) != nil
          if near != overEdge {
            overEdge = near
            (near ? NSCursor.resizeLeftRight : NSCursor.arrow).set()
          }
        case .ended:
          if overEdge { NSCursor.arrow.set() }
          overEdge = false
        }
      }
      .gesture(
        DragGesture(minimumDistance: 0)
          .onChanged { g in
            if drag == nil { drag = begin(at: g.startLocation, width: width, length: length) }
            switch drag {
            case .trim(let origin, let originX, let nsPerPoint):
              store.trimTo(origin + Int64(Double(g.location.x - originX) * nsPerPoint))
            default:
              let t = snapped(g.location.x, width: width, length: length)
              EditorClock.shared.scrubNs = t
              store.seek(t)
            }
          }
          .onEnded { g in
            switch drag {
            case .trim:
              store.trimEnd()
            default:
              store.seek(snapped(g.location.x, width: width, length: length))
              EditorClock.shared.scrubNs = nil
            }
            drag = nil
          })
      .accessibilityElement(children: .ignore)
      .accessibilityLabel("Timeline, \(store.pieces.count) pieces")
      .accessibilityValue(accessibilityValue)
      .accessibilityAdjustableAction { dir in
        store.seek(EditorClock.shared.playheadNs + (dir == .increment ? 1_000_000_000 : -1_000_000_000))
      }
    }
    .frame(height: Self.height)
  }

  private struct PieceSpan { let index: Int; let x: CGFloat; let width: CGFloat }

  /// Where each piece sits across the track, as the canvas draws it.
  private func pieceSpans(_ x: (Int64) -> CGFloat) -> [PieceSpan] {
    var start: Int64 = 0
    return store.pieces.enumerated().map { i, p in
      let len = p.outNs - p.inNs
      defer { start += len }
      return PieceSpan(index: i, x: x(start), width: max(1, x(start + len) - x(start)) - 2)
    }
  }

  private var accessibilityValue: String {
    var v = "Playhead \(editorTimecode(EditorClock.shared.shown, fps: store.fps)) of \(editorTimecode(store.lengthNs, fps: store.fps))"
    if let (a, b) = store.markedRange {
      v += ", marked \(editorTimecode(a, fps: store.fps)) to \(editorTimecode(b, fps: store.fps))"
    }
    return v
  }

  /// A piece edge under `x`: left of a join is the outgoing piece's out, right
  /// of it the incoming piece's in (as in Final Cut).
  private func edge(at px: CGFloat, width: CGFloat, length: Int64) -> (index: Int, inEdge: Bool, sourceNs: Int64)? {
    var start: Int64 = 0
    for (i, p) in store.pieces.enumerated() {
      let len = p.outNs - p.inNs
      let x0 = CGFloat(Double(start) / Double(length)) * width
      let x1 = CGFloat(Double(start + len) / Double(length)) * width
      if px >= x0 - 1, px - x0 <= edgeSlop { return (i, true, p.inNs) }
      if px <= x1 + 1, x1 - px <= edgeSlop { return (i, false, p.outNs) }
      start += len
    }
    return nil
  }

  private func begin(at p: CGPoint, width: CGFloat, length: Int64) -> Drag {
    if p.y >= Self.rulerH, let e = edge(at: p.x, width: width, length: length),
       store.trimBegin(e.index, inEdge: e.inEdge) {
      return .trim(originNs: e.sourceNs, originX: p.x, nsPerPoint: Double(length) / Double(width))
    }
    return .scrub
  }

  /// The playhead snaps to joins and marks within a few points.
  private func snapped(_ px: CGFloat, width: CGFloat, length: Int64) -> Int64 {
    let t = Int64(Double(max(0, min(px, width)) / width) * Double(length))
    var targets: [Int64] = (0...store.pieces.count).map { store.pieceStart($0) }
    if store.markIn >= 0 { targets.append(store.markIn) }
    if store.markOut >= 0 { targets.append(store.markOut) }
    let slop = Int64(Double(snapPoints) / Double(width) * Double(length))
    return targets.min { abs($0 - t) < abs($1 - t) }.flatMap { abs($0 - t) <= slop ? $0 : nil } ?? t
  }

  private func drawRuler(_ ctx: GraphicsContext, width: CGFloat, length: Int64) {
    // A tick every 1, 5, 10, 30 or 60 s, whichever keeps them 60 pt or more apart.
    let seconds = Double(length) / 1e9
    let steps: [Double] = [1, 2, 5, 10, 15, 30, 60, 120, 300, 600]
    let step = steps.first { seconds / $0 * 60 <= Double(width) } ?? 600
    var t = 0.0
    while t <= seconds {
      let xx = CGFloat(t / seconds) * width
      var tick = Path()
      tick.move(to: CGPoint(x: xx, y: Self.rulerH - 6))
      tick.addLine(to: CGPoint(x: xx, y: Self.rulerH))
      ctx.stroke(tick, with: .color(MVTheme.hairline), lineWidth: 1)
      ctx.draw(Text(editorRulerLabel(Int64(t * 1e9)))
                 .font(MVTheme.font(11)).monospacedDigit().foregroundColor(MVTheme.body),
               at: CGPoint(x: xx + 3, y: 0), anchor: .topLeading)
      t += step
    }
  }

  // One surface per piece: its fill and its frames, no outline (the 2 pt
  // join already parts it from the next). The chosen one is lifted by the
  // wash over the others, not ringed in the accent.
  private func drawPiece(_ ctx: GraphicsContext, piece: (inNs: Int64, outNs: Int64), rect: CGRect) {
    let shape = RoundedRectangle(cornerRadius: MVTheme.radius(5)).path(in: rect)
    ctx.fill(shape, with: .color(MVTheme.surface2))
    var inner = ctx
    inner.clip(to: shape)
    // Thumbnails of this piece, each at its own source time, tiled to fill it.
    let tile = rect.height * 16 / 9
    let len = Double(piece.outNs - piece.inNs)
    var tx = rect.minX
    while tx < rect.maxX {
      let at = piece.inNs + Int64(Double(tx - rect.minX) / Double(rect.width) * len)
      if let thumb = store.thumbs.last(where: { $0.sourceNs <= at }) ?? store.thumbs.first {
        let aspect = CGFloat(thumb.image.width) / CGFloat(max(1, thumb.image.height))
        let w = rect.height * aspect
        inner.draw(Image(decorative: thumb.image, scale: 1), in: CGRect(x: tx, y: rect.minY, width: w, height: rect.height))
        tx += w
      } else {
        tx += tile
      }
    }
  }

  // The envelope in the body ink: the accent is kept for what you marked.
  private func drawWave(_ ctx: GraphicsContext, piece: (inNs: Int64, outNs: Int64), rect: CGRect) {
    let shape = RoundedRectangle(cornerRadius: MVTheme.radius(4)).path(in: rect)
    ctx.fill(shape, with: .color(MVTheme.surface2))
    guard !store.peaks.isEmpty, store.sourceNs > 0 else { return }
    let n = store.peaks.count
    let mid = rect.midY
    var wave = Path()
    let cols = max(1, Int(rect.width / 2))
    for c in 0..<cols {
      let s = piece.inNs + Int64(Double(c) / Double(cols) * Double(piece.outNs - piece.inNs))
      let b = min(n - 1, max(0, Int(Double(s) / Double(store.sourceNs) * Double(n))))
      let h = CGFloat(store.peaks[b]) * (rect.height / 2 - 2)
      let xx = rect.minX + CGFloat(c) * 2
      wave.move(to: CGPoint(x: xx, y: mid - h))
      wave.addLine(to: CGPoint(x: xx, y: mid + h))
    }
    ctx.stroke(wave, with: .color(MVTheme.body), lineWidth: 1.2)
  }
}

/// The marked range: a band over both tracks and a bracket at each end that is
/// set (shape, not only colour, says "marked"). Its own layer, so it fades in
/// and out as a whole; it keeps the range it was given while it fades out.
private struct EditorMarks: View {
  let range: (Int64, Int64)
  let markIn: Int64
  let markOut: Int64
  let width: CGFloat
  let length: Int64

  var body: some View {
    Canvas { ctx, size in
      let x = { (t: Int64) -> CGFloat in CGFloat(Double(t) / Double(max(1, length))) * width }
      let top = TimelineArea.rulerH + TimelineArea.gap
      let band = CGRect(x: x(range.0), y: top, width: max(1, x(range.1) - x(range.0)), height: size.height - top)
      ctx.fill(Path(band), with: .color(MVTheme.accent.opacity(0.18)))
      for (t, isIn) in [(markIn, true), (markOut, false)] where t >= 0 {
        let xx = x(t)
        let arm: CGFloat = isIn ? 6 : -6
        var bracket = Path()
        bracket.move(to: CGPoint(x: xx + arm, y: top))
        bracket.addLine(to: CGPoint(x: xx, y: top))
        bracket.addLine(to: CGPoint(x: xx, y: size.height - 2))
        bracket.addLine(to: CGPoint(x: xx + arm, y: size.height - 2))
        ctx.stroke(bracket, with: .color(MVTheme.accent), lineWidth: 2)
      }
    }
    .allowsHitTesting(false)
  }
}

/// While the clip is read: the timeline's tracks at their own size, quiet and
/// still (nothing pulses), with one line saying what is happening.
private struct TimelineSkeleton: View {
  var body: some View {
    VStack(alignment: .leading, spacing: TimelineArea.gap) {
      Color.clear.frame(height: TimelineArea.rulerH)
      RoundedRectangle(cornerRadius: MVTheme.radius(5))
        .fill(MVTheme.surface2)
        .frame(height: TimelineArea.videoH)
        .overlay(alignment: .leading) {
          Text("Reading the clip…")
            .font(MVTheme.font(12))
            .foregroundStyle(MVTheme.body)
            .padding(.leading, MVTheme.space(12))
        }
      RoundedRectangle(cornerRadius: MVTheme.radius(4))
        .fill(MVTheme.surface2)
        .frame(height: TimelineArea.audioH)
      Spacer(minLength: 0)
    }
    .frame(height: TimelineArea.height)
    .accessibilityElement(children: .combine)
  }
}

/// The playhead: the one layer that follows the clock. The title ink on a
/// thin halo of the field, so it reads over any frame without borrowing a
/// status colour.
private struct EditorPlayhead: View {
  @ObservedObject private var clock = EditorClock.shared
  let width: CGFloat
  let length: Int64

  var body: some View {
    Canvas { ctx, size in
      let px = CGFloat(Double(max(0, min(clock.shown, length))) / Double(max(1, length))) * width
      var line = Path()
      line.move(to: CGPoint(x: px, y: 0))
      line.addLine(to: CGPoint(x: px, y: size.height))
      ctx.stroke(line, with: .color(MVTheme.canvas), lineWidth: 4)
      ctx.stroke(line, with: .color(MVTheme.title), lineWidth: 2)
      var head = Path()
      head.move(to: CGPoint(x: px - 6, y: 0))
      head.addLine(to: CGPoint(x: px + 6, y: 0))
      head.addLine(to: CGPoint(x: px, y: 8))
      head.closeSubpath()
      ctx.fill(head, with: .color(MVTheme.title))
    }
    .allowsHitTesting(false)
  }
}

/// Light and colour for the whole clip (docs/plans/video-editor-look.md
/// slice 2): PR 11's five sliders, Auto (auto tone on the frame at the
/// playhead) and Reset, in a popover. The preview follows as a slider moves;
/// a drag is one undo step.
struct EditorLightButton: View {
  @ObservedObject var store: VideoEditorStore
  @State private var shown = false

  var body: some View {
    Button { shown.toggle() } label: { Image(systemName: "sun.max.fill") }
      .help("Light and colour for the whole clip").accessibilityLabel("Light and colour")
      .popover(isPresented: $shown, arrowEdge: .bottom) {
        VStack(alignment: .leading, spacing: MVTheme.space(8)) {
          ForEach(0..<VideoEditorStore.lightSpecs.count, id: \.self) { i in
            let spec = VideoEditorStore.lightSpecs[i]
            VStack(alignment: .leading, spacing: MVTheme.space(2)) {
              HStack {
                Text(spec.name).font(MVTheme.font(13))
                Spacer()
                // Edit's own words for the same sliders (AdjustStore): "+0.5 EV", "+25".
                Text(i == 0 ? String(format: "%+.1f EV", store.light[i]) : String(format: "%+.0f", store.light[i]))
                  .font(MVTheme.font(12)).monospacedDigit().foregroundStyle(.secondary)
              }
              Slider(value: Binding(get: { store.light[i] },
                                    set: { store.setLight(i, $0, phase: 1) }),
                     in: spec.min...spec.max, step: spec.step) { editing in
                store.setLight(i, store.light[i], phase: editing ? 0 : 2)
              }
              .accessibilityLabel(spec.name)
            }
          }
          HStack {
            Button("Auto") { store.autoLight() }.help("Exposure and contrast from the frame on screen")
            Spacer()
            Button("Reset") { store.resetLight() }.disabled(store.light.allSatisfy { $0 == 0 })
              .help("Every slider back to 0")
          }
          .font(MVTheme.font(13))
        }
        .padding(MVTheme.space(12))
        .frame(width: 280)
      }
  }
}

/// The edit's sound (docs/plans/video-editor-look.md slice 3): mute, volume
/// and fades on the program's start and end. Playback hears them; Export
/// exact writes them (Export on keyframes carries mute only).
struct EditorSoundButton: View {
  @ObservedObject var store: VideoEditorStore
  @State private var shown = false

  private func row(_ name: String, _ readout: String, _ value: Float, _ range: ClosedRange<Float>,
                   _ step: Float, _ param: Int) -> some View {
    VStack(alignment: .leading, spacing: MVTheme.space(2)) {
      HStack {
        Text(name).font(MVTheme.font(13))
        Spacer()
        Text(readout).font(MVTheme.font(12)).monospacedDigit().foregroundStyle(.secondary)
      }
      Slider(value: Binding(get: { value }, set: { store.setSound(param, $0, phase: 1) }),
             in: range, step: step) { editing in
        store.setSound(param, value, phase: editing ? 0 : 2)
      }
      .accessibilityLabel(name)
    }
  }

  var body: some View {
    Button { shown.toggle() } label: {
      Image(systemName: store.muted ? "speaker.slash.fill" : "speaker.wave.2.fill")
    }
    .help("Sound for the whole clip").accessibilityLabel("Sound")
    .popover(isPresented: $shown, arrowEdge: .bottom) {
      VStack(alignment: .leading, spacing: MVTheme.space(8)) {
        Toggle("Mute", isOn: Binding(get: { store.muted }, set: { _ in store.toggleMute() }))
          .font(MVTheme.font(13))
        row("Volume", String(format: "%.0f %%", store.volume * 100), store.volume, 0...2, 0.05, 0)
          .disabled(store.muted)
        row("Fade in", String(format: "%.1f s", store.fadeIn), store.fadeIn, 0...5, 0.1, 1)
          .disabled(store.muted)
        row("Fade out", String(format: "%.1f s", store.fadeOut), store.fadeOut, 0...5, 0.1, 2)
          .disabled(store.muted)
      }
      .padding(MVTheme.space(12))
      .frame(width: 260)
    }
  }
}
