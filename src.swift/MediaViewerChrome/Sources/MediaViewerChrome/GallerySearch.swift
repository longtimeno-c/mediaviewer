// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The gallery search bar (plan/17 "Gallery search bar", 2026-09-27; plan/16
// `/`): a slim field pinned at the top of the `G` grid.
//
//   Names     always there, no add-on: filters the grid as you type by the
//             names the tiles show (FoldedNames), folder tiles too. A view
//             filter only: the host moves the gallery's keys among what it
//             shows (mv_chrome_set_gallery_filter), the viewer still walks
//             the whole folder.
//   Contents  only with the Local search pack loaded: the pack's own text
//             search in this folder, answered as the gallery's "Search: <text>"
//             list (the search panel's open-as-gallery path). Nothing about
//             searching lives here; the base only sends the text.
//
// The index control at the right end is the pack's (mv_addon2_gallery_accessory),
// embedded like its Settings view; an older pack without it leaves Names only.
// Nothing here blocks the main thread: names are folded off it, once per listing,
// a keystroke runs one memmem pass, and every pack call is [no-block].
import AppKit
import Combine
import MVChromeBridge
import SwiftUI

@MainActor
final class GallerySearchStore: ObservableObject {
  static let shared = GallerySearchStore()

  enum Mode: Hashable { case names, contents }
  enum Contents: Equatable { case idle, searching, opened, nothing, failed, notIndexed }

  /// The field. Kept while the gallery stays on the same folder.
  @Published var text = "" { didSet { if text != oldValue { textChanged() } } }
  /// Remembered for the session; Contents only while the pack offers it.
  @Published private(set) var mode: Mode = .names
  /// What the Names filter shows (indices into FolderStore.names / .folders),
  /// nil when it is off.
  @Published private(set) var items: [Int]?
  @Published private(set) var folders: [Int]?
  @Published private(set) var contents: Contents = .idle
  /// The Local search pack is loaded and vends the bar's pieces.
  @Published private(set) var hasPack = false
  /// Moves when the pack's chrome is re-attached: the accessory is re-embedded.
  @Published private(set) var packGeneration = 0
  /// `/` (gallery_search): the field takes the keyboard, text selected.
  @Published private(set) var focusRequest = 0

  // Computed, not stored: FolderStore's own init reaches listingChanged(_:).
  private var folderStore: FolderStore { FolderStore.shared }
  private var folded: FoldedNames?
  private var foldedFolders: FoldedNames?
  private var foldedGeneration: UInt64 = .max
  private var folding = false
  private var debounce: Task<Void, Never>?
  private var seq: UInt64 = 0
  /// The folder the bar belongs to: a result list has no folder of its own.
  private var barFolder = ""
  /// A list our Contents query opened is on screen.
  private var ownList = false
  /// We are closing that list ourselves (not "Back to folder").
  private var closingOwnList = false
  private var packWatch: AnyCancellable?

  private init() {
    mv_addon2_set_gallery_answer_callback(galleryAnswerTrampoline)
    // LocalSearchStore polls the pack (≤ 2 s) and moves chromeGeneration on
    // every attach or detach.
    packWatch = LocalSearchStore.shared.$chromeGeneration.sink { [weak self] _ in
      DispatchQueue.main.async { MainActor.assumeIsolated { self?.refreshPack() } }
    }
  }

