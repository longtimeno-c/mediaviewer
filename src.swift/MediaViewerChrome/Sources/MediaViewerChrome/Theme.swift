// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The chrome's colours and face, and the theme an open add-on may put in
// their place (docs/design/25 "Themes"; the Windows twin is IslandHost.Theme.cs).
//
// With no theme the tokens are AppKit's semantic colours, which follow light,
// dark and increased contrast by themselves. A theme is seven colours per
// palette: five built into the chrome (BuiltinThemes.swift), or one from an
// add-on's file, read and checked by the core (contrast included) and handed
// here as JSON. This file never reads an add-on.
//
// Start-up never waits for an add-on ("the view comes first"): the tokens of
// the theme in use are kept in the defaults and painted at once. The add-on
// is verified on a worker afterwards, and if it is gone or no longer
// verifies the chrome returns to the default and Settings says so.
//
// A theme colours the chrome only. Photo and video pixels, the histogram's
// channels and the marks' colours are not tokens (rule 2).
import AppKit
import CoreText
import SwiftUI
import MVChromeBridge

/// The chrome's tokens. Read in view bodies, on the main actor only.
enum MVTheme {
  static var canvas: Color { themed?.canvas ?? Color(nsColor: .windowBackgroundColor) }
  static var title: Color { themed?.title ?? Color(nsColor: .labelColor) }
  static var body: Color { themed?.body ?? Color(nsColor: .secondaryLabelColor) }
  static var hairline: Color { themed?.hairline ?? Color(nsColor: .separatorColor) }
  static var surface: Color { themed?.surface ?? Color(nsColor: .controlBackgroundColor) }
  static var disabled: Color { themed?.disabled ?? Color(nsColor: .disabledControlTextColor) }
  /// Selection, progress, the focused control. The system's accent by default.
  static var accent: Color { themed?.accent ?? Color.accentColor }

  /// Built once when a theme is applied, never per view body.
  struct Colours {
    let canvas, surface, title, body, disabled, hairline, accent: Color
  }
  fileprivate(set) static var themed: Colours?
  /// A family installed on this Mac that the theme asked for, or nil.
  fileprivate(set) static var family: String?

  private static let registered: Bool = {
    // CMake copies the face beside the executable (cmake/darwin.cmake).
    let dir = Bundle.main.executableURL?.deletingLastPathComponent()
    guard let url = dir?.appendingPathComponent("CozetteVector.ttf") else { return false }
    return CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
  }()

  static func font(_ size: CGFloat = 16) -> Font {
    _ = registered
    return .custom(family ?? "CozetteVector", size: size * typeScale)
  }

  // Theme shape (theme schema 2, docs/design/25 "Theme, shape"): scales on the
  // chrome's own literals, which are the defaults (1). Set once when a theme
  // is painted; read in view bodies like the colours.
  fileprivate(set) static var radiusScale: CGFloat = 1
  fileprivate(set) static var density: CGFloat = 1
  fileprivate(set) static var typeScale: CGFloat = 1
  /// A corner radius the chrome would draw as `r`.
  static func radius(_ r: CGFloat) -> CGFloat { r * radiusScale }
  /// Padding or spacing the chrome would use as `p`.
  static func space(_ p: CGFloat) -> CGFloat { (p * density).rounded() }
}

/// The host's tokens for MediaViewer's own add-on windows (Import, Duplicates,
/// Local search; issue #204), which are separate bundles and cannot see
/// MVTheme. They look this class up by name (`NSClassFromString`) and call
/// `tokens`; an older app has no such class and they keep the system's
/// colours. `MVChromeThemeDidChange` is posted after every repaint.
@objc(MVChromeTheme)
public final class MVChromeTheme: NSObject {
  /// "canvas", "surface", "title", "body", "disabled", "hairline", "accent":
  /// NSColor (dynamic: they follow light / dark); "font": the family name;
  /// "radius", "density", "typeScale": NSNumber scales (1 = the literals).
  @MainActor @objc public static func tokens() -> [String: Any] { published }
  @MainActor fileprivate static var published: [String: Any] = defaults()
  static let didChange = Notification.Name("MVChromeThemeDidChange")

