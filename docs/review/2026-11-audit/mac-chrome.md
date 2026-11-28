# macOS chrome audit (src.swift + src/shell Mac glue)

Read-only audit, 2026-11-25, branch `claude/project-audit-performance-180749`. Paths are relative to the repo root.
"Confirmed" means I read the code path quoted. "Suspected" means the code reads as described, but I did not reproduce the effect. I left out anything already filed and cite the issue instead.

Coverage note: ~36k lines of Swift plus ~26k lines of Mac shell. I read the stores, the bridge glue and the hot views closely: ChromeHost, FolderStore, Gallery/Filmstrip, SlotStore/ScreenView thumbnails, MetadataStore, VideoEditorStore, Driftwire, EnhanceRun, Settings→Storage, AddonNotices, ShareMenu, ChromePulse, AITable, ImportModel/ImportView, slots_mac.mm, quicklook_mac.mm, spotlight_importer_mac.mm, and the menu bar in main_mac.mm. I sampled the rest by pattern search: sync I/O, timers, observers, colours, accessibility and formatters. Large views I did **not** read line by line: AdjustView, PersonSheet, ManagementView, SearchPanel, LocalSearchView body, PhotosBackupView body, and the ScreenView bulk.

---

## A) Screen inventory

| Surface | Files | How it opens | What it does |
|---|---|---|---|
| Welcome card | drawn on the Metal canvas by `src/shell/main_mac.mm` (~776-784, 16860-16980, `welcome_layout.h`) | empty window / launch | Open, recent folders (pin/remove via right-click), paired servers, iCloud Photos row |
| Command bar (toolbar) | `CommandBarView.swift`, `SlotItems.swift` (add-on bar items/status), `AddonsView.swift` (AddonBarItems), About flyout in CommandBarView | always | Open/View/Edit flyouts, breadcrumb/list title, share button, update pill, Local search pill, add-on buttons |
| Filmstrip | `FilmstripView.swift` | View → Filmstrip, `T` | lazy strip of thumbnails, marks, play badges |
| Gallery | `GalleryView.swift`, `FileSearch.swift`, `FolderDrop.swift`, `FileDrag.swift` | View → Gallery, `G`; `/` searches folders, ⌘F (no Local search) opens file search | folder tiles plus photo grid, shift-click range, drag out/in, add-on badges/context menu |
| Page sidebar | `PageSidebarView.swift` | View → Page Sidebar | page thumbnails for PDF/DOCX/PPTX |
| Folder tree / Places | `FolderTreeView.swift` | View → Folder Tree | tree, places, volumes, paired servers, Photos Library |
| Metadata pane | `MetadataView.swift`, `MetadataStore.swift`, `FacesPanelView.swift` | View → Metadata, `I` | Summary / All tags / Streams, edit tags, date, location, labels; add-on info sections |
| Edit workspace | `EditView.swift`, `EditRailView.swift`, `EditStore.swift`, `AdjustView.swift`, `AdjustStore.swift`, `ToolBarView.swift`, `EnhanceHubView.swift`, `EnhanceRun.swift` | Edit menu, Return | crop/straighten, adjust, canvas tools, Enhance tab |
| Export / Clip tools sheets | `ExportView.swift`, `ClipToolsView.swift` | ⌘S / clip commands | save a copy; trim/split/remux |
| Jobs pane | `JobsView.swift`, `JobsStore.swift` | jobs command | background job list |
| Transport | `TransportView.swift`, `VideoStore.swift` | on clips | play, scrub, rate, volume |
| Video Editor window | `VideoEditorView.swift` | Edit → Trim Video / editor command | timeline, pieces, look, sound, speed |
| Help (keyboard shortcuts) | `HelpView.swift` | `?`, Help → Keyboard Shortcuts | live key table |
| Settings (all pages) | `SettingsView.swift` (Filmstrip, Browsing, Viewer, File types, Appearance, Updates, Storage, Photos Library, Add-ons, Keyboard), `ThemePicker.swift`, `ThemeField.swift`, `AddonListView.swift`, `AddonSettingsPage.swift`, `OpenAddons.swift`, `AddonsView.swift` (Import page), `PhoneAccess.swift`, `Driftwire.swift` (offer) | ⌘, | prefs, themes, add-on list/settings/install/remove, key remap |
| About | app menu (`showAbout:` main_mac.mm ~8508) plus command-bar flyout (CommandBarView ~100-118) | App menu / bar | version, licences |
| Notices | `HintNoticeView.swift`, `ResumeNoticeView.swift`, `RestartNoticeView.swift`, `NoticeStore.swift`, `ProgressLine.swift`, `AddonNotices.swift` (UN notifications plus menu-bar status items) | automatic | hints, resume indexing, restart to update, add-on keep-alive menus (Driftwire, VPN, host) |
| Add-on screens (Driftwire window, VPN, Places, Phone access card, Enhance sheet) | `ScreenView.swift`, `SlotStore.swift` | add-on commands / status / share | add-on view trees drawn with SwiftUI |
| Share | `ShareMenu.swift` | ⌘⇧S, bar button, right-click | Driftwire first, then NSSharingServicePicker |
| Local search window, People, index transfer, model tiers | `AIChrome/*` (SearchPanel, SearchModel, PeopleView, PersonSheet, ManagementView, IndexTransferView, TierPicker) hosted by `LocalSearchView.swift` | ⌘F, path icon, Settings → Local search | search, people clustering/corrections, management |
| Photos library and backup | `PhotosBackupView.swift`, `src/shell/photos_items_mac.mm`, `photos_backup_mac.mm` | File → Open Photos Library; Settings | browse iCloud Photos, back up originals |
| Import, Duplicates, Check library | `ImportChrome/*` | ⌘⇧I (`open_import`, commands.h:281) | card import, duplicates, library check |
| Connect to a Server | native alert in main_mac.mm (`connectToServerFromMenu:` 8569) | File menu | pair with a desktop/host by code |
| Menus | `installMainMenu` main_mac.mm 8519-8691 | menu bar | App/File/Edit/View/Go/Window/Help |
| Finder integration | `src/shell/quicklook_mac.mm`, `spotlight_importer_mac.mm` | Finder | thumbnails, Spotlight attributes |

