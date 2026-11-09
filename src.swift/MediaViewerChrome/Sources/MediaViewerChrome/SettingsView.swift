// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The Settings screen (docs/design/16 "Settings"), laid out like the Windows one
// (IslandHost.Settings.cs): grouped General preferences and a separate keyboard
// shortcuts tab, with a persistent Done footer. Native owns every value -- this view
// reads the flags and the live command table through the bridge and posts
// changes back, so the keys, `?` and this list cannot drift.
import Combine
import Foundation
import SwiftUI
import MVChromeBridge

struct KeyRow: Identifiable, Equatable {
  let row: Int32
  let name: String
  let keys: String
  var id: Int32 { row }
}

@MainActor
final class SettingsStore: ObservableObject {
  static let shared = SettingsStore()

  static let filmstripFolder: Int32 = 1 << 0
  static let filmstripImage: Int32 = 1 << 1
  static let wrap: Int32 = 1 << 2
  static let stickyZoom: Int32 = 1 << 3
  // Bits 4-6 were the canvas background, removed by issue #408 (settings.h).
  /// Set = folders list the kind (settings.h kSettingShow*). Off by default.
  static let showAudio: Int32 = 1 << 12
  static let showDocuments: Int32 = 1 << 13
  /// #345: set = the AI badge is hidden (settings.h kSettingHideAiBadge), so the
  /// badge is on by default and in every word saved before the bit existed.
  static let hideAiBadge: Int32 = 1 << 14

  @Published private(set) var visible = false
  @Published private(set) var flags: Int32 = 0
  @Published private(set) var rows: [KeyRow] = []
  @Published private(set) var captureRow: Int32 = -1
  /// PR 9: packed sort order (bits 0-2 key, bit 3 descending), as sort_order.h.
  @Published private(set) var sort: Int32 = 0
  /// -1 without an updater (unsigned build), 0 stable, 1 preview.
  @Published private(set) var updateChannel: Int32 = -1
  /// -1 without an updater, 0 off, 1 on (also the app menu's switch).
  @Published private(set) var updateAutoCheck: Int32 = -1

  private var generation: UInt64 = .max

  private init() {
    // Issue #178: polled when the main run loop has run (ChromePulse), not on a timer.
    ChromePulse.shared.add { [weak self] in
      self?.poll()
      return false
    }
  }

  private func poll() {
    let v = mv_chrome_settings_visible()
    if v != visible { visible = v }
    guard v else { return }
    // The menu can change the sort too, and that does not move the keys generation.
    let sortNow = mv_chrome_sort_order()
    if sortNow != sort { sort = sortNow }
    let channelNow = mv_chrome_update_channel()
    if channelNow != updateChannel { updateChannel = channelNow }
    let autoNow = mv_chrome_update_auto_check()
    if autoNow != updateAutoCheck { updateAutoCheck = autoNow }
    let g = mv_chrome_keys_generation()
    if g == generation { return }
    generation = g
    flags = mv_chrome_view_flags()
    captureRow = mv_chrome_key_capture_row()
    rows = Self.readTable()
  }

  /// "Ctrl+Shift+O" -> "⇧⌘O": the table's labels in Mac glyphs. Command and
  /// Control both count as Ctrl in the table, so ⌘ is what the bar shows.
  nonisolated static func macLabel(_ keys: String) -> String {
    keys.replacingOccurrences(of: "Ctrl+", with: "⌘")
      .replacingOccurrences(of: "Alt+", with: "⌥")
      .replacingOccurrences(of: "Shift+", with: "⇧")
  }

  /// The shared table's name of reveal_in_explorer (command_table.cpp); lookups
  /// such as keyLabel keep using it.
  static let revealName = "Show in Explorer"

  /// Issue #412: the words the Mac shows for a table name. ⌘E reveals a file
  /// in Finder, and opens a Photos library item in Photos, selected; the Open
  /// flyout, `?` and Settings say which.
  static func displayName(_ name: String) -> String {
    guard name == revealName else { return name }
    return FolderStore.shared.currentIsPhotosItem ? "Open in Photos" : "Show in Finder"
  }

