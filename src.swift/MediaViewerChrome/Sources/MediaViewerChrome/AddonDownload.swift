// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// One add-on archive download with byte progress (AddonChannel, AddonsView.swift).
//
// The async URLSession.download(from:delegate:) never reported bytes: the
// task's Progress counted a handful of units (5, then 100), not bytes, so
// Settings sat at 0 % until Checking (owner report, 2026-09-28). A download
// task on its own delegate session reports every write; this bridges it to
// async/await and moves the file into place before the continuation resumes
// (URLSession deletes its temp file when didFinishDownloadingTo returns).
//
// Foundation only, so a headless script can compile it beside a test driver.
import Foundation

enum AddonDownload {
  /// Downloads `url` to `destination` (which must not exist yet), calling
  /// `progress(done, total)` at most ~10 times a second and once at the end.
  /// `total` is -1 while the server has not said. Returns the HTTP status;
  /// the file is at `destination` only for a 200. Cancelling the calling
  /// task cancels the download.
  static func run(_ url: URL, to destination: URL, configuration: URLSessionConfiguration,
                  progress: (@Sendable (Int64, Int64) -> Void)?) async throws -> Int {
    let delegate = Delegate(destination: destination, progress: progress)
    // The session holds its delegate strongly until invalidated: finish and
    // invalidate as soon as the one task is done.
    let session = URLSession(configuration: configuration, delegate: delegate, delegateQueue: nil)
    defer { session.finishTasksAndInvalidate() }
    let task = session.downloadTask(with: url)
    return try await withTaskCancellationHandler {
      try await withCheckedThrowingContinuation { (c: CheckedContinuation<Int, Error>) in
        delegate.begin(c)
        task.resume()
      }
    } onCancel: {
      task.cancel()
    }
  }

  private final class Delegate: NSObject, URLSessionDownloadDelegate, @unchecked Sendable {
    // The session's delegate queue is serial: the callbacks never overlap,
    // and `lock` only covers the continuation handed over from the caller.
    private let destination: URL
    private let progress: (@Sendable (Int64, Int64) -> Void)?
    private let lock = NSLock()
    private var continuation: CheckedContinuation<Int, Error>?
    private var moveError: Error?
    private var lastReport = DispatchTime(uptimeNanoseconds: 0)
    private var lastDone: Int64 = 0
    private var lastTotal: Int64 = -1

    init(destination: URL, progress: (@Sendable (Int64, Int64) -> Void)?) {
      self.destination = destination
      self.progress = progress
    }

    func begin(_ c: CheckedContinuation<Int, Error>) {
      lock.lock()
      continuation = c
      lock.unlock()
    }

    private func finish(_ result: Result<Int, Error>) {
      lock.lock()
      let c = continuation
      continuation = nil
      lock.unlock()
      c?.resume(with: result)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
      guard let progress else { return }
      lastDone = totalBytesWritten
      lastTotal = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : -1
      // ~10 updates a second: enough to look live, not a SwiftUI storm.
      let now = DispatchTime.now()
      guard now.uptimeNanoseconds &- lastReport.uptimeNanoseconds >= 100_000_000 else { return }
      lastReport = now
      progress(lastDone, lastTotal)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
      // Only a 200 is kept; the temp file goes when this returns.
      guard (downloadTask.response as? HTTPURLResponse)?.statusCode == 200 else { return }
      do {
        try FileManager.default.moveItem(at: location, to: destination)
      } catch {
        moveError = error
      }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
      if let error {
        finish(.failure(error))
        return
      }
      if let moveError {
        finish(.failure(moveError))
        return
      }
      let code = (task.response as? HTTPURLResponse)?.statusCode ?? 0
      if code == 200 { progress?(lastDone, lastTotal) }
      finish(.success(code))
    }
  }
}
