# Windows chrome audit, part 2

Read-only. Paths are relative to `src.managed/`. I read every file in scope in full: `IslandHost.{Panels,Adjust,Edit,Gallery,LocalSearch,Clip,VideoEditor}.cs`, `MediaViewer.Ai.Chrome/{PeopleGrid,PersonSheet,ManagePanel}.cs`, and `MediaViewer.Interop/{Ai,Addons,Slots,Clip}.cs`. To check some claims I also read a few helpers and native call sites (listed at the end).

I left out everything already reported (`Guarded`, `CrashCapture.Scrub`, the `SaveTheme` race, Panels.cs:1068 colours, Gallery.cs:525 type scale). None of the findings below duplicates an open issue. The nearest issues are #356, which covers the 128 KB parked-text limit, and #576, which covers UI-thread path probing elsewhere.

How each finding is marked:
- **Confirmed by reading**: the code path is unambiguous in the files listed.
- **Suspected**: it depends on runtime behaviour or on a native contract I did not verify.

---

## Issues by severity

### High

**H1. Gallery zoom (Ctrl+wheel / `ScaleGallery`) leaves the thumbnails already on screen at their old size.** Confirmed by reading.
- Where: `IslandHost.Gallery.cs:132-166` (`ResizeRealisedTiles`), with the factories at `:1218-1254` and `:1331-1379`.
- What `ResizeRealisedTiles` assumes: `col.Children[0]` is the tile's `Image`. The guard `if (box is Image image && image.Source is BitmapImage bitmap) bitmap.DecodePixelWidth = ...` (line 143) relies on that.
- What is actually there: `col.Children[0]` is the `Grid` that `ThumbBox` returns (`IslandHost.Tiles.cs:89-111`). The `Image` sits under it and has its own fixed `Width`/`Height = GalleryTile` from creation time (Gallery.cs:1220-1221).
- Effect on photo tiles: the outer box grows or shrinks, but the picture stays at the old size until the repeater recycles the tile. Growing shows it small and centred; shrinking crops it. `DecodePixelWidth` is never raised, so zoomed-in tiles stay at the old decode resolution.
- Effect on folder tiles: the same happens to `mosaic`, `placeholder`, `box` and `overlay`, which all have fixed old sizes (lines 1336, 1346, 1358, 1379).
- Fix: keep references to the inner elements (for example in the tile's `Tag`) and resize them, or bind their size to the box (`Stretch`, no explicit `Width`).

**H2. While a folder is indexing, every row in Settings → Local search → Indexed folders is rebuilt about once a second, so keyboard focus is lost.** Confirmed by reading.
- Where: `ManagePanel.cs:1042-1164` (`ShowRoots`).
- The comment at line 1045 says unchanged rows are left alone so a button keeps focus.
- In fact the guard (line 1046) compares whole `RootRow` records, including `Done`, `Assets`, `Bytes`, `Scanning` and `DocumentsDone`. While any folder indexes, those change on each `AI_ROOTS` event, and then `_roots.Children.Clear()` rebuilds every row.
- Effect: a keyboard or Narrator user on "Pause", "Rescan", "Remove…" or the "Videos: … ▾" menu of any folder loses focus each time a count moves. That makes the folder list close to unusable from the keyboard during indexing.
- Fix: diff per row and update the text and bar in place, as `ShowFetch` and `RefreshIndexRow` already do.

**H3. Dragging a piece edge in the Video Editor rebuilds the whole timeline on every pointer move.** Confirmed by reading the chrome side; suspected cost.
- The chain:
  - `IslandHost.VideoEditor.cs:552-561`: each `PointerMoved` sends `EditorTrimTo` with no coalescing.
  - Native `editor_trim_to` (`src/shell/main.cpp:7329-7335`) calls `push_editor_view(app, true)`, which brings a new generation.
  - `SetVideoEditorView` (`:259-299`) then calls `RenderEditor(full: true)`, which runs `DrawEditorTrack` (`:1082-1182`).
- What `DrawEditorTrack` does each time:
  - `track.Children.Clear()`.
  - One new `Image` per thumbnail tile for every piece.
  - A `w/2`-point `Polygon` per piece for the waveform.
  - Ruler labels and wash layers.
  - `EditorFade` storyboards.
- Effect: at mouse rates of 125–1000 Hz this is a visual-tree rebuild per input event on the UI thread. That conflicts with the "pan/scrub stays smooth" rule.
- Fix: coalesce `EditorTrimTo` to one per frame (`CompositionTarget.Rendering`, as `QueueAdjustRedraw` does), and update only the moved piece's geometry while dragging.

### Medium

**M1. Each Adjust slider tick rebuilds the curve and histogram canvases and replaces several buttons' content.** Confirmed by reading; that native pushes once per value is suspected.
- The chain:
  - `IslandHost.Adjust.cs:907-922`: each `ValueChanged` sends its command at once.
  - Native `push_adjust_pane` (`main.cpp:2844-2853`, called from the command handlers) pushes `SetAdjustView`.
  - `SetAdjustView` (`:295-341`) calls `RenderAdjust` (`:1330-1428`).
- What `RenderAdjust` does each push:
  - Walks all 56 sliders.
  - `RenderCurve`: `Children.Clear()`, about 20 shapes, and a 97-sample `EvaluateCurve` that allocates two arrays per sample.
  - `RenderHistogram`: `Children.Clear()` and 4 polygons.
  - Builds new `ButtonLabel` TextBlocks for the White balance, Red-eye and Brush buttons and for every section's Auto (`:1377-1421`), even when the text has not changed.
- The curve drag (`OnCurveMoved`, `:1164-1185`) also sends and redraws per pointer move with no coalescing.
- Fix:
  - Only touch what changed: compare values, and set the content only when the label differs.
  - Batch the curve and histogram redraws through the existing `QueueAdjustRedraw`.
  - Coalesce curve sends to one per frame.

**M2. A failed tree listing collapses the whole subtree.** Confirmed by reading.
- Where: `IslandHost.Panels.cs:2202-2241`, with `MediaViewerSession.ListSubdirectories` (`MediaViewer.Interop/MediaViewerSession.cs:487-509`).
- Why it happens:
  - `ListSubdirectories` returns an empty list on `MvStatus.Io`, or when the second call fails because the folder grew between the size call and the fill call.
  - `TreeNodeUi.Apply` treats that as "no subfolders": it removes every child node, expanded ones included, and marks the node `_empty`, so the chevron disappears.
  - A transient error on a network share or a removable drive during a watcher-triggered `Refresh()` therefore wipes what the user had opened.
- Also confirmed: concurrent `List()` calls (one `Task.Run` per `Refresh`) can land out of order, so an older listing overwrites a newer one. `Apply` has no generation check.
- Fix: return `null` (or a status) on failure and keep the existing children; add a per-node listing generation.

**M3. People grid: a read for the old scope or folder is applied after the user has changed scope or folder.** Confirmed by reading.
- Where: `PeopleGrid.cs:337-345`, `:375-441`.
- Why it happens:
  - `SetScope` and `OnFolderChanged` call `Refresh()`. If a read is already running, they only set `_loadAgain`.
  - When the running read lands, `generation == _loadGeneration` is still true, because the counter only increments when a read starts. So it is applied: the grid shows the previous folder's people.
  - The new scope is read only 500 ms later (`Task.Delay(500)`).
- Effect: in that window a click (`ShowPhotos` uses the new `ScopeDir` with an old person) or a drag-merge acts on a stale list.
- Fix: capture `(scopeDir, scope)` with the read and drop the result if they no longer match, or bump the generation in `SetScope` and `OnFolderChanged`.

**M4. People grid: one unexpected exception can stop the grid from ever refreshing again.** Confirmed by reading; suspected trigger.
- Where: `PeopleGrid.cs:403-439`.
- The comment says "a read that throws must still clear `_loading`".
- But the catch filter is `MediaViewerException or JsonException or InvalidOperationException or FormatException`:
  - `GetProperty("id")` on a row without `"id"` throws `KeyNotFoundException`, which is not in the filter.
  - The `ReadJson` underflow in M7 throws `ArgumentOutOfRangeException`, which is not in the filter either.
- Either one faults the task. The dispatcher callback never runs, `_loading` stays `true`, and every later `Refresh()` only sets `_loadAgain`.
- `ManagePanel.RefreshRoots` (`:1033`) does catch `KeyNotFoundException`.
- Fix: put the enqueue in a `finally`.

**M5. Person sheet: a failed "Split into new person" leaves the faces hidden although they are still the person's.** Confirmed by reading.
- Where: `PersonSheet.cs:887-899`.
- Why it happens:
  - `RemoveLocal(ids)` hides the faces straight away.
  - The worker swallows `MediaViewerException` and returns 0, and `done` is `_ => { }`. There is no status line and no `Load()`.
  - `_grid.Refresh()` reloads the sheet only when the person's face count changed (`PeopleChanged`, `:451-456`), and after a failed split it has not.
- Effect: the sheet keeps showing those faces as gone until it is reopened.
- Fix: return success or failure and, on failure, `SetStatus(CouldNotChange)` and `Load()`, as `MoveTo` does.

**M6. Settings → Local search reads the disk on the UI thread.** Confirmed by reading.
- Where: `ManagePanel.cs:1352-1374` (`ShowSuggestions`).
- `System.IO.Directory.Exists(dir)` runs for Pictures and Videos on every `ShowRoots` redraw. That redraw happens about once a second during indexing, because of H2.
- Effect: with Known Folders redirected to a network share or OneDrive, this blocks the UI thread, against rule 1.
- Fix: resolve and check both folders once on a worker, and cache the result.

**M7. The interop JSON readers turn `needed == 0` into a negative length.** Confirmed by reading; whether a pack or core ever returns that is suspected.
- Where: `Interop/Ai.cs:530`, `Interop/Addons.cs:178`, `:485`, `:582`, `Interop/Slots.cs:203`.
- The expression is `Encoding.UTF8.GetString(buf, 0, (int)Math.Max(0, needed - 1))` with `uint needed`.
  - If native returns `Ok` without setting `needed`, or sets it to 0, then `needed - 1` wraps to `uint.MaxValue`. `Math.Max(0u, …)` keeps it, and the `(int)` cast gives `-1`, so the call throws `ArgumentOutOfRangeException`.
  - If native ever reports `needed > cap` together with `Ok`, the read runs past the buffer.
- None of the callers expects that exception type:
  - `SlotNative.Json` catches only `EntryPointNotFoundException`, and it is called on the UI thread.
  - For `PeopleGrid`, see M4.
- Related: `ImportApi.ReadPath` (`Addons.cs:189`), `AddonNative.MakeStaging` (`:625`) and `Load` (`:692`) pass `Array.IndexOf(buf, 0)` straight in, so a missing NUL gives `-1` and the same throw. `AiApi.ReadPath` already guards this (`Ai.cs:541-542`).
- Fix: in one shared helper, compute `int len = (int)Math.Min(cap, Math.Max(1u, needed)) - 1`, clamp the `IndexOf` result, and use that helper in all three classes.

### Low

**L1. Typing a value into an Adjust row and pressing Enter leaves keyboard focus nowhere.** Confirmed by reading.
- Where: `IslandHost.Adjust.cs:974-1014`.
- `Finish(true)`, reached by Enter, removes the field and shows the readout button again, but does not focus anything. Only the Esc path calls `slider.Focus(...)` (`:1008`). The pane's ↑↓/A/R keys are tied to a focused slider (`OnAdjustKey`), so they stop working until the user tabs back in.
- Suspected, same place: `field.Focus(FocusState.Programmatic)` (`:1012`) runs before the field is loaded. If that focus call fails, `LostFocus` never fires and the field stays open with no focus.

**L2. The Curves editor cannot be used from the keyboard.** Confirmed by reading.
- Where: `IslandHost.Adjust.cs:820-860`.
- The curve `Canvas` is not focusable. Adding, moving and removing points are pointer-only (`OnCurvePressed`, `OnCurveMoved`), and it has only an accessible name ("Curve") with no value or pattern. The rest of the pane is keyboard-complete, and CLAUDE.md asks for every command to be reachable from the keyboard.
- Fix: a focusable point cursor (←→ choose a point, ↑↓ move it, Insert or Delete to add or remove).

**L3. In light mode the Curves "Image" channel line is close to invisible.** Confirmed by reading.
- Where: `IslandHost.Adjust.cs:1209-1215`, `:1249-1255`.
- `tints[0] = (210,212,218)` is used as the stroke and the point fill of the Image curve in every theme, drawn over `ChromeColour.Surface`. In light themes that is light grey on near-white.
- `RenderHistogram` already uses `Brush(Title)` for luma (`:1480`); the curve should too.
- The lit clip-triangle colours (`:1445`, `:1451`) are also hard-coded RGB without a theme role.

**L4. A slot add-on button in the Video Editor may run on a different file from the one being edited.** Suspected.
- Where: `IslandHost.VideoEditor.cs:1075-1080`.
- `RunEditorAddon` sends `Items[_selectedIndex].Path` along with `_editorPieces`. If the viewer's selection can move while the editor window is open, the add-on gets one clip's path with another clip's kept ranges. Native knows the edited clip, so the path should come from the editor view args.

**L5. The Video Editor's Light and Sound flyouts may leave a native "drag" session open.** Suspected.
- Where: `IslandHost.VideoEditor.cs:951-961`, `:1015-1029`.
- `EditorLightDrag 1` is sent on `Opening` and `EditorLightDrag 0` only on `Closed`. If the editor window closes while a flyout is open (`DetachVideoEditor` disposes the island), `Closed` may never fire, and native keeps the one-undo-step session open.
- Fix: send `EditorLightDrag 0` in `DetachEditorSources`.

**L6. Changing Compute or the index size limit clears the ComboBox's items from inside its own `SelectionChanged`.** Suspected.
- Where: `ManagePanel.cs:202-206`, `:372-376`, `:569-575`, `:599-610`, `:955-966`.
- `SelectionChanged` calls `Set`, which calls `RefreshSettings`, which runs `Items.Clear()` and refills the items while the combo is still raising the event. The `_updating` flag stops the loop, but not the collection change during the event. This is a classic source of WinUI reentrancy faults; in this island host it can fail-fast.
- Fix: rebuild the items only when the list actually changes (it rarely does), or defer `RefreshSettings` with `DispatcherQueue.TryEnqueue`.

**L7. If a multi-person merge partly fails, the merges that succeeded get no Undo.** Confirmed by reading.
- Where: `PeopleGrid.cs:733-766`.
- When any `PersonMergeCovers` throws, `failed` is set and the tokens collected from the merges that did succeed are dropped, so there is no Undo for them.
- Also: unlike renames (`_renames`), a merge has no guard against a people read that was already in flight. That read lands and briefly puts the merged-away people back until the next refresh.

**L8. The Local search "remove" and install workers stay stuck if anything other than `MediaViewerException` is thrown.** Suspected.
- Where: `IslandHost.LocalSearch.cs:1210-1226`, `:1236-1249`, `:1388-1392`.
- `RemoveCore` and `RemovePiece` catch only `MediaViewerException`. `FinishAiChange` (`ReadAddonStates`, `FamilyUsage`) has no guard. Any other exception leaves `slot.Busy` and `Removing` true for good, and Settings shows "Removing" forever.
- In `BeginPieceInstall` the budget callback writes the UI-owned statics `_aiUsed` and `_aiCeiling` from the pool thread.

**L9. People can only be moved left and right in the person sheet.** Confirmed by reading.
- Where: `PersonSheet.cs:1173-1176`.
- Only ←/→ step the cursor. ↑/↓ do not move by rows in the two `UniformGridLayout` grids, so a person with 300 faces is a long walk from the keyboard. A small gap, but the Mac sheet's grid navigation (#192) implies rows.

---

## Improvements (not bugs)

- **Gallery tiles for screen readers:** neither gallery nor folder tiles (`IslandHost.Gallery.cs:1272-1316`, `:1413-1472`) are focusable and they have no accessible name on the tile itself. Navigation is native-routed, so Narrator cannot list the grid's items. Consider `AutomationProperties.Name` and `PositionInSet`/`SizeOfSet` on the tile border, and a live region announcing the selected item.
- **Folder tree size:** the tree builds one row per subfolder with no virtualisation (`IslandHost.Panels.cs:1821-1826`). A folder with thousands of subfolders (camera DCIM dumps, date folders) builds them all in one `Apply`. An `ItemsRepeater` over a flattened list would bound the cost.
- **All tags list:** `TagListFactory.RecycleElement` is a no-op and every realised cell builds a new `MenuFlyout` through `Selectable` (`IslandHost.Panels.cs:1407-1431`, `:223-258`). Scrolling a 2,000-tag file allocates a flyout per row each time it comes into view. A single shared context flyout on the list would remove that.
- **"Merge into…" menus:** these list every person (`PeopleGrid.cs:1277-1285`, `PersonSheet.cs:1108-1123`), hundreds of items with no search. The `PersonPicker` the sheet already uses for "Move to…" would fit.
- **Jobs pane:** it polls every 250 ms while visible and re-reads progress for every job ever listed (`IslandHost.Clip.cs:824-892`). Rows in Done, Failed or Cancelled could be skipped once drawn in a final state.
- **Video Editor strip:** `SetVideoEditorStrip` swaps RGBA to BGRA with a managed per-byte loop on the UI thread (`IslandHost.VideoEditor.cs:335-343`). It runs once per clip, so this is fine today, but native could emit BGRA directly.
- **Error colours:** `ManagePanel` shows errors in `AddonColour.Accent` (`:346`, `:1106`). This is the same missing error role as already reported for Panels, now on the add-on side.

---

## Not read or not verified

- These helpers were only skimmed or not read: `Shared/FakeInput.cs`, `Look`, `FaceTile`, `FaceCrops`, `AiChrome`, `ManagePanel.Transfer.cs`, `UnknownFacesSheet`, `PhotoPreview`, `PersonPicker`, `IslandHost.Tiles.cs` (only `ThumbBox` and `ShowTileImage`), `IslandHost.Filmstrip.cs` (only the drain).
- I did not check the native struct layouts (`chrome_edit_args`, `chrome_editor_view_args`, `adjust_view`, `mv_ai_status`) field by field against their headers. The hand-read offsets are internally consistent, and the ABI smoke test is meant to cover `MvAiStatus`.
- To check claims, I read these native call sites: `push_meta_pane` and `metadata_selection_changed` (they confirm an item change pushes an empty record, so the meta editors do drop their drafts), `take_parked_text` (128 KB buffer), `editor_trim_to` and `push_adjust_pane`.