  /// The shortcut currently bound to the command called `name`, for menu labels.
  /// The bar asks for about 20 labels each time it renders, so the table is
  /// parsed once per keys generation, not once per label (issue #179).
  static func keyLabel(_ name: String) -> String? {
    let g = mv_chrome_keys_generation()
    if g != labelsGeneration {
      var fresh: [String: String] = [:]
      // The first row of a name wins, as first(where:) did.
      for row in readTable() where fresh[row.name] == nil {
        fresh[row.name] = row.keys.isEmpty ? "" : macLabel(row.keys)
      }
      labels = fresh
      labelsGeneration = g
    }
    guard let label = labels[name], !label.isEmpty else { return nil }
    return label
  }
  private static var labels: [String: String] = [:]
  private static var labelsGeneration: UInt64 = .max

  private static func readTable() -> [KeyRow] {
    let need = Int(mv_chrome_command_table(nil, 0))
    guard need > 0 else { return [] }
    var buf = [CChar](repeating: 0, count: need + 1)
    _ = mv_chrome_command_table(&buf, Int32(buf.count))
    let text = String(cString: buf)
    var seen = Set<Int32>()
    var out: [KeyRow] = []
    for line in text.split(separator: "\n") {
      let f = line.split(separator: "\t", omittingEmptySubsequences: false)
      guard f.count >= 6, let row = Int32(f[5]), !seen.contains(row) else { continue }
      seen.insert(row)
      out.append(KeyRow(row: row, name: String(f[2]), keys: String(f[3])))
    }
    return out
  }

  func setFlag(_ bit: Int32, _ on: Bool) {
    mv_chrome_set_view_flags(on ? flags | bit : flags & ~bit)
    generation = .max
    poll()
  }


  var sortKey: Int {
    get { Int(sort & 7) }
    set { mv_chrome_set_sort_order((sort & 8) | Int32(newValue & 7)); sort = mv_chrome_sort_order() }
  }
  var sortDescending: Bool {
    get { sort & 8 != 0 }
    set { mv_chrome_set_sort_order(newValue ? sort | 8 : sort & ~8); sort = mv_chrome_sort_order() }
  }

  func setUpdateChannel(_ channel: Int32) {
    mv_chrome_set_update_channel(channel)
    updateChannel = mv_chrome_update_channel()
  }

  func setUpdateAutoCheck(_ on: Bool) {
    mv_chrome_set_update_auto_check(on)
    updateAutoCheck = mv_chrome_update_auto_check()
  }

  func checkForUpdates() { mv_chrome_check_for_updates() }

  func beginCapture(_ row: Int32) { mv_chrome_key_capture_begin(row); generation = .max; poll() }
  func cancelCapture() { mv_chrome_key_capture_cancel(); generation = .max; poll() }
  func resetKeys() { mv_chrome_keys_reset(); generation = .max; poll() }
  /// A key layout was applied (KeyLayoutRow): re-read the table now.
  func keysChanged() { generation = .max; poll() }
  func close() { mv_chrome_menu(kMenuSettings) }
}

// A separate label column keeps every switch on the same trailing edge,
// including rows whose descriptions wrap at smaller window sizes. One surface:
// the fill marks the row, no hairline around it, words 12 pt from its edge
// (docs/design/20 "Craft rules"; IslandHost.Settings.cs SettingsRow).
private struct SettingsRow<Content: View>: View {
  let title: String
  let detail: String
  @ViewBuilder let content: () -> Content

  var body: some View {
    HStack(spacing: MVTheme.space(24)) {
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        Text(title).font(MVTheme.font()).foregroundStyle(MVTheme.title)
        Text(detail).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      content()
        .labelsHidden()
        .accessibilityLabel(title)
    }
    .padding(MVTheme.space(12))
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(RoundedRectangle(cornerRadius: MVTheme.radius(8)).fill(MVTheme.surface))
    .settingsCardOutline(MVTheme.radius(8))
  }
}

