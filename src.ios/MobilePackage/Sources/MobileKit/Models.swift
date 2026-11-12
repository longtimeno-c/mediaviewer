// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// docs/plans/mobile.md: the value types the screens read. Plain data, nothing about how the
// core stores it.
import Foundation

public struct MediaID: Hashable, Sendable {
  /// The core's id for the item: in the catalogue, the decimal `assets.id`.
  public let raw: String
  /// Which opened catalogue the id belongs to (0: not from one). A new generation can reuse an
  /// id for another picture, so two generations' items are never equal.
  public let catalogue: UInt64
  public init(_ raw: String, catalogue: UInt64 = 0) {
    self.raw = raw
    self.catalogue = catalogue
  }
}

public enum MediaKind: Hashable, Sendable { case photo, clip }

public struct MediaItem: Identifiable, Hashable, Sendable {
  public let id: MediaID
  public let kind: MediaKind
  public let capturedAt: Date
  public let pixelWidth: Int
  public let pixelHeight: Int
  /// Clips only.
  public let duration: TimeInterval?
  public let fileName: String
  /// The shape when `pixelWidth` is 0: the catalogue knows a picture's shape from its tile, not
  /// its size.
  public let aspectHint: Double?

  public init(id: MediaID, kind: MediaKind, capturedAt: Date, pixelWidth: Int, pixelHeight: Int,
              duration: TimeInterval?, fileName: String, aspectHint: Double? = nil) {
    self.id = id
    self.kind = kind
    self.capturedAt = capturedAt
    self.pixelWidth = pixelWidth
    self.pixelHeight = pixelHeight
    self.duration = duration
    self.fileName = fileName
    self.aspectHint = aspectHint
  }

  public var aspect: Double {
    if pixelHeight > 0 { return Double(pixelWidth) / Double(pixelHeight) }
    return aspectHint ?? 1
  }
}

/// A place inside a clip that a search or the index knows about.
public struct Moment: Identifiable, Hashable, Sendable {
  public enum Kind: Hashable, Sendable { case speech, scene, wordsOnScreen }
  public let seconds: TimeInterval
  public let kind: Kind
  /// What was said, what is on screen, or a short name for the scene.
  public let text: String
  public var id: TimeInterval { seconds }

  public init(seconds: TimeInterval, kind: Kind, text: String) {
    self.seconds = seconds
    self.kind = kind
    self.text = text
  }
}

public struct SearchHit: Identifiable, Hashable, Sendable {
  public let item: MediaItem
  /// A speech or scene hit opens the clip at this moment.
  public let moment: Moment?
  public var id: String { item.id.raw + (moment.map { "@\($0.seconds)" } ?? "") }

  public init(item: MediaItem, moment: Moment?) {
    self.item = item
    self.moment = moment
  }
}

public struct Folder: Identifiable, Hashable, Sendable {
  public let id: String
  public let name: String
  /// Everything in this folder and the folders beneath it.
  public let itemCount: Int
  public let hasSubfolders: Bool

  public init(id: String, name: String, itemCount: Int, hasSubfolders: Bool) {
    self.id = id
    self.name = name
    self.itemCount = itemCount
    self.hasSubfolders = hasSubfolders
  }
}

public struct Person: Identifiable, Hashable, Sendable {
  public let id: String
  public let name: String
  public let itemCount: Int
  /// The item whose picture stands for this person.
  public let face: MediaItem

  public init(id: String, name: String, itemCount: Int, face: MediaItem) {
    self.id = id
    self.name = name
    self.itemCount = itemCount
    self.face = face
  }
}

/// One month's stretch of a list. Newest first, as the list is.
public struct MonthRange: Hashable, Sendable {
  public let key: Int
  public let date: Date
  public let range: Range<Int>

  public init(key: Int, date: Date, range: Range<Int>) {
    self.key = key
    self.date = date
    self.range = range
  }
}

/// A library's worth of items, read one at a time. The core can back this with rows it already has
/// mapped, so a 100,000-item library is not copied into Swift values (§9's memory budget).
public protocol MediaList: Sendable {
  var count: Int { get }
  var months: [MonthRange] { get }
  /// Called on the main thread for what is on screen, so it must return within a frame.
  func item(at index: Int) -> MediaItem
}

/// A list already in memory: results the core has handed over, and small or test libraries.
public struct ArrayMediaList: MediaList {
  private let items: [MediaItem]
  public let months: [MonthRange]

