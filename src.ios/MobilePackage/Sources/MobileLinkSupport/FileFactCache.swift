// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File mode's tag cache (docs/plans/phone-in-the-app.md §4): each file's search fields, kept by
// path, size and mtime (`FilePath.stamp`), so a folder seen before searches at once and reads no
// file again; a file changed in place has another stamp and is read afresh. In memory, and in one
// file in the library's caches (excluded from backup with the folder it is in). Thread-safe;
// `load` and `save` touch the disk, so never on the main thread.
import Foundation

/// What file search keeps of one file: its fields in the desktop's why-order, and the two the
/// viewer's details show.
public struct FileFacts: Codable, Hashable, Sendable {
  public let fields: [String]
  public let camera: String
  public let lens: String

  public init(fields: [String], camera: String, lens: String) {
    self.fields = fields
    self.camera = camera
    self.lens = lens
  }
}

public final class FileFactCache: @unchecked Sendable {
  private let file: URL
  private let limit: Int
  private let lock = NSLock()
  private var fields: [String: FileFacts] = [:]
  private var loaded = false
  private var dirty = false

  /// `limit`: entries kept; past it, the cache starts again rather than grow without end.
  public init(file: URL, limit: Int = 100_000) {
    self.file = file
    self.limit = limit
  }

  /// Reads the file once.
  public func load() {
    let already = lock.withLock { () -> Bool in
      defer { loaded = true }
      return loaded
    }
    guard !already else { return }
    guard let data = try? Data(contentsOf: file),
          let stored = try? JSONDecoder().decode([String: FileFacts].self, from: data) else { return }
    lock.withLock {
      // What was learnt before the file was read wins.
      fields.merge(stored) { mine, _ in mine }
    }
  }

  public func facts(for stamp: String) -> FileFacts? { lock.withLock { fields[stamp] } }

  public func set(_ value: FileFacts, for stamp: String) {
    lock.withLock {
      if fields.count >= limit, fields[stamp] == nil { fields.removeAll(keepingCapacity: true) }
      fields[stamp] = value
      dirty = true
    }
  }

  public var count: Int { lock.withLock { fields.count } }

  /// Writes the file when something changed. `write` runs the write (so the caller can refuse it
  /// for a library that has gone); false from it leaves the cache dirty.
  public func save(write: (() throws -> Void) throws -> Bool = { body in try body(); return true }) {
    let snapshot: [String: FileFacts]? = lock.withLock {
      guard dirty else { return nil }
      dirty = false
      return fields
    }
    guard let snapshot, let data = try? JSONEncoder().encode(snapshot) else { return }
    let file = self.file
    let written = (try? write {
      try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
      try data.write(to: file, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
    }) ?? false
    if !written { lock.withLock { dirty = true } }
  }

  /// Forget.
  public func removeAll() {
    lock.withLock {
      fields.removeAll()
      dirty = false
    }
  }
}

