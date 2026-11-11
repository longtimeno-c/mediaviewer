// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileKit
import SwiftUI

public struct RootView: View {
  @State private var model: AppModel
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @AppStorage(Preferences.appearance) private var appearance = "dark"

  public init(model: AppModel) { _model = State(initialValue: model) }

  public var body: some View {
    ZStack {
      Theme.ground.ignoresSafeArea()
      switch model.stage {
      case .welcome: WelcomeScreen().transition(.opacity)
      case .connect(let code, let start): ConnectScreen(code: code, start: start).id("\(code)#\(model.scanCount)").transition(.opacity)
      case .firstCopy: FirstCopyScreen().transition(.opacity)
      case .main: MainTabs().transition(.opacity)
      }
    }
    .animation(reduceMotion ? nil : .easeOut(duration: 0.2), value: model.stage)
    .environment(model)
    .preferredColorScheme(chosenScheme(appearance))
    .tint(Theme.accent)
    .fullScreenCover(isPresented: $model.isScanning) {
      ScanScreen().environment(model).preferredColorScheme(.dark).tint(Theme.accent)
    }
    // The link MediaViewer shows beside its code, for when the camera cannot be used (§11.1).
    .onOpenURL { url in
      guard url.scheme == "mediaviewer", url.host == "pair" else { return }
      model.openedPairLink(url.absoluteString)
    }
    // An alert, not a confirmation dialog: one up here would keep Settings' Forget dialog from showing.
    .alert("Pair with another library?", isPresented: Binding(
      get: { model.pendingPairLink != nil }, set: { if !$0 { model.pendingPairLink = nil } }
    )) {
      Button("Pair with the new library") { model.confirmPendingPairLink() }
      Button("Keep this library", role: .cancel) { model.pendingPairLink = nil }
    } message: {
      Text("Once the new library is paired, everything MediaViewer keeps on this phone for the current one is removed. Nothing on your Synology changes.")
    }
    .task { await model.run() }
  }
}

struct MainTabs: View {
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @Namespace private var viewerNamespace

  var body: some View {
    @Bindable var model = model
    TabView(selection: $model.tab) {
      Tab("Library", systemImage: "photo.on.rectangle.angled", value: AppModel.Tab.library) {
        LibraryScreen().transferStrip(model)
      }
      Tab("Search", systemImage: "magnifyingglass", value: AppModel.Tab.search, role: .search) {
        SearchScreen().transferStrip(model)
      }
      if !model.people.isEmpty || model.peopleFailed {
        Tab("People", systemImage: "person.2", value: AppModel.Tab.people) {
          PeopleScreen().transferStrip(model)
        }
      }
      Tab("Settings", systemImage: "gearshape", value: AppModel.Tab.connection) {
        SettingsScreen().transferStrip(model)
      }
    }
    .tabViewStyle(.sidebarAdaptable)
    .modifier(TabBarShrinks())
    .transferAccessory(model)
    .environment(\.viewerNamespace, viewerNamespace)
    .fullScreenCover(item: $model.viewer) { request in
      ViewerScreen(request: request, namespace: viewerNamespace)
        .environment(model)
        .preferredColorScheme(.dark)
        .tint(Theme.accent)
    }
  }
}

/// Scrolling down gives the photos the room the tab bar was using (iOS 26 and later).
private struct TabBarShrinks: ViewModifier {
  func body(content: Content) -> some View {
    if #available(iOS 26.0, *) {
      content.tabBarMinimizeBehavior(.onScrollDown)
    } else {
      content
    }
  }
}

// Backup (release 1.1, docs/plans/mobile.md §10b) is not in this release: its slot above the tab
// bar compiles only with MV_BACKUP, which no build defines yet.
private extension View {
  func transferStrip(_ model: AppModel) -> some View {
    #if MV_BACKUP
    modifier(TransferStrip(activity: model.transfer))
    #else
    self
    #endif
  }

  func transferAccessory(_ model: AppModel) -> some View {
    #if MV_BACKUP
    modifier(TransferAccessory(activity: model.transfer))
    #else
    self
    #endif
  }
}

#if MV_BACKUP
/// The slot above the tab bar, there only while something is running (iOS 26.1 and later).
private struct TransferAccessory: ViewModifier {
  let activity: TransferActivity?

  func body(content: Content) -> some View {
    if #available(iOS 26.1, *) {
      content.tabViewBottomAccessory(isEnabled: activity != nil) {
        if let activity { TransferLine(activity: activity).padding(.horizontal, 16) }
      }
    } else {
      content
    }
  }
}

/// Earlier systems have no such slot, so the same line sits at the bottom of each tab.
private struct TransferStrip: ViewModifier {
  let activity: TransferActivity?

  func body(content: Content) -> some View {
    if #available(iOS 26.1, *) {
      content
    } else {
      content.safeAreaInset(edge: .bottom, spacing: 0) {
        if let activity {
          TransferLine(activity: activity)
            .padding(Theme.inset)
            .background(Theme.surface)
        }
      }
    }
  }
}

/// One line of words over the one progress bar; the words shorten before they wrap.
private struct TransferLine: View {
  let activity: TransferActivity

  var body: some View {
    VStack(alignment: .leading, spacing: 4) {
      ViewThatFits(in: .horizontal) {
        words(activity.line)
        words(activity.shortLine)
      }
      ProgressTrack(fraction: activity.fraction)
    }
    .dynamicTypeSize(...DynamicTypeSize.xxLarge)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(activity.line)
  }

  private func words(_ text: String) -> some View {
    Text(text)
      .font(.footnote.weight(.medium).monospacedDigit())
      // The system's own colour, which the glass keeps readable over a bright photo.
      .foregroundStyle(.primary)
      .lineLimit(1)
      .fixedSize(horizontal: true, vertical: false)
  }
}
#endif
