// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A thin, checked view of the mv.ai.1 table (mediaviewer_ai.h), the theme the
// base chrome uses, and small helpers shared by the search panel and the
// management view. Nothing here logs a path, a query or a name (rule 6).
import AppKit
import CAiApi
import CoreText
import Foundation
import SwiftUI

/// The table. [no-block] calls may run on the main actor; [worker-thread]
/// calls (roots_json, result_thumb, people_json, person_faces_json, and the
/// writes that may wait on the indexer) only from a detached task. The table
/// lives until the chrome's -shutdown, which closes it: a call after that
/// fails instead of reaching an unloaded pack, and -shutdown waits (bounded)
/// for the calls already inside it.
final class AITable: @unchecked Sendable {
  let api: UnsafePointer<mv_ai_api>
  private let gate = NSCondition()
  private var closed = false
  private var inFlight = 0

  init(_ api: UnsafePointer<mv_ai_api>) { self.api = api }

  var ctx: UnsafeMutableRawPointer? { UnsafeRawPointer(api).load(fromByteOffset: 8, as: UnsafeMutableRawPointer?.self) }
  /// The entries, read one at a time through the pointer: a pack built
  /// before an entry was appended has a shorter table (struct_size), and
  /// copying the whole struct would read past it.
  var a: Entries { Entries(table: self) }

  @dynamicMemberLookup
  struct Entries {
    let table: AITable
    subscript<T>(dynamicMember field: KeyPath<mv_ai_api, T?>) -> T? {
      guard table.isOpen, let offset = MemoryLayout<mv_ai_api>.offset(of: field),
            table.structSize >= offset + MemoryLayout<T?>.size else { return nil }
      return UnsafeRawPointer(table.api).load(fromByteOffset: offset, as: T?.self)
    }
  }

  private var structSize: Int { Int(UnsafeRawPointer(api).load(as: UInt32.self)) }

  private var isOpen: Bool {
    gate.lock()
    defer { gate.unlock() }
    return !closed
  }

  /// Runs `body` while the table is open, counted so -shutdown can wait for
  /// it; nil once closed. Everything a detached task calls goes through here
  /// (json, path and status do it themselves).
  func guarded<R>(_ body: () -> R) -> R? {
    gate.lock()
    if closed {
      gate.unlock()
      return nil
    }
    inFlight += 1
    gate.unlock()
    defer {
      gate.lock()
      inFlight -= 1
      if inFlight == 0 { gate.broadcast() }
      gate.unlock()
    }
    return body()
  }

  /// A status call through the guard; a closed table or a missing entry fails.
  @discardableResult
  func call(_ body: () -> mv_status?) -> mv_status {
    (guarded(body) ?? nil) ?? MV_ERR_INVALID_ARG
  }

  /// -shutdown: no new calls, and up to `timeout` for the ones in flight.
  /// True when none is left inside the pack.
  @discardableResult
  func close(timeout: TimeInterval) -> Bool {
    gate.lock()
    defer { gate.unlock() }
    closed = true
    let deadline = Date(timeIntervalSinceNow: timeout)
    while inFlight > 0 && gate.wait(until: deadline) {}
    return inFlight == 0
  }

  /// The buffer rule: retry with `needed`.
  func json(_ call: (UnsafeMutablePointer<CChar>?, UInt32, UnsafeMutablePointer<UInt32>?) -> mv_status) -> String? {
    guarded {
      var cap: UInt32 = 64 * 1024
      for _ in 0..<5 {
        var buf = [CChar](repeating: 0, count: Int(cap))
        var needed: UInt32 = 0
        let s = buf.withUnsafeMutableBufferPointer { call($0.baseAddress, cap, &needed) }
        if s == MV_OK { return String(cString: buf) }
        if s != MV_ERR_INVALID_ARG || needed <= cap { return nil }
        cap = needed + 1024
      }
      return nil
    } ?? nil
  }

  func path(_ call: (UnsafeMutablePointer<CChar>?, UInt32) -> mv_status) -> String? {
    guarded {
      var buf = [CChar](repeating: 0, count: 8 * 1024)
      let s = buf.withUnsafeMutableBufferPointer { call($0.baseAddress, UInt32($0.count)) }
      return s == MV_OK ? String(cString: buf) : nil
    } ?? nil
  }

