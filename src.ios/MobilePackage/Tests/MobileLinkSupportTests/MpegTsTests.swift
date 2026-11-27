// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import Foundation
@testable import MobileLinkSupport
import XCTest

/// Transport streams made packet by packet: a PAT, a PMT naming the streams, and PCRs on the video PID.
private enum Stream {
  static let pmtPID = 0x100, videoPID = 0x101, audioPID = 0x102

  static func packet(pid: Int, start: Bool = false, adaptation: [UInt8]? = nil, payload: [UInt8] = []) -> [UInt8] {
    var p: [UInt8] = [0x47, UInt8((start ? 0x40 : 0) | (pid >> 8)), UInt8(pid & 0xFF), 0]
    if let adaptation {
      p[3] = payload.isEmpty ? 0x20 : 0x30
      p.append(UInt8(adaptation.count))
      p += adaptation
    } else {
      p[3] = 0x10
    }
    p += payload
    return p + [UInt8](repeating: 0xFF, count: 188 - p.count)
  }

  static func pat() -> [UInt8] {
    let section: [UInt8] = [0x00, 0xB0, 13, 0, 1, 0xC1, 0, 0, 0, 1, UInt8(0xE0 | (pmtPID >> 8)), UInt8(pmtPID & 0xFF), 0, 0, 0, 0]
    return packet(pid: 0, start: true, payload: [0] + section)
  }

  static func pmt(streams: [UInt8]) -> [UInt8] {
    var loop: [UInt8] = []
    for (i, type) in streams.enumerated() {
      let pid = videoPID + i
      loop += [type, UInt8(0xE0 | (pid >> 8)), UInt8(pid & 0xFF), 0xF0, 0]
    }
    let length = 9 + loop.count + 4
    let section: [UInt8] = [0x02, UInt8(0xB0 | (length >> 8)), UInt8(length & 0xFF), 0, 1, 0xC1, 0, 0,
                            UInt8(0xE0 | (videoPID >> 8)), UInt8(videoPID & 0xFF), 0xF0, 0] + loop + [0, 0, 0, 0]
    return packet(pid: pmtPID, start: true, payload: [0] + section)
  }

  /// A packet on the video PID carrying a PCR of `ticks` (90 kHz).
  static func pcr(_ ticks: Int64) -> [UInt8] {
    let field: [UInt8] = [0x10, UInt8((ticks >> 25) & 0xFF), UInt8((ticks >> 17) & 0xFF), UInt8((ticks >> 9) & 0xFF),
                          UInt8((ticks >> 1) & 0xFF), UInt8(((ticks & 1) << 7) | 0x7E), 0]
    return packet(pid: videoPID, adaptation: field, payload: [1, 2, 3])
  }

  /// `count` packets: the tables, a first PCR, filler, and a last PCR `seconds` later.
  static func file(streams: [UInt8] = [0x1B, 0x81], seconds: Double = 12, count: Int = 40, m2ts: Bool = false) -> Data {
    var packets = [pat(), pmt(streams: streams), pcr(900_000)]
    while packets.count < count - 1 { packets.append(packet(pid: audioPID, payload: [9])) }
    packets.append(pcr(900_000 + Int64(seconds * 90_000)))
    var out = Data()
    for (i, p) in packets.enumerated() {
      if m2ts { out += [0, 0, UInt8(i >> 8), UInt8(i & 0xFF)] }
      out += p
    }
    return out
  }
}

final class MpegTsTests: XCTestCase {
  private func layout(_ data: Data) -> MpegTsLayout? {
    let size = MpegTs.packetSize(of: data) ?? 188
    let tailStart = UInt64(max(0, data.count - 10 * size))
    return MpegTs.layout(head: data, tail: data.subdata(in: Int(tailStart)..<data.count), tailStart: tailStart,
                         fileSize: UInt64(data.count))
  }

  func testAnH264TransportStreamPlays() throws {
    let l = try XCTUnwrap(layout(Stream.file()))
    XCTAssertEqual(l.packetSize, 188)
    XCTAssertEqual(l.packetCount, 40)
    XCTAssertEqual(l.duration, 12, accuracy: 0.001)
    XCTAssertEqual(l.tables.count, 376)
    XCTAssertEqual(l.tables.first, 0x47)
  }

  func testM2tsIsReadAt192AndServedAt188() throws {
    let data = Stream.file(m2ts: true)
    let l = try XCTUnwrap(layout(data))
    XCTAssertEqual(l.packetSize, 192)
    XCTAssertEqual(l.packetCount, 40)
    // Chunks of any size come out as whole 188-byte packets, each starting with the sync byte.
    var repacker = MpegTs.Repacker(packetSize: 192)
    var out = Data()
    var i = 0
    while i < data.count {
      let n = min(7 + i % 300, data.count - i)
      out += repacker.push(data.subdata(in: i..<(i + n)))
      i += n
    }
    XCTAssertEqual(out.count, 40 * 188)
    XCTAssertTrue(stride(from: 0, to: out.count, by: 188).allSatisfy { out[$0] == 0x47 })
  }

  func testWhatHlsCannotCarryIsLeftToTheDownload() {
    XCTAssertNil(layout(Stream.file(streams: [0x02, 0x81])), "MPEG-2 video")
    XCTAssertNil(layout(Stream.file(streams: [0x24, 0x0F])), "HEVC")
    XCTAssertNil(layout(Stream.file(streams: [0x1B, 0x80])), "LPCM sound")
    XCTAssertNil(layout(Data(repeating: 0, count: 188 * 10)), "not a transport stream")
  }

  func testSegmentsCoverTheFileOnceAndLeadWithTheTables() throws {
    let packets = MpegTs.segmentPackets * 2 + 10
    let l = MpegTsLayout(packetSize: 192, packetCount: packets, tables: Data(repeating: 0x47, count: 376), duration: 30)
    XCTAssertEqual(MpegTs.segmentCount(l), 3)
    var next: UInt64 = 0
    for k in 0..<3 {
      let (offset, length) = MpegTs.fileRange(of: k, in: l)
      XCTAssertEqual(offset, next)
      next += length
    }
    XCTAssertEqual(next, UInt64(packets * 192))
    XCTAssertEqual(MpegTs.servedLength(of: 0, in: l), MpegTs.segmentPackets * 188)
    XCTAssertEqual(MpegTs.servedLength(of: 2, in: l), 376 + 10 * 188)
    let playlist = MpegTs.playlist(l) { "\($0).ts" }
    XCTAssertTrue(playlist.hasPrefix("#EXTM3U\n"))
    XCTAssertTrue(playlist.hasSuffix("#EXT-X-ENDLIST\n"))
    XCTAssertEqual(playlist.components(separatedBy: "#EXTINF:").count - 1, 3)
    let total = playlist.split(separator: "\n").filter { $0.hasPrefix("#EXTINF:") }
      .compactMap { Double($0.dropFirst(8).dropLast()) }.reduce(0, +)
    XCTAssertEqual(total, 30, accuracy: 0.01)
  }
}
