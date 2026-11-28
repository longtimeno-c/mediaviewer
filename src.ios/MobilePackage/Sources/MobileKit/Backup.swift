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

/// A backup folder the NAS offers, or a folder inside one (each person on a shared NAS backs up
/// to their own: "Phone backup/Tristan"). `id` is the core's destination: the backup folder's id,
/// then "/" and the path inside it. `writable` false: the service does not let this phone write there.
public struct BackupFolder: Hashable, Sendable, Identifiable {
  public let id: String
  public let label: String
  public let writable: Bool

  public init(id: String, label: String, writable: Bool) {
    self.id = id
    self.label = label
    self.writable = writable
  }

  /// The backup folder this is, or is inside.
  public var rootID: String { id.split(separator: "/", maxSplits: 1).first.map(String.init) ?? id }
  /// The last part of the label: the folder's own name.
  public var name: String { label.split(separator: "/").last.map(String.init) ?? label }

  /// The folder `name` inside this one; nil for a name that is not one plain folder name.
  public func child(named name: String) -> BackupFolder? {
    guard let clean = Self.folderName(name) else { return nil }
    return BackupFolder(id: id + "/" + clean, label: label + "/" + clean, writable: writable)
  }

  /// `typed` as a new folder's name, trimmed; nil when the server would refuse it (the core's
  /// driftwire::safe_relative for one part): empty, hidden (a leading "."), a path, a character
  /// Windows cannot keep, a trailing "." or space, or longer than 255 bytes.
  public static func folderName(_ typed: String) -> String? {
    let name = typed.trimmingCharacters(in: .whitespacesAndNewlines)
    guard !name.isEmpty, name.utf8.count <= 255, !name.hasPrefix("."), !name.hasSuffix("."), !name.hasSuffix(" ") else {
      return nil
    }
    if name.contains(where: { "/\\:<>\"|?*".contains($0) }) { return nil }
    if name.unicodeScalars.contains(where: { $0.properties.generalCategory == .control || $0.properties.generalCategory == .format }) {
      return nil
    }
    // A Windows device name is a device in every folder ("CON", "com1.txt").
    var base = name.split(separator: ".", maxSplits: 1, omittingEmptySubsequences: false).first.map { $0.lowercased() } ?? ""
    while base.hasSuffix(" ") { base.removeLast() }
    if ["con", "prn", "aux", "nul", "conin$", "conout$"].contains(base) { return nil }
    if ["com", "lpt"].contains(String(base.prefix(3))), base.count == 4, let n = base.last, n.isASCII && n.isNumber || "¹²³".contains(n) {
      return nil
    }
    return name
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

/// How far a run has looked through Photos before it knows what is left to back up.
public struct BackupListing: Hashable, Sendable {
  public let looked: Int
  public let of: Int

  public init(looked: Int, of: Int) {
    self.looked = looked
    self.of = of
  }

  public var fraction: Double { of > 0 ? min(1, Double(looked) / Double(of)) : 0 }
  /// "Looked at 1,500 of 18,320 photos".
  public var line: String { "Looked at \(looked.formatted()) of \(of.formatted()) photos" }
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
  /// The sample library's "Sample server on this iPhone".
  public var isSample: Bool
  public var failures: [BackupFailureRow]
  public var space: BackupSpace?
  /// While a run looks through Photos (before `total` is known).
  public var listing: BackupListing?

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
  /// The folders inside a backup folder (or a folder in one). Throws `BackupState`.
  func subfolders(of folder: BackupFolder) async throws -> [BackupFolder]
  /// Makes the folder `name` inside `folder` (there already is fine). Throws `BackupState`, or
  /// `BackupFolderRefused` for a name the server will not take (a file has it).
  func makeFolder(named name: String, in folder: BackupFolder) async throws -> BackupFolder
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

/// New Folder's name was refused: not a plain folder name, or a file already has it.
public struct BackupFolderRefused: Error, Hashable, Sendable {
  public init() {}
}

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
  public func subfolders(of folder: BackupFolder) async throws -> [BackupFolder] { [] }
  public func makeFolder(named name: String, in folder: BackupFolder) async throws -> BackupFolder {
    throw BackupState.unreachable
  }
  public func setAutomatic(_ on: Bool) {}
  public func setAllowsSleep(_ on: Bool) {}
  public func start(overMobileData: Bool) {}
  public func stop() {}
  public func notBackedUp() async -> [String] { [] }
  public func thumbnail(ofPhoto id: String, side: CGFloat) async -> UIImage? { nil }
}
