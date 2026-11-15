// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Transport-stream clips (.mts, .m2ts, .ts) played as HLS (MobileLinkSupport/MpegTs.swift): a small
// HTTP server on 127.0.0.1 that only this app talks to serves each clip's playlist and its segments,
// read from the NAS as AVPlayer asks for them. AVFoundation will not take HLS segments from a
// resource loader, so this is a socket, not an AVAssetResourceLoaderDelegate. Nothing leaves the phone.
import AVFoundation
import CMobileLink
import Foundation
import MobileLinkSupport
import Network
import VideoToolbox

final class TsStreams: @unchecked Sendable {
  static let shared = TsStreams()
  /// Clips kept playable at once; the oldest is dropped (a player still on it gets a 404 and stops).
  static let kept = 6

  private struct Clip {
    let layout: MpegTsLayout
    let read: ClipRead
  }

  private let lock = NSLock()
  private var clips: [String: Clip] = [:]
  private var order: [String] = []
  private var listener: NWListener?
  private var port: UInt16 = 0
  private var waiting: [CheckedContinuation<UInt16?, Never>] = []
  private let queue = DispatchQueue(label: "mediaviewer.tsstreams", qos: .userInitiated)

  /// Reads the clip's two ends and says how it plays: nil when it is not a transport stream iOS can
  /// play as HLS (MPEG-2 video, LPCM audio, no clock). Blocks; call on the link queue.
  static func layout(read: ClipRead, size: UInt64) -> MpegTsLayout? {
    guard size >= 188 * 4 else { return nil }
    guard let head = readAll(read, at: 0, length: min(size, UInt64(MpegTs.headBytes))),
          let packet = MpegTs.packetSize(of: head) else { return nil }
    let tailLength = min(size, UInt64(MpegTs.tailBytes))
    let tailStart = (size - tailLength) / UInt64(packet) * UInt64(packet)
    guard let tail = readAll(read, at: tailStart, length: size - tailStart) else { return nil }
    return MpegTs.layout(head: head, tail: tail, tailStart: tailStart, fileSize: size)
  }

  /// The clip's playlist URL, once the server is listening.
  func register(_ layout: MpegTsLayout, read: @escaping ClipRead) async -> URL? {
    guard let port = await listening() else { return nil }
    let token = UUID().uuidString.lowercased()
    lock.withLock {
      clips[token] = Clip(layout: layout, read: read)
      order.append(token)
      while order.count > Self.kept { clips[order.removeFirst()] = nil }
    }
    return URL(string: "http://127.0.0.1:\(port)/\(token)/index.m3u8")
  }