  func status() -> mv_ai_status? {
    guarded {
      var st = mv_ai_status()
      st.struct_size = UInt32(MemoryLayout<mv_ai_status>.size)
      guard let fn = a.status, fn(ctx, &st) == MV_OK else { return nil }
      return st
    } ?? nil
  }

  /// face_thumb was appended to mv.ai.1 late in Milestone H: a pack whose
  /// table is shorter does not have it (struct_size says so).
  var hasFaceThumb: Bool { a.face_thumb != nil }

  /// Whether the pack's table reaches `field` (an entry appended after the
  /// pack was built is past its struct_size and must not be read).
  func has(_ field: PartialKeyPath<mv_ai_api>) -> Bool {
    guard isOpen, let offset = MemoryLayout<mv_ai_api>.offset(of: field) else { return false }
    return structSize >= offset + MemoryLayout<UnsafeRawPointer>.size
  }

  /// 2026-09-27 audio entries: root_set_media, result_snippet.
  var hasAudio: Bool { a.result_snippet != nil }

  /// Issue #72: the pack has a Photos library source (its table has the
  /// entries, and it is not a build without one).
  var hasPhotos: Bool {
    guard has(\mv_ai_api.photos_access), let fn = a.photos_access else { return false }
    var access: UInt32 = 0
    return guarded { fn(ctx, &access) } == MV_OK && access != MV_AI_PHOTOS_UNSUPPORTED.rawValue
  }

  /// Loaded through the pack's read-only door (MV_AI_READER_ENTRY_SYMBOL): a
  /// later MediaViewer window, whose process does not host the add-ons
  /// (2026-10-07). Its index is the first window's, opened read-only: search,
  /// find-similar and People's photos work; nothing that changes the index or
  /// a setting does (the pack refuses them). settings_json says so; read once.
  lazy var readOnly: Bool = {
    let obj = parseJSON(json { a.settings_json?(ctx, $0, $1, $2) ?? MV_ERR_INVALID_ARG }) as? [String: Any]
    return obj?["read_only"] as? Bool ?? false
  }()

  func setSetting(_ key: String, _ valueJSON: String) {
    call { a.set_setting?(ctx, key, valueJSON) }
  }

  /// "Index anyway": ignore the battery pause until the Mac is next on power
  /// (or MediaViewer restarts). Never saved. [no-block]
  func indexAnyway() { setSetting("battery_override", "1") }
}

/// `body(nil)` for nil, else the C string: the table takes NULL for "no scope".
func withOptionalCString<R>(_ s: String?, _ body: (UnsafePointer<CChar>?) -> R) -> R {
  guard let s else { return body(nil) }
  return s.withCString { body($0) }
}

func parseJSON(_ text: String?) -> Any? {
  guard let text else { return nil }
  return try? JSONSerialization.jsonObject(with: Data(text.utf8))
}

func int64(_ v: Any?) -> Int64 {
  if let n = v as? NSNumber { return n.int64Value }
  return 0
}

/// The base chrome's colour roles (MVTheme in MediaViewerChrome): semantic
/// AppKit colours, so light, dark and increased contrast follow the system.
enum AITheme {
  static let title = Color(nsColor: .labelColor)
  static let body = Color(nsColor: .secondaryLabelColor)
  static let hairline = Color(nsColor: .separatorColor)
  static let surface = Color(nsColor: .controlBackgroundColor)
  static let canvas = Color(nsColor: .windowBackgroundColor)

  private static let registered: Bool = {
    // The base app registers the face for the process; registering again
    // from the same file is harmless and covers a chrome shown first.
    let dir = Bundle.main.executableURL?.deletingLastPathComponent()
    guard let url = dir?.appendingPathComponent("CozetteVector.ttf") else { return false }
    return CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
  }()

  static func font(_ size: CGFloat = 16) -> Font {
    _ = registered
    return .custom("CozetteVector", size: size)
  }
}

/// "1:05" / "1:02:03" for a moment badge.
func momentText(_ ms: Int64) -> String {
  let total = max(0, Int(ms / 1000))
  let h = total / 3600, m = (total % 3600) / 60, s = total % 60
  return h > 0 ? String(format: "%d:%02d:%02d", h, m, s) : String(format: "%d:%02d", m, s)
}

