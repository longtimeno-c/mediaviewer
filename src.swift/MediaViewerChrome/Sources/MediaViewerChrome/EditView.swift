// SPDX-License-Identifier: GPL-3.0-or-later
// PR 29 (docs/design/20): the Edit workspace. The header (name, ✦ Auto, Undo /
// Reset / Compare / Save copy) sits at the top of the right pane column, the
// icon rail (EditRailView) beside it; under it is the tab's pane: Crop, Trim
// and Enhance (EnhanceHubView) here, Adjust / Info / Jobs the existing
// AdjustView / MetadataView / JobsView. Every control runs a command the
// keyboard already has, and says which key in its help and accessibility hint,
// so the pane teaches the keys rather than replacing them.
import MVChromeBridge
import SwiftUI

// ---- the header -------------------------------------------------------------------
// docs/plans/edit-refresh.md S2 (§2.5): the strip is the inspector's header, as
// tall as its content (main_mac.mm hosts it with an intrinsic height). Row 1:
// the file name (truncated in the middle), the edit count, a Photos / iCloud
// chip and ✦ Auto with the tools popover. Row 2: Undo · Reset · Compare, and
// Save copy… filled with the accent. Below 340 pt both rows are icons only
// (ViewThatFits); tooltips and the screen reader carry the live keys. The tabs
// moved to the rail (EditRailView), and so did Done.

