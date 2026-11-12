// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md §11.3, word for word, and the short lines the same screens need.
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
  public static let clipStopped = "This clip stopped playing."

  public static func line(for failure: CopyFailure, sample: Bool = false) -> String {
    switch failure {
    case .synologyUnreachable:
      return sample ? "The copy stopped. The sample library went out of reach." : "The copy stopped. Your Synology went out of reach."
    case .notEnoughSpace(let bytes): return "There isn't enough space on this phone. The library needs \(ProgressWords.size(bytes))."
    case .catalogueRejected:
      return sample ? "What arrived isn't the signed sample library." : "What arrived doesn't match what your computer published."
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
