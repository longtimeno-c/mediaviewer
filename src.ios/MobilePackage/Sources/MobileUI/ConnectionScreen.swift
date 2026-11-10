// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Connection: Synology status, where the catalogue came from and when, cache size, forget.
// Each failure names its cause.
import MobileKit
import SwiftUI

struct ConnectionScreen: View {
  @Environment(AppModel.self) private var model
  @State private var confirmingForget = false
  @AppStorage(Preferences.filmstrip) private var showsFilmstrip = true

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(spacing: 12) {
          failureCard
          if case .reachable(let address, _) = model.connection.synology {
            InfoCard {
              InfoRow(label: "Synology", value: synologyWords)
              InfoRow(label: "Address", value: address)
            }
          }
          if let catalogue = model.connection.catalogue {
            InfoCard {
              InfoRow(label: "Catalogue from", value: catalogue.source == .synology ? "Your Synology" : "Your Mac, nearby")
              InfoRow(label: "Updated", value: catalogue.updatedAt.formatted(.relative(presentation: .named)))
              InfoRow(label: "Items", value: catalogue.itemCount.formatted())
            }
          }
          InfoCard {
            InfoRow(label: "Saved on this phone", value: ProgressWords.size(model.connection.cacheBytes))
          }
          InfoCard {
            Toggle(isOn: $showsFilmstrip) {
              Text("Filmstrip in the viewer").font(.subheadline).foregroundStyle(Theme.title)
            }
            .tint(Theme.accent)
          }
          Button("Forget this library", role: .destructive) { confirmingForget = true }
            .buttonStyle(QuietButtonStyle(color: Color.red))
            .confirmationDialog("Forget this library?", isPresented: $confirmingForget, titleVisibility: .visible) {
              Button("Forget", role: .destructive) { model.forgetLibrary() }
            } message: {
              Text("You'll need a new code from MediaViewer to connect again.")
            }
        }
        .frame(maxWidth: Theme.readableWidth)
        .padding(Theme.inset)
        .frame(maxWidth: .infinity)
      }
      .background(Theme.ground)
      .navigationTitle("Connection")
      .navigationBarTitleDisplayMode(.inline)
    }
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
        Text(FailureCopy.synologyUnreachable).font(.subheadline).foregroundStyle(Theme.title)
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
    case .publisherChanged:
      NoticeCard(message: FailureCopy.publisherChanged, actionTitle: "Scan code") { model.isScanning = true }
    }
  }
}
