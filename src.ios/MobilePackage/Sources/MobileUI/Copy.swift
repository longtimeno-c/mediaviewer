// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §11.3, word for word, and the short lines the same screens need.

import Foundation
import MobileKit

public enum FailureCopy {
  public static let synologyUnreachable =
    "Can't reach your Synology. You can keep searching what's on this phone."
  public static func catalogueStale(days: Int) -> String {
    "Last updated \(days) days ago. Open MediaViewer on your Mac near your Synology to refresh it."
  }
  public static let originalGone = "This file isn't on the NAS any more."
  public static let notThereYet = "This one hasn't reached your Synology yet."
  public static let folderNotOnNAS =
    "Your Synology isn't sharing the folder this came from. Add it in MediaViewer on your computer, named the same as its shared folder."
  public static let ownNetworkOnly =
    "Your Synology can't be reached from the internet. Your provider may not give that building a public address."
  public static let serviceTooOld = "The MediaViewer service on your Synology needs updating."
  public static let publisherChanged =
    "This library was published from a different computer. Scan a new code from that computer to keep using it."
  public static let unsupportedTower =
    "This library uses the largest search model, which this phone can't run yet."

  // §11.1's Connect states.
  public static let codeExpired = "This code has expired. Show a new one in MediaViewer, then scan it."
  public static let codeNotReadable =
    "That isn't a code MediaViewer can read. Scan the one shown by MediaViewer on your Mac or Windows PC."
  /// §8.2: distinct from a removal, and no new code is needed if the owner extends it.
  public static func accessEnded(library: String, at end: Date?) -> String {
    let name = library.isEmpty ? "this library" : "“\(library)”"
    guard let end else { return "Your access to \(name) has ended. Ask its owner to extend it, then try again." }
    let when = end.formatted(date: .long, time: .shortened)
    return "Your access to \(name) ended on \(when). Ask its owner to extend it, then try again."
  }
  /// The confirm screen's line for a guest code: "Guest access for 24 hours, from when you pair."
  public static func guestAccess(for seconds: TimeInterval) -> String {
    "Guest access for \(duration(seconds)), from when you pair."
  }
  /// Whole days, then whole hours, then minutes: what the computer's menu offered.
  public static func duration(_ seconds: TimeInterval) -> String {
    let s = Int(seconds.rounded())
    func plural(_ n: Int, _ unit: String) -> String { "\(n) \(unit)\(n == 1 ? "" : "s")" }
    if s >= 86_400, s % 86_400 == 0 { return plural(s / 86_400, "day") }
    if s >= 3_600, s % 3_600 == 0 { return plural(s / 3_600, "hour") }
    return plural(max(1, s / 60), "minute")
  }
  public static func accessUntil(_ end: Date) -> String {
    "Access until \(end.formatted(date: .abbreviated, time: .shortened))"
  }
  public static let deviceRevoked =
    "This phone was removed from your library on your computer. Scan a new code from MediaViewer to use it again."
  public static let libraryHandedOver =
    "Your library is now published from a different computer, which your old one approved. Nothing changes on this phone."
  public static let photosNotAllowed = "MediaViewer isn't allowed to add to Photos. You can change that in Settings."
  public static let noCamera = "This device has no camera. Choose a picture of the code, or paste its link."
  public static let cameraNotAllowed = "MediaViewer isn't allowed to use the camera."
  public static let noCodeInPicture = "There's no MediaViewer code in that picture."
  public static let pasteNotCode = "That isn't a MediaViewer link. Copy the link MediaViewer shows next to its code."
  public static let libraryFailed = "Couldn't read this library on the phone."
  public static let peopleFailed = "Couldn't read the people in this library."
  public static let searchFailed = "Search isn't working right now."
  // File mode (docs/plans/phone-in-the-app.md §4): a library with no catalogue yet.
  public static let smartSearchNeedsLocalSearch =
    "Smart search (words, faces, places and sounds) needs Local search. Install it on your Mac or PC and index this library."
  public static let fileSearchNoFolder = "Open a folder in Library to search it."
  public static let fileSearchHint = "File search looks at the names and tags of the files in this folder."
  public static let fileModeTop = "Choose a folder."
  public static let fileModeSettings = "Folders, searched by name and tag"
  public static let clipStopped = "This clip stopped playing."

