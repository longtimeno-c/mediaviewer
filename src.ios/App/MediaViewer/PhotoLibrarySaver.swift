// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// "Save to Photos": the original from the NAS, added to the phone's library as it was shot. Asks
// only for add access, which cannot read anything already in Photos.
import MobileKit
import MobileLink
import ImageIO
import Photos
import UniformTypeIdentifiers

struct PhotoLibrarySaver: PhotoSaver {
  let originals: LinkImages

  func save(_ item: MediaItem) -> AsyncThrowingStream<TransferProgress, Error> {
    AsyncThrowingStream { continuation in
      let task = Task.detached(priority: .userInitiated) {
        do {
          let access = await PHPhotoLibrary.requestAuthorization(for: .addOnly)
          guard access == .authorized || access == .limited else { throw MediaFailure.photosNotAllowed }
          var file: URL?
          for try await event in originals.original(for: item) {
            switch event {
            case .progress(let progress): continuation.yield(progress)
            case .finished(let url): file = url
            }
          }
          guard let file else { throw MediaFailure.synologyUnreachable }
          try await Self.add(file, as: item)
          continuation.finish()
        } catch {
          continuation.finish(throwing: error)
        }
      }
      continuation.onTermination = { _ in task.cancel() }
    }
  }

  private static func add(_ file: URL, as item: MediaItem) async throws {
    // Photos refuses a resource without an extension (PHPhotosError 3302) and the cached original
    // has none, so it gets a hard link under the original's own name, gone once Photos has its copy.
    let folder = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString, isDirectory: true)
    try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: folder) }
    let type = fileType(of: file, named: item.fileName, isClip: item.kind == .clip)
    var name = safeName(item.fileName)
    if mediaType(forExtension: (name as NSString).pathExtension) == nil, let ext = type.preferredFilenameExtension {
      name += "." + ext  // no extension Photos knows: it takes the one the content says
    }
    let named = folder.appendingPathComponent(name)
    do { try FileManager.default.linkItem(at: file, to: named) } catch { try FileManager.default.copyItem(at: file, to: named) }

    let options = PHAssetResourceCreationOptions()
    options.originalFilename = named.lastPathComponent
    options.uniformTypeIdentifier = type.identifier
    try await PHPhotoLibrary.shared().performChanges {
      PHAssetCreationRequest.forAsset().addResource(with: item.kind == .clip ? .video : .photo, fileURL: named, options: options)
    }
  }

  /// The type from the name's extension; else, for a picture, what its bytes say; else MPEG-4 for
  /// a clip and JPEG for a picture.
  private static func fileType(of file: URL, named name: String, isClip: Bool) -> UTType {
    if let known = mediaType(forExtension: (name as NSString).pathExtension) { return known }
    if !isClip, let source = CGImageSourceCreateWithURL(file as CFURL, nil),
       let sniffed = CGImageSourceGetType(source), let type = UTType(sniffed as String) {
      return type
    }
    return isClip ? .mpeg4Movie : .jpeg
  }

  /// A picture or movie type the system knows for this extension. An unknown extension gives a
  /// dynamic type, which Photos refuses, so that counts as none.
  private static func mediaType(forExtension ext: String) -> UTType? {
    guard !ext.isEmpty, let type = UTType(filenameExtension: ext), !type.isDynamic,
          type.conforms(to: .image) || type.conforms(to: .movie) else { return nil }
    return type
  }

  /// The file's own name, with no folder part, so it stays inside the temporary folder.
  private static func safeName(_ name: String) -> String {
    let last = (name as NSString).lastPathComponent
    return last.isEmpty || last == "." || last == ".." ? "Original" : last
  }
}
