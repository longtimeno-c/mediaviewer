// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the files in a folder (MobileKit/FolderChanges.swift): Select, Add, New folder, Move,
// Rename and Delete, and the files being sent. The screens are FolderEditing.swift.
import Foundation
import MobileKit
import SwiftUI
import UIKit

/// A file being sent to the server.
public struct Upload: Identifiable, Equatable, Sendable {
  public enum State: Equatable, Sendable {
    /// Being copied from the picker into the phone's temporary folder.
    case preparing
    case waiting
    /// Its folder is being moved, renamed or deleted: sent once that is settled, or dropped.
    case held
    case sending
    /// Why it stopped; Try again carries on from where the server's copy ends.
    case failed(String)
  }

  public let id: UUID
  public internal(set) var name: String
  /// nil while it is being prepared.
  public internal(set) var file: OutgoingFile?
  public let folder: Folder
  public internal(set) var done: Int64 = 0
  public internal(set) var state: State = .waiting

  init(id: UUID = UUID(), file: OutgoingFile, folder: Folder) {
    self.id = id
    self.name = file.name
    self.file = file
    self.folder = folder
  }

  init(preparing name: String, folder: Folder) {
    id = UUID()
    self.name = name
    file = nil
    self.folder = folder
    state = .preparing
  }

  public var fraction: Double? {
    guard let bytes = file?.bytes, bytes > 0 else { return nil }
    return min(1, Double(done) / Double(bytes))
  }
}

extension FolderPage {
  func entry(_ id: String) -> FolderEntry? {
    if let folder = subfolders.first(where: { $0.id == id }) { return .folder(folder) }
    if let file = others.first(where: { $0.id == id }) { return .file(file) }
    return entries.indices.lazy.map { self.entries[$0].item }.first { $0.id.raw == id }.map(FolderEntry.item)
  }

  /// What is chosen, in the page's order.
  var chosen: [FolderEntry] {
    guard !selected.isEmpty else { return [] }
    var found: [FolderEntry] = subfolders.filter { selected.contains($0.id) }.map(FolderEntry.folder)
    found += others.filter { selected.contains($0.id) }.map(FolderEntry.file)
    for i in entries.indices where selected.contains(entries[i].item.id.raw) { found.append(.item(entries[i].item)) }
    return found
  }
}

extension LibraryModel {
  static let changeMotion = Animation.snappy(duration: 0.3)

  /// Whether this phone may change files in the page's folder: asked once it has listed, and
  /// remembered, so a folder opened again (or one inside it) has its commands from the first frame.
  func askChangeable(_ page: FolderPage) {
    guard let folder = page.folder else { return }
    let changes = changes
    Task {
      let can = await changes.canChange(in: folder)
      knownChangeable[folder.id] = can
      if page.changeable != can { page.changeable = can }
      if !can, page.selecting { setSelecting(false, on: page) }
    }
  }

  // MARK: Selection

  public func setSelecting(_ on: Bool, on page: FolderPage) {
    page.selecting = on
    page.selected = []
  }

  public func toggle(_ id: String, on page: FolderPage) {
    if page.selected.contains(id) { page.selected.remove(id) } else { page.selected.insert(id) }
  }

  // MARK: Changes

  /// A name the server would not take, said before anything is sent: a slash would make folders
  /// (the server makes the ones above a path), and a dot first hides it.
  static func nameProblem(_ name: String) -> ChangeFailure? {
    let refused = CharacterSet(charactersIn: "/\\:*?\"<>|")
    if name.rangeOfCharacter(from: refused) != nil || name.hasPrefix(".") || name.hasSuffix(".") { return .badName }
    if name.utf8.count > 200 { return .nameTooLong }
    return nil
  }

  /// A new folder in the page's folder; the page shows it in place.
  @discardableResult
  public func makeFolder(named raw: String, in page: FolderPage) async -> Bool {
    let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let folder = page.folder, !name.isEmpty else { return false }
    if let problem = Self.nameProblem(name) {
      page.changeFailure = ChangeCopy.line(for: problem, name: name)
      return false
    }
    return await run(.newFolder, on: page, entries: [], touching: [folder]) { changes, _ in
      _ = try await changes.makeFolder(named: name, in: folder)
    }
  }

