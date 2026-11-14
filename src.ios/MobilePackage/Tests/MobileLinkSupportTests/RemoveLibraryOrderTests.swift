// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// LinkConnection.removeLibrary's order (Forget, or pairing over a library) with the same pieces it
// uses: mark forgotten, fire the copy's cancel, stop and await every tracked writer, then wipe.
// The writers stand in for the quiet sync (no cancel of its own: it stops on `forgotten` or
// cancellation) and two first copies queued one at a time, both blocked mid-copy.
import Foundation
import MobileLinkSupport
import XCTest

final class RemoveLibraryOrderTests: XCTestCase {
  private final class Library: @unchecked Sendable {
    private let lock = NSLock()
    private var wiped = false
    private var writesAfterWipe = 0
    private var active = 0
    var forgotten = false
    var copyCancelled = false

    func write() {
      lock.withLock { if wiped { writesAfterWipe += 1 } }
    }
    func enter() { lock.withLock { active += 1 } }
    func leave() { lock.withLock { active -= 1 } }
    func wipe() { lock.withLock { wiped = true } }
    var running: Int { lock.withLock { active } }
    var lateWrites: Int { lock.withLock { writesAfterWipe } }
    var shouldStop: Bool { lock.withLock { forgotten || copyCancelled } }
    func stop(cancelCopy: Bool) { lock.withLock { forgotten = true; copyCancelled = cancelCopy } }
  }

  /// Writes a block at a time until told to stop, as a copy does between ranges.
  private static func writer(_ library: Library, started: XCTestExpectation) async {
    library.enter()
    defer { library.leave() }
    started.fulfill()
    while !library.shouldStop && !Task.isCancelled {
      library.write()
      try? await Task.sleep(for: .milliseconds(5))
    }
    library.write()  // the last block in flight when the stop landed, before the wipe
  }

  func testNothingWritesAfterTheWipeAndNothingNewStarts() async throws {
    let library = Library()
    let work = TrackedWork()
    let oneCopy = OneAtATime()

    // The quiet sync: tracked, no cancel of its own.
    let quietStarted = expectation(description: "quiet sync running")
    work.run(priority: .utility) { await Self.writer(library, started: quietStarted) }
    // Two first copies, one at a time: whichever gets in first, the other waits behind it.
    let copyRunning = expectation(description: "a first copy running")
    copyRunning.assertForOverFulfill = false
    work.run { _ = try? await oneCopy.run { await Self.writer(library, started: copyRunning) } }
    work.run { _ = try? await oneCopy.run { await Self.writer(library, started: copyRunning) } }
    await fulfillment(of: [quietStarted, copyRunning], timeout: 2)

    // removeLibrary's order.
    library.stop(cancelCopy: true)
    await work.stopAndWait()
    XCTAssertEqual(library.running, 0, "every writer has ended before the wipe")
    library.wipe()

    XCTAssertNil(work.run { library.write() }, "new work is refused while stopped")
    try await Task.sleep(for: .milliseconds(100))
    XCTAssertEqual(library.lateWrites, 0, "nothing is written after the wipe")
  }
}