struct EditStripView: View {
  @ObservedObject private var store = EditStore.shared
  @State private var toolsShown = false

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      ViewThatFits(in: .horizontal) {
        titleRow(labels: true)
        titleRow(labels: false)
      }
      if store.isClip {
        HStack(spacing: MVTheme.space(6)) {
          Button("Clip tools…") { store.run(EditCommand.clipTools) }
            .buttonStyle(FlatButtonStyle(compact: true))
            .help("Rotate, split, remux, save a frame, audio, GIF (⌘S)")
          Spacer(minLength: 0)
        }
      } else {
        ViewThatFits(in: .horizontal) {
          actionRow(labels: true)
          actionRow(labels: false)
        }
      }
    }
    .padding(.horizontal, MVTheme.space(12))
    .padding(.top, MVTheme.space(10))
    .padding(.bottom, MVTheme.space(10))
    .frame(maxWidth: .infinity, alignment: .topLeading)
    .background(ThemeField())
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
    .overlay(alignment: .bottom) { Rectangle().fill(MVTheme.hairline).frame(height: 1) }
    .accessibilityElement(children: .contain)
    .accessibilityLabel(store.title)
  }

  private var countText: String {
    store.editCount == 1 ? "1 edit" : "\(store.editCount) edits"
  }

  @ViewBuilder
  private func titleRow(labels: Bool) -> some View {
    HStack(spacing: MVTheme.space(6)) {
      // The name, its count and chip take what Auto leaves. Their ideal width
      // is small on purpose: ViewThatFits measures ideal widths, and a long
      // file name (it truncates in the middle anyway) must not cost ✦ Auto
      // its label.
      HStack(spacing: MVTheme.space(6)) {
        Text(store.name.isEmpty ? store.title : store.name)
          .font(MVTheme.font(15))
          .foregroundStyle(MVTheme.title)
          .lineLimit(1)
          .truncationMode(.middle)
          .layoutPriority(-1)
          .help(store.name)
          .accessibilityLabel("\(store.title): \(store.name)")
        if store.editCount > 0 && !store.isClip {
          Text(labels ? countText : "\(store.editCount)")
            .font(MVTheme.font(11).monospacedDigit())
            .foregroundStyle(MVTheme.body)
            .padding(.horizontal, MVTheme.space(6))
            .padding(.vertical, MVTheme.space(1))
            .background(Capsule().fill(MVTheme.surface2))
            .overlay(Capsule().stroke(MVTheme.hairline, lineWidth: 0.5))
            .fixedSize()
            .help("\(countText) on this photo; the original is never changed")
            .accessibilityLabel(countText)
        }
        if store.chips & 2 != 0 {
          chip(labels ? "iCloud" : nil, symbol: "icloud.and.arrow.down",
               help: "Downloading the original from iCloud…")
        } else if store.chips & 1 != 0 {
          chip(labels ? "Photos" : nil, symbol: "photo.on.rectangle",
               help: "A photo in your Photos library: results are added as new photos")
        }
      }
      .frame(minWidth: 0, idealWidth: 96, maxWidth: .infinity, alignment: .leading)
      if !store.isClip {
        autoButton(labels: labels)
      }
    }
  }

  private func chip(_ text: String?, symbol: String, help: String) -> some View {
    HStack(spacing: MVTheme.space(3)) {
      Image(systemName: symbol).font(.system(size: 10))
      if let text { Text(text).font(MVTheme.font(11)) }
    }
    .foregroundStyle(MVTheme.body)
    .padding(.horizontal, MVTheme.space(6))
    .padding(.vertical, MVTheme.space(2))
    .background(Capsule().fill(MVTheme.surface2))
    .fixedSize()
    .help(help)
    .accessibilityLabel(help)
  }

  /// ✦ Auto (one undo step; filled while it is the last edit, and a click then
  /// takes it back) beside the chevron that opens the tools popover.
  private func autoButton(labels: Bool) -> some View {
    HStack(spacing: 0) {
      Button { store.autoEnhance() } label: {
        HStack(spacing: MVTheme.space(4)) {
          if store.autoBusy {
            ProgressView().controlSize(.mini)
          } else {
            Image(systemName: store.autoOn ? "checkmark" : "sparkles").font(.system(size: 11, weight: .semibold))
          }
          if labels { Text("Auto").font(MVTheme.font(13)) }
        }
        .padding(.leading, MVTheme.space(9))
        .padding(.trailing, MVTheme.space(6))
        .padding(.vertical, MVTheme.space(4))
        .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .disabled(!store.canEdit || store.autoBusy)
      .help(store.autoOn ? withKey("Auto is on: click to take it back", "Auto enhance")
                         : withKey("Auto: better light and colour in one click (one undo step)", "Auto enhance"))
      .accessibilityLabel(store.autoOn ? "Auto enhance, on" : "Auto enhance")
      Rectangle().fill(store.autoOn ? MVTheme.onAccent.opacity(0.4) : MVTheme.hairline).frame(width: 1, height: 16)
      Button { toolsShown.toggle() } label: {
        Image(systemName: "chevron.down").font(.system(size: 9, weight: .bold))
          .padding(.horizontal, MVTheme.space(6))
          .padding(.vertical, MVTheme.space(6))
          .contentShape(Rectangle())
      }
      .buttonStyle(.plain)
      .help("Enhance tools: Remove, Restyle, Faces, Enhance image, Extend")
      .accessibilityLabel("Enhance tools")
      .popover(isPresented: $toolsShown, arrowEdge: .bottom) {
        EnhanceToolsPopover { toolsShown = false }
      }
    }
    .foregroundStyle(store.autoOn ? MVTheme.onAccent : MVTheme.accent)
    .background(Capsule().fill(store.autoOn ? MVTheme.accent : MVTheme.selectionWash))
    .overlay(Capsule().stroke(MVTheme.accent.opacity(store.autoOn ? 0 : 0.5), lineWidth: 1))
    .fixedSize()
  }

  @ViewBuilder
  private func actionRow(labels: Bool) -> some View {
    HStack(spacing: MVTheme.space(4)) {
      HeaderButton(title: "Undo", symbol: "arrow.uturn.backward", labels: labels,
                   help: withKey("Undo the last edit", "Undo edit"), name: "Undo edit") { store.run(EditCommand.undo) }
        .disabled(store.editCount == 0)
      HeaderButton(title: "Reset", symbol: "arrow.counterclockwise", labels: labels,
                   help: withKey("Reset to the original, exactly", "Reset edits"),
                   name: "Reset to original") { store.run(EditCommand.reset) }
        .disabled(store.editCount == 0 && !store.cropActive)
      HeaderButton(title: "Compare", symbol: "square.split.2x1", labels: labels,
                   help: withKey("Show the original while on (or hold the key); nothing is changed",
                                 "Show original (hold)"),
                   selected: store.showOriginal) { store.setShowOriginal(!store.showOriginal) }
        .accessibilityAddTraits(store.showOriginal ? [.isSelected] : [])
      Spacer(minLength: MVTheme.space(6))
      Button { store.saveCopy() } label: {
        if labels { Text("Save copy…") } else { Image(systemName: "square.and.arrow.down") }
      }
      .buttonStyle(AccentButtonStyle())
      .help(withKey("Write a new file with these edits; the original is never changed", "Export…"))
      .accessibilityLabel("Save copy")
      .fixedSize()
    }
  }
}