  @MainActor fileprivate static func defaults() -> [String: Any] {
    [
      "canvas": NSColor.windowBackgroundColor, "surface": NSColor.controlBackgroundColor,
      "title": NSColor.labelColor, "body": NSColor.secondaryLabelColor,
      "disabled": NSColor.disabledControlTextColor, "hairline": NSColor.separatorColor,
      "accent": NSColor.controlAccentColor, "font": "CozetteVector",
      "radius": 1.0, "density": 1.0, "typeScale": 1.0,
    ]
  }
}

/// One palette of a theme: 0xRRGGBBAA per token.
struct ThemePalette: Equatable {
  static let tokens = ["canvas", "surface", "title", "body", "disabled", "hairline", "accent"]
  var rgba: [String: UInt32] = [:]

  init?(_ json: Any?) {
    guard let obj = json as? [String: Any] else { return nil }
    for token in Self.tokens {
      guard let text = obj[token] as? String, text.count == 9, text.hasPrefix("#"),
            let value = UInt32(text.dropFirst(), radix: 16)
      else { return nil }
      rgba[token] = value
    }
  }

  func colour(_ token: String) -> NSColor {
    let v = rgba[token] ?? 0
    return NSColor(srgbRed: CGFloat((v >> 24) & 0xFF) / 255, green: CGFloat((v >> 16) & 0xFF) / 255,
                   blue: CGFloat((v >> 8) & 0xFF) / 255, alpha: CGFloat(v & 0xFF) / 255)
  }
  /// The tokens the native empty window draws with (issue #408), 0xRRGGBBAA.
  var home: mv_home_palette {
    mv_home_palette(canvas: rgba["canvas"] ?? 0, surface: rgba["surface"] ?? 0,
                    title: rgba["title"] ?? 0, body: rgba["body"] ?? 0,
                    hairline: rgba["hairline"] ?? 0, accent: rgba["accent"] ?? 0)
  }
}

/// A theme as the core hands it over (src/addon/theme.h theme_to_json).
struct ThemeTokens: Equatable {
  let addon: String
  let id: String
  let name: String
  let font: String
  let dark: ThemePalette?
  let light: ThemePalette?
  /// Theme shape (schema 2); 1 each for a schema 1 theme or an older cache.
  let radius: Double
  let density: Double
  let typeScale: Double

  init?(json: String) {
    guard let obj = (try? JSONSerialization.jsonObject(with: Data(json.utf8))) as? [String: Any],
          let addon = obj["addon"] as? String, let id = obj["id"] as? String,
          let theme = obj["theme"] as? [String: Any]
    else { return nil }
    self.addon = addon
    self.id = id
    name = obj["name"] as? String ?? id
    font = theme["font"] as? String ?? ""
    dark = ThemePalette(theme["dark"])
    light = ThemePalette(theme["light"])
    if dark == nil && light == nil { return nil }
    // The core bounds these; clamp again so a hand-edited cache cannot do worse.
    let shape = theme["shape"] as? [String: Any] ?? [:]
    func scale(_ key: String, _ lo: Double, _ hi: Double) -> Double {
      let v = (shape[key] as? NSNumber)?.doubleValue ?? 1
      return v.isFinite ? Swift.min(hi, Swift.max(lo, v)) : 1
    }
    radius = scale("radius", 0, 2)
    density = scale("density", 0.75, 1.25)
    typeScale = scale("type_scale", 0.85, 1.25)
  }
}

@MainActor
final class ThemeStore: ObservableObject {
  static let shared = ThemeStore()

  /// Bumped whenever the tokens change: every chrome root rebuilds on it.
  @Published private(set) var generation = 0
  /// "addon/theme" of the theme chosen, "" for the default.
  @Published private(set) var selection = ""
  /// Why the chosen theme is not showing, for Settings; "" when it is.
  @Published private(set) var note = ""

  private var tokens: ThemeTokens?
  /// What was last painted (an inner nil is the system's colours); nil before
  /// the first paint. A repaint that would show the same returns early, so
  /// Reduce Motion or Reduce Transparency never rebuilds the chrome and
  /// drops what is typed in it.
  private var painted: ThemeTokens??
  private static let keySelection = "mv.theme.selection"
  private static let keyTokens = "mv.theme.tokens"
  // The verify rig (MV_ADDON_SELFTEST, main_mac.mm) keeps its choice in
  // memory: a test never changes the theme the person has chosen, and leaves
  // nothing in their preferences.
  private var volatile: [String: String]? =
    ProcessInfo.processInfo.environment["MV_ADDON_SELFTEST"] == nil ? nil : [:]

