// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Work that writes into the library (the first copy, the quiet sync), kept so "Forget this library"
// can cancel all of it and wait for it to end before anything is deleted. Work started after the
// stop is refused until `reopen()`.
import Foundation

public final class TrackedWork: @unchecked Sendable {
  private let lock = NSLock()
  private var running: [UUID: Task<Void, Never>] = [:]
  private var stopped = false

  public init() {}

  /// Runs `body` as a tracked task. nil when stopped: the caller treats it as cancelled.
  @discardableResult
  public func run(priority: TaskPriority = .userInitiated, _ body: @escaping @Sendable () async -> Void) -> Task<Void, Never>? {
    let id = UUID()
    lock.lock()
    defer { lock.unlock() }
    guard !stopped else { return nil }
    let task = Task.detached(priority: priority) { [weak self] in
      await body()
      self?.finished(id)
    }
    running[id] = task
    return task
  }

  public var isStopped: Bool {
    lock.lock()
    defer { lock.unlock() }
    return stopped
  }

  /// Cancels everything running, refuses new work, and returns once all of it has ended.
  public func stopAndWait() async {
    let tasks: [Task<Void, Never>] = {
      lock.lock()
      defer { lock.unlock() }
      stopped = true
      return Array(running.values)
    }()
    tasks.forEach { $0.cancel() }
    for task in tasks { await task.value }
  }

  public func reopen() {
    lock.lock()
    stopped = false
    lock.unlock()
  }

  private func finished(_ id: UUID) {
    lock.lock()
    running[id] = nil
    lock.unlock()
  }
}
