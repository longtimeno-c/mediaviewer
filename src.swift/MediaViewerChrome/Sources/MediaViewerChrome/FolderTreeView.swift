// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// PR 9 (docs/design/06, docs/design/16 Ctrl/Cmd+Shift+E): the folder tree, rooted at the open folder (not the computer). A node lists its subfolders each time it expands, in a
// background task through the host's portable `io::list_subdirectories` -- the
// main actor never touches the disk (rule 1). Choosing a folder opens it exactly
// as Open Folder does.
import AppKit
import MVChromeBridge
import SwiftUI

// `fetch` is a directory read: one call into a buffer that usually fits, not a
// size call and then a fill (two reads, and a folder made between them would
// cut the last row short). Only a listing longer than the buffer reads again,
// into one sized from that same read, until a whole listing fits.
private func readLines(_ fetch: (UnsafeMutablePointer<CChar>?, Int32) -> Int32) -> [(String, String)]? {
  var buf = [CChar](repeating: 0, count: 16 * 1024)
  while true {
    let needed = Int(buf.withUnsafeMutableBufferPointer { fetch($0.baseAddress, Int32($0.count)) })
    if needed < 0 { return nil }
    if needed < buf.count { break }
    buf = [CChar](repeating: 0, count: needed + 1)
  }
  return String(cString: buf).split(separator: "\n").compactMap { line in
    let f = line.split(separator: "\t", maxSplits: 1, omittingEmptySubsequences: false)
    return f.count == 2 ? (String(f[0]), String(f[1])) : nil
  }
}

@MainActor
final class TreeNode: ObservableObject, Identifiable {
  let name: String
  let path: String
  @Published var children: [TreeNode]?  // nil until first expanded
  /// Expanding lists the subfolders again (issue #190): a folder made or
  /// deleted since the last look shows the next time its parent opens. What
  /// was listed stays on screen until the new listing lands.
  @Published var isExpanded = false {
    didSet { if isExpanded && !oldValue { _ = refresh() } }
  }
  /// The listing in flight; a second caller awaits it rather than polling.
  private var listing: Task<Void, Never>?

  nonisolated var id: String { path }

  init(name: String, path: String) {
    self.name = name
    self.path = path
  }

  func loadIfNeeded() async {
    if children != nil && listing == nil { return }
    await refresh().value
  }

  /// Lists the subfolders off the main actor, or joins the listing already
  /// running. Children still listed keep their node (and so their own
  /// expansion); only what changed is published.
  @discardableResult
  func refresh() -> Task<Void, Never> {
    if let listing { return listing }
    let dir = path
    let task = Task { [weak self] in
      let list = await Task.detached(priority: .userInitiated) {
        readLines { mv_chrome_list_subdirectories(dir, $0, $1) } ?? []
      }.value
      guard let self else { return }
      self.listing = nil
      let old = Dictionary((self.children ?? []).map { ($0.path, $0) }, uniquingKeysWith: { a, _ in a })
      let next = list.map { old[$0.1] ?? TreeNode(name: $0.0, path: $0.1) }
      if self.children == nil || next.map(\.path) != self.children?.map(\.path) { self.children = next }
    }
    listing = task
    return task
  }

  /// Expands this node and each descendant named by `chain` (chain[0] is `path`).
  /// The last node, the folder just opened, is listed again even when it was
  /// listed before.
  func reveal(_ chain: [String]) async {
    guard chain.first == path else { return }
    if !isExpanded {
      isExpanded = true  // starts the listing
    } else if chain.count == 1 {
      refresh()
    }
    await loadIfNeeded()
    guard chain.count > 1, let kids = children else { return }
    let rest = Array(chain.dropFirst())
    if let child = kids.first(where: { $0.path == rest[0] }) {
      await child.reveal(rest)
    }
  }
}

@MainActor
final class FolderTreeStore: ObservableObject {
  static let shared = FolderTreeStore()

