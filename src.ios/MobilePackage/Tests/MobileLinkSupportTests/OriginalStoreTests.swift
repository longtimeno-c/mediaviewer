// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import MobileLinkSupport
import XCTest

final class OriginalStoreTests: XCTestCase {
  private var dir: URL!

  override func setUp() {
    dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
  }

  override func tearDown() { try? FileManager.default.removeItem(at: dir) }

  private final class Counter: @unchecked Sendable {
    private let lock = NSLock()
    private var n = 0
    func bump() { lock.lock(); n += 1; lock.unlock() }
    var value: Int { lock.lock(); defer { lock.unlock() }; return n }
  }

  /// Two viewers of one photo share one fetch, and neither sees a file with a hole in it.
  func testConcurrentAsksShareOneFetch() async throws {
    let store = OriginalStore(dir: dir, limitBytes: 1 << 30)
    let calls = Counter()
    let bytes = Data(repeating: 7, count: 300_000)
    let fetch: @Sendable (URL) async throws -> Void = { part in
      calls.bump()
      // Written in pieces, slowly, as the link delivers them.
      FileManager.default.createFile(atPath: part.path, contents: nil)
      let handle = try FileHandle(forWritingTo: part)
      for start in stride(from: 0, to: bytes.count, by: 50_000) {
        try handle.write(contentsOf: bytes[start..<min(start + 50_000, bytes.count)])
        try await Task.sleep(for: .milliseconds(20))
      }
      try handle.close()
    }
    async let a = store.file(key: "7-300000", size: 300_000, fetch: fetch)
    async let b = store.file(key: "7-300000", size: 300_000, fetch: fetch)
    let (first, second) = try await (a, b)
    XCTAssertEqual(first, second)
    XCTAssertEqual(calls.value, 1)
    XCTAssertEqual(try Data(contentsOf: first), bytes)
  }

  /// A fetch cut short is never cached.
  func testShortFetchIsRefused() async {
    let store = OriginalStore(dir: dir, limitBytes: 1 << 30)
    do {
      _ = try await store.file(key: "8-1000", size: 1000) { part in
        FileManager.default.createFile(atPath: part.path, contents: Data(count: 400))
      }
      XCTFail("A short file was accepted")
    } catch {
      XCTAssertEqual(error as? OriginalStoreError, .short(expected: 1000, got: 400))
    }
    let left = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
    XCTAssertTrue(left.isEmpty, "\(left)")
  }

  /// The least recently used goes first, and opening a file counts as using it.
  func testEvictsOldestAndTouchesOnHit() async throws {
    let store = OriginalStore(dir: dir, limitBytes: 2500)
    func put(_ key: String) async throws -> URL {
      try await store.file(key: key, size: 1000) { FileManager.default.createFile(atPath: $0.path, contents: Data(count: 1000)) }
    }
    let old = try await put("a")
    try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSinceNow: -300)], ofItemAtPath: old.path)
    let middle = try await put("b")
    try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSinceNow: -200)], ofItemAtPath: middle.path)
    _ = try await put("a")  // a hit: now the newest
    _ = try await put("c")  // over the limit: "b" is the oldest
    XCTAssertTrue(FileManager.default.fileExists(atPath: old.path))
    XCTAssertFalse(FileManager.default.fileExists(atPath: middle.path))
  }
}

/// Forget refuses the move into the store: the caller hears CancellationError and nothing is left.
final class OriginalStoreRefusedTests: XCTestCase {
  func testARefusedMoveLeavesNothing() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("refused-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let store = OriginalStore(dir: dir, limitBytes: 1 << 20)
    do {
      _ = try await store.file(key: "a", size: 4, place: { _ in false }) { part in
        try Data([1, 2, 3, 4]).write(to: part)
      }
      XCTFail("a refused move must not succeed")
    } catch is CancellationError {
    }
    let left = try FileManager.default.contentsOfDirectory(atPath: dir.path)
    XCTAssertEqual(left, [])
  }
}