/// Settings' cards are a fill, no hairline (docs/design/20 "Craft rules"); under
/// Increase Contrast the outline comes back, as add-on screen cards do. The
/// Windows twin is CardOutline (IslandHost.Settings.cs), a high-contrast theme's.
struct SettingsCardOutline: ViewModifier {
  let radius: CGFloat
  @Environment(\.colorSchemeContrast) private var contrast

  func body(content: Content) -> some View {
    content.overlay {
      if contrast == .increased {
        RoundedRectangle(cornerRadius: radius).stroke(MVTheme.hairline, lineWidth: 1)
      }
    }
  }
}

extension View {
  func settingsCardOutline(_ radius: CGFloat) -> some View { modifier(SettingsCardOutline(radius: radius)) }
}

/// A group's title on the General page: 16 semibold, 12 pt above (Windows'
/// SettingsSection). Add-ons' heading is one too (AddonListView.swift).
struct SettingsSectionTitle: View {
  let title: String
  var body: some View {
    Text(title).font(MVTheme.font(16)).fontWeight(.semibold)
      .foregroundStyle(MVTheme.title).padding(.top, MVTheme.space(12))
      .accessibilityAddTraits(.isHeader)
  }
}

private struct SettingsToggle: View {
  let title: String
  let detail: String
  let bit: Int32
  /// The bit means "off" (a setting that is on by default).
  var inverted = false
  @ObservedObject var store: SettingsStore
  var body: some View {
    SettingsRow(title: title, detail: detail) {
      Toggle(title, isOn: Binding(
        get: { (store.flags & bit != 0) != inverted },
        set: { store.setFlag(bit, $0 != inverted) }))
        .toggleStyle(.switch)
        .fixedSize()
    }
  }
}

private struct SettingsButton: View {
  let title: String
  let action: () -> Void
  var body: some View {
    Button(title, action: action).buttonStyle(SettingsButtonStyle())
  }
}

// A filled button: the text colour at 0.08 at rest, one step of its own fill
// under the pointer (0.12, eased) and 0.16 pressed, as Windows' SettingsButton.
private struct SettingsButtonStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    SettingsButtonBody(configuration: configuration)
  }
}

private struct SettingsButtonBody: View {
  let configuration: ButtonStyleConfiguration
  @State private var hover = false
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    configuration.label
      .font(MVTheme.font())
      .foregroundStyle(enabled ? MVTheme.title : MVTheme.disabled)
      .padding(.horizontal, MVTheme.space(12)).padding(.vertical, MVTheme.space(7))
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(6))
        .fill(MVTheme.title.opacity(configuration.isPressed ? 0.16 : hover && enabled ? 0.12 : 0.08)))
      .contentShape(Rectangle())
      .onHover { hover = $0 }
      .animation(.easeOut(duration: 0.15), value: hover)
  }
}

// #612 (docs/design/04 "Thumbnail cache upkeep"), Settings -> Storage, the twin
// of IslandHost.Storage.cs: what MediaViewer keeps on disk with sizes, the
// thumbnail cache's limit and Clear. Sizes are measured and Clear runs on a
// background task, never the main thread (rule 1).
struct StorageRow: Identifiable, Equatable {
  let id: String
  let bytes: Int64
}

@MainActor
final class StorageStore: ObservableObject {
  static let shared = StorageStore()

  /// The limit's choices. 0, the default, is none: a cap too small brings back
  /// cold thumbnails, so none smaller than 1 GB is offered.
  static let capChoices: [UInt64] = [0, 1_000_000_000, 2_000_000_000, 5_000_000_000, 10_000_000_000,
                                     20_000_000_000]

  @Published private(set) var rows: [StorageRow] = []
  @Published private(set) var measuring = false
  @Published private(set) var clearing = false
  @Published private(set) var cap: UInt64 = 0

