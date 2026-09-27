// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// AI.bundle's principal class (plan/17 "UI and commands"; the chrome brief):
// the host (src/shell/addons_mac.mm) instantiates it with NSBundle and talks to
// it by message send only. It owns the search panel, the Settings management
// view, and everything they read from the pack through mv.ai.1.
//
// From the host:  -attachWithTable:host:, -deliverEvent:status:identifier:payload:,
//                 -shutdown (the selectors Import's chrome answers too), and the
//                 optional -runCommand:, -folderChanged:, -itemChanged:, -settingsView,
//                 and the gallery search bar's -galleryAccessory, -galleryVisible:,
//                 -galleryQuery: (2026-09-27).
// To the host:    selectors on the host object (MvAddonHostMac): openList:,
//                 closeList, viewerState, seekTo:, setMarkers:, viewerWindow,
//                 galleryAnswer:.
// The chrome never calls a host dispatcher from native code, and nothing here
// logs a path, a query, or a name (rule 6).
import AppKit
import CAiApi
import SwiftUI

@objc(MVAIChrome)
public final class MVAIChrome: NSObject {
  private var table: AITable?
  private var host: NSObject?
  private var search: SearchModel?
  private var manage: ManagementModel?
  private var panel: SearchPanelController?
  private var settingsHost: NSView?
  private var folder = ""
  // The gallery search bar (plan/17, 2026-09-27): its index control and its
  // own search, apart from the panel's so neither stomps the other's query.
  private var galleryIndex: GalleryIndexModel?
  private var galleryHost: NSView?
  private var gallerySearch: SearchModel?
  private var galleryShown = false
  /// The model whose results the viewer lists now: its clip matches are the
  /// scrub markers and N / Shift+N.
  private weak var lister: SearchModel?

  @objc public override init() { super.init() }

  @objc(attachWithTable:host:)
  public func attach(table: NSValue, host: NSObject) {
    guard let raw = table.pointerValue else { return }
    self.table = AITable(raw.assumingMemoryBound(to: mv_ai_api.self))
    self.host = host
  }

  @MainActor private func searchModel() -> SearchModel? {
    if let search { return search }
    guard let table else { return nil }
    let m = SearchModel(table: table)
    m.chrome = self
    if !folder.isEmpty { m.folderChanged(folder) }
    search = m
    return m
  }

  @MainActor private func managementModel() -> ManagementModel? {
    if let manage { return manage }
    guard let table else { return nil }
    let m = ManagementModel(table: table)
    m.chrome = self
    manage = m
    return m
  }

  @MainActor private func panelController() -> SearchPanelController? {
    if let panel { return panel }
    guard let model = searchModel() else { return nil }
    let p = SearchPanelController(model: model)
    panel = p
    return p
  }

  // MARK: from the host

  @objc(deliverEvent:status:identifier:payload:)
  public func deliver(event: UInt32, status: UInt32, identifier: UInt64, payload: Int64) {
    MainActor.assumeIsolated {
      switch event {
      case MV_ADDON_EVENT_AI_SEARCH_DONE.rawValue:
        // To its owner only: a model releases an answer it does not own.
        if let g = gallerySearch, g.owns(identifier) {
          g.searchDone(identifier, status: status, count: payload)
        } else {
          search?.searchDone(identifier, status: status, count: payload)
        }
      case MV_ADDON_EVENT_AI_STATUS.rawValue:
        search?.pollStatus()
        manage?.statusChanged()
        galleryIndex?.statusChanged()
      case MV_ADDON_EVENT_AI_ROOTS.rawValue:
        search?.refreshCoverage()
        manage?.reloadRoots()
        galleryIndex?.rootsChanged()
      case MV_ADDON_EVENT_AI_COMPUTE.rawValue:
        manage?.reloadSettings()
        manage?.statusChanged()
      case MV_ADDON_EVENT_AI_PEOPLE.rawValue:
        manage?.reloadPeople()
      default:
        break  // Import's kinds (1-7) never reach this chrome
      }
    }
  }

