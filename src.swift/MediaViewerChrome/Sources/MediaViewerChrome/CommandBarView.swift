// SPDX-License-Identifier: GPL-2.0-or-later
// The in-window command bar. Styled to match the Windows island bar
// (IslandHost.cs): the canvas colour, CozetteVector 16 pt, flat text buttons
// with a faint hover wash, dark flyouts with a hairline border, and a hairline
// under the bar. The commands are the same ones the system menu bar runs
// (mv_chrome_menu tags mirror MvMenuCmd in main_mac.mm).
import AppKit
import CoreText
import SwiftUI
import MVChromeBridge

/// The Windows palette (IslandHost.cs): Canvas / Title / Body / Hairline.
enum MVTheme {
  static let canvas = Color(red: 33 / 255, green: 35 / 255, blue: 42 / 255)
  static let title = Color(red: 220 / 255, green: 222 / 255, blue: 228 / 255)
  static let body = Color(red: 150 / 255, green: 154 / 255, blue: 164 / 255)
  static let hairline = Color(red: 58 / 255, green: 60 / 255, blue: 68 / 255)

  private static let registered: Bool = {
    // CMake copies the face beside the executable (cmake/darwin.cmake).
    let dir = Bundle.main.executableURL?.deletingLastPathComponent()
    guard let url = dir?.appendingPathComponent("CozetteVector.ttf") else { return false }
    return CTFontManagerRegisterFontsForURL(url as CFURL, .process, nil)
  }()

  static func font(_ size: CGFloat = 16) -> Font {
    _ = registered
    return .custom("CozetteVector", size: size)
  }
}

private struct FlatButtonStyle: ButtonStyle {
  func makeBody(configuration: Configuration) -> some View {
    FlatButtonBody(configuration: configuration)
  }
  private struct FlatButtonBody: View {
    let configuration: Configuration
    @State private var hover = false
    @Environment(\.isEnabled) private var enabled
    var body: some View {
      configuration.label
        .font(MVTheme.font())
        .foregroundStyle(enabled ? MVTheme.title : MVTheme.body.opacity(0.5))
        .padding(.horizontal, 14).padding(.vertical, 7)
        .background(
          RoundedRectangle(cornerRadius: 4).fill(Color.white.opacity(
            configuration.isPressed ? 0.11 : (hover && enabled ? 0.06 : 0))))
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

/// A bar button that opens a dark flyout beneath it.
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
          .preferredColorScheme(.dark)
      }
  }
}

public struct CommandBarView: View {
  @ObservedObject private var store = FolderStore.shared

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
        // Same order and labels as the Windows bar: Open, View, Settings, About;
        // `?` at the far right.
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
        Button("Settings") { mv_chrome_menu(18) }.buttonStyle(FlatButtonStyle())
        BarFlyout(title: "About") { _ in
          VStack(alignment: .leading, spacing: 0) {
            Text("MediaViewer")
              .font(MVTheme.font())
              .foregroundStyle(MVTheme.title)
            Text(aboutVersionLine())
              .font(MVTheme.font())
              .foregroundStyle(MVTheme.body)
            Text("Licensed GPL-2.0-or-later")
              .font(MVTheme.font())
              .foregroundStyle(MVTheme.body)
              .padding(.top, 6)
            Text("Everything works from the keyboard. Press ? for the shortcuts of what you are doing.")
              .font(MVTheme.font())
              .foregroundStyle(MVTheme.title)
              .padding(.top, 10)
              .fixedSize(horizontal: false, vertical: true)
          }
          .padding(12)
          .frame(width: 300, alignment: .leading)
        }
        // Milestone G: the one-time Import hint and a running import's line.
        AddonBarItems()
        Spacer()
        // What F7 / F8 / Delete will act on (plan/16): the marks if any, else
        // the current item.
        if store.markedCount > 0 {
          Text("\(store.markedCount) marked")
            .font(MVTheme.font())
            .foregroundStyle(MVTheme.body)
            .padding(.trailing, 6)
        }
        Button("?") { mv_chrome_menu(16) }
          .buttonStyle(FlatButtonStyle())
          .help("Keyboard shortcuts  ?")
      }
      .padding(.horizontal, 6)
      .frame(maxHeight: .infinity)
      if !store.crumbs.isEmpty {
        PathBar()
          .frame(height: 27)
      }
      Rectangle().fill(MVTheme.hairline).frame(height: 1)
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity)
    .background(MVTheme.canvas)
  }
}