  private init() {}

  func refresh() {
    cap = mv_chrome_thumb_cache_cap()
    guard !measuring else { return }
    measuring = true
    Task.detached(priority: .utility) {
      // One walk when it fits: asking for the size first would walk twice.
      var size = 64 * 1024
      var json = ""
      for _ in 0..<3 {
        var buf = [CChar](repeating: 0, count: size)
        let need = Int(mv_chrome_storage_report(&buf, Int32(buf.count)))
        if need < size { json = String(cString: buf); break }
        size = need + 4096
      }
      let obj = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any] ?? [:]
      let rows = (obj["rows"] as? [[String: Any]] ?? []).compactMap { r -> StorageRow? in
        guard let id = r["id"] as? String else { return nil }
        return StorageRow(id: id, bytes: (r["bytes"] as? NSNumber)?.int64Value ?? 0)
      }
      await MainActor.run {
        self.rows = rows
        self.measuring = false
      }
    }
  }

  func clear() {
    guard !clearing else { return }
    clearing = true
    Task.detached(priority: .userInitiated) {
      _ = mv_chrome_thumb_cache_clear()
      await MainActor.run {
        self.clearing = false
        self.refresh()
      }
    }
  }

  func setCap(_ bytes: UInt64) {
    mv_chrome_set_thumb_cache_cap(bytes)
    cap = mv_chrome_thumb_cache_cap()
  }

  /// What each row is called (io/storage.h has the ids).
  static func words(_ id: String) -> (title: String, detail: String)? {
    switch id {
    case "thumbnails":
      return ("Thumbnails", "Previews of the photos and videos you have browsed. They tidy themselves; Clear removes them all, and they are made again as you browse.")
    case "search_cache":
      return ("File search", "The tags of files in folders you have searched or sorted by date taken.")
    case "clipboard":
      return ("Clipboard copies", "Images copied with ⌘⌥C.")
    case "metadata_snapshots":
      return ("Metadata snapshots", "What a file's details were before you changed them, so you can revert. Never removed automatically.")
    case "crash_reports":
      return ("Crash reports", "Kept on this Mac. Nothing is sent without your answer.")
    case "addon_settings":
      return ("Add-on settings", "What you chose on add-ons' settings pages.")
    default:
      break
    }
    if id.hasPrefix("addon:") {
      let folder = String(id.dropFirst("addon:".count))
      switch folder {
      case "ai": return ("AI search", "The add-on and its index of your photos.")
      case "places": return ("Places", "The add-on and its place database.")
      case "import": return ("Import", "The add-on.")
      case "driftwire": return ("Driftwire", "The add-on.")
      case "enhance": return ("Enhance", "The add-on and the models it downloaded.")
      case "instagram": return ("Instagram Post", "The add-on.")
      default: return (folder, "An add-on and its data.")
      }
    }
    if id.hasPrefix("open_addon:") {
      return (String(id.dropFirst("open_addon:".count)), "An add-on from another maker.")
    }
    return nil
  }

  /// An add-on store folder ("addon:AI Audio"), or nil for the app's own rows.
  static func addonFolder(_ id: String) -> String? {
    id.hasPrefix("addon:") ? String(id.dropFirst("addon:".count)) : nil
  }

  /// The add-on page a store folder belongs to (owner 2026-11-04: each add-on's
  /// storage on its own page; Local search's pieces "AI Audio", "AI Documents",
  /// "AI Faces" with it). On the Mac a folder is the add-on's manifest name.
  static func page(ofFolder folder: String) -> String {
    if folder == "AI" || folder.hasPrefix("AI ") { return AddonNav.localSearchPage }
    switch folder {
    case "Import": return AddonNav.importPage
    case "Places": return "places"
    case "Driftwire": return DriftwireStore.addonID
    case "Enhance": return SlotAddonOffer.enhanceID
    case "Instagram Post": return SlotAddonOffer.instagramID
    default: return folder.lowercased()
    }
  }

  /// What a folder holds, in the add-on page's words.
  static func pieceWords(_ folder: String) -> String {
    switch folder {
    case "AI": return "Photo and video search: the add-on, its models and the index"
    case "AI Audio": return "Sound and speech search models"
    case "AI Documents": return "Document search model"
    case "AI Faces": return "People: the face models"
    case "AI Large": return "The large picture model (Maxxed)"
    case "Enhance": return "The add-on and the models it downloaded"
    case "Places": return "The add-on and its place database"
    default: return "The add-on and its data"
    }
  }

  /// The rows on an add-on's page.
  func rows(forPage page: String) -> [StorageRow] {
    rows.filter { row in
      guard let folder = Self.addonFolder(row.id) else { return false }
      return Self.page(ofFolder: folder) == page
    }
  }

  static func sizeText(_ bytes: Int64) -> String {
    ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
  }
}

