// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A clip the server converts as it plays (docs/plans/phone-clip-convert.md): its plan, as the
// service sent it, and the HLS playlist the loopback server gives AVPlayer for it. Every segment
// is `segmentMs` of the clip but the last, which runs to the end.
import Foundation

public struct ConvertPlan: Sendable, Equatable {
  public let durationMs: Int64
  public let segmentMs: UInt32
  public let segments: UInt32
  public let width: UInt32, height: UInt32

  public init(durationMs: Int64, segmentMs: UInt32, segments: UInt32, width: UInt32, height: UInt32) {
    self.durationMs = durationMs
    self.segmentMs = segmentMs
    self.segments = segments
    self.width = width
    self.height = height
  }

  /// Seconds of segment k.
  public func seconds(of k: Int) -> Double {
    guard segments > 0, k >= 0, k < Int(segments) else { return 0 }
    if k < Int(segments) - 1 { return Double(segmentMs) / 1000 }
    return max(0.001, Double(durationMs - Int64(segmentMs) * Int64(segments - 1)) / 1000)
  }

  /// The first segment alone, as a clip of its own (a poster's).
  public var firstSegment: ConvertPlan {
    ConvertPlan(durationMs: Int64((seconds(of: 0) * 1000).rounded()), segmentMs: segmentMs, segments: min(segments, 1),
                width: width, height: height)
  }

  public func playlist(segmentURL: (Int) -> String) -> String {
    let count = Int(segments)
    let longest = (0..<count).map { seconds(of: $0) }.max() ?? 1
    var lines = ["#EXTM3U", "#EXT-X-VERSION:3", "#EXT-X-TARGETDURATION:\(Int(longest.rounded(.up)))",
                 "#EXT-X-MEDIA-SEQUENCE:0", "#EXT-X-PLAYLIST-TYPE:VOD", "#EXT-X-INDEPENDENT-SEGMENTS"]
    for k in 0..<count {
      lines.append(String(format: "#EXTINF:%.3f,", seconds(of: k)))
      lines.append(segmentURL(k))
    }
    lines.append("#EXT-X-ENDLIST")
    return lines.joined(separator: "\n") + "\n"
  }
}
