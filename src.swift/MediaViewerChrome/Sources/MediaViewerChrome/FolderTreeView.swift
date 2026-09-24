// SPDX-License-Identifier: GPL-2.0-or-later
// PR 9 (plan/06, plan/16 Ctrl/Cmd+Shift+E): the folder tree, rooted at the open folder (not the computer). A node lists its subfolders on first expand, in a
// background task through the host's portable `io::list_subdirectories` -- the
// main actor never touches the disk (rule 1). Choosing a folder opens it exactly
// as Open Folder does.
import AppKit
import MVChromeBridge
import SwiftUI

private func readLines(_ fetch: (UnsafeMutablePointer<CChar>?, Int32) -> Int32) -> [(String, String)]? {
  let needed = Int(fetch(nil, 0))
  if needed < 0 { return nil }
  if needed == 0 { return [] }
  var buf = [CChar](repeating: 0, count: needed + 1)
  _ = buf.withUnsafeMutableBufferPointer { fetch($0.baseAddress, Int32($0.count)) }
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
  @Published var isExpanded = false {
    didSet { if isExpanded { Task { await loadIfNeeded() } } }
  }
  private var loading = false

  nonisolated var id: String { path }

  init(name: String, path: String) {
    self.name = name
    self.path = path
  }

  func loadIfNeeded() async {
    if children != nil { return }
    if loading {
      while loading && children == nil {
        try? await Task.sleep(nanoseconds: 20_000_000)
      }
      return
    }
    loading = true
    let dir = path
    let list = await Task.detached(priority: .userInitiated) {
      readLines { mv_chrome_list_subdirectories(dir, $0, $1) } ?? []
    }.value
    children = list.map { TreeNode(name: $0.0, path: $0.1) }
    loading = false
  }

  /// Expands this node and each descendant named by `chain` (chain[0] is `path`).
  func reveal(_ chain: [String]) async {
    guard chain.first == path else { return }
    if !isExpanded { isExpanded = true }
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
  private var timer: Timer?
  private var revealedKey = ""

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
  }

  var parentPath: String? {
    guard !currentPath.isEmpty, currentPath != "/" else { return nil }
    let parent = (currentPath as NSString).deletingLastPathComponent
    return parent.isEmpty ? "/" : parent
  }

  private func poll() {
    let nowVisible = mv_chrome_tree_visible()
    if nowVisible != visible { visible = nowVisible }
    guard nowVisible else { return }
    let crumbs = readCrumbs()
    guard let first = crumbs.first else {
      root = nil
      currentPath = ""
      revealedKey = ""
      return
    }
    currentPath = crumbs.last?.path ?? first.path
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

private struct TreeRow: View {
  @ObservedObject var node: TreeNode
  let current: String
  let depth: Int

  var body: some View {
    DisclosureGroup(isExpanded: $node.isExpanded) {
      if let kids = node.children {
        ForEach(kids) { TreeRow(node: $0, current: current, depth: depth + 1) }
      }
    } label: {
      Button {
        node.path.withCString { mv_chrome_open_folder($0) }
      } label: {
        HStack(spacing: 6) {
          Image(systemName: node.path == current ? "folder.fill" : "folder")
          Text(node.name).lineLimit(1).truncationMode(.middle)
          Spacer(minLength: 0)
        }
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .fontWeight(node.path == current ? .semibold : .regular)
      .foregroundStyle(node.path == current ? Color.accentColor : Color.primary)
    }
    .id(node.path)
  }
}

struct FolderTreeView: View {
  @ObservedObject private var store = FolderTreeStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      Text("Folders").font(.headline).padding(.horizontal, 14).padding(.top, 12).padding(.bottom, 8)
      Divider()
      if let root = store.root {
        ScrollViewReader { proxy in
          ScrollView {
            LazyVStack(alignment: .leading, spacing: 2) {
              if let parent = store.parentPath {
                Button {
                  parent.withCString { mv_chrome_open_folder($0) }
                } label: {
                  Label("Up to \((parent as NSString).lastPathComponent.isEmpty ? "/" : (parent as NSString).lastPathComponent)",
                        systemImage: "arrow.up.left")
                    .foregroundStyle(.secondary)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .padding(.bottom, 6)
              }
              TreeRow(node: root, current: store.currentPath, depth: 0)
            }
            .padding(10)
          }
          .onChange(of: store.scrollTick) { _, _ in
            let path = store.currentPath
            guard !path.isEmpty else { return }
            proxy.scrollTo(path, anchor: .center)
          }
        }
      } else {
        Text("No folder open").foregroundStyle(.secondary).padding(14)
        Spacer()
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(.regularMaterial)
    .overlay(alignment: .trailing) { Rectangle().fill(Color.primary.opacity(0.15)).frame(width: 1) }
  }
}
