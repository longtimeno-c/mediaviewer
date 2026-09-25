// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 13 / 14 (plan/08 "Execution & UX"): the state the Jobs pane reads -- the
// host's clip job queue (abi/clip_session, the same queue the Windows ABI
// drives), polled through the bridge. Nothing here waits on a job: every call
// is a bookkeeping read, and the list is rebuilt only when the host's jobs
// generation moves; progress is re-read while something runs.
import AppKit
import Combine
import MVChromeBridge

struct ClipJobRow: Identifiable, Equatable {
  let id: UInt64
  var state: Int32 = 1  // mv_clip_job_state
  var op: Int32 = 0
  var fraction: Double = 0
  var elapsedMs: Int64 = 0
  var etaMs: Int64 = -1
  var error: Int32 = 0
  var outputCount: Int32 = 0
  var title = ""
  var source = ""

  var running: Bool { state == 1 || state == 2 }

  private static func clock(_ ms: Int64) -> String {
    let s = max(0, Int(ms / 1000))
    return s >= 3600 ? String(format: "%d:%02d:%02d", s / 3600, (s % 3600) / 60, s % 60)
                     : String(format: "%d:%02d", s / 60, s % 60)
  }

  /// The Windows pane's wording (IslandHost.Clip.cs StatusOf).
  var status: String {
    switch state {
    case 1: return "Waiting"
    case 2:
      let pct = String(format: "%.0f %%", fraction * 100)
      return etaMs >= 0 ? "\(pct) · \(Self.clock(etaMs)) left" : pct
    case 3:
      return outputCount > 1 ? "Done in \(Self.clock(elapsedMs)) · \(outputCount) files"
                             : "Done in \(Self.clock(elapsedMs))"
    case 5: return "Cancelled · nothing written"
    default:
      switch error {
      case 4 where op == 2: return "Failed · no hardware encoder here; use the instant trim"
      case 4: return "Failed · this clip or container cannot do that"
      case 1: return "Failed · the range is empty or too long"
      case 3: return "Failed · could not write beside the clip"
      default: return "Failed"
      }
    }
  }
}

@MainActor
final class JobsStore: ObservableObject {
  static let shared = JobsStore()

  @Published private(set) var visible = false
  @Published private(set) var rows: [ClipJobRow] = []

  private var generation: UInt64 = .max
  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    poll()
  }

  private func poll() {
    let nowVisible = mv_chrome_jobs_visible()
    if nowVisible != visible { visible = nowVisible }
    guard nowVisible else { return }
    let g = mv_chrome_jobs_generation()
    if g != generation || rows.contains(where: { $0.running }) {
      generation = g
      reload()
    }
  }

  private static func text(_ id: UInt64, _ which: Int32) -> String {
    let need = mv_chrome_job_text(id, which, nil, 0)
    var buf = [CChar](repeating: 0, count: Int(max(need, 1)))
    _ = mv_chrome_job_text(id, which, &buf, Int32(buf.count))
    return String(cString: buf)
  }

  func reload() {
    let count = mv_chrome_jobs(nil, 0)
    var ids = [UInt64](repeating: 0, count: Int(max(count, 0)))
    if count > 0 {
      let n = ids.withUnsafeMutableBufferPointer { mv_chrome_jobs($0.baseAddress, count) }
      ids = Array(ids.prefix(Int(min(n, count))))
    }
    var next: [ClipJobRow] = []
    for id in ids {
      var j = mv_chrome_job()
      guard mv_chrome_job_info(id, &j) else { continue }
      var row = ClipJobRow(id: id)
      row.state = j.state
      row.op = j.op
      row.fraction = j.fraction
      row.elapsedMs = j.elapsed_ms
      row.etaMs = j.eta_ms
      row.error = j.error
      row.outputCount = j.output_count
      // Titles and names do not change while a job runs; keep the old strings.
      if let old = rows.first(where: { $0.id == id }), !old.title.isEmpty {
        row.title = old.title
        row.source = old.source
      } else {
        row.title = Self.text(id, 0)
        row.source = Self.text(id, 1)
      }
      next.append(row)
    }
    if next != rows { rows = next }
  }

  func cancel(_ id: UInt64) { mv_chrome_job_cancel(id); reload() }
  func retry(_ id: UInt64) { mv_chrome_job_retry(id); reload() }
  func reveal(_ id: UInt64) { mv_chrome_job_reveal(id) }
  func clearFinished() { mv_chrome_jobs_clear_finished(); reload() }
  func close() { mv_chrome_jobs_close() }
  func blur() { mv_chrome_jobs_blur() }
}
