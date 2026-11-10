// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import MobileLinkSupport
import XCTest

final class ContinuedCopyTests: XCTestCase {
  private final class FakeTask: ContinuedTaskHandle, @unchecked Sendable {
    private let lock = NSLock()
    private var log: [Bool] = []
    private var latest: Int64 = 0
    func complete(success: Bool) { lock.lock(); log.append(success); lock.unlock() }
    func progress(done: Int64, total: Int64) { lock.lock(); latest = done; lock.unlock() }
    var completions: [Bool] { lock.lock(); defer { lock.unlock() }; return log }
    var done: Int64 { lock.lock(); defer { lock.unlock() }; return latest }
  }
  private struct Refused: Error {}

  func testHandedOverDuringTheCopy() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.submit {}
    book.adopt(task)
    book.progress(done: 5, total: 10)
    book.finished(success: true)
    XCTAssertEqual(task.done, 5)
    XCTAssertEqual(task.completions, [true])
  }

  /// The original bug: the system hands the task over on its own queue before submit returns.
  /// The copy must already own it then (fails if the copy registers after the request).
  func testHandOverBeforeSubmitReturnsIsKept() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.submit { book.adopt(task) }  // the launch handler runs inside submit
    book.progress(done: 5, total: 10)
    XCTAssertTrue(task.completions.isEmpty, "Completed while the copy still runs: \(task.completions)")
    book.finished(success: true)
    XCTAssertEqual(task.done, 5)
    XCTAssertEqual(task.completions, [true])
  }

  /// The copy ends (Cancel while waiting for Wi-Fi) before the system hands the task over.
  func testHandedOverAfterTheCopyEnded() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.submit {}
    book.finished(success: false)
    book.adopt(task)
    XCTAssertEqual(task.completions, [false], "Nobody would ever complete it")
    book.finished(success: true)  // a late second word changes nothing
    XCTAssertEqual(task.completions, [false])
  }

  /// The review's repro: a refused request, then a late hand-over from the earlier one.
  func testLateHandOverAfterARefusedRequestIsCompleted() {
    let book = ContinuedCopy()
    book.submit {}
    book.finished(success: false)
    XCTAssertFalse(book.submit { throw Refused() })
    book.finished(success: true)
    let late = FakeTask()
    book.adopt(late)
    XCTAssertEqual(late.completions.count, 1, "A hand-over nobody owns is completed at once")
  }

  /// A hand-over when no copy has ever run is completed at once too.
  func testHandOverWithNoCopyIsCompletedAtOnce() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.adopt(task)
    XCTAssertEqual(task.completions, [false])
    book.expire()  // its expiry later finds nothing to complete twice
    XCTAssertEqual(task.completions, [false])
  }

  func testLeftoverFromAnEarlierCopyIsCompletedWhenTheNextStarts() {
    let book = ContinuedCopy()
    let old = FakeTask()
    book.submit {}
    book.adopt(old)
    book.submit {}  // a new copy, the old one never finished
    XCTAssertEqual(old.completions, [false])
    let new = FakeTask()
    book.adopt(new)
    book.finished(success: true)
    XCTAssertEqual(new.completions, [true])
    XCTAssertEqual(old.completions, [false])
  }

  /// An expiry belongs to the copy it hit: the next copy starts unexpired.
  func testANewCopyStartsUnexpired() {
    let book = ContinuedCopy()
    book.submit {}
    book.adopt(FakeTask())
    book.expire()
    XCTAssertTrue(book.isExpired)
    book.finished(success: false)
    book.submit {}
    XCTAssertFalse(book.isExpired)
  }

  /// The gate never skips a stop, even when bytes may already move.
  func testTheGateStopsWhenBytesMayAlreadyMove() async {
    do {
      try await CopyGate.wait(mayMove: { true }, stopped: { true }, waiting: {})
      XCTFail("An expiry or a Forget was skipped")
    } catch {}
  }

  /// Expiry during "Waiting for Wi-Fi" ends the wait and completes the task exactly once. Bounded:
  /// a regression fails in a second instead of hanging.
  func testExpiryDuringTheWiFiWaitCompletesTheTaskOnce() async throws {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.submit {}
    book.adopt(task)
    let copy = Task {
      var succeeded = false
      defer { book.finished(success: succeeded) }
      do {
        try await CopyGate.wait(mayMove: { false }, stopped: { book.isExpired }, waiting: {}, poll: .milliseconds(20))
        succeeded = true
      } catch {}
    }
    try await Task.sleep(for: .milliseconds(100))
    XCTAssertTrue(task.completions.isEmpty)
    book.expire()
    for _ in 0..<50 where task.completions.isEmpty { try await Task.sleep(for: .milliseconds(20)) }
    if task.completions.isEmpty {
      copy.cancel()  // so the test ends
      await copy.value
      XCTFail("Expiry did not end the wait")
      return
    }
    await copy.value
    XCTAssertEqual(task.completions, [false])
    book.finished(success: false)
    XCTAssertEqual(task.completions, [false], "Completed once")
  }
}