  var active: Bool { !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
  var shownCount: Int { (items?.count ?? 0) + (folders?.count ?? 0) }
  var totalCount: Int { folderStore.names.count + folderStore.folders.count }
  var contentsMode: Bool { mode == .contents && hasPack }

  private func refreshPack() {
    let has = mv_addon2_gallery_accessory("ai") != nil
    packGeneration += 1
    guard has != hasPack else { return }
    hasPack = has
    // Contents without the pack is Names: the same text filters the grid.
    if !has { contents = .idle; ownList = false; applyNamesSoon(now: true) }
  }

  func requestFocus() { focusRequest += 1 }

  func setMode(_ m: Mode) {
    guard m != mode else { return }
    mode = m
    debounce?.cancel()
    if contentsMode {
      pushFilter(nil, nil)
      items = nil
      folders = nil
      runContents()
    } else {
      contents = .idle
      closeOwnList()
      applyNamesSoon(now: true)
    }
  }

  /// The empty Names result's "Search contents instead".
  func searchContentsInstead() { setMode(.contents) }

  private func textChanged() {
    if contentsMode {
      debounce?.cancel()
      if !active {
        contents = .idle
        closeOwnList()
        return
      }
      debounce = Task { [weak self] in
        try? await Task.sleep(nanoseconds: 300_000_000)
        guard !Task.isCancelled else { return }
        self?.runContents()
      }
    } else {
      applyNamesSoon(now: !active)
    }
  }

  // MARK: Names

  /// ~70 ms after the last key (an empty field clears at once).
  private func applyNamesSoon(now: Bool) {
    debounce?.cancel()
    if now {
      applyNames()
      return
    }
    debounce = Task { [weak self] in
      try? await Task.sleep(nanoseconds: 70_000_000)
      guard !Task.isCancelled else { return }
      self?.applyNames()
    }
  }

  private func applyNames() {
    guard !contentsMode, active else {
      if items != nil || folders != nil {
        items = nil
        folders = nil
      }
      pushFilter(nil, nil)
      return
    }
    let generation = folderStore.listingGeneration
    guard let folded, let foldedFolders, foldedGeneration == generation else {
      foldNames(generation)
      return
    }
    let query = FoldedNames.fold(text.trimmingCharacters(in: .whitespacesAndNewlines))
    let names = folded.matching(query)
    let folderHits = foldedFolders.matching(query)
    if names != items { items = names }
    if folderHits != folders { folders = folderHits }
    pushFilter(names, folderHits)
  }

  /// Folding thousands of names is Foundation work (~11 ms for 10,000): off the
  /// main actor, once per listing; the filter itself then runs on it (< 1 ms).
  private func foldNames(_ generation: UInt64) {
    guard !folding else { return }
    folding = true
    let names = folderStore.names
    let folderNames = folderStore.folders.map { folderStore.folderName($0) }
    Task.detached(priority: .userInitiated) {
      let made = FoldedNames(names)
      let madeFolders = FoldedNames(folderNames)
      await MainActor.run {
        self.folding = false
        guard self.folderStore.listingGeneration == generation else {
          // Relisted meanwhile: fold the new names.
          self.applyNames()
          return
        }
        self.folded = made
        self.foldedFolders = madeFolders
        self.foldedGeneration = generation
        self.applyNames()
      }
    }
  }

  private func pushFilter(_ names: [Int]?, _ folderHits: [Int]?) {
    let generation = folderStore.listingGeneration
    guard let names, let folderHits else {
      mv_chrome_set_gallery_filter(generation, false, nil, 0, nil, 0)
      return
    }
    let i32 = names.map { Int32(clamping: $0) }
    let f32 = folderHits.map { Int32(clamping: $0) }
    i32.withUnsafeBufferPointer { ip in
      f32.withUnsafeBufferPointer { fp in
        mv_chrome_set_gallery_filter(generation, true, ip.baseAddress, Int32(ip.count),
                                     fp.baseAddress, Int32(fp.count))
      }
    }
  }

  // MARK: Contents

  private func runContents() {
    debounce?.cancel()
    debounce = nil
    let q = text.trimmingCharacters(in: .whitespacesAndNewlines)
    guard contentsMode, !q.isEmpty, !barFolder.isEmpty else {
      contents = .idle
      return
    }
    seq += 1
    switch mv_addon2_gallery_query(q, barFolder, seq) {
    case 0: contents = .searching
    case 1:
      contents = .notIndexed
      closeOwnList()
    default:
      contents = .failed
      closeOwnList()
    }
  }

  fileprivate func answered(seq answer: UInt64, state: Int32) {
    guard answer == seq, contentsMode else { return }
    switch state {
    case 0:
      contents = .opened
      ownList = true
    case 1:
      contents = .nothing
      // Older results must not stand under the new words.
      closeOwnList()
    default:
      contents = .failed
      closeOwnList()
    }
  }

  /// The not-indexed state's buttons: the pack indexes the folder, then the
  /// same words run again (results grow as the index commits).
  func indexFolder(recursive: Bool) {
    guard mv_addon2_run_command(recursive ? "gallery_index_tree" : "gallery_index_folder") else { return }
    runContents()
  }

  /// Return in the field in Contents mode: no need to wait for the debounce.
  func runNow() {
    if contentsMode, debounce != nil { runContents() }
    if !contentsMode, debounce != nil { applyNames() }
  }

  private func closeOwnList() {
    guard ownList else { return }
    ownList = false
    // Asked of the host, not FolderStore: its poll may not have seen the list yet.
    if mv_chrome_list_open() {
      closingOwnList = true
      folderStore.closeList()
    }
  }

  // MARK: following the viewer

  /// FolderStore relisted (a folder opened, a list opened or closed, a watch
  /// event): changing folder clears the field; "Back to folder" from our
  /// list clears it too; otherwise the filter is re-run on the new names.
  func listingChanged(_ store: FolderStore) {
    let now = Self.currentFolder()
    if !now.isEmpty, now != barFolder {
      let hadFolder = !barFolder.isEmpty
      barFolder = now
      if hadFolder && !text.isEmpty {
        debounce?.cancel()
        ownList = false
        contents = .idle
        text = ""
      }
    }
    if store.listTitle == nil, ownList || closingOwnList {
      let byUser = !closingOwnList
      ownList = false
      closingOwnList = false
      if byUser && contentsMode {
        contents = .idle
        text = ""
      }
    }
    if !contentsMode && active { applyNames() }
  }

  private static func currentFolder() -> String {
    let need = Int(mv_chrome_current_folder(nil, 0))
    guard need > 0 else { return "" }
    var buf = [CChar](repeating: 0, count: need + 1)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_current_folder($0.baseAddress, Int32($0.count)) }
    return String(cString: buf)
  }