  /// The clip's first picture, from a muted player that is let go once it has one.
  static func firstFrame(_ url: URL, maxPixels: Int, timeout: Duration = .seconds(8)) async -> CGImage? {
    let item = AVPlayerItem(asset: AVURLAsset(url: url))
    let output = AVPlayerItemVideoOutput(pixelBufferAttributes: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA])
    item.add(output)
    let player = AVPlayer(playerItem: item)
    player.isMuted = true
    player.automaticallyWaitsToMinimizeStalling = false
    player.play()
    defer { player.pause(); player.replaceCurrentItem(with: nil) }
    let deadline = ContinuousClock.now + timeout
    while ContinuousClock.now < deadline, !Task.isCancelled, item.status != .failed {
      let time = item.currentTime()
      if output.hasNewPixelBuffer(forItemTime: time),
         let buffer = output.copyPixelBuffer(forItemTime: time, itemTimeForDisplay: nil) {
        return image(of: buffer, maxPixels: maxPixels)
      }
      try? await Task.sleep(for: .milliseconds(50))
    }
    return nil
  }

  private static func image(of buffer: CVPixelBuffer, maxPixels: Int) -> CGImage? {
    var cg: CGImage?
    VTCreateCGImageFromCVPixelBuffer(buffer, options: nil, imageOut: &cg)
    guard let cg else { return nil }
    let long = max(cg.width, cg.height)
    guard long > maxPixels else { return cg }
    let scale = Double(maxPixels) / Double(long)
    let w = Int((Double(cg.width) * scale).rounded()), h = Int((Double(cg.height) * scale).rounded())
    guard let context = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.noneSkipFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
    else { return cg }
    context.interpolationQuality = .medium
    context.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
    return context.makeImage() ?? cg
  }

  // MARK: The server

  private func listening() async -> UInt16? {
    await withCheckedContinuation { (c: CheckedContinuation<UInt16?, Never>) in
      lock.lock()
      if port != 0 {
        lock.unlock()
        c.resume(returning: port)
        return
      }
      waiting.append(c)
      let starting = listener != nil
      lock.unlock()
      if !starting { start() }
    }
  }

  private func start() {
    let parameters = NWParameters.tcp
    parameters.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(.loopback), port: .any)
    guard let listener = try? NWListener(using: parameters) else { return finishStart(nil) }
    lock.withLock { self.listener = listener }
    listener.newConnectionHandler = { [weak self] connection in self?.accept(connection) }
    listener.stateUpdateHandler = { [weak self] state in
      guard let self else { return }
      switch state {
      case .ready: finishStart(listener.port?.rawValue)
      case .failed, .cancelled:
        lock.withLock {
          if self.listener === listener { self.listener = nil }
          self.port = 0
        }
        finishStart(nil)
      default: break
      }
    }
    listener.start(queue: queue)
  }

  private func finishStart(_ port: UInt16?) {
    let resumed = lock.withLock { () -> [CheckedContinuation<UInt16?, Never>] in
      if let port { self.port = port }
      defer { waiting = [] }
      return waiting
    }
    for c in resumed { c.resume(returning: port) }
  }

  private func accept(_ connection: NWConnection) {
    connection.start(queue: queue)
    receiveRequest(connection, buffer: Data())
  }

  private func receiveRequest(_ connection: NWConnection, buffer: Data) {
    connection.receive(minimumIncompleteLength: 1, maximumLength: 16 * 1024) { [weak self] data, _, complete, error in
      guard let self else { return connection.cancel() }
      var buffer = buffer
      if let data { buffer.append(data) }
      if let end = buffer.range(of: Data("\r\n\r\n".utf8)) {
        let head = String(decoding: buffer[..<end.lowerBound], as: UTF8.self)
        linkNetQueue.async { self.answer(head, on: connection) }
      } else if error != nil || complete || buffer.count > 64 * 1024 {
        connection.cancel()
      } else {
        receiveRequest(connection, buffer: buffer)
      }
    }
  }

  /// One request, one response, then the connection closes.
  private func answer(_ head: String, on connection: NWConnection) {
    let lines = head.components(separatedBy: "\r\n")
    let parts = lines.first?.split(separator: " ") ?? []
    guard parts.count >= 2, parts[0] == "GET" || parts[0] == "HEAD" else { return respond(connection, status: "405 Method Not Allowed") }
    let headOnly = parts[0] == "HEAD"
    let path = parts[1].split(separator: "/").map(String.init)
    guard path.count == 2, let clip = lock.withLock({ clips[path[0]] }) else { return respond(connection, status: "404 Not Found") }
    let range = lines.dropFirst().first { $0.lowercased().hasPrefix("range:") }.flatMap(Self.byteRange)
    if path[1] == "index.m3u8" {
      let body = Data(MpegTs.playlist(clip.layout) { "\($0).ts" }.utf8)
      return respond(connection, type: "application/vnd.apple.mpegurl", body: body, range: range, headOnly: headOnly)
    }
    guard path[1].hasSuffix(".ts"), let k = Int(path[1].dropLast(3)), k >= 0, k < MpegTs.segmentCount(clip.layout)
    else { return respond(connection, status: "404 Not Found") }
    if headOnly {
      return respond(connection, type: "video/mp2t", length: MpegTs.servedLength(of: k, in: clip.layout), range: range)
    }
    let cancel = LinkCancel()
    connection.stateUpdateHandler = { state in
      if case .failed = state { cancel.fire() } else if case .cancelled = state { cancel.fire() }
    }
    var body = k == 0 ? Data() : clip.layout.tables
    var repacker = MpegTs.Repacker(packetSize: clip.layout.packetSize)
    let (offset, length) = MpegTs.fileRange(of: k, in: clip.layout)
    let status = clip.read(offset, length, cancel) { chunk in
      body.append(repacker.push(chunk))
      return true
    }
    guard status == 0 else { return respond(connection, status: "502 Bad Gateway") }
    respond(connection, type: "video/mp2t", body: body, range: range, headOnly: false)
  }

  /// "bytes=a-b", "bytes=a-": the first range only.
  private static func byteRange(_ line: String) -> (Int, Int?)? {
    guard let value = line.split(separator: "=", maxSplits: 1).last?.trimmingCharacters(in: .whitespaces) else { return nil }
    let ends = value.split(separator: ",").first?.split(separator: "-", omittingEmptySubsequences: false) ?? []
    guard ends.count == 2, let start = Int(ends[0]) else { return nil }
    return (start, Int(ends[1]))
  }

  private func respond(_ connection: NWConnection, status: String) {
    send(connection, header: "HTTP/1.1 \(status)\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", body: nil)
  }

  private func respond(_ connection: NWConnection, type: String, length: Int, range: (Int, Int?)?) {
    send(connection, header: header(type: type, total: length, range: range).text, body: nil)
  }

  private func respond(_ connection: NWConnection, type: String, body: Data, range: (Int, Int?)?, headOnly: Bool) {
    let (text, slice) = header(type: type, total: body.count, range: range)
    send(connection, header: text, body: headOnly ? nil : body.subdata(in: slice))
  }

  private func header(type: String, total: Int, range: (Int, Int?)?) -> (text: String, slice: Range<Int>) {
    var slice = 0..<total
    var status = "200 OK"
    var extra = ""
    if let (start, end) = range, start < total {
      slice = start..<min(total, (end ?? (total - 1)) + 1)
      status = "206 Partial Content"
      extra = "Content-Range: bytes \(slice.lowerBound)-\(slice.upperBound - 1)/\(total)\r\n"
    }
    let text = "HTTP/1.1 \(status)\r\nContent-Type: \(type)\r\nContent-Length: \(slice.count)\r\n\(extra)" +
      "Accept-Ranges: bytes\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n"
    return (text, slice)
  }

  private func send(_ connection: NWConnection, header: String, body: Data?) {
    var data = Data(header.utf8)
    if let body { data.append(body) }
    connection.send(content: data, contentContext: .finalMessage, isComplete: true, completion: .contentProcessed { _ in
      connection.cancel()
    })
  }

  private static func readAll(_ read: ClipRead, at offset: UInt64, length: UInt64) -> Data? {
    var data = Data()
    data.reserveCapacity(Int(length))
    let status = read(offset, length, LinkCancel()) { chunk in
      data.append(chunk)
      return true
    }
    return status == 0 ? data : nil
  }
}
