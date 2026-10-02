// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// The in-window command bar. Styled to match the Windows island bar
// (IslandHost.cs): the canvas colour, CozetteVector 16 pt, flat text buttons
// with a faint hover wash, system-themed flyouts with a hairline border, and a hairline
// under the bar. The commands are the same ones the system menu bar runs
// (mv_chrome_menu tags mirror MvMenuCmd in main_mac.mm).
import AppKit
import SwiftUI
import MVChromeBridge

// MVTheme, the chrome's colours and face, is in Theme.swift (plan/23).

/// The chrome's one button look: CozetteVector text, no border, a faint wash
/// on hover. `selected` keeps the pressed wash (a chosen tab or preset);
/// `compact` is the panes' tighter padding. Shared by the command bar and
/// the Edit workspace (PR 29) so every chrome button matches.
struct FlatButtonStyle: ButtonStyle {
  var selected = false
  var compact = false

  func makeBody(configuration: Configuration) -> some View {
    FlatButtonBody(configuration: configuration, selected: selected, compact: compact)
  }
  private struct FlatButtonBody: View {
    let configuration: Configuration
    let selected: Bool
    let compact: Bool
    @State private var hover = false
    @Environment(\.isEnabled) private var enabled
    var body: some View {
      configuration.label
        .font(MVTheme.font(compact ? 14 : 16))
        .foregroundStyle(enabled ? MVTheme.title : MVTheme.disabled)
        .padding(.horizontal, compact ? 8 : 14).padding(.vertical, compact ? 5 : 7)
        .background(
          RoundedRectangle(cornerRadius: 4).fill(Color.primary.opacity(
            configuration.isPressed || selected ? 0.11 : (hover && enabled ? 0.06 : 0))))
        .contentShape(Rectangle())
        .onHover { hover = $0 }
    }
  }
}

private struct FlyoutItem: View {
  let title: String
  var shortcut: String? = nil
  var enabled = true
  let action: () -> Void
  var body: some View {
    Button(action: action) {
      HStack(spacing: 24) {
        Text(title)
        Spacer(minLength: 0)
        if let shortcut { Text(shortcut).foregroundStyle(MVTheme.body) }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
    }
    .buttonStyle(FlatButtonStyle())
    .disabled(!enabled)
  }
}

private struct FlyoutRule: View {
  var body: some View {
    Rectangle().fill(MVTheme.hairline).frame(height: 1).padding(.vertical, 4)
  }
}

/// "Version 0.1.2", from CMake project(VERSION). "Version unknown" if the host
/// was built without one — the same wording as the Windows About flyout.
private func aboutVersionLine() -> String {
  var buf = [CChar](repeating: 0, count: 64)
  guard mv_chrome_app_version(&buf, Int32(buf.count)) else { return "Version unknown" }
  return "Version \(String(cString: buf))"
}

/// Opens a file bundled in Contents/Resources (macpack.py puts LICENSE.txt,
/// NOTICE.txt and THIRD-PARTY.md there); if it is not there — a bare `swift
/// run` — the same file on GitHub. Part of the About flyout, which is the
/// GPLv3 "Appropriate Legal Notices" (plan/11).
private func openLegalFile(_ name: String, github: String) {
  if let url = Bundle.main.resourceURL?.appendingPathComponent(name),
     FileManager.default.fileExists(atPath: url.path) {
    NSWorkspace.shared.open(url)
  } else if let url = URL(string: "https://github.com/longtimeno-c/mediaviewer/blob/main/" + github) {
    NSWorkspace.shared.open(url)
  }
}

/// A bar button that opens a system-themed flyout beneath it.
private struct BarFlyout<Content: View>: View {
  let title: String
  @ViewBuilder var content: (_ close: @escaping () -> Void) -> Content
  @State private var shown = false
  var body: some View {
    Button(title) { shown.toggle() }
      .buttonStyle(FlatButtonStyle())
      .popover(isPresented: $shown, arrowEdge: .bottom) {
        VStack(alignment: .leading, spacing: 0) { content { shown = false } }
          .padding(4)
          .frame(minWidth: 240)
          .background(MVTheme.canvas)
      }
  }
}

public struct CommandBarView: View {
  @ObservedObject private var store = FolderStore.shared
  @ObservedObject private var notice = NoticeStore.shared
  @ObservedObject private var edit = EditStore.shared