/// One formatter for every count (the status line is rebuilt at 4 Hz).
/// NumberFormatter is thread-safe for formatting since macOS 10.9.
private let decimalFormatter: NumberFormatter = {
  let f = NumberFormatter()
  f.numberStyle = .decimal
  return f
}()

func countText(_ n: UInt64) -> String {
  decimalFormatter.string(from: NSNumber(value: n)) ?? String(n)
}

func bytesText(_ n: UInt64) -> String {
  ByteCountFormatter.string(fromByteCount: Int64(clamping: n), countStyle: .file)
}

/// "about 6–9 min", from the pack's measured range; "" when unknown.
func etaText(_ low: Double, _ high: Double) -> String {
  guard low >= 0, high >= 0 else { return "" }
  if high < 60 { return "under a minute" }
  if high >= 5400 {
    let lo = max(1, Int((low / 3600).rounded())), hi = max(lo, Int((high / 3600).rounded()))
    return lo == hi ? "about \(lo) h" : "about \(lo)–\(hi) h"
  }
  let lo = max(1, Int((low / 60).rounded(.up))), hi = max(lo, Int((high / 60).rounded(.up)))
  return lo == hi ? "about \(lo) min" : "about \(lo)–\(hi) min"
}

/// The status line (the brief's "Footer / status"), shared by the panel footer
/// and the management view.
struct StatusLine: Equatable {
  var text = ""
  var detail = ""          // the fallback reason, the migration, a full index
  var badge = ""           // "Neural Engine", "CPU"
  var progress: Double = 0
  var spinning = false
  var indexing = false     // something is in progress (indexing or waiting)
  var paused = false       // the user paused it
  var onBattery = false    // waiting on battery: "Index anyway" can override it
  var idle = true
  var help = ""            // the provider's own words for a fault (a tooltip), paths replaced
  var sound = ""           // "Sound: 12 of 40 clips · Speech: 8 of 40"; "" without the piece
  var audioReady = false   // the ai-audio piece is loaded
  /// The passes beside the one `text` names, while any runs: "Pictures ✓",
  /// "Sound 14%". Empty without the audio piece (pictures is then the only pass).
  var stages: [Stage] = []

  struct Stage: Equatable, Identifiable {
    var name: String
    var done: UInt64
    var total: UInt64
    var id: String { name }
    var finished: Bool { done >= total }
    var fraction: Double { total == 0 ? 1 : min(1, Double(done) / Double(total)) }
  }

  init() {}