private struct StorageSection: View {
  @ObservedObject var storage = StorageStore.shared
  @ObservedObject var settings = SettingsStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      if storage.rows.isEmpty {
        Text(storage.measuring ? "Measuring…" : "Sizes are not available.")
          .font(MVTheme.font()).foregroundStyle(MVTheme.body)
      }
      // Add-ons' storage is on each add-on's page (Settings → Add-ons); here,
      // their total.
      let addonBytes = storage.rows.filter { StorageStore.addonFolder($0.id) != nil }.reduce(Int64(0)) { $0 + $1.bytes }
      ForEach(storage.rows.filter { StorageStore.addonFolder($0.id) == nil }) { row in
        if let w = StorageStore.words(row.id) {
          SettingsRow(title: w.title, detail: w.detail) {
            HStack(spacing: MVTheme.space(12)) {
              Text(StorageStore.sizeText(row.bytes)).font(MVTheme.font().monospacedDigit())
                .foregroundStyle(MVTheme.body)
                .contentTransition(.numericText())
                .animation(.easeOut(duration: 0.15), value: row.bytes)
              if row.id == "thumbnails" {
                SettingsButton(title: storage.clearing ? "Clearing…" : "Clear") { storage.clear() }
                  .disabled(storage.clearing || row.bytes == 0)
                  .accessibilityLabel("Clear thumbnails")
              }
            }
          }
        }
      }
      if addonBytes > 0 {
        SettingsRow(title: "Add-ons", detail: "Each add-on's page (Settings → Add-ons) shows what it keeps.") {
          Text(StorageStore.sizeText(addonBytes)).font(MVTheme.font().monospacedDigit())
            .foregroundStyle(MVTheme.body)
            .contentTransition(.numericText())
            .animation(.easeOut(duration: 0.15), value: addonBytes)
        }
      }
      SettingsRow(title: "Thumbnail cache limit",
                  detail: "The cache already removes thumbnails of files that are gone or changed. A limit also removes the oldest; with a small one, folders open slower.") {
        Picker("Thumbnail cache limit", selection: Binding(get: { storage.cap },
                                                           set: { storage.setCap($0) })) {
          ForEach(StorageStore.capChoices, id: \.self) { choice in
            Text(choice == 0 ? "No limit" : StorageStore.sizeText(Int64(choice))).tag(choice)
          }
        }
        .pickerStyle(.menu).frame(width: 180)
      }
    }
    .onAppear { storage.refresh() }
    // Settings stays built while hidden: each opening measures again.
    .onChange(of: settings.visible) { _, visible in if visible { storage.refresh() } }
  }
}

struct SettingsView: View {
  @ObservedObject private var store = SettingsStore.shared
  @ObservedObject private var theme = ThemeStore.shared
  @ObservedObject private var addons = OpenAddonStore.shared
  @ObservedObject private var photos = PhotosBackupStore.shared
  @ObservedObject private var addonNav = AddonNav.shared  // #510: an add-on's page
  @State private var filter = ""
  @State private var keyboard = false

