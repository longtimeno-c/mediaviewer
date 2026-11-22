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
    case waiting
    case sending
    /// Why it stopped; Try again carries on from where the server's copy ends.
    case failed(String)
  }

  public let id: UUID
  public let file: OutgoingFile
  public let folder: Folder
  public internal(set) var done: Int64 = 0
  public internal(set) var state: State = .waiting

  public var fraction: Double? { file.bytes > 0 ? min(1, Double(done) / Double(file.bytes)) : nil }
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

  /// Whether this phone may change files in the page's folder: asked once it has listed.
  func askChangeable(_ page: FolderPage) {
    guard let folder = page.folder else { return }
    let changes = changes
    Task {
      let can = await changes.canChange(in: folder)
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

  /// A new folder in the page's folder; the page shows it in place.
  /// A name the server would not take, said before anything is sent: a slash would make folders
  /// (the server makes the ones above a path), and a dot first hides it.
  static func nameProblem(_ name: String) -> ChangeFailure? {
    let refused = CharacterSet(charactersIn: "/\\:*?\"<>|")
    if name.rangeOfCharacter(from: refused) != nil || name.hasPrefix(".") || name.hasSuffix(".") { return .badName }
    if name.utf8.count > 200 { return .nameTooLong }
    return nil
  }

  @discardableResult
  public func makeFolder(named raw: String, in page: FolderPage) async -> Bool {
    let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
    guard let folder = page.folder, !name.isEmpty else { return false }
    if let problem = Self.nameProblem(name) {
      page.changeFailure = ChangeCopy.line(for: problem, name: name)
      return false
    }
    return await change(on: page, naming: name) {
      _ = try await $0.makeFolder(named: name, in: folder)
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
    let worked = await change(on: page, naming: name) { try await $0.rename(entry, in: folder, to: name) }
    if worked, entry.isFolder { dropUploads(into: entry, on: page) }
    return worked
  }

  /// Moves each into `destination`, one at a time; stops at the first refusal, which says which.
  @discardableResult
  public func move(_ moving: [FolderEntry], from page: FolderPage, to destination: Folder) async -> Bool {
    guard let folder = page.folder, !moving.isEmpty, destination.id != folder.id else { return false }
    let ok = await change(on: page, leaving: moving) { changes in
      for entry in moving {
        do {
          try await changes.move(entry, from: folder, to: destination)
        } catch let failure as ChangeFailure {
          throw Named(failure: failure, name: entry.name)
        }
      }
    }
    forgetListing(of: destination)
    if ok { moving.filter(\.isFolder).forEach { dropUploads(into: $0, on: page) } }
    return ok
  }

  /// Deletes each: they go to the trash on the server.
  @discardableResult
  public func delete(_ deleting: [FolderEntry], in page: FolderPage) async -> Bool {
    guard let folder = page.folder, !deleting.isEmpty else { return false }
    let worked = await change(on: page, leaving: deleting) { changes in
      for entry in deleting {
        do {
          try await changes.delete(entry, in: folder)
        } catch let failure as ChangeFailure {
          throw Named(failure: failure, name: entry.name)
        }
      }
    }
    if worked { deleting.filter(\.isFolder).forEach { dropUploads(into: $0, on: page) } }
    return worked
  }

  private struct Named: Error {
    let failure: ChangeFailure
    let name: String
  }

  /// Runs one change on `page`: what it moves away goes at once, the page lists again (in place,
  /// animated), and a refusal is said on the page. Selection ends when it worked.
  private func change(on page: FolderPage, naming name: String? = nil, leaving: [FolderEntry] = [],
                      _ body: (FolderChanges) async throws -> Void) async -> Bool {
    page.changing = true
    page.changeFailure = nil
    defer { page.changing = false }
    let gone = Set(leaving.map(\.id))
    let before = (page.subfolders, page.others)
    if !gone.isEmpty {
      // Folders and files leave the page at once; photos when the new listing comes.
      withAnimation(Self.changeMotion) {
        page.subfolders.removeAll { gone.contains($0.id) }
        page.others.removeAll { gone.contains($0.id) }
      }
    }
    var worked = true
    do {
      try await body(changes)
    } catch let named as Named {
      page.changeFailure = ChangeCopy.line(for: named.failure, name: named.name)
      worked = false
    } catch let failure as ChangeFailure {
      page.changeFailure = ChangeCopy.line(for: failure, name: name)
      worked = false
    } catch {
      page.changeFailure = ChangeCopy.line(for: .failed)
      worked = false
    }
    if worked {
      setSelecting(false, on: page)
    } else if !gone.isEmpty {
      // Refused (or the link dropped): what left the page comes back, even if the listing after
      // cannot be had.
      withAnimation(Self.changeMotion) {
        page.subfolders = before.0
        page.others = before.1
      }
    }
    if let folder = page.folder { forgetListing(of: folder) }
    await load(page, animated: true).value
    return worked
  }

  public func dismissChangeFailure(on page: FolderPage) { page.changeFailure = nil }

  /// The folders a move can go to, under `folder` (nil: the top: every folder this phone may change).
  public func moveDestinations(under folder: Folder?) async -> [Folder] {
    guard let folder else { return await changes.changeableRoots() }
    return (try? await changes.folders(in: folder)) ?? []
  }

  // MARK: Sending files

  /// Sends each file into the page's folder, after any already waiting. They keep going when the
  /// folder is left; the folder's page, when on screen, shows each as it arrives.
  public func add(_ files: [OutgoingFile], to page: FolderPage) {
    guard let folder = page.folder, !files.isEmpty else { return }
    uploads += files.map { Upload(id: UUID(), file: $0, folder: folder) }
    sendNext()
  }

  /// Stops one and takes it off the list; the phone's copy of the file goes. (An unfinished copy the
  /// server kept is picked up if the same file is added there again.)
  public func cancel(_ upload: Upload) {
    guard let index = uploads.firstIndex(where: { $0.id == upload.id }) else { return }
    let sending = uploads[index].state == .sending
    uploads.remove(at: index)
    Self.discard(upload.file)
    if sending {
      uploading?.cancel()
      uploading = nil
      sendNext()
    }
  }

  public func retry(_ upload: Upload) {
    guard let index = uploads.firstIndex(where: { $0.id == upload.id }) else { return }
    uploads[index].state = .waiting
    sendNext()
  }

  private func sendNext() {
    guard uploading == nil, let next = uploads.first(where: { $0.state == .waiting }) else { return }
    let changes = changes
    uploading = Task { [weak self] in
      guard let self else { return }
      self.update(next.id) { $0.state = .sending }
      let progress = ProgressThrottle { [weak self] done in
        Task { @MainActor in self?.update(next.id) { if $0.state == .sending { $0.done = done } } }
      }
      do {
        _ = try await changes.add(next.file, to: next.folder) { done, _ in progress.report(done) }
        guard !Task.isCancelled else { return }
        self.uploads.removeAll { $0.id == next.id }
        Self.discard(next.file)
        self.arrived(in: next.folder)
      } catch {
        guard !Task.isCancelled else { return }
        let failure = (error as? ChangeFailure) ?? .failed
        self.update(next.id) { $0.state = .failed(ChangeCopy.line(for: failure, name: next.file.name)) }
      }
      // A stopped send: cancel(_:) took it off the list and has already moved on to the next.
      guard !Task.isCancelled else { return }
      self.uploading = nil
      self.sendNext()
    }
  }

  private func update(_ id: UUID, _ change: (inout Upload) -> Void) {
    guard let index = uploads.firstIndex(where: { $0.id == id }) else { return }
    change(&uploads[index])
  }

  /// A file arrived in `folder`: its page, if open, shows it; a page opened later lists it afresh.
  private func arrived(in folder: Folder) {
    forgetListing(of: folder)
    if let page = ([root] + stack).last(where: { $0.folder?.id == folder.id }) { load(page, animated: true) }
  }

  /// A file picked for sending is a copy in the phone's temporary folder: it goes once sent or stopped.
  static func discard(_ file: OutgoingFile) {
    guard file.url.path.hasPrefix(FileManager.default.temporaryDirectory.path) else { return }
    let dir = file.url.deletingLastPathComponent()
    Task.detached(priority: .utility) { try? FileManager.default.removeItem(at: dir) }
  }

  /// A folder moved, renamed or deleted: files still waiting to go into it (or a folder in it) are
  /// not sent, since sending one would make the folder again where it was. Said on the page.
  private func dropUploads(into entry: FolderEntry, on page: FolderPage) {
    let id = entry.id
    let dropped = uploads.filter { $0.folder.id == id || $0.folder.id.hasPrefix(id + "/") }
    guard !dropped.isEmpty else { return }
    dropped.forEach(cancel)
    page.changeFailure = ChangeCopy.uploadsDropped(dropped.count, folder: entry.name)
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
