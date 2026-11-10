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
    book.asked()
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
    book.asked()
    book.finished(success: false)
    book.adopt(task)
    XCTAssertEqual(task.completions, [false], "Nobody would ever complete it")
    book.finished(success: true)  // a late second word changes nothing
    XCTAssertEqual(task.completions, [false])
  }

  func testCompletedExactlyOnce() {
    let book = ContinuedCopy()
    let task = FakeTask()
    book.asked()
    book.adopt(task)
    book.finished(success: false)
    book.finished(success: true)
    XCTAssertEqual(task.completions, [false])
  }

  func testNextCopyDoesNotInheritAnEndedOne() {
    let book = ContinuedCopy()
    book.asked()
    book.finished(success: true)
    book.asked()  // a new copy asks before the first hand-over came
    let task = FakeTask()
    book.adopt(task)
    XCTAssertTrue(task.completions.isEmpty, "The new copy is still running")
    book.finished(success: true)
    XCTAssertEqual(task.completions, [true])
  }
}