  public init() {}

  private func key(_ name: String) -> String? { SettingsStore.keyLabel(name) }

  public var body: some View {
    let hasFolder = store.itemCount > 0
    let current = (store.currentIndex >= 0 && store.currentIndex < store.names.count)
      ? store.names[store.currentIndex] : nil
    // No .keyboardShortcut here: the router (main_mac.mm) owns the keys; the
    // shortcut column is only a label, read from the live table so a remap shows.
    VStack(spacing: 0) {
      HStack(spacing: 0) {
        // Same order and labels as the Windows bar: Open, View, Edit image /
        // Edit video (PR 29), Settings, About; the folder trail, then `?`, at
        // the far right.
        HStack(spacing: 0) {
          BarFlyout(title: "Open") { close in
            FlyoutItem(title: "Media…", shortcut: key("Open media…")) { close(); mv_chrome_menu(1) }
            FlyoutItem(title: "Folder…", shortcut: key("Open folder…")) { close(); mv_chrome_menu(17) }
            FlyoutItem(title: current.map { "Open: " + $0 } ?? "Open: (nothing open)",
                       shortcut: key("Show in Explorer"), enabled: current != nil) {
              close(); mv_chrome_menu(20)
            }
          }
          BarFlyout(title: "View") { close in
            FlyoutItem(title: "Zoom in", shortcut: key("Zoom in"), enabled: false) {}
            FlyoutItem(title: "Zoom out", shortcut: key("Zoom out"), enabled: false) {}
            FlyoutRule()
            FlyoutItem(title: "Fit to window", shortcut: key("Fit")) { close(); mv_chrome_fit() }
            FlyoutItem(title: "50 %", enabled: false) {}
            FlyoutItem(title: "100 %", shortcut: key("Zoom 100 %")) { close(); mv_chrome_one_to_one() }
            FlyoutItem(title: "200 %", enabled: false) {}
            FlyoutItem(title: "400 %", enabled: false) {}
            FlyoutRule()
            FlyoutItem(title: "Gallery", shortcut: key("Gallery"), enabled: hasFolder) { close(); mv_chrome_menu(9) }
            FlyoutItem(title: "Full screen", shortcut: key("Fullscreen")) { close(); mv_chrome_menu(10) }
            FlyoutItem(title: "Filmstrip", shortcut: key("Filmstrip"), enabled: hasFolder) { close(); mv_chrome_menu(8) }
            FlyoutRule()
            FlyoutItem(title: "Clipping warnings", shortcut: key("Clipping"), enabled: false) {}
            FlyoutItem(title: "Frame-time overlay", shortcut: key("Frame-time overlay")) { close(); mv_chrome_menu(19) }
            FlyoutItem(title: "Keyboard shortcuts", shortcut: key("Keyboard shortcuts")) { close(); mv_chrome_menu(16) }
          }
          // PR 29 (plan/20): the visible way in to every edit, a bar button like
          // Settings. Return does the same. Shown only over media.
          if edit.showsBarButton {
            Button(edit.open ? "Done" : edit.title) { edit.toggle() }
              .buttonStyle(FlatButtonStyle(selected: edit.open))
              .help(edit.open ? "Close the editor (Return or Esc)"
                              : (edit.isClip ? "Edit video: trim, split, clip tools (Return)"
                                             : "Edit image: crop, rotate, colour, info (Return)"))
              .accessibilityLabel(edit.open ? "Done editing" : edit.title)
              .accessibilityHint("Key Return")
          }
          Button("Settings") { mv_chrome_menu(18) }.buttonStyle(FlatButtonStyle())
          BarFlyout(title: "About") { _ in
            VStack(alignment: .leading, spacing: 0) {
              Text("MediaViewer")
                .font(MVTheme.font())
                .foregroundStyle(MVTheme.title)
              Text(aboutVersionLine())
                .font(MVTheme.font())
                .foregroundStyle(MVTheme.body)
              Text("Copyright (C) 2026 longtimeno-c")
                .font(MVTheme.font())
                .foregroundStyle(MVTheme.body)
                .padding(.top, 6)
              Text("Licensed under GNU GPL v3 or later")
                .font(MVTheme.font())
                .foregroundStyle(MVTheme.body)
              FlyoutItem(title: "Licence (GPL-3.0-or-later)") {
                openLegalFile("LICENSE.txt", github: "LICENSE")
              }
              FlyoutItem(title: "Notice") {
                openLegalFile("NOTICE.txt", github: "NOTICE")
              }
              FlyoutItem(title: "Third-party notices") {
                openLegalFile("THIRD-PARTY.md", github: "THIRD-PARTY.md")
              }
              Text("Everything works from the keyboard. Press ? for the shortcuts of what you are doing.")
                .font(MVTheme.font())
                .foregroundStyle(MVTheme.title)
                .padding(.top, 10)
                .fixedSize(horizontal: false, vertical: true)
            }
            .padding(12)
            .frame(width: 300, alignment: .leading)
          }
        }
        .fixedSize()
        UpdateBarItem()
        // Milestone G: the one-time Import hint and a running import's line.
        AddonBarItems()
        // Milestone H: the indexing pill, only while the AI pack is indexing.
        LocalSearchBarItem()
        Spacer()
        // PR 12: what a rating key or a metadata write just did.
        if !notice.text.isEmpty {
          Text(notice.text)
            .font(MVTheme.font())
            .foregroundStyle(MVTheme.title)
            .padding(.trailing, 10)
            .lineLimit(1)
            .accessibilityLabel(notice.text)
        }
        // What F7 / F8 / Delete will act on (plan/16): the marks if any, else
        // the current item.
        if store.markedCount > 0 {
          Text("\(store.markedCount) marked")
            .font(MVTheme.font())
            .foregroundStyle(MVTheme.body)
            .padding(.trailing, 6)
        }
        // The folder trail sits beside `?` rather than in a row of its own, so
        // opening a folder does not push the canvas down.
        if !store.crumbs.isEmpty || store.listTitle != nil {
          PathBar()
          Rectangle().fill(MVTheme.hairline).frame(width: 1, height: 18).padding(.horizontal, 4)
        }
        Button("?") { mv_chrome_menu(16) }
          .buttonStyle(FlatButtonStyle())
          .help("Keyboard shortcuts  ?")
      }
      .padding(.horizontal, 6)
      .frame(maxHeight: .infinity)
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(MVTheme.canvas)
  }
}

/// PR 20 updates (plan/13), after About like the Windows bar: quiet text while
/// Sparkle checks or downloads, then the one button that restarts the app for
/// an update. Never a modal; quitting normally installs it too.
private struct UpdateBarItem: View {
  @ObservedObject private var store = FolderStore.shared