  init(_ s: mv_ai_status) {
    let state = s.state
    switch state {
    case MV_AI_STATE_INDEXING.rawValue:
      // The line names the pass that is running: pictures first, then the
      // clips' sound, then their speech. The ETA covers pictures only (the
      // pack's estimate), so it is shown only while pictures run.
      switch Self.phase(s) {
      case .pictures where Self.picturesDone(s) >= s.assets_total && s.icloud_videos_left > 0:
        // Everything on this Mac is done; a clip is on its way from iCloud.
        text = "Downloading from iCloud · \(countText(s.icloud_videos_left)) clips left"
      case .pictures where s.cloud_fetch == MV_AI_ICLOUD_DOWNLOADING.rawValue:
        // A file from iCloud Drive is on its way (2026-10-05).
        text = "Downloading from iCloud Drive (\(Int((s.cloud_fetch_progress * 100).rounded()))%) · "
          + "\(countText(s.cloud_files_left)) left to index"
      case .pictures:
        let eta = etaText(s.eta_low_seconds, s.eta_high_seconds)
        text = (s.sound_total > 0 || s.speech_total > 0 ? "Indexing pictures " : "Indexing ")
          + "\(countText(Self.picturesDone(s))) of \(countText(s.assets_total))"
          + (eta.isEmpty ? "" : " · \(eta)")
      case .sound:
        text = "Indexing sound \(countText(s.sound_done)) of \(countText(s.sound_total)) clips"
      case .speech:
        text = "Indexing speech \(countText(s.speech_done)) of \(countText(s.speech_total)) clips"
      }
      spinning = true
      indexing = true
      idle = false
    case MV_AI_STATE_YIELDING.rawValue:
      switch s.yield_reason {
      case MV_AI_YIELD_VIEWER.rawValue: text = "Paused while a video plays"
      case MV_AI_YIELD_BATTERY.rawValue:
        text = "Paused on battery"
        onBattery = true
      case MV_AI_YIELD_FRAMES.rawValue: text = "Paused to keep playback smooth"
      default: text = "Paused"
      }
      indexing = true
      idle = false
    case MV_AI_STATE_PAUSED.rawValue:
      text = "Paused"
      paused = true
      idle = false
    case MV_AI_STATE_LOADING.rawValue:
      // It opens the models only between the viewer's busy spells.
      let waiting = s.yield_reason != 0
      // Core ML compiles each model for this Mac once (minutes, cached
      // after): the pack says when this load is that first one. Every other
      // start is an ordinary load, and says so (2026-09-27). Search answers
      // meanwhile, on CPU; indexing waits for the Neural Engine (2026-10-05).
      let first = s.flags & MV_AI_STATUS_FIRST_COMPILE != 0
      text = waiting ? "Loading the search model when the viewer is idle"
        : first ? "Preparing the search model for this Mac (first time only, a few minutes). Search works meanwhile"
        : "Loading the search model…"
      spinning = !waiting
      idle = false
    case MV_AI_STATE_ERROR.rawValue:
      text = "Search is unavailable: the model would not load"
      idle = false
    default:
      text = s.frames_indexed == 0 ? "Nothing indexed yet"
        : "Up to date · \(countText(s.frames_indexed)) moments"
      if s.cloud_files_left > 0 { text += " · \(countText(s.cloud_files_left)) only in iCloud Drive" }
    }
    // The ring follows the pass the line names.
    func fraction(_ done: UInt64, _ total: UInt64) -> Double {
      total == 0 ? 0 : min(1, Double(done) / Double(total))
    }
    let phase = Self.phase(s)
    switch phase {
    case .pictures: progress = fraction(Self.picturesDone(s), s.assets_total)
    case .sound: progress = fraction(s.sound_done, s.sound_total)
    case .speech: progress = fraction(s.speech_done, s.speech_total)
    }
    audioReady = s.flags & MV_AI_STATUS_AUDIO_READY != 0
    badge = s.backend == MV_AI_BACKEND_COREML.rawValue ? "Neural Engine" : "CPU"
    var notes: [String] = []  // appended below; the sound line goes first while it runs
    if s.provider_fault != 0 {
      switch s.provider_fault {
      case 1: notes.append("Core ML is not in this build — using CPU")
      case 2: notes.append("Core ML runtime not found — using CPU")
      case 3: notes.append("Core ML failed — using CPU")
      case 4: notes.append("Core ML results differed from the CPU — using CPU")
      case 5: notes.append("Core ML was slower than the CPU here — using CPU")
      default: notes.append("Using CPU")
      }
    }
    if s.flags & MV_AI_STATUS_SMALL_FALLBACK != 0 {
      // Auto keeps the large model off a provider that failed it here (2026-10-05).
      notes.append("Using the smaller search model: the larger one failed on Core ML on this Mac")
    }
    help = withUnsafeBytes(of: s.provider_detail_utf8) { raw in
      String(decoding: raw.prefix(while: { $0 != 0 }), as: UTF8.self)
    }
    if s.migrate_total > 0 && s.migrate_done < s.migrate_total {
      notes.append("Upgrading the index: \(countText(s.migrate_done)) of \(countText(s.migrate_total)). Searches use the current index until it finishes")
    }
    if s.flags & MV_AI_STATUS_INDEX_FULL != 0 {
      notes.append("Index is full — raise the cap or remove a folder")
    }
    if let fetch = Self.icloudNote(s) { notes.append(fetch) }
    if s.flags & MV_AI_STATUS_NO_MODELS != 0 {
      notes.append("The search models could not be loaded. Reinstall Core in Settings")
    }
    // The Sound piece's progress: "Sound: 12 of 40 clips · Speech: 8 of 40".
    if s.sound_total > 0 || s.speech_total > 0 {
      var parts: [String] = []
      if s.sound_total > 0 { parts.append("Sound: \(countText(s.sound_done)) of \(countText(s.sound_total)) clips") }
      if s.speech_total > 0 { parts.append("Speech: \(countText(s.speech_done)) of \(countText(s.speech_total))") }
      sound = parts.joined(separator: " · ")
      // While any pass runs, the others sit beside the line as stages
      // rather than as a sentence under it.
      if !idle && (Self.picturesDone(s) < s.assets_total || s.sound_done < s.sound_total
                   || s.speech_done < s.speech_total) {
        let all: [(Phase, Stage)] = [
          (.pictures, Stage(name: "Pictures", done: Self.picturesDone(s), total: s.assets_total)),
          (.sound, Stage(name: "Sound", done: s.sound_done, total: s.sound_total)),
          (.speech, Stage(name: "Speech", done: s.speech_done, total: s.speech_total)),
        ]
        let shown = state == MV_AI_STATE_INDEXING.rawValue ? phase : nil
        stages = all.filter { $0.0 != shown && $0.1.total > 0 }.map(\.1)
      }
    }
    detail = notes.joined(separator: " · ")
  }