  @objc(runCommand:)
  public func runCommand(_ name: String) -> Bool {
    MainActor.assumeIsolated {
      switch name {
      case "search_open":
        guard let p = panelController() else { return false }
        p.show(over: viewerWindow())
        return true
      case "search_similar":
        let v = viewerState()
        let path = v["path"] as? String ?? ""
        guard !path.isEmpty, let m = searchModel(), let p = panelController() else { return false }
        m.findSimilar(path: path, isVideo: v["video"] as? Bool ?? false,
                      positionMs: int64(v["position_ms"]))
        p.show(over: viewerWindow())
        return true
      case "search_next_match", "search_prev_match":
        let v = viewerState()
        guard v["video"] as? Bool == true, let m = lister ?? searchModel() else { return false }
        return m.stepMatch(forward: name == "search_next_match", path: v["path"] as? String ?? "",
                           positionMs: int64(v["position_ms"]))
      // The gallery search bar's "This folder is not indexed yet" buttons.
      case "gallery_index_folder", "gallery_index_tree":
        guard !folder.isEmpty, let g = galleryIndexModel() else { return false }
        g.index(recursive: name == "gallery_index_tree")
        return true
      // The command bar pill's "Index anyway" while it waits on battery.
      case "index_anyway":
        guard let table else { return false }
        table.indexAnyway()
        search?.pollStatus()
        manage?.statusChanged()
        return true
      default:
        return false
      }
    }
  }

  @objc(folderChanged:)
  public func folderChanged(_ dir: String) {
    MainActor.assumeIsolated {
      folder = dir
      galleryIndex?.folderChanged(dir)
      if let search {
        search.folderChanged(dir)
      } else if let table, !dir.isEmpty {
        // Before the panel was ever opened: still let a covered root queue its delta.
        _ = table.a.note_folder_opened?(table.ctx, dir)
      }
    }
  }

  @objc(itemChanged:)
  public func itemChanged(_ path: String) {
    MainActor.assumeIsolated {
      guard let m = lister ?? search else { return }  // no search yet: no markers
      let v = path.isEmpty ? [:] : viewerState()
      m.itemChanged(path, isVideo: v["video"] as? Bool ?? false)
    }
  }

  /// Settings → Local search: the management view. Kept, so the base Settings
  /// can re-embed the same view. It answers its height for a width
  /// (-fittingHeightForWidth:) and says when that changes (SettingsHostView).
  @objc public func settingsView() -> NSView {
    MainActor.assumeIsolated {
      if let settingsHost { return settingsHost }
      guard let m = managementModel() else { return NSView() }
      let host = SettingsHostView(ManagementView(model: m))
      settingsHost = host
      return host
    }
  }

  // MARK: the gallery search bar (plan/17 "Gallery search bar", 2026-09-27)

  @MainActor private func galleryIndexModel() -> GalleryIndexModel? {
    if let galleryIndex { return galleryIndex }
    guard let table else { return nil }
    let m = GalleryIndexModel(table: table)
    m.folderChanged(folder)
    if galleryShown { m.setVisible(true) }
    galleryIndex = m
    return m
  }

  /// The index control for the right end of the gallery's field. Kept, like
  /// the Settings view; it sizes itself.
  @objc public func galleryAccessory() -> NSView {
    MainActor.assumeIsolated {
      if let galleryHost { return galleryHost }
      guard let m = galleryIndexModel() else { return NSView() }
      let hosting = NSHostingView(rootView: GalleryIndexControl(model: m))
      hosting.sizingOptions = [.intrinsicContentSize]
      galleryHost = hosting
      return hosting
    }
  }

  /// The grid was shown or hidden: the control polls only while it is shown.
  @objc(galleryVisible:)
  public func galleryVisible(_ visible: NSNumber) {
    MainActor.assumeIsolated {
      galleryShown = visible.boolValue
      galleryIndex?.setVisible(galleryShown)
    }
  }

