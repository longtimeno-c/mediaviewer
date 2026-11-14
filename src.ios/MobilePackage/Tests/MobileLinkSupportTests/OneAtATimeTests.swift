// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
import MobileLinkSupport
import XCTest

final class OneAtATimeTests: XCTestCase {
  private final class Record: @unchecked Sendable {
    private let lock = NSLock()
    private var started: [String] = []
    private var running = 0
    private(set) var overlapped = false
    private var flags: Set<String> = []
    func begin(_ name: String) {
      lock.lock(); defer { lock.unlock() }
      started.append(name)
      running += 1
      if running > 1 { overlapped = true }
    }
    func end() { lock.lock(); running -= 1; lock.unlock() }
    func set(_ flag: String) { lock.lock(); flags.insert(flag); lock.unlock() }
    func has(_ flag: String) -> Bool { lock.lock(); defer { lock.unlock() }; return flags.contains(flag) }
    var starts: [String] { lock.lock(); defer { lock.unlock() }; return started }
  }

  /// A body that runs until cancelled (a copy waiting for Wi-Fi), then takes a moment to unwind,
  /// as a copy does while it closes its read and completes its task. A new run supersedes it at
  /// once, and waits that moment.
  private static func untilCancelled(_ name: String, _ record: Record) -> @Sendable () async throws -> Void {
    {
      record.begin(name)
      defer { record.end() }
      do {
        try await Task.sleep(for: .seconds(30))
      } catch {
        // Not Task.sleep: in a cancelled task that returns at once.
        await withCheckedContinuation { (done: CheckedContinuation<Void, Never>) in
          DispatchQueue.global().asyncAfter(deadline: .now() + 0.4) { done.resume() }
        }
        throw error
      }
    }
  }

  /// Three runs in quick succession: never two at once, and the middle one, superseded while it
  /// waited, never starts.
  func testThreeQuickRunsNeverOverlapAndTheMiddleOneNeverStarts() async throws {
    let one = OneAtATime()
    let record = Record()
    let a = Task { try await one.run(Self.untilCancelled("A", record)) }
    try await Task.sleep(for: .milliseconds(100))
    let b = Task { try await one.run { record.begin("B"); record.end() } }
    try await Task.sleep(for: .milliseconds(50))
    let c = Task { try await one.run { record.begin("C"); record.end() } }
    _ = await a.result
    _ = await b.result
    try await c.value
    XCTAssertFalse(record.overlapped)
    XCTAssertEqual(record.starts, ["A", "C"])
  }

  /// A run whose caller is cancelled while it waits for the one before never runs its body, and the
  /// next run still works.
  func testACancelledWaiterNeverRunsAndALaterRunStillWorks() async throws {
    let one = OneAtATime()
    let record = Record()
    let a = Task { try await one.run(Self.untilCancelled("A", record)) }
    try await Task.sleep(for: .milliseconds(100))
    let b = Task { try await one.run { record.begin("B"); record.end() } }  // supersedes A, waits for it to unwind
    try await Task.sleep(for: .milliseconds(50))
    b.cancel()  // the person cancels while it waits
    _ = await a.result  // A has ended: B must not start now
    if case .success = await b.result { XCTFail("A cancelled run reported success") }
    let later = try await one.run { () -> String in record.begin("D"); record.end(); return "done" }
    XCTAssertEqual(later, "done")
    XCTAssertEqual(record.starts, ["A", "D"])
  }

  /// Cancelling the caller reaches the body itself, quickly.
  func testCancellingTheCallerCancelsTheBody() async throws {
    let one = OneAtATime()
    let record = Record()
    let caller = Task {
      try await one.run {
        record.set("started")
        do {
          try await Task.sleep(for: .seconds(5))
        } catch {
          record.set("cancelled")
          throw error
        }
      }
    }
    for _ in 0..<50 where !record.has("started") { try await Task.sleep(for: .milliseconds(10)) }
    let at = Date()
    caller.cancel()
    while !record.has("cancelled") && Date().timeIntervalSince(at) < 2 { try await Task.sleep(for: .milliseconds(20)) }
    XCTAssertTrue(record.has("cancelled"), "The body never saw the caller's cancel")
    XCTAssertLessThan(Date().timeIntervalSince(at), 2)
  }
}
