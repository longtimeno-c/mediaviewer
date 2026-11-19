// Copyright (C) 2026 longtimeno-c
// SPDX-License-Identifier: GPL-3.0-or-later
// A NAS folder's other files (PDFs, saves, archives): one quiet list by name, under its folders.
// Above a folder's photos it shows a few rows and the rest behind "Show all", so the photos stay in
// view; a folder of only files lists them all. A tap fetches the file and opens the phone's own
// preview (Quick Look); MediaViewer decodes none of them.
import MobileKit
import SwiftUI

struct FileList: View {
  let files: [OtherFile]
  /// Rows shown before "Show all"; nil shows every file.
  var collapsedTo: Int?
  @State private var expanded = false

  var body: some View {
    let shown = expanded || collapsedTo == nil ? files : Array(files.prefix(collapsedTo!))
    VStack(alignment: .leading, spacing: 8) {
      Text(files.count == 1 ? "1 file" : "\(files.count.formatted()) files")
        .font(.subheadline.weight(.semibold))
        .foregroundStyle(Theme.title)
        .accessibilityAddTraits(.isHeader)
      LazyVStack(spacing: 0) {
        ForEach(Array(shown.enumerated()), id: \.element.id) { index, file in
          if index > 0 { Divider().overlay(Theme.lifted).padding(.leading, 56) }
          FileRow(file: file)
        }
        if shown.count < files.count {
          Divider().overlay(Theme.lifted)
          Button {
            expanded = true
          } label: {
            Text("Show all \(files.count.formatted()) files")
              .font(.subheadline.weight(.medium))
              .foregroundStyle(Theme.accent)
              .frame(maxWidth: .infinity, minHeight: 44)
              .contentShape(Rectangle())
          }
          .buttonStyle(TileButtonStyle())
        }
      }
      .background(RoundedRectangle(cornerRadius: Theme.cardRadius, style: .continuous).fill(Theme.surface))
    }
    .padding(.horizontal, Theme.inset)
    .padding(.top, 8)
    .padding(.bottom, 8)
  }
}

private struct FileRow: View {
  let file: OtherFile
  @Environment(AppModel.self) private var model

  var body: some View {
    let library = model.library
    let opening = library.openingID == file.id
    let failure = library.openFailure.flatMap { $0.id == file.id ? $0.message : nil }
    Button { library.open(file) } label: {
      HStack(spacing: 12) {
        FileGlyph(family: file.family)
        VStack(alignment: .leading, spacing: 2) {
          Text(file.name)
            .font(.subheadline)
            .foregroundStyle(Theme.title)
            .lineLimit(2)
            .truncationMode(.middle)
            .multilineTextAlignment(.leading)
          if opening {
            DownloadProgress()
          } else if let failure {
            Text(failure).font(.caption).foregroundStyle(Theme.warm)
          } else {
            Text(detail).font(.caption.monospacedDigit()).foregroundStyle(Theme.body).lineLimit(1)
          }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        if opening {
          Image(systemName: "xmark").font(.footnote.weight(.semibold)).foregroundStyle(Theme.body)
            .accessibilityHidden(true)
        }
      }
      .padding(.horizontal, Theme.inset)
      .padding(.vertical, 10)
      .frame(minHeight: 56)
      .contentShape(Rectangle())
    }
    .buttonStyle(TileButtonStyle())
    .accessibilityElement(children: .ignore)
    .accessibilityLabel([file.name, opening ? "Downloading" : (failure ?? detail)].joined(separator: ", "))
    .accessibilityAddTraits(.isButton)
    .accessibilityHint(opening ? "Stops the download" : "Opens the file")
  }

  /// "PDF · 2.4 MB · 3 Mar 2024"
  private var detail: String {
    let ext = file.fileExtension.uppercased()
    let size = ByteCountFormatter.string(fromByteCount: file.bytes, countStyle: .file)
    return [ext.isEmpty ? nil : ext, size, file.modified.formatted(date: .abbreviated, time: .omitted)]
      .compactMap { $0 }.joined(separator: " · ")
  }
}

/// The one row being fetched: the only view that redraws as the bytes come in.
private struct DownloadProgress: View {
  @Environment(AppModel.self) private var model

  var body: some View {
    let p = model.library.openProgress
    VStack(alignment: .leading, spacing: 2) {
      ProgressTrack(fraction: p.total > 0 ? Double(p.done) / Double(p.total) : nil)
        .frame(height: 3)
        .padding(.vertical, 3)
      Text(p.total > 0 ? "Downloading · \(ProgressWords.amounts(p.done, of: p.total))" : "Downloading…")
        .font(.caption.monospacedDigit())
        .foregroundStyle(Theme.body)
    }
  }
}

/// What kind of file it is, as a symbol on a lifted square.
struct FileGlyph: View {
  let family: OtherFile.Family
  @ScaledMetric(relativeTo: .subheadline) private var side: CGFloat = 32

  var body: some View {
    Image(systemName: Self.symbol(family))
      .font(.system(size: side * 0.45, weight: .medium))
      .foregroundStyle(family == .pdf ? Theme.warm : Theme.body)
      .frame(width: side, height: side)
      .background(RoundedRectangle(cornerRadius: 7, style: .continuous).fill(Theme.lifted))
      .accessibilityHidden(true)
  }

  static func symbol(_ family: OtherFile.Family) -> String {
    switch family {
    case .pdf: return "doc.fill"
    case .text: return "doc.text.fill"
    case .sheet: return "tablecells.fill"
    case .slides: return "rectangle.on.rectangle.angled.fill"
    case .archive: return "doc.zipper"
    case .audio: return "music.note"
    case .code: return "curlybraces"
    case .disc: return "gamecontroller.fill"
    case .other: return "doc.fill"
    }
  }
}
