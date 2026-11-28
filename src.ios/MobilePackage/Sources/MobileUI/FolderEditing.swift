// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A folder's page, changed from the phone (LibraryChanges.swift): an Add menu and Select in its bar,
// a bar of Move, Rename and Delete under what is chosen (a tile held starts Select with it chosen,
// as in Photos), and the files being sent. Only where this phone may change files (FolderPage.changeable).
import MobileKit
import PhotosUI
import SwiftUI
import UIKit
import UniformTypeIdentifiers

extension EnvironmentValues {
  /// The page the tiles are on, when this phone may change it: in Select mode a tap chooses.
  @Entry var editingPage: FolderPage? = nil
}

struct FolderEditing: ViewModifier {
  let page: FolderPage
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion
  @State private var pickingPhotos = false
  @State private var photos: [PhotosPickerItem] = []
  @State private var pickingFiles = false
  @State private var naming = false
  @State private var newName = ""
  /// What Rename acts on: kept apart from whether its alert shows, which closes before its button acts.
  @State private var renaming: FolderEntry?
  @State private var renameShown = false
  @State private var renameText = ""
  @State private var moving: MoveRequest?
  @State private var deleting: [FolderEntry] = []
  @State private var confirmingDelete = false

  func body(content: Content) -> some View {
    let editable = page.changeable
    content
      .environment(\.editingPage, editable ? page : nil)
      .navigationTitle(page.selecting ? ChangeCopy.selected(page.selected.count) : page.title)
      .navigationBarBackButtonHidden(page.selecting)
      .toolbar(page.selecting ? .hidden : .automatic, for: .tabBar)
      .toolbar {
        if editable {
          if page.selecting {
            ToolbarItem(placement: .topBarTrailing) {
              Button("Done") { model.library.setSelecting(false, on: page) }
                .fontWeight(.semibold)
                .accessibilityIdentifier("select-done")
            }
            ToolbarItemGroup(placement: .bottomBar) { selectionBar }
          } else {
            ToolbarItem(placement: .topBarTrailing) { addMenu }
            ToolbarItem(placement: .topBarTrailing) {
              Button("Select") { model.library.setSelecting(true, on: page) }
                .accessibilityIdentifier("select")
            }
          }
        }
      }
      .photosPicker(isPresented: $pickingPhotos, selection: $photos, matching: .any(of: [.images, .videos]),
                    preferredItemEncoding: .current)
      .onChange(of: photos) { _, picked in
        guard !picked.isEmpty else { return }
        photos = []
        // Each shows at once, then is sent once copied from Photos.
        let rows = model.library.preparing(picked.map { _ in "Photo" }, for: page)
        Task {
          for (row, item) in zip(rows, picked) { model.library.prepared(row, await Outgoing.file(from: item)) }
        }
      }
      .fileImporter(isPresented: $pickingFiles, allowedContentTypes: [.item], allowsMultipleSelection: true) { result in
        guard case .success(let urls) = result else { return }
        // Each shows at once by its name, then is sent once copied from Files.
        let rows = model.library.preparing(urls.map(\.lastPathComponent), for: page)
        Task {
          for (row, url) in zip(rows, urls) { model.library.prepared(row, await Outgoing.file(from: url)) }
        }
      }
      .alert(ChangeCopy.newFolder, isPresented: $naming) {
        // Return makes it, as Create does.
        TextField("Name", text: $newName)
          .onSubmit {
            naming = false
            createFolder()
          }
        Button("Create") { createFolder() }
          .disabled(newName.trimmingCharacters(in: .whitespaces).isEmpty)
          .accessibilityIdentifier("new-folder-confirm")
        Button("Cancel", role: .cancel) {}
      }
      .alert(renaming.map { "Rename “\($0.name)”" } ?? "Rename", isPresented: $renameShown) {
        TextField("Name", text: $renameText)
          .onSubmit {
            renameShown = false
            confirmRename()
          }
        Button("Rename") { confirmRename() }
        .accessibilityIdentifier("rename-confirm")
        Button("Cancel", role: .cancel) {}
      }
      .confirmationDialog(ChangeCopy.deleteTitle(deleting.count, name: deleting.first?.name), isPresented: $confirmingDelete,
                          titleVisibility: .visible) {
        Button("Delete", role: .destructive) {
          let gone = deleting
          Task { await model.library.delete(gone, in: page) }
        }
        Button("Cancel", role: .cancel) {}
      } message: {
        Text(ChangeCopy.deleteMessage(deleting.count, sample: model.connection.isSample))
      }
      .sheet(item: $moving) { request in
        MovePicker(request: request) { destination in
          Task { await model.library.move(request.entries, from: page, to: destination) }
        }
        .environment(model)
      }
  }