  var body: some View {
    let version = store.updateVersion
    switch store.updatePhase {
    case 1:
      status("Checking for updates…", help: "Asking GitHub for a newer version.")
    case 2:
      // Sparkle does not report bytes for a silent background download, so the
      // bar is indeterminate here (Windows shows Velopack's percent).
      HStack(spacing: 0) {
        status(version.map { "Downloading update \($0)…" } ?? "Downloading update…",
               help: "Downloading in the background. This does not interrupt anything.")
        ProgressView()
          .progressViewStyle(.linear)
          .controlSize(.small)
          .frame(width: 90)
          .padding(.trailing, 10)
          .accessibilityLabel("Update download progress")
      }
    case 3:
      Button("Update ready — restart") { mv_chrome_restart_to_update() }
        .buttonStyle(FlatButtonStyle())
        .help(version.map { "Version \($0) is downloaded. Restart to use it, or it installs when you quit MediaViewer." }
              ?? "A new version is downloaded. Restart to use it, or it installs when you quit MediaViewer.")
    default:
      EmptyView()
    }
  }

  private func status(_ text: String, help: String) -> some View {
    // Status, not an action: smaller than the bar's buttons.
    Text(text)
      .font(MVTheme.font(12))
      .foregroundStyle(MVTheme.body)
      .padding(.horizontal, 10)
      .lineLimit(1)
      .fixedSize()
      .help(help)
      .accessibilityLabel(text)
  }
}

/// The trail from the highest folder reached to the one on screen, in the
/// command bar just left of `?`. Up and Root are icons outside the scrolling
/// ancestor trail; the current name stays pinned and a long middle becomes an
/// ancestor menu; a search icon ends it. Shown whenever a folder is open,
/// including while a photo is on the canvas.
struct PathBar: View {
  @ObservedObject private var store = FolderStore.shared
  @State private var trailWidth: CGFloat = 0
  @State private var nameWidth: CGFloat = 0
  private let maxTrailWidth: CGFloat = 300
  private let maxNameWidth: CGFloat = 200