/// The trail from the highest folder reached to the one on screen. The current
/// name stays pinned; a long middle becomes an ancestor menu. Up and Root stay outside the scroller.
/// Visible whenever a folder is open, including while a photo is on the canvas.
struct PathBar: View {
  @ObservedObject private var store = FolderStore.shared

  var body: some View {
    let crumbs = store.crumbs
    let shown = display(crumbs)
    HStack(spacing: 4) {
      Button { store.navigateUp() } label: {
        Label("Up", systemImage: "arrow.up")
      }
      .buttonStyle(.borderless)
      .fixedSize()
      .disabled(!store.canGoUp)
      .help("Open the enclosing folder (⌘↑)")
      .accessibilityLabel("Up one folder")
      .foregroundStyle(store.canGoUp ? MVTheme.title : MVTheme.body.opacity(0.4))

      Button { store.openCrumb(0) } label: {
        Label("Root", systemImage: "folder")
      }
      .buttonStyle(.borderless)
      .fixedSize()
      .disabled(crumbs.count < 2)
      .help(crumbs.first.map { "Return to browsing root: " + $0.path } ?? "No folder open")
      .accessibilityLabel("Return to browsing root")
      .foregroundStyle(crumbs.count > 1 ? MVTheme.title : MVTheme.body.opacity(0.4))
      Rectangle().fill(MVTheme.hairline).frame(width: 1, height: 16).padding(.horizontal, 6)

      ScrollView(.horizontal, showsIndicators: false) {
        HStack(spacing: 4) {
          ForEach(shown.dropLast()) { crumb in
            if crumb.id != shown.first?.id {
              Image(systemName: "chevron.right")
                .font(.system(size: 8, weight: .bold))
                .foregroundStyle(MVTheme.body)
            }
            if crumb.isEllipsis {
              Menu {
                ForEach(crumbs.dropFirst().dropLast(2)) { ancestor in
                  Button(ancestor.name) { store.openCrumb(ancestor.index) }
                    .help(ancestor.path)
                }
              } label: { Text("…") }
              .menuStyle(.borderlessButton)
              .fixedSize()
              .foregroundStyle(MVTheme.title)
              .help("Open a parent folder")
              .accessibilityLabel("Hidden parent folders")
            } else {
              Button(crumb.name) { store.openCrumb(crumb.index) }
                .buttonStyle(.borderless)
                .foregroundStyle(MVTheme.title)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: 160)
                .help(crumbs.first(where: { $0.index == crumb.index })?.path ?? crumb.name)
            }
          }
        }
      }
      if let current = shown.last {
        if shown.count > 1 {
          Image(systemName: "chevron.right")
            .font(.system(size: 8, weight: .bold))
            .foregroundStyle(MVTheme.body)
        }
        Text(current.name)
          .font(MVTheme.font(14))
          .fontWeight(.semibold)
          .foregroundStyle(MVTheme.title)
          .lineLimit(1)
          .truncationMode(.middle)
          .frame(maxWidth: 240, alignment: .leading)
          .layoutPriority(1)
          .help(crumbs.last?.path ?? current.name)
      }
      Spacer(minLength: 0)
    }
    .font(MVTheme.font(14))
    .padding(.horizontal, 10)
    .frame(maxWidth: .infinity, alignment: .leading)
    .background(MVTheme.canvas)
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

private struct PathPiece: Identifiable {
  let id: Int
  let index: Int
  let name: String
  let isEllipsis: Bool
}