  /// Down / Return in Names mode: the first tile shown is selected and the
  /// grid takes the keyboard.
  func leaveField(toFirst: Bool) {
    if toFirst, !contentsMode, let first = items?.first { folderStore.select(first) }
    mv_chrome_gallery_blur()
  }
}

// A non-capturing C function pointer (mv_addon2_set_gallery_answer_callback);
// addons_mac.mm calls it on the main thread, so the isolation is asserted.
private func galleryAnswerTrampoline(_ seq: UInt64, _ state: Int32, _ count: Int32) {
  _ = count
  MainActor.assumeIsolated { GallerySearchStore.shared.answered(seq: seq, state: state) }
}

/// The bar itself: magnifier, field, count, clear, the Names · Contents
/// toggle and the pack's index control. Same surface, hairline and radius as
/// the rest of the gallery chrome.
struct GallerySearchBar: View {
  @ObservedObject private var store = GallerySearchStore.shared
  @FocusState private var focused: Bool

  var body: some View {
    HStack(spacing: 8) {
      Image(systemName: "magnifyingglass")
        .foregroundStyle(MVTheme.body)
      TextField("Search this folder", text: $store.text)
        .textFieldStyle(.plain)
        .font(MVTheme.font(14))
        .foregroundStyle(MVTheme.title)
        .focused($focused)
        .onSubmit {
          store.runNow()
          store.leaveField(toFirst: true)
        }
        .onKeyPress(.downArrow) {
          store.runNow()
          store.leaveField(toFirst: true)
          return .handled
        }
        .onExitCommand {
          // Esc: first clears the text, then gives the keyboard back.
          if store.text.isEmpty { store.leaveField(toFirst: false) } else { store.text = "" }
        }
        .accessibilityLabel("Search this folder")
      if store.contentsMode && store.contents == .searching {
        ProgressView().controlSize(.small)
      }
      if store.active && !store.contentsMode {
        Text("\(store.shownCount) of \(store.totalCount)")
          .font(MVTheme.font(12))
          .foregroundStyle(MVTheme.body)
          .monospacedDigit()
          .fixedSize()
      }
      if !store.text.isEmpty {
        Button {
          store.text = ""
        } label: {
          Image(systemName: "xmark.circle.fill")
        }
        .buttonStyle(.plain)
        .foregroundStyle(MVTheme.body)
        .help("Clear")
        .accessibilityLabel("Clear the search")
      }
      if store.hasPack {
        Picker("Search", selection: Binding(get: { store.mode }, set: { store.setMode($0) })) {
          Text("Names").tag(GallerySearchStore.Mode.names)
          Text("Contents").tag(GallerySearchStore.Mode.contents)
        }
        .pickerStyle(.segmented)
        .labelsHidden()
        .fixedSize()
        .help("Names: match file names. Contents: Local search looks at what is in them.")
        Rectangle().fill(MVTheme.hairline).frame(width: 1, height: 16)
        GalleryAccessoryHost()
          .id(store.packGeneration)
          .fixedSize()
      }
    }
    .padding(.horizontal, 10)
    .frame(height: 30)
    .background(RoundedRectangle(cornerRadius: 6).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(focused ? Color.accentColor : MVTheme.hairline,
                                                             lineWidth: 1))
    .onChange(of: store.focusRequest) { _, _ in
      focused = true
      // The field editor exists once focus lands: select what is there.
      DispatchQueue.main.async {
        NSApp.sendAction(#selector(NSText.selectAll(_:)), to: nil, from: nil)
      }
    }
  }
}

/// What the grid shows instead of tiles while the bar has something to say:
/// no names matched, or a Contents search that did not open a list.
struct GallerySearchNotice: View {
  @ObservedObject private var store = GallerySearchStore.shared