/// A header action: an icon and its name (or the icon alone below 340 pt),
/// a wash on hover, the selected wash while on.
private struct HeaderButton: View {
  let title: String
  let symbol: String
  let labels: Bool
  let help: String
  /// The screen reader's name; the title when nil. Never the symbol's name,
  /// which is what an icon-only button (below 340 pt) read before 2026-11-05.
  var name: String? = nil
  var selected = false
  let action: () -> Void
  @State private var hover = false
  @Environment(\.isEnabled) private var enabled

  var body: some View {
    Button(action: action) {
      HStack(spacing: MVTheme.space(4)) {
        Image(systemName: symbol).font(.system(size: 12, weight: .medium))
          .accessibilityHidden(true)
        if labels { Text(title).font(MVTheme.font(13)).lineLimit(1) }
      }
      .foregroundStyle(!enabled ? MVTheme.disabled : (selected ? MVTheme.accent : MVTheme.title))
      .padding(.horizontal, MVTheme.space(labels ? 8 : 7))
      .padding(.vertical, MVTheme.space(5))
      .background(RoundedRectangle(cornerRadius: MVTheme.radius(6))
        .fill(selected ? MVTheme.selectionWash : (hover && enabled ? MVTheme.hoverWash : .clear)))
      .contentShape(Rectangle())
    }
    .buttonStyle(.plain)
    .onHover { hover = $0 }
    .help(help)
    .accessibilityLabel(name ?? title)
    .fixedSize()
  }
}

// ---- the tab's pane (Crop or Trim) ----------------------------------------------------

struct EditPaneView: View {
  @ObservedObject private var store = EditStore.shared

  var body: some View {
    // The width class (docs/plans/edit-refresh.md §2.5) for the Crop and
    // Enhance layouts; read once at the root, so a drag does not rebuild.
    WidthClassReader { _ in
      ScrollView {
        Group {
          switch store.tab {
          case .trim: TrimPane()
          case .enhance: EnhanceHubView()
          default: CropPane()
          }
        }
        .padding(MVTheme.space(14))
        .frame(maxWidth: .infinity, alignment: .topLeading)
      }
    }
    .background(ThemeField())
    .overlay(alignment: .leading) { Rectangle().fill(MVTheme.hairline).frame(width: 1) }
  }
}

private struct SectionTitle: View {
  let text: String
  var body: some View {
    Text(text.uppercased()).font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
      .tracking(0.6)
      .accessibilityAddTraits(.isHeader)
      .accessibilityLabel(text)
  }
}

private struct CropPane: View {
  @ObservedObject private var store = EditStore.shared
  @Environment(\.paneWidthClass) private var cls

