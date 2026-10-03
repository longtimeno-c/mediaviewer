// SPDX-License-Identifier: GPL-3.0-or-later
// PR 29 (docs/design/20): the Edit workspace. The strip (title, tabs, Undo / Reset /
// Original / Save copy) sits at the top of the right pane column; under it is
// the tab's pane: Crop and Trim here, Colour / Info / Jobs the existing
// AdjustView / MetadataView / JobsView. Every control runs a command the
// keyboard already has, and says which key in its help and accessibility hint,
// so the pane teaches the keys rather than replacing them.
import MVChromeBridge
import SwiftUI

// ---- the strip -------------------------------------------------------------------

struct EditStripView: View {
  @ObservedObject private var store = EditStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: 8) {
      HStack(alignment: .firstTextBaseline) {
        Text(store.title).font(MVTheme.font()).foregroundStyle(MVTheme.title)
        Text(store.name)
          .font(MVTheme.font(14))
          .foregroundStyle(MVTheme.body)
          .lineLimit(1)
          .truncationMode(.middle)
        Spacer()
        // No .keyboardShortcut: the router owns Esc (it cancels a crop first).
        Button("Done") { store.close() }
          .buttonStyle(FlatButtonStyle(compact: true))
          .help("Close the editor (Esc, or Return again)")
          .accessibilityHint("Applies a crop in progress and closes the editor")
      }
      // Tabs: a segmented row of plain buttons so each can carry its key.
      HStack(spacing: 4) {
        ForEach(store.tabs, id: \.rawValue) { t in
          let selected = store.tab == t
          Button { store.select(t) } label: { Text(t.label).frame(maxWidth: .infinity) }
          .buttonStyle(FlatButtonStyle(selected: selected, compact: true))
          .help("\(t.label) (\(t.key))")
          .accessibilityLabel(t.label)
          .accessibilityHint("Key \(t.key)")
          .accessibilityAddTraits(selected ? [.isSelected] : [])
        }
      }
      if store.isClip {
        HStack(spacing: 8) {
          Button("Clip tools…") { store.run(EditCommand.clipTools) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .help("Rotate, split, remux, save a frame, audio, GIF (⌘S)")
          Spacer()
        }
      } else {
        HStack(spacing: 8) {
          Button("Undo") { store.run(EditCommand.undo) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .disabled(store.editCount == 0)
            .help("Undo the last edit (⌘Z)")
            .accessibilityLabel("Undo edit")
            .accessibilityHint("Key Command Z")
          Button("Reset") { store.run(EditCommand.reset) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .disabled(store.editCount == 0 && !store.cropActive)
            .help("Reset to the original, exactly (⌘R)")
            .accessibilityLabel("Reset to original")
            .accessibilityHint("Key Command R")
          Button("Original") { store.setShowOriginal(!store.showOriginal) }
          .buttonStyle(FlatButtonStyle(selected: store.showOriginal, compact: true))
          .accessibilityAddTraits(store.showOriginal ? [.isSelected] : [])
          .help("Show the original while on (or hold Y)")
          .accessibilityHint("Shows the photo without edits; nothing is changed")
          Spacer()
          Button("Save copy…") { store.saveCopy() }
            .buttonStyle(FlatButtonStyle(compact: true))
            .fixedSize()
            .help("Write a new file with these edits; the original is never changed (⌘S)")
        }
      }
      Text(store.isClip ? "Trims and tools write new files; the clip is never changed."
                        : (store.editCount == 1 ? "1 edit · the original is never changed"
                                                : "\(store.editCount) edits · the original is never changed"))
        .font(MVTheme.font(12))
        .foregroundStyle(MVTheme.body)
    }
    .padding(12)
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(MVTheme.canvas)
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
    .overlay(alignment: .bottom) { Rectangle().fill(MVTheme.hairline).frame(height: 1) }
    .clipped()
    .accessibilityElement(children: .contain)
    .accessibilityLabel(store.title)
  }
}

// ---- the tab's pane (Crop or Trim) ----------------------------------------------------

struct EditPaneView: View {
  @ObservedObject private var store = EditStore.shared

  var body: some View {
    ScrollView {
      Group {
        if store.tab == .trim {
          TrimPane()
        } else {
          CropPane()
        }
      }
      .padding(14)
      .frame(maxWidth: .infinity, alignment: .topLeading)
    }
    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
    .background(MVTheme.canvas)
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
    .clipped()
  }
}

private struct SectionTitle: View {
  let text: String
  var body: some View {
    Text(text).font(MVTheme.font(14)).foregroundStyle(MVTheme.body)
      .accessibilityAddTraits(.isHeader)
  }
}

private struct CropPane: View {
  @ObservedObject private var store = EditStore.shared
  private let columns = [GridItem(.adaptive(minimum: 64), spacing: 6)]

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      // Start / apply / cancel.
      VStack(alignment: .leading, spacing: 6) {
        SectionTitle(text: "Crop")
        if store.cropActive {
          if store.cropWidth > 0 {
            Text("\(store.cropWidth) × \(store.cropHeight) px")
              .font(MVTheme.font(14))
              .foregroundStyle(MVTheme.title)
              .accessibilityLabel("Crop size \(store.cropWidth) by \(store.cropHeight) pixels")
          }
          HStack {
            Button("Apply") { store.run(EditCommand.cropCommit) }
              .buttonStyle(FlatButtonStyle(compact: true))
              .help("Apply the crop (Return)")
              .accessibilityHint("Key Return")
            Button("Cancel") { store.cancelCrop() }
              .help("Drop this crop (Esc)")
              .accessibilityHint("Key Escape")
          }
        } else {
          Button("Crop and straighten") { store.run(EditCommand.cropMode) }
          .buttonStyle(FlatButtonStyle(compact: true))
          .disabled(!store.canEdit)
          .help("Start cropping (⇧C)")
          .accessibilityHint("Key Shift C")
        }
      }

      VStack(alignment: .leading, spacing: 6) {
        SectionTitle(text: "Aspect ratio")
        LazyVGrid(columns: columns, alignment: .leading, spacing: 6) {
          ForEach(CropAspect.allCases, id: \.rawValue) { a in
            let selected = store.aspect == a
            Button { store.setAspect(a, portrait: store.portrait) } label: {
              Text(a.label).frame(maxWidth: .infinity)
            }
              .buttonStyle(FlatButtonStyle(selected: selected, compact: true))
              .accessibilityLabel("Aspect \(a.label)")
              .accessibilityAddTraits(selected ? [.isSelected] : [])
          }
        }
        Button(store.portrait ? "Portrait ⇄" : "Landscape ⇄") {
          store.setAspect(store.aspect, portrait: !store.portrait)
        }
          .buttonStyle(FlatButtonStyle(compact: true))
          .disabled(!store.aspect.hasOrientation)
          .help("Swap portrait / landscape (X while cropping)")
        Text("A next ratio · X swap, while cropping").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      }

      VStack(alignment: .leading, spacing: 6) {
        HStack {
          SectionTitle(text: "Straighten")
          Spacer()
          Text(String(format: "%+.1f°", store.straighten)).font(MVTheme.font(14)).foregroundStyle(MVTheme.body)
          Button("0") { store.setStraighten(0) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .help("Level")
            .accessibilityLabel("Straighten to zero")
        }
        Slider(value: Binding(get: { store.straighten }, set: { store.setStraighten(($0 * 2).rounded() / 2) }),
               in: -45...45) { editing in store.draggingStraighten = editing }
          .accessibilityLabel("Straighten")
          .accessibilityValue(String(format: "%.1f degrees", store.straighten))
        Text(", . tilt 0.5° while cropping").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      }

      VStack(alignment: .leading, spacing: 6) {
        SectionTitle(text: "Rotate and flip")
        HStack(spacing: 8) {
          iconButton("rotate.left", "Rotate left", "[", EditCommand.rotateLeft)
          iconButton("rotate.right", "Rotate right", "]", EditCommand.rotateRight)
          iconButton("arrow.left.and.right.righttriangle.left.righttriangle.right", "Flip horizontal", "H",
                     EditCommand.flipH)
          iconButton("arrow.up.and.down.righttriangle.up.righttriangle.down", "Flip vertical", "V",
                     EditCommand.flipV)
        }
        Text("A JPEG with only turns is rewritten losslessly.").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      }

      Text("While cropping: ← → ↑ ↓ move · ⇧ + arrows resize · Return apply · Esc cancel")
        .font(MVTheme.font(12))
        .foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
    }
    .disabled(!store.canEdit)
  }

  private func iconButton(_ symbol: String, _ label: String, _ key: String, _ command: Int32) -> some View {
    Button { store.run(command) } label: { Image(systemName: symbol).frame(width: 22, height: 18) }
      .buttonStyle(FlatButtonStyle(compact: true))
      .help("\(label) (\(key))")
      .accessibilityLabel(label)
      .accessibilityHint("Key \(key)")
  }
}

