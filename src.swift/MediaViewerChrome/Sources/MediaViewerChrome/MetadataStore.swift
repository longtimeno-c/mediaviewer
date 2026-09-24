// SPDX-License-Identifier: GPL-2.0-or-later
// PR 9 (plan/06, plan/16): the state the metadata pane reads. It mirrors a record
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
  let space: String  // exif | iptc | xmp | container | computed
  let group: String
  let label: String
  let value: String
  let rawTag: String
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

@MainActor
final class MetadataStore: ObservableObject {
  static let shared = MetadataStore()

  @Published private(set) var visible = false
  @Published private(set) var loading = false
  @Published private(set) var summary: [MetaRow] = []
  @Published private(set) var properties: [MetaProperty] = []
  @Published private(set) var streams: [MetaStream] = []
  @Published private(set) var chapters: [MetaChapter] = []

  private var generation: UInt64 = .max
  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.15, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    poll()
  }

  /// A still has no streams; the Streams tab is for clips.
  var isClip: Bool { !streams.isEmpty }

  private func poll() {
    let nowVisible = mv_chrome_meta_pane_visible()
    if nowVisible != visible { visible = nowVisible }
    guard nowVisible else { return }  // closed: no parsing, no work
    let isLoading = mv_chrome_meta_loading()
    if isLoading != loading { loading = isLoading }
    let g = mv_chrome_meta_generation()
    guard g != generation else { return }
    generation = g
    reload()
  }

  private func reload() {
    summary = readTable { mv_chrome_meta_summary($0, $1) }.enumerated().map { i, f in
      MetaRow(id: i, label: String(f.first ?? ""), value: f.count > 1 ? String(f[1]) : "")
    }
    properties = readTable { mv_chrome_meta_properties($0, $1) }.enumerated().compactMap { i, f in
      guard f.count >= 5 else { return nil }
      return MetaProperty(
        id: i, space: String(f[0]), group: String(f[1]), label: String(f[2]), value: String(f[3]),
        rawTag: String(f[4]))
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
  }
}
