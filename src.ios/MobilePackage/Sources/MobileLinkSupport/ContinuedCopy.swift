// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The bookkeeping between a first copy and the system's continued-processing task (iOS 26,
// docs/plans/mobile.md §8.4). The system hands the task over some time after it was asked for,
// maybe after the copy already ended; whichever comes second completes it, so it never waits forever.
import Foundation

/// The system's task, or a test's stand-in.
public protocol ContinuedTaskHandle: AnyObject {
  func complete(success: Bool)
  func progress(done: Int64, total: Int64)
}

public final class ContinuedCopy: @unchecked Sendable {
  private enum State {
    case idle
    case asked                        // submitted; the system has not handed it over yet
    case running(ContinuedTaskHandle)
    case ended(success: Bool)          // the copy ended before the hand-over
  }
  private let lock = NSLock()
  private var state = State.idle
  private var expiredFlag = false

  public init() {}

  /// Called before submitting the request: the system may hand the task over on its own queue
  /// before submit even returns, and that hand-over must find this copy waiting for it. A task
  /// still held from an earlier copy is completed here, never one adopted for this copy.
  public func asking() {
    lock.lock()
    var leftover: ContinuedTaskHandle?
    if case .running(let old) = state { leftover = old }
    state = .asked
    expiredFlag = false
    lock.unlock()
    leftover?.complete(success: false)
  }

  /// The request was refused: nothing will be handed over for it.
  public func submitFailed() {
    lock.lock()
    defer { lock.unlock() }
    if case .asked = state { state = .idle }
  }

  /// The system took the time back. Whoever runs the copy stops at once (the Wi-Fi wait included)
  /// and then calls `finished`, which completes the task, as Apple requires even after expiry.
  public func expire() {
    lock.lock()
    defer { lock.unlock() }
    expiredFlag = true
  }

  public var isExpired: Bool {
    lock.lock()
    defer { lock.unlock() }
    return expiredFlag
  }

  /// The system's hand-over. Completed at once when the copy has already ended.
  public func adopt(_ task: ContinuedTaskHandle) {
    lock.lock()
    let ended: Bool?
    switch state {
    case .ended(let success):
      ended = success
      state = .idle
    case .asked, .idle:
      ended = nil
      state = .running(task)
    case .running(let old):
      ended = nil
      old.complete(success: false)
      state = .running(task)
    }
    lock.unlock()
    if let ended { task.complete(success: ended) }
  }

  public func progress(done: Int64, total: Int64) {
    lock.lock()
    defer { lock.unlock() }
    if case .running(let task) = state { task.progress(done: done, total: total) }
  }

  /// The copy ended, however it ended.
  public func finished(success: Bool) {
    lock.lock()
    var task: ContinuedTaskHandle?
    switch state {
    case .running(let running):
      task = running
      state = .idle
    case .asked:
      state = .ended(success: success)  // the hand-over may still come
    case .idle, .ended:
      break
    }
    lock.unlock()
    task?.complete(success: success)
  }
}