  private func createFolder() {
    let name = newName
    Task { await model.library.makeFolder(named: name, in: page) }
  }

  private func confirmRename() {
    guard let entry = renaming else { return }
    let name = renameText
    Task { await model.library.rename(entry, to: name, in: page) }
  }

  private func startRename(_ entry: FolderEntry) {
    renameText = entry.name
    renaming = entry
    renameShown = true
  }

  private var addMenu: some View {
    Menu {
      Button { pickingPhotos = true } label: { Label("Add from Photos", systemImage: "photo.on.rectangle") }
      Button { pickingFiles = true } label: { Label("Add from Files", systemImage: "folder") }
      Divider()
      Button {
        newName = ""
        naming = true
      } label: { Label(ChangeCopy.newFolder, systemImage: "folder.badge.plus") }
    } label: {
      Label("Add", systemImage: "plus")
    }
    .accessibilityIdentifier("add")
  }

  @ViewBuilder
  private var selectionBar: some View {
    let chosen = page.chosen
    let idle = chosen.isEmpty || page.changing
    Button { moving = MoveRequest(entries: chosen, from: page.folder) } label: { Label("Move", systemImage: "folder") }
      .disabled(idle)
    Spacer()
    Button { if let one = chosen.first { startRename(one) } } label: { Label("Rename", systemImage: "pencil") }
      .disabled(idle || chosen.count != 1)
    Spacer()
    Button(role: .destructive) {
      deleting = chosen
      confirmingDelete = true
    } label: { Label("Delete", systemImage: "trash") }
      .disabled(idle)
  }
}

/// The tile's part in Select mode: a check over it, and holding it starts Select with it chosen
/// (as Photos does; Move, Rename and Delete are then on the bar).
struct Choosable: ViewModifier {
  let entry: FolderEntry
  /// Where the check sits on the tile.
  var corner: Alignment = .topTrailing
  @Environment(\.editingPage) private var page
  @Environment(AppModel.self) private var model

  func body(content: Content) -> some View {
    let chosen = page?.selected.contains(entry.id) == true
    let holdable = page != nil && page?.selecting != true
    content
      .overlay(alignment: corner) {
        if page?.selecting == true {
          Image(systemName: chosen ? "checkmark.circle.fill" : "circle")
            .font(.title3)
            .symbolRenderingMode(.palette)
            .foregroundStyle(.white, chosen ? Theme.accent : .black.opacity(0.25))
            .padding(6)
            .accessibilityHidden(true)
            .transition(.opacity)
        }
      }
      .accessibilityAddTraits(chosen ? .isSelected : [])
      // Beside the tile's own tap, which still comes when the finger lifts (LibraryModel.tapped).
      .simultaneousGesture(LongPressGesture(minimumDuration: 0.4).onEnded { _ in hold() }, including: holdable ? .all : .subviews)
      .accessibilityActions {
        if holdable { Button("Select") { hold(lifting: false) } }
      }
  }

  private func hold(lifting: Bool = true) {
    guard let page, !page.selecting else { return }
    if lifting { UIImpactFeedbackGenerator(style: .light).impactOccurred() }
    model.library.startSelecting(holding: entry.id, on: page, lifting: lifting)
  }
}

/// On a scroll view of tiles: a hold followed by a drag never lifts on its tile, so the tap the
/// hold would pass over is not coming.
struct DropsHold: ViewModifier {
  @Environment(\.editingPage) private var page
  @Environment(AppModel.self) private var model

  func body(content: Content) -> some View {
    content.onScrollPhaseChange { _, phase in
      if phase != .idle, let page, page.held != nil { model.library.dropHold(on: page) }
    }
  }
}

