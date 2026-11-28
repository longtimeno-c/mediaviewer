// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Connection, as the app's settings: your server's status, where the catalogue came from and
// when, what is kept on this phone, the look, the viewer's filmstrip, and Forget. Each failure
// names its cause.
import MobileKit
import SwiftUI

/// Settings' words, shared with the tests.
enum SettingsCopy {
  static let directOnMobileData = "Faster direct connection on mobile data"
  static let directOnMobileDataDetail =
    "When MediaViewer reaches your server through your own relay, it tries to connect to it directly, which is faster. On Wi-Fi it always tries."
  /// What kind of server the sample library stands in for.
  static let sampleServerKind = "A computer"
  /// The sample's stand-in for a server's mirror (docs/plans/self-host-drives.md §4).
  static let sampleDrives = "Server's drives"
  static let sampleMirrorHealthy = "A mirror of two drives, both working"
  static let sampleMirrorLost = "A mirror that lost a drive"
  static let sampleLoseDrive = "Show a lost drive"
  static let sampleMendDrive = "Put the drive back"
  static let serverDrives = "Server's drives"
}

struct SettingsScreen: View {
  @Environment(AppModel.self) private var model
  @State private var confirmingForget = false
  @AppStorage(Preferences.filmstrip) private var showsFilmstrip = true
  @AppStorage(Preferences.appearance) private var appearance = "dark"
  @AppStorage(Preferences.directOnMobileData) private var directOnMobileData = true
  @Environment(\.scenePhase) private var scenePhase
  /// What locks this phone; read again on coming back, as a passcode may have been set meanwhile.
  @State private var lockKind: OwnerCheckKind?

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 12) {
          failureCard
          if model.connection.serverStorage > 0, !model.connection.serverStorageWords.isEmpty {
            NoticeCard(message: model.connection.serverStorageWords)
              .accessibilityLabel(SettingsCopy.serverDrives + ": " + model.connection.serverStorageWords)
          }
          section("Library") {
            if model.connection.isSample {
              InfoRow(label: "Library", value: "Sample library")
              Text(FailureCopy.sampleAbout)
                .font(.footnote)
                .foregroundStyle(Theme.body)
                .fixedSize(horizontal: false, vertical: true)
              // The sample stands in for a server that is a computer (docs/plans/self-hosted.md §8),
              // so the words a self-hosted library shows can be seen without one.
              InfoRow(label: "Server", value: SettingsCopy.sampleServerKind)
              // The phone stands in for the server's mirror, so "lost a drive" can be seen with no server.
              InfoRow(label: SettingsCopy.sampleDrives,
                      value: model.connection.serverStorage == 2 ? SettingsCopy.sampleMirrorLost : SettingsCopy.sampleMirrorHealthy)
              Button(model.connection.serverStorage == 2 ? SettingsCopy.sampleMendDrive : SettingsCopy.sampleLoseDrive) {
                let lose = model.connection.serverStorage != 2
                Task { await model.services.connection.showSampleLostDrive(lose) }
              }
              .buttonStyle(SecondaryButtonStyle())
              if case .reachable(let address, _, _) = model.connection.synology {
                InfoRow(label: "Served from", value: address)
              }
            } else if case .reachable(let address, _, _) = model.connection.synology {
              InfoRow(label: "Server", value: synologyWords)
              InfoRow(label: "Address", value: address)
            }
            if let end = model.connection.accessUntil {
              InfoRow(label: "Access", value: FailureCopy.accessUntil(end))
            }
            if model.connection.fileMode {
              InfoRow(label: "Catalogue", value: FailureCopy.fileModeSettings)
            }
            if let catalogue = model.connection.catalogue {
              InfoRow(label: "Catalogue from",
                      value: model.connection.isSample ? "GitHub" : catalogue.source == .synology ? "Your server" : "Your Mac, nearby")
              InfoRow(label: "Updated", value: catalogue.updatedAt.formatted(.relative(presentation: .named)))
              InfoRow(label: "Items", value: catalogue.itemCount.formatted())
            }
            InfoRow(label: "Saved on this phone", value: ProgressWords.size(model.connection.cacheBytes))
            soundSearch
          }
          section("Mobile data") {
            Toggle(isOn: $directOnMobileData) {
              VStack(alignment: .leading, spacing: 2) {
                Text(SettingsCopy.directOnMobileData).font(.subheadline).foregroundStyle(Theme.title)
                Text(SettingsCopy.directOnMobileDataDetail)
                  .font(.footnote)
                  .foregroundStyle(Theme.body)
                  .fixedSize(horizontal: false, vertical: true)
              }
            }
            .tint(Theme.accent)
            // VoiceOver: the name, then the detail once as the hint (not read twice from the label).
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(SettingsCopy.directOnMobileData)
            .accessibilityValue(directOnMobileData ? "On" : "Off")
            .accessibilityHint(SettingsCopy.directOnMobileDataDetail)
            .accessibilityAddTraits(.isToggle)
            .accessibilityAction { directOnMobileData.toggle() }
            .accessibilityIdentifier("directOnMobileData")
          }
          section(VPNCopy.section) {
            NavigationLink {
              VPNScreen()
            } label: {
              HStack {
                VStack(alignment: .leading, spacing: 2) {
                  Text(VPNCopy.toggle).font(.subheadline).foregroundStyle(Theme.title)
                  Text(VPNCopy.summary(model.vpn)).font(.footnote).foregroundStyle(Theme.body).lineLimit(2)
                }
                Spacer()
                Image(systemName: "chevron.right").font(.footnote.weight(.semibold)).foregroundStyle(Theme.body)
              }
              .frame(minHeight: 44)
              .contentShape(Rectangle())
            }
            .buttonStyle(TileButtonStyle())
            .accessibilityIdentifier(VPNIDs.settingsRow)
          }
          section("Backup") {
            NavigationLink {
              BackupScreen()
            } label: {
              HStack {
                VStack(alignment: .leading, spacing: 2) {
                  Text("Back up this phone").font(.subheadline).foregroundStyle(Theme.title)
                  Text(backupSummary).font(.footnote).foregroundStyle(Theme.body).lineLimit(2)
                }
                Spacer()
                Image(systemName: "chevron.right").font(.footnote.weight(.semibold)).foregroundStyle(Theme.body)
              }
              .frame(minHeight: 44)
              .contentShape(Rectangle())
            }
            .buttonStyle(TileButtonStyle())
            .accessibilityIdentifier("settings.backup")
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
          section(LockCopy.section) { lockToggle }
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
                   : "Everything MediaViewer keeps on this phone is removed. Nothing on your server changes. You'll need a new code from MediaViewer to connect again.")
            }
        }
        .frame(maxWidth: Theme.readableWidth)
        .padding(Theme.inset)
        .frame(maxWidth: .infinity)
      }
      .background(Theme.ground)
      .navigationTitle("Settings")
      .navigationBarTitleDisplayMode(.inline)
      .navigationDestination(isPresented: Binding(get: { model.showsBackup }, set: { model.showsBackup = $0 })) {
        BackupScreen()
      }
      .navigationDestination(isPresented: Binding(get: { model.showsVPN }, set: { model.showsVPN = $0 })) {
        VPNScreen()
      }
      .onAppear { lockKind = model.lock.kind() }
      .onChange(of: scenePhase) { _, phase in if phase == .active { lockKind = model.lock.kind() } }
    }
  }

  /// docs/plans/mobile.md §12: turning it on asks for Face ID (or the passcode) first, so it is
  /// known to work before it is needed.
  @ViewBuilder
  private var lockToggle: some View {
    let lock = model.lock
    let name = LockCopy.toggle(lockKind ?? .passcode)
    let detail = lockKind == nil && !lock.isOn ? LockCopy.noPasscode : LockCopy.detail
    Toggle(isOn: Binding(get: { lock.isOn }, set: { on in
      if on { Task { await lock.turnOn() } } else { lock.turnOff() }
    })) {
      VStack(alignment: .leading, spacing: 2) {
        Text(name).font(.subheadline).foregroundStyle(Theme.title)
        Text(detail)
          .font(.footnote)
          .foregroundStyle(Theme.body)
          .fixedSize(horizontal: false, vertical: true)
      }
    }
    .tint(Theme.accent)
    .disabled((lockKind == nil && !lock.isOn) || lock.isChecking)
    .accessibilityElement(children: .ignore)
    .accessibilityLabel(name)
    .accessibilityValue(lock.isOn ? "On" : "Off")
    .accessibilityHint(detail)
    .accessibilityAddTraits(.isToggle)
    .accessibilityAction { if lock.isOn { lock.turnOff() } else { Task { await lock.turnOn() } } }
    .accessibilityIdentifier("settings.lock")
  }

  /// §9 "Sound search": what clips sound like, once its model is on the phone. Shown only for a
  /// library whose clips the Mac listened to.
  @ViewBuilder
  private var soundSearch: some View {
    switch model.connection.soundSearch {
    case .notNeeded:
      EmptyView()
    case .ready:
      InfoRow(label: "Sound search", value: "On")
    case .downloading(let done, let total):
      InfoRow(label: "Sound search", value: "Downloading · " + ProgressWords.amounts(done, of: total))
    case .waitingForWiFi(let bytes):
      InfoRow(label: "Sound search", value: "Waiting for Wi-Fi")
      Text("Search can find clips by what they sound like once a \(ProgressWords.size(bytes)) model is on this phone. It downloads on Wi-Fi.")
        .font(.footnote)
        .foregroundStyle(Theme.body)
        .fixedSize(horizontal: false, vertical: true)
      Button("Download now over mobile data") {
        Task { await model.services.sync.downloadSoundSearch(overMobileData: true) }
      }
      .buttonStyle(QuietButtonStyle(color: Theme.accent))
    case .unavailable(let problem):
      InfoRow(label: "Sound search", value: Self.words(problem))
      if problem == .offline || problem == .rejected {
        Button("Try again") { Task { await model.services.sync.downloadSoundSearch(overMobileData: false) } }
          .buttonStyle(QuietButtonStyle(color: Theme.accent))
      }
    }
  }

  private static func words(_ problem: SoundSearchProblem) -> String {
    switch problem {
    case .offline: return "Couldn't download"
    case .notEnoughSpace: return "Not enough space on this phone"
    case .gone: return "Needs an app update"
    case .rejected: return "The download was damaged"
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

  private var backupSummary: String {
    let b = model.backup
    guard let folder = b.folder else {
      return model.connection.isSample ? "Try it with a folder on this iPhone" : "Off · choose a folder on your server"
    }
    if b.total > 0 { return "\(folder.label) · \(b.line)" }
    return folder.label
  }

  private var synologyWords: String {
    if case .reachable(_, overInternet: false, _) = model.connection.synology { return "Connected on its own network" }
    if case .reachable(_, _, relayed: true) = model.connection.synology { return "Connected through your relay" }
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
    case .accessEnded:
      NoticeCard(message: FailureCopy.accessEnded(library: model.connection.libraryName, at: model.connection.accessUntil),
                 actionTitle: "Try again") { Task { await model.services.connection.retry() } }
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