  /// Rooted at the highest folder reached, not the one on screen. Opening a
  /// child keeps this root and expands the path down to the open folder.
  @Published private(set) var root: TreeNode?
  @Published private(set) var currentPath = ""
  /// Bumps after the path down to `currentPath` has been expanded, so the row
  /// can be scrolled into view once it exists.
  @Published private(set) var scrollTick = 0
  @Published private(set) var visible = false
  /// docs/design/26: the Photos library was added in Settings and may be opened as a
  /// folder; whether its listing is what is on screen.
  @Published private(set) var photosAvailable = false
  @Published private(set) var photosOpen = false
  /// docs/plans/desktop-client.md: another computer's folders, one row per paired
  /// server, pinned above the open folder's tree; the remote folder on screen.
  @Published private(set) var servers: [RemoteServer] = []
  @Published private(set) var remoteOpen = ""
  private var serversGeneration: UInt64 = 0
  private var revealedKey = ""

  private init() {
    // Issue #178: polled when the main run loop has run (ChromePulse), not on a timer.
    ChromePulse.shared.add { [weak self] in
      self?.poll()
      return false
    }
  }

  var parentPath: String? {
    guard !currentPath.isEmpty, currentPath != "/" else { return nil }
    let parent = (currentPath as NSString).deletingLastPathComponent
    return parent.isEmpty ? "/" : parent
  }

  private func poll() {
    let nowVisible = mv_chrome_tree_visible()
    if nowVisible != visible {
      visible = nowVisible
      // Shown again: reveal the open folder afresh, which lists it again.
      if nowVisible { revealedKey = "" }
    }
    guard nowVisible else { return }
    let photos = mv_chrome_photos_library_available()
    if photos != photosAvailable { photosAvailable = photos }
    let photosShown = mv_chrome_photos_library_open()
    if photosShown != photosOpen { photosOpen = photosShown }
    let gen = mv_chrome_remote_generation()
    if gen != serversGeneration {
      serversGeneration = gen
      let rows = readServerLines()
      if rows != servers.map(\.plain) {
        let old = Dictionary(servers.map { ($0.key, $0) }, uniquingKeysWith: { a, _ in a })
        servers = rows.map { r in
          let s = old[r.key] ?? RemoteServer(name: r.name, key: r.key)
          s.state = r.state
          s.role = r.role
          return s
        }
      }
    }
    var keyBuf = [CChar](repeating: 0, count: 4096)
    _ = keyBuf.withUnsafeMutableBufferPointer { mv_chrome_remote_open_key($0.baseAddress, Int32($0.count)) }
    let openKey = String(cString: keyBuf)
    if openKey != remoteOpen { remoteOpen = openKey }
    let crumbs = readCrumbs()
    // Only what moved is published: a poll runs on every pass of the main run
    // loop while the tree is up, and an unchanged write would redraw it.
    guard let first = crumbs.first else {
      if root != nil { root = nil }
      if !currentPath.isEmpty { currentPath = "" }
      revealedKey = ""
      return
    }
    let path = crumbs.last?.path ?? first.path
    if path != currentPath { currentPath = path }
    if root?.path != first.path {
      root = TreeNode(name: first.name, path: first.path)
      revealedKey = ""
    }
    let key = crumbs.map(\.path).joined(separator: "\n")
    guard key != revealedKey, let root else { return }
    revealedKey = key
    let chain = crumbs.map(\.path)
    Task {
      await root.reveal(chain)
      scrollTick += 1
    }
  }

  private func readServerLines() -> [RemoteServer.Plain] {
    var buf = [CChar](repeating: 0, count: 8 * 1024)
    while true {
      let needed = Int(buf.withUnsafeMutableBufferPointer { mv_chrome_remote_servers($0.baseAddress, Int32($0.count)) })
      if needed < 0 { return [] }
      if needed < buf.count { break }
      buf = [CChar](repeating: 0, count: needed + 1)
    }
    return String(cString: buf).split(separator: "\n").compactMap { line in
      let f = line.split(separator: "\t", omittingEmptySubsequences: false)
      guard f.count >= 3 else { return nil }
      return RemoteServer.Plain(name: String(f[0]), key: String(f[1]), state: String(f[2]),
                                role: f.count >= 4 ? String(f[3]) : "")
    }
  }

