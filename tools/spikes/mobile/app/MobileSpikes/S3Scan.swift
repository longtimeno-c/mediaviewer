// M1 spike S3 (throwaway): the int8 dot-product scan on synthetic rows.
//
// Rows are the L/14 shape: 768 bytes. "heap" also holds the 29 bytes a row costs beside its vector
// in vector_store (vectors.h says 797 MB at 1M), so its footprint is the reader's. "mmap" reads a
// flat file, the M12 idea: clean file-backed pages. Neither runs the gates or the ranking, so a
// pass here is a floor for a pass of a real query.
import Darwin
import Foundation

enum S3Scan {
  static let dim = 768
  static let sideBytesPerRow = 29
  static let seed: UInt64 = 0x4D56_5331

  private static func kernels() -> [(String, mv_kernel)] {
    var list: [(String, mv_kernel)] = [("desktop_copy", MV_KERNEL_DESKTOP_COPY), ("neon", MV_KERNEL_NEON)]
    if mv_has_dotprod() != 0 { list.append(("dotprod", MV_KERNEL_DOTPROD)) }
    return list
  }

  static func cacheFile(rows: Int) -> URL {
    FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
      .appendingPathComponent("s3-\(rows).bin")
  }

  static func deleteCacheFiles() {
    for rows in [100_000, 1_000_000] { try? FileManager.default.removeItem(at: cacheFile(rows: rows)) }
  }

  /// Writes the rows in 64 MB pieces, so writing them never costs the footprint it is here to save.
  private static func writeFile(_ url: URL, bytes: Int) throws -> Double {
    let t0 = Measure.nowMs()
    FileManager.default.createFile(atPath: url.path, contents: nil)
    let handle = try FileHandle(forWritingTo: url)
    defer { try? handle.close() }
    let piece = 64 * 1024 * 1024
    var left = bytes
    var round: UInt64 = 0
    while left > 0 {
      try autoreleasepool {
        let n = min(piece, left)
        var data = Data(count: n)
        data.withUnsafeMutableBytes {
          mv_fill_rows($0.bindMemory(to: Int8.self).baseAddress!, n, seed &+ round)
        }
        try handle.write(contentsOf: data)
        left -= n
        round += 1
      }
    }
    return Measure.nowMs() - t0
  }

  private static func scanAll(_ rows: UnsafePointer<Int8>, count: Int, q: UnsafePointer<Int8>,
                              kernel: mv_kernel, threads: Int) -> (best: Int32, hits: Int) {
    let cap = 50
    let chunk = max(1, count / threads)
    let tops = UnsafeMutablePointer<mv_hit>.allocate(capacity: cap * threads)
    defer { tops.deallocate() }
    let kept = UnsafeMutablePointer<Int>.allocate(capacity: threads)
    defer { kept.deallocate() }
    DispatchQueue.concurrentPerform(iterations: threads) { i in
      let first = i * chunk
      let n = i == threads - 1 ? count - first : chunk
      kept[i] = mv_scan(rows, first, n, dim, q, kernel, tops + i * cap, cap)
    }
    var best = Int32.min
    var hits = 0
    for i in 0..<threads {
      hits += kept[i]
      for j in 0..<kept[i] { best = max(best, tops[i * cap + j].score) }
    }
    return (best, hits)
  }

  static func run(rows: Int, mode: String, passes: Int, trigger: String) {
    let report = Report.shared
    let bytes = rows * dim
    report.enter("s3.\(mode).\(rows).load")
    let before = Measure.footprintMB()
    var prepareMs = 0.0
    var fileReused = false
    var base: UnsafeMutablePointer<Int8>?
    var side: UnsafeMutableRawPointer?
    var mapped: UnsafeMutableRawPointer?

    if mode == "heap" {
      let t0 = Measure.nowMs()
      guard let p = malloc(bytes)?.assumingMemoryBound(to: Int8.self) else {
        report.leave()
        report.emit("s3", ["mode": mode, "rows": rows, "error": "malloc failed"])
        return
      }
      mv_fill_rows(p, bytes, seed)
      let sideBytes = rows * sideBytesPerRow
      if let s = malloc(sideBytes) {
        mv_fill_rows(s.assumingMemoryBound(to: Int8.self), sideBytes, seed &+ 1)
        side = s
      }
      base = p
      prepareMs = Measure.nowMs() - t0
    } else {
      let url = cacheFile(rows: rows)
      do {
        let existing = (try? FileManager.default.attributesOfItem(atPath: url.path)[.size] as? Int) ?? 0
        if existing == bytes {
          fileReused = true
        } else {
          prepareMs = try writeFile(url, bytes: bytes)
        }
        let fd = open(url.path, O_RDONLY)
        guard fd >= 0 else { throw NSError(domain: "S3", code: Int(errno)) }
        defer { close(fd) }
        let m = mmap(nil, bytes, PROT_READ, MAP_FILE | MAP_PRIVATE, fd, 0)
        guard let m, m != MAP_FAILED else { throw NSError(domain: "S3", code: Int(errno)) }
        madvise(m, bytes, MADV_SEQUENTIAL)
        mapped = m
        base = m.assumingMemoryBound(to: Int8.self)
      } catch {
        report.leave()
        report.emit("s3", ["mode": mode, "rows": rows, "error": "\(error)"])
        return
      }
    }
    defer {
      if mode == "heap" {
        free(base)
        free(side)
      } else if let mapped {
        munmap(mapped, bytes)
      }
    }
    guard let rowsPtr = base else { return }
    let afterLoad = Measure.footprintMB()

    var query = [Int8](repeating: 0, count: dim)
    query.withUnsafeMutableBufferPointer { mv_fill_rows($0.baseAddress!, dim, seed &+ 99) }

    // All three kernels must agree, or their timings mean nothing.
    let a = query.withUnsafeBufferPointer { q in
      kernels().map { mv_dot($0.1, rowsPtr, q.baseAddress!, dim) }
    }

    let cores = Measure.performanceCores()
    for threads in Array(Set([1, cores])).sorted() {
      for (name, kernel) in kernels() {
        report.enter("s3.\(mode).\(rows).\(name).t\(threads)")
        var times: [Double] = []
        var best: Int32 = 0
        for _ in 0..<passes {
          let t0 = Measure.nowMs()
          let r = query.withUnsafeBufferPointer {
            scanAll(rowsPtr, count: rows, q: $0.baseAddress!, kernel: kernel, threads: threads)
          }
          times.append(Measure.nowMs() - t0)
          best = r.best
        }
        report.leave()
        // After the scan, not after the load: a mapped file has touched no page until it is read.
        let afterScan = Measure.footprintMB()
        report.emit("s3", [
          "trigger": trigger,
          "mode": mode,
          "rows": rows,
          "dim": dim,
          "kernel": name,
          "threads": threads,
          "passes": passes,
          "pass_first_ms": Measure.rounded(times.first ?? -1),
          "pass_p50_ms": Measure.rounded(Measure.percentile(times, 0.5)),
          "pass_min_ms": Measure.rounded(times.min() ?? -1),
          "pass_max_ms": Measure.rounded(times.max() ?? -1),
          "prepare_ms": Measure.rounded(prepareMs),
          "file_reused": fileReused,
          "best_score": Int(best),
          "kernels_agree": Set(a).count == 1,
          "has_dotprod": mv_has_dotprod() != 0,
          "mb_before": Measure.rounded(before.now),
          "mb_after_load": Measure.rounded(afterLoad.now),
          "mb_after_scan": Measure.rounded(afterScan.now),
          "mb_peak_after_scan": Measure.rounded(afterScan.peak),
        ])
      }
    }
  }
}
