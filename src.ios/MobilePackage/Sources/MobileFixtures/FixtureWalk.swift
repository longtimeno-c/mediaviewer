// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A folder walk the app does by itself, for the frame meter with no test attached (XCUITest's own
// work on the main thread at each animation's end weighs on any transition it watches):
//   -fixtureState library -fixtureFrameMeter YES -fixtureWalk 3
// goes into the first folder, into its first folder, and back twice, that many times, 1.5 s apart,
// then writes the meter's figures for the walk, and how many changes it made, to Documents/walk.txt.
#if !DEBUG
#error("MobileFixtures is debug only and must not be built into a release")
#endif
import Foundation
import MobileUI

@MainActor
enum FixtureWalk {
  static func runIfAsked(_ model: AppModel) async {
    let rounds = UserDefaults.standard.integer(forKey: "fixtureWalk")
    guard rounds > 0 else { return }
    let library = model.library
    while library.root.subfolders.isEmpty { try? await Task.sleep(for: .milliseconds(100)) }
    try? await Task.sleep(for: .seconds(6))
    let before = FixtureFrameMeter.line ?? ""
    let pause = Duration.milliseconds(1500)
    for _ in 0..<rounds {
      guard let first = library.root.subfolders.first else { return }
      library.enter(first)
      try? await Task.sleep(for: pause)
      if let inner = library.current.subfolders.first { library.enter(inner) }
      try? await Task.sleep(for: pause)
      library.stack.removeLast()
      try? await Task.sleep(for: pause)
      library.stack.removeLast()
      try? await Task.sleep(for: pause)
    }
    let after = FixtureFrameMeter.line ?? ""
    let text = "changes=\(rounds * 4)\nbefore \(before)\nafter \(after)\n"
    if let docs = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first {
      try? Data(text.utf8).write(to: docs.appendingPathComponent("walk.txt"))
    }
  }
}
