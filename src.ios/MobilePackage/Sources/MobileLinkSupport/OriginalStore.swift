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
  // One fetch per file. Each caller waits on a continuation of its own, so a caller that is
  // cancelled returns at once; the fetch stops when the last caller has gone.
  // `token` names one fetch: an old fetch winding down after its callers left must never answer
  // the callers of a newer one for the same file.
  private struct Entry {
    let token: UUID
    var task: Task<Void, Never>
    var waiters: [UUID: CheckedContinuation<URL, Error>] = [:]
  }
  private var inFlight: [String: Entry] = [:]

  public init(dir: URL, limitBytes: Int64) {
    self.dir = dir
    self.limit = limitBytes
  }

  /// The file named `key`, `size` bytes long: from the cache, from a fetch already running, or from
  /// `fetch`, which writes the whole file to the URL it is given. A cancelled caller gets
  /// CancellationError at once; the fetch is cancelled when no caller still wants the file.
  /// `place` runs the move into the store, or refuses it (false: the library is gone; the caller hears
  /// CancellationError and nothing is left behind).
  public func file(key: String, size: UInt64,
                   place: @escaping @Sendable (_ move: () throws -> Void) throws -> Bool = { move in try move(); return true },
                   fetch: @escaping @Sendable (URL) async throws -> Void) async throws -> URL {
    let target = dir.appendingPathComponent(key)
    if Self.size(of: target) == size {
      Self.touch(target)
      return target
    }
    if inFlight[key] == nil {
      let dir = self.dir
      let token = UUID()
      let task = Task {
        let result: Result<URL, Error>
        do {
          let part = dir.appendingPathComponent("\(key).\(UUID().uuidString).part")
          defer { try? FileManager.default.removeItem(at: part) }
          try await fetch(part)
          try Task.checkCancellation()
          let got = Self.size(of: part) ?? 0
          guard got == size else { throw OriginalStoreError.short(expected: size, got: got) }
          let placed: Bool
          do {
            placed = try place {
              try? FileManager.default.removeItem(at: target)
              try FileManager.default.moveItem(at: part, to: target)
            }
          } catch {
            throw OriginalStoreError.notMoved
          }
          guard placed else { throw CancellationError() }
          result = .success(target)
        } catch {
          result = .failure(error)
        }
        await self.finish(key, token, result)
      }
      inFlight[key] = Entry(token: token, task: task)
    }
    let id = UUID()
    let url = try await withTaskCancellationHandler {
      try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<URL, Error>) in
        register(key, id, continuation, target: target, size: size)
      }
    } onCancel: {
      Task { await self.cancel(key, id) }
    }
    trim()
    return url
  }

  // Runs inside withCheckedThrowingContinuation, in the same turn of the actor as file(): a cancel's
  // Task needs the actor too, so it always finds this caller registered or already answered.
  private func register(_ key: String, _ id: UUID, _ continuation: CheckedContinuation<URL, Error>, target: URL, size: UInt64) {
    guard inFlight[key] != nil else {
      // Finished between the start and now: the file is there, or the fetch failed.
      if Self.size(of: target) == size {
        continuation.resume(returning: target)
      } else {
        continuation.resume(throwing: OriginalStoreError.notMoved)
      }
      return
    }
    inFlight[key]?.waiters[id] = continuation
  }

  private func cancel(_ key: String, _ id: UUID) {
    // Already answered (the fetch ended first): nothing to do, and nothing to remember.
    guard let continuation = inFlight[key]?.waiters.removeValue(forKey: id) else { return }
    continuation.resume(throwing: CancellationError())
    leaveIfEmpty(key)
  }

  private func leaveIfEmpty(_ key: String) {
    guard let entry = inFlight[key], entry.waiters.isEmpty else { return }
    entry.task.cancel()
    inFlight[key] = nil
  }

  private func finish(_ key: String, _ token: UUID, _ result: Result<URL, Error>) {
    guard inFlight[key]?.token == token, let entry = inFlight.removeValue(forKey: key) else { return }
    for continuation in entry.waiters.values { continuation.resume(with: result) }
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
