// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Issue #178: the one wake source for the chrome stores' polls. Each store used
// to run its own repeating Timer for the whole session (about 70 main-thread
// wake-ups a second on a still). Almost everything the stores mirror changes on
// the main thread (a key, a click, a menu, a completion the host pumps there),
// so the stores poll on the main run loop's way back to sleep, after that work
// ran, and an idle app is not woken at all.
//
// What changes off the main thread without waking it (a playing clip's
// position, a running job's progress, a backup's count) is watched by a store
// returning `true` from its poll: one coalesced timer with tolerance then runs
// at `interval` until no store asks for it. For a second after the user's last
// input the timer also runs, so a follow-up that lands off the main thread (a
// seek, a clip finishing its open) still shows without another event.
import AppKit

@MainActor
final class ChromePulse {
  static let shared = ChromePulse()

  /// The cadence while something is in progress: the old fastest store timer.
  static let interval: TimeInterval = 0.1
  /// How long after the last key / click / scroll the timer keeps running.
  static let inputGrace: TimeInterval = 1.0

  /// Debug counters (issue #178's measure): poll rounds and timer fires.
  private(set) var rounds: UInt64 = 0
  private(set) var timerFires: UInt64 = 0
  var ticking: Bool { timer != nil }

  /// A store's poll; `true` while it watches something that moves off the main thread.
  private var pollers: [() -> Bool] = []
  private var observer: CFRunLoopObserver?
  private var monitor: Any?
  private var timer: Timer?
  private var lastInput: CFAbsoluteTime = 0

  private init() {
    // Before waiting: once per pass of the main run loop, after its events,
    // timers and main-queue blocks ran. Order 0 runs ahead of Core Animation's
    // commit, so what a poll publishes is drawn in the same pass. Default mode
    // only, as the store timers were: nothing polls inside a menu or a modal.
    observer = CFRunLoopObserverCreateWithHandler(
      kCFAllocatorDefault, CFRunLoopActivity.beforeWaiting.rawValue, true, 0
    ) { _, _ in
      MainActor.assumeIsolated { ChromePulse.shared.round() }
    }
    CFRunLoopAddObserver(CFRunLoopGetMain(), observer, .defaultMode)
    monitor = NSEvent.addLocalMonitorForEvents(
      matching: [.keyDown, .leftMouseDown, .leftMouseUp, .rightMouseDown, .otherMouseDown, .scrollWheel]
    ) { event in
      MainActor.assumeIsolated { ChromePulse.shared.lastInput = CFAbsoluteTimeGetCurrent() }
      return event
    }
  }

  /// Registers a store's poll and runs it once now.
  func add(_ poll: @escaping @MainActor () -> Bool) {
    pollers.append(poll)
    if poll() { schedule(true) }
  }

  private func round() {
    rounds &+= 1
    var busy = false
    for poll in pollers where poll() { busy = true }
    schedule(busy || CFAbsoluteTimeGetCurrent() - lastInput < Self.inputGrace)
  }

  private func schedule(_ want: Bool) {
    if want, timer == nil {
      // The fire only wakes the run loop; the observer polls on its way back to sleep.
      let t = Timer(timeInterval: Self.interval, repeats: true) { _ in
        MainActor.assumeIsolated { ChromePulse.shared.timerFires &+= 1 }
      }
      t.tolerance = Self.interval / 5
      RunLoop.main.add(t, forMode: .default)
      timer = t
    } else if !want, let t = timer {
      t.invalidate()
      timer = nil
    }
  }
}
