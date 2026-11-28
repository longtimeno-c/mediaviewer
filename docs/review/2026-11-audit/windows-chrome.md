# Windows chrome audit (src.managed): read-only

Date: 2026-11-25. Branch `claude/project-audit-performance-180749` at a232bda4.

## How this was done, and what it did not cover

- **Swept with grep across all of src.managed:** `async void` and async lambdas, `.Result`/`.Wait`/`GetResult`, sync `File.*`/`Directory.*` calls, timers, `XamlReader` strings, hard-coded colours, literal font sizes, `AutomationProperties.SetName`, image decode sites, and banned controls (from the list in `tools/check-winui-controls.ps1`).
- **Read in full or at the hotspots:** IslandHost.cs (attach/detach, focus, font, Guarded), CrashCapture.cs, Telemetry.cs, TextRecognition.cs, ThemePack.cs, Storage.cs, Tiles.cs, Settings.cs (tabs), Screens.cs (image/thumbnail nodes, folder picker), Slots.cs (SlotImagePath), OpenAddons.cs, Share.cs, Video.cs (timer/SMTC), InstallFlow.cs, FileSearch.cs (tag watch), the Interop NativeMethods.cs, Updater UpdateManifest.Evaluate, ImportWindow.cs (layout, thumbs, preset panel), ImportChrome.cs, SearchWindow.cs (lifecycle), Ai.Chrome Look.cs (fades), Shared/FakeInput.cs (caret), and the Chrome.Tests project.
- **Not covered:** the parallel per-area sub-audits could not be started (the tool-permission check timed out), so the largest files were only sampled through the greps above, not read line by line. These are Panels, Adjust, Edit, Gallery, LocalSearch, VideoEditor, Clip, PeopleGrid, PersonSheet, ManagePanel, and Interop Ai/Addons/Slots. Treat the absence of findings there as "not audited", not "clean".
- **Already filed, so skipped:** #508 (Driftwire/add-on screens rough), #204 (add-on window tokens/backdrop), #196, #399, #205, #575, #576, #611, #525, #153/#155 (chrome tests in general), #151.

---

## A) Screen inventory

