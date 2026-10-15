// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Settings → Appearance → Theme (issue #394): a sheet of cards, one per theme,
// each drawn in that theme's own seven tokens, in place of a plain menu. The
// Windows twin is IslandHost.ThemePicker.cs.
//
// Default, the built-in palettes, then every theme an installed add-on
// contributes. A built-in's tokens are in the binary. An add-on's are read
// through mv_open_addons_theme on a worker when the sheet opens (it re-verifies
// the add-on), and its card shows a plain hairline until they arrive.
//
// Choosing applies the theme as the menu did. The sheet closes first and the
// theme is applied on dismissal: a theme change rebuilds every chrome root
// (ThemedRoot), which would otherwise tear the sheet down mid-animation.
//
// Keyboard-complete without Full Keyboard Access: arrows move, Return or
// Space choose, Esc closes.
import MVChromeBridge
import SwiftUI

/// One card: what it is called and the palette it is drawn with.
struct ThemeChoice: Identifiable, Equatable {
  let key: String
  let name: String
  let detail: String
  /// nil: the system's own colours (Default), or an add-on's not read yet.
  let tokens: ThemeTokens?
  var id: String { key }
}

/// Add-on themes' tokens, read on a worker for the sheet's swatches. The
/// built-ins and Default never come here.
@MainActor
final class ThemePreviewStore: ObservableObject {
  static let shared = ThemePreviewStore()
  @Published private(set) var tokens: [String: ThemeTokens] = [:]
  private var reading = Set<String>()

  private init() {}

  /// Reads every key not yet read. A theme that no longer verifies stays blank.
  func load(_ keys: [String]) {
    let wanted = keys.filter { tokens[$0] == nil && !reading.contains($0) }
    guard !wanted.isEmpty else { return }
    reading.formUnion(wanted)
    Task.detached(priority: .utility) {
      var read: [String: ThemeTokens] = [:]
      for key in wanted {
        let parts = key.split(separator: "/", maxSplits: 1).map(String.init)
        guard parts.count == 2 else { continue }
        let json = OpenAddonBridge.read { mv_open_addons_theme(parts[0], parts[1], $0, $1) }
        if let t = ThemeTokens(json: json) { read[key] = t }
      }
      let done = read
      await MainActor.run {
        self.reading.subtract(wanted)
        self.tokens.merge(done) { _, new in new }
      }
    }
  }

  /// An add-on was installed, updated or removed: read again next time.
  func forget() { tokens = [:] }
}

enum ThemeChoices {
  static let defaultDetail = "Follows the system"

  @MainActor
  static func all(addons: OpenAddonStore, previews: ThemePreviewStore) -> [ThemeChoice] {
    var list = [ThemeChoice(key: "", name: "Default", detail: defaultDetail, tokens: nil)]
    for item in BuiltinThemes.all {
      list.append(ThemeChoice(key: item.key, name: item.name, detail: detail(item),
                              tokens: ThemeTokens(json: item.json)))
    }
    for addon in addons.installed where addon.state == "ok" {
      for theme in addon.themes {
        let key = ThemeStore.key(addon: addon.addonID, theme: theme.id)
        list.append(ThemeChoice(key: key, name: theme.name, detail: addon.name,
                                tokens: previews.tokens[key]))
      }
    }
    return list
  }

  /// "Dark · Rain": the appearance the palette keeps and its ground.
  static func detail(_ item: BuiltinThemes.Item) -> String {
    let look = item.json.contains("\"dark\":null") ? "Light" : "Dark"
    guard item.effect != "none" else { return look }
    let ground = item.effect.replacingOccurrences(of: "-", with: " ")
    return look + " · " + ground.prefix(1).uppercased() + ground.dropFirst()
  }

  /// What the Theme row's button says for the selection.
  @MainActor
  static func name(_ selection: String, addons: OpenAddonStore) -> String {
    if selection.isEmpty { return "Default" }
    if let item = BuiltinThemes.item(selection: selection) { return item.name }
    if let theme = addons.themes.first(where: { $0.key == selection }) { return theme.name }
    // "Not available" when the add-on is there but refused (the row's line
    // says why), "Not installed" when it is gone.
    let addonID = String(selection.prefix { $0 != "/" })
    return addons.installed.contains { $0.addonID == addonID } ? "Not available" : "Not installed"
  }
}

/// A theme's seven colours, resolved for the appearance on screen. With no
/// tokens: the system's own (Default), never the theme in use.
struct ThemeSwatch {
  let canvas, surface, title, body, disabled, hairline, accent: Color

