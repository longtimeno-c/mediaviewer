// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A document's "Open in <app>" (docs/design/20): a PDF or DOCX has nothing to
// edit here, so the bar's Edit button becomes the apps that can open it. The
// default app comes first, MediaViewer is left out. Launch Services is asked
// off the main thread, once per file shown.
import AppKit
import Combine
import MVChromeBridge

@MainActor
final class OpenInStore: ObservableObject {
  static let shared = OpenInStore()

  struct App: Identifiable, Equatable {
    let url: URL
    let name: String
    var id: URL { url }
  }

  /// Default first, then the rest by name; empty until the lookup answers.
  @Published private(set) var apps: [App] = []
  private var path = ""
  private var lookup: Task<Void, Never>?

  private init() {}

  /// EditStore calls this on each poll while a document is on the canvas.
  func follow() {
    // The length returned leaves out the terminator.
    let need = mv_chrome_current_item_path(nil, 0)
    var buf = [CChar](repeating: 0, count: Int(max(need, 0)) + 1)
    _ = mv_chrome_current_item_path(&buf, Int32(buf.count))
    let current = String(cString: buf)
    guard current != path else { return }
    path = current
    apps = []
    lookup?.cancel()
    guard !current.isEmpty else { return }
    let file = URL(fileURLWithPath: current)
    lookup = Task.detached(priority: .userInitiated) {
      let found = OpenInStore.apps(for: file)
      await MainActor.run {
        let store = OpenInStore.shared
        if store.path == current { store.apps = found }
      }
    }
  }

  /// `nil`: the host picks the default (the same first row once it is known).
  func open(_ app: App?) {
    if let app {
      app.url.path.withCString { mv_chrome_open_in_app($0) }
    } else {
      mv_chrome_open_in_app(nil)
    }
  }

  nonisolated private static func apps(for file: URL) -> [App] {
    let ws = NSWorkspace.shared
    // Every MediaViewer build (release, .dev, a test copy) shares this prefix.
    let ours = "io.github.longtimeno-c.mediaviewer"
    var seen = Set<String>()
    func app(_ url: URL) -> App? {
      let id = Bundle(url: url)?.bundleIdentifier ?? url.path
      if id.hasPrefix(ours) || url.standardizedFileURL == Bundle.main.bundleURL.standardizedFileURL { return nil }
      guard seen.insert(id).inserted else { return nil }
      let name = FileManager.default.displayName(atPath: url.path)
      return App(url: url, name: name.hasSuffix(".app") ? String(name.dropLast(4)) : name)
    }
    var out: [App] = []
    if let def = ws.urlForApplication(toOpen: file), let a = app(def) { out.append(a) }
    let rest = ws.urlsForApplications(toOpen: file).compactMap(app)
      .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    out.append(contentsOf: rest)
    return out
  }
}
