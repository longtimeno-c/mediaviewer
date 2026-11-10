// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Originals fetched from the NAS, on disk in Caches (docs/plans/mobile.md §10 step 5). One fetch per
// file however many viewers ask; a fetch writes to its own part file, is checked for its full size
// and is moved in only then; the least recently used files go first when the cache is full.
import Foundation

public enum OriginalStoreError: Error, Equatable {
  /// Fewer bytes arrived than the catalogue says the file has.
  case short(expected: UInt64, got: UInt64)
  case notMoved
}

public actor OriginalStore {
  private let dir: URL
  private let limit: Int64
  // One fetch per file; it is cancelled when the last viewer that wanted it has gone.
  private var inFlight: [String: (task: Task<URL, Error>, waiters: Int)] = [:]

  public init(dir: URL, limitBytes: Int64) {
    self.dir = dir
    self.limit = limitBytes
  }

  /// The file named `key`, `size` bytes long: from the cache, from a fetch already running, or from
  /// `fetch`, which writes the whole file to the URL it is given. Cancelling the caller stops the
  /// fetch once no other caller still wants the file.
  public func file(key: String, size: UInt64, fetch: @escaping @Sendable (URL) async throws -> Void) async throws -> URL {
    let target = dir.appendingPathComponent(key)
    if Self.size(of: target) == size {
      Self.touch(target)
      return target
    }
    let task: Task<URL, Error>
    if let running = inFlight[key] {
      task = running.task
      inFlight[key]?.waiters += 1
    } else {
      let dir = self.dir
      task = Task<URL, Error> {
        let part = dir.appendingPathComponent("\(key).\(UUID().uuidString).part")
        defer { try? FileManager.default.removeItem(at: part) }
        try await fetch(part)
        try Task.checkCancellation()
        let got = Self.size(of: part) ?? 0
        guard got == size else { throw OriginalStoreError.short(expected: size, got: got) }
        try? FileManager.default.removeItem(at: target)
        do { try FileManager.default.moveItem(at: part, to: target) } catch { throw OriginalStoreError.notMoved }
        return target
      }
      inFlight[key] = (task, 1)
    }
    let result: Result<URL, Error> = await withTaskCancellationHandler {
      do { return .success(try await task.value) } catch { return .failure(error) }
    } onCancel: {
      Task { await self.leave(key, task: task, cancelled: true) }
    }
    leave(key, task: task, cancelled: false)
    let url = try result.get()
    trim()
    return url
  }

  /// One caller is done with `key`. The last to leave by cancelling stops the fetch.
  private func leave(_ key: String, task: Task<URL, Error>, cancelled: Bool) {
    guard var entry = inFlight[key], entry.task == task else { return }
    if cancelled {
      entry.waiters -= 1
      if entry.waiters <= 0 {
        task.cancel()
        inFlight[key] = nil
      } else {
        inFlight[key] = entry
      }
    } else {
      inFlight[key] = nil  // finished: callers still waiting hold the task and read its result
    }
  }

  /// Oldest first, until the cache is under its limit. Files being written are not counted.
  public func trim() {
    let keys: [URLResourceKey] = [.contentModificationDateKey, .fileSizeKey]
    guard let names = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: keys) else { return }
    var entries: [(url: URL, date: Date, size: Int64)] = []
    var total: Int64 = 0
    for url in names where url.pathExtension != "part" {
      guard let values = try? url.resourceValues(forKeys: Set(keys)) else { continue }
      let size = Int64(values.fileSize ?? 0)
      total += size
      entries.append((url, values.contentModificationDate ?? .distantPast, size))
    }
    for entry in entries.sorted(by: { $0.date < $1.date }) where total > limit {
      try? FileManager.default.removeItem(at: entry.url)
      total -= entry.size
    }
  }

  static func size(of url: URL) -> UInt64? {
    guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
          let size = attributes[.size] as? NSNumber else { return nil }
    return size.uint64Value
  }

  static func touch(_ url: URL) {
    try? FileManager.default.setAttributes([.modificationDate: Date()], ofItemAtPath: url.path)
  }
}
