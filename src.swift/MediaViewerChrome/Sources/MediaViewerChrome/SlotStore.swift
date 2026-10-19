// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Screens and slots (docs/design/25 §7, §10; PRs 58-59): what MediaViewer's own
// add-ons add to pages the app already has, read from the core's registry
// (src/addon/slots.h through mv_chrome_slots_*). The Windows twin is
// IslandHost.Slots.cs.
//
// Everything here is a table: the contributions are read once when an add-on
// attaches or detaches, the trees and rows when the core says they changed
// (one callback on the main thread, with what changed). Nothing is asked of
// an add-on per frame, per tile or per key: a tile's badges are a dictionary
// lookup, and typing posts a query and returns. With no such add-on installed
// every list is empty and nothing is drawn.
import AppKit
import MVChromeBridge
import SwiftUI

/// One node of a view tree (docs/design/25 §10), parsed from the core's
/// canonical JSON. `props` keeps the rest of the node's keys.
struct ScreenNode: Identifiable {
  let id: String
  let type: String
  let props: [String: Any]
  let children: [ScreenNode]

  init?(_ any: Any?) {
    guard let obj = any as? [String: Any], let id = obj["id"] as? String, let type = obj["type"] as? String
    else { return nil }
    self.id = id
    self.type = type
    var rest = obj
    rest["id"] = nil
    rest["type"] = nil
    let kids = rest.removeValue(forKey: "children") as? [Any] ?? []
    props = rest
    children = kids.compactMap { ScreenNode($0) }
  }

  func string(_ key: String) -> String? { props[key] as? String }
  func double(_ key: String) -> Double? { (props[key] as? NSNumber)?.doubleValue }
  func int(_ key: String) -> Int? { (props[key] as? NSNumber)?.intValue }
  func bool(_ key: String) -> Bool { (props[key] as? NSNumber)?.boolValue ?? false }
  func rows(_ key: String) -> [[String: Any]] { props[key] as? [[String: Any]] ?? [] }
  var label: String { string("label") ?? "" }
  var disabled: Bool { bool("disabled") }
  var hidden: Bool { bool("hidden") }
}

/// Where a screen lives, so its events and surfaces are addressed.
struct ScreenAddress: Hashable {
  let addon: String
  let screen: String
}

struct SlotSection: Identifiable {
  var id: String { "\(addon)/\(section)" }
  let addon: String
  let section: String
  let title: String
  let revision: Int
  let tree: ScreenNode
}

struct SlotScreen: Identifiable {
  var id: String { "\(addon)/\(screen)" }
  let addon: String
  let screen: String
  let title: String
  let place: String  // window | pane | sheet
  let width: CGFloat
  let height: CGFloat
  let revision: Int
  let tree: ScreenNode?
}

struct SlotItem: Identifiable {
  var id: String { "\(addon)/\(item)" }
  let addon: String
  let item: String
  let slot: String    // bar | path | context | status
  let label: String
  let symbol: String
  let target: String  // none | file | files | folder
  var opens = ""      // the screen it opens, if any
}

struct SlotBadge {
  let key: String
  let label: String
  let symbol: String
}

struct SlotSearchRow: Identifiable {
  var id: String { path + "|" + addon + "/" + provider }
  let path: String
  let why: String
  let addon: String
  let provider: String
}

struct SlotSearchState {
  var generation: UInt64 = 0
  var searching = false
  var read = 0
  var total = 0
  var providerNames: [String] = []
  var rows: [SlotSearchRow] = []
}

/// The host's glyph for a contribution's symbol (contrib.h kSymbols).
func slotSymbol(_ name: String) -> String {
  switch name {
  case "map": return "map"
  case "pin": return "mappin"
  case "star": return "star"
  case "tag": return "tag"
  case "search": return "magnifyingglass"
  case "info": return "info.circle"
  case "person": return "person"
  case "camera": return "camera"
  case "folder": return "folder"
  case "sparkle": return "sparkles"
  case "check": return "checkmark"
  case "flag": return "flag"
  case "heart": return "heart"
  case "clock": return "clock"
  case "globe": return "globe"
  case "photo": return "photo"
  case "film": return "film"
  case "bolt": return "bolt"
  case "gear": return "gearshape"
  case "plus": return "plus"
  case "list": return "list.bullet"
  case "grid": return "square.grid.2x2"
  case "share": return "square.and.arrow.up"
  case "warning": return "exclamationmark.triangle"
  case "minus": return "minus"
  default: return "puzzlepiece"
  }
}