extension View {
  func choosable(_ entry: FolderEntry, corner: Alignment = .topTrailing) -> some View {
    modifier(Choosable(entry: entry, corner: corner))
  }

  func dropsHold() -> some View { modifier(DropsHold()) }
}

/// A tap on a tile: in Select mode it chooses; otherwise `open`.
@MainActor
struct TileTap {
  let page: FolderPage?
  let model: AppModel

  func run(_ entry: FolderEntry, open: () -> Void) {
    if let page, page.selecting {
      model.library.tapped(entry.id, on: page)
    } else {
      open()
    }
  }
}

// MARK: Sending

/// The files being sent into this page's folder: each with its bar, and a way to stop it.
struct UploadRows: View {
  let page: FolderPage
  @Environment(AppModel.self) private var model
  @Environment(\.accessibilityReduceMotion) private var reduceMotion

  var body: some View {
    let here = model.library.uploads.filter { $0.folder.id == page.folder?.id }
    if !here.isEmpty {
      VStack(alignment: .leading, spacing: 0) {
        ForEach(Array(here.enumerated()), id: \.element.id) { index, upload in
          if index > 0 { Divider().overlay(Theme.lifted).padding(.leading, Theme.inset) }
          UploadRow(upload: upload)
        }
      }
      .background(RoundedRectangle(cornerRadius: Theme.cardRadius, style: .continuous).fill(Theme.surface))
      .padding(.horizontal, Theme.inset)
      .padding(.vertical, 8)
      .arrives(reduceMotion: reduceMotion)
    }
  }
}

private struct UploadRow: View {
  let upload: Upload
  @Environment(AppModel.self) private var model

  var body: some View {
    HStack(spacing: 12) {
      VStack(alignment: .leading, spacing: 4) {
        Text(ChangeCopy.uploading(upload.name))
          .font(.subheadline)
          .foregroundStyle(Theme.title)
          .lineLimit(1)
          .truncationMode(.middle)
        switch upload.state {
        case .preparing:
          Text(ChangeCopy.uploadPreparing).font(.caption).foregroundStyle(Theme.body)
        case .waiting, .held:
          Text(ChangeCopy.uploadWaiting).font(.caption).foregroundStyle(Theme.body)
        case .sending:
          ProgressTrack(fraction: upload.fraction).frame(height: 3).padding(.vertical, 3)
          Text(ProgressWords.amounts(upload.done, of: upload.file?.bytes ?? 0))
            .font(.caption.monospacedDigit())
            .foregroundStyle(Theme.body)
        case .failed(let why):
          Text(why).font(.caption).foregroundStyle(Theme.warm).fixedSize(horizontal: false, vertical: true)
          Button("Try again") { model.library.retry(upload) }
            .font(.caption.weight(.semibold))
            .buttonStyle(.borderless)
        }
      }
      .frame(maxWidth: .infinity, alignment: .leading)
      Button { model.library.cancel(upload) } label: {
        Image(systemName: "xmark").font(.footnote.weight(.semibold)).foregroundStyle(Theme.body)
          .frame(width: 44, height: 44)
          .contentShape(Rectangle())
      }
      .accessibilityLabel("Stop adding \(upload.name)")
    }
    .padding(.leading, Theme.inset)
    .padding(.vertical, 6)
    .accessibilityElement(children: .contain)
  }
}

/// Picked files, copied into the phone's temporary folder (off the main thread) so they can be
/// sent after the picker has gone, and sent again after a stop.
enum Outgoing {
  /// Through a file coordinator, so a file in iCloud Drive not yet on the phone is fetched first.
  /// nil for a folder (only files are sent) or a file that could not be read.
  static func file(from url: URL) async -> OutgoingFile? {
    await Task.detached(priority: .userInitiated) {
      let scoped = url.startAccessingSecurityScopedResource()
      defer { if scoped { url.stopAccessingSecurityScopedResource() } }
      guard (try? url.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) != true else { return nil }
      var made: OutgoingFile?
      var error: NSError?
      NSFileCoordinator().coordinate(readingItemAt: url, options: [.withoutChanges], error: &error) { readable in
        made = copy(readable, named: url.lastPathComponent)
      }
      return made
    }.value
  }

