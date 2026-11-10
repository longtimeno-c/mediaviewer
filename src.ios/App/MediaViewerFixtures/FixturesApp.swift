// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The interface on stand-in services, for the Simulator and for checking every state. Never
// archived: MobileFixtures does not compile outside DEBUG.
import MobileFixtures
import SwiftUI

@main
struct FixturesApp: App {
  var body: some Scene {
    WindowGroup { FixtureHost() }
  }
}