  @discardableResult
  public func rename(_ entry: FolderEntry, to raw: String, in page: FolderPage) async -> Bool {
    let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let folder = page.folder, !name.isEmpty, name != entry.name else { return false }
    if let problem = Self.nameProblem(name) {
      page.changeFailure = ChangeCopy.line(for: problem, name: name)
      return false
    }
    return await run(.rename(name), on: page, entries: [entry], touching: [folder], leaves: false) { changes, entry in
      try await changes.rename(entry, in: folder, to: name)
    }
  }

  /// Moves each into `destination`. A refusal of one does not stop the rest; the page says how
  /// many moved and why the others did not.
  @discardableResult
  public func move(_ moving: [FolderEntry], from page: FolderPage, to destination: Folder) async -> Bool {
    guard let folder = page.folder, !moving.isEmpty, destination.id != folder.id else { return false }
    return await run(.move(destination.name), on: page, entries: moving, touching: [folder, destination]) { changes, entry in
      try await changes.move(entry, from: folder, to: destination)
    }
  }

  /// Deletes each: they go to the trash on the server.
  @discardableResult
  public func delete(_ deleting: [FolderEntry], in page: FolderPage) async -> Bool {
    guard let folder = page.folder, !deleting.isEmpty else { return false }
    return await run(.delete, on: page, entries: deleting, touching: [folder]) { changes, entry in
      try await changes.delete(entry, in: folder)
    }
  }

  enum Verb {
    case newFolder
    case rename(String)
    case move(String)
    case delete
  }

  /// One change on `page`, of each of `entries` (or once, with none):
  /// - this phone's files still going into a folder among them are held first (the server refuses
  ///   the change while one is being written there, and one sent after would make the folder again);
  /// - what leaves the page goes at once;
  /// - each is tried; a refusal of one is noted and the rest go on, unless the server can't be
  ///   reached or will not let this phone change anything;
  /// - what did not change comes back, the held files of what moved or went are dropped and the
  ///   others sent, and every page and tile showing a touched folder lists again.
  private func run(_ verb: Verb, on page: FolderPage, entries: [FolderEntry], touching: [Folder], leaves: Bool = true,
                   _ change: (FolderChanges, FolderEntry) async throws -> Void) async -> Bool {
    page.changing = true
    page.changeFailure = nil
    defer { page.changing = false }
    let folders = entries.filter(\.isFolder)
    let held = hold(under: folders.map(\.id))
    let before = (page.subfolders, page.others)
    let leaving = leaves ? Set(entries.map(\.id)) : []
    if !leaving.isEmpty {
      // Folders and files leave the page at once; photos when the new listing comes.
      withAnimation(Self.changeMotion) {
        page.subfolders.removeAll { leaving.contains($0.id) }
        page.others.removeAll { leaving.contains($0.id) }
      }
    }
    var done: [FolderEntry] = []
    var refused: [(name: String, failure: ChangeFailure)] = []
    let work = entries.isEmpty ? [nil] : entries.map(Optional.some)
    for entry in work {
      do {
        try await change(changes, entry ?? .folder(page.folder!))
        if let entry { done.append(entry) }
      } catch {
        let failure = (error as? ChangeFailure) ?? .failed
        refused.append((entry?.name ?? Self.newName(verb) ?? "", failure))
        if [.unreachable, .unsupported, .notAllowed].contains(failure) { break }
      }
    }
    let changed = Set(done.map(\.id))
    // What did not change comes back, where it was.
    if !leaving.isEmpty, changed.count < leaving.count {
      withAnimation(Self.changeMotion) {
        page.subfolders = before.0.filter { !leaving.contains($0.id) || !changed.contains($0.id) }
        page.others = before.1.filter { !leaving.contains($0.id) || !changed.contains($0.id) }
      }
    }
    // Files held for a folder that moved or went are dropped; the others go on.
    let gone = folders.filter { changed.contains($0.id) }.map(\.id)
    let dropped = release(held, droppingUnder: gone)
    page.changeFailure = Self.report(verb, total: entries.count, done: done.count, refused: refused)
      ?? dropped.map { ChangeCopy.uploadsDropped($0.count, folder: $0.folder) }
    if refused.isEmpty {
      setSelecting(false, on: page)
    } else {
      page.selected.subtract(changed)
    }
    let changedFolders = folders.compactMap { entry -> Folder? in
      if case .folder(let folder) = entry { return folder }
      return nil
    }
    await touched(touching + changedFolders, reloading: page)
    return refused.isEmpty
  }

