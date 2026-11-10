// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The real app's shell: the screens and, once src/mobile is built, the real core. It links nothing
// from MobileFixtures, which is what keeps stand-in data out of a release.
import MobileKit
import MobileUI
import SwiftUI

@main
struct MediaViewerApp: App {
  @State private var model = AppModel(services: PendingCore.services(), scanner: CameraScanSurface())

  var body: some Scene {
    WindowGroup { RootView(model: model) }
  }
}
