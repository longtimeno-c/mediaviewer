// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The real app's shell: the screens and, once src/mobile is built, the real core. It links nothing
// from MobileFixtures, which is what keeps stand-in data out of a release.
import MobileKit
import MobileLink
import MobileUI
import SwiftUI

@main
struct MediaViewerApp: App {
  @State private var model = AppModel(services: PendingCore.services(), scanner: CameraScanSurface())

  init() { LinkConnection.registerBackgroundWork() }

  var body: some Scene {
    WindowGroup {
      RootView(model: model)
        #if DEBUG
        // The Simulator has no camera: a test can hand over the code a scan would have read.
        .task {
          if let code = ProcessInfo.processInfo.environment["MV_TEST_SCAN_CODE"], model.scanCount == 0 { model.scanned(code) }
        }
        #endif
    }
  }
}