  enum Phase { case pictures, sound, speech }

  /// The opt-in iCloud fetch (Settings → Photos Library), when it has
  /// something to say: what it is downloading, or what it waits for.
  static func icloudNote(_ s: mv_ai_status) -> String? {
    let left = countText(s.icloud_videos_left)
    switch s.icloud_fetch {
    case MV_AI_ICLOUD_DOWNLOADING.rawValue:
      let pct = Int((Double(s.icloud_fetch_progress) * 100).rounded(.down))
      return "Downloading from iCloud (\(pct)%) · \(left) clips left"
    case MV_AI_ICLOUD_WAIT_INDEXER.rawValue:
      return "iCloud: \(left) clips left to download"
    case MV_AI_ICLOUD_WAIT_NETWORK.rawValue:
      return s.icloud_videos_left > 0 ? "iCloud downloads wait for an unmetered network · \(left) clips left" : nil
    case MV_AI_ICLOUD_WAIT_POWER.rawValue:
      return s.icloud_videos_left > 0 ? "iCloud downloads wait for power · \(left) clips left" : nil
    case MV_AI_ICLOUD_LOW_DISK.rawValue:
      return s.icloud_videos_left > 0 ? "iCloud downloads wait: under 10 GB free · \(left) clips left" : nil
    case MV_AI_ICLOUD_RETRY_LATER.rawValue:
      return "iCloud did not send \(left) clips; they are tried again next time MediaViewer starts"
    default:
      return nil
    }
  }

  /// Pictures are handled once indexed, failed, or only in iCloud (issue #72).
  static func picturesDone(_ s: mv_ai_status) -> UInt64 {
    // Evicted iCloud Drive files wait for their opt-in fetch: not work in hand.
    s.assets_done + s.assets_failed + s.assets_unavailable + s.cloud_files_left
  }

  /// The pass running now: pictures until they are all handled, then sound,
  /// then speech. Pictures again once everything is handled.
  static func phase(_ s: mv_ai_status) -> Phase {
    if picturesDone(s) < s.assets_total { return .pictures }
    if s.sound_done < s.sound_total { return .sound }
    if s.speech_done < s.speech_total { return .speech }
    return .pictures
  }
}

/// A small ring: the running pass's progress as a solid arc over a faint
/// track, and while work runs a soft comet that sweeps the track. The comet is
/// a gradient tail, not a second hard arc, so it never reads as progress.
/// Driven by a TimelineView (stable under the 4 Hz status re-renders, paused
/// when nothing turns); Reduce Motion leaves the arc alone.
struct AIProgressRing: View {
  let progress: Double
  let spinning: Bool
  var lineWidth: CGFloat = 2.5

