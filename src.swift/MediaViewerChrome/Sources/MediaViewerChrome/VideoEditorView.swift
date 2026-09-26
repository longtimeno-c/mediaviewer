// SPDX-License-Identifier: GPL-2.0-or-later
// PR 30 (plan/21, issue #40; owner 2026-09-26): the Video Editor window's
// timeline. The preview above it is the viewer's own canvas, moved into the
// window (main_mac.mm); this is the SwiftUI under it: transport, the cut
// tools, a thumbnail track and a waveform over the edited program, a playhead
// you can drag, and Export. The cut list lives in the host
// (shell/video_timeline.h); this polls it and posts edits back.
//
// Keyboard-complete (plan/16): Space play · ← → frame (⇧ ten) · J K L back a
// second / pause / on a second · I O set in / out · ⌘B split · ⌫ delete the
// selected piece · ⌘Z / ⇧⌘Z undo / redo · ⌘E export · ⌘W close.
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
final class VideoEditorStore: ObservableObject {
  static let shared = VideoEditorStore()

  @Published private(set) var open = false
  @Published private(set) var ready = false
  @Published private(set) var name = ""
  @Published private(set) var lengthNs: Int64 = 0
  @Published private(set) var sourceNs: Int64 = 0
  @Published private(set) var playheadNs: Int64 = 0
  @Published private(set) var playing = false
  @Published private(set) var pieces: [(inNs: Int64, outNs: Int64)] = []
  @Published private(set) var selected = -1
  @Published private(set) var canUndo = false
  @Published private(set) var canRedo = false
  @Published private(set) var edited = false
  @Published private(set) var thumbs: [EditorThumb] = []
  @Published private(set) var peaks: [Float] = []
  /// While the playhead is dragged it shows the finger, not the player.
  @Published var scrubNs: Int64?

  private var generation: UInt64 = .max
  private var stripCount: Int32 = -1
  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
  }

  private func poll() {
    var v = mv_editor_view()
    guard mv_chrome_editor_view(&v) else { return }
    let isOpen = v.open != 0
    if isOpen != open { open = isOpen }
    guard isOpen else { return }
    if scrubNs == nil, v.playhead_ns != playheadNs { playheadNs = v.playhead_ns }
    if (v.playing != 0) != playing { playing = v.playing != 0 }
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
    var buf = [CChar](repeating: 0, count: 1024)
    _ = mv_chrome_editor_name(&buf, Int32(buf.count))
    name = String(cString: buf)
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

  var shownPlayhead: Int64 { scrubNs ?? playheadNs }

  func seek(_ t: Int64) { mv_chrome_editor_seek(max(0, min(t, lengthNs))) }
  func togglePlay() { mv_chrome_editor_toggle_play() }
  func step(_ frames: Int32) { mv_chrome_editor_step(frames) }
  func split() { mv_chrome_editor_edit(1) }
  func deleteSelected() { mv_chrome_editor_edit(2) }
  func setIn() { mv_chrome_editor_edit(3) }
  func setOut() { mv_chrome_editor_edit(4) }
  func undo() { mv_chrome_editor_edit(5) }
  func redo() { mv_chrome_editor_edit(6) }
  func select(_ i: Int) { mv_chrome_editor_select(Int32(i)) }
  func export(exact: Bool) { mv_chrome_editor_export(exact ? 1 : 0) }
  func close() { mv_chrome_editor_close() }
}