  var body: some View {
    VStack(spacing: 12) {
      Text(message)
        .font(MVTheme.font(14))
        .foregroundStyle(MVTheme.body)
        .multilineTextAlignment(.center)
      if !store.contentsMode && store.hasPack {
        Button("Search contents instead") { store.searchContentsInstead() }
          .buttonStyle(FlatButtonStyle())
      }
      if store.contentsMode && store.contents == .notIndexed {
        HStack(spacing: 8) {
          Button("Index this folder") { store.indexFolder(recursive: false) }
          Button("Index this folder and subfolders") { store.indexFolder(recursive: true) }
        }
        .buttonStyle(FlatButtonStyle())
      }
    }
    .frame(maxWidth: .infinity)
    .padding(.top, 60)
  }

  private var query: String { store.text.trimmingCharacters(in: .whitespacesAndNewlines) }

  private var message: String {
    guard store.contentsMode else { return "No files named “\(query)” in this folder." }
    switch store.contents {
    case .notIndexed: return "This folder is not indexed yet"
    case .failed: return "The search did not finish."
    default: return "Nothing found for “\(query)” in this folder."
    }
  }
}

/// The pack's index control (an NSView the chrome keeps), sized by itself.
private struct GalleryAccessoryHost: NSViewRepresentable {
  func makeNSView(context: Context) -> NSView {
    guard let raw = mv_addon2_gallery_accessory("ai") else { return NSView() }
    let view = Unmanaged<NSView>.fromOpaque(raw).takeUnretainedValue()
    // Rebuilt after the pack re-attaches: move it here.
    view.removeFromSuperview()
    return view
  }

  func updateNSView(_ nsView: NSView, context: Context) {}
}
