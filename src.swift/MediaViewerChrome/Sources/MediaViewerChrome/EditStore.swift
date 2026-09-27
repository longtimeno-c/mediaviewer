// SPDX-License-Identifier: GPL-3.0-or-later
// PR 29 (plan/20): the state the Edit workspace's strip and panes read. It
// mirrors the host's shell::edit_workspace and the crop draft, re-read only
// when the host's edit generation moves -- the AdjustStore shape.
import AppKit
import Combine
import MVChromeBridge

/// shell::edit_tab and shell::crop_aspect values (main_mac.mm static_asserts them).
enum EditTab: Int32, CaseIterable {
  case crop = 0, colour = 1, info = 2, trim = 3, jobs = 4

  var label: String {
    switch self {
    case .crop: return "Crop"
    case .colour: return "Colour"
    case .info: return "Info"
    case .trim: return "Trim"
    case .jobs: return "Jobs"
    }
  }

  /// The key that opens this tab, for the label and the accessibility hint.
  var key: String {
    switch self {
    case .crop: return "⇧C"
    case .colour: return "⇧A"
    case .info: return "I"
    case .trim: return "⌘T"
    case .jobs: return "⌘J"
    }
  }
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

  var isClip: Bool { subject == 2 }
  var canEdit: Bool { subject != 0 }
  var title: String { isClip ? "Edit video" : "Edit image" }
  var tabs: [EditTab] { isClip ? [.trim, .jobs] : [.crop, .colour, .info] }

  private var generation: UInt64 = .max
  private var timer: Timer?

  private init() {
    timer = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
      MainActor.assumeIsolated { self?.poll() }
    }
    poll()
  }

  private func poll() {
    let g = mv_chrome_edit_generation()
    // The subject follows the item even while closed (the command bar's label).
    var v = mv_edit_view()
    guard mv_chrome_edit_view(&v) else { return }
    if v.subject != subject { subject = v.subject }
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
  }

  func run(_ command: Int32) { mv_chrome_run_command(command) }
  func toggle() { run(EditCommand.editWorkspace) }
  func select(_ t: EditTab) { mv_chrome_edit_select_tab(t.rawValue) }
  func close() { mv_chrome_edit_close() }
  func setAspect(_ a: CropAspect, portrait p: Bool) { mv_chrome_edit_set_aspect(a.rawValue, p ? 1 : 0) }
  func setStraighten(_ degrees: Double) {
    straighten = degrees
    mv_chrome_edit_set_straighten(Float(degrees))
  }
  func cancelCrop() { mv_chrome_edit_cancel_crop() }
  func setShowOriginal(_ on: Bool) { mv_chrome_edit_show_original(on ? 1 : 0) }
  func saveCopy() { mv_chrome_edit_save_copy() }
}