@MainActor
final class SlotStore: ObservableObject {
  static let shared = SlotStore()

  @Published private(set) var sections: [SlotSection] = []
  @Published private(set) var screens: [SlotScreen] = []
  @Published private(set) var items: [SlotItem] = []
  /// #410: each add-on's first window screen (Settings' Open, the share icon).
  @Published private(set) var windowScreens: [String: String] = [:]
  @Published private(set) var status: [(id: String, label: String, text: String)] = []
  /// True while an add-on answers Local search (the path bar's button opens it).
  @Published private(set) var localSearch = false
  /// The prefixes file-search providers declare ("place:"), folded as the core folds.
  private(set) var filePrefixes: [String] = []

  /// The query starts with a prefix a file-search provider claims (and no digit
  /// follows it: a date term, as slot_registry::search routes): names are not
  /// matched. Any other "word:" query is an ordinary name query.
  func claimsFilePrefix(_ folded: String) -> Bool {
    for pre in filePrefixes where !pre.isEmpty && folded.hasPrefix(pre) {
      let rest = folded.dropFirst(pre.count).drop { $0 == " " }
      if let c = rest.first, c.isASCII, c.isNumber { continue }
      return true
    }
    return false
  }
  @Published private(set) var fileSearch = SlotSearchState()
  @Published private(set) var localResults = SlotSearchState()
  /// Badges by file name in the folder the gallery shows: a tile's lookup.
  @Published private(set) var badgesByName: [String: [SlotBadge]] = [:]
  /// Section ids that want a location (asked only for a geotagged file).
  private(set) var sectionDefs: [(addon: String, id: String, location: Bool)] = []
  private var addonDirs: [String: String] = [:]
  private var badgeDefs: [String: SlotBadge] = [:]
  private var badgeFolder = ""
  private var infoKey = ""

  private init() {
    mv_chrome_slots_set_callback { bits in
      MainActor.assumeIsolated { SlotStore.shared.changed(bits) }
    }
    // A hop later, not inside init: reading the contributions reaches
    // MetadataStore.shared, whose askSlots asks SlotStore.shared, and
    // re-entering a `static let` initialiser traps (swift_once).
    DispatchQueue.main.async { SlotStore.shared.changed(0x7F) }
  }

  // MARK: reading

  /// nil when the table could not be read (never an empty table: a reader
  /// that took nil for "nothing" would close every screen window).
  private static func read(_ fetch: (UnsafeMutablePointer<CChar>?, Int32) -> Int32) -> Any? {
    var need = Int(fetch(nil, 0))
    // An add-on's worker can grow the table between the size and the copy; the
    // copy then writes nothing and says the new size: ask again with that.
    for _ in 0..<4 {
      guard need > 0 else { return nil }
      var buf = [CChar](repeating: 0, count: need + 1)
      let got = Int(buf.withUnsafeMutableBufferPointer { fetch($0.baseAddress, Int32($0.count)) })
      if got <= need { return try? JSONSerialization.jsonObject(with: Data(String(cString: buf).utf8)) }
      need = got
    }
    return nil
  }

  private func changed(_ bits: UInt32) {
    if bits & 1 != 0 { readContributions() }
    if bits & 2 != 0 { readInfo() }
    if bits & 4 != 0 { readScreens() }
    if bits & 8 != 0 {
      fileSearch = readSearch(1)
      localResults = readSearch(2)
    }
    if bits & 16 != 0 { readBadges() }
    if bits & 32 != 0 { readStatus() }
    if bits & 64 != 0 { readNotices() }
  }