  private func readCrumbs() -> [(name: String, path: String)] {
    var out: [(String, String)] = []
    var buf = [CChar](repeating: 0, count: 4096)
    let count = Int(mv_chrome_crumb_count())
    for i in 0..<count {
      let ok = buf.withUnsafeMutableBufferPointer { ptr -> Bool in
        guard let base = ptr.baseAddress else { return false }
        return mv_chrome_crumb_path(Int32(i), base, Int32(ptr.count))
      }
      guard ok else { continue }
      let path = String(cString: buf)
      let leaf = path == "/" ? "/" : (path as NSString).lastPathComponent
      out.append((leaf.isEmpty ? path : leaf, path))
    }
    return out
  }
}

/// One paired server (docs/plans/desktop-client.md): its node lists the named
/// folders through mv_chrome_list_subdirectories, off the main actor.
@MainActor
final class RemoteServer: ObservableObject, Identifiable {
  struct Plain: Equatable {
    let name: String
    let key: String
    let state: String
    var role = ""
  }
  let name: String
  let key: String
  let node: TreeNode
  @Published var state = "unknown"
  /// What the server lets this Mac do ("view", "change", "admin"; "" not known yet).
  @Published var role = ""
  nonisolated var id: String { key }
  var plain: Plain { Plain(name: name, key: key, state: state, role: role) }

  init(name: String, key: String) {
    self.name = name
    self.key = key
    node = TreeNode(name: name, path: key)
  }

  /// What the row says beside the name when the server is not simply there.
  var note: String? {
    switch state {
    case "unreachable": return "Can't reach it"
    case "access_ended": return "Access ended"
    case "wrong_service": return "Not the paired server"
    case "too_old": return "Needs an update"
    default: return RemoteServer.roleWords(role)
    }
  }

  /// The role the server gave this Mac (#686 §2), said only while nothing is wrong.
  static func roleWords(_ role: String) -> String? {
    switch role {
    case "view": return "View only"
    case "change": return "Can change files"
    case "admin": return "Admin"
    default: return nil
    }
  }
}

private struct RemoteServerRow: View {
  @ObservedObject var server: RemoteServer
  @ObservedObject var node: TreeNode
  let current: String

  init(server: RemoteServer, current: String) {
    self.server = server
    self.node = server.node
    self.current = current
  }

  var body: some View {
    DisclosureGroup(isExpanded: $node.isExpanded) {
      if let kids = node.children {
        ForEach(kids) { TreeRow(node: $0, current: current, cursor: nil, depth: 1) }
      }
    } label: {
      Button {
        node.isExpanded = true  // a server's top is its folders: open the row
      } label: {
        HStack(spacing: MVTheme.space(6)) {
          Image(systemName: "externaldrive.connected.to.line.below")
          Text(server.name).lineLimit(1).truncationMode(.middle)
          if let note = server.note {
            Text(note).foregroundStyle(MVTheme.body).lineLimit(1)
          }
          Spacer(minLength: 0)
        }
        .padding(.horizontal, MVTheme.space(6))
        .padding(.vertical, MVTheme.space(2))
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .foregroundStyle(current.hasPrefix(server.key) ? MVTheme.accent : MVTheme.title)
      .fontWeight(current.hasPrefix(server.key) ? .semibold : .regular)
      .help("\(server.name): another computer's folders, read-only from here.")
      .folderDropTarget(server.key, name: server.name, radius: MVTheme.radius(4)) { [node] in
        node.isExpanded = true
      }
      .contextMenu {
        Button("Forget This Server") { server.key.withCString { mv_chrome_remote_forget($0) } }
      }
    }
    .id(server.key)
  }
}

private struct TreeRow: View {
  @ObservedObject var node: TreeNode
  let current: String
  /// The keyboard's row while the tree has focus (issue #185); nil otherwise.
  let cursor: String?
  let depth: Int

