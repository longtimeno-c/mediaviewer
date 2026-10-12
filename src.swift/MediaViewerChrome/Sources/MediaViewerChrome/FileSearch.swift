// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// File search (docs/design/16 "File search", 2026-09-28): a find-by-name field over
// the `G` grid, part of the base app. No add-on and no index: it filters the
// folder already listed by the names the tiles show (FoldedNames), folder
// tiles too. The path bar's search icon and ⌘F open it whenever Local search
// is not installed (or failed to load); with Local search they open its panel.
//
// It is the 2026-09-27 gallery search bar's Names mode, shown only while open:
// the field appears above the grid when asked for and goes when Esc empties
// it, the gallery closes, or another folder opens. A view filter only: the
// host moves the gallery's keys among what it shows (mv_chrome_set_gallery_filter),
// the viewer still walks the whole folder. Nothing here blocks the main
// thread: names are folded off it, once per listing, and a keystroke runs one
// memmem pass.
import AppKit
import Combine
import MVChromeBridge
import SwiftUI

@MainActor
final class FileSearchStore: ObservableObject {
  static let shared = FileSearchStore()

  /// The field is on screen above the grid.
  @Published private(set) var isOpen = false
  /// The field's text.
  @Published var text = "" { didSet { if text != oldValue { applyNamesSoon(now: !active) } } }
  /// What the filter shows (indices into FolderStore.names / .folders), nil
  /// when it is off.
  @Published private(set) var items: [Int]?
  @Published private(set) var folders: [Int]?
  /// The field takes the keyboard, text selected.
  @Published private(set) var focusRequest = 0

  // Computed, not stored: FolderStore's own init reaches listingChanged(_:).
  private var folderStore: FolderStore { FolderStore.shared }
  private var folded: FoldedNames?
  private var foldedFolders: FoldedNames?
  private var foldedGeneration: UInt64 = .max
  private var folding = false
  private var debounce: Task<Void, Never>?
  /// The folder the field belongs to: another folder closes it.
  private var searchFolder = ""
  /// docs/design/25 PR 59: what the add-ons' search providers matched (Places:
  /// `portugal`), by the generation of the query they answer. Name hits show
  /// at once; a provider's rows join them when they arrive, and a stale
  /// answer (an older query) is never shown.
  private var nameHits: [Int] = []
  private var providerGeneration: UInt64 = 0
  @Published private(set) var providerNote = ""
  private var slotWatch: AnyCancellable?

  private init() {
    slotWatch = SlotStore.shared.$fileSearch.sink { [weak self] state in
      MainActor.assumeIsolated { self?.providersAnswered(state) }
    }
  }

  var active: Bool { !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }
  var shownCount: Int { (items?.count ?? 0) + (folders?.count ?? 0) }
  var totalCount: Int { folderStore.names.count + folderStore.folders.count }

  /// main_mac.mm has shown the gallery and made its hosting view first
  /// responder: the field appears (or stays) and takes the keyboard.
  func open() {
    if !isOpen {
      isOpen = true
      searchFolder = Self.currentFolder()
      // Fold now, so the first keystroke's pass does not wait for it.
      foldNames(folderStore.listingGeneration)
    }
    focusRequest += 1
  }

  /// Esc on an empty field, the clear button, the gallery closing, another
  /// folder: the field goes and the grid shows the whole folder again.
  func close() {
    guard isOpen else { return }
    debounce?.cancel()
    isOpen = false
    text = ""  // didSet drops the filter
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
    guard isOpen, active else {
      if items != nil || folders != nil {
        items = nil
        folders = nil
      }
      if providerGeneration != 0 {
        // The providers' last answer goes with the text (an empty query asks nobody).
        providerGeneration = 0
        SlotStore.shared.search(scope: 1, query: "", folder: "")
        providerNote = ""
      }
      pushFilter(nil, nil)
      return
    }
    let generation = folderStore.listingGeneration
    guard let folded, let foldedFolders, foldedGeneration == generation else {
      foldNames(generation)
      return
    }
    let typed = text.trimmingCharacters(in: .whitespacesAndNewlines)
    let query = FoldedNames.fold(typed)
    // A prefix a provider declares ("place:") is the provider's alone: no name has
    // it. Unclaimed ("place:" with Places not installed), it is an ordinary name query.
    let prefixed = SlotStore.shared.claimsFilePrefix(query)
    let names = prefixed ? [] : folded.matching(query)
    let folderHits = prefixed ? [] : foldedFolders.matching(query)
    nameHits = names
    // The providers hear the query on their own workers; this returns at once.
    providerGeneration = SlotStore.shared.search(scope: 1, query: typed, folder: searchFolder)
    let merged = merge(names)
    if merged != items { items = merged }
    if folderHits != folders { folders = folderHits }
    pushFilter(merged, folderHits)
  }

  /// Name hits, then the providers' rows for this query in this folder.
  /// `state`: the providers' answer (a @Published sink sees the new value before
  /// the store holds it, so it is passed in).
  private func merge(_ names: [Int], _ given: SlotSearchState? = nil) -> [Int] {
    let state = given ?? SlotStore.shared.fileSearch
    guard state.generation == providerGeneration, !state.rows.isEmpty else { return names }
    var seen = Set(names)
    var out = names
    let prefix = searchFolder.hasSuffix("/") ? searchFolder : searchFolder + "/"
    // Name -> index once per listing, not a scan of every name per row
    // (2,000 rows x 10,000 names on the main actor).
    let generation = folderStore.listingGeneration
    if nameIndexGeneration != generation {
      nameIndex = Dictionary(folderStore.names.enumerated().map { ($1, $0) }, uniquingKeysWith: { first, _ in first })
      nameIndexGeneration = generation
    }
    for row in state.rows where row.path.hasPrefix(prefix) {
      let name = String(row.path.dropFirst(prefix.count))
      if let i = nameIndex[name], seen.insert(i).inserted { out.append(i) }
    }
    return out
  }
  private var nameIndex: [String: Int] = [:]
  private var nameIndexGeneration: UInt64 = .max

