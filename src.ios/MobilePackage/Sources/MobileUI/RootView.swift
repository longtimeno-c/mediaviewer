// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
import MobileKit
import SwiftUI

public struct RootView: View {
  @State private var model: AppModel
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

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
    .preferredColorScheme(.dark)
    .tint(Theme.accent)
    .fullScreenCover(isPresented: $model.isScanning) {
      ScanScreen().environment(model).preferredColorScheme(.dark).tint(Theme.accent)
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
        LibraryScreen().modifier(TransferStrip(activity: model.transfer))
      }
      Tab("Search", systemImage: "magnifyingglass", value: AppModel.Tab.search, role: .search) {
        SearchScreen().modifier(TransferStrip(activity: model.transfer))
      }
      if !model.people.isEmpty || model.peopleFailed {
        Tab("People", systemImage: "person.2", value: AppModel.Tab.people) {
          PeopleScreen().modifier(TransferStrip(activity: model.transfer))
        }
      }
      Tab("Connection", systemImage: "externaldrive.connected.to.line.below", value: AppModel.Tab.connection) {
        ConnectionScreen().modifier(TransferStrip(activity: model.transfer))
      }
    }
    .tabViewStyle(.sidebarAdaptable)
    .modifier(TabBarShrinks())
    .modifier(TransferAccessory(activity: model.transfer))
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
