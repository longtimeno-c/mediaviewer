// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Clips the phone can't play, converted by the server as they play (docs/plans/phone-clip-convert.md):
// the plan from the service, then an HLS playlist on the loopback server (TsStreams) whose segments
// are read from the service as AVPlayer asks for them. nil when the service converts nothing (an
// older one, or its owner turned it off: the caps say so, with no round trip) or the file is not a
// clip, and the phone keeps "can't play here".
import CMobileLink
import Foundation
import MobileLinkSupport

enum ConvertedClips {
  /// File mode: the clip by its folder and path.
  static func url(link: LinkHandle, root: String, rel: String) async -> URL? {
    let plan: ConvertPlan? = await onLinkQueue {
      root.withCString { r in
        rel.withCString { p in
          var out = mv_convert_plan()
          return mv_files_convert_plan(link.raw, r, p, &out) == 0 ? ConvertPlan(out) : nil
        }
      }
    }
    guard let plan else { return nil }
    return await TsStreams.shared.register(converted: plan) { k, cancel, sink in
      let box = Unmanaged.passRetained(BlockBox(sink))
      defer { box.release() }
      return root.withCString { r in
        rel.withCString { p in
          mv_files_convert_read(link.raw, r, p, k, cancel.raw, { ctx, _, bytes, count in
            guard let ctx, let bytes else { return 0 }
            return Unmanaged<BlockBox>.fromOpaque(ctx).takeUnretainedValue().body(Data(bytes: bytes, count: count)) ? 1 : 0
          }, box.toOpaque())
        }
      }
    }
  }

  /// A catalogue asset.
  static func url(link: LinkHandle, asset: Int64) async -> URL? {
    let plan: ConvertPlan? = await onLinkQueue {
      var out = mv_convert_plan()
      return mv_asset_convert_plan(link.raw, asset, &out) == 0 ? ConvertPlan(out) : nil
    }
    guard let plan else { return nil }
    return await TsStreams.shared.register(converted: plan) { k, cancel, sink in
      let box = Unmanaged.passRetained(BlockBox(sink))
      defer { box.release() }
      return mv_asset_convert_read(link.raw, asset, k, cancel.raw, { ctx, _, bytes, count in
        guard let ctx, let bytes else { return 0 }
        return Unmanaged<BlockBox>.fromOpaque(ctx).takeUnretainedValue().body(Data(bytes: bytes, count: count)) ? 1 : 0
      }, box.toOpaque())
    }
  }
}

extension ConvertPlan {
  init?(_ p: mv_convert_plan) {
    guard p.segments > 0, p.segment_ms > 0, p.duration_ms > 0 else { return nil }
    self.init(durationMs: p.duration_ms, segmentMs: p.segment_ms, segments: p.segments, width: p.width, height: p.height)
  }
}