  private var columns: [GridItem] {
    // Wide: the seven ratios in one row (docs/plans/edit-refresh.md §2.5).
    cls.twoColumns ? Array(repeating: GridItem(.flexible(), spacing: MVTheme.space(6)), count: CropAspect.allCases.count)
                   : [GridItem(.adaptive(minimum: 64), spacing: MVTheme.space(6))]
  }

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(18)) {
      // Start / apply / cancel.
      VStack(alignment: .leading, spacing: MVTheme.space(8)) {
        SectionTitle(text: "Crop")
        if store.cropActive {
          if store.cropWidth > 0 {
            Text("\(store.cropWidth) × \(store.cropHeight) px")
              .font(MVTheme.font(15).monospacedDigit())
              .foregroundStyle(MVTheme.title)
              .accessibilityLabel("Crop size \(store.cropWidth) by \(store.cropHeight) pixels")
          }
          HStack(spacing: MVTheme.space(6)) {
            Button("Apply") { store.run(EditCommand.cropCommit) }
              .buttonStyle(AccentButtonStyle())
              .help("Apply the crop (Return)")
              .accessibilityHint("Key Return")
            Button("Cancel") { store.cancelCrop() }
              .buttonStyle(FlatButtonStyle(compact: true))
              .help("Drop this crop (Esc)")
              .accessibilityHint("Key Escape")
          }
        } else {
          Button { store.run(EditCommand.cropMode) } label: {
            Label("Crop and straighten", systemImage: "crop")
          }
          .buttonStyle(AccentButtonStyle(filled: false))
          .disabled(!store.canEdit)
          .help(withKey("Start cropping", "Crop / straighten"))
          .accessibilityHint("Key \(liveKey("Crop / straighten"))")
        }
      }

      VStack(alignment: .leading, spacing: MVTheme.space(8)) {
        SectionTitle(text: "Aspect ratio")
        LazyVGrid(columns: columns, alignment: .leading, spacing: MVTheme.space(6)) {
          ForEach(CropAspect.allCases, id: \.rawValue) { a in
            let selected = store.aspect == a
            Button { store.setAspect(a, portrait: store.portrait) } label: {
              Text(a.label).lineLimit(1).minimumScaleFactor(0.8).frame(maxWidth: .infinity)
            }
              .buttonStyle(FlatButtonStyle(selected: selected, compact: true))
              .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(4))
                .stroke(selected ? MVTheme.accent : MVTheme.hairline, lineWidth: selected ? 1.5 : 0.5))
              .accessibilityLabel("Aspect \(a.label)")
              .accessibilityAddTraits(selected ? [.isSelected] : [])
          }
        }
        HStack(spacing: MVTheme.space(8)) {
          Button {
            store.setAspect(store.aspect, portrait: !store.portrait)
          } label: {
            Label(store.portrait ? "Portrait" : "Landscape",
                  systemImage: store.portrait ? "rectangle.portrait" : "rectangle")
          }
            .buttonStyle(FlatButtonStyle(compact: true))
            .disabled(!store.aspect.hasOrientation)
            .help("Swap portrait / landscape (X while cropping)")
          Text("A next ratio · X swap").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        }
      }

      if cls.twoColumns {
        // Wide: straighten beside rotate and flip.
        HStack(alignment: .top, spacing: MVTheme.space(18)) {
          straighten.frame(maxWidth: .infinity, alignment: .leading)
          rotate.frame(maxWidth: .infinity, alignment: .leading)
        }
      } else {
        straighten
        rotate
      }

      Text("While cropping: ← → ↑ ↓ move · ⇧ + arrows resize · Return apply · Esc cancel")
        .font(MVTheme.font(12))
        .foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
    }
    .disabled(!store.canEdit)
  }

  private var straighten: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      HStack(spacing: MVTheme.space(6)) {
        SectionTitle(text: "Straighten")
        Spacer()
        Text(String(format: "%+.1f°", store.straighten))
          .font(MVTheme.font(14).monospacedDigit())
          .foregroundStyle(store.straighten == 0 ? MVTheme.body : MVTheme.title)
        IconButton(symbol: "arrow.counterclockwise", help: "Level (0°)") { store.setStraighten(0) }
          .disabled(store.straighten == 0)
          .accessibilityLabel("Straighten to zero")
      }
      Slider(value: Binding(get: { store.straighten }, set: { store.setStraighten(($0 * 2).rounded() / 2) }),
             in: -45...45) { editing in store.draggingStraighten = editing }
        .controlSize(.small)
        .accessibilityLabel("Straighten")
        .accessibilityValue(String(format: "%.1f degrees", store.straighten))
      Text(", . tilt 0.5° while cropping").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
    }
  }

  private var rotate: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(8)) {
      SectionTitle(text: "Rotate and flip")
      HStack(spacing: MVTheme.space(6)) {
        iconButton("rotate.left", "Rotate left", "[", EditCommand.rotateLeft)
        iconButton("rotate.right", "Rotate right", "]", EditCommand.rotateRight)
        iconButton("arrow.left.and.right.righttriangle.left.righttriangle.right", "Flip horizontal", "H",
                   EditCommand.flipH)
        iconButton("arrow.up.and.down.righttriangle.up.righttriangle.down", "Flip vertical", "V",
                   EditCommand.flipV)
      }
      Text("A JPEG with only turns is rewritten losslessly.").font(MVTheme.font(12)).foregroundStyle(MVTheme.body)
        .fixedSize(horizontal: false, vertical: true)
    }
  }

  private func iconButton(_ symbol: String, _ label: String, _ key: String, _ command: Int32) -> some View {
    Button { store.run(command) } label: { Image(systemName: symbol).font(.system(size: 14)).frame(width: 26, height: 22) }
      .buttonStyle(FlatButtonStyle(compact: true))
      .overlay(RoundedRectangle(cornerRadius: MVTheme.radius(4)).stroke(MVTheme.hairline, lineWidth: 0.5))
      .help("\(label) (\(key))")
      .accessibilityLabel(label)
      .accessibilityHint("Key \(key)")
  }
}

private struct TrimPane: View {
  @ObservedObject private var store = EditStore.shared
  @ObservedObject private var video = VideoStore.shared

  var body: some View {
    VStack(alignment: .leading, spacing: MVTheme.space(14)) {
      VStack(alignment: .leading, spacing: MVTheme.space(6)) {
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
              .buttonStyle(FlatButtonStyle(compact: true))
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
      VStack(alignment: .leading, spacing: MVTheme.space(6)) {
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