  public static func line(for failure: CopyFailure, sample: Bool = false) -> String {
    switch failure {
    case .synologyUnreachable:
      return sample ? "The copy stopped. The sample library went out of reach." : "The copy stopped. Your Synology went out of reach."
    case .notEnoughSpace(let bytes): return "There isn't enough space on this phone. The library needs \(ProgressWords.size(bytes))."
    case .catalogueRejected:
      return sample ? "What arrived isn't the signed sample library." : "What arrived doesn't match what your computer published."
    case .accessEnded:
      return "Your access to this library has ended, so it isn't copied. Ask its owner to extend it."
    }
  }

  // The sample library (§11.1). Said wherever the Synology would be named.
  public static let sampleCode = "SAMPLE"
  public static let sampleUnreachable =
    "Can't reach the sample library. You can keep searching what's on this phone."
  public static let sampleAbout =
    "Free photos and clips from Wikimedia Commons, to try MediaViewer without a Mac or a NAS. They're downloaded from GitHub, and search runs on this phone."
  public static let sampleRejected =
    "The sample library didn't pass its signature check, so none of it is shown. Try again later."
  public static let enterCodeHelp =
    "Paste the link MediaViewer shows beside its code, or type \(sampleCode) to try the sample library."

  public static let notMatched =
    "Nothing was paired. Scan the code again from MediaViewer on your own computer."
  public static let noMobileData = "No mobile data right now, so the route from outside wasn't tested."

  // Phone backup (1.1, §10b, §11.1).
  public static let backupPausedOffWiFi = "Backup is paused until this phone is on Wi-Fi."
  public static let backupFolderNotAllowed =
    "Your Synology doesn't let this phone write to that folder. The owner can allow it in MediaViewer on their computer."
  public static let backupLimitReached =
    "The backup folder has reached the storage limit set for it. Raise the limit in MediaViewer on your computer, or choose another folder."
  public static let backupNASFull = "Your Synology is full, so nothing more can be backed up."
  public static let backupServiceTooOld = "The MediaViewer service on your Synology needs updating before it can take backups."
  public static let backupFolderGone = "That backup folder isn't on your Synology any more. Choose another."
  public static let backupNoFolders =
    "Your Synology has no backup folder yet. Add one in MediaViewer on your computer, under Phone access."
  public static let backupNoPhotos = "MediaViewer isn't allowed to read Photos. You can change that in Settings."
  public static let backupLimited = "Only the photos you chose for MediaViewer are backed up."
  /// Owner's call B8 (2026-11-12).
  public static let backupSkipped = "Hidden photos, and the other frames of a burst, are not backed up."
  public static let backupNotIndexed =
    "New photos appear in search once your computer has indexed the backup folder and published the library again."
  public static let backupSampleAbout =
    "With the sample library, backup copies your photos to a folder inside MediaViewer on this iPhone, so you can see how it works. Nothing leaves this phone, and Forget removes the folder."
  public static let sampleBackupFolder = "Sample NAS on this iPhone"

  public static func backupLine(for state: BackupState, sample: Bool = false) -> String? {
    switch state {
    case .pausedOffWiFi: return backupPausedOffWiFi
    case .folderNotAllowed: return backupFolderNotAllowed
    case .limitReached:
      return sample ? "The sample folder on this iPhone is full (250 MB). Forget the sample library to empty it." : backupLimitReached
    case .nasFull: return sample ? "This iPhone is nearly full, so the sample backup stopped." : backupNASFull
    case .unreachable: return sample ? sampleUnreachable : synologyUnreachable
    case .serviceTooOld: return backupServiceTooOld
    case .folderGone: return backupFolderGone
    case .noPhotosAccess: return backupNoPhotos
    case .off, .checking, .running, .done: return nil
    }
  }

  public static func cannotDecode(_ format: String) -> String {
    "This phone can't open \(format) files. Showing the thumbnail."
  }

  public static func line(for failure: MediaFailure, sample: Bool = false) -> String {
    switch failure {
    case .synologyUnreachable: return sample ? sampleUnreachable : synologyUnreachable
    case .originalGone: return originalGone
    case .notThereYet: return notThereYet
    case .folderNotOnNAS: return folderNotOnNAS
    case .cannotDecode(let format): return cannotDecode(format)
    case .serviceTooOld: return serviceTooOld
    case .publisherChanged: return sample ? sampleRejected : publisherChanged
    case .photosNotAllowed: return photosNotAllowed
    }
  }
}
