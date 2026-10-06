// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The People grid at 200 people (owner report, 2026-10-03: "my settings page
// is really laggy"). The grid sits in the base Settings list through
// SettingsHostView, which is sized by a full layout pass
// (-fittingHeightForWidth:) and is not lazy, so every card is live at once.
// Measured here with an empty mv.ai.1 table (no pack; monogram covers):
//   - the grid at rest;
//   - the layout pass the base pays for the grid's height;
//   - a model publish the grid does not show (the status line is republished
//     at up to 4 Hz while indexing);
//   - a people update where the face counts moved, and one where the list
//     also came back re-sorted (faces streaming in; a merge).
// Each case waits for SwiftUI to apply the change and for any animation to
// end, so CPU time is the number to compare; the wall clock is mostly waits.
// Layout and update work, not drawing: the window is never shown.
//   cd src.swift/AIChrome && swift test -c release -Xswiftc -enable-testing --filter PeopleGridBench
import AppKit
import SwiftUI
import XCTest
@testable import AIChrome
import CAiApi

@MainActor
final class PeopleGridBench: XCTestCase {
  private static let width: CGFloat = 752  // the Settings column, inside its padding
  private static let count = 200

  /// A table with every entry absent: each call fails, no pack is needed.
  private static let api: UnsafeMutablePointer<mv_ai_api> = {
    let p = UnsafeMutablePointer<mv_ai_api>.allocate(capacity: 1)
    p.initialize(to: mv_ai_api())
    p.pointee.struct_size = UInt32(MemoryLayout<mv_ai_api>.size)
    return p
  }()

  private static func people(bump: Int = 0) -> [Person] {
    (0..<count).map { i in
      // Half named, face counts spread so the grid's order is the index's.
      Person(id: UInt64(i + 1), name: i % 2 == 0 ? "Person \(i)" : "",
             faces: 1 + (i * 7 + bump * (i % 3)) % 400, coverFace: 0, coverBox: [0.4, 0.3, 0.2, 0.2])
    }
  }

  private func model() -> ManagementModel {
    let m = ManagementModel(table: AITable(UnsafePointer(Self.api)))
    m.applyPeople(Self.people())
    return m
  }

  /// A hosted grid in an offscreen window, laid out and settled; torn down
  /// after the test so nothing of it runs into the next.
  private func hosted(_ m: ManagementModel, limit: Int? = nil) -> NSHostingView<PeopleGrid> {
    let host = NSHostingView(rootView: PeopleGrid(model: m, open: { _ in }, limit: limit, showAll: {}))
    host.sizingOptions = []
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: Self.width, height: 4000),
                          styleMask: [.borderless], backing: .buffered, defer: false)
    window.isReleasedWhenClosed = false
    window.contentView = host
    host.frame = NSRect(x: 0, y: 0, width: Self.width, height: 4000)
    host.layoutSubtreeIfNeeded()
    spin(0.3)
    addTeardownBlock { @MainActor in
      window.contentView = nil
      window.close()
      self.spin(0.2)
    }
    return host
  }

  /// Lets SwiftUI apply a published change (it does so on the next run-loop turn).
  private func spin(_ seconds: TimeInterval) {
    RunLoop.main.run(until: Date(timeIntervalSinceNow: seconds))
  }

  private var metrics: [XCTMetric] { [XCTClockMetric(), XCTCPUMetric()] }

  /// -fittingHeightForWidth: for the grid alone: one full layout pass.
  func testLayoutPass200() {
    let m = model()
    measure(metrics: metrics) {
      let c = NSHostingController(rootView: PeopleGrid(model: m, open: { _ in }))
      c.sizingOptions = []
      let size = c.sizeThatFits(in: CGSize(width: Self.width, height: .greatestFiniteMagnitude))
      XCTAssertGreaterThan(size.height, 1000)
    }
  }

  /// The same pass for the grid as Settings has it since 2026-10-06: the
  /// first 24 people and "Show all" (ManagementView.peopleShown).
  func testLayoutPassSettings() {
    let m = model()
    measure(metrics: metrics) {
      let c = NSHostingController(rootView: PeopleGrid(model: m, open: { _ in }, limit: ManagementView.peopleShown,
                                                       showAll: {}))
      c.sizingOptions = []
      let size = c.sizeThatFits(in: CGSize(width: Self.width, height: .greatestFiniteMagnitude))
      XCTAssertGreaterThan(size.height, 300)
    }
  }

  /// The counts moved and the list came back in the index's order (most
  /// faces first), as it does while faces stream in and after a merge.
  func testPeopleReorder200() {
    reorder(limit: nil)
  }

  /// The same, for the grid Settings shows (24 people and "Show all").
  func testPeopleReorderSettings() {
    reorder(limit: ManagementView.peopleShown)
  }

  private func reorder(limit: Int?) {
    let m = model()
    _ = hosted(m, limit: limit)
    var bump = 0
    measure(metrics: metrics) {
      bump += 1
      let list = Self.people(bump: bump).sorted {
        if $0.name.isEmpty != $1.name.isEmpty { return !$0.name.isEmpty }
        return $0.faces > $1.faces
      }
      m.applyPeople(list)
      spin(0.5)
    }
  }

  /// Nothing happens for half a second: what the grid costs at rest (the
  /// waits' own cost, which the other cases include).
  func testIdle200() {
    let m = model()
    _ = hosted(m)
    measure(metrics: metrics) {
      spin(0.5)
    }
  }

  /// 10 publishes of a property the grid does not show (a status tick),
  /// each applied before the next. CPU time is the number: the wall clock
  /// is mostly the waits.
  func testPublishTicks200() {
    let m = model()
    _ = hosted(m)
    var n = 0
    measure(metrics: metrics) {
      for _ in 0..<10 {
        n += 1
        m.message = "tick \(n)"
        spin(0.02)
      }
    }
  }

  /// The face counts moved (faces streaming in): the list is replaced, and
  /// the grid's spring (0.3 s) runs to its end before the next. CPU time is
  /// the number, the animation's frames included.
  func testPeopleUpdate200() {
    let m = model()
    _ = hosted(m)
    var bump = 0
    measure(metrics: metrics) {
      bump += 1
      m.applyPeople(Self.people(bump: bump))
      spin(0.5)
    }
  }
}
