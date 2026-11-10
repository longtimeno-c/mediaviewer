// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The bookkeeping between a first copy and the system's continued-processing task (iOS 26,
// docs/plans/mobile.md §8.4). One rule: a task that no live copy owns is completed at once. A copy
// is live from `submit` to `finished`; a hand-over while one is live belongs to it and is completed
// when it finishes; a hand-over at any other time is completed as it arrives.
import Foundation

/// The system's task, or a test's stand-in.
public protocol ContinuedTaskHandle: AnyObject {
  func complete(success: Bool)
  func progress(done: Int64, total: Int64)
}

public final class ContinuedCopy: @unchecked Sendable {
  private let lock = NSLock()
  private var live = false                  // a copy runs and owns any hand-over
  private var held: ContinuedTaskHandle?    // only ever while `live`
  private var lastOutcome: Bool?            // the latest copy's, for a hand-over that comes late
  private var expiredFlag = false

  public init() {}

  /// A copy starts: it becomes live first, then asks, because the system may hand the task over on
  /// its own queue before `request` even returns. A refused request changes nothing: the copy still
  /// runs, only in the foreground. Returns whether the request was accepted.
  @discardableResult
  public func submit(_ request: () throws -> Void) -> Bool {
    lock.lock()
    let leftover = held
    held = nil
    live = true
    expiredFlag = false
    lastOutcome = nil
    lock.unlock()
    leftover?.complete(success: false)  // an earlier copy that never finished
    do {
      try request()
      return true
    } catch {
      return false
    }
  }

  /// The system's hand-over.
  public func adopt(_ task: ContinuedTaskHandle) {
    lock.lock()
    if live {
      let previous = held
      held = task
      lock.unlock()
      previous?.complete(success: false)
      return
    }
    let outcome = lastOutcome ?? false
    lock.unlock()
    task.complete(success: outcome)  // nobody owns it
  }

  public func progress(done: Int64, total: Int64) {
    lock.lock()
    let task = held
    lock.unlock()
    task?.progress(done: done, total: total)
  }

  /// The copy ended, however it ended.
  public func finished(success: Bool) {
    lock.lock()
    let task = held
    held = nil
    live = false
    lastOutcome = success
    lock.unlock()
    task?.complete(success: success)
  }

  /// The system took the time back. The live copy stops (the Wi-Fi wait included, through
  /// `isExpired`) and its `finished` completes the task, as Apple requires even after expiry.
  /// With no live copy there is nothing held: a hand-over then was completed as it came.
  public func expire() {
    lock.lock()
    expiredFlag = true
    let orphan = live ? nil : held
    if !live { held = nil }
    lock.unlock()
    orphan?.complete(success: false)
  }

  public var isExpired: Bool {
    lock.lock()
    defer { lock.unlock() }
    return expiredFlag
  }
}
