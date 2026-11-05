// SPDX-License-Identifier: GPL-3.0-or-later
// PR 29 (docs/design/20): the state the Edit workspace's strip and panes read. It
// mirrors the host's shell::edit_workspace and the crop draft, re-read only
// when the host's edit generation moves -- the AdjustStore shape.
import AppKit
import Combine
import MVChromeBridge

/// shell::edit_tab and shell::crop_aspect values (main_mac.mm static_asserts them).
enum EditTab: Int32, CaseIterable {
  // The wire values stay (colour is labelled Adjust); docs/plans/edit-refresh.md
  // S2 appends Enhance.
  case crop = 0, colour = 1, info = 2, trim = 3, jobs = 4, enhance = 5

  var label: String {
    switch self {
    case .crop: return "Crop"
    case .colour: return "Adjust"
    case .info: return "Info"
    case .trim: return "Trim"
    case .jobs: return "Jobs"
    case .enhance: return "Enhance"
    }
  }

  /// The rail's icon (SF Symbols).
  var symbol: String {
    switch self {
    case .crop: return "crop"
    case .colour: return "slider.horizontal.3"
    case .info: return "info.circle"
    case .trim: return "timeline.selection"
    case .jobs: return "list.bullet.rectangle"
    case .enhance: return "sparkles"
    }
  }

  /// The command table's name of the key that opens this tab (the live key,
  /// as remapped in Settings, comes from SettingsStore.keyLabel).
  var commandName: String? {
    switch self {
    case .crop: return "Crop / straighten"
    case .colour: return "Adjust pane"
    case .info: return "Metadata pane"
    case .trim: return "Trim clip"
    case .jobs: return "Jobs"
    case .enhance: return nil
    }
  }

  /// The key that opens this tab, for the label and the accessibility hint.
  @MainActor var key: String {
    if let name = commandName, let live = SettingsStore.keyLabel(name) { return live }
    switch self {
    case .crop: return "⇧C"
    case .colour: return "⇧A"
    case .info: return "I"
    case .trim: return "⌘T"
    case .jobs: return "⌘J"
    case .enhance: return ""
    }
  }
}

/// The live key of a command-table row, "" when it has none (remappable in Settings).
@MainActor func liveKey(_ name: String) -> String { SettingsStore.keyLabel(name) ?? "" }
/// "Text (key)" when the command has a key, else "Text".
@MainActor func withKey(_ text: String, _ name: String) -> String {
  let k = liveKey(name)
  return k.isEmpty ? text : "\(text) (\(k))"
}

enum CropAspect: Int32, CaseIterable {
  case free = 0, original, square, r4x3, r3x2, r16x9, r5x4

  var label: String {
    switch self {
    case .free: return "Free"
    case .original: return "Original"
    case .square: return "1:1"
    case .r4x3: return "4:3"
    case .r3x2: return "3:2"
    case .r16x9: return "16:9"
    case .r5x4: return "5:4"
    }
  }

  /// Free has no orientation and a square's is itself.
  var hasOrientation: Bool { self != .free && self != .square }
}

/// commands.h ids the strip and panes run (as their keys would).
enum EditCommand {
  static let metadataPane: Int32 = 92
  static let rotateLeft: Int32 = 96
  static let rotateRight: Int32 = 97
  static let flipH: Int32 = 98
  static let flipV: Int32 = 99
  static let cropMode: Int32 = 100
  static let cropCommit: Int32 = 101
  static let undo: Int32 = 113
  static let reset: Int32 = 114
  static let trimMode: Int32 = 134
  static let trimIn: Int32 = 135
  static let trimOut: Int32 = 136
  static let trimClear: Int32 = 137
  static let trimPreview: Int32 = 138
  static let trimKeyframe: Int32 = 139
  static let trimReencode: Int32 = 140
  static let clipTools: Int32 = 144
  static let clipSplit: Int32 = 145
  static let trimRemoveMiddle: Int32 = 146
  static let editWorkspace: Int32 = 150  // after PR 15's copy_path, copy_flattened, share
}

@MainActor
final class EditStore: ObservableObject {
  static let shared = EditStore()