  init(_ tokens: ThemeTokens?, dark: Bool) {
    guard let t = tokens, let palette = dark ? (t.dark ?? t.light) : (t.light ?? t.dark) else {
      canvas = Color(nsColor: .windowBackgroundColor)
      surface = Color(nsColor: .controlBackgroundColor)
      title = Color(nsColor: .labelColor)
      body = Color(nsColor: .secondaryLabelColor)
      disabled = Color(nsColor: .disabledControlTextColor)
      hairline = Color(nsColor: .separatorColor)
      accent = Color(nsColor: .controlAccentColor)
      return
    }
    func c(_ token: String) -> Color { Color(nsColor: palette.colour(token)) }
    canvas = c("canvas"); surface = c("surface"); title = c("title"); body = c("body")
    disabled = c("disabled"); hairline = c("hairline"); accent = c("accent")
  }

  var strip: [Color] { [canvas, surface, title, body, hairline, accent] }
}

/// A small window in the theme's colours: a bar, a pane with text, an accent.
struct ThemePreview: View {
  let swatch: ThemeSwatch
  /// An add-on theme whose tokens are still being read.
  var pending = false

  var body: some View {
    let r = MVTheme.radius(6)
    ZStack(alignment: .topLeading) {
      swatch.canvas
      if !pending {
        VStack(alignment: .leading, spacing: 6) {
          HStack(spacing: 4) {
            Circle().fill(swatch.accent).frame(width: 6, height: 6)
            Capsule().fill(swatch.body.opacity(0.7)).frame(width: 28, height: 4)
            Spacer(minLength: 0)
            Capsule().fill(swatch.disabled).frame(width: 14, height: 4)
          }
          .padding(.horizontal, 7).frame(height: 14)
          .background(swatch.surface)
          .overlay(alignment: .bottom) { Rectangle().fill(swatch.hairline).frame(height: 1) }
          HStack(alignment: .top, spacing: 6) {
            VStack(alignment: .leading, spacing: 4) {
              Text("Aa").font(.system(size: 15, weight: .semibold)).foregroundStyle(swatch.title)
              Capsule().fill(swatch.body).frame(width: 44, height: 3)
              Capsule().fill(swatch.body.opacity(0.6)).frame(width: 32, height: 3)
            }
            Spacer(minLength: 0)
            RoundedRectangle(cornerRadius: 4).fill(swatch.surface)
              .overlay(RoundedRectangle(cornerRadius: 4).stroke(swatch.hairline, lineWidth: 1))
              .overlay(alignment: .bottomTrailing) {
                Capsule().fill(swatch.accent).frame(width: 18, height: 6).padding(5)
              }
              .frame(width: 46, height: 34)
          }
          .padding(.horizontal, 8)
          Spacer(minLength: 0)
          HStack(spacing: 0) {
            ForEach(Array(swatch.strip.enumerated()), id: \.offset) { _, colour in colour }
          }
          .frame(height: 8)
        }
      }
    }
    .clipShape(RoundedRectangle(cornerRadius: r))
    .overlay(RoundedRectangle(cornerRadius: r).stroke(swatch.hairline, lineWidth: 1))
    .accessibilityHidden(true)
  }
}

struct ThemePickerSheet: View {
  let choices: [ThemeChoice]
  let selection: String
  let note: String
  /// The key chosen, or nil to keep the theme as it is.
  let finish: (String?) -> Void

  @Environment(\.colorScheme) private var scheme
  @State private var cursor = 0
  @FocusState private var focused: Bool
  private static let columns = 4

