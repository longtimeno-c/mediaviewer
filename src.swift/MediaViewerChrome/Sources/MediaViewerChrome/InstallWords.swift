// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// #525 (docs/design/18 "Installing"): the words of every add-on install, the
// same on both platforms. The Windows copy is src.managed/Shared/InstallWords.cs,
// and Chrome.Tests checks that every string there is here too, and that the
// failure kinds match. Pure, no UI.
import Foundation

/// Why an add-on install did not finish (InstallFailure in InstallWords.cs).
enum InstallFailure: Equatable, Sendable {
  /// The download did not arrive: no connection, the server went away.
  case offline
  /// The bytes that arrived are not the signed ones (size, SHA-256, manifest).
  case damaged
  /// The store refused the unpacked files (a signature or a hash).
  case notVerified
  /// The disk is full.
  case noSpace
  /// Installed, but it did not load or attach.
  case notStarted
  /// Anything else.
  case failed
}

/// The one next step a failure offers.
enum InstallNext: Equatable, Sendable {
  case retry, reinstall
}

enum InstallWords {
  // The steps, under the bar.
  static let connecting = "Connecting…"
  static let verifying = "Checking the download…"
  static let verifyingFile = "Checking the file…"
  static let installing = "Installing…"
  static let starting = "Starting…"

  /// "412 MB of 1.08 GB".
  static func downloaded(_ done: String, _ total: String) -> String { "\(done) of \(total)" }

  // What the channel says before anything is installed.
  static let unreachable = "Could not reach the download server."

  static func checkingFor(_ name: String) -> String { "Checking for \(name)…" }

  static func notPublished(_ name: String) -> String {
    "\(name) is not published for download yet. It will be offered here once a release carries it."
  }

  static func needsNewerApp(_ name: String) -> String {
    "The published \(name) needs a newer MediaViewer. Update MediaViewer, then install it."
  }

  // The buttons.
  static func install(_ name: String, _ size: String) -> String { "Install \(name), \(size)" }

  static func updateTo(_ version: String, _ size: String) -> String { "Update to \(version), \(size)" }

  static let retry = "Retry"
  static let reinstall = "Remove and install again"
  static let reinstallTip = "Removes the installed copy, keeps its data and settings, and downloads it again."

  // The outcome line.
  static func downloading(_ name: String, _ version: String?) -> String {
    version.map { "Downloading \(name) \($0)…" } ?? "Downloading \(name)…"
  }

  static func installed(_ name: String) -> String { "\(name) installed." }

  static func updated(_ name: String, _ version: String) -> String { "\(name) updated to \(version)." }

  static func reinstalled(_ name: String) -> String { "\(name) installed again." }

  static func failure(_ failure: InstallFailure, _ name: String) -> String {
    switch failure {
    case .offline: return "\(name) could not be downloaded. Check the connection, then retry."
    case .damaged: return "The download of \(name) was damaged, so nothing was installed."
    case .notVerified: return "\(name) did not pass verification, so nothing was installed."
    case .noSpace: return "There is not enough free space to install \(name)."
    case .notStarted: return "\(name) is installed but could not start."
    case .failed: return "\(name) could not be installed."
    }
  }

  /// Retry, unless the copy on disk is itself unusable: then Remove and install
  /// again. A working copy whose update failed is never removed.
  static func next(_ failure: InstallFailure, broken: Bool) -> InstallNext {
    broken || failure == .notStarted ? .reinstall : .retry
  }

  static func nextLabel(_ next: InstallNext) -> String { next == .reinstall ? reinstall : retry }

  // Updates available (the bar's notice; checked in the background only while
  // "Check for updates automatically" is on, docs/design/13).
  static func updatesBadge(_ count: Int) -> String {
    count == 1 ? "Add-on update available" : "\(count) add-on updates available"
  }

  /// "Places 1.1.0 is available. Open Settings → Add-ons to update it."
  static func updatesTip(_ updates: [AddonUpdated]) -> String {
    switch updates.count {
    case 0: return ""
    case 1: return "\(updates[0].name) \(updates[0].version) is available. Open Settings → Add-ons to update it."
    default:
      let names = RestartWords.join(updates.map { "\($0.name) \($0.version)" })
      return "\(names) are available. Open Settings → Add-ons to update them."
    }
  }
}
