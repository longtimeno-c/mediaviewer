// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// Phone backup, release 1.1 (docs/plans/mobile.md §10b, §11.1): the phone's photos and videos to
// a backup folder on the NAS while the app is open. The screens see only this seam; the real
// service is MobileLink's `LinkBackup` (PhotoKit and src/mobile/backup.*), the stand-in is
// MobileFixtures'. With the sample library the "NAS" is a folder on this iPhone.
import UIKit

/// How files are filed under the folder, by the photo's own capture date. The raw values are the
/// core's (src/mobile/backup.h) and the desktop Photos backup's.
public enum BackupLayout: Int, CaseIterable, Hashable, Sendable {
  case yearMonth = 5
  case year = 1
  case yearMonthDay = 2
  case oneFolder = 4

  public var title: String {
    switch self {
    case .yearMonth: "Year and month"
    case .year: "Year"
    case .yearMonthDay: "Year, month and day"
    case .oneFolder: "One folder"
    }
  }

  /// Where IMG_0412.HEIC from 21 September 2026 lands.
  public var example: String {
    switch self {
    case .yearMonth: "2026/09/IMG_0412.HEIC"
    case .year: "2026/IMG_0412.HEIC"
    case .yearMonthDay: "2026/09/21/IMG_0412.HEIC"
    case .oneFolder: "IMG_0412.HEIC"
    }
  }
}

/// A backup folder the NAS offers. `writable` false: the service does not let this phone write there.
public struct BackupFolder: Hashable, Sendable, Identifiable {
  public let id: String
  public let label: String
  public let writable: Bool

  public init(id: String, label: String, writable: Bool) {
    self.id = id
    self.label = label
    self.writable = writable
  }
}

/// §11.1's states for the Backup screen.
public enum BackupState: Hashable, Sendable {
  /// No folder chosen yet, or backup switched off.
  case off
  /// Photos access was refused: Settings is the way back.
  case noPhotosAccess
  case checking
  case running
  case done
  case pausedOffWiFi
  /// The service does not let this phone write to the chosen folder.
  case folderNotAllowed
  case limitReached
  case nasFull
  case unreachable
  case serviceTooOld
  /// The chosen folder is gone from the service's list.
  case folderGone
}

public struct BackupFailureRow: Hashable, Sendable, Identifiable {
  public let id: String
  public let name: String
  public let reason: String

  public init(id: String, name: String, reason: String) {
    self.id = id
    self.name = name
    self.reason = reason
  }
}

public struct BackupSpace: Hashable, Sendable {
  public let limitBytes: Int64
  public let usedBytes: Int64
  public let roomBytes: Int64

  public init(limitBytes: Int64, usedBytes: Int64, roomBytes: Int64) {
    self.limitBytes = limitBytes
    self.usedBytes = usedBytes
    self.roomBytes = roomBytes
  }
}

public struct BackupSnapshot: Hashable, Sendable {
  public var state: BackupState
  public var backedUp: Int
  public var total: Int
  /// The file being copied, and how far, while running.
  public var currentName: String?
  public var currentFraction: Double
  public var folder: BackupFolder?
  public var layout: BackupLayout
  /// Starts on its own whenever the app is open on Wi-Fi (§10b). On by default.
  public var automatic: Bool
  /// The screen may sleep during a backup (off by default: it stays awake).
  public var allowsSleep: Bool
  /// Only the photos the person chose are covered (limited Photos access).
  public var limitedAccess: Bool
  /// The sample library's "Sample NAS on this iPhone".
  public var isSample: Bool
  public var failures: [BackupFailureRow]
  public var space: BackupSpace?

  public init(state: BackupState = .off, backedUp: Int = 0, total: Int = 0, currentName: String? = nil,
              currentFraction: Double = 0, folder: BackupFolder? = nil, layout: BackupLayout = .yearMonth,
              automatic: Bool = true, allowsSleep: Bool = false, limitedAccess: Bool = false, isSample: Bool = false,
              failures: [BackupFailureRow] = [], space: BackupSpace? = nil) {
    self.state = state
    self.backedUp = backedUp
    self.total = total
    self.currentName = currentName
    self.currentFraction = currentFraction
    self.folder = folder
    self.layout = layout
    self.automatic = automatic
    self.allowsSleep = allowsSleep
    self.limitedAccess = limitedAccess
    self.isSample = isSample
    self.failures = failures
    self.space = space
  }

  public var toGo: Int { max(0, total - backedUp) }

  /// §10b: "1,204 of 18,320 backed up · 17,116 to go".
  public var line: String {
    "\(backedUp.formatted()) of \(total.formatted()) backed up · \(toGo.formatted()) to go"
  }

  /// The strip above the tab bar, while a backup runs.
  public var activity: TransferActivity? {
    guard state == .running || state == .checking, total > 0 else { return nil }
    let partial = total > 0 ? (Double(backedUp) + currentFraction) / Double(total) : nil
    return TransferActivity(line: line, shortLine: "\(backedUp.formatted()) of \(total.formatted())",
                            fraction: state == .checking ? nil : partial)
  }
}

/// Long work that carries on while the person uses the app, such as a phone backup. It shows above
/// the tab bar only while it runs.
public protocol TransferMonitor: Sendable {
  /// What is running now (nil when nothing is), then every change.
  func activity() -> AsyncStream<TransferActivity?>
}

public protocol BackupService: TransferMonitor {
  /// What is known now, without waiting.
  var current: BackupSnapshot { get }
  /// The current state, then every change.
  func updates() -> AsyncStream<BackupSnapshot>
  /// The backup folders the NAS offers (or the sample library's). Throws `BackupState`.
  func folders() async throws -> [BackupFolder]
  /// Asks for Photos access if it was never asked; true once it is given (all or some).
  func requestPhotosAccess() async -> Bool
  func choose(folder: BackupFolder, layout: BackupLayout) async
  func setAutomatic(_ on: Bool)
  func setAllowsSleep(_ on: Bool)
  /// Starts now. `overMobileData`: the person said so for this run.
  func start(overMobileData: Bool)
  /// Stops after the current file (its unfinished copy on the NAS is gone on with next time).
  func stop()
  /// The photos not backed up yet, as of the last check, newest first: their Photos identifiers.
  func notBackedUp() async -> [String]
  /// A small picture of one of them, from Photos.
  func thumbnail(ofPhoto id: String, side: CGFloat) async -> UIImage?
}

public extension BackupService {
  func activity() -> AsyncStream<TransferActivity?> {
    let source = updates()
    return AsyncStream { continuation in
      let task = Task {
        var last: TransferActivity??
        for await snapshot in source {
          let next = snapshot.activity
          if last == nil || last! != next { continuation.yield(next) }
          last = .some(next)
        }
        continuation.finish()
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }
}

extension BackupState: Error {}

/// No backup: a build without one, or before the library is open.
public struct NoBackup: BackupService {
  public init() {}
  public var current: BackupSnapshot { BackupSnapshot() }
  public func updates() -> AsyncStream<BackupSnapshot> {
    AsyncStream { $0.yield(BackupSnapshot()); $0.finish() }
  }
  public func folders() async throws -> [BackupFolder] { [] }
  public func requestPhotosAccess() async -> Bool { false }
  public func choose(folder: BackupFolder, layout: BackupLayout) async {}
  public func setAutomatic(_ on: Bool) {}
  public func setAllowsSleep(_ on: Bool) {}
  public func start(overMobileData: Bool) {}
  public func stop() {}
  public func notBackedUp() async -> [String] { [] }
  public func thumbnail(ofPhoto id: String, side: CGFloat) async -> UIImage? { nil }
}
