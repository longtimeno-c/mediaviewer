// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Forget's gate for the app's own writes (LinkConnection.whileLive): a forgotten or replaced
// library writes nothing, and Forget waits for a write already running.
import Foundation
import MobileLinkSupport
import XCTest

final class WriteGateTests: XCTestCase {
  private final class Library {}

  func testOnlyTheOpenLibraryWrites() {
    let gate = WriteGate()
    let a = Library(), b = Library()
    XCTAssertNil(gate.whileLive(a) { 1 }, "nothing is open yet")
    gate.open(for: a)
    XCTAssertEqual(gate.whileLive(a) { 1 }, 1)
    XCTAssertNil(gate.whileLive(b) { 1 }, "another library's late write")
    gate.close()  // Forget
    XCTAssertNil(gate.whileLive(a) { 1 }, "a write after Forget")
    gate.open(for: b)  // paired again, with another library
    XCTAssertNil(gate.whileLive(a) { 1 }, "the forgotten library's write, after the new pairing")
    XCTAssertEqual(gate.whileLive(b) { 2 }, 2)
  }

  func testForgetWaitsForAWriteAlreadyRunning() {
    let gate = WriteGate()
    let a = Library()
    gate.open(for: a)
    let entered = DispatchSemaphore(value: 0)
    let release = DispatchSemaphore(value: 0)
    let wrote = DispatchSemaphore(value: 0)
    DispatchQueue.global().async {
      _ = gate.whileLive(a) {
        entered.signal()
        release.wait()  // a move into the store, still running
      }
      wrote.signal()
    }
    entered.wait()
    let closed = DispatchSemaphore(value: 0)
    DispatchQueue.global().async {
      gate.close()
      closed.signal()
    }
    XCTAssertEqual(closed.wait(timeout: .now() + 0.3), .timedOut, "Forget went on while a write was running")
    release.signal()
    XCTAssertEqual(closed.wait(timeout: .now() + 5), .success)
    XCTAssertEqual(wrote.wait(timeout: .now() + 5), .success)
    XCTAssertNil(gate.whileLive(a) { 1 })
  }
}
