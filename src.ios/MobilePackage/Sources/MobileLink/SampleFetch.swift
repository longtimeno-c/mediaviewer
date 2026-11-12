// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The sample library's transport (docs/plans/mobile.md §11.1): the core's `mv_fetch_fn`, an HTTPS GET
// of a byte range from the static folder src/mobile/sample.h names. Called on a core worker and
// blocks it, as the link's own reads do; never on the main thread. Nothing about the person is
// sent: no cookies, no cache, no headers but the range. Everything it returns is checked by the
// core against the signed manifest before it is used.
import CMobileLink
import Foundation

final class SampleFetch: NSObject, URLSessionDataDelegate, @unchecked Sendable {
  static let shared = SampleFetch()

  /// Where the sample library is read from. A Debug build may be pointed at a test folder
  /// (`MV_SAMPLE_BASE`: a loopback server or a file:// folder); a release reads only the core's.
  static var base: String {
    #if DEBUG
    if let override = ProcessInfo.processInfo.environment["MV_SAMPLE_BASE"], !override.isEmpty {
      return override.hasSuffix("/") ? override : override + "/"
    }
    #endif
    return String(cString: mv_sample_base())
  }

  /// The C callback; `ctx` is `shared`, which lives as long as the app.
  static let callback: mv_fetch_fn = { ctx, url, at, length, sink, sinkCtx, stopped, stoppedCtx in
    guard let ctx, let url, let sink else { return Int32(MV_LINK_INVALID.rawValue) }
    let me = Unmanaged<SampleFetch>.fromOpaque(ctx).takeUnretainedValue()
    return me.get(String(cString: url), at: at, length: length,
                  sink: { offset, data in
                    data.withUnsafeBytes { raw in
                      sink(sinkCtx, offset, raw.bindMemory(to: UInt8.self).baseAddress, raw.count) != 0
                    }
                  },
                  stopped: { stopped?(stoppedCtx) == 1 })
  }

  static var context: UnsafeMutableRawPointer { Unmanaged.passUnretained(shared).toOpaque() }

  private let lock = NSLock()
  private var running: [Int: Transfer] = [:]
  private var made: URLSession?

  /// Made on first use, under the lock: several core workers can ask at once.
  private var session: URLSession {
    lock.withLock {
      if let made { return made }
      let c = URLSessionConfiguration.ephemeral
      c.httpCookieStorage = nil
      c.httpShouldSetCookies = false
      c.urlCache = nil
      c.requestCachePolicy = .reloadIgnoringLocalAndRemoteCacheData
      c.timeoutIntervalForRequest = 30  // with no bytes for this long, the read fails and sync resumes later
      c.waitsForConnectivity = false
      c.httpMaximumConnectionsPerHost = 6
      let queue = OperationQueue()
      queue.name = "mediaviewer.sample.fetch"
      queue.maxConcurrentOperationCount = 4
      let session = URLSession(configuration: c, delegate: self, delegateQueue: queue)
      made = session
      return session
    }
  }

  /// One read in flight. Its delegate callbacks run on the session's queue, one at a time per task.
  private final class Transfer: @unchecked Sendable {
    let at: UInt64
    let length: UInt64
    let sink: (UInt64, Data) -> Bool
    let done = DispatchSemaphore(value: 0)
    var status = Int32(MV_LINK_UNREACHABLE.rawValue)
    var skip: UInt64 = 0      // a server that ignored the range: bytes before `at` to drop
    var delivered: UInt64 = 0
    var refusedBySink = false

    init(at: UInt64, length: UInt64, sink: @escaping (UInt64, Data) -> Bool) {
      self.at = at
      self.length = length
      self.sink = sink
    }
  }

  func get(_ text: String, at: UInt64, length: UInt64, sink: @escaping (UInt64, Data) -> Bool,
           stopped: @escaping () -> Bool) -> Int32 {
    guard length > 0 else { return Int32(MV_LINK_OK.rawValue) }
    guard let url = URL(string: text), Self.allowed(url) else { return Int32(MV_LINK_NOT_FOUND.rawValue) }
    if url.isFileURL { return readFile(url, at: at, length: length, sink: sink, stopped: stopped) }
    var request = URLRequest(url: url)
    request.httpMethod = "GET"
    let last = length == UInt64.max ? nil : at.addingReportingOverflow(length - 1)
    if at > 0 || last != nil {
      let end = last.map { $0.overflow ? "" : String($0.partialValue) } ?? ""
      request.setValue("bytes=\(at)-\(end)", forHTTPHeaderField: "Range")
    }
    request.setValue("identity", forHTTPHeaderField: "Accept-Encoding")
    let transfer = Transfer(at: at, length: length, sink: sink)
    let task = session.dataTask(with: request)
    lock.withLock { running[task.taskIdentifier] = transfer }
    task.resume()
    // Stopped (a cancel, Forget) ends the request at once, not at its next byte.
    while transfer.done.wait(timeout: .now() + .milliseconds(250)) == .timedOut {
      if stopped() { task.cancel() }
    }
    lock.withLock { running[task.taskIdentifier] = nil }
    if transfer.refusedBySink || stopped() { return Int32(MV_LINK_CANCELLED.rawValue) }
    return transfer.status
  }