  @Published private(set) var open = false
  @Published private(set) var tab: EditTab = .crop
  @Published private(set) var subject: Int32 = 0  // 0 none, 1 still, 2 clip
  @Published private(set) var galleryVisible = false
  /// A PDF or DOCX: the bar offers "Open in <app>" instead (OpenInStore).
  @Published private(set) var document = false
  /// Slice 5: the Enhance add-on is loaded; the strip offers Enhance….
  @Published private(set) var enhance = false
  /// The wand menu's state (mv_edit_view.wand bits).
  @Published private(set) var wand: Int32 = 0
  var brushOn: Bool { wand & 1 != 0 }
  var brushed: Bool { wand & 2 != 0 }
  var brushFull: Bool { wand & 4 != 0 }
  var offersRestyle: Bool { wand & 8 != 0 }
  var eyesBusy: Bool { wand & 16 != 0 }
  // docs/plans/edit-refresh.md S2 (shell/adjust_pane.h kWand*).
  var offersExtend: Bool { wand & 32 != 0 }
  var offersDetail: Bool { wand & 64 != 0 }
  var facesReady: Bool { wand & 128 != 0 }
  var autoOn: Bool { wand & 256 != 0 }
  var autoBusy: Bool { wand & 512 != 0 }
  /// The header's chip: 1 a Photos-library photo, 2 fetching its original.
  @Published private(set) var chips: Int32 = 0
  /// The inspector is folded to the rail.
  @Published private(set) var collapsed = false
  @Published private(set) var cropActive = false
  @Published private(set) var aspect: CropAspect = .free
  @Published private(set) var portrait = false
  @Published var straighten: Double = 0
  @Published private(set) var editCount = 0
  @Published private(set) var showOriginal = false
  @Published private(set) var cropWidth = 0
  @Published private(set) var cropHeight = 0
  @Published private(set) var name = ""
  /// While the straighten slider is held, it wins over the polled angle.
  var draggingStraighten = false
  /// docs/plans/edit-refresh.md S3: the canvas tool's context bar (ToolBarView),
  /// the host's shell::tool_bar. The size slider wins over the polled size
  /// while it is held.
  @Published private(set) var toolBar = ToolBarState()
  var draggingToolSize = false

  var isClip: Bool { subject == 2 }
  var canEdit: Bool { subject != 0 }
  /// The bar button shows only over media: not on the empty start, not over the gallery.
  var showsBarButton: Bool { open || (canEdit && !galleryVisible) }
  var title: String { isClip ? "Edit video" : "Edit image" }
  /// The rail's order (shell/edit_workspace.cpp kStillTabs).
  var tabs: [EditTab] { isClip ? [.trim, .jobs] : [.crop, .colour, .enhance, .info] }

  private var generation: UInt64 = .max

  private init() {
    // Issue #178: polled when the main run loop has run (ChromePulse), not on a timer.
    ChromePulse.shared.add { [weak self] in
      self?.poll()
      return false
    }
  }

  private func poll() {
    let g = mv_chrome_edit_generation()
    // The subject follows the item even while closed (the command bar's label).
    var v = mv_edit_view()
    guard mv_chrome_edit_view(&v) else { return }
    if v.subject != subject { subject = v.subject }
    if (v.document != 0) != document { document = v.document != 0 }
    if (v.enhance != 0) != enhance { enhance = v.enhance != 0 }
    if v.wand != wand { wand = v.wand }
    if v.chips != chips { chips = v.chips }
    if (v.collapsed != 0) != collapsed { collapsed = v.collapsed != 0 }
    if document { OpenInStore.shared.follow() }
    let gallery = mv_chrome_gallery_visible()
    if gallery != galleryVisible { galleryVisible = gallery }
    guard g != generation else { return }
    generation = g
    open = v.open != 0
    tab = EditTab(rawValue: v.tab) ?? .crop
    cropActive = v.crop_active != 0
    aspect = CropAspect(rawValue: v.aspect) ?? .free
    portrait = v.portrait != 0
    if !draggingStraighten { straighten = Double(v.straighten) }
    editCount = Int(v.edit_count)
    showOriginal = v.show_original != 0
    cropWidth = Int(v.crop_width)
    cropHeight = Int(v.crop_height)
    let need = mv_chrome_edit_name(nil, 0)
    var buf = [CChar](repeating: 0, count: Int(max(need, 1)))
    _ = mv_chrome_edit_name(&buf, Int32(buf.count))
    name = String(cString: buf)
    var t = mv_tool_bar_view()
    if mv_chrome_tool_bar(&t) {
      let next = ToolBarState(t)
      if next != toolBar { toolBar = next }
    }
  }