func editorTimecode(_ ns: Int64) -> String {
  let cs = max(0, ns) / 10_000_000
  let s = cs / 100
  return s >= 3600
    ? String(format: "%d:%02d:%02d.%02d", s / 3600, (s / 60) % 60, s % 60, cs % 100)
    : String(format: "%d:%02d.%02d", s / 60, s % 60, cs % 100)
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
        Text("Space play · ← → frame · I O in / out · ⌘B split · ⌫ delete piece · ⌘Z undo · ⌘E export")
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
    .onAppear { focused = true }
    .onKeyPress(.space) { store.togglePlay(); return .handled }
    .onKeyPress(keys: [.leftArrow, .rightArrow]) { press in
      let n: Int32 = press.modifiers.contains(.shift) ? 10 : 1
      store.step(press.key == .leftArrow ? -n : n)
      return .handled
    }
    .onKeyPress(keys: [.delete, .deleteForward]) { _ in store.deleteSelected(); return .handled }
    .onKeyPress(characters: .init(charactersIn: "ijklobezIJKLOBEZ")) { press in
      let cmd = press.modifiers.contains(.command)
      switch press.characters.lowercased() {
      case "i" where !cmd: store.setIn()
      case "o" where !cmd: store.setOut()
      case "j" where !cmd: store.seek(store.playheadNs - 1_000_000_000)
      case "k" where !cmd: if store.playing { store.togglePlay() }
      case "l" where !cmd: store.seek(store.playheadNs + 1_000_000_000)
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
      Button { store.togglePlay() } label: { Image(systemName: store.playing ? "pause.fill" : "play.fill") }
        .help("Play / pause (Space)").accessibilityLabel(store.playing ? "Pause" : "Play")
      Button { store.step(1) } label: { Image(systemName: "forward.frame") }
        .help("Next frame (→)").accessibilityLabel("Next frame")
      Text("\(editorTimecode(store.shownPlayhead)) / \(editorTimecode(store.lengthNs))")
        .font(MVTheme.font(14))
        .foregroundStyle(MVTheme.title)
        .monospacedDigit()
        .padding(.horizontal, 10)
        .accessibilityLabel("Playhead \(editorTimecode(store.shownPlayhead)) of \(editorTimecode(store.lengthNs))")
      divider
      Button("Split") { store.split() }.help("Split at the playhead (⌘B)")
      Button("Delete") { store.deleteSelected() }.help("Delete the selected piece (⌫)")
        .disabled(store.pieces.count < 2)
      Button("Set in") { store.setIn() }.help("Cut everything before the playhead (I)")
      Button("Set out") { store.setOut() }.help("Cut everything after the playhead (O)")
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

/// The ruler, the video track (thumbnails per piece), the audio track
/// (waveform per piece) and the playhead, over the whole program.
private struct TimelineArea: View {
  @ObservedObject private var store = VideoEditorStore.shared

  private let rulerH: CGFloat = 18
  private let videoH: CGFloat = 76
  private let audioH: CGFloat = 52
  private let gap: CGFloat = 6

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
            let rect = CGRect(x: x(start), y: rulerH + gap, width: max(1, x(start + len) - x(start)) - 2,
                              height: videoH)
            drawPiece(ctx, index: i, piece: p, rect: rect)
            let arect = CGRect(x: rect.minX, y: rect.maxY + gap, width: rect.width, height: audioH)
            drawWave(ctx, piece: p, rect: arect)
            start += len
          }
          // The playhead.
          let px = x(store.shownPlayhead)
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
        .contentShape(Rectangle())
        .gesture(
          DragGesture(minimumDistance: 0)
            .onChanged { g in
              let t = Int64(Double(max(0, min(g.location.x, width)) / width) * Double(length))
              store.scrubNs = t
              store.seek(t)
            }
            .onEnded { g in
              let t = Int64(Double(max(0, min(g.location.x, width)) / width) * Double(length))
              store.seek(t)
              store.scrubNs = nil
            })
      }
      .accessibilityElement(children: .ignore)
      .accessibilityLabel("Timeline, \(store.pieces.count) pieces")
      .accessibilityValue("Playhead \(editorTimecode(store.shownPlayhead)) of \(editorTimecode(store.lengthNs))")
      .accessibilityAdjustableAction { dir in
        store.seek(store.playheadNs + (dir == .increment ? 1_000_000_000 : -1_000_000_000))
      }
    }
    .frame(height: rulerH + gap + videoH + gap + audioH + 4)
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
      tick.move(to: CGPoint(x: xx, y: rulerH - 6))
      tick.addLine(to: CGPoint(x: xx, y: rulerH))
      ctx.stroke(tick, with: .color(MVTheme.hairline), lineWidth: 1)
      ctx.draw(Text(editorTimecode(Int64(t * 1e9)).components(separatedBy: ".")[0])
                 .font(MVTheme.font(10)).foregroundColor(MVTheme.body),
               at: CGPoint(x: xx + 2, y: 1), anchor: .topLeading)
      t += step
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