  /// Only https, and only the folder the core named: a Debug build's test folder may be a loopback
  /// server or a file:// folder.
  private static func allowed(_ url: URL) -> Bool {
    guard let base = URL(string: base) else { return false }
    guard url.absoluteString.hasPrefix(base.absoluteString) else { return false }
    #if DEBUG
    if url.isFileURL || url.host == "127.0.0.1" || url.host == "localhost" { return true }
    #endif
    return url.scheme == "https"
  }

  private func transfer(for task: URLSessionTask) -> Transfer? { lock.withLock { running[task.taskIdentifier] } }

  func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                  newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
    // GitHub sends a release file from its CDN by a redirect; never to plain http.
    completionHandler(request.url?.scheme == "https" ? request : nil)
  }

  func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive response: URLResponse,
                  completionHandler: @escaping (URLSession.ResponseDisposition) -> Void) {
    guard let t = transfer(for: dataTask), let http = response as? HTTPURLResponse else {
      completionHandler(.allow)  // a file:// or other answer: the bytes decide
      return
    }
    switch http.statusCode {
    case 206:
      completionHandler(.allow)
    case 200:
      t.skip = t.at  // the whole file: drop what comes before the range
      completionHandler(.allow)
    case 416:
      t.status = Int32(MV_LINK_OK.rawValue)  // asked from the end: nothing more to read
      completionHandler(.cancel)
    case 404, 410:
      t.status = Int32(MV_LINK_NOT_FOUND.rawValue)
      completionHandler(.cancel)
    default:
      t.status = Int32(MV_LINK_UNREACHABLE.rawValue)
      completionHandler(.cancel)
    }
  }

  func urlSession(_ session: URLSession, dataTask: URLSessionDataTask, didReceive data: Data) {
    guard let t = transfer(for: dataTask), !t.refusedBySink else { return }
    var chunk = data
    if t.skip > 0 {
      let drop = Int(min(UInt64(chunk.count), t.skip))
      chunk = chunk.subdata(in: drop..<chunk.count)
      t.skip -= UInt64(drop)
      if chunk.isEmpty { return }
    }
    if t.length != UInt64.max, t.delivered + UInt64(chunk.count) > t.length {
      chunk = chunk.prefix(Int(t.length - t.delivered))
    }
    guard !chunk.isEmpty else { return }
    if !t.sink(t.delivered, chunk) {
      t.refusedBySink = true
      dataTask.cancel()
      return
    }
    t.delivered += UInt64(chunk.count)
    if t.length != UInt64.max, t.delivered >= t.length {
      t.status = Int32(MV_LINK_OK.rawValue)
      dataTask.cancel()  // a server that ignored the range keeps sending: enough
    }
  }

  func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
    guard let t = transfer(for: task) else { return }
    if error == nil, let http = task.response as? HTTPURLResponse, http.statusCode == 200 || http.statusCode == 206 {
      t.status = Int32(MV_LINK_OK.rawValue)
    }
    t.done.signal()
  }

  /// A Debug build's file:// test folder: the same ranges from a local file.
  private func readFile(_ url: URL, at: UInt64, length: UInt64, sink: (UInt64, Data) -> Bool,
                        stopped: () -> Bool) -> Int32 {
    guard let handle = try? FileHandle(forReadingFrom: url) else { return Int32(MV_LINK_NOT_FOUND.rawValue) }
    defer { try? handle.close() }
    do {
      try handle.seek(toOffset: at)
      var done: UInt64 = 0
      while length == UInt64.max || done < length {
        if stopped() { return Int32(MV_LINK_CANCELLED.rawValue) }
        let want = Int(min(UInt64(1 << 20), length == UInt64.max ? UInt64(1 << 20) : length - done))
        guard let chunk = try handle.read(upToCount: want), !chunk.isEmpty else { break }
        if !sink(done, chunk) { return Int32(MV_LINK_CANCELLED.rawValue) }
        done += UInt64(chunk.count)
      }
      return Int32(MV_LINK_OK.rawValue)
    } catch {
      return Int32(MV_LINK_UNREACHABLE.rawValue)
    }
  }
}