  private func move(_ by: Int) -> KeyPress.Result {
    let next = cursor + by
    if choices.indices.contains(next) { cursor = next }
    return .handled
  }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(14)) {
      VStack(alignment: .leading, spacing: MVTheme.space(4)) {
        Text("Theme").font(MVTheme.font(20)).foregroundStyle(MVTheme.title)
        Text(note.isEmpty
             ? "Colours and a quiet ground for the window, its bars and panes. Your photos are never recoloured."
             : note)
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
      ScrollViewReader { scroll in
        ScrollView {
          LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: MVTheme.space(12)),
                                   count: Self.columns),
                    spacing: MVTheme.space(12)) {
            ForEach(Array(choices.enumerated()), id: \.element.id) { index, choice in
              card(choice, index: index)
            }
          }
          .padding(MVTheme.space(4))
        }
        .onChange(of: cursor) { _, now in
          withAnimation(nil) { scroll.scrollTo(choices[now].key) }
        }
        .onAppear { scroll.scrollTo(selection, anchor: .center) }
      }
      HStack(spacing: MVTheme.space(12)) {
        Text("Arrows move · Return chooses · Esc closes")
          .font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        Spacer()
        Button("Cancel") { finish(nil) }.keyboardShortcut(.cancelAction)
      }
    }
    .padding(MVTheme.space(20))
    .frame(width: 720, height: 560)
    .background(MVTheme.canvas)
    .focusable()
    .focusEffectDisabled()
    .focused($focused)
    .onKeyPress(.leftArrow) { move(-1) }
    .onKeyPress(.rightArrow) { move(1) }
    .onKeyPress(.upArrow) { move(-Self.columns) }
    .onKeyPress(.downArrow) { move(Self.columns) }
    .onKeyPress(.home) { cursor = 0; return .handled }
    .onKeyPress(.end) { cursor = max(0, choices.count - 1); return .handled }
    .onKeyPress(keys: [.return, .space]) { _ in
      finish(choices.indices.contains(cursor) ? choices[cursor].key : nil)
      return .handled
    }
    .onKeyPress(.escape) { finish(nil); return .handled }
    .onAppear {
      cursor = choices.firstIndex { $0.key == selection } ?? 0
      focused = true
    }
  }

  private func card(_ choice: ThemeChoice, index: Int) -> some View {
    let chosen = choice.key == selection
    let current = index == cursor
    let pending = choice.tokens == nil && !choice.key.isEmpty
    let r = MVTheme.radius(10)
    return Button {
      cursor = index
      finish(choice.key)
    } label: {
      VStack(alignment: .leading, spacing: MVTheme.space(8)) {
        ThemePreview(swatch: ThemeSwatch(choice.tokens, dark: scheme == .dark), pending: pending)
          .frame(height: 92)
        HStack(alignment: .firstTextBaseline, spacing: MVTheme.space(4)) {
          VStack(alignment: .leading, spacing: MVTheme.space(2)) {
            Text(choice.name).font(MVTheme.font(14)).foregroundStyle(MVTheme.title).lineLimit(1)
            Text(choice.detail).font(MVTheme.font(11)).foregroundStyle(MVTheme.body).lineLimit(1)
          }
          Spacer(minLength: 0)
          if chosen {
            Image(systemName: "checkmark.circle.fill").foregroundStyle(MVTheme.accent)
          }
        }
      }
      .padding(MVTheme.space(8))
      .background(RoundedRectangle(cornerRadius: r).fill(MVTheme.surface))
      .overlay(RoundedRectangle(cornerRadius: r)
        .stroke(chosen ? MVTheme.accent : current ? MVTheme.body : MVTheme.hairline,
                lineWidth: chosen || current ? 2 : 1))
      .overlay {
        if current && chosen {
          // Both: an outer ring for the keyboard, the accent for the choice.
          RoundedRectangle(cornerRadius: r + 3).stroke(MVTheme.body, lineWidth: 1).padding(-3)
        }
      }
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .focusable(false)
    .id(choice.key)
    .accessibilityLabel(choice.name + ", " + choice.detail)
    .accessibilityAddTraits(chosen ? .isSelected : [])
  }
}

/// The Theme row's control: the chosen theme's colours and name; opens the sheet.
struct ThemePickerButton: View {
  @ObservedObject private var theme = ThemeStore.shared
  @ObservedObject private var addons = OpenAddonStore.shared
  @ObservedObject private var previews = ThemePreviewStore.shared
  @Environment(\.colorScheme) private var scheme
  @State private var open = false
  @State private var picked: String?

  private var choices: [ThemeChoice] { ThemeChoices.all(addons: addons, previews: previews) }

  var body: some View {
    let current = choices.first { $0.key == theme.selection }
    Button {
      previews.load(addons.themes.map(\.key))
      open = true
    } label: {
      HStack(spacing: MVTheme.space(8)) {
        HStack(spacing: 0) {
          ForEach(Array(ThemeSwatch(current?.tokens, dark: scheme == .dark).strip.enumerated()),
                  id: \.offset) { _, colour in colour.frame(width: 6) }
        }
        .frame(height: 16)
        .clipShape(RoundedRectangle(cornerRadius: 3))
        .overlay(RoundedRectangle(cornerRadius: 3).stroke(MVTheme.hairline, lineWidth: 1))
        Text(ThemeChoices.name(theme.selection, addons: addons)).lineLimit(1)
        Spacer(minLength: 0)
        Image(systemName: "chevron.right").font(.system(size: 10, weight: .semibold))
          .foregroundStyle(MVTheme.body)
      }
      .font(MVTheme.font())
      .foregroundStyle(MVTheme.title)
      .padding(.horizontal, MVTheme.space(10)).padding(.vertical, MVTheme.space(6))
      .frame(width: 180)
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(6)).fill(Color.primary.opacity(0.08)))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .help("Choose a theme")
    .accessibilityValue(ThemeChoices.name(theme.selection, addons: addons))
    .sheet(isPresented: $open, onDismiss: {
      // After the sheet is gone: the theme rebuilds the chrome (ThemedRoot).
      if let key = picked { theme.choose(key) }
      picked = nil
    }) {
      ThemePickerSheet(choices: choices, selection: theme.selection, note: theme.note) { key in
        picked = key
        open = false
      }
    }
  }
}
