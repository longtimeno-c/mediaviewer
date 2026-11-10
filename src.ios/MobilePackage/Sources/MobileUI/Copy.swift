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
  public static let libraryFailed = "Couldn't read this library on the phone."
  public static let peopleFailed = "Couldn't read the people in this library."
  public static let searchFailed = "Search isn't working right now."
  public static let clipStopped = "This clip stopped playing."

  public static func line(for failure: CopyFailure) -> String {
    switch failure {
    case .synologyUnreachable: return "The copy stopped. Your Synology went out of reach."
    case .notEnoughSpace(let bytes): return "There isn't enough space on this phone. The library needs \(ProgressWords.size(bytes))."
    case .catalogueRejected: return "What arrived doesn't match what your computer published."
    }
  }

  public static let notMatched =
    "Nothing was paired. Scan the code again from MediaViewer on your own computer."
  public static let noMobileData = "No mobile data right now, so the route from outside wasn't tested."

  public static func cannotDecode(_ format: String) -> String {
    "This phone can't open \(format) files. Showing the thumbnail."
  }

  public static func line(for failure: MediaFailure) -> String {
    switch failure {
    case .synologyUnreachable: return synologyUnreachable
    case .originalGone: return originalGone
    case .notThereYet: return notThereYet
    case .folderNotOnNAS: return folderNotOnNAS
    case .cannotDecode(let format): return cannotDecode(format)
    case .serviceTooOld: return serviceTooOld
    case .publisherChanged: return publisherChanged
    }
  }
}
