// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 9 (docs/design/06, docs/design/16): the state the metadata pane reads. It mirrors a record
// the host already holds -- summary rows, the full tag tree, per-stream inspector
// -- and re-reads it only when the host's metadata generation moves. Nothing here
// reads the file; the pane opening, closing or switching tabs is free.
import AppKit
import Combine
import MVChromeBridge

struct MetaRow: Identifiable {
  let id: Int
  let label: String
  let value: String
}

struct MetaProperty: Identifiable {
  let id: Int
  let space: String  // exif | iptc | xmp | container | computed | c2pa | png_text | icc | xmp_raw
  let group: String
  let label: String
  let value: String
  let rawTag: String
  /// PR 29: the value in the form an edit takes, and what an edit may do:
  /// "e" set and remove, "s" set only (into the XMP sidecar), "r" read-only.
  let raw: String
  let access: String

  var editable: Bool { access == "e" || access == "s" }
  var removable: Bool { access == "e" }
  /// Why a row cannot be changed, for its lock's help tag.
  var lockReason: String {
    switch space {
    case "container": return "Stored in the file's container; container tags are read-only"
    case "computed": return "Worked out by the viewer, not stored in the file"
    case "c2pa": return "Content Credentials are signed by their maker; shown as stated, never changed"
    case "png_text": return "A PNG text chunk; shown, never rewritten"
    case "icc": return "Part of the embedded colour profile"
    case "xmp_raw": return "An XMP property read as written; it is shown, not edited here"
    default: return "Describes the file itself (size, layout, maker note); changing it would break the file"
    }
  }
}

/// #290: meta::geo_point — WGS 84 degrees, north / east positive; metres.
struct MetaLocation: Equatable {
  let lat: Double
  let lon: Double
  let altitude: Double?
}

/// #352: what the Location editor's text says (meta::parse_location, through the bridge).
enum LocationParse: Equatable {
  case point(lat: Double, lon: Double, preview: String)
  case blank
  case invalid(String)
}

/// #352 (meta::location_rights_of): what the Summary's Location row may do.
struct LocationRights: Equatable {
  var canSet = false
  var canClear = false
  var inOriginal = false
}

struct MetaStream: Identifiable {
  let id: Int
  let kind: String  // video | audio | subtitle | attachment | data
  let codec: String
  var fields: [MetaRow]
}

struct MetaChapter: Identifiable {
  let id: Int
  let startMs: Int64
  let title: String
}

/// Reads one of the host's "lines of tab-separated fields" tables. Two calls: the
/// first reports the length needed, the second fills a buffer of exactly that size.
private func readTable(_ fetch: (UnsafeMutablePointer<CChar>?, Int32) -> Int32) -> [[Substring]] {
  let needed = Int(fetch(nil, 0))
  guard needed > 0 else { return [] }
  var buf = [CChar](repeating: 0, count: needed + 1)
  _ = buf.withUnsafeMutableBufferPointer { fetch($0.baseAddress, Int32($0.count)) }
  return String(cString: buf).split(separator: "\n", omittingEmptySubsequences: true)
    .map { $0.split(separator: "\t", omittingEmptySubsequences: false) }
}

/// Reads one of the host's plain strings (two calls, like `readTable`).
private func readText(_ fetch: (UnsafeMutablePointer<CChar>?, Int32) -> Int32) -> String {
  let needed = Int(fetch(nil, 0))
  guard needed > 0 else { return "" }
  var buf = [CChar](repeating: 0, count: needed + 1)
  _ = buf.withUnsafeMutableBufferPointer { fetch($0.baseAddress, Int32($0.count)) }
  return String(cString: buf)
}

@MainActor
final class MetadataStore: ObservableObject {
  static let shared = MetadataStore()

  @Published private(set) var visible = false
  @Published private(set) var loading = false
  @Published private(set) var summary: [MetaRow] = []
  @Published private(set) var properties: [MetaProperty] = []
  @Published private(set) var streams: [MetaStream] = []
  @Published private(set) var chapters: [MetaChapter] = []
  /// The item the record is for: a half-typed edit belongs to it (issue #190).
  @Published private(set) var itemPath = ""
  /// #290: provenance and position as the core decided them (meta::summary). The
  /// summary's "Made with" row already shows the first two; these are for the
  /// map (#344) and the AI badge (#345).
  @Published private(set) var madeWith = ""
  @Published private(set) var aiGenerated = false
  @Published private(set) var location: MetaLocation?
  /// #352: the Location editor's starting text ("38.7979, -9.3907") and what the row may do.
  @Published private(set) var locationText = ""
  @Published private(set) var locationRights = LocationRights()

  // PR 12 (docs/design/06 "Writing"): what the pane edits. The host owns the truth; a
  // change is queued there and lands on its I/O pool. The rating and comment are
  // edited as tags under All tags; the keys 0-5 and Ctrl+I go through the host.
  @Published private(set) var canEdit = false
  @Published private(set) var canRevert = false
  /// Moves on each ask for the pane's keyboard (`I`, Ctrl+I); the view focuses
  /// once per ask: All tags' search when `focusTags`, else the pane (issue #185).
  @Published private(set) var focusSeq: UInt64 = 0
  private(set) var focusTags = false

  private var generation: UInt64 = .max

  private init() {
    // Issue #178: polled when the main run loop has run (ChromePulse), not on a timer.
    ChromePulse.shared.add { [weak self] in
      self?.poll()
      return false
    }
  }

  /// A still has no streams; the Streams tab is for clips.
  var isClip: Bool { !streams.isEmpty }