  private func stored(_ key: String) -> String? {
    volatile != nil ? volatile?[key] : UserDefaults.standard.string(forKey: key)
  }
  private func store(_ value: String?, _ key: String) {
    if volatile != nil {
      volatile?[key] = value
    } else if let value {
      UserDefaults.standard.set(value, forKey: key)
    } else {
      UserDefaults.standard.removeObject(forKey: key)
    }
  }

  private init() {
    selection = stored(Self.keySelection) ?? ""
    if let builtin = BuiltinThemes.item(selection: selection) {
      // The palette is in the binary. The defaults hold the choice and the
      // resolved tokens, which is what the next launch paints from.
      let json = builtin.json
      tokens = ThemeTokens(json: json)
      if stored(Self.keyTokens) != json { store(json, Self.keyTokens) }
    } else if !selection.isEmpty, let cached = stored(Self.keyTokens) {
      tokens = ThemeTokens(json: cached)
    }
    paint()
    // The system's contrast setting outranks a theme, and may change while
    // the app runs.
    NSWorkspace.shared.notificationCenter.addObserver(
      forName: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil, queue: .main
    ) { [weak self] _ in
      MainActor.assumeIsolated { self?.paint() }
    }
    if !selection.isEmpty, BuiltinThemes.item(selection: selection) == nil { verify() }
  }

  static func key(addon: String, theme: String) -> String { addon + "/" + theme }

  /// Settings' Theme row, and whether the choice just made there is what
  /// rebuilt the page (so Settings can stay at that row).
  static let rowAnchor = "theme-row"
  private var chosenHere = false
  func takeChosenHere() -> Bool {
    defer { chosenHere = false }
    return chosenHere
  }

  /// Settings' picker. "" returns to the default.
  func choose(_ key: String) {
    guard key != selection else { return }
    chosenHere = true
    selection = key
    note = ""
    store(key, Self.keySelection)
    if key.isEmpty {
      tokens = nil
      store(nil, Self.keyTokens)
      paint()
    } else if let builtin = BuiltinThemes.item(selection: key) {
      apply(builtin)
    } else {
      verify()
    }
  }

  /// A built-in palette, from the binary. No add-on read.
  private func apply(_ item: BuiltinThemes.Item) {
    let json = item.json
    guard let fresh = ThemeTokens(json: json) else { return }
    note = ""
    if stored(Self.keyTokens) != json { store(json, Self.keyTokens) }
    if fresh != tokens {
      tokens = fresh
      paint()
    }
  }

  /// An add-on was installed, updated or removed: what is painted may be stale.
  func addonsChanged() {
    ThemePreviewStore.shared.forget()
    if !selection.isEmpty { verify() }
  }

  /// Reads the theme from its add-on, which re-verifies the add-on: a worker.
  /// A built-in is already in the binary, so it never takes that path.
  private func verify() {
    if let builtin = BuiltinThemes.item(selection: selection) {
      apply(builtin)
      return
    }
    let chosen = selection
    let parts = chosen.split(separator: "/", maxSplits: 1).map(String.init)
    guard parts.count == 2 else { return }
    Task.detached(priority: .utility) {
      let json = OpenAddonBridge.read { mv_open_addons_theme(parts[0], parts[1], $0, $1) }
      await MainActor.run {
        guard self.selection == chosen else { return }  // chosen again meanwhile
        if let fresh = ThemeTokens(json: json) {
          self.note = ""
          self.store(json, Self.keyTokens)
          if fresh != self.tokens {
            self.tokens = fresh
            self.paint()
          }
        } else {
          // Gone, changed on disk, or refused: the default, and say so. The
          // choice is kept, so installing the add-on again brings it back.
          self.note = "The chosen theme's add-on is missing or did not pass verification, so the default is showing."
          self.store(nil, Self.keyTokens)
          if self.tokens != nil {
            self.tokens = nil
            self.paint()
          }
        }
      }
    }
  }