  var body: some View {
    DisclosureGroup(isExpanded: $node.isExpanded) {
      if let kids = node.children {
        ForEach(kids) { TreeRow(node: $0, current: current, cursor: cursor, depth: depth + 1) }
      }
    } label: {
      Button {
        node.path.withCString { mv_chrome_open_folder($0) }
      } label: {
        HStack(spacing: MVTheme.space(6)) {
          Image(systemName: node.path == current ? "folder.fill" : "folder")
          Text(node.name).lineLimit(1).truncationMode(.middle)
          Spacer(minLength: 0)
        }
        // The keyboard row's wash is inset round its contents: the icon keeps
        // 6 from its edge rather than touching it (docs/design/20 "Craft rules").
        .padding(.horizontal, MVTheme.space(6))
        .padding(.vertical, MVTheme.space(2))
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .fontWeight(node.path == current ? .semibold : .regular)
      .foregroundStyle(node.path == current ? MVTheme.accent : MVTheme.title)
      .background(
        RoundedRectangle(cornerRadius: MVTheme.radius(4))
          .fill(node.path == cursor ? MVTheme.title.opacity(0.1) : .clear)  // the theme's ink, as before
      )
      // docs/design/16 "Drag and drop into folders": files dropped on a row go
      // into that folder; resting on a closed one opens it in the tree.
      .folderDropTarget(node.path, name: node.name, radius: MVTheme.radius(4)) { [node] in
        node.isExpanded = true
      }
      .accessibilityAddTraits(node.path == cursor ? [.isSelected] : [])
    }
    .id(node.path)
  }
}

struct FolderTreeView: View {
  @ObservedObject private var store = FolderTreeStore.shared
  // docs/design/16 "Folder tree" (issue #185): ⌘⇧E shows and focuses the tree.
  // ↑ ↓ walk the shown rows, → ← open and close a folder, Enter opens it and
  // returns to the canvas, Esc returns to the canvas (a second Esc closes it).
  @FocusState private var focused: Bool
  @State private var cursor: String?

  /// The rows on screen, top to bottom: the root, then each expanded folder's
  /// children. Walked on a key press only, never per render.
  private func shownNodes() -> [TreeNode] {
    guard let root = store.root else { return [] }
    var out: [TreeNode] = []
    var stack: [TreeNode] = [root]
    while let node = stack.popLast() {
      out.append(node)
      if node.isExpanded, let kids = node.children { stack.append(contentsOf: kids.reversed()) }
    }
    return out
  }

  private func takeFocus() {
    let shown = shownNodes()
    if cursor == nil || !shown.contains(where: { $0.path == cursor }) {
      cursor = shown.contains(where: { $0.path == store.currentPath }) ? store.currentPath : shown.first?.path
    }
    focused = true
  }

  private func move(_ by: Int) -> KeyPress.Result {
    let shown = shownNodes()
    guard !shown.isEmpty else { return .ignored }
    let at = shown.firstIndex { $0.path == cursor } ?? 0
    cursor = shown[min(max(at + by, 0), shown.count - 1)].path
    return .handled
  }

  /// → opens a closed folder, else steps into its first child.
  private func openOrEnter() -> KeyPress.Result {
    let shown = shownNodes()
    guard let node = shown.first(where: { $0.path == cursor }) else { return move(0) }
    if !node.isExpanded {
      node.isExpanded = true  // lists its children off the main actor
    } else if let first = node.children?.first {
      cursor = first.path
    }
    return .handled
  }

  /// ← closes an open folder, else steps out to its parent.
  private func closeOrLeave() -> KeyPress.Result {
    let shown = shownNodes()
    guard let node = shown.first(where: { $0.path == cursor }) else { return move(0) }
    if node.isExpanded && node.children?.isEmpty == false {
      node.isExpanded = false
    } else if let parent = shown.first(where: { $0.children?.contains { $0 === node } == true }) {
      cursor = parent.path
    }
    return .handled
  }

  private func openCursor() -> KeyPress.Result {
    guard let path = cursor else { return .ignored }
    path.withCString { mv_chrome_open_folder($0) }
    mv_chrome_tree_blur()
    return .handled
  }