  private var shown: [KeyRow] {
    let q = filter.trimmingCharacters(in: .whitespaces).lowercased()
    if q.isEmpty { return store.rows }
    return store.rows.filter {
      SettingsStore.displayName($0.name).lowercased().contains(q) || $0.keys.lowercased().contains(q)
        || SettingsStore.macLabel($0.keys).lowercased().contains(q)
    }
  }

  private var hint: String {
    if !keyboard { return "Changes are saved automatically." }
    return store.captureRow >= 0
      ? "Press the new shortcut, or Esc to cancel."
      : "Choose a shortcut to change it. Conflicts swap shortcuts."
  }

  private func section(_ title: String) -> some View { SettingsSectionTitle(title: title) }

  private static let topAnchor = "settings.top"

  private var preferences: some View {
    ScrollViewReader { scroll in
    ScrollView {
      VStack(spacing: 0) {
      Color.clear.frame(height: 0).id(Self.topAnchor)
      Group {
      // #510: an add-on's page in place of the General page (AddonListView.swift).
      if let page = addonNav.page {
        AddonPageView(id: page)
      } else {
      VStack(alignment: .leading, spacing: MVTheme.space(8)) {
        section("Filmstrip")
        SettingsToggle(title: "When opening a folder", detail: "Show thumbnails below the viewer.",
                       bit: SettingsStore.filmstripFolder, store: store)
        SettingsToggle(title: "When opening an image", detail: "Show nearby images from the same folder.",
                       bit: SettingsStore.filmstripImage, store: store)
        section("Browsing")
        SettingsToggle(title: "Wrap at the end", detail: "Continue from the last item to the first.",
                       bit: SettingsStore.wrap, store: store)
        SettingsToggle(title: "Keep pan and zoom", detail: "Keep your view position when moving to the next item.",
                       bit: SettingsStore.stickyZoom, store: store)
        SettingsRow(title: "Sort folder by", detail: "Applies to the gallery and filmstrip.") {
          Picker("Sort folder by", selection: Binding(get: { store.sortKey }, set: { store.sortKey = $0 })) {
            Text("Name").tag(0)
            Text("Date modified").tag(1)
            Text("Size").tag(2)
            Text("Type").tag(3)
            Text("Date taken (EXIF)").tag(4)
          }
          .pickerStyle(.menu).frame(width: 180)
        }
        SettingsRow(title: "Descending order", detail: "Reverse the selected sort order.") {
          Toggle("Descending order", isOn: Binding(get: { store.sortDescending }, set: { store.sortDescending = $0 }))
            .toggleStyle(.switch).fixedSize()
        }
        // #345 (docs/design/06 "The AI badge"): on by default (owner, 2026-10-11).
        section("Viewer")
        SettingsToggle(title: "Show a badge on AI-generated images",
                       detail: "When a file says it was made by an AI tool, mark it in the corner. Files that say nothing get no mark.",
                       bit: SettingsStore.hideAiBadge, inverted: true, store: store)
        section("File types")
        SettingsToggle(title: "Show audio files",
                       detail: "List MP3, M4A and M4P files in folders. A file you open directly still opens.",
                       bit: SettingsStore.showAudio, store: store)
        SettingsToggle(title: "Show documents",
                       detail: "List PDF, Word (DOCX) and PowerPoint (PPTX) files in folders. A file you open directly still opens.",
                       bit: SettingsStore.showDocuments, store: store)
        section("Appearance")
        // docs/design/25: the chrome's colours. Default, then the built-in
        // palettes, then every theme an installed add-on contributes, chosen
        // in a sheet. The photo is never recoloured.
        SettingsRow(title: "Theme",
                    detail: theme.note.isEmpty
                      ? "Colours and a quiet ground for the window, its bars and panes, and the area around your photos and videos. Your photos are never recoloured."
                      : theme.note) {
          // Issue #394: a sheet of cards in each theme's colours (ThemePicker.swift).
          ThemePickerButton()
        }
        .id(ThemeStore.rowAnchor)
        if store.updateChannel >= 0 {
          section("Updates")
          if store.updateAutoCheck >= 0 {
            SettingsRow(title: "Automatic update checks",
                        detail: "Ask GitHub for new versions at launch and every six hours.") {
              Toggle("Check for updates automatically",
                     isOn: Binding(get: { store.updateAutoCheck == 1 }, set: { store.setUpdateAutoCheck($0) }))
                .toggleStyle(.switch).fixedSize()
                .help("Asks GitHub for a newer version at launch and every 6 hours. The request reveals your IP address and app version, nothing about your files.")
            }
          }
          // Windows' row (issue #220): the app menu's "Check for Updates…".
          SettingsRow(title: "Check for updates", detail: "Ask GitHub for a new version now.") {
            Button("Check now") { store.checkForUpdates() }
              .help("Asks GitHub for a newer version now. The request reveals your IP address and app version, nothing about your files.")
          }
          SettingsRow(title: "Update channel",
                      detail: "Preview gets signed test builds before they become stable. Switching back to Stable keeps this version until a newer stable one is released.") {
            Picker("Update channel", selection: Binding(get: { store.updateChannel },
                                                        set: { store.setUpdateChannel($0) })) {
              Text("Stable").tag(Int32(0))
              Text("Preview").tag(Int32(1))
            }
            .pickerStyle(.menu).frame(width: 180)
          }
        }
        // #612: what MediaViewer keeps on disk, the thumbnail cache's limit and Clear.
        section("Storage")
        StorageSection()
        // docs/design/26: the Photos library's backup, once the library was added
        // (Local search -> Add Photos Library); absent otherwise.
        PhotosBackupSection()
          .id(PhotosBackupStore.anchor)
        // Import and (Milestone H, docs/design/17) Local search, as items of one
        // Add-ons section; Local search is absent on an Intel Mac. Its heading
        // is a section's, as every other group's.
        AddonsSection()
      }
      }
      }
      }
      .padding(.horizontal, MVTheme.space(24)).padding(.bottom, MVTheme.space(24))
      .frame(maxWidth: 800)
      .frame(maxWidth: .infinity)
    }
    // docs/design/25: a package handed to the app opens Settings; the question
    // about it is at the foot of this page, so bring it into view.
    .onChange(of: addons.offer) { _, offer in
      guard offer != nil else { return }
      keyboard = false
      addonNav.open(AddonNav.othersPage)  // #510: the question is on From other makers' page
      DispatchQueue.main.async {
        withAnimation(nil) { scroll.scrollTo(OpenAddonSheet.anchor, anchor: .center) }
      }
    }
    // docs/design/25 PR 57: the script rig shows From others (a stopped add-on's line).
    .onChange(of: addons.revealRequests) { _, _ in
      keyboard = false
      addonNav.open(AddonNav.othersPage)
      DispatchQueue.main.async {
        withAnimation(nil) { scroll.scrollTo(OpenAddonStore.sectionAnchor, anchor: .top) }
      }
    }
    // #510: back from an add-on's page, the list where it was (at that row).
    .onChange(of: addonNav.page) { old, now in
      // An add-on's page opens at its top, not where the list was scrolled to.
      // Synchronous, so a jump to an anchor on the page (a package offer) still wins.
      if now != nil {
        withAnimation(nil) { scroll.scrollTo(Self.topAnchor, anchor: .top) }
        return
      }
      guard let old else { return }
      DispatchQueue.main.async {
        withAnimation(nil) { scroll.scrollTo(AddonNav.rowAnchor(old), anchor: .center) }
      }
    }
    // docs/design/26: the welcome card's iCloud Photos row opens Settings here.
    .onChange(of: photos.revealRequests) { _, _ in
      guard photos.takeReveal() else { return }
      keyboard = false
      DispatchQueue.main.async {
        withAnimation(nil) { scroll.scrollTo(PhotosBackupStore.anchor, anchor: .top) }
      }
    }
    .onAppear {
      // #510: the list, or the page something asked for while Settings was closed.
      addonNav.settingsShown()
      if addons.offer != nil {
        addonNav.open(AddonNav.othersPage)
        scroll.scrollTo(OpenAddonSheet.anchor, anchor: .center)
      } else if photos.takeReveal() {
        scroll.scrollTo(PhotosBackupStore.anchor, anchor: .top)
      } else if theme.takeChosenHere() {
        // Choosing a theme rebuilds the chrome; stay at the row that did it.
        scroll.scrollTo(ThemeStore.rowAnchor, anchor: .center)
      }
    }
    }
  }