  func run(_ command: Int32) { mv_chrome_run_command(command) }
  func toggle() { run(EditCommand.editWorkspace) }
  func select(_ t: EditTab) { mv_chrome_edit_select_tab(t.rawValue) }
  /// The rail's icon: select, fold (the active one) or unfold.
  func rail(_ t: EditTab) { mv_chrome_edit_rail(t.rawValue) }
  /// The rail's and the Enhance tab's tools.
  enum Tool: Int32 { case remove = 0, redEye = 1, faces = 2, extend = 3 }
  func tool(_ t: Tool) { mv_chrome_edit_tool(t.rawValue) }
  /// ✦ Auto: one undo step; again right after takes it back.
  func autoEnhance() { mv_chrome_edit_auto() }
  func close() { mv_chrome_edit_close() }
  func setAspect(_ a: CropAspect, portrait p: Bool) { mv_chrome_edit_set_aspect(a.rawValue, p ? 1 : 0) }
  func setStraighten(_ degrees: Double) {
    straighten = degrees
    mv_chrome_edit_set_straighten(Float(degrees))
  }
  func cancelCrop() { mv_chrome_edit_cancel_crop() }
  func setShowOriginal(_ on: Bool) { mv_chrome_edit_show_original(on ? 1 : 0) }
  func saveCopy() { mv_chrome_edit_save_copy() }
  func enhancePhoto() { mv_chrome_edit_enhance() }
  /// The wand menu: Enhance's sheet started on Restyle with words.
  func restylePhoto() { mv_chrome_edit_enhance_command("restyle") }
  /// docs/plans/edit-refresh.md S5: an Enhance tool in the Edit pane ("remove",
  /// "restyle", "detail", "extend"); `run` starts it. Without the docked panel,
  /// Enhance's sheet opens instead.
  func enhanceTool(_ photo: String, run: Bool = false) { mv_chrome_edit_enhance_tool(photo, run ? 1 : 0) }
  /// The docked panel's result row: Show (`keep` false) or Keep editing.
  func enhanceResult(path: String, keep: Bool) { mv_chrome_edit_enhance_result(path, keep ? 1 : 0) }
  /// The Remove brush: on (at `size`, −100…100 about the default) or off.
  func setBrush(_ on: Bool, size: Double) { mv_chrome_adjust_pick(on ? 3 : 0, Float(size)) }
  /// 0 the last stroke off, 1 all of them, 2 Remove (Enhance's sheet).
  func brushAction(_ action: Int32) { mv_chrome_adjust_brush(action) }
  /// S3: the context bar's buttons, size slider and Paint / Erase toggle.
  func toolAction(_ a: ToolBarState.Action) { mv_chrome_tool_action(a.rawValue) }
  func setToolSize(_ px: Double) { mv_chrome_tool_set_size(Float(px)) }
  func setToolErase(_ on: Bool) { mv_chrome_tool_set_erase(on ? 1 : 0) }
  func openClosedEyes() { mv_chrome_edit_faces(0) }
  func improveFaces() { mv_chrome_edit_faces(1) }
}

/// docs/plans/edit-refresh.md S3: what the context bar below the photo shows
/// (shell::tool_bar through mv_tool_bar_view).
struct ToolBarState: Equatable {
  /// shell::canvas_tool.
  enum Tool: Int32 { case none = 0, brush = 1, redEye = 2, whiteBalance = 3, faces = 4, extend = 5 }
  /// shell::tool_bar_action.
  enum Action: Int32 { case done = 0, undo = 1, redo = 2, clear = 3, apply = 4, findEyes = 5 }

  var visible = false
  var tool: Tool = .none
  var sizePx: Double = 40
  var erase = false
  var strokes = 0
  var spots = 0
  var canApply = false
  var busy = false
  var progress = 0
  var full = false
  var canRedo = false
  var canUndo = false  // S4a: a stroke or a Clear to undo
  var eyes = 0

  init() {}
  init(_ v: mv_tool_bar_view) {
    visible = v.visible != 0
    tool = Tool(rawValue: v.tool) ?? .none
    sizePx = Double(v.size_px)
    erase = v.erase != 0
    strokes = Int(v.strokes)
    spots = Int(v.spots)
    canApply = v.can_apply != 0
    busy = v.busy != 0
    progress = Int(v.progress_permille)
    full = v.full != 0
    canRedo = v.can_redo != 0
    canUndo = v.can_undo != 0
    eyes = Int(v.eyes)
  }
}
