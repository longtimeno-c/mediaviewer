// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Connection, as the app's settings: the Synology's status, where the catalogue came from and
// when, what is kept on this phone, the look, the viewer's filmstrip, and Forget. Each failure
// names its cause.
import MobileKit
import SwiftUI

struct SettingsScreen: View {
  @Environment(AppModel.self) private var model
  @State private var confirmingForget = false
  @AppStorage(Preferences.filmstrip) private var showsFilmstrip = true
  @AppStorage(Preferences.appearance) private var appearance = "dark"

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 12) {
          failureCard
          section("Library") {
            if model.connection.isSample {
              InfoRow(label: "Library", value: "Sample library")
              Text(FailureCopy.sampleAbout)
                .font(.footnote)
                .foregroundStyle(Theme.body)
                .fixedSize(horizontal: false, vertical: true)
              if case .reachable(let address, _) = model.connection.synology {
                InfoRow(label: "Served from", value: address)
              }
            } else if case .reachable(let address, _) = model.connection.synology {
              InfoRow(label: "Synology", value: synologyWords)
              InfoRow(label: "Address", value: address)
            }
            if model.connection.fileMode {
              InfoRow(label: "Catalogue", value: FailureCopy.fileModeSettings)
            }
            if let catalogue = model.connection.catalogue {
              InfoRow(label: "Catalogue from",
                      value: model.connection.isSample ? "GitHub" : catalogue.source == .synology ? "Your Synology" : "Your Mac, nearby")
              InfoRow(label: "Updated", value: catalogue.updatedAt.formatted(.relative(presentation: .named)))
              InfoRow(label: "Items", value: catalogue.itemCount.formatted())
            }
            InfoRow(label: "Saved on this phone", value: ProgressWords.size(model.connection.cacheBytes))
          }
          section("Look") {
            Picker("Appearance", selection: $appearance) {
              Text("Dark").tag("dark")
              Text("Light").tag("light")
              Text("System").tag("system")
            }
            .pickerStyle(.segmented)
            .accessibilityLabel("Appearance")
            Toggle(isOn: $showsFilmstrip) {
              Text("Filmstrip in the viewer").font(.subheadline).foregroundStyle(Theme.title)
            }
            .tint(Theme.accent)
          }
          section("About") {
            InfoRow(label: "Version", value: Self.version)
            NavigationLink {
              AcknowledgementsScreen()
            } label: {
              HStack {
                Text("Acknowledgements").font(.subheadline).foregroundStyle(Theme.title)
                Spacer()
                Image(systemName: "chevron.right").font(.footnote.weight(.semibold)).foregroundStyle(Theme.body)
              }
              .frame(minHeight: 44)
              .contentShape(Rectangle())
            }
            .buttonStyle(TileButtonStyle())
          }
          Button(model.connection.isSample ? "Forget the sample library" : "Forget this library", role: .destructive) {
            confirmingForget = true
          }
            .buttonStyle(QuietButtonStyle(color: Theme.danger))
            .frame(maxWidth: .infinity)
            .confirmationDialog(model.connection.isSample ? "Forget the sample library?" : "Forget this library?",
                                isPresented: $confirmingForget, titleVisibility: .visible) {
              Button("Forget", role: .destructive) { model.forgetLibrary() }
            } message: {
              Text(model.connection.isSample
                   ? "Everything MediaViewer keeps on this phone for it is removed. To open it again, enter the code \(FailureCopy.sampleCode) on the Scan screen."
                   : "Everything MediaViewer keeps on this phone is removed. Nothing on your Synology changes. You'll need a new code from MediaViewer to connect again.")
            }
        }
        .frame(maxWidth: Theme.readableWidth)
        .padding(Theme.inset)
        .frame(maxWidth: .infinity)
      }
      .background(Theme.ground)
      .navigationTitle("Settings")
      .navigationBarTitleDisplayMode(.inline)
    }
  }

  private static var version: String {
    let info = Bundle.main.infoDictionary
    let short = info?["CFBundleShortVersionString"] as? String ?? "?"
    let build = info?["CFBundleVersion"] as? String ?? "?"
    return "\(short) (\(build))"
  }

  private func section<Content: View>(_ title: String, @ViewBuilder _ content: () -> Content) -> some View {
    VStack(alignment: .leading, spacing: 8) {
      Text(title)
        .font(.subheadline.weight(.semibold))
        .foregroundStyle(Theme.title)
        .padding(.horizontal, Theme.inset)
        .accessibilityAddTraits(.isHeader)
      InfoCard { content() }
    }
    .padding(.top, 4)
  }

  private var synologyWords: String {
    if case .reachable(_, overInternet: false) = model.connection.synology { return "Connected on its own network" }
    return "Connected over the internet"
  }

  @ViewBuilder
  private var failureCard: some View {
    switch model.connection.synology {
    case .reachable:
      EmptyView()
    case .unreachable(let tried):
      VStack(alignment: .leading, spacing: 8) {
        Text(model.connection.isSample ? FailureCopy.sampleUnreachable : FailureCopy.synologyUnreachable)
          .font(.subheadline).foregroundStyle(Theme.title)
        ForEach(tried) { AddressFailureRow(failure: $0) }
        Button("Try again") { Task { await model.services.connection.retry() } }
          .buttonStyle(SecondaryButtonStyle())
          .padding(.top, 4)
      }
      .padding(Theme.inset)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(Theme.surface))
    case .reachableOnlyOnItsOwnNetwork:
      NoticeCard(message: FailureCopy.ownNetworkOnly)
    case .serviceTooOld:
      NoticeCard(message: FailureCopy.serviceTooOld)
    case .publisherChanged where model.connection.isSample:
      NoticeCard(message: FailureCopy.sampleRejected, actionTitle: "Try again") { Task { await model.services.connection.retry() } }
    case .publisherChanged:
      NoticeCard(message: FailureCopy.publisherChanged, actionTitle: "Scan code") { model.isScanning = true }
    case .deviceRevoked:
      NoticeCard(message: FailureCopy.deviceRevoked, actionTitle: "Scan code") { model.isScanning = true }
    }
  }
}

/// The notices the libraries built into the app ask to travel with it.
private struct AcknowledgementsScreen: View {
  /// Acknowledgements.txt (tools/ios/acknowledgements.py): paragraphs a blank line apart, each
  /// library's opening with a "# Name" line.
  private let blocks: [String] = {
    guard let url = Bundle.main.url(forResource: "Acknowledgements", withExtension: "txt"),
          let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
    return text.components(separatedBy: "\n\n").map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { !$0.isEmpty }
  }()

  var body: some View {
    ScrollView {
      LazyVStack(alignment: .leading, spacing: 10) {
        ForEach(Array(blocks.enumerated()), id: \.offset) { _, block in
          if block.hasPrefix("# ") {
            Text(block.dropFirst(2))
              .font(.headline)
              .foregroundStyle(Theme.title)
              .padding(.top, 18)
              .accessibilityAddTraits(.isHeader)
          } else {
            Text(block)
              .font(.footnote)
              .foregroundStyle(Theme.body)
              .fixedSize(horizontal: false, vertical: true)
          }
        }
      }
      .textSelection(.enabled)
      .frame(maxWidth: 680, alignment: .leading)
      .frame(maxWidth: .infinity, alignment: .leading)
      .padding(Theme.inset)
    }
    .background(Theme.ground)
    .navigationTitle("Acknowledgements")
    .navigationBarTitleDisplayMode(.inline)
  }
}