  static func file(from item: PhotosPickerItem) async -> OutgoingFile? {
    (try? await item.loadTransferable(type: PickedFile.self))?.file
  }

  /// A copy in a folder of its own, under its own name.
  static func copy(_ url: URL, named name: String) -> OutgoingFile? {
    let dir = FileManager.default.temporaryDirectory.appendingPathComponent("Outgoing/\(UUID().uuidString)", isDirectory: true)
    let target = dir.appendingPathComponent(name.isEmpty ? "File" : name)
    do {
      try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
      try FileManager.default.copyItem(at: url, to: target)
      let bytes = (try? FileManager.default.attributesOfItem(atPath: target.path)[.size] as? NSNumber)?.int64Value ?? 0
      return OutgoingFile(url: target, name: target.lastPathComponent, bytes: bytes)
    } catch {
      try? FileManager.default.removeItem(at: dir)
      return nil
    }
  }

  struct PickedFile: Transferable {
    let file: OutgoingFile

    static var transferRepresentation: some TransferRepresentation {
      FileRepresentation(importedContentType: .movie) { received in try picked(received) }
      FileRepresentation(importedContentType: .image) { received in try picked(received) }
    }

    private static func picked(_ received: ReceivedTransferredFile) throws -> PickedFile {
      guard let file = Outgoing.copy(received.file, named: received.file.lastPathComponent) else {
        throw CocoaError(.fileReadUnknown)
      }
      return PickedFile(file: file)
    }
  }
}

// MARK: Moving

struct MoveRequest: Identifiable {
  let id = UUID()
  let entries: [FolderEntry]
  /// Where they are now: not a place to move them to.
  let from: Folder?
}

/// Where to move: the folders this phone may change, and the folders inside them.
private struct MovePicker: View {
  let request: MoveRequest
  let chosen: (Folder) -> Void
  @Environment(\.dismiss) private var dismiss

  var body: some View {
    NavigationStack {
      MoveLevel(folder: nil, request: request, chosen: pick)
      .navigationDestination(for: Folder.self) { MoveLevel(folder: $0, request: request, chosen: pick) }
      .toolbar {
        ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
      }
    }
    .presentationDetents([.large])
  }

  private func pick(_ destination: Folder) {
    chosen(destination)
    dismiss()
  }
}

private struct MoveLevel: View {
  let folder: Folder?
  let request: MoveRequest
  let chosen: (Folder) -> Void
  @Environment(AppModel.self) private var model
  @State private var folders: [Folder]?

  var body: some View {
    let moving = Set(request.entries.filter(\.isFolder).map(\.id))
    List {
      if let folders {
        if folders.isEmpty {
          Text(folder == nil ? ChangeCopy.noChangeableFolders : "No folders inside.")
            .font(.subheadline)
            .foregroundStyle(Theme.body)
            .listRowBackground(Theme.surface)
        }
        ForEach(folders) { child in
          // A folder being moved can't go into itself.
          if moving.contains(child.id) {
            Label(child.name, systemImage: "folder").foregroundStyle(Theme.body.opacity(0.5))
              .listRowBackground(Theme.surface)
          } else {
            NavigationLink(value: child) { Label(child.name, systemImage: "folder").foregroundStyle(Theme.title) }
              .listRowBackground(Theme.surface)
              .accessibilityIdentifier("move-to:\(child.id)")
          }
        }
      } else {
        ProgressView().frame(maxWidth: .infinity).listRowBackground(Color.clear)
      }
    }
    .scrollContentBackground(.hidden)
    .background(Theme.ground)
    .navigationTitle(folder?.name ?? "Move to")
    .navigationBarTitleDisplayMode(.inline)
    .toolbar {
      if let folder {
        ToolbarItem(placement: .confirmationAction) {
          Button(ChangeCopy.moveHere) { chosen(folder) }
            .disabled(folder.id == request.from?.id)
            .accessibilityIdentifier("move-here")
        }
      }
    }
    .task { folders = await model.library.moveDestinations(under: folder) }
  }
}