| Surface | File(s) | How it opens | What it does |
|---|---|---|---|
| Command bar (top) | IslandHost.cs `BuildChrome` (1552), ToolBar.cs, Buttons.cs, FileMenu.cs | Always present; Tab from the canvas (`NavigateFocus`, 693) | Open/recent folders, view/sort/kind menus, Settings, `?` help, busy bar |
| Busy bar | IslandHost.cs 1258–1355 | Delayed on a decode that misses the cache (`SetBusy`) | Indeterminate 2 px slide (a stand-in for the banned ProgressBar) |
| Welcome / empty canvas ground | Theme.cs / ThemeField.cs, `ThemeMotion` (IslandHost.cs 509) | No file open | Themed animated ground; input forwarded to native |
| Telemetry consent | Telemetry.cs 56–129 | First run, while the Asked bit is clear | Two equal-weight answers, defaults to off |
| Filmstrip | Filmstrip.cs, Tiles.cs | Browse mode | Virtualised ItemsRepeater of thumbs; shares `Items` with the gallery |
| Gallery | Gallery.cs, GalleryRange.cs, Tiles.cs | Gallery mode key / bar | Virtualised grid of files plus folder tiles with covers; range selection |
| Page sidebar | PageSidebar.cs | Multi-page TIFF/PDF/DOCX; Ctrl+PageUp/PageDown | Page thumbnails (`mv_folder_page_thumbs`) |
| Transport / video controls | Video.cs, IslandHost.cs `BuildSpeed` (837) | A clip is open | Play/scrub, speed menu (Q/E), audio flyout (Shift+M), SMTC media keys |
| Clip tools / trim / jobs | Clip.cs | Ctrl+T trim, Ctrl+S tools flyout, Ctrl+B split, Ctrl+J jobs pane | Lossless trim/split/remux; job queue |
| Video Editor add-on surface | VideoEditor.cs | Edit workspace on a clip | Timeline, waveform, export |
| Metadata pane | Panels.cs | `I`; Ctrl+I to edit the comment | Summary card, full tree, colour labels, location/comment editing, slot info sections |
| Folder tree pane | Panels.cs (`_tree`) | Toggle in the view menu | Lazy directory tree |
| Adjust pane | Adjust.cs | Shift+A | Tone/colour sliders, histogram, clipping marks |
| Edit workspace + icon rail | Edit.cs, Rail.cs | Enter on a still/clip | Crop (Shift+C), rotate/flip, Auto, Enhance entry |
| Enhance dock / hub | EnhanceDock.cs, EnhanceHub.cs, Enhance.cs, Glow.cs | Edit strip "Enhance…" (add-on) | One-click enhance card, options, glow while working |
| Text recognition (Live Text) | TextRecognition.cs | Native calls `RecognizeText` on a pool worker | Windows.Media.Ocr; word boxes returned to native |
| Find in folder / file search | FileSearch.cs | `/` typeahead, Ctrl+F | Name and tag filter with a debounce and a tag-generation watch |
| Local search panel/window | LocalSearch.cs, Ai.Chrome SearchWindow.cs | Ctrl+F (AI pack), Ctrl+Shift+F similar, N/Shift+N matches | Semantic search results grid, indexing status pill |
| People screens | Faces.cs, Ai.Chrome PeopleGrid/PersonSheet/UnknownFacesSheet/PersonPicker/FaceTile | From Local search | Face clusters, naming, review, cover choice |
| AI manage panel / model picker | Ai.Chrome ManagePanel(.Transfer).cs, ModelPicker.cs, StatusPill.cs | Local search settings | Indexed folders, models, transfer |
| Settings: General | Settings.cs 90–163 (Filmstrip, Browsing, Viewer, File types, Appearance, Updates and privacy), Storage.cs, Telemetry.cs | Ctrl+, or the Settings button | Every viewer preference; thumb-cache cap; telemetry switch |
| Settings: Keyboard shortcuts | Settings.cs (keys column, capture) | Settings tab 2 | Rebind keys with capture/cancel |
| Theme picker / theme packs | ThemePicker.cs, ThemePack.cs, BuiltinThemes.cs | Settings → Appearance | Built-in and add-on themes, cached to theme.json |
| Add-on list / settings / install | AddonList.cs, AddonSettings.cs, AddonSettingsScreen.cs, InstallFlow.cs, Addons.cs | Settings → add-ons; an import-card hint | Install/remove/update, the add-on's own settings screen |
| Open add-ons (from a file) | OpenAddons.cs | Drop / Open with an `.mvaddon` (`OfferAddon`) or "From a file…" | Inspect, then install on an explicit answer |
| Add-on described screens (panes/sheets/windows) | Screens.cs, ScreenLook.cs, ScreenReveal.cs, ScreenSelfTest.cs, Slots.cs | Add-on contributions | Renders the core-checked JSON tree as native controls |
| Driftwire | Driftwire.cs, Screens.cs route tints (594–599) | Share menu / add-on | LAN transfer UI (see #508, #497) |
| Phone access | PhoneAccess.cs | Settings slot | Install/offer for the phone-access add-on |
| Remote | Remote.cs | Settings | Remote-control rows, 2 s poll |
| Share | Share.cs | Ctrl+Shift+S | Windows share sheet plus a Driftwire entry |
| Notices | Notices.cs, HintNotice.cs, RestartNotice.cs, ResumeNotice.cs, AddonAlert.cs, Update.cs | Native/event driven | Corner cards: hints, restart-to-finish, resume indexing, add-on alerts, update bar |
| Progress | Progress.cs | Long jobs | Progress words with a 1 s tick |
| About | About.cs | Settings / About | Version, licences (opened off-thread) |
| AI badge | AiBadge.cs | Local search present | Badge identity (see #399) |
| Import window | Import.Chrome ImportWindow.cs, ImportChrome.cs, ImportLook.cs | Ctrl+Shift+I; card arrival | Sources, grouped GridView of the plan, presets, history |
| Find duplicates / Check library | DuplicatesWindow.cs, CheckLibraryWindow.cs | Import's tools menu | Content-hash duplicates to the Recycle Bin; library re-hash |
| Accessibility glue | Accessibility.cs, FocusOrder/FocusRing (IslandHost.cs 988–1030) | Tab/Shift+Tab across islands | Tab ring across XAML islands (#217) |

---

## B) Issues by severity

### High

**B1. Chrome click handlers swallow exceptions without a report.** Confirmed by reading.
- Location: IslandHost.cs:1165–1169.
- `Guarded` catches every exception and only calls `Debug.WriteLine`, which is compiled out of release builds. Every menu item (`Item`, 1225), every TextButton and every timer tick wrapped in `Guarded` therefore loses its bugs silently in production.
- `CrashCapture.Handled` (CrashCapture.cs:73) already exists for this, writes no message (so needs no scrubbing), and is used for add-on start failures.
- Suggest: call `CrashCapture.Handled("chrome:"+…, ex)` from `Guarded` (rate-limited), so release bugs leave a local, scrubbed trail. The same applies to `VideoEditor.cs:702 Guard` and the many bare `catch (Exception ex) { Debug.WriteLine(ex); }` at the native entry points (`Attach` 529, `Resize` 686, `ApplySettings` 768, and others).

**B2. Crash-report scrubbing misses filenames that have spaces or a non-media extension.** Confirmed by reading.
- Location: CrashCapture.cs:29–35, 53–65.
- `PathLike` only matches drive-letter or UNC paths. `MediaName` only matches an extension list without `\s` in the name. Paths that are not drive-anchored are fully scrubbed, but bare names are not:
  - "My Holiday Photo.jpg" becomes "My Holiday `<file>.jpg`", leaking "My Holiday".
  - The app also opens .pdf/.docx/.pptx/.mp3/.m4a/.wav/.flac (#585, #588), and these are not in the list, so "Tax return 2025.pdf" survives intact.
- These text reports are what #613 will show and save. Hard rule: "No user paths, filenames … in crash reports".
- Suggest: extend the extension list to every format the app opens, and scrub quoted strings ('…' and "…") in exception messages wholesale rather than trying to recognise filenames. `Scrub` has no tests (see D1).

### Medium

**B3. Theme cache writes race.** Confirmed by reading.
- Location: IslandHost.ThemePack.cs:160–183.
- `SaveTheme` starts an unsynchronised `Task.Run` per call. All of them write the same `ThemeFile + ".tmp"` and then `File.Move(..., overwrite:true)`.
- Two quick picks (arrowing through the theme picker, or `LoadCachedTheme` re-saving a stale built-in at 145 while the user picks) can:
  - make one writer fail with an IOException, which is silently caught; or
  - finish out of order.
- Either way the persisted selection can be an older theme than the one on screen, and the next launch reverts.
- Suggest: serialise the saves through a single-slot "latest wins" worker, or write a unique temp name and compare a sequence number before the move. Storage.cs:89–100 (thumb-cap writes) has the same pattern, with lower stakes.

**B4. An add-on can make the host open arbitrary paths, including UNC, to render thumbnails.** Suspected: the core side was read, the effect was not run.
- Location: Screens.cs:306 → `SlotThumb` 1275 → `LoadSlotThumb` 1316 `SlotNative.ThumbnailPath(path)`, with the core's check `src/addon/screen.cpp:140 file_path`.
- For `thumbnail` / `thumbnail_grid` nodes the core accepts any UTF-8 path of up to 4096 characters (no root/UNC check). So an add-on's screen can make the viewer read `\\host\share\x.jpg`. On Windows that is an SMB connection with the user's NTLM credentials.
- This contradicts the owner's call "no network ever" for add-ons (the script sandbox, plan/25).
- Suggest: reject `\\`, `//` and `\\?\UNC` paths in `file_path` for thumbnail nodes, or only allow paths the add-on was granted (marked files, chosen folders).
- Note: the chrome's own `SlotImagePath` (Slots.cs:327) only rejects `..`. An absolute `src` would pass `Path.Combine` unchanged. Today the core's `package_image` (screen.cpp:119) blocks it, but the chrome should not rely on that alone (defence in depth: also reject `Path.IsPathRooted`).

**B5. Import: the per-card binding checkboxes always start unchecked.** Confirmed by reading.
- Location: ImportWindow.cs:959–975.
- `bind` and `auto` are created without reading the card's current binding. `auto` is disabled until `bind` is clicked.
- For a card already set to "Import as soon as inserted":
  - the panel shows both boxes unchecked;
  - checking "Always use these settings" calls `BindCard(vol, preset, on:false)`, which silently turns auto-import off;
  - a `BindCard` failure is swallowed (969) with no message.
- Suggest: read the binding with the sources/presets JSON, set `IsChecked`/`IsEnabled` from it, and surface failures in the banner.

**B6. Import: async-void click lambdas can crash on unexpected JSON.** Confirmed by reading.
- Location: ImportWindow.cs:949–955 and 197 (both await `AddFolderSource`/`RefreshSources`); RefreshSources 1st try at ~`(json, presets) = await Task.Run(...)`.
- `RefreshSources` only catches `MediaViewerException` around the native call. The parse that follows (`all["presets"]!`, `GetProperty("total_bytes")`, `GetValue<string>()`) throws `KeyNotFoundException`, `InvalidOperationException` or `JsonException` on a schema drift between the add-on's native and managed halves (they version separately).
- Every caller is an `async (_, _) =>` lambda, i.e. async void, so any such throw is a process fail-fast. `CheckUnfinished` (~512) has the same shape.
- `PickFolder` (495–511) also calls `InitializeWithWindow.Initialize` outside its try.
- Suggest: one try/catch per handler, as `ShowHistory` (1261–1266) already does.

**B7. Ai.Chrome: the end of a fade can touch a closed window.** Suspected.
- Location: Look.cs:302–320, with SearchWindow.cs:788–812.
- `PanelOut` runs `done()` from a DispatcherQueueTimer tick with no try/catch. SearchWindow's `done` is `AppWindow.Hide(); Hidden?.Invoke();`.
- If `Dispose()` (which calls `Close()`, 811) runs within the 150 ms fade, for example an add-on unload or shutdown right after Esc, the tick touches `AppWindow` on a closed window. An exception escaping a timer tick is a fail-fast in this host.
- Also, `_fading` only loses the timer on the success path.
- Suggest: wrap `done()` in try/catch, and have `Dispose` stop pending fades.

**B8. FakeInput: the caret timer can outlive its control.** Suspected.
- Location: Shared/FakeInput.cs:561–583, 171.
- The blink timer is repeating and is only stopped from `LostFocus`. A focused FakeInput removed from the tree (a screen re-render through `ScreenTreePatch`, a sheet or island disposed) often gets no `LostFocus`. The timer keeps ticking every 530 ms and roots the control through `OnBlink`.
- The caret also ignores the system caret blink rate and "Show animations" off (`UISettings.AnimationsEnabled`; the chrome has `HostAnimationsEnabled()`), which is an accessibility gap.
- Suggest: stop on `Unloaded`; honour the animations setting (steady caret).

**B9. The error/status red is not a theme role, so it ignores high contrast.** Confirmed by reading.
- Locations:
  - Panels.cs:1068 (metadata field error note: `#E5484D` literal, `FontSize = 11` literal at ~1075);
  - Screens.cs:647–648 and 995–996 (done/failed badges, literal green/red/orange with `Colors.White` ink).
- `ChromeColour` (Theme.cs:18–44) has no Danger/Success role. In a contrast theme every other colour switches to system colours, but these stay fixed, which can fail contrast (for example white on #E67E22).
- Suggest: add `Danger`/`Success` roles, resolved from the theme, the system or a contrast highlight, as `ControlAccent` is.

**B10. Gallery text ignores the theme's type scale.** Confirmed by reading.
- Location: Gallery.cs:525, 638, 703, 1229 (`FontSize = 13 / 12`); Filmstrip.cs:367 (`10`); Update.cs:34 and 416 (`12`); Slots.cs:732, 823 and 909.
- PR 56 resolves every literal through `TypeSize()` (Theme.cs:82), and the rest of the chrome does. These sites don't, so a theme with a larger type scale enlarges the bar and Settings but not the gallery captions, update status or slot glyphs.
- Import's `XamlReader` templates hard-code `FontSize="15"/"12"` (ImportWindow.cs:189–190; CheckLibraryWindow.cs:163–166) while the code around them uses `Look.TypeSize`.

### Low

**B11. `OnTakeFocusRequested` has no try/catch.** Confirmed by reading. IslandHost.cs:955–979.
- It is a XAML event and calls `sender.SiteBridge.WindowId`, `GetAncestor` and `SetFocus`. Elsewhere the file insists that "an exception escaping a XAML event … is a fail-fast" (665–669).
- A source torn down mid-Tab (an island re-attach) could null `SiteBridge`.

**B12. `ShutdownXaml` leaves some repeating timers running.** Confirmed by reading.
- Location: IslandHost.cs:919–939.
- It stops `_busyDelay` and `_hintNoticeTimer` only. These are not stopped before `ShutdownQueue()`:
  - `_remoteTimer` (Remote.cs:65, repeating);
  - `_addonUpdateTimer` (InstallFlow.cs:182, re-arms itself);
  - `_gsTagWatch` (FileSearch.cs:684);
  - `_updateWaitTimer` (Update.cs:141);
  - `_restartWaitTimer` (RestartNotice.cs:287);
  - `_jobsTimer` (Clip.cs:640);
  - `_videoTimer`.
- Probably harmless once the queue is gone, but inconsistent with the stated teardown order.

**B13. Import thumbnails decode at full size.** Confirmed by reading.
- Location: ImportWindow.cs:714.
- `new BitmapImage(new Uri(path))` has no `DecodePixelWidth`, unlike every chrome tile (Tiles.cs:41, Gallery.cs:144, PageSidebar.cs:229, SearchWindow.cs:1730).
- On a 2,000-file card the realised tiles each hold the full disk-thumb bitmap.

**B14. Failed slot thumbnails are never retried.** Confirmed by reading. Screens.cs:1331–1334.
- A failed or absent thumbnail is cached as `null` until the 4,000-entry wipe, so a file that gains a thumbnail later (an import in progress) shows an empty tile for the rest of the session.

**B15. Settings tabs read as toggle buttons.** Confirmed by reading. Settings.cs:403–433, 289–343.
- The General / Keyboard segments are two `ToggleButton`s with `SetName` only. Narrator announces "toggle button, on" rather than a tab and its position.
- Suggest: `AutomationProperties.SetPositionInSet`/`SizeOfSet`, plus a `SetItemStatus("Selected")` as CheckItem does (1248).

**B16. Telemetry consent can be dismissed with no visible way back.** Confirmed by reading. Telemetry.cs:62–72.
- The Flyout is light-dismissable, and `_consentShown` stays true for the process.
- Dismissing leaves telemetry off (correct), but the screen won't reappear until the next launch. That is fine by design 13; noted only because the comment at 67–69 implies light-dismiss was considered.

**Checked and clean:**
- No banned control appears in code or in the `XamlReader` strings (Import/Duplicates/CheckLibrary build only Grid/StackPanel/TextBlock).
- `TextRecognition.cs:55 GetResult()` runs on a native pool worker, as documented (23–24).
- The `async void` methods (OpenAddons.cs:534, Screens.cs:2159, Share.cs:321, ImportWindow.cs:1261, FakeInput.cs:406) are all fully wrapped.
- Gallery, filmstrip, Local search, People and add-on grids are virtualised ItemsRepeaters, and BindTile/UnbindTile (Tiles.cs:62–81) prevents handler leaks.
- Interop NativeMethods are consistent `LibraryImport` cdecl with explicit UTF-8 and caller-owned buffers.
- The updater verifies the signature first, then the channel, rollback and blocklist, then SHA-256 and size (UpdateManifest.cs:251–305).

---

## C) Improvements and optimisations

1. **The update manifest has no freshness bound.** UpdateManifest.cs:251–284. A replayed old-but-valid manifest (same version as running) silently means "up to date" forever, which is a freeze attack. Adding an `expires`/max-age check on `released` would close it.
2. **Import rebuilds its whole preset panel on every toggle.** `BuildPresetPanel()` runs from 871, 880, 920, 990 and 996, which loses keyboard focus each time (a keyboard-only user is thrown back to the start after every switch) and re-creates every control. Updating the changed row, or restoring focus to the toggled control, would fix both.
3. **Repeated per-tile colour objects.** Screens.cs route tints (594–731) allocate a new `SolidColorBrush` per element and per render. Caching per tint matters on Driftwire's live re-renders.
4. **Startup.** `RegisterUiFont` → `SessionFontCopy` (IslandHost.cs:1092–1126) runs about 5 stats on the UI thread at every Attach, plus on first install a file copy and a directory sweep. `LoadCachedTheme` (ThemePack.cs:125) is a deliberate sync read. Both are small, but they are the only UI-thread disk I/O on the startup path, so they are worth one measurement against rule 1.
5. **Duplicated code worth sharing.** There are three copies of `Template(string)`/`XamlReader` grid roots (ImportWindow.cs:161, DuplicatesWindow.cs:226, CheckLibraryWindow.cs:90/163) and two `Guarded`/`Guard` helpers. The pattern `Task.Run(() => { try { write } catch (IOException) {} })` appears in ThemePack, Storage and Addons:983. One `SettingsFile.WriteLatest(path, text)` helper would fix B3 everywhere.
6. **Polling that could be event-driven.** `_remoteTimer` 2 s (Remote.cs:65), Remote's poll (215), the tag watch at 300 ms (FileSearch.cs:684) and SearchWindow `_poll` could ride completion events. The tag watch already stops itself, so this is low priority.
7. **Hard-coded column widths in Import.** ImportWindow.cs:174–176 uses 240 / * / 320 DIP columns that don't scale with density or type scale, so at 150% text scaling with a large type theme the right column truncates the destination. The path has a tooltip (906); the preset toggles don't.

---

## D) Test gaps (MediaViewer.Chrome.Tests)

The test project compiles only `Shared/*Words.cs`, GlowMath, SpringLatch, KeyUpLetters, ScreenTreePatch and ExceptionFrames, plus source-text checks of the command tables (csproj `Compile Include` lines; SourceText.cs). These pure, privacy- or correctness-critical pieces have no tests:

1. **`CrashCapture.Scrub`** (CrashCapture.cs:53). This is the rule-6 guarantee. Cases: drive paths, UNC, `\\?\`, names with spaces, document and audio extensions, username/machine name, quoted names in IOException messages. It would have caught B2. It is `internal static`, so link the file or add `InternalsVisibleTo`.
2. **FakeInput editing logic** (Shared/FakeInput.cs): caret, selection, word-jump and paste-trim. It is the only text entry in the whole chrome, and it is Shared, so it can be linked like the others, with logic only (no XAML).
3. **Theme parsing and the built-in refresh** (ThemePack.cs `ParseTheme`, `LoadCachedTheme`'s stale-built-in rule) and the type/density scaling (`TypeSize`/`Space`, Theme.cs:75–84).
4. **`SpeedLabel` / ladder parity with native `kRateLadder`** (IslandHost.cs:821–829). A source-text test like CommandIdTests would keep the two ladders from drifting.
5. **Add-on path guards** (`SlotImagePath`, Slots.cs:327): absolute, rooted and `..` inputs. This pairs with B4.
6. **Updater:** `Evaluate` is exercised in MediaViewer.Updater.Tests/Program.cs (a console harness, not xUnit, so it does not run under `dotnet test` with the chrome tests). Worth confirming that CI runs it.
