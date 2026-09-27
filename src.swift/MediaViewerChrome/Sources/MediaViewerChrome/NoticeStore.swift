// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 12: the one-line notice in the command bar -- what a rating key or a click
// just did ("★★★★☆  saved", "Could not save the rating"). The host owns the
// text and clears it after a few seconds; this only mirrors it.
import Combine
import Foundation
import MVChromeBridge

@MainActor
final class NoticeStore: ObservableObject {
  static let shared = NoticeStore()

  @Published private(set) var text = ""
  private var generation: UInt64 = .max
  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.15, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    poll()
  }

  private func poll() {
    let g = mv_chrome_notice_generation()
    guard g != generation else { return }
    generation = g
    let needed = Int(mv_chrome_notice_text(nil, 0))
    guard needed > 0 else {
      text = ""
      return
    }
    var buf = [CChar](repeating: 0, count: needed + 1)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_notice_text($0.baseAddress, Int32($0.count)) }
    text = String(cString: buf)
  }
}
