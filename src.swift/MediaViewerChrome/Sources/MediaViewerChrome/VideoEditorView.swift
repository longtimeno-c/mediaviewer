// SPDX-License-Identifier: GPL-3.0-or-later
// PR 30 (plan/21, issue #40; owner 2026-09-26): the Video Editor window's
// timeline. The preview above it is the viewer's own canvas, moved into the
// window (main_mac.mm); this is the SwiftUI under it: transport, the cut
// tools, a thumbnail track and a waveform over the edited program, a playhead
// you can drag, piece edges you can drag, a marked range, and Export. The cut
// list lives in the host (shell/video_timeline.h); this polls it and posts
// edits back.
//
// Keyboard-complete (plan/16): Space play · ← → frame (⇧ ten) · J K L shuttle
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
  @Published private(set) var thumbs: [EditorThumb] = []
  @Published private(set) var peaks: [Float] = []

  private let clock = EditorClock.shared
  private var generation: UInt64 = .max
  private var stripCount: Int32 = -1
  private var timer: Timer?

  private init() {}

  /// The window is up: poll the host. Stops again when the window goes, so a
  /// closed editor costs nothing (it used to poll for the rest of the session).
  func start() {
    guard timer == nil else { return }
    generation = .max
    stripCount = -1
    poll()
    timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
  }

  func stop() {
    timer?.invalidate()
    timer = nil
    open = false
  }

  private func poll() {
    var v = mv_editor_view()
    guard mv_chrome_editor_view(&v) else { return }
    let isOpen = v.open != 0
    if isOpen != open { open = isOpen }
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
  func select(_ i: Int) { mv_chrome_editor_select(Int32(i)) }
  func export(exact: Bool) { mv_chrome_editor_export(exact ? 1 : 0) }
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

  var body: some View {
    VStack(spacing: 0) {
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      toolbar
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      if store.ready {
        TimelineArea()
          .padding(.horizontal, 12)
          .padding(.vertical, 10)
      } else {
        VStack {
          Spacer()
          ProgressView("Reading the clip…").font(MVTheme.font(14))
          Spacer()
        }
        .frame(maxWidth: .infinity)
      }
      HStack {
        Text("Space play · ← → frame · J K L shuttle · I O mark · X clear · [ ] trim · ⌘B split · ⌫ delete · ⌘Z undo · ⌘E export")
          .font(MVTheme.font(12))
          .foregroundStyle(MVTheme.body)
        Spacer()
      }
      .padding(.horizontal, 12)
      .padding(.bottom, 8)
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(MVTheme.canvas)
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
    .onKeyPress(characters: .init(charactersIn: "iobezxIOBEZX[]")) { press in
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
      default: return .ignored
      }
      return .handled
    }
  }

  private var toolbar: some View {
    HStack(spacing: 2) {
      Button { store.step(-1) } label: { Image(systemName: "backward.frame") }
        .help("Previous frame (←)").accessibilityLabel("Previous frame")
      EditorPlayButton()
      Button { store.step(1) } label: { Image(systemName: "forward.frame") }
        .help("Next frame (→)").accessibilityLabel("Next frame")
      EditorTimecode()
        .padding(.horizontal, 10)
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
      Spacer(minLength: 8)
      Button("Export") { store.export(exact: false) }
        .disabled(!store.edited)
        .help("Write the edit as a new file, cut on keyframes: instant, no quality loss (⌘E)")
      Button("Export exact") { store.export(exact: true) }
        .disabled(!store.edited)
        .help("Frame-accurate: re-encoded on the hardware encoder, slower (⇧⌘E)")
      divider
      Button("Done") { store.close() }.help("Close the editor (⌘W)")
    }
    .buttonStyle(FlatButtonStyle(compact: true))
  }

  private var divider: some View {
    Rectangle().fill(MVTheme.hairline).frame(width: 1, height: 18).padding(.horizontal, 6)
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
          for (i, p) in store.pieces.enumerated() {
            let len = p.outNs - p.inNs
            let rect = CGRect(x: x(start), y: Self.rulerH + Self.gap, width: max(1, x(start + len) - x(start)) - 2,
                              height: Self.videoH)
            drawPiece(ctx, index: i, piece: p, rect: rect)
            let arect = CGRect(x: rect.minX, y: rect.maxY + Self.gap, width: rect.width, height: Self.audioH)
            drawWave(ctx, piece: p, rect: arect)
            start += len
          }
          drawMarks(ctx, x: x, height: size.height)
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
                 .font(MVTheme.font(10)).foregroundColor(MVTheme.body),
               at: CGPoint(x: xx + 2, y: 1), anchor: .topLeading)
      t += step
    }
  }

  private func drawMarks(_ ctx: GraphicsContext, x: (Int64) -> CGFloat, height: CGFloat) {
    guard let (a, b) = store.markedRange else { return }
    let top = Self.rulerH + Self.gap
    let band = CGRect(x: x(a), y: top, width: max(1, x(b) - x(a)), height: height - top)
    ctx.fill(Path(band), with: .color(Color.accentColor.opacity(0.18)))
    // Brackets at the ends that are set: shape, not only colour, says "marked".
    for (t, isIn) in [(store.markIn, true), (store.markOut, false)] where t >= 0 {
      let xx = x(t)
      let arm: CGFloat = isIn ? 6 : -6
      var bracket = Path()
      bracket.move(to: CGPoint(x: xx + arm, y: top))
      bracket.addLine(to: CGPoint(x: xx, y: top))
      bracket.addLine(to: CGPoint(x: xx, y: height - 2))
      bracket.addLine(to: CGPoint(x: xx + arm, y: height - 2))
      ctx.stroke(bracket, with: .color(.accentColor), lineWidth: 2)
    }
  }

  private func drawPiece(_ ctx: GraphicsContext, index: Int, piece: (inNs: Int64, outNs: Int64), rect: CGRect) {
    let shape = RoundedRectangle(cornerRadius: 5).path(in: rect)
    ctx.fill(shape, with: .color(Color.primary.opacity(0.08)))
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
    let selected = index == store.selected
    ctx.stroke(shape, with: .color(selected ? Color.accentColor : MVTheme.hairline), lineWidth: selected ? 3 : 1)
  }

  private func drawWave(_ ctx: GraphicsContext, piece: (inNs: Int64, outNs: Int64), rect: CGRect) {
    let shape = RoundedRectangle(cornerRadius: 4).path(in: rect)
    ctx.fill(shape, with: .color(Color.primary.opacity(0.05)))
    guard !store.peaks.isEmpty, store.sourceNs > 0 else {
      ctx.draw(Text("No audio").font(MVTheme.font(11)).foregroundColor(MVTheme.body),
               at: CGPoint(x: rect.minX + 8, y: rect.midY), anchor: .leading)
      return
    }
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
    ctx.stroke(wave, with: .color(Color.accentColor.opacity(0.75)), lineWidth: 1.2)
  }
}

/// The playhead: the one layer that follows the clock.
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
      ctx.stroke(line, with: .color(.red), lineWidth: 2)
      var head = Path()
      head.move(to: CGPoint(x: px - 6, y: 0))
      head.addLine(to: CGPoint(x: px + 6, y: 0))
      head.addLine(to: CGPoint(x: px, y: 8))
      head.closeSubpath()
      ctx.fill(head, with: .color(.red))
    }
    .allowsHitTesting(false)
  }
}