private struct TrimPane: View {
  @ObservedObject private var store = EditStore.shared
  @ObservedObject private var video = VideoStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: 14) {
      VStack(alignment: .leading, spacing: 6) {
        SectionTitle(text: "Trim")
        if video.trimArmed {
          if !video.trimLabel.isEmpty {
            Text(video.trimLabel).font(MVTheme.font(14)).foregroundStyle(MVTheme.title).fixedSize(horizontal: false, vertical: true)
          }
          HStack {
            cmd("Set in", "[", EditCommand.trimIn)
            cmd("Set out", "]", EditCommand.trimOut)
            cmd(video.trimPreviewing ? "Stop preview" : "Preview", "P", EditCommand.trimPreview)
          }
          HStack {
            Button("Save") { store.run(EditCommand.trimKeyframe) }
              .buttonStyle(FlatButtonStyle(compact: true))
              .help("Keyframe cut: instant, no re-encode (Return)")
              .accessibilityHint("Key Return. Fast, cuts on keyframes")
            Button("Save exact") { store.run(EditCommand.trimReencode) }
              .help("Frame-accurate re-encode, slower (⇧Return)")
              .accessibilityHint("Key Shift Return. Slower, frame accurate")
          }
          HStack {
            cmd("Copy without in–out", "⌘X", EditCommand.trimRemoveMiddle)
            cmd("Clear", "⌫", EditCommand.trimClear)
          }
          cmd("Stop trimming", "⌘T", EditCommand.trimMode)
        } else {
          Button("Start trimming") { store.run(EditCommand.trimMode) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .help("Set in and out points on the scrub bar (⌘T)")
            .accessibilityHint("Key Command T")
        }
      }
      VStack(alignment: .leading, spacing: 6) {
        SectionTitle(text: "Tools")
        cmd("Split at playhead", "⌘B", EditCommand.clipSplit)
        cmd("More clip tools…", "⌘S", EditCommand.clipTools)
      }
      Text("Space play · , . frame step · J K L shuttle · Q E skip")
        .font(MVTheme.font(12))
        .foregroundStyle(MVTheme.body)
    }
    .disabled(!store.canEdit)
  }

  private func cmd(_ title: String, _ key: String, _ command: Int32) -> some View {
    Button(title) { store.run(command) }
      .buttonStyle(FlatButtonStyle(compact: true))
      .help("\(title) (\(key))")
      .accessibilityHint("Key \(key)")
  }
}
