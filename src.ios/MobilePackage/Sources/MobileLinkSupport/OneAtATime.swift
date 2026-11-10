// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Runs that must never overlap (the first copy: "Copy over mobile data" starts a new run while the
// old one is still unwinding). A new run cancels the one before it and starts only once that one
// has ended completely, its `defer`s included. Cancelling a caller cancels its run.
import Foundation

public actor OneAtATime {
  private var previousDone: Task<Void, Never>?
  private var cancelPrevious: (@Sendable () -> Void)?

  public init() {}

  public func run<T: Sendable>(_ body: @escaping @Sendable () async throws -> T) async throws -> T {
    let before = previousDone
    cancelPrevious?()  // the run itself, so its cancellation handlers fire and it unwinds
    let task = Task<T, Error> {
      await before?.value
      try Task.checkCancellation()
      return try await body()
    }
    previousDone = Task { _ = try? await task.value }
    cancelPrevious = { task.cancel() }
    return try await withTaskCancellationHandler {
      try await task.value
    } onCancel: {
      task.cancel()
    }
  }
}
