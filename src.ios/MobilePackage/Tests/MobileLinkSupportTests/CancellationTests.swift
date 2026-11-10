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
    let cancelledAt = Date()
    first.cancel()
    // The viewer that left gets its answer at once, not when the download ends.
    switch await first.result {
    case .success: XCTFail("A cancelled caller got the file")
    case .failure(let error): XCTAssertTrue(error is CancellationError, "\(error)")
    }
    XCTAssertLessThan(Date().timeIntervalSince(cancelledAt), 0.5)
    try await Task.sleep(for: .milliseconds(300))
    XCTAssertFalse(sawCancel.isSet, "Another viewer still wanted it")
    second.cancel()
    for _ in 0..<40 where !sawCancel.isSet { try await Task.sleep(for: .milliseconds(50)) }
    XCTAssertTrue(sawCancel.isSet, "The fetch never saw the cancel")
  }

  /// LinkConnection's "Waiting for Wi-Fi" loop (CopyGate) run as the first copy runs it, under
  /// TrackedWork: Forget (stopAndWait, and the forgotten flag) ends it at once, and nothing
  /// copies after.
  func testForgetEndsTheWaitForWiFi() async throws {
    let work = TrackedWork()
    let forgotten = Flag()
    let waited = Flag()
    let copied = Flag()
    let ended = Flag()
    _ = work.run {
      do {
        try await CopyGate.wait(mayMove: { false }, stopped: { forgotten.isSet }, waiting: { waited.set() },
                                poll: .milliseconds(50))
        copied.set()
      } catch {
        ended.set()
      }
    }
    for _ in 0..<40 where !waited.isSet { try await Task.sleep(for: .milliseconds(10)) }
    XCTAssertTrue(waited.isSet, "It never said it was waiting for Wi-Fi")
    let began = Date()
    forgotten.set()
    await work.stopAndWait()
    XCTAssertLessThan(Date().timeIntervalSince(began), 1.0)
    XCTAssertTrue(ended.isSet)
    XCTAssertFalse(copied.isSet)

    // The forgotten flag alone ends it too, as when Forget runs between two polls.
    let alone = Flag()
    let stillForgotten = Flag()
    stillForgotten.set()
    do {
      try await CopyGate.wait(mayMove: { false }, stopped: { stillForgotten.isSet }, waiting: {}, poll: .milliseconds(50))
    } catch {
      alone.set()
    }
    XCTAssertTrue(alone.isSet)
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