  /// Contents mode: {"text", "folder", "seq"}. @1 when the folder is not in
  /// the index (nothing runs: the bar offers to index it); @0 when the search
  /// runs, its answer coming back through the host's -galleryAnswer:.
  @objc(galleryQuery:)
  public func galleryQuery(_ request: NSDictionary) -> NSNumber {
    MainActor.assumeIsolated {
      let text = (request["text"] as? String ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
      let dir = request["folder"] as? String ?? ""
      let seq = (request["seq"] as? NSNumber)?.uint64Value ?? 0
      guard !text.isEmpty, !dir.isEmpty, seq != 0, let table, let index = galleryIndexModel() else {
        return NSNumber(value: -1)
      }
      var state: UInt32 = 0
      guard table.call({ table.a.folder_coverage?(table.ctx, dir, &state) }) == MV_OK, state != 0 else {
        return NSNumber(value: 1)
      }
      let m: SearchModel
      if let gallerySearch {
        m = gallerySearch
      } else {
        m = SearchModel(table: table)
        m.chrome = self
        gallerySearch = m
      }
      // The folder's own root, or a recursive one above it: its subfolders too.
      let tree = index.folder == dir && index.recursive
      m.galleryQuery(text, dir: dir, tree: tree, seq: seq)
      return NSNumber(value: 0)
    }
  }

  func galleryAnswer(seq: UInt64, state: Int32, count: Int) {
    let sel = NSSelectorFromString("galleryAnswer:")
    guard let host, host.responds(to: sel) else { return }
    let answer: NSDictionary = [
      "seq": NSNumber(value: seq),
      "state": NSNumber(value: state),
      "count": NSNumber(value: count),
    ]
    _ = host.perform(sel, with: answer)
  }

  /// Before the host unloads the pack. Closes the table: a detached read
  /// still running fails instead of calling into an unloaded library, and
  /// this waits (at most 2 s) for the calls already inside the pack.
  @objc public func shutdown() {
    _ = close(wait: 2)
  }

  /// Quit: the same, without waiting for a read still inside the pack (a
  /// roots or people read can take seconds on a large index). True when none
  /// was in flight; false tells the host to leave the pack running for the
  /// process exit rather than free it under that read.
  @objc public func shutdownForQuit() -> Bool {
    close(wait: 0)
  }

  private func close(wait: TimeInterval) -> Bool {
    guard Thread.isMainThread else {
      return DispatchQueue.main.sync { self.close(wait: wait) }
    }
    return MainActor.assumeIsolated {
      panel?.close()
      panel = nil
      search?.disappeared()
      search?.releaseAll()
      search = nil
      manage?.stop()
      manage = nil
      settingsHost = nil
      gallerySearch?.releaseAll()
      gallerySearch = nil
      galleryIndex?.stop()
      galleryIndex = nil
      galleryHost = nil
      lister = nil
      hostSetMarkers(path: "", ms: [], current: -1)
      let idle = table?.close(timeout: wait) ?? true
      table = nil
      host = nil
      return idle
    }
  }

  // MARK: to the host (selectors on MvAddonHostMac)

  func viewerState() -> [String: Any] {
    let sel = NSSelectorFromString("viewerState")
    guard let host, host.responds(to: sel),
          let v = host.perform(sel)?.takeUnretainedValue() as? [String: Any] else { return [:] }
    return v
  }

  func viewerWindow() -> NSWindow? {
    let sel = NSSelectorFromString("viewerWindow")
    guard let host, host.responds(to: sel) else { return NSApp.mainWindow }
    return host.perform(sel)?.takeUnretainedValue() as? NSWindow
  }

  @MainActor func hostOpenList(_ request: NSDictionary, from model: SearchModel) -> Bool {
    let sel = NSSelectorFromString("openList:")
    guard let host, host.responds(to: sel),
          let ok = host.perform(sel, with: request)?.takeUnretainedValue() as? NSNumber,
          ok.boolValue else { return false }
    panel?.hide()
    lister = model
    return true
  }

  func hostSetMarkers(path: String, ms: [Int64], current: Int) {
    let sel = NSSelectorFromString("setMarkers:")
    guard let host, host.responds(to: sel) else { return }
    let request: NSDictionary = [
      "path": path,
      "ms": ms.map { NSNumber(value: $0) },
      "current": NSNumber(value: current),
    ]
    _ = host.perform(sel, with: request)
  }

  func hostSeek(_ ms: Int64) {
    let sel = NSSelectorFromString("seekTo:")
    guard let host, host.responds(to: sel) else { return }
    _ = host.perform(sel, with: NSNumber(value: ms))
  }

  /// Settings → Precision changed: the panel's open search answers again.
  /// The gallery bar's picks it up with its next query.
  @MainActor func precisionChanged() { search?.precisionChanged() }

  /// People → "Show photos": the person as a search, in the panel.
  @MainActor func showPerson(id: UInt64, name: String) {
    guard let m = searchModel(), let p = panelController() else { return }
    m.showPerson(id: id, name: name)
    p.show(over: viewerWindow())
  }
}