  var body: some View {
    ZStack {
      Circle().stroke(Color.primary.opacity(0.12), lineWidth: lineWidth)
      if spinning {
        TimelineView(.animation(minimumInterval: 1.0 / 60.0, paused: false)) { context in
          let t = context.date.timeIntervalSinceReferenceDate
          Circle()
            .trim(from: 0, to: 0.32)
            .stroke(AngularGradient(gradient: Gradient(colors: [Color.accentColor.opacity(0),
                                                                Color.accentColor.opacity(0.7)]),
                                    center: .center,
                                    startAngle: .degrees(0), endAngle: .degrees(0.32 * 360)),
                    style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
            .rotationEffect(.degrees((t.truncatingRemainder(dividingBy: 1.4) / 1.4) * 360))
        }
        .transition(.opacity)
      }
      if progress > 0.002 {
        Circle()
          .trim(from: 0, to: min(1, progress))
          .stroke(Color.accentColor, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
          .rotationEffect(.degrees(-90))
          .animation(.easeOut(duration: 0.4), value: progress)
      }
    }
    .padding(lineWidth / 2)
    .animation(.easeInOut(duration: 0.25), value: spinning)
  }
}

/// "Sound 14%" or "Pictures ✓": one pass beside the status line.
struct StageChip: View {
  let stage: StatusLine.Stage

  var body: some View {
    HStack(spacing: 4) {
      if stage.finished {
        Image(systemName: "checkmark").font(.system(size: 9, weight: .semibold))
          .foregroundStyle(Color.accentColor)
        Text(stage.name)
      } else {
        AIProgressRing(progress: stage.fraction, spinning: false, lineWidth: 1.5)
          .frame(width: 10, height: 10)
        Text("\(stage.name) \(Int((stage.fraction * 100).rounded(.down)))%")
          .monospacedDigit()
      }
    }
    .font(AITheme.font(11.5))
    .foregroundStyle(AITheme.body)
    .help(stage.finished ? "\(stage.name): all \(countText(stage.total)) done"
          : "\(stage.name): \(countText(stage.done)) of \(countText(stage.total))")
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(stage.finished ? "\(stage.name) done"
                        : "\(stage.name) \(countText(stage.done)) of \(countText(stage.total))")
  }
}

/// The status pill: ring, text, compute badge, Pause / Resume, and "Index
/// anyway" while it waits on battery (when the owner passes `onIndexAnyway`).
/// Without `onPause` (a read-only window) there is no Pause / Resume.
struct StatusPill: View {
  let line: StatusLine
  var onIndexAnyway: (() -> Void)? = nil
  let onPause: ((Bool) -> Void)?
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    HStack(spacing: 8) {
      if !line.idle {
        AIProgressRing(progress: line.progress, spinning: line.spinning && !reduceMotion)
          .frame(width: 18, height: 18)
      } else {
        Image(systemName: "checkmark.circle").foregroundStyle(AITheme.body)
      }
      VStack(alignment: .leading, spacing: 2) {
        Text(line.text).font(AITheme.font(13)).foregroundStyle(AITheme.title)
          .lineLimit(1).contentTransition(.numericText())
        if !line.stages.isEmpty {
          HStack(spacing: 10) {
            ForEach(line.stages) { StageChip(stage: $0) }
          }
        }
        if !line.detail.isEmpty {
          Text(line.detail).font(AITheme.font(12)).foregroundStyle(AITheme.body)
            .lineLimit(2)
            .help(line.help.isEmpty ? line.detail : "\(line.detail)\n\nCore ML said: \(line.help)")
        }
      }
      Spacer(minLength: 8)
      Text(line.badge)
        .font(AITheme.font(12))
        .foregroundStyle(AITheme.body)
        .padding(.horizontal, 7).padding(.vertical, 2)
        .background(Capsule().stroke(AITheme.hairline, lineWidth: 1))
        .fixedSize()
      if line.onBattery, let onIndexAnyway {
        Button("Index anyway", action: onIndexAnyway)
          .buttonStyle(.borderless)
          .font(AITheme.font(13))
          .fixedSize()
          .help("Carry on indexing on battery until the Mac is next on power. "
                + "The battery setting in Settings → Local search stays as it is.")
      }
      if line.indexing || line.paused, let onPause {
        Button(line.paused ? "Resume" : "Pause") { onPause(!line.paused) }
          .buttonStyle(.borderless)
          .font(AITheme.font(13))
          .fixedSize()
      }
    }
    .padding(.horizontal, 12).padding(.vertical, 6)
    .background(Capsule().fill(Color.primary.opacity(0.05)))
    .accessibilityElement(children: .combine)
  }
}
