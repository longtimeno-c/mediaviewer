// SPDX-License-Identifier: GPL-2.0-or-later
// PR 9 (plan/06, plan/16 Ctrl/Cmd+Shift+E): the folder tree. Roots are the home
// folder and mounted volumes; a node lists its subfolders on first expand, in a
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
    guard children == nil, !loading else { return }
    loading = true
    let dir = path
    let list = await Task.detached(priority: .userInitiated) {
      readLines { mv_chrome_list_subdirectories(dir, $0, $1) } ?? []
    }.value
    children = list.map { TreeNode(name: $0.0, path: $0.1) }
    loading = false
  }
}

@MainActor
final class FolderTreeStore: ObservableObject {
  static let shared = FolderTreeStore()

  @Published private(set) var roots: [TreeNode] = []
  @Published private(set) var visible = false
  @Published private(set) var current = ""
  private var timer: Timer?
  private var revealed = false

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
  }

  private func poll() {
    let nowVisible = mv_chrome_tree_visible()
    if nowVisible != visible { visible = nowVisible }
    guard nowVisible else {
      revealed = false
      return
    }
    var buf = [CChar](repeating: 0, count: 4096)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_current_folder($0.baseAddress, Int32($0.count)) }
    let folder = String(cString: buf)
    if folder != current {
      current = folder
      revealed = false
    }
    if roots.isEmpty {
      Task {
        let list = await Task.detached { readLines { mv_chrome_tree_roots($0, $1) } ?? [] }.value
        if roots.isEmpty { roots = list.map { TreeNode(name: $0.0, path: $0.1) } }
      }
    } else if !revealed && !current.isEmpty {
      revealed = true
      Task { await reveal(current) }
    }
  }

  /// Expands the chain of folders down to `path`, so the open folder is visible.
  private func reveal(_ path: String) async {
    guard let root = roots.filter({ path == $0.path || path.hasPrefix($0.path + "/") })
      .max(by: { $0.path.count < $1.path.count })
    else { return }
    var node = root
    let rest = path.dropFirst(root.path.count).split(separator: "/").map(String.init)
    for component in rest {
      node.isExpanded = true
      await node.loadIfNeeded()
      guard let next = node.children?.first(where: { $0.name == component }) else { return }
      node = next
    }
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
  }
}

struct FolderTreeView: View {
  @ObservedObject private var store = FolderTreeStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: 0) {
      Text("Folders").font(.headline).padding(.horizontal, 14).padding(.top, 12).padding(.bottom, 8)
      Divider()
      ScrollView {
        LazyVStack(alignment: .leading, spacing: 2) {
          ForEach(store.roots) { TreeRow(node: $0, current: store.current, depth: 0) }
        }
        .padding(10)
      }
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(.regularMaterial)
    .overlay(alignment: .trailing) { Divider() }
  }
}
