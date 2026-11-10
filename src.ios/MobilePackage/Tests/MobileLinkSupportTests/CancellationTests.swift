// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import MobileLinkSupport
import XCTest

final class CancellationTests: XCTestCase {
  private final class Flag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false
    func set() { lock.lock(); value = true; lock.unlock() }
    var isSet: Bool { lock.lock(); defer { lock.unlock() }; return value }
  }

  /// Leaving the viewer stops the download nobody else wants.
  func testLastViewerLeavingStopsTheFetch() async throws {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: dir) }
    let store = OriginalStore(dir: dir, limitBytes: 1 << 30)
    let sawCancel = Flag()
    let fetch: @Sendable (URL) async throws -> Void = { _ in
      do {
        try await Task.sleep(for: .seconds(30))  // a large RAW over a slow link
      } catch {
        sawCancel.set()
        throw error
      }
    }
    let first = Task { try await store.file(key: "1-9", size: 9, fetch: fetch) }
    let second = Task { try await store.file(key: "1-9", size: 9, fetch: fetch) }
    try await Task.sleep(for: .milliseconds(200))
    first.cancel()
    try await Task.sleep(for: .milliseconds(300))
    XCTAssertFalse(sawCancel.isSet, "Another viewer still wanted it")
    second.cancel()
    for _ in 0..<40 where !sawCancel.isSet { try await Task.sleep(for: .milliseconds(50)) }
    XCTAssertTrue(sawCancel.isSet, "The fetch never saw the cancel")
  }

  /// Forget during "Waiting for Wi-Fi" returns at once instead of waiting for Wi-Fi.
  func testStopAndWaitEndsAWaitThatWouldNeverEnd() async throws {
    let work = TrackedWork()
    let started = Flag()
    let task = work.run {
      started.set()
      // The wait loop: only cancellation ends it here, as when the phone stays on mobile data.
      while true {
        do { try await Task.sleep(for: .milliseconds(100)) } catch { return }
      }
    }
    XCTAssertNotNil(task)
    for _ in 0..<20 where !started.isSet { try await Task.sleep(for: .milliseconds(10)) }
    let began = Date()
    await work.stopAndWait()
    XCTAssertLessThan(Date().timeIntervalSince(began), 1.0)
    XCTAssertNil(work.run { }, "Nothing starts again until reopened")
    work.reopen()
    XCTAssertNotNil(work.run { })
  }
}