  private static func newName(_ verb: Verb) -> String? {
    if case .rename(let name) = verb { return name }
    return nil
  }

  /// What to say on the page: nil when everything changed.
  static func report(_ verb: Verb, total: Int, done: Int,
                     refused: [(name: String, failure: ChangeFailure)]) -> String? {
    guard let first = refused.first else { return nil }
    let why = ChangeCopy.line(for: first.failure, name: first.name.isEmpty ? nil : first.name)
    guard total > 1, done > 0 else { return why }
    return ChangeCopy.partly(verb: verb, done: done, of: total) + " " + why
  }

  public func dismissChangeFailure(on page: FolderPage) { page.changeFailure = nil }

  /// The folders a move can go to, under `folder` (nil: the top: every folder this phone may change).
  public func moveDestinations(under folder: Folder?) async -> [Folder] {
    guard let folder else { return await changes.changeableRoots() }
    return (try? await changes.folders(in: folder)) ?? []
  }

  /// `folders` changed: each, and each folder above it, is listed afresh wherever it shows (a page
  /// on the stack, or a tile's cover and counts), and `page` lists again now.
  func touched(_ folders: [Folder], reloading page: FolderPage? = nil) async {
    let ids = Set(folders.map(\.id))
    let shows = { (id: String) in ids.contains { $0 == id || $0.hasPrefix(id + "/") } }
    forgetListings { shows($0) }
    forgetPreviews { key in ids.contains { $0 == key || $0.hasPrefix(key + "/") || key.hasPrefix($0 + "/") } }
    var loads: [Task<Void, Never>] = []
    for other in [root] + stack {
      guard let id = other.folder?.id, shows(id) || other === page else { continue }
      loads.append(load(other, animated: true))
    }
    for task in loads { await task.value }
  }

  // MARK: Sending files

  /// Sends each file into the page's folder, after any already waiting. They keep going when the
  /// folder is left; the folder's page, when on screen, shows each as it arrives.
  public func add(_ files: [OutgoingFile], to page: FolderPage) {
    guard let folder = page.folder, !files.isEmpty else { return }
    uploads += files.map { Upload(file: $0, folder: folder) }
    sendNext()
  }

  /// Rows for files being copied from a picker, shown at once; `prepared` fills each in.
  public func preparing(_ names: [String], for page: FolderPage) -> [UUID] {
    guard let folder = page.folder else { return [] }
    let rows = names.map { Upload(preparing: $0, folder: folder) }
    uploads += rows
    return rows.map(\.id)
  }

  /// A picked file is ready (or could not be read from the phone).
  public func prepared(_ id: UUID, _ file: OutgoingFile?) {
    update(id) {
      if let file {
        $0.file = file
        $0.name = file.name
        $0.state = .waiting
      } else {
        $0.state = .failed(ChangeCopy.line(for: .unreadable, name: $0.name))
      }
    }
    sendNext()
  }

  /// Stops one and takes it off the list; the phone's copy of the file goes. (An unfinished copy the
  /// server kept is picked up if the same file is added there again.)
  public func cancel(_ upload: Upload) {
    guard let index = uploads.firstIndex(where: { $0.id == upload.id }) else { return }
    let sending = uploads[index].state == .sending
    let file = uploads[index].file
    uploads.remove(at: index)
    if let file { Self.discard(file) }
    if sending {
      uploading?.cancel()
      uploading = nil
      sendNext()
    }
  }

  public func retry(_ upload: Upload) {
    guard let index = uploads.firstIndex(where: { $0.id == upload.id }), uploads[index].file != nil else { return }
    uploads[index].state = .waiting
    sendNext()
  }

