// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §11.1: the sample library says what it is wherever the source is named.
import MobileKit
@testable import MobileUI
import XCTest

final class SampleLibraryModelTests: XCTestCase {
  func testTheSampleIsNamedWhereTheSynologyWouldBe() {
    XCTAssertEqual(FailureCopy.line(for: MediaFailure.synologyUnreachable, sample: true), FailureCopy.sampleUnreachable)
    XCTAssertEqual(FailureCopy.line(for: MediaFailure.synologyUnreachable), FailureCopy.synologyUnreachable)
    XCTAssertFalse(FailureCopy.line(for: CopyFailure.synologyUnreachable, sample: true).contains("Synology"))
    XCTAssertFalse(FailureCopy.line(for: CopyFailure.catalogueRejected, sample: true).contains("computer"))
    XCTAssertTrue(FailureCopy.enterCodeHelp.contains(FailureCopy.sampleCode))
    XCTAssertEqual(FailureCopy.sampleCode, "SAMPLE")
    // A list not signed by the sample's key is "rejected", never "another computer".
    XCTAssertEqual(FailureCopy.line(for: MediaFailure.publisherChanged, sample: true), FailureCopy.sampleRejected)
    XCTAssertEqual(FailureCopy.line(for: MediaFailure.publisherChanged), FailureCopy.publisherChanged)
  }

  /// docs/plans/self-hosted.md §8: the phone says "your server" (a NAS or a computer); "Synology" and
  /// "NAS" are not words it shows, and the sample library stands in for a server that is a computer.
  func testThePhoneSaysYourServer() {
    let lines: [String] = [
      FailureCopy.synologyUnreachable, FailureCopy.catalogueStale(days: 3), FailureCopy.originalGone,
      FailureCopy.notThereYet, FailureCopy.folderNotOnNAS, FailureCopy.ownNetworkOnly, FailureCopy.serviceTooOld,
      FailureCopy.sampleAbout, FailureCopy.backupFolderNotAllowed, FailureCopy.backupNASFull,
      FailureCopy.backupServiceTooOld, FailureCopy.backupFolderGone, FailureCopy.backupNoFolders,
      FailureCopy.sampleBackupFolder, FailureCopy.line(for: CopyFailure.synologyUnreachable),
      FailureCopy.driveNotConnected("Travel drive"), SettingsCopy.directOnMobileDataDetail,
    ]
    for line in lines {
      XCTAssertFalse(line.contains("Synology"), line)
      XCTAssertFalse(line.contains("NAS"), line)
    }
    XCTAssertEqual(FailureCopy.synologyUnreachable, "Can't reach your server. You can keep searching what's on this phone.")
    XCTAssertEqual(FailureCopy.sampleBackupFolder, "Sample server on this iPhone")
    XCTAssertEqual(SettingsCopy.sampleServerKind, "A computer")
    XCTAssertEqual(FailureCopy.driveNotConnected("Travel drive"),
                   "The drive with “Travel drive” isn't connected to your server. Connect it, then try again.")
  }

  /// An offline folder joins the top level after its own folders, once; one already listed stays.
  func testOfflineFoldersJoinTheTopLevel() {
    let own = [Folder(id: "a", name: "Photos", itemCount: 3, hasSubfolders: true)]
    let travel = Folder(id: "t", name: "Travel drive", itemCount: -1, hasSubfolders: false, offline: true)
    let camera = Folder(id: "c", name: "Camera card", itemCount: -1, hasSubfolders: false, offline: true)
    XCTAssertEqual(LibraryModel.merging([], into: own), own)
    XCTAssertEqual(LibraryModel.merging([travel, camera], into: own).map(\.name), ["Photos", "Camera card", "Travel drive"])
    XCTAssertEqual(LibraryModel.merging([travel], into: own + [travel]).count, 2)
    // A catalogue's own folder for that share keeps browsing; no second chip beside it.
    let photosRoot = Folder(id: "r", name: "photos", itemCount: -1, hasSubfolders: false, offline: true)
    XCTAssertEqual(LibraryModel.merging([photosRoot], into: own), own)
  }

  @MainActor
  func testAnOfflineFolderSaysSoAndIsNotOpened() {
    let model = LibraryModel(store: OfflineStore())
    model.enter(Folder(id: "t", name: "Travel drive", itemCount: -1, hasSubfolders: false, offline: true))
    XCTAssertTrue(model.trail.isEmpty)
    XCTAssertEqual(model.failure, FailureCopy.driveNotConnected("Travel drive"))
  }

  func testAnIdentityFromAComputerIsNotTheSample() {
    XCTAssertFalse(LibraryIdentity(name: "Family", fingerprint: "7F3A 92C1").isSample)
    XCTAssertTrue(LibraryIdentity(name: "Sample library", fingerprint: "1234 5678", isSample: true).isSample)
    XCTAssertFalse(ConnectionSnapshot.noLibrary.isSample)
  }

  /// mediaviewer://sample is opened as a pair link is: never over a pairing or a first copy.
  @MainActor
  func testTheSampleLinkFollowsThePairLinkRules() {
    XCTAssertEqual(AppModel.pairLinkAction(stage: .welcome, isScanning: false, hasLibrary: false), .open)
    XCTAssertEqual(AppModel.pairLinkAction(stage: .firstCopy, isScanning: false, hasLibrary: true), .ignore)
    XCTAssertEqual(AppModel.pairLinkAction(stage: .main, isScanning: false, hasLibrary: true), .confirm)
  }
}

/// Entering SAMPLE with a library paired asks before replacing it, by the library's name.
final class SampleReplacesTests: XCTestCase {
  private func snapshot(library: Bool, sample: Bool, name: String) -> ConnectionSnapshot {
    var s = ConnectionSnapshot(hasLibrary: library, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
    s.isSample = sample
    s.libraryName = name
    return s
  }

  @MainActor
  func testAPairedLibraryIsNamedBeforeItIsReplaced() {
    XCTAssertEqual(AppModel.sampleWouldReplace(snapshot(library: true, sample: false, name: "Family")), "Family")
    XCTAssertEqual(AppModel.sampleWouldReplace(snapshot(library: true, sample: false, name: "")), "your library")
  }

  @MainActor
  func testNothingToLoseAsksNothing() {
    XCTAssertNil(AppModel.sampleWouldReplace(snapshot(library: false, sample: false, name: "")))
    XCTAssertNil(AppModel.sampleWouldReplace(snapshot(library: true, sample: true, name: "Sample library")))
  }
}

/// A store with nothing in it: the offline folder test never asks it.
private struct OfflineStore: LibraryStore {
  func list(in scope: LibraryScope) async throws -> any MediaList { ArrayMediaList([]) }
  func folders(under parent: Folder?) async throws -> [Folder] { [] }
  func people() async throws -> [Person] { [] }
  func details(for item: MediaItem) async throws -> MediaDetails { throw MediaFailure.synologyUnreachable }
  func preview(of folder: Folder, count: Int) async -> FolderPreview { .empty }
}