  var body: some View {
    // Milestone H: a result list has no folder trail; its title stands in.
    if let title = store.listTitle {
      ListTitleBar(title: title)
    } else {
      trail
    }
  }

  @ViewBuilder private var trail: some View {
    let crumbs = store.crumbs
    let shown = display(crumbs)
    HStack(spacing: 2) {
      PathIcon(symbol: "arrow.up", enabled: store.canGoUp) { store.navigateUp() }
        .help("Open the enclosing folder (⌘↑)")
        .accessibilityLabel("Up one folder")
      PathIcon(symbol: "house", enabled: crumbs.count > 1) { store.openCrumb(0) }
        .help(crumbs.first.map { "Return to browsing root: " + $0.path } ?? "No folder open")
        .accessibilityLabel("Return to browsing root")

      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 4) {
          ForEach(shown.dropLast()) { crumb in
            if crumb.isEllipsis {
              Menu {
                ForEach(crumbs.dropFirst().dropLast(2)) { ancestor in
                  Button(ancestor.name) { store.openCrumb(ancestor.index) }
                    .help(ancestor.path)
                }
              } label: { Text("…") }
              .menuStyle(.borderlessButton)
              .menuIndicator(.hidden)
              .fixedSize()
              .foregroundStyle(MVTheme.body)
              .help("Open a parent folder")
              .accessibilityLabel("Hidden parent folders")
            } else {
              Button(crumb.name) { store.openCrumb(crumb.index) }
                .buttonStyle(.borderless)
                .foregroundStyle(MVTheme.body)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: 120)
                .help(crumbs.first(where: { $0.index == crumb.index })?.path ?? crumb.name)
            }
            Image(systemName: "chevron.right")
              .font(.system(size: 8, weight: .bold))
              .foregroundStyle(MVTheme.body)
          }
        }
        .padding(.leading, 4)
        .background(GeometryReader { geo in
          Color.clear.preference(key: TrailWidthKey.self, value: geo.size.width)
        })
      }
      // Only as wide as the trail, up to a cap; past that it scrolls.
      .frame(width: min(trailWidth, maxTrailWidth))
      .onPreferenceChange(TrailWidthKey.self) { trailWidth = $0 }

      if let current = shown.last {
        // As wide as the name, up to a cap: a bare maxWidth frame took the
        // whole 200 pt for a short name and left the search icon adrift
        // beside nothing (owner report, 2026-09-28).
        Text(current.name)
          .fontWeight(.semibold)
          .foregroundStyle(MVTheme.title)
          .lineLimit(1)
          .truncationMode(.middle)
          .frame(width: min(nameWidth, maxNameWidth), alignment: .leading)
          .background(
            Text(current.name)
              .fontWeight(.semibold)
              .lineLimit(1)
              .fixedSize()
              .hidden()
              .background(GeometryReader { geo in
                Color.clear.preference(key: NameWidthKey.self, value: ceil(geo.size.width))
              })
          )
          .onPreferenceChange(NameWidthKey.self) { nameWidth = $0 }
          .layoutPriority(1)
          .help(crumbs.last?.path ?? current.name)
      }
      PathSearchButton()
    }
    .font(MVTheme.font(14))
    .padding(.horizontal, 6)
  }

  private func display(_ crumbs: [Crumb]) -> [PathPiece] {
    if crumbs.count <= 4 {
      return crumbs.map {
        PathPiece(id: $0.index, index: $0.index, name: $0.name, isEllipsis: false)
      }
    }
    let n = crumbs.count
    return [
      PathPiece(id: crumbs[0].index, index: crumbs[0].index, name: crumbs[0].name, isEllipsis: false),
      PathPiece(id: -1, index: -1, name: "…", isEllipsis: true),
      PathPiece(id: crumbs[n - 2].index, index: crumbs[n - 2].index, name: crumbs[n - 2].name, isEllipsis: false),
      PathPiece(id: crumbs[n - 1].index, index: crumbs[n - 1].index, name: crumbs[n - 1].name, isEllipsis: false),
    ]
  }
}

