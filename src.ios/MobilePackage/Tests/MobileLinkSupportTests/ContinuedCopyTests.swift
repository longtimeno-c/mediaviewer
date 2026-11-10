// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileLinkSupport
import XCTest

final class ContinuedCopyTests: XCTestCase {
  private final class FakeTask: ContinuedTaskHandle {
    var completions: [Bool] = []
    var done: Int64 = 0
    func complete(success: Bool) { completions.append(success) }
    func progress(done: Int64, total: Int64) { self.done = done }
  }

  func testHandedOverDuringTheCopy() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.asking()
    book.adopt(task)
    book.progress(done: 5, total: 10)
    book.finished(success: true)
    XCTAssertEqual(task.done, 5)
    XCTAssertEqual(task.completions, [true])
  }

  /// The copy ends (Cancel while waiting for Wi-Fi) before the system hands the task over.
  func testHandedOverAfterTheCopyEnded() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.asking()
    book.finished(success: false)
    book.adopt(task)
    XCTAssertEqual(task.completions, [false], "Nobody would ever complete it")
    book.finished(success: true)  // a late second word changes nothing
    XCTAssertEqual(task.completions, [false])
  }

  func testCompletedExactlyOnce() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.asking()
    book.adopt(task)
    book.finished(success: false)
    book.finished(success: true)
    XCTAssertEqual(task.completions, [false])
  }

  /// The reviewers' probe: the system hands the task over on its own queue before submit returns.
  /// The copy asks first, so the hand-over is this copy's and is never failed by the asking.
  func testHandOverBeforeSubmitReturnsIsKept() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.asking()     // before submit
    book.adopt(task)  // the launch handler, while submit is still running
    book.progress(done: 5, total: 10)
    XCTAssertTrue(task.completions.isEmpty, "Completed while the copy still runs: \(task.completions)")
    book.finished(success: true)
    XCTAssertEqual(task.done, 5)
    XCTAssertEqual(task.completions, [true])
  }

  /// A task left from an earlier copy is completed when the next copy asks, and only that one.
  func testLeftoverFromAnEarlierCopyIsCompletedOnAsking() {
    let book = ContinuedCopy()
    let old = FakeTask()
    book.asking()
    book.adopt(old)
    book.asking()  // a new copy, the old one never finished
    XCTAssertEqual(old.completions, [false])
    let new = FakeTask()
    book.adopt(new)
    book.finished(success: true)
    XCTAssertEqual(new.completions, [true])
    XCTAssertEqual(old.completions, [false])
  }

  /// Expiry during "Waiting for Wi-Fi": the wait ends (as LinkConnection runs it, with the expired
  /// flag as its stop), and the task is completed exactly once.
  func testExpiryDuringTheWiFiWaitCompletesTheTaskOnce() async throws {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.asking()
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
    let began = Date()
    await copy.value
    XCTAssertLessThan(Date().timeIntervalSince(began), 1.0)
    XCTAssertEqual(task.completions, [false])
    book.finished(success: false)
    XCTAssertEqual(task.completions, [false], "Completed once")
  }

  func testNextCopyDoesNotInheritAnEndedOne() {
    let book = ContinuedCopy()
    book.asking()
    book.finished(success: true)
    book.asking()  // a new copy asks before the first hand-over came
    let task = FakeTask()
    book.adopt(task)
    XCTAssertTrue(task.completions.isEmpty, "The new copy is still running")
    book.finished(success: true)
    XCTAssertEqual(task.completions, [true])
  }
}