  private func readContributions() {
    guard let doc = Self.read({ mv_chrome_slots_contributions($0, $1) }) as? [String: Any] else { return }
    addonDirs = [:]
    for a in doc["addons"] as? [[String: Any]] ?? [] {
      if let id = a["id"] as? String { addonDirs[id] = a["dir"] as? String ?? "" }
    }
    sectionDefs = (doc["sections"] as? [[String: Any]] ?? []).compactMap {
      guard let a = $0["addon"] as? String, let id = $0["id"] as? String else { return nil }
      return (a, id, ($0["location"] as? Bool) ?? false)
    }
    items = (doc["items"] as? [[String: Any]] ?? []).compactMap {
      guard let a = $0["addon"] as? String, let id = $0["id"] as? String else { return nil }
      return SlotItem(addon: a, item: id, slot: $0["slot"] as? String ?? "bar", label: $0["label"] as? String ?? id,
                      symbol: $0["symbol"] as? String ?? "", target: $0["target"] as? String ?? "none",
                      opens: $0["opens"] as? String ?? "")
    }
    var windows: [String: String] = [:]
    for sc in doc["screens"] as? [[String: Any]] ?? [] {
      guard let a = sc["addon"] as? String, let id = sc["id"] as? String, sc["place"] as? String == "window",
            windows[a] == nil else { continue }
      windows[a] = id
    }
    if windows != windowScreens { windowScreens = windows }
    badgeDefs = [:]
    for b in doc["badges"] as? [[String: Any]] ?? [] {
      guard let key = b["key"] as? String else { continue }
      badgeDefs[key] = SlotBadge(key: key, label: b["label"] as? String ?? "", symbol: b["symbol"] as? String ?? "")
    }
    localSearch = (doc["local_search"] as? Bool) ?? false
    filePrefixes = (doc["providers"] as? [[String: Any]] ?? []).flatMap { p -> [String] in
      (p["files"] as? Bool) == true ? (p["prefixes"] as? [String] ?? []) : []
    }
    SlotTileFlags.shared.set(badges: !badgeDefs.isEmpty, context: items.contains { $0.slot == "context" })
    // A new add-on may want the file on screen and the folder: ask again.
    infoKey = ""
    MetadataStore.shared.slotsChanged()
    if !badgeDefs.isEmpty, !badgeFolder.isEmpty {
      let folder = badgeFolder
      badgeFolder = ""
      requestBadges(folder: folder)
    } else if badgeDefs.isEmpty {
      badgesByName = [:]
    }
  }

  private func readInfo() {
    guard let doc = Self.read({ mv_chrome_slots_info($0, $1) }) as? [String: Any] else { return }
    sections = (doc["sections"] as? [[String: Any]] ?? []).compactMap {
      guard let a = $0["addon"] as? String, let id = $0["id"] as? String, let tree = ScreenNode($0["tree"])
      else { return nil }
      return SlotSection(addon: a, section: id, title: $0["title"] as? String ?? "", revision: $0["revision"] as? Int ?? 0,
                         tree: tree)
    }
  }

  private func readScreens() {
    guard let list = Self.read({ mv_chrome_slots_screens($0, $1) }) as? [[String: Any]] else { return }
    screens = list.compactMap {
      guard let a = $0["addon"] as? String, let id = $0["id"] as? String else { return nil }
      return SlotScreen(addon: a, screen: id, title: $0["title"] as? String ?? id, place: $0["place"] as? String ?? "window",
                        width: CGFloat(($0["width"] as? Int) ?? 0), height: CGFloat(($0["height"] as? Int) ?? 0),
                        revision: $0["revision"] as? Int ?? 0, tree: ScreenNode($0["tree"]))
    }
    SlotWindows.shared.sync(screens)
  }

  private func readSearch(_ scope: UInt32) -> SlotSearchState {
    let doc = Self.read { mv_chrome_slots_search_results(scope, $0, $1) } as? [String: Any] ?? [:]
    var s = SlotSearchState()
    s.generation = (doc["generation"] as? NSNumber)?.uint64Value ?? 0
    s.searching = (doc["searching"] as? Bool) ?? false
    for p in doc["providers"] as? [[String: Any]] ?? [] {
      s.read += p["read"] as? Int ?? 0
      s.total += p["total"] as? Int ?? 0
      if let name = p["name"] as? String { s.providerNames.append(name) }
    }
    s.rows = (doc["rows"] as? [[String: Any]] ?? []).compactMap {
      guard let path = $0["path"] as? String else { return nil }
      return SlotSearchRow(path: path, why: $0["why"] as? String ?? "", addon: $0["addon"] as? String ?? "",
                           provider: $0["provider"] as? String ?? "")
    }
    return s
  }

  private func readBadges() {
    let doc = Self.read { mv_chrome_slots_badges($0, $1) } as? [String: Any] ?? [:]
    let folder = doc["folder"] as? String ?? ""
    var byName: [String: [SlotBadge]] = [:]
    let prefix = folder.hasSuffix("/") ? folder : folder + "/"
    for (path, keys) in doc["tiles"] as? [String: [String]] ?? [:] {
      // Only this folder's own files: a tile is keyed by its name.
      guard path.hasPrefix(prefix) else { continue }
      let name = String(path.dropFirst(prefix.count))
      guard !name.contains("/") else { continue }
      byName[name] = keys.compactMap { badgeDefs[$0] }
    }
    badgesByName = byName
  }