### Parity with Windows (`src.managed`)
The two chromes match closely. Most Windows files name their Mac twin and the reverse. Differences I found:
- **Mac only:** Photos Library browsing and backup (PhotoKit; Windows has 2 files mentioning it), Final Cut Pro search (`LocalSearchView.swift` finalCut, fcp_mac.mm), and menu-bar keep-alive items (Windows has tray, `HostTray.cs`). All are OS-specific and expected.
- **Windows only:**
  - The Settings "Default app" row (`IslandHost.Settings.cs` ~157). The Mac only has an app-menu item, and only in bundle builds (`main_mac.mm:8548-8552`). Related to #619.
  - The telemetry consent row in "Updates and privacy" (`IslandHost.Telemetry.cs`). This is by design: the Mac keeps no telemetry spool (`main_mac.mm:1917`). The section names differ ("Updates" on the Mac, `SettingsView.swift:582`), which is cosmetic.
- Known Windows-side gaps are already filed: #196, #204, #205, #399, #508.

---

## B) Issues by severity

### High

**B1. Marks are matched by file name, not by item. Add-on "files" actions and mark badges can include unmarked files.** Confirmed by reading; reachability suspected.
- `FolderStore.swift:119` keeps `markedNames: Set<String>`, filled at `:525` (`fresh.insert(names[i])`).
- `SlotItems.swift:46-47`: an add-on item with target `"files"` is sent `store.names.indices.filter { store.markedNames.contains(store.names[$0]) }`. Every listed item whose *name* equals a marked item's name goes to the add-on.
- `GalleryView.swift:208` and `FilmstripView.swift:26` draw MarkBadge by name too.
- When it bites: in a result list (Local search results, "Similar to…", a Places list) that spans folders, `IMG_0001.JPG` from two cards is common (`FolderStore.swift:20-21` says so for thumbnails, issue #177). Marking one marks and sends both.
- `dragFiles` (`:619-620`) uses `mv_chrome_is_marked(index)`, which is correct, so the two paths disagree.
- Fix: keep a `Set<Int>` of marked indices, or keys (`thumbKeys`), instead of names.

**B2. An add-on screen's `image` node reads and decodes its file on the main thread on every body pass.** Confirmed.
- `ScreenView.swift:707-708`: `ImageNode.body` calls `NSImage(contentsOf: url)` inline. Any re-render (hover, a publish of any screen, theme change) re-reads from disk.
- This breaks hard constraint 1 for chrome. The file is under the add-on's folder, which could be on a slow or network home directory.
- Fix: cache by URL plus mtime in SlotStore, and decode with ImageIO off main.

### Medium

**B3. Add-on thumbnail grids cause an invalidation storm and decode on main.** Confirmed.
- `ScreenView.swift:26-69`: `SlotThumbs` is one `ObservableObject` with `@Published images: [String: NSImage]`. Every `SlotThumbnail` observes it (`:65`).
- Each arriving thumbnail therefore re-renders every thumbnail cell in every open add-on screen (People/Places/Driftwire grids, up to "a 2,000-tile grid" per `:31`).
- `arrived` (`:54`) also does `NSImage(contentsOfFile:)` on main.
- FolderStore solved exactly this with per-item `ThumbSlot` plus off-main ImageIO decode (`FolderStore.swift:11-24, 670-678`). Reuse that pattern.
- Also: a failed path is never retried (`failed` at `:56`), and `order.removeFirst()` is O(n) per insert.

**B4. EnhanceRun republishes on every add-on screen change.** Confirmed by reading; cost suspected.
- `EnhanceRun.swift` subscribes to `SlotStore.shared.$screens` for all add-ons.
- `read()` assigns `cards = tree`, `revision = …`, `canDiscard`, `discardLabel` and `outdated` without an equality check. Every observer of `EnhanceRun.shared` (Edit pane Enhance tab, `EnhanceRunRow`) re-renders when any add-on posts a screen diff, for example Driftwire's transfer progress.
- Fix: guard on the Enhance screen's own revision before assigning.

**B5. The canvas (welcome card, recent folders, iCloud Photos row) is invisible to VoiceOver.** Confirmed by search.
- The welcome rows are drawn into the Metal view (`main_mac.mm:776-784, 16962-16980`).
- In main_mac.mm the only `isAccessibilityElement` override is the pane splitter (`:2899`). `MvMetalView` exposes no role, label or children.
- Keyboard reachability exists (⌘O, File → Open Recent, Open Photos Library), but a VoiceOver user cannot discover the welcome card's rows or the photo on screen.
- Fix: NSAccessibilityElement children for the welcome rows, and an image role/label for the canvas.

**B6. Gallery and filmstrip cells have no button role, selected state or "marked" state for VoiceOver.** Confirmed.
- `GalleryView.swift:204-235` and `FilmstripView.swift:24-37` are `onTapGesture` on plain stacks: no `.isButton`, no `.isSelected` for the current cell, no `accessibilityAction`.
- `MarkBadge.swift` has no label. VoiceOver reads the SF Symbol name, not "Marked".
- Compare `PageSidebarView.swift:231-233`, which does it right (`.accessibilityElement(children: .ignore)`, label, `[.isButton, .isSelected]`), and `FolderTreeView.swift:376`.
- Folder tiles (`GalleryView.swift:340-388`) have the same gap.

**B7. FolderStore's thumbnail eviction can evict the selected item, despite its comment.** Confirmed.
- `FolderStore.swift:690-694`: the comment says "Not the selected item", but the loop evicts `decoded.removeFirst()` unconditionally and drops it from `requested`.
- Because cells are keyed by index and get no fresh `onAppear` while on screen, an evicted cell that is still visible keeps its placeholder until it scrolls away and back. The selected item is often the oldest decode.
- Fix: skip `thumbKey(at: current.index)` (and visible cells) when evicting.

**B8. The Import grid invalidates the whole window per thumbnail and keeps thumbnails without a bound.** Confirmed. This is related to, but not the same item as, #194 (which covers per-click plan rebuilds and polling).
- `ImportModel.swift:172`: `@Published var thumbs: [Int: NSImage]` on the window's single model. Each arrival (`:435`) re-renders `ImportView` in full.
- Nothing evicts: a 5,000-file card keeps every NSImage.
- `NSImage(contentsOfFile:)` (`:431`) defers decode to first draw, which happens on main.
- Fix: decode a CGImage thumbnail off main, use per-tile slots, and an LRU.

### Low

**B9. Progress phase can reappear after an install finishes.** Suspected.
- `Driftwire.swift:101` posts each phase with `Task { @MainActor in self.phase = p }`. Completion runs `await MainActor.run { self.phase = nil … }` (`:110-112`).
- Unstructured tasks are not ordered against `MainActor.run`, so a late `.installing` can land after `phase = nil` and leave a stuck progress bar.
- The same shape appears at `AddonsView.swift:429` (and likely PhoneAccess). Fix: drop phases once `busy == false`, or tag them with a run id.

**B10. Theme and colour inconsistencies.** Confirmed.
- `Theme.swift:48`: `onAccent` is always white. An add-on theme with a light accent gives white-on-light text on every primary button (`ScreenView.swift:437`, FacesPanelView `:383`). Contrast impact is suspected and depends on the theme.
- `ScreenView.swift:422` uses `Color.red` for destructive in the info pane, while `:437` uses `.systemRed` in windows.
- Import window:
  - It uses `.green` / `.orange` / `.caption` / `.title3` and literal 8/14 spacing (`ImportView.swift:228-236, 272-306, 322`).
  - HostTheme exposes only colours and `radius` (`HostTheme.swift:45`), so Import ignores the theme's font, density and type scale that MVTheme applies everywhere else.
  - This is a Mac-side twin of #204.

**B11. Animations ignore Reduce Motion in a few places, while the rest of the chrome gates them.** Confirmed.
- `SettingsView.swift:459, 474` (Storage numeric transitions).
- `Driftwire.swift:165`.

**B12. The bridge has two string-copy contracts, and some callers ignore the length.** Confirmed.
- The two contracts:
  - `MvCopyOut` (`main_mac.mm:1629-1636`) truncates silently, possibly mid-UTF-8.
  - The slots `copy_out` (`slots_mac.mm:35-42`) writes nothing when the buffer is short.
- Callers that ignore the returned length:
  - `FolderStore.reloadListTitle` (`FolderStore.swift:297-299`): a remote key longer than 4095 bytes is compared truncated.
  - `VideoEditorStore` name (`VideoEditorView.swift:154-156`): 1 KB buffer.
- Fix: settle on one contract (return need, never truncate) and use the two-call pattern everywhere, as `readText` (`MetadataStore.swift:91-97`) already does.

**B13. Volume list can go stale.** Suspected.
- `FolderTreeView.swift` `reloadVolumes()` (~586-600) starts a new detached task per mount/unmount notification and assigns whichever finishes last.
- A quick mount followed by an unmount can leave a stale volume listed. Fix: add a generation guard, as `SlotItems.swift` Installed.reload does (`:463-476`).

**B14. Stores freeze while a modal runs.** Confirmed, partly by design.
- `ChromePulse.swift:44-49` polls in default mode only.
- `AddonNotices.chooseFiles` (`AddonNotices.swift:~320`) uses `panel.runModal()` from the status menu. Connect to a Server also uses a modal alert.
- While these are up, job and backup progress stops updating. Prefer `begin(completionHandler:)`, or a sheet.

**B15. Menu-bar discoverability.** Confirmed; not a key-reachability bug.
- Most menu items deliberately carry no key equivalent (`main_mac.mm:8580-8601, 8604-8606`), so the menu does not show the chords.
- Import (⌘⇧I), Export (⌘S), Local search (⌘F), Video Editor, Jobs and Driftwire have no menu-bar entry at all (search of main_mac.mm and addons_mac.mm).
- They are reachable from the keyboard via the command table (`commands.h:263, 281`).
- Consider a "Tools" or "Window" group, matching Windows if added there.

Checked and found fine:
- Text-field ⌘A/C/V/X/Z handling: `installTextEditKeys`, `main_mac.mm:17767-17818`.
- C callbacks hop to main before `assumeIsolated` (`slots_mac.mm:69-72, 230`; FolderStore trampolines).
- `AITable` guarded calls and struct_size checks (`AITable.swift:33-86`).
- Surface pixel copy size checks (`SlotStore.swift:555-590`).
- Editor pieces capacity (`main_mac.mm:2392-2400`).
- Quick Look image lifetime (`quicklook_mac.mm`, `CFBridgingRelease` before the block).
- Spotlight importer exception guard.
- Status-item handler lifetime (`AddonNotices.swift:210-250`).

---

## C) Improvements and optimisations

1. **Gallery body invalidation:**
   - `GalleryView` observes all of `FolderStore` (`GalleryView.swift:82`), so publishes that don't affect the grid rebuild the `GeometryReader` closure and its `ForEach` diffing: `updatePhase/Version/Notes` (`FolderStore.swift:146-152`), `remoteFolder`, `canGoBack/Forward`, `listTitle`.
   - Move the command-bar-only state into its own small ObservableObject (the CurrentItem pattern, issue #179).
2. **ChromePulse poll cost:**
   - `FolderStore.poll` (`FolderStore.swift:207-294`) makes ~15 bridge calls plus a 512-byte folder-query copy on *every* main run-loop pass, including every presented frame while a clip plays.
   - One "chrome generation" counter from the host would let it return after one call.
3. **Surface copies:**
   - `SlotStore.surfacePixels` (`SlotStore.swift:570-578`) copies up to 64 MB twice on main (`[UInt8]` then `Data(bytes)`) when a revision moves, inside a view body.
   - Copy straight into a `CFMutableData`, off main.
4. **Duplicated code:**
   - `FileDrag.swift` exists twice (MediaViewerChrome and AIChrome, 120 lines each).
   - `FolderNavModel.swift` is copied byte-for-byte into the test target (`Tests/MediaViewerChromeTests/FolderNavModel.swift`, identical by diff), which is a drift risk.
   - The two-call string/JSON readers are re-implemented in at least 6 places: `MetadataStore.readTable/readText`, `FolderStore.bridgeString/string`, `SlotStore.read`, `SlotTargets.path`, `AddonStore.readString`, `StorageStore.refresh`.
5. **Formatters per call:**
   - `ImportModel.dayLabel` builds two DateFormatters per day header on each replan (`ImportModel.swift:415-423`).
   - `PhotosBackupView.swift:390-403` does the same per row.
   - These are minor; use static formatters.
6. **Startup:** `LocalSearchStore` runs a 0.5 s repeating timer for the app's lifetime (`LocalSearchView.swift:142-147`), even with the pack never installed (`:197` throttles but still wakes). Fold it into ChromePulse's "busy" mechanism.
7. **Timers and tracking mode:** the 30 Hz editor playhead timer (`VideoEditorView.swift:112`) is in default mode only, so the playhead stalls during menu tracking or live resize. Add it to `.common`.
8. **UX roughness:**
   - Driftwire install progress and the Storage numbers animate even with Reduce Motion on (B11).
   - Settings has no "Default app" row on the Mac (see A).

---

## D) Test gaps

- **No Swift unit test imports `MediaViewerChrome`.** The target can't link the bridge (`Package.swift:29-32`). So FolderStore, SlotStore parsing, MetadataStore table parsing, ScreenView tree patching, EnhanceRun state mapping, ShareMenu file parsing and StorageStore words are all untested on the Mac.
  - Windows tests the equivalents: `ScreenTreePatchTests`, `InstallWordsTests`, `ProgressWordsTests`, `RestartWordsTests`, `DropWordsTests`, `KeptSettingsPageTests`.
  - A stub `MVChromeBridge` test target (C stubs returning fixtures) would unlock them. Umbrella issue: #153.
- Tests that would have caught B1, B3 and B7:
  - Marks with duplicate names in a result list should give the right badges and the right "files" payload.
  - A SlotThumbs arrival should re-render one cell.
  - Eviction should keep the current item.
- Bridge contract tests for `MvCopyOut` versus slots `copy_out` truncation (B12) belong in tests/ (C++). #151 covers unexercised ABI functions in general.
- Import: `ImportModelTests.swift` (30 lines) covers almost nothing. Nothing covers thumbnail staleness across ⌃Tab (`ImportModel.swift:432-435`, issue #193 logic) or `dayLabel`.
- AIChrome has the best coverage (SearchModel, Tier, PeopleCorrections, StatusLine). Missing: `AITable.close` racing `guarded` (`AITable.swift:53-86`).
- No UI smoke test exists for the Mac (#153). Quick Look and Spotlight extensions have no automated test of their entry points (`quicklook_mac.mm` provider, `get_metadata`).
