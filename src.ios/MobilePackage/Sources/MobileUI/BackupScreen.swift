// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// §11.1 Backup (1.1): the folder, how files are filed, how much is done, what is left. The screen
// says one thing (§10b): "1,204 of 18,320 backed up · 17,116 to go", then the one progress line.
// States: running; paused off Wi-Fi; folder not allowed by the service; storage limit reached;
// NAS full. With the sample library, the folder is "Sample server on this iPhone" and says so.
import MobileKit
import SwiftUI

struct BackupScreen: View {
  @Environment(AppModel.self) private var model
  @State private var folders: [BackupFolder]?
  @State private var foldersFailure: BackupState?
  /// Which question is the newest: only its answer counts (an older one may come back later).
  @State private var asking = 0
  @State private var choosing = false

  private var backup: BackupSnapshot { model.backup }
  private var service: BackupService { model.services.backup }

  var body: some View {
    ScrollView {
      VStack(alignment: .leading, spacing: 12) {
        if backup.isSample {
          NoticeCard(message: FailureCopy.backupSampleAbout)
        }
        if let words = FailureCopy.backupLine(for: backup.state, sample: backup.isSample) {
          stateCard(words)
        }
        progressCard
        settingsCard
        if !backup.failures.isEmpty { failuresCard }
        if backup.state != .off && backup.toGo > 0 {
          NavigationLink {
            NotBackedUpScreen()
          } label: {
            row("Not backed up", value: backup.toGo.formatted())
          }
          .buttonStyle(TileButtonStyle())
          .padding(.horizontal, Theme.inset)
          .accessibilityIdentifier("backup.notBackedUp")
        }
        if !backup.isSample && backup.folder != nil {
          Text(FailureCopy.backupNotIndexed)
            .font(.footnote)
            .foregroundStyle(Theme.body)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, Theme.inset)
        }
      }
      .frame(maxWidth: Theme.readableWidth)
      .padding(Theme.inset)
      .frame(maxWidth: .infinity)
    }
    .background(Theme.ground)
    .navigationTitle("Backup")
    .navigationBarTitleDisplayMode(.inline)
    .task { await loadFolders(asking) }
    .sheet(isPresented: $choosing) {
      FolderChoice(folders: folders, failure: foldersFailure, sample: backup.isSample, current: backup.folder, layout: backup.layout,
                   askAgain: askAgain) { folder, layout in
        Task { await service.choose(folder: folder, layout: layout) }
      }
      .environment(model)
      .presentationDetents([.medium, .large])
    }
  }

  /// Asks the server again, shown as asking until the answer is in: the owner may have added the
  /// folder or let this phone back up since the last answer.
  private func askAgain() {
    asking += 1
    folders = nil
    foldersFailure = nil
    let question = asking
    Task { await loadFolders(question) }
  }

  private func chooseFolder() {
    askAgain()
    choosing = true
  }

  private func loadFolders(_ question: Int) async {
    let answer: Result<[BackupFolder], BackupState>
    do {
      answer = .success(try await service.folders())
    } catch let state as BackupState {
      answer = .failure(state)
    } catch {
      answer = .failure(.unreachable)
    }
    guard question == asking else { return }
    switch answer {
    case .success(let list):
      folders = list
      foldersFailure = nil
    case .failure(let state):
      folders = nil
      foldersFailure = state
    }
  }

  // MARK: The parts

  @ViewBuilder
  private var progressCard: some View {
    InfoCard {
      VStack(alignment: .leading, spacing: 6) {
        if backup.folder == nil {
          Text(backup.isSample ? "Choose the sample folder to try backup." : "Choose the folder on your server to back this phone up to.")
            .font(.subheadline).foregroundStyle(Theme.title)
            .fixedSize(horizontal: false, vertical: true)
        } else if backup.state == .checking && backup.total == 0 {
          Text("Checking what's already backed up…").font(.subheadline).foregroundStyle(Theme.title)
          ProgressTrack(fraction: nil)
        } else {
          Text(backup.line)
            .font(.subheadline.monospacedDigit())
            .foregroundStyle(Theme.title)
            .accessibilityIdentifier("backup.line")
          ProgressTrack(fraction: backup.total > 0 ? (Double(backup.backedUp) + (backup.state == .running ? backup.currentFraction : 0)) / Double(backup.total) : 0,
                        faded: backup.state != .running)
          if backup.state == .running, let name = backup.currentName {
            Text("Copying \(name)").font(.footnote).foregroundStyle(Theme.body).lineLimit(1)
          } else if backup.state == .done && backup.failures.isEmpty && backup.toGo == 0 {
            Text("Everything is backed up.").font(.footnote).foregroundStyle(Theme.body)
          }
        }
        if backup.limitedAccess {
          Text(FailureCopy.backupLimited).font(.footnote).foregroundStyle(Theme.body)
        }
        if backup.folder != nil {
          Text(FailureCopy.backupSkipped).font(.footnote).foregroundStyle(Theme.body)
            .fixedSize(horizontal: false, vertical: true)
        }
      }
      if backup.folder != nil { actionButton }
    }
  }

  @ViewBuilder
  private var actionButton: some View {
    switch backup.state {
    case .running, .checking:
      Button("Stop") { service.stop() }
        .buttonStyle(SecondaryButtonStyle())
        .accessibilityIdentifier("backup.stop")
    case .pausedOffWiFi:
      Button("Back up now over mobile data") { service.start(overMobileData: true) }
        .buttonStyle(SecondaryButtonStyle())
        .accessibilityIdentifier("backup.mobileData")
    case .noPhotosAccess:
      Button("Open Settings") {
        if let url = URL(string: UIApplication.openSettingsURLString) { UIApplication.shared.open(url) }
      }
      .buttonStyle(SecondaryButtonStyle())
    case .folderNotAllowed, .folderGone:
      Button("Choose another folder") { chooseFolder() }
        .buttonStyle(SecondaryButtonStyle())
    default:
      if backup.toGo > 0 || backup.total == 0 {
        Button("Back up now") { service.start(overMobileData: false) }
          .buttonStyle(PrimaryButtonStyle())
          .accessibilityIdentifier("backup.start")
      }
    }
  }

  private var settingsCard: some View {
    InfoCard {
      Button {
        chooseFolder()
      } label: {
        row("Folder", value: backup.folder?.label ?? "Choose")
      }
      .buttonStyle(TileButtonStyle())
      .accessibilityIdentifier("backup.folder")
      if backup.folder != nil {
        row("Folders", value: backup.layout.title)
        if let space = backup.space {
          row("Space left", value: ProgressWords.size(space.roomBytes))
        }
      }
      Toggle(isOn: Binding(get: { backup.automatic }, set: { service.setAutomatic($0) })) {
        VStack(alignment: .leading, spacing: 2) {
          Text("Back up when the app is open").font(.subheadline).foregroundStyle(Theme.title)
          Text("On Wi-Fi only, newest photos first").font(.footnote).foregroundStyle(Theme.body)
        }
      }
      .tint(Theme.accent)
      .accessibilityIdentifier("backup.automatic")
      Toggle(isOn: Binding(get: { backup.allowsSleep }, set: { service.setAllowsSleep($0) })) {
        Text("Let the screen sleep during a backup").font(.subheadline).foregroundStyle(Theme.title)
      }
      .tint(Theme.accent)
    }
  }

  private var failuresCard: some View {
    InfoCard {
      Text("Couldn't back up").font(.subheadline.weight(.semibold)).foregroundStyle(Theme.title)
        .frame(maxWidth: .infinity, alignment: .leading)
      ForEach(backup.failures.prefix(20)) { f in
        InfoRow(label: f.name, value: f.reason)
      }
    }
  }

  private func stateCard(_ words: String) -> some View {
    Text(words)
      .font(.subheadline)
      .foregroundStyle(Theme.title)
      .fixedSize(horizontal: false, vertical: true)
      .padding(Theme.inset)
      .frame(maxWidth: .infinity, alignment: .leading)
      .background(RoundedRectangle(cornerRadius: Theme.cardRadius).fill(Theme.surface))
      .accessibilityIdentifier("backup.state")
  }

  private func row(_ label: String, value: String) -> some View {
    HStack {
      Text(label).font(.subheadline).foregroundStyle(Theme.title)
      Spacer()
      Text(value).font(.subheadline).foregroundStyle(Theme.body).lineLimit(1)
      Image(systemName: "chevron.right").font(.footnote.weight(.semibold)).foregroundStyle(Theme.body)
    }
    .frame(minHeight: 44)
    .contentShape(Rectangle())
  }
}