  private var shortcuts: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(12)) {
      HStack {
        Text("Keyboard shortcuts").font(MVTheme.font(18)).foregroundStyle(MVTheme.title)
        Spacer()
        SettingsButton(title: "Reset to default") { store.resetKeys() }
      }
      KeyLayoutRow()
      TextField("Search commands or keys", text: $filter)
        .textFieldStyle(.roundedBorder)
        .font(MVTheme.font())
        .onExitCommand {
          if store.captureRow >= 0 { store.cancelCapture() }
          else if !filter.isEmpty { filter = "" }
          else { store.close() }
        }
      ScrollView {
        LazyVStack(spacing: 0) {
          ForEach(shown) { row in
            HStack(spacing: MVTheme.space(16)) {
              Text(SettingsStore.displayName(row.name)).font(MVTheme.font()).foregroundStyle(MVTheme.title)
                .frame(maxWidth: .infinity, alignment: .leading)
              Button {
                store.beginCapture(row.row)
              } label: {
                Text(store.captureRow == row.row ? "Press shortcut…"
                     : (row.keys.isEmpty ? "Unbound" : SettingsStore.macLabel(row.keys)))
                  .frame(width: 160)
              }
              .buttonStyle(SettingsButtonStyle())
              .accessibilityLabel("Change shortcut for " + SettingsStore.displayName(row.name))
            }
            .padding(.vertical, MVTheme.space(6))
            Rectangle().fill(MVTheme.hairline.opacity(0.5)).frame(height: 1)
          }
          if shown.isEmpty {
            Text("No matching shortcuts").font(MVTheme.font()).foregroundStyle(MVTheme.body)
              .padding(.top, MVTheme.space(16))
          }
        }
      }
    }
    .padding(MVTheme.space(24))
    .frame(maxWidth: 900)
    .frame(maxWidth: .infinity)
  }

  var body: some View {
    VStack(spacing: 0) {
      HStack(spacing: MVTheme.space(24)) {
        Text("Settings").font(MVTheme.font(24)).foregroundStyle(MVTheme.title)
        Spacer(minLength: 0)
        Picker("Settings category", selection: $keyboard) {
          Text("General").tag(false)
          Text("Keyboard shortcuts").tag(true)
        }
        .pickerStyle(.segmented).labelsHidden().frame(maxWidth: 320)
      }
      .padding(.horizontal, MVTheme.space(24)).padding(.vertical, MVTheme.space(18))
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      Group {
        if keyboard && addons.offer == nil { shortcuts } else { preferences }
      }
      .frame(maxWidth: .infinity, maxHeight: .infinity)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
      HStack(spacing: MVTheme.space(12)) {
        Text(hint).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        Spacer()
        if store.captureRow >= 0 {
          SettingsButton(title: "Cancel change") { store.cancelCapture() }
        }
        SettingsButton(title: "Done") { store.close() }
      }
      .padding(.horizontal, MVTheme.space(24)).padding(.vertical, MVTheme.space(12))
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(ThemeField())
    .onChange(of: keyboard) { _, _ in store.cancelCapture() }
    // #510: each opening shows the list, or the page something asked for.
    .onChange(of: store.visible) { _, visible in if visible { addonNav.settingsShown() } }
  }
}