  /// Holds every file going into one of `folders` or a folder in it, stopping the one being sent
  /// (the server keeps what it has). Returns them.
  private func hold(under folders: [String]) -> [UUID] {
    guard !folders.isEmpty else { return [] }
    let under = { (id: String) in folders.contains { id == $0 || id.hasPrefix($0 + "/") } }
    var held: [UUID] = []
    for i in uploads.indices where under(uploads[i].folder.id) && uploads[i].state != .preparing {
      if uploads[i].state == .sending {
        uploading?.cancel()
        uploading = nil
      }
      uploads[i].state = .held
      held.append(uploads[i].id)
    }
    return held
  }

  /// Lets the held files go on, but drops those going under `gone`. What was dropped, for the page.
  private func release(_ held: [UUID], droppingUnder gone: [String]) -> (count: Int, folder: String)? {
    var dropped = 0
    var folder = ""
    for id in held {
      guard let upload = uploads.first(where: { $0.id == id }) else { continue }
      if gone.contains(where: { upload.folder.id == $0 || upload.folder.id.hasPrefix($0 + "/") }) {
        dropped += 1
        folder = upload.folder.name
        cancel(upload)
      } else {
        update(id) { $0.state = .waiting }
      }
    }
    sendNext()
    return dropped > 0 ? (dropped, folder) : nil
  }

  private func sendNext() {
    guard uploading == nil, let next = uploads.first(where: { $0.state == .waiting }), let file = next.file else { return }
    let changes = changes
    uploading = Task { [weak self] in
      guard let self else { return }
      // A send keeps going a while after the app is left, as long as the system allows.
      let background = BackgroundTime.begin("Adding files to your server")
      defer { background.end() }
      self.update(next.id) { $0.state = .sending }
      let progress = ProgressThrottle { [weak self] done in
        Task { @MainActor in self?.update(next.id) { if $0.state == .sending { $0.done = done } } }
      }
      do {
        _ = try await changes.add(file, to: next.folder) { done, _ in progress.report(done) }
        guard !Task.isCancelled else { return }
        self.uploads.removeAll { $0.id == next.id }
        Self.discard(file)
        await self.touched([next.folder])
      } catch {
        guard !Task.isCancelled else { return }
        let failure = (error as? ChangeFailure) ?? .failed
        self.update(next.id) { $0.state = .failed(ChangeCopy.line(for: failure, name: next.name)) }
      }
      // A stopped send: cancel(_:) or hold(under:) has already moved on.
      guard !Task.isCancelled else { return }
      self.uploading = nil
      self.sendNext()
    }
  }

  private func update(_ id: UUID, _ change: (inout Upload) -> Void) {
    guard let index = uploads.firstIndex(where: { $0.id == id }) else { return }
    change(&uploads[index])
  }

  /// A file picked for sending is a copy in the phone's temporary folder: it goes once sent or stopped.
  static func discard(_ file: OutgoingFile) {
    guard file.url.path.hasPrefix(FileManager.default.temporaryDirectory.path) else { return }
    let dir = file.url.deletingLastPathComponent()
    Task.detached(priority: .utility) { try? FileManager.default.removeItem(at: dir) }
  }
}

/// Time to finish a send after the app is left. Nothing outside an app (the package's tests).
@MainActor
struct BackgroundTime {
  private let id: UIBackgroundTaskIdentifier

  static func begin(_ name: String) -> BackgroundTime {
    guard Bundle.main.bundleURL.pathExtension == "app" else { return BackgroundTime(id: .invalid) }
    var id = UIBackgroundTaskIdentifier.invalid
    id = UIApplication.shared.beginBackgroundTask(withName: name) {
      UIApplication.shared.endBackgroundTask(id)
    }
    return BackgroundTime(id: id)
  }

  func end() {
    guard id != .invalid else { return }
    UIApplication.shared.endBackgroundTask(id)
  }
}

/// At most ten progress steps a second reach the screen.
final class ProgressThrottle: @unchecked Sendable {
  private let lock = NSLock()
  private var last: ContinuousClock.Instant?
  private let send: @Sendable (Int64) -> Void

  init(_ send: @escaping @Sendable (Int64) -> Void) { self.send = send }

  func report(_ done: Int64) {
    let now = ContinuousClock.now
    let go = lock.withLock { () -> Bool in
      if let last, now - last < .milliseconds(100) { return false }
      last = now
      return true
    }
    if go { send(done) }
  }
}