  /// #452: notices and keep-alive (AddonNotices.swift draws them).
  private func readNotices() {
    guard let doc = Self.read({ mv_chrome_slots_notices($0, $1) }) as? [String: Any] else { return }
    AddonNotices.shared.read(doc)
  }

  private func readStatus() {
    let list = Self.read { mv_chrome_slots_status($0, $1) } as? [[String: Any]] ?? []
    status = list.compactMap {
      guard let a = $0["addon"] as? String, let id = $0["id"] as? String, let text = $0["text"] as? String else {
        return nil
      }
      return ("\(a)/\(id)", $0["label"] as? String ?? "", text)
    }
  }

  // MARK: asking

  /// The chrome's tokens as 0xRRGGBBAA, resolved for the current appearance.
  static func theme() -> mv_chrome_slot_theme {
    func rgba(_ c: Color) -> UInt32 {
      var out: UInt32 = 0
      NSApp.effectiveAppearance.performAsCurrentDrawingAppearance {
        let ns = NSColor(c).usingColorSpace(.sRGB) ?? .gray
        let r = UInt32(max(0, min(255, (ns.redComponent * 255).rounded())))
        let g = UInt32(max(0, min(255, (ns.greenComponent * 255).rounded())))
        let b = UInt32(max(0, min(255, (ns.blueComponent * 255).rounded())))
        let a = UInt32(max(0, min(255, (ns.alphaComponent * 255).rounded())))
        out = r << 24 | g << 16 | b << 8 | a
      }
      return out
    }
    let dark = NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
    return mv_chrome_slot_theme(canvas: rgba(MVTheme.canvas), surface: rgba(MVTheme.surface), title: rgba(MVTheme.title),
                                body: rgba(MVTheme.body), disabled: rgba(MVTheme.disabled),
                                hairline: rgba(MVTheme.hairline), accent: rgba(MVTheme.accent), dark: dark ? 1 : 0)
  }

  static var scale: Float { Float(NSApp.mainWindow?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 2) }

  /// The Metadata pane shows `path` (with the pane's own location text). Asks
  /// only when something contributes a section, and once per file.
  func requestInfo(path: String, locationText: String, width: CGFloat) {
    guard !sectionDefs.isEmpty else { return }
    let key = path + "\n" + locationText + "\n" + String(Int(width))
    guard key != infoKey else { return }
    infoKey = key
    var theme = Self.theme()
    _ = path.withCString { p in
      locationText.withCString { l in mv_chrome_slots_request_info(p, l, &theme, Float(width), Self.scale) }
    }
  }

  func clearInfo() {
    guard !infoKey.isEmpty else { return }
    infoKey = ""
    mv_chrome_slots_clear_info()
  }

  @discardableResult
  func search(scope: UInt32, query: String, folder: String) -> UInt64 {
    query.withCString { q in folder.withCString { f in mv_chrome_slots_search(scope, q, f) } }
  }

  /// One request per folder listing: a relist of the same folder (files copied in)
  /// asks again, so the new files get their badges.
  func requestBadges(folder: String) {
    let listing = FolderStore.shared.listingGeneration
    guard !badgeDefs.isEmpty, folder != badgeFolder || listing != badgeListing else { return }
    badgeFolder = folder
    badgeListing = listing
    _ = folder.withCString { mv_chrome_slots_request_badges($0) }
  }
  private var badgeListing: UInt64 = .max

  func badges(forName name: String) -> [SlotBadge] { badgesByName[name] ?? [] }

  func invoke(_ item: SlotItem, paths: [String]) {
    let json = (try? JSONSerialization.data(withJSONObject: paths)).flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
    _ = item.addon.withCString { a in item.item.withCString { i in json.withCString { mv_chrome_slots_invoke(a, i, $0) } } }
    // Choosing it again while its screen is open brings that screen forward
    // (the core keeps the one already open); updates never do.
    if !item.opens.isEmpty { SlotWindows.shared.bringForward(ScreenAddress(addon: item.addon, screen: item.opens)) }
  }