/// The folder, from the service's backup folders, and how files are filed in it.
private struct FolderChoice: View {
  /// nil while the service is still being asked.
  let folders: [BackupFolder]?
  let failure: BackupState?
  let sample: Bool
  let current: BackupFolder?
  let layout: BackupLayout
  let askAgain: () -> Void
  let chosen: (BackupFolder, BackupLayout) -> Void
  @Environment(\.dismiss) private var dismiss
  @State private var folder: BackupFolder?
  @State private var pickedLayout: BackupLayout = .yearMonth

  var body: some View {
    NavigationStack {
      ScrollView {
        VStack(alignment: .leading, spacing: 12) {
          if let words = FailureCopy.backupChoiceLine(folders: folders, failure: failure, sample: sample) {
            Text(words).font(.subheadline).foregroundStyle(Theme.title).fixedSize(horizontal: false, vertical: true)
              .accessibilityIdentifier("backup.choose.line")
          } else if folders == nil {
            HStack(spacing: 8) {
              ProgressView()
              Text(FailureCopy.backupAskingForFolders).font(.subheadline).foregroundStyle(Theme.body)
            }
            .frame(minHeight: 44)
            .accessibilityIdentifier("backup.choose.asking")
          }
          if folders != nil || failure != nil {
            Button("Ask again", action: askAgain)
              .font(.subheadline)
              .foregroundStyle(Theme.accent)
              .frame(minHeight: 44)
              .accessibilityIdentifier("backup.choose.again")
          }
          ForEach(list) { f in
            Button {
              folder = f
            } label: {
              HStack {
                Image(systemName: folder?.id == f.id ? "checkmark.circle.fill" : "circle")
                  .foregroundStyle(folder?.id == f.id ? Theme.accent : Theme.body)
                VStack(alignment: .leading, spacing: 2) {
                  Text(f.label).font(.subheadline).foregroundStyle(Theme.title)
                  if !f.writable {
                    Text("Not allowed for this phone").font(.footnote).foregroundStyle(Theme.body)
                  }
                }
                Spacer()
              }
              .frame(minHeight: 44)
              .contentShape(Rectangle())
            }
            .buttonStyle(TileButtonStyle())
            .disabled(!f.writable)
            .accessibilityIdentifier("backup.folder.\(f.label)")
          }
          if !list.isEmpty {
            Text("Folders").font(.subheadline.weight(.semibold)).foregroundStyle(Theme.title).padding(.top, 8)
            ForEach(BackupLayout.allCases, id: \.self) { l in
              Button {
                pickedLayout = l
              } label: {
                HStack {
                  Image(systemName: pickedLayout == l ? "checkmark.circle.fill" : "circle")
                    .foregroundStyle(pickedLayout == l ? Theme.accent : Theme.body)
                  VStack(alignment: .leading, spacing: 2) {
                    Text(l.title + (l == .yearMonth ? " (recommended)" : "")).font(.subheadline).foregroundStyle(Theme.title)
                    Text(l.example).font(.footnote.monospaced()).foregroundStyle(Theme.body)
                  }
                  Spacer()
                }
                .frame(minHeight: 44)
                .contentShape(Rectangle())
              }
              .buttonStyle(TileButtonStyle())
            }
            Text("Changing this moves nothing that is already backed up.")
              .font(.footnote).foregroundStyle(Theme.body)
          }
        }
        .padding(Theme.inset)
      }
      .background(Theme.ground)
      .navigationTitle("Back up to")
      .navigationBarTitleDisplayMode(.inline)
      .toolbar {
        ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
        ToolbarItem(placement: .confirmationAction) {
          Button("Done") {
            if let folder { chosen(folder, pickedLayout) }
            dismiss()
          }
          .disabled(folder == nil || folders == nil || failure != nil)
          .accessibilityIdentifier("backup.choose.done")
        }
      }
      .onChange(of: folders) { _, _ in
        folder = BackupFolderPick.kept(folder, in: folders, current: current)
      }
      .onAppear {
        folder = BackupFolderPick.kept(nil, in: folders, current: current)
        pickedLayout = layout
      }
    }
  }