  /// docs/design/26: the Photos library, a folder beside the folders, once it was
  /// added in Settings. Opening it lists the whole library.
  private var photosRow: some View {
    Button {
      mv_chrome_open_photos_library()
    } label: {
      HStack(spacing: MVTheme.space(6)) {
        Image(systemName: store.photosOpen ? "photo.on.rectangle.angled.fill" : "photo.on.rectangle.angled")
        Text("Photos Library").lineLimit(1)
        Spacer(minLength: 0)
      }
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .fontWeight(store.photosOpen ? .semibold : .regular)
    .foregroundStyle(store.photosOpen ? MVTheme.accent : MVTheme.title)
    .help("Your Photos library, iCloud Photos included, as a folder. Read-only: nothing in it is changed.")
    .folderDropTarget("photos:library", name: "Photos Library", radius: MVTheme.radius(4))
    .padding(.horizontal, MVTheme.space(10))
    .padding(.vertical, MVTheme.space(8))
  }

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      Text("Folders").font(MVTheme.font()).fontWeight(.semibold).foregroundStyle(MVTheme.title).padding(.horizontal, MVTheme.space(14)).padding(.top, MVTheme.space(12)).padding(.bottom, MVTheme.space(8))
      Divider()
      PlacesList()
      Divider()
      if store.photosAvailable {
        photosRow
        Divider()
      }
      if !store.servers.isEmpty {
        VStack(alignment: .leading, spacing: MVTheme.space(2)) {
          ForEach(store.servers) { RemoteServerRow(server: $0, current: store.remoteOpen) }
        }
        .padding(.horizontal, MVTheme.space(10))
        .padding(.vertical, MVTheme.space(6))
        Divider()
      }
      if let root = store.root {
        ScrollViewReader { proxy in
          ScrollView {
            LazyVStack(alignment: .leading, spacing: MVTheme.space(2)) {
              if let parent = store.parentPath {
                Button {
                  parent.withCString { mv_chrome_open_folder($0) }
                } label: {
                  Label("Up to \((parent as NSString).lastPathComponent.isEmpty ? "/" : (parent as NSString).lastPathComponent)",
                        systemImage: "arrow.up.left")
                    .foregroundStyle(MVTheme.body)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .padding(.bottom, MVTheme.space(6))
              }
              TreeRow(node: root, current: store.currentPath, cursor: focused ? cursor : nil, depth: 0)
            }
            .padding(MVTheme.space(10))
          }
          .onChange(of: store.scrollTick) { _, _ in
            let path = store.currentPath
            guard !path.isEmpty else { return }
            proxy.scrollTo(path, anchor: .center)
          }
          .onChange(of: cursor) { _, path in
            if let path, focused { proxy.scrollTo(path) }
          }
        }
      } else if !store.photosOpen && store.remoteOpen.isEmpty {
        Text("No folder open").foregroundStyle(MVTheme.body).padding(MVTheme.space(14))
        Spacer()
      } else {
        Spacer()
      }
    }
    .font(MVTheme.font(14))  // rows and the Up button follow a theme's font
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(.regularMaterial)
    .overlay(alignment: .trailing) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
    .focusable()
    .focusEffectDisabled()
    .focused($focused)
    .onKeyPress(.upArrow) { move(-1) }
    .onKeyPress(.downArrow) { move(1) }
    .onKeyPress(.rightArrow) { openOrEnter() }
    .onKeyPress(.leftArrow) { closeOrLeave() }
    .onKeyPress(.return) { openCursor() }
    .onKeyPress(.escape) { mv_chrome_tree_blur(); return .handled }
    .onAppear { if store.visible { takeFocus() } }
    .onChange(of: store.visible) { _, nowVisible in if nowVisible { takeFocus() } }
  }
}

/// docs/design/16 "Folder navigation": Finder's sidebar places above the tree --
/// the usual folders of this Mac and every mounted volume (a card, a drive, a
/// share). Volumes are read off the main thread and again when one mounts or
/// ejects. A click opens the place as Open Folder does; each row takes drops.
@MainActor
final class PlacesStore: ObservableObject {
  static let shared = PlacesStore()