  private func providersAnswered(_ state: SlotSearchState) {
    guard isOpen, active, state.generation == providerGeneration else {
      if !providerNote.isEmpty { providerNote = "" }
      return
    }
    if state.searching && state.total > 0 {
      providerNote = "Still reading tags — \(state.read) of \(state.total)"
    } else if let why = state.rows.first?.why, !why.isEmpty {
      providerNote = why
    } else {
      providerNote = ""
    }
    let merged = merge(nameHits, state)
    guard merged != items else { return }
    items = merged
    pushFilter(merged, folders ?? [])
  }

  /// Folding thousands of names is Foundation work (~11 ms for 10,000): off the
  /// main actor, once per listing; the filter itself then runs on it (< 1 ms).
  private func foldNames(_ generation: UInt64) {
    guard !folding, foldedGeneration != generation else { return }
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
          if self.isOpen { self.foldNames(self.folderStore.listingGeneration) }
          return
        }
        self.folded = made
        self.foldedFolders = madeFolders
        self.foldedGeneration = generation
        if self.active { self.applyNames() }
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

  // MARK: following the viewer

  /// FolderStore relisted (a folder opened, a list opened or closed, a watch
  /// event): another folder closes the field; otherwise the filter is re-run
  /// on the new names.
  func listingChanged(_ store: FolderStore) {
    guard isOpen else { return }
    let now = Self.currentFolder()
    if !now.isEmpty, now != searchFolder {
      close()
      return
    }
    if active { applyNames() } else { foldNames(store.listingGeneration) }
  }

  private static func currentFolder() -> String {
    let need = Int(mv_chrome_current_folder(nil, 0))
    guard need > 0 else { return "" }
    var buf = [CChar](repeating: 0, count: need + 1)
    _ = buf.withUnsafeMutableBufferPointer { mv_chrome_current_folder($0.baseAddress, Int32($0.count)) }
    return String(cString: buf)
  }

  /// Down / Return: the first tile shown is selected and the grid takes the
  /// keyboard. The field stays, with its text, until Esc empties it.
  func leaveField(toFirst: Bool) {
    if toFirst, let first = items?.first { folderStore.select(first) }
    mv_chrome_gallery_blur()
  }
}

/// The field: magnifier, text, count and close. Same surface, hairline and
/// radius as the rest of the gallery chrome.
struct FileSearchBar: View {
  @ObservedObject private var store = FileSearchStore.shared
  @FocusState private var focused: Bool

  var body: some View {
    HStack(spacing: MVTheme.space(8)) {
      Image(systemName: "magnifyingglass")
        .foregroundStyle(MVTheme.body)
      TextField("Find files by name in this folder", text: $store.text)
        .textFieldStyle(.plain)
        .font(MVTheme.font(14))
        .foregroundStyle(MVTheme.title)
        .focused($focused)
        .onSubmit { store.leaveField(toFirst: true) }
        .onKeyPress(.downArrow) {
          store.leaveField(toFirst: true)
          return .handled
        }
        .onExitCommand {
          // Esc: first clears the text, then closes the field.
          if store.text.isEmpty {
            store.close()
            store.leaveField(toFirst: false)
          } else {
            store.text = ""
          }
        }
        .accessibilityLabel("Find files by name")
      if store.active && !store.providerNote.isEmpty {
        Text(store.providerNote)
          .font(MVTheme.font(12))
          .foregroundStyle(MVTheme.body)
          .lineLimit(1)
          .accessibilityLabel("Matched by an add-on: \(store.providerNote)")
      }
      if store.active {
        Text("\(store.shownCount) of \(store.totalCount)")
          .font(MVTheme.font(12))
          .foregroundStyle(MVTheme.body)
          .monospacedDigit()
          .fixedSize()
      }
      Button {
        store.close()
        store.leaveField(toFirst: false)
      } label: {
        Image(systemName: "xmark.circle.fill")
      }
      .buttonStyle(.plain)
      .foregroundStyle(MVTheme.body)
      .help("Close file search (Esc)")
      .accessibilityLabel("Close file search")
    }
    .padding(.horizontal, MVTheme.space(10))
    .frame(height: 30)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(6)).fill(MVTheme.surface))
    .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(6)).strokeBorder(focused ? MVTheme.accent : MVTheme.hairline,
                                                             lineWidth: 1))
    .onAppear { takeFocus() }
    .onChange(of: store.focusRequest) { _, _ in takeFocus() }
  }

  private func takeFocus() {
    focused = true
    // The field editor exists once focus lands: select what is there.
    DispatchQueue.main.async {
      NSApp.sendAction(#selector(NSText.selectAll(_:)), to: nil, from: nil)
    }
  }
}

/// In the grid's place when nothing matches.
struct FileSearchNotice: View {
  @ObservedObject private var store = FileSearchStore.shared

  var body: some View {
    Text("No files named “\(store.text.trimmingCharacters(in: .whitespacesAndNewlines))” in this folder.")
      .font(MVTheme.font(14))
      .foregroundStyle(MVTheme.body)
      .multilineTextAlignment(.center)
      .frame(maxWidth: .infinity)
      .padding(.top, MVTheme.space(60))
  }
}