  private var list: [BackupFolder] { folders ?? [] }
}

/// The chooser's selection after each answer: kept only while the server still offers it to this
/// phone; otherwise the folder in use, or the only one this phone may write. While asking (no list)
/// a pick is held as it is, to be checked against the answer; Done stays off until then.
enum BackupFolderPick {
  static func kept(_ selected: BackupFolder?, in folders: [BackupFolder]?, current: BackupFolder?) -> BackupFolder? {
    guard let folders else { return selected }
    let usable = folders.filter(\.writable)
    if let selected, let still = usable.first(where: { $0.id == selected.id }) { return still }
    if let current, let still = usable.first(where: { $0.id == current.id }) { return still }
    return usable.count == 1 && folders.count == 1 ? usable[0] : nil
  }
}

/// §10b: "A Not backed up view shows exactly which photos remain."
private struct NotBackedUpScreen: View {
  @Environment(AppModel.self) private var model
  @State private var ids: [String] = []
  private let columns = [GridItem(.adaptive(minimum: 88), spacing: 2)]

  var body: some View {
    ScrollView {
      LazyVGrid(columns: columns, spacing: 2) {
        ForEach(ids, id: \.self) { id in
          PhotoTile(id: id)
        }
      }
    }
    .background(Theme.ground)
    .navigationTitle("Not backed up")
    .navigationBarTitleDisplayMode(.inline)
    .task { ids = await model.services.backup.notBackedUp() }
  }
}

private struct PhotoTile: View {
  @Environment(AppModel.self) private var model
  let id: String
  @State private var image: UIImage?

  var body: some View {
    Color.clear
      .aspectRatio(1, contentMode: .fit)
      .overlay {
        if let image { Image(uiImage: image).resizable().scaledToFill() } else { Theme.surface }
      }
      .clipped()
      .task(id: id) { image = await model.services.backup.thumbnail(ofPhoto: id, side: 176) }
  }
}