  private func poll() {
    let nowVisible = mv_chrome_meta_pane_visible()
    if nowVisible != visible { visible = nowVisible }
    guard nowVisible else { return }  // closed: no parsing, no work
    let isLoading = mv_chrome_meta_loading()
    if isLoading != loading { loading = isLoading }
    // Cheap host reads, every tick.
    let editable = mv_chrome_meta_can_edit()
    if editable != canEdit { canEdit = editable }
    let revertible = mv_chrome_meta_can_revert()
    if revertible != canRevert { canRevert = revertible }
    let focus = mv_chrome_meta_focus_seq()
    if focus != focusSeq {
      focusTags = mv_chrome_meta_focus_tags()  // set before the view sees the seq move
      focusSeq = focus
    }

    let g = mv_chrome_meta_generation()
    guard g != generation else { return }
    generation = g
    reload()
  }

  func revert() { mv_chrome_meta_revert() }

  // PR 29: any tag, the date, and removing.
  func setTag(_ key: String, _ value: String) {
    key.withCString { k in value.withCString { v in mv_chrome_meta_set_tag(k, v) } }
  }
  func removeTag(_ key: String) { key.withCString { mv_chrome_meta_set_tag($0, nil) } }
  func setDate(_ value: String) { value.withCString { mv_chrome_meta_set_date($0) } }
  func removeDate() { mv_chrome_meta_set_date(nil) }

  /// #352: reads the text exactly as the write will. Pure and cheap: the field's
  /// preview calls it on every keystroke.
  func parseLocation(_ text: String) -> LocationParse {
    var r = mv_chrome_location_input()
    r.struct_size = UInt32(MemoryLayout<mv_chrome_location_input>.size)
    let ok = text.withCString { mv_chrome_meta_parse_location($0, &r) }
    guard ok else { return .invalid("Could not read the position") }
    let message = withUnsafeBytes(of: &r.text) { raw -> String in
      let bytes = raw.bindMemory(to: UInt8.self)
      let end = bytes.firstIndex(of: 0) ?? bytes.count
      return String(decoding: bytes[..<end], as: UTF8.self)
    }
    switch r.kind {
    case UInt32(MV_LOCATION_INPUT_POINT): return .point(lat: r.lat, lon: r.lon, preview: message)
    case UInt32(MV_LOCATION_INPUT_BLANK): return .blank
    default: return .invalid(message)
    }
  }
  /// The position as typed; blank removes every GPS row (the view confirms first).
  func setLocation(_ text: String) { text.withCString { mv_chrome_meta_set_location($0) } }

  /// The date as the editor shows it ("2024-05-01 14:03:22"), from the summary.
  var dateTaken: String {
    summary.first { $0.label == "Date taken" }?.value ?? ""
  }
  func blur() { mv_chrome_meta_blur() }

  private func reload() {
    // Read here, when the generation moved (a selection move bumps it), not per poll.
    let index = mv_chrome_current_index()
    let path = index < 0 ? "" : readText { mv_chrome_item_path(index, $0, $1) }
    if path != itemPath { itemPath = path }
    summary = readTable { mv_chrome_meta_summary($0, $1) }.enumerated().map { i, f in
      MetaRow(id: i, label: String(f.first ?? ""), value: f.count > 1 ? String(f[1]) : "")
    }
    properties = readTable { mv_chrome_meta_properties($0, $1) }.enumerated().compactMap { i, f in
      guard f.count >= 5 else { return nil }
      return MetaProperty(
        id: i, space: String(f[0]), group: String(f[1]), label: String(f[2]), value: String(f[3]),
        rawTag: String(f[4]), raw: f.count > 5 ? String(f[5]) : String(f[3]),
        access: f.count > 6 ? String(f[6]) : "r")
    }
    var parsed: [MetaStream] = []
    var chaps: [MetaChapter] = []
    for f in readTable({ mv_chrome_meta_streams($0, $1) }) {
      switch f.first {
      case "S" where f.count >= 4:
        parsed.append(MetaStream(id: Int(f[1]) ?? parsed.count, kind: String(f[2]), codec: String(f[3]), fields: []))
      case "F" where f.count >= 3 && !parsed.isEmpty:
        parsed[parsed.count - 1].fields.append(
          MetaRow(id: parsed[parsed.count - 1].fields.count, label: String(f[1]), value: String(f[2])))
      case "C" where f.count >= 3:
        chaps.append(MetaChapter(id: chaps.count, startMs: Int64(f[1]) ?? 0, title: String(f[2])))
      default: break
      }
    }
    streams = parsed
    chapters = chaps
    madeWith = readText { mv_chrome_meta_made_with($0, $1) }
    var facts = mv_chrome_meta_facts()
    facts.struct_size = UInt32(MemoryLayout<mv_chrome_meta_facts>.size)
    _ = mv_chrome_meta_facts_get(&facts)
    aiGenerated = facts.flags & UInt32(MV_META_FACT_AI_GENERATED) != 0
    location = facts.flags & UInt32(MV_META_FACT_WHERE) != 0
      ? MetaLocation(
        lat: facts.lat, lon: facts.lon,
        altitude: facts.flags & UInt32(MV_META_FACT_ALTITUDE) != 0 ? facts.altitude_m : nil)
      : nil
    locationRights = LocationRights(
      canSet: facts.flags & UInt32(MV_META_FACT_LOCATION_SET) != 0,
      canClear: facts.flags & UInt32(MV_META_FACT_LOCATION_CLEAR) != 0,
      inOriginal: facts.flags & UInt32(MV_META_FACT_LOCATION_IN_ORIGINAL) != 0)
    locationText = readText { mv_chrome_meta_location_text($0, $1) }
  }
}
