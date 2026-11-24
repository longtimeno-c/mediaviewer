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
    // mediaviewer://sample is the sample library's QR code and link (§11.1), opened the same way;
    // mediaviewer://sample-guest the same library as a guest (nas-owner-and-guests.md §8.2).
    .onOpenURL { url in
      guard url.scheme == "mediaviewer", url.host == "pair" || url.host == "sample" || url.host == "sample-guest" else { return }
      model.openedPairLink(url.absoluteString)
    }
    // An alert, not a confirmation dialog: one up here would keep Settings' Forget dialog from showing.
    .alert("Pair with another library?", isPresented: Binding(
      get: { model.pendingPairLink != nil }, set: { if !$0 { model.pendingPairLink = nil } }
    )) {
      Button("Pair with the new library") { model.confirmPendingPairLink() }
      Button("Keep this library", role: .cancel) { model.pendingPairLink = nil }
    } message: {
      Text(model.connection.isSample
           ? "Once the new library is paired, the sample library is removed from this phone."
           : "Once the new library is paired, everything MediaViewer keeps on this phone for the current one is removed. Nothing on your server changes.")
    }
    .task { await model.run() }
  }
}

struct MainTabs: View {
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @Namespace private var viewerNamespace
  /// What the slot above the tab bar shows: `model.transfer`, held still while the keyboard is up.
  @State private var slot: TransferActivity?

  var body: some View {
    @Bindable var model = model
    TabView(selection: $model.tab) {
      Tab("Library", systemImage: "photo.on.rectangle.angled", value: AppModel.Tab.library) {
        LibraryScreen().transferStrip()
      }
      Tab("Search", systemImage: "magnifyingglass", value: AppModel.Tab.search, role: .search) {
        SearchScreen().transferStrip()
      }
      if !model.people.isEmpty || model.peopleFailed {
        Tab("People", systemImage: "person.2", value: AppModel.Tab.people) {
          PeopleScreen().transferStrip()
        }
      }
      Tab("Settings", systemImage: "gearshape", value: AppModel.Tab.connection) {
        SettingsScreen().transferStrip()
      }
    }
    .tabViewStyle(.sidebarAdaptable)
    .modifier(TabBarShrinks(searching: model.tab == .search))
    .modifier(TransferAccessory(activity: slot))
    .background(TransferSlotFollower(slot: $slot))
    .environment(\.viewerNamespace, viewerNamespace)
    .fullScreenCover(item: $model.viewer) { request in
      ViewerScreen(request: request, namespace: viewerNamespace)
        .environment(model)
        .preferredColorScheme(.dark)
        .tint(Theme.accent)
    }
  }
}

/// Scrolling down gives the photos the room the tab bar was using (iOS 26 and later). Not on Search:
/// results arriving under the field while someone types must not fold the bar the field belongs to.
private struct TabBarShrinks: ViewModifier {
  let searching: Bool

  func body(content: Content) -> some View {
    if #available(iOS 26.0, *) {
      content.tabBarMinimizeBehavior(searching ? .never : .onScrollDown)
    } else {
      content
    }
  }
}

// Phone backup (release 1.1, docs/plans/mobile.md §10b): its line above the tab bar while it runs.
private extension View {
  func transferStrip() -> some View { modifier(TransferStrip()) }
}

/// Copies `model.transfer` into the slot, except while the keyboard is up. Then iOS shows the slot
/// on top of the keyboard, and any change to it (a new count, the work starting or ending) puts
/// the keyboard away and brings it back: with a backup running at launch the keyboard left Search
/// as the person typed. The slot catches up when the keyboard goes. Even redrawing the tabs with
/// the same slot does it, so the count and the keyboard's state live in this view, and the tabs
/// change only when the slot does.
private struct TransferSlotFollower: View {
  @Environment(AppModel.self) private var model
  @Binding var slot: TransferActivity?
  @State private var keyboardUp = false

  var body: some View {
    Color.clear
      .onAppear { slot = model.transfer }
      .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillShowNotification)) { _ in keyboardUp = true }
      .onReceive(NotificationCenter.default.publisher(for: UIResponder.keyboardWillHideNotification)) { _ in keyboardUp = false }
      .onChange(of: model.transfer) { _, new in if !keyboardUp { slot = new } }
      .onChange(of: keyboardUp) { _, up in if !up { slot = model.transfer } }
  }
}

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

/// Earlier systems have no such slot, so the same line sits at the bottom of each tab. It reads the
/// count itself, so the tabs (and the slot above, TransferSlotFollower) are not redrawn with it.
private struct TransferStrip: ViewModifier {
  @Environment(AppModel.self) private var model

  func body(content: Content) -> some View {
    if #available(iOS 26.1, *) {
      content
    } else {
      content.safeAreaInset(edge: .bottom, spacing: 0) {
        if let activity = model.transfer {
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