  private func paint() {
    let contrast = NSWorkspace.shared.accessibilityDisplayShouldIncreaseContrast
    let shown = contrast ? nil : tokens
    if let painted, painted == shown {
      chosenHere = false  // nothing rebuilds, so Settings has no row to return to
      return
    }
    painted = .some(shown)
    guard let t = shown else {
      MVTheme.themed = nil
      MVTheme.family = nil
      MVTheme.radiusScale = 1
      MVTheme.density = 1
      MVTheme.typeScale = 1
      NSApp?.appearance = nil
      mv_chrome_set_home_theme(false, nil, nil, nil, 0, 1, 1, 1)
      MVChromeTheme.published = MVChromeTheme.defaults()
      generation += 1
      NotificationCenter.default.post(name: MVChromeTheme.didChange, object: nil)
      return
    }
    // One palette: the chrome keeps that appearance, so the system's own
    // controls match the theme. Two: it follows the system.
    if t.dark != nil && t.light != nil {
      NSApp?.appearance = nil
    } else {
      NSApp?.appearance = NSAppearance(named: t.dark != nil ? .darkAqua : .aqua)
    }
    func dynamicNS(_ token: String) -> NSColor {
      let dark = (t.dark ?? t.light)!.colour(token)
      let light = (t.light ?? t.dark)!.colour(token)
      return NSColor(name: nil) { appearance in
        appearance.bestMatch(from: [.aqua, .darkAqua]) == .darkAqua ? dark : light
      }
    }
    func dynamic(_ token: String) -> Color { Color(nsColor: dynamicNS(token)) }
    MVTheme.themed = MVTheme.Colours(
      canvas: dynamic("canvas"), surface: dynamic("surface"), title: dynamic("title"),
      body: dynamic("body"), disabled: dynamic("disabled"), hairline: dynamic("hairline"),
      accent: dynamic("accent"))
    MVTheme.family = t.font.isEmpty || NSFontManager.shared.availableMembers(ofFontFamily: t.font) == nil
      ? nil : t.font
    MVTheme.radiusScale = CGFloat(t.radius)
    MVTheme.density = CGFloat(t.density)
    MVTheme.typeScale = CGFloat(t.typeScale)
    // Issue #408: the native empty window and the canvas clear take the same
    // tokens and the built-in's ground; an add-on theme has no ground.
    var darkHome = t.dark?.home ?? mv_home_palette()
    var lightHome = t.light?.home ?? mv_home_palette()
    let ground = BuiltinThemes.item(selection: selection)
    withUnsafePointer(to: &darkHome) { d in
      withUnsafePointer(to: &lightHome) { l in
        mv_chrome_set_home_theme(true, t.dark != nil ? d : nil, t.light != nil ? l : nil,
                                 ground?.effect ?? "none", ground?.effectRGB ?? 0,
                                 Float(ground?.intensity ?? 1), Float(ground?.speed ?? 1),
                                 Float(ground?.size ?? 1))
      }
    }
    var published: [String: Any] = ["font": MVTheme.family ?? "CozetteVector",
                                    "radius": t.radius, "density": t.density, "typeScale": t.typeScale]
    for token in ThemePalette.tokens { published[token] = dynamicNS(token) }
    MVChromeTheme.published = published
    generation += 1
    NotificationCenter.default.post(name: MVChromeTheme.didChange, object: nil)
  }
}

/// Every chrome root (ChromeHost.host): rebuilt when the theme changes, which
/// is rare, so no view has to watch the store. The tint reaches the system's
/// own controls.
struct ThemedRoot<Content: View>: View {
  @ObservedObject private var theme = ThemeStore.shared
  let content: Content
  var body: some View {
    content
      .tint(MVTheme.accent)
      .id(theme.generation)
  }
}

/// String-returning bridge calls that must run ONCE (an install, a hash of a
/// package): a buffer first, a second call only if the answer did not fit and
/// the call is a read.
enum OpenAddonBridge {
  static func read(capacity: Int = 1 << 16, retry: Bool = true,
                   _ call: (UnsafeMutablePointer<CChar>?, Int32) -> Int32) -> String {
    var buf = [CChar](repeating: 0, count: capacity)
    let need = Int(buf.withUnsafeMutableBufferPointer { call($0.baseAddress, Int32($0.count)) })
    if need < capacity { return String(cString: buf) }
    guard retry else { return "" }
    buf = [CChar](repeating: 0, count: need + 1)
    _ = buf.withUnsafeMutableBufferPointer { call($0.baseAddress, Int32($0.count)) }
    return String(cString: buf)
  }
}
