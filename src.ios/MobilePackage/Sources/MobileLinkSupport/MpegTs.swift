// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// MPEG transport streams on the phone (AVCHD .mts/.m2ts, .ts; D5 as amended by #702). iOS opens none
// of them as a file ("Cannot Open"), but AVPlayer plays the same bytes as HLS: a VOD playlist whose
// segments are ~1 MiB runs of the file's packets, each after the first led by the file's PAT and PMT
// so it can be read on its own. M2TS's 4-byte timecode before each packet is dropped on the way
// (192 → 188). Only H.264 video plays this way (HLS takes no MPEG-2 video and no HEVC in TS), and
// only with AAC, AC-3, E-AC-3 or MPEG audio, or none. Pure: the bytes come from the caller.
import Foundation

public struct MpegTsLayout: Sendable, Equatable {
  /// 188, or 192 for M2TS.
  public let packetSize: Int
  /// Whole packets in the file.
  public let packetCount: Int
  /// The PAT and PMT packets, 188 bytes each, that lead every segment after the first.
  public let tables: Data
  /// Seconds, from the first and last PCR.
  public let duration: Double
}

public enum MpegTs {
  /// Packets per segment: 5,577 × 188 bytes is just under 1 MiB.
  public static let segmentPackets = 5577
  /// What the caller reads from each end of the file for `layout`.
  public static let headBytes = 512 * 1024
  public static let tailBytes = 256 * 1024

  static let sync: UInt8 = 0x47
  static let videoH264: UInt8 = 0x1B
  /// Video HLS cannot carry in TS: MPEG-1, MPEG-2, MPEG-4 Part 2, HEVC, VC-1.
  static let otherVideo: Set<UInt8> = [0x01, 0x02, 0x10, 0x24, 0xEA]
  /// MPEG-1/2 audio, AAC (ADTS), AC-3, E-AC-3.
  static let playableAudio: Set<UInt8> = [0x03, 0x04, 0x0F, 0x81, 0x87]
  /// Audio iOS's HLS does not take: AAC LATM, LPCM (Blu-ray, some AVCHD), DTS, TrueHD.
  static let otherAudio: Set<UInt8> = [0x11, 0x80, 0x82, 0x83, 0x85, 0x86]

  /// 188 or 192 when `head` starts on packets of that size, else nil.
  public static func packetSize(of head: Data) -> Int? {
    for size in [188, 192] {
      let lead = size - 188
      let needed = min(3, head.count / size)
      guard needed >= 1 else { continue }
      if (0..<needed).allSatisfy({ head[head.startIndex + $0 * size + lead] == sync }) { return size }
    }
    return nil
  }

  /// How to play the file as HLS, from its first `headBytes` and its last bytes from `tailStart` (a
  /// multiple of the packet size). nil: not a transport stream iOS can play this way.
  public static func layout(head: Data, tail: Data, tailStart: UInt64, fileSize: UInt64) -> MpegTsLayout? {
    guard let size = packetSize(of: head) else { return nil }
    let packets = Int(fileSize / UInt64(size))
    guard packets > 0, tailStart % UInt64(size) == 0 else { return nil }
    let headPackets = packetsOf(head, size: size)
    guard let pat = headPackets.first(where: { pid($0) == 0 && startsUnit($0) }),
          let pmtPID = programMapPID(pat),
          let pmt = headPackets.first(where: { pid($0) == pmtPID && startsUnit($0) }),
          let program = programOf(pmt) else { return nil }
    guard program.streams.contains(videoH264), program.streams.isDisjoint(with: otherVideo),
          program.streams.isDisjoint(with: otherAudio) else { return nil }
    guard let first = headPackets.lazy.compactMap({ pcr(of: $0, on: program.pcrPID) }).first,
          let last = packetsOf(tail, size: size).reversed().lazy.compactMap({ pcr(of: $0, on: program.pcrPID) }).first
    else { return nil }
    var ticks = last - first
    if ticks < 0 { ticks += 1 << 33 }  // the 33-bit clock wrapped
    let duration = Double(ticks) / 90_000
    guard duration > 0.1 else { return nil }
    return MpegTsLayout(packetSize: size, packetCount: packets, tables: Data(pat) + Data(pmt), duration: duration)
  }

  public static func segmentCount(_ layout: MpegTsLayout) -> Int {
    (layout.packetCount + segmentPackets - 1) / segmentPackets
  }

  /// The file bytes segment `k` is made from.
  public static func fileRange(of k: Int, in layout: MpegTsLayout) -> (offset: UInt64, length: UInt64) {
    let first = k * segmentPackets
    let count = max(0, min(segmentPackets, layout.packetCount - first))
    return (UInt64(first) * UInt64(layout.packetSize), UInt64(count) * UInt64(layout.packetSize))
  }

  /// Segment `k`'s length as served: its tables, then its packets at 188 bytes.
  public static func servedLength(of k: Int, in layout: MpegTsLayout) -> Int {
    let packets = Int(fileRange(of: k, in: layout).length) / layout.packetSize
    return (k == 0 ? 0 : layout.tables.count) + packets * 188
  }