  /// `items` are newest first. Splits them in one pass, so call this off the main thread.
  public init(_ items: [MediaItem]) {
    self.items = items
    let calendar = Calendar.current
    var months: [MonthRange] = []
    var start = 0
    var key = -1
    var date = Date.distantPast
    for (index, item) in items.enumerated() {
      let parts = calendar.dateComponents([.year, .month], from: item.capturedAt)
      let current = (parts.year ?? 0) * 100 + (parts.month ?? 0)
      guard current != key else { continue }
      if index > start || key != -1 { months.append(MonthRange(key: key, date: date, range: start..<index)) }
      start = index
      key = current
      date = item.capturedAt
    }
    if key != -1 { months.append(MonthRange(key: key, date: date, range: start..<items.count)) }
    self.months = months
  }

  public var count: Int { items.count }
  public func item(at index: Int) -> MediaItem { items[index] }
}

public enum LibraryScope: Hashable, Sendable {
  case everything
  case folder(Folder)
  case person(Person)
}

public struct MediaDetails: Hashable, Sendable {
  public let place: String?
  public let camera: String?
  public let lens: String?
  public let exposure: String?
  public let folderPath: String
  public let formatName: String
  public let fileBytes: Int64

  public init(place: String?, camera: String?, lens: String?, exposure: String?, folderPath: String,
              formatName: String, fileBytes: Int64) {
    self.place = place
    self.camera = camera
    self.lens = lens
    self.exposure = exposure
    self.folderPath = folderPath
    self.formatName = formatName
    self.fileBytes = fileBytes
  }
}

/// Why a picture or a clip could not come from the NAS. The screens own the words.
public enum MediaFailure: Error, Hashable, Sendable {
  case synologyUnreachable
  case originalGone
  case notThereYet
  /// The NAS service has no folder named for the share this file is on.
  case folderNotOnNAS
  /// ImageIO cannot decode it; `format` is the name a person knows ("Sony ARW").
  case cannotDecode(format: String)
  case serviceTooOld
  case publisherChanged
  /// The person has not let MediaViewer add to Photos.
  case photosNotAllowed
}

// MARK: Connection

/// One address the phone tried, and why it did not answer.
public struct AddressFailure: Hashable, Sendable, Identifiable {
  public let address: String
  public let reason: String
  public var id: String { address }

  public init(address: String, reason: String) {
    self.address = address
    self.reason = reason
  }
}

public enum SynologyState: Hashable, Sendable {
  case reachable(address: String, overInternet: Bool)
  case unreachable([AddressFailure])
  case reachableOnlyOnItsOwnNetwork
  case serviceTooOld
  case publisherChanged
  /// The computer removed this phone from the library.
  case deviceRevoked
}

public struct CatalogueInfo: Hashable, Sendable {
  public enum Source: Hashable, Sendable { case synology, nearbyMac }
  public let source: Source
  public let updatedAt: Date
  public let itemCount: Int
  /// Rises when a new computer takes the library over with the old one's signature (§5, rule 5).
  public let epoch: UInt64

  public init(source: Source, updatedAt: Date, itemCount: Int, epoch: UInt64 = 0) {
    self.source = source
    self.updatedAt = updatedAt
    self.itemCount = itemCount
    self.epoch = epoch
  }
}

public struct ConnectionSnapshot: Hashable, Sendable {
  public let hasLibrary: Bool
  public let synology: SynologyState
  public let macNearby: Bool
  public let catalogue: CatalogueInfo?
  public let cacheBytes: Int64
  /// Which paired library this snapshot is about ("" with none), stamped when it is published, so a
  /// late snapshot is never read as another library's.
  public var libraryID: String = ""
  /// The sample library (§11.1), read from its static folder on GitHub rather than a Synology. The
  /// app says so wherever the library's source is named.
  public var isSample = false
  /// The paired library's name, as its code gave it ("" with none): said when something would replace it.
  public var libraryName = ""

  public init(hasLibrary: Bool, synology: SynologyState, macNearby: Bool, catalogue: CatalogueInfo?,
              cacheBytes: Int64) {
    self.hasLibrary = hasLibrary
    self.synology = synology
    self.macNearby = macNearby
    self.catalogue = catalogue
    self.cacheBytes = cacheBytes
  }

  /// A phone with nothing paired.
  public static let noLibrary = ConnectionSnapshot(
    hasLibrary: false, synology: .unreachable([]), macNearby: false, catalogue: nil, cacheBytes: 0)
}