  struct Place: Identifiable, Equatable {
    let name: String
    let path: String
    let symbol: String
    var id: String { path }
  }

  /// Paths from the system, not the disk: no I/O on the main thread.
  let folders: [Place]
  @Published private(set) var volumes: [Place] = []
  private var observers: [NSObjectProtocol] = []

  private init() {
    let fm = FileManager.default
    let home = fm.homeDirectoryForCurrentUser
    func place(_ dir: FileManager.SearchPathDirectory, _ name: String, _ symbol: String) -> Place? {
      fm.urls(for: dir, in: .userDomainMask).first.map { Place(name: name, path: $0.path, symbol: symbol) }
    }
    folders = [
      Place(name: home.lastPathComponent, path: home.path, symbol: "house"),
      place(.desktopDirectory, "Desktop", "menubar.dock.rectangle"),
      place(.picturesDirectory, "Pictures", "photo"),
      place(.moviesDirectory, "Movies", "film"),
      place(.downloadsDirectory, "Downloads", "arrow.down.circle"),
    ].compactMap { $0 }
    let center = NSWorkspace.shared.notificationCenter
    for name in [NSWorkspace.didMountNotification, NSWorkspace.didUnmountNotification, NSWorkspace.didRenameVolumeNotification] {
      observers.append(center.addObserver(forName: name, object: nil, queue: .main) { _ in
        MainActor.assumeIsolated { PlacesStore.shared.reloadVolumes() }
      })
    }
    reloadVolumes()
  }

  func reloadVolumes() {
    Task {
      let found = await Task.detached(priority: .utility) { () -> [Place] in
        let keys: [URLResourceKey] = [.volumeNameKey, .volumeIsInternalKey, .volumeIsRootFileSystemKey]
        let urls = FileManager.default.mountedVolumeURLs(includingResourceValuesForKeys: keys,
                                                         options: [.skipHiddenVolumes]) ?? []
        return urls.compactMap { url in
          let v = try? url.resourceValues(forKeys: Set(keys))
          let name = v?.volumeName ?? url.lastPathComponent
          let root = v?.volumeIsRootFileSystem ?? false
          let symbol = root ? "internaldrive" : (v?.volumeIsInternal ?? false) ? "internaldrive" : "externaldrive"
          return Place(name: name.isEmpty ? url.path : name, path: url.path, symbol: symbol)
        }
      }.value
      if found != self.volumes { self.volumes = found }
    }
  }
}

private struct PlacesList: View {
  @ObservedObject private var places = PlacesStore.shared
  @ObservedObject private var tree = FolderTreeStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(2)) {
      ForEach(places.folders) { PlaceRow(place: $0, current: tree.currentPath) }
      if !places.volumes.isEmpty {
        Text("Locations")
          .font(MVTheme.font(11))
          .fontWeight(.semibold)
          .foregroundStyle(MVTheme.body)
          .padding(.top, MVTheme.space(6))
          .padding(.horizontal, MVTheme.space(6))
        ForEach(places.volumes) { PlaceRow(place: $0, current: tree.currentPath) }
      }
    }
    .padding(.horizontal, MVTheme.space(10))
    .padding(.vertical, MVTheme.space(6))
  }
}

private struct PlaceRow: View {
  let place: PlacesStore.Place
  let current: String

  var body: some View {
    let here = current == place.path
    Button {
      place.path.withCString { mv_chrome_open_folder($0) }
    } label: {
      HStack(spacing: MVTheme.space(6)) {
        Image(systemName: place.symbol).frame(width: 16)
        Text(place.name).lineLimit(1).truncationMode(.middle)
        Spacer(minLength: 0)
      }
      .padding(.horizontal, MVTheme.space(6))
      .padding(.vertical, MVTheme.space(2))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .fontWeight(here ? .semibold : .regular)
    .foregroundStyle(here ? MVTheme.accent : MVTheme.title)
    .help(place.path)
    .folderDropTarget(place.path, name: place.name, radius: MVTheme.radius(4))
  }
}