  /// A VOD playlist, each segment's time its share of the bytes (a camcorder's rate is near constant).
  public static func playlist(_ layout: MpegTsLayout, segmentURL: (Int) -> String) -> String {
    let count = segmentCount(layout)
    let times = (0..<count).map { k -> Double in
      let packets = Double(fileRange(of: k, in: layout).length) / Double(layout.packetSize)
      return layout.duration * packets / Double(layout.packetCount)
    }
    var lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:\(Int((times.max() ?? 1).rounded(.up)) + 1)",
                 "#EXT-X-MEDIA-SEQUENCE:0", "#EXT-X-PLAYLIST-TYPE:VOD"]
    for (k, time) in times.enumerated() {
      lines.append(String(format: "#EXTINF:%.3f,", time))
      lines.append(segmentURL(k))
    }
    lines.append("#EXT-X-ENDLIST")
    return lines.joined(separator: "\n") + "\n"
  }

  /// Turns the file's bytes, in chunks of any size, into 188-byte packets.
  public struct Repacker {
    private let size: Int
    private var carry = Data()

    public init(packetSize: Int) { size = packetSize }

    public mutating func push(_ chunk: Data) -> Data {
      if size == 188 { return chunk }
      carry.append(chunk)
      let whole = carry.count / size
      var out = Data(capacity: whole * 188)
      carry.withUnsafeBytes { raw in
        for i in 0..<whole {
          out.append(contentsOf: UnsafeRawBufferPointer(rebasing: raw[(i * size + size - 188)..<((i + 1) * size)]))
        }
      }
      carry.removeFirst(whole * size)
      return out
    }
  }

  // MARK: Packets

  static func packetsOf(_ data: Data, size: Int) -> [[UInt8]] {
    let bytes = [UInt8](data)
    let lead = size - 188
    var out: [[UInt8]] = []
    out.reserveCapacity(bytes.count / size)
    var i = 0
    while i + size <= bytes.count {
      if bytes[i + lead] == sync { out.append(Array(bytes[(i + lead)..<(i + size)])) }
      i += size
    }
    return out
  }

  static func pid(_ p: [UInt8]) -> Int { (Int(p[1] & 0x1F) << 8) | Int(p[2]) }
  static func startsUnit(_ p: [UInt8]) -> Bool { p[1] & 0x40 != 0 }

  /// Where the payload starts, after any adaptation field; nil when there is none.
  static func payloadStart(_ p: [UInt8]) -> Int? {
    let control = (p[3] >> 4) & 0x3
    switch control {
    case 1: return 4
    case 3:
      let start = 5 + Int(p[4])
      return start < 188 ? start : nil
    default: return nil
    }
  }

  /// A PSI section's bytes, from its table id on, for a packet that starts one.
  static func section(_ p: [UInt8]) -> ArraySlice<UInt8>? {
    guard let start = payloadStart(p), start < 188 else { return nil }
    let at = start + 1 + Int(p[start])  // pointer field
    guard at + 3 <= 188 else { return nil }
    let length = (Int(p[at + 1] & 0x0F) << 8) | Int(p[at + 2])
    let end = min(188, at + 3 + length)
    return p[at..<end]
  }

  /// The PAT's first program's PMT.
  static func programMapPID(_ pat: [UInt8]) -> Int? {
    guard let s = section(pat), s.first == 0x00 else { return nil }
    let bytes = Array(s)
    var i = 8
    while i + 4 <= bytes.count - 4 {  // the last 4 are the CRC
      let program = (Int(bytes[i]) << 8) | Int(bytes[i + 1])
      let pid = (Int(bytes[i + 2] & 0x1F) << 8) | Int(bytes[i + 3])
      if program != 0 { return pid }
      i += 4
    }
    return nil
  }

  static func programOf(_ pmt: [UInt8]) -> (pcrPID: Int, streams: Set<UInt8>)? {
    guard let s = section(pmt), s.first == 0x02 else { return nil }
    let bytes = Array(s)
    guard bytes.count >= 12 else { return nil }
    let pcrPID = (Int(bytes[8] & 0x1F) << 8) | Int(bytes[9])
    let infoLength = (Int(bytes[10] & 0x0F) << 8) | Int(bytes[11])
    var i = 12 + infoLength
    var streams = Set<UInt8>()
    while i + 5 <= bytes.count - 4 {
      streams.insert(bytes[i])
      let esInfo = (Int(bytes[i + 3] & 0x0F) << 8) | Int(bytes[i + 4])
      i += 5 + esInfo
    }
    return streams.isEmpty ? nil : (pcrPID, streams)
  }

  /// The PCR base (90 kHz) a packet on `pcrPID` carries.
  static func pcr(of p: [UInt8], on pcrPID: Int) -> Int64? {
    guard pid(p) == pcrPID, (p[3] >> 4) & 0x2 != 0, p[4] >= 7, p[5] & 0x10 != 0 else { return nil }
    return (Int64(p[6]) << 25) | (Int64(p[7]) << 17) | (Int64(p[8]) << 9) | (Int64(p[9]) << 1) | (Int64(p[10]) >> 7)
  }
}