/// The path row while search results are open (plan/17 "Results are the
/// gallery"): what was searched, how many results, and the way back.
private struct ListTitleBar: View {
  @ObservedObject private var store = FolderStore.shared
  @ObservedObject private var search = LocalSearchStore.shared
  let title: String

  var body: some View {
    HStack(spacing: 8) {
      Button { store.closeList() } label: {
        Label("Back to folder", systemImage: "chevron.backward")
      }
      .buttonStyle(.borderless)
      .fixedSize()
      .foregroundStyle(MVTheme.title)
      .help("Close the results and return to the folder (Esc)")
      Rectangle().fill(MVTheme.hairline).frame(width: 1, height: 16).padding(.horizontal, 4)
      // The ⌘F panel again in one click (Local search loaded); else a plain glyph.
      if search.searchAvailable {
        PathSearchButton()
      } else {
        Image(systemName: "magnifyingglass").foregroundStyle(MVTheme.body)
      }
      // "Search: <query>", the Windows breadcrumb's wording.
      Text(title.isEmpty ? "Search results" : "Search: " + title)
        .font(MVTheme.font(14))
        .fontWeight(.semibold)
        .foregroundStyle(MVTheme.title)
        .lineLimit(1)
        .truncationMode(.tail)
        .layoutPriority(1)
      Text(store.itemCount == 1 ? "1 result" : "\(store.itemCount) results")
        .font(MVTheme.font(13))
        .foregroundStyle(MVTheme.body)
        .contentTransition(.numericText())
      Spacer(minLength: 0)
    }
    .font(MVTheme.font(14))
    .padding(.horizontal, 10)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(MVTheme.canvas)
    .accessibilityElement(children: .contain)
    .accessibilityLabel("Search results for \(title)")
  }
}

private struct PathPiece: Identifiable {
  let id: Int
  let index: Int
  let name: String
  let isEllipsis: Bool
}

private struct NameWidthKey: PreferenceKey {
  static let defaultValue: CGFloat = 0
  static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = nextValue() }
}

private struct TrailWidthKey: PreferenceKey {
  static let defaultValue: CGFloat = 0
  static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = nextValue() }
}

/// The path group's search icon (owner, 2026-09-28; it replaced the gallery
/// search bar). Always there: it does what ⌘F does. With Local search loaded
/// or still starting at launch it opens that panel (a click while it starts
/// opens it once it attaches); without it, file search, which needs no add-on
/// and no index (plan/16 "File search").
private struct PathSearchButton: View {
  @ObservedObject private var search = LocalSearchStore.shared

  var body: some View {
    PathIcon(symbol: "magnifyingglass", enabled: true) {
      if search.searchAvailable {
        search.openSearchWhenReady()
      } else {
        mv_chrome_file_search()
      }
    }
    .help(search.loaded ? "Search photos and videos (⌘F)"
          : search.searchAvailable ? "Search photos and videos (⌘F). Local search is starting…"
          : "Find files by name in this folder (⌘F)")
    .accessibilityLabel(search.searchAvailable ? "Search" : "Find files by name")
  }
}

/// A square icon button for the path group, with the bar's hover wash.
private struct PathIcon: View {
  let symbol: String
  let enabled: Bool
  let action: () -> Void
  @State private var hover = false
  var body: some View {
    Button(action: action) {
      Image(systemName: symbol)
        .font(.system(size: 12, weight: .semibold))
        .frame(width: 26, height: 26)
        .background(RoundedRectangle(cornerRadius: 4)
          .fill(Color.primary.opacity(hover && enabled ? 0.06 : 0)))
        .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .foregroundStyle(enabled ? MVTheme.title : MVTheme.disabled)
    .disabled(!enabled)
    .onHover { hover = $0 }
  }
}