// MARK: Pairing and copying

/// What the code says it pairs with (§7.4). The desktop shows the same two things, and the person
/// confirms they match, so a fake code cannot quietly pair the phone with someone else's server.
public struct LibraryIdentity: Hashable, Sendable {
  public let name: String
  /// Short enough to compare by eye: "7F3A 92C1".
  public let fingerprint: String
  /// The sample library's code: nothing on a computer to compare with.
  public let isSample: Bool

  public init(name: String, fingerprint: String, isSample: Bool = false) {
    self.name = name
    self.fingerprint = fingerprint
    self.isSample = isSample
  }
}

public enum PairingStep: Int, CaseIterable, Hashable, Sendable {
  case mac, synology, outsideRoute
}

public enum OutsideRouteResult: Hashable, Sendable {
  case reachable
  case reachableOnlyOnItsOwnNetwork
  /// An iPad without mobile data skips the test and says so.
  case skippedNoMobileData
}

public enum PairingEvent: Hashable, Sendable {
  case working(PairingStep)
  case passed(PairingStep)
  /// Not done by this build (the Mac step until nearby sync exists).
  case skipped(PairingStep)
  case outsideRoute(OutsideRouteResult)
}

public enum PairingFailure: Error, Hashable, Sendable {
  case codeExpired
  case codeNotReadable
  case synologyUnreachable([AddressFailure])
  case serviceTooOld
  /// It answered with something not signed by the key the code pins (the sample library's list).
  case catalogueRejected
}

public enum CopyEvent: Hashable, Sendable {
  /// Copies wait for Wi-Fi unless the person taps; the size is shown first.
  case waitingForWiFi(totalBytes: Int64)
  case copying(doneBytes: Int64, totalBytes: Int64)
  /// Back after the app was left on a system that stops the copy when it is.
  case resuming(doneBytes: Int64, totalBytes: Int64)
}

public struct SearchDateRange: Identifiable, Hashable, Sendable {
  public let id: String
  /// "This month", "2025".
  public let label: String
  public let from: Date
  public let to: Date

  public init(id: String, label: String, from: Date, to: Date) {
    self.id = id
    self.label = label
    self.from = from
    self.to = to
  }
}

public struct SearchFilters: Hashable, Sendable {
  public var person: Person?
  public var place: String?
  public var date: SearchDateRange?
  public var camera: String?
  public var kind: MediaKind?

  public init(person: Person? = nil, place: String? = nil, date: SearchDateRange? = nil, camera: String? = nil,
              kind: MediaKind? = nil) {
    self.person = person
    self.place = place
    self.date = date
    self.camera = camera
    self.kind = kind
  }

  public var isEmpty: Bool { self == SearchFilters() }
}

/// What this library can be narrowed by. An empty list hides that filter.
public struct SearchFilterOptions: Hashable, Sendable {
  public let people: [Person]
  public let places: [String]
  public let dates: [SearchDateRange]
  public let cameras: [String]

  public init(people: [Person], places: [String], dates: [SearchDateRange], cameras: [String]) {
    self.people = people
    self.places = places
    self.dates = dates
    self.cameras = cameras
  }

  public static let none = SearchFilterOptions(people: [], places: [], dates: [], cameras: [])
}

#if MV_BACKUP
/// Long work that carries on while the person uses the app, such as a phone backup. It shows above
/// the tab bar only while it runs.
public struct TransferActivity: Hashable, Sendable {
  /// "1,204 of 18,320 backed up · 17,116 to go"
  public let line: String
  /// The same, for when space is short: "1,204 of 18,320".
  public let shortLine: String
  /// nil while how far along is unknown.
  public let fraction: Double?

  public init(line: String, shortLine: String, fraction: Double?) {
    self.line = line
    self.shortLine = shortLine
    self.fraction = fraction
  }
}
#endif

public enum CopyFailure: Error, Hashable, Sendable {
  case synologyUnreachable
  case notEnoughSpace(neededBytes: Int64)
  /// What arrived does not match what the computer published and signed.
  case catalogueRejected
}

public enum DownloadEvent: Sendable {
  case progress(TransferProgress)
  /// The whole file, on the phone.
  case finished(URL)
}

public struct TransferProgress: Hashable, Sendable {
  public let doneBytes: Int64
  public let totalBytes: Int64

  public init(doneBytes: Int64, totalBytes: Int64) {
    self.doneBytes = doneBytes
    self.totalBytes = totalBytes
  }
}