  func send(_ at: ScreenAddress, node: String, kind: UInt32, value: Any?) {
    var text = ""
    if let value {
      if let s = value as? String {
        text = (try? JSONSerialization.data(withJSONObject: [s])).flatMap { String(data: $0, encoding: .utf8) }
          .map { String($0.dropFirst().dropLast()) } ?? ""
      } else if let b = value as? Bool {
        text = b ? "true" : "false"
      } else if let n = value as? Double {
        text = n == n.rounded() && abs(n) < 1e15 ? String(Int64(n)) : String(n)
      } else if let list = value as? [String] {
        text = (try? JSONSerialization.data(withJSONObject: list)).flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
      }
    }
    _ = at.addon.withCString { a in
      at.screen.withCString { s in node.withCString { n in text.withCString { mv_chrome_slots_event(a, s, n, kind, $0) } } }
    }
  }

  /// #410: opens an add-on's window (Settings' Open), or brings it forward.
  func openScreen(addon: String, screen: String, path: String = "") {
    var theme = Self.theme()
    _ = addon.withCString { a in
      screen.withCString { s in path.withCString { p in mv_chrome_slots_open_screen(a, s, p, &theme, 0, 0, Self.scale) } }
    }
    SlotWindows.shared.bringForward(ScreenAddress(addon: addon, screen: screen))
  }

  /// #410: one of the add-on's items, by id, with `paths` (the gallery's share
  /// icon asks Driftwire's "send_files" this way). False when it has no such item.
  @discardableResult
  func invoke(addon: String, item: String, paths: [String]) -> Bool {
    let json = (try? JSONSerialization.data(withJSONObject: paths)).flatMap { String(data: $0, encoding: .utf8) } ?? "[]"
    return addon.withCString { a in item.withCString { i in json.withCString { mv_chrome_slots_invoke(a, i, $0) } } }
  }

  func close(_ at: ScreenAddress) {
    _ = at.addon.withCString { a in at.screen.withCString { mv_chrome_slots_close_screen(a, $0) } }
  }

  /// An image a tree names, inside the add-on's verified folder.
  func imageURL(addon: String, src: String) -> URL? {
    guard let dir = addonDirs[addon], !dir.isEmpty, !src.contains("..") else { return nil }
    return URL(fileURLWithPath: dir).appendingPathComponent(src)
  }

  /// A surface's pixels as an image (premultiplied BGRA), or nil.
  nonisolated static func surfaceImage(_ at: ScreenAddress, surface: String) -> CGImage? {
    surfacePixels(at, surface: surface)?.image
  }

  /// The image and its revision (a new post moves it: a provisional zoom then resets).
  nonisolated static func surfacePixels(_ at: ScreenAddress, surface: String) -> (image: CGImage, revision: UInt64)? {
    var w: UInt32 = 0
    var h: UInt32 = 0
    var rev: UInt64 = 0
    let need = at.addon.withCString { a in
      at.screen.withCString { s in surface.withCString { mv_chrome_slots_surface(a, s, $0, nil, 0, &w, &h, &rev) } }
    }
    guard need > 0, w > 0, h > 0 else { return nil }
    // SwiftUI asks on every body pass (hover, pinch, any publish): a revision
    // already made into an image is drawn as is, never copied again (up to 64 MB).
    let key = "\(at.addon)\u{1}\(at.screen)\u{1}\(surface)"
    surfaceCacheLock.lock()
    let hit = surfaceCache[key]
    surfaceCacheLock.unlock()
    if let hit, hit.revision == rev { return hit }
    var bytes = [UInt8](repeating: 0, count: Int(need))
    let got = bytes.withUnsafeMutableBufferPointer { buf in
      at.addon.withCString { a in
        at.screen.withCString { s in
          surface.withCString { mv_chrome_slots_surface(a, s, $0, buf.baseAddress, need, &w, &h, &rev) }
        }
      }
    }
    guard got == need, let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
    let made = CGImage(width: Int(w), height: Int(h), bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: Int(w) * 4,
                   space: CGColorSpace(name: CGColorSpace.sRGB)!,
                   bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedFirst.rawValue
                     | CGBitmapInfo.byteOrder32Little.rawValue),
                   provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    guard let made else { return nil }
    surfaceCacheLock.lock()
    if surfaceCache.count >= 16 { surfaceCache.removeAll() }  // a few live surfaces at most
    surfaceCache[key] = (made, rev)
    surfaceCacheLock.unlock()
    return (made, rev)
  }
  private nonisolated(unsafe) static var surfaceCache: [String: (image: CGImage, revision: UInt64)] = [:]
  private nonisolated static let surfaceCacheLock = NSLock()

  func items(in slot: String) -> [SlotItem] { items.filter { $0.slot == slot } }
}
