# iPhone/iPad app audit (src.ios + src/mobile link side)

Read-only audit, 2026-11-25, branch `claude/project-audit-performance-180749`. Nothing built or run.
Paths are relative to the repo root. "Confirmed" = I read the code path end to end; "suspected" =
the code points that way but it depends on runtime/OS behaviour I could not verify by reading.

Open issues checked (`gh issue list --state open --limit 200`): the only iOS-adjacent ones are #46
(iPhone Photos companion), #153 (chrome tests + UI smoke per platform), #497 (Driftwire six-digit
code, desktop LAN), #686 (phone access add-on). None of the findings below is already filed, except
where #153 is cited.

Not covered in depth: LinkConnection.swift 620–1040 (sync, sound search), LinkFiles.swift beyond its
set-up, FolderEditing/FolderTiles/BackupScreen/VPNScreen beyond skims, and the server side beyond
pairing-code expiry and the VPN hop's authentication.

---

## A) Screen inventory

| # | Screen | File(s) | Reached from | What it does | UITest coverage |
|---|---|---|---|---|---|
| 1 | Welcome | `MobileUI/SetupScreens.swift:10` | First launch / after Forget (`AppModel.stage(for:)` AppModel.swift:271) | Requirements list, "Scan code" | SmallScreen `welcome`; Accessibility audit `welcome` |
| 2 | Scan (camera, Choose a picture, Paste link, Enter a code) | `SetupScreens.swift:60–357` | Welcome, notice "Scan code", Connect "Scan again"; `fullScreenCover` in RootView.swift:27 | AVCapture QR, Vision/CIDetector on a picked image, clipboard `mediaviewer://pair|sample`, typed code (SAMPLE) | SampleLibraryTests test1/test2 (typed code); camera path none (Simulator: `MV_TEST_SCAN_CODE` bypass, MediaViewerApp.swift:21) |
| 3 | Connect (name + fingerprint check, steps, failures, "replace with sample" alert) | `SetupScreens.swift:361–692` | Any scanned/typed/linked code | `identify` then `pair`; route/outside-route steps | LinkEndToEndTests test1, SampleLibraryTests test1–2, Accessibility (`connect.confirm`, try-again test) |
| 4 | Pair-link confirm alert (`mediaviewer://pair|sample|sample-guest`) | `RootView.swift:33–52`, `AppModel.swift:341–380` | Another app opens the URL | Open / confirm replace / ignore by stage | Unit: `MobileUITests/PairLinkTests.swift`; no UITest |
| 5 | First copy | `SetupScreens.swift:707–805` | After pairing; relaunch mid-copy | Catalogue copy, Wi-Fi wait, mobile-data button, BGContinuedProcessingTask | LinkEndToEndTests test1 (interruption), Accessibility, SmallScreen |
| 6 | Library: All media (month sections, folder shelf, files, notices, route chip) | `LibraryScreens.swift:11–173`, `MediaGrid.swift` | Library tab | Date grid, notice slot, skeleton | RealCatalogueTests, ScrollHitchTests (grid), SmallScreen `library.*` |
| 7 | Folder pages (stack), folder tiles, file list | `LibraryScreens.swift:44`, `FolderTiles.swift`, `FileList.swift` | Folder tile / shelf | NavigationStack per folder, prefetch, offline drives | FolderNavigationTests (2), ScrollHitch folder nav |
| 8 | Folder editing (select, new folder, rename, move, delete, Add from Files/Photos, uploads) | `FolderEditing.swift`, `LibraryChanges.swift` | Select/Add in a changeable folder | Server-side changes, uploads with background time | FolderChangeTests (4), SampleChangesTests |
| 9 | Other-file preview (Quick Look) | `LibraryScreens.swift:25`, `AppModel.swift:790–826` | Tap a PDF/doc row | Download then QL | FolderNavigationTests `testAFolderOfDocuments` |
| 10 | Search (recents, live results, nothing found, filters, file-mode scope row) | `SearchScreen.swift`, `SearchModel` in `AppModel.swift:974–1166` | Search tab | Debounced live search, filter chips, recents | LiveSearch, SearchKeyboard (2), SearchSmoothness, SearchUpdates, SampleLibrary test3, LiveSync, ScrollHitch typing |
| 11 | People / Person | `LibraryScreens.swift:226–321` | People tab (only when people exist) | Face grid, person grid | SearchKeyboardTests (tab arriving); SmallScreen; nothing opens a person |
| 12 | Viewer (pager, pinch/double-tap zoom, pull to close, chrome, filmstrip) | `ViewerScreen.swift:8–328`, `PagerController.swift` | Any tile (fullScreenCover, RootView.swift:91) | Ladder tile → JPEG-512 → original | ViewerBehaviourTests, FilmstripScrubTests, ScrollHitch paging, LinkEndToEnd test2, SampleLibrary test4 |
| 13 | Details (inspector on iPad, sheet on iPhone) | `ViewerScreen.swift:367–433` | Viewer "info" | EXIF/place/file/video facts | SmallScreen `details` (screenshot only) |
| 14 | Clip player (moments, scrub, hold for 2×, too big / not playable / download) | `ClipScreen.swift` | Clip tile, Play, search moment | AVPlayer over resource loader / local HLS | LinkEndToEnd test2, SampleLibrary test4, Accessibility (3) |
| 15 | Save to Photos / Share | `OriginalActions.swift`, `App/MediaViewer/PhotoLibrarySaver.swift` | Viewer chrome | Download original, PHAssetCreationRequest / share sheet | None functional (Accessibility only checks the buttons exist) |
| 16 | Settings | `SettingsScreen.swift` | Settings tab | Library facts, mobile-data toggle, VPN row, backup row, look, filmstrip, Forget | SmallScreen `connection*`, Accessibility; SampleLibrary test5 (Forget) |
| 17 | Backup | `BackupScreen.swift`, `MobileLink/LinkBackup.swift` | Settings → Backup | Phone → NAS folder backup | SampleLibrary test4b, SearchKeyboard test1 |
| 18 | VPN | `VPNScreen.swift`, `MobileLink/LinkVPN.swift`, `App/DriftwireTunnel/PacketTunnelProvider.swift` | Settings → VPN | Paste code, consent, switch, auto-connect + home Wi-Fi | VPNSettingsTests (8) — fixtures only, no tunnel |
| 19 | Transfer strip / bottom accessory | `RootView.swift:170–230` | Running backup/copy | One line above tab bar | SearchKeyboardTests |
| 20 | Offline / error states (notices, route chip) | `AppModel.swift:471–565`, `LibraryScreens.swift:137–172` | Connection snapshots | Revoked, access ended, unreachable, own-network, stale, storage | SmallScreen screenshots only (`library.unreachable`, etc.) |
| 21 | Live Photos | — | — | **Not implemented**: viewer and Save to Photos handle the still only (no `PHLivePhoto` anywhere; only LinkBackup.swift:464 mentions the paired video) | — |

iPad: `TabView.sidebarAdaptable` (RootView.swift:86), inspector Details, `readableWidth` 480 for forms.
No iPad-specific UITest exists (grep for iPad/size class in UITests finds only VPNSettingsTests).

---

## B) Issues by severity

### High

**B1. Pinch zoom never gets more detail than the screen: the "original" is decoded to screen size, then zoomed 4×.** Confirmed by reading.
`PagerController.swift:230–233` passes `fittingPixels` = view bounds × screen scale; `LinkImages.swift:159–163` (and file mode 179–184) decode the downloaded original with `kCGImageSourceThumbnailMaxPixelSize = longest`, i.e. ~2796 px on a Pro Max. `PagerController.swift:433` allows `maximumZoomScale = 4` and `scrollViewDidEndZooming` (557–559) never asks for a sharper decode. At 2.5× double-tap (572) or 4× pinch, a 24–48 MP photo shows an upscaled 2.8k image. The whole original is downloaded (up to 1 GB store) and then thrown away at screen resolution. Fix: decode to the original's size (or tile it) once zoom passes ~1.2×, or decode at `fittingPixels × maximumZoomScale` capped by memory.

**B2. No CI runs anything in src.ios.** Confirmed: `.github/workflows/*.yml` has no `MobilePackage`, `src.ios` or `xcodebuild` reference; the only iOS job is `ios-core` (C++ smoke, ci.yml:203–249). The Swift package tests (MobileUITests, MobileLinkSupportTests, MobileCoreTests) and all 17 UITest files run only by hand, so the "CI stays green" rule does not protect the phone app. Related to #153 but that issue is about desktop chrome; worth its own line.

### Medium

**B3. Main thread waits on Keychain I/O under the connection lock.** Confirmed by reading.
`LinkConnection.swift:587–592` (`noteAccess`) and `608–617` (`noteAddresses`) call `PairingRecord.save()` (SecItemDelete + SecItemAdd, Keychain.swift:11–18) inside `lock.withLock`. The same `lock` is taken on the main thread many times per frame: `LinkImages.cachedImage` → `connection.libraryID` / `connection.link` (LinkImages.swift:48–51, called from `MediaTile.body` via `LayeredImages.cachedImage`), `ModeLibrary`/`ModeSearch` `mode()` → `connection.current`. A connect that learns a new address or grant therefore stalls grid drawing for the Keychain round trip. Violates the hard rule "never block the UI thread waiting on a worker's lock". Fix: copy the record under the lock, save outside it.

**B4. Keychain writes are delete-then-add, so a kill or failure between them loses the pairing.** Confirmed by reading (`Keychain.swift:11–18`, `VPNTunnel.swift:209–216`). Re-saves happen after ordinary connects (B3), not only at pairing. If `SecItemAdd` fails (or the process dies) after `SecItemDelete`, `openStored` finds no record and publishes `.noLibrary` (LinkConnection.swift:153–154): the person must get a new code from the computer. Use `SecItemUpdate`, falling back to add on `errSecItemNotFound`.

**B5. "Cancel" on the first-copy screen forgets the pairing, with no confirmation.** Confirmed by reading.
`SetupScreens.swift:781` `cancel()` → `model.forgetLibrary()` (AppModel.swift:437–449), which removes the Keychain record and the device's library. Settings' Forget asks first (SettingsScreen.swift:165–172); this button, shown on every state of the copy (762–773), does not. It also appears when an already-paired library is sent back to the copy (`stage = .firstCopy`, AppModel.swift:296–297), so one tap there unpairs an existing library. Ask, or make Cancel pause the copy instead.

**B6. Grid tiles, the filmstrip, pager cells and faces read the 64 px tile synchronously on the main thread.** Confirmed path; impact suspected.
`MediaTile.shown` (MediaGrid.swift:43–46) → `LayeredImages.cachedImage` (LiveServices.swift:197–199) → `LinkImages.cachedImage` (LinkImages.swift:45–60), which on a memory miss calls `mv_tiny_get` (takes `view_m` shared lock, link_abi.cpp:667–676), copies the bytes and builds `UIImage(data:)`, whose decode happens lazily at render, on the main thread. The same happens in `FilmstripCell` (ViewerScreen.swift:295, 321, 325 — three calls per cell), `ZoomPageCell.show` (PagerController.swift:432) and `PersonCell` (LibraryScreens.swift:265). The catalogue's own off-main decode (`CoreImages`, LocalCatalogue.swift:346–375) is bypassed whenever its cache misses, which is every first appearance during a fast scroll. The file header (LinkImages.swift:5–6) accepts this for the tiny tile; worth measuring with the ScrollHitch harness on a device, and at least decoding with `kCGImageSourceShouldCacheImmediately` so the cost is paid once.

**B7. The audio session is never deactivated, so the person's music does not come back after a clip.** Confirmed by reading.
`ClipScreen.swift:72` sets `.playback` on every load; no `setActive(false, options: .notifyOthersOnDeactivation)` anywhere in src.ios (grep). Playing a clip interrupts Music/Podcasts and closing the viewer leaves them paused.

**B8. First play of a clip waits for a 4 MB probe that cannot be cancelled.** Confirmed by reading.
`LinkClips.swift:68–71` measures throughput before returning a route, by reading `min(size, 4 MB)` of the file (126–145) with `cancel: nil` (134). On a 10 Mbit/s mobile link that is ~3.2 s before AVPlayer is even created, on the first clip of every route epoch, and leaving the page does not stop it. The bytes are discarded and AVPlayer reads the same head again. Also `mv_asset_folder_check` adds a round trip first (49). Options: start playback and measure from the loader's own reads; or reuse the probe bytes; at least pass a `LinkCancel`.

**B9. Save/Share downloads keep running after they are superseded or the viewer closes.** Confirmed by reading.
`OriginalTransfer.save/share` (OriginalActions.swift:39–78) start an unstructured `Task` and only compare a token; the `for try await` loop keeps consuming, so the underlying `LinkImages.original` download (Task.detached, LinkImages.swift:222–246) runs to the end. Starting Share on another photo, or closing the viewer, leaves the whole original downloading (on mobile data too) with nothing on screen. Keep the `Task` and cancel it in `begin` and when the viewer goes.

**B10. An unreachable server is not retried while the app stays open, and its card has no Try again.** Confirmed in Swift.
`connect` runs only at launch (LinkConnection.swift:152), on `willEnterForeground` (108–111), on a network change (289–294) and from a notice's Retry. The `.unreachable` notice has `action: nil` (AppModel.swift:487–490). A NAS that comes back while the phone sits on the same Wi-Fi stays "unreachable" (and the route chip says "On this phone") until the app is backgrounded. Add Try again to that card, and/or a backoff retry while unreachable and active. Also, `pathChanged` returns early on `.unsatisfied` (LinkConnection.swift:289), so going offline publishes nothing and the screens keep the last "reachable" state until a request fails (suspected UX impact).

**B11. Backup relists the entire Photos library on every foreground.** Confirmed by reading.
`didBecomeActive` → `startIfAutomatic` (LinkBackup.swift:82–84) → `run` → `listPhotos` (352–390), which enumerates every asset twice, calls `PHAssetResource.assetResources` per asset (via `originals(of:)`, 465) and `cloudIdentifierMappings` for all ids, each time. With a 50k-photo library this is seconds of CPU and Photos XPC per app switch, then `didEnterBackground` stops it (86–88). Use `PHPhotoLibrary` change history (already used for deletions, 404–460) to list only what changed since the last token.

**B12. Swift 6 concurrency checking is off.** Confirmed.
`Package.swift:61` `swiftLanguageModes: [.v5]`; project `SWIFT_VERSION = 5.0` (project.pbxproj:411, 428) and no `SWIFT_STRICT_CONCURRENCY`. 43 `@unchecked Sendable` types and `nonisolated(unsafe) static weak var shared` (LinkConnection.swift:168) are therefore unchecked by the compiler. Concrete examples that rely on discipline: `LinkConnection.handle` `didSet` calls into C under `lock` (21); `ScopedCache.object` empties the cache outside its lock (ScopedCache.swift:20–25), racing a concurrent `set` for the old library. Turning on `-strict-concurrency=complete` as warnings would find the rest.

**B13. PrivacyInfo is missing the App Group UserDefaults reason; the extension has no manifest.** Confirmed by reading.
`LinkVPN.swift:44` writes `UserDefaults(suiteName: group.…)`, which the extension can read; the manifest declares only `CA92.1` (PrivacyInfo.xcprivacy:41–48), which excludes data shared with other apps/extensions. Add `1C8F.1`. `App/DriftwireTunnel/` contains only the Swift file (synchronized group, project.pbxproj:65), so the extension ships no PrivacyInfo; if its C++ (BoringSSL, netstack) links a required-reason API (e.g. `mach_absolute_time`, `stat` timestamps) App Store Connect will flag it (suspected).

**B14. No hardware-keyboard support on iPad.** Confirmed: no `keyboardShortcut`, `UIKeyCommand` or `onKeyPress` in MobileUI. With a Magic Keyboard the viewer cannot page (←/→), close (Esc), play (Space), or open Details; Search cannot be focused with ⌘F. CLAUDE.md asks for every command from the keyboard.

### Low

**B15. Custom URL scheme carries the pairing secret.** Suspected risk. `mediaviewer://pair#…` (MediaViewer.plist:27–38) includes the access key and identity (parsed in LinkConnection.swift:378–382). Any app can register the same scheme; if the person taps the link on a phone with such an app, it receives the code. Mitigated by expiry (`code_ttl_s`, src/addons/phone/link.cpp:254) and the fingerprint check, which the attacker would also pass. A universal link (associated domain) would close it.

**B16. Pairing survives app deletion.** Confirmed behaviour: Keychain items persist across uninstall; `openStored` re-adopts the record and republishes `hasLibrary` (LinkConnection.swift:116–152). Someone deleting the app to unpair a phone keeps a working pairing on it. Clear Keychain items on first launch when the "installed" UserDefaults marker is absent.

**B17. Library first paint hashes every item first.** Confirmed. `LibraryModel.grid(of:)` (AppModel.swift:963–971) builds a `MediaItem` (C row + Strings) for every position to hash a signature, and `listing` awaits it before any page shows (882). For a 50–100k library that is tens of ms on the critical path at launch and on each new catalogue. The signature is only needed to skip redraws when a listing repeats; skip it on first load or compute it in C.

**B18. Superseded prefetches and Photos thumbnail continuation.** `LinkImages.prefetch` (80–90) starts detached tasks with no cancel token; a fast swipe through the viewer queues lane-1 requests for pages already passed (they warm the disk LRU, so wasted bandwidth rather than a bug). `LinkBackup.thumbnail(ofPhoto:)` (223–240) resumes only on a non-degraded image; with `isNetworkAccessAllowed = false` and an iCloud-only asset the final callback may never come, leaving the awaiting task suspended (suspected).

**B19. Concurrent GCD queue for blocking network calls.** `linkNetQueue` is `.concurrent` (LinkCore.swift:9) and runs long blocking reads (originals, clip ranges, `ClipLoader` per AVPlayer range request, LinkClips.swift:296). Gates limit some paths, but nothing caps the total; many concurrent blocked blocks can push GCD towards its thread limit (suspected; watch for "thread explosion" in Instruments).

**B20. ITSAppUsesNonExemptEncryption = false while shipping a VPN.** MediaViewer.plist:46–53, DriftwireTunnel.plist:17–18. The comment records the owner's App Store Connect answers; note that a VPN's primary function is encryption, which normally falls under mass-market 740.17(b)(1) with an annual self-classification report rather than "no non-exempt encryption". Worth a second look before 1.0 (suspected, legal).

**B21. Background-task double end.** `BackgroundTime.begin` (LibraryChanges.swift:396–403) ends the task in its expiration handler, and `end()` (405–408) ends it again; the work itself is not cancelled on expiry. Harmless log noise, but the upload keeps running after the OS asked it to stop.

**B22. Scan accepts any QR code.** `metadataOutput` (SetupScreens.swift:349–355) delivers the first QR seen (a Wi-Fi or menu code), which moves to Connect and shows "not readable". Filtering for `mediaviewer://` (as Paste does, 159) would keep the camera scanning.

**B23. Tests use sleeps.** 57 `sleep(` calls in UITests (e.g. SmallScreenTests.swift:28 `sleep(4)`, AccessibilityTests.swift:91 `sleep(8)` × 22 runs). CLAUDE.md: no sleeps. SmallScreenTests also skips unless run through `tools/ios/small-screen.sh`.

Checked and fine: Keychain items use `AfterFirstUnlockThisDeviceOnly`; the VPN item is in its own access group and the extension's entitlement lists only that group (DriftwireTunnel.entitlements:15–18); no logging of paths/hosts (no `print`/`Logger`/`NSLog` in Sources or App); the loopback HLS server binds 127.0.0.1 with a random per-clip UUID path (TsStreams.swift:62–72, 133); the VPN hop authenticates the server with CPace on the code's key then Noise (src/addons/vpn/tunnel.h:10–13); camera `startRunning` is off the main thread (SetupScreens.swift:332–342); 5.4 organisation requirement is recorded (docs/plans/phone-vpn.md:476).

---

## C) Improvements and optimisations

**Search latency (README.md:273: 110–271 ms key-to-answer, 4.2 ms search).** Where the rest goes, from the code:
1. **90 ms fixed debounce** before the search starts (`AppModel.swift:1092`). This alone is most of the 110 ms floor. Run the first key immediately (cancel-and-replace already exists via `CoreSearch` cancel flag, LocalCatalogue.swift:464–481) and debounce only while keys arrive faster than the answer; or drop to ~30 ms.
2. **Per-hit hydration** on the search queue: `itemFor` (C row → Swift Strings) and a snippet `String(cString:)` per hit (LocalCatalogue.swift:505–510). Cheap per hit but linear in hits; cap the first page (e.g. 300) and fill the rest lazily, as `GridSource(list:)` already does for the library.
3. **Two thread hops + a detached `GridSource(entries:)` build** with a uniqueness pass over all ids (AppModel.swift:1111–1113, 68–77).
4. **SwiftUI diff of the whole result grid with a spring** (`SearchScreen.swift:51`, `.animation(answerMotion, value: answer)`), keyed by id (MediaGrid.swift:171–175), and `section.range.map { Slot }` rebuilt on every body pass (173). If the measurement ends at "answer on screen", part of the 271 ms top is the animation and tiles' first ladder requests. Measure with signposts at: key → `run` → service start → service end → `entries` set → first tile painted.

**Scroll hitching.** B6 (main-thread tiny tile decode) is the main candidate. Also: `MediaTile` builds `"\(context)/\(entry.position)"` and the accessibility label strings per tile per body (MediaGrid.swift:208, 216; `accessibilityName` formats a date each time, AppModel.swift:27–29) — cache in `GridEntry` or compute only when VoiceOver is on. `LazyVGrid` with month `Section`s needs ids for every position on each update; for very large libraries a `UICollectionView` with compositional layout (already used for the pager) would scale better (suspected; check ScrollHitchTests on a 50k fixture).

**Thumbnail decode/downsample.** JPEG-512 is decoded at 512 px (LinkImages.swift:72) for 120 pt tiles; decoding at `tileSide × scale` (360 px on a 3× phone) saves ~50% memory and decode time. `FilmstripCell` asks for the JPEG-512 over the network for every 48 pt cell scrubbed past (ViewerScreen.swift:324); the 64 px tile is enough there. Cache budgets add up to ~450 MB+ of NSCache (LinkImages 256 MB + 8000 tiles, CoreImages 96 MB, LinkFiles 96 MB: LinkImages.swift:17–18, LocalCatalogue.swift:335, LinkFiles.swift:46); size the JPEG cache from the visible tile count (~4 screens) instead.

**Launch time.** B17 (signature hash before first paint). `LiveServices.services()` runs inside `@State` init of the `App` (MediaViewerApp.swift:12), constructing LinkConnection (NWPathMonitor start, notifications), LinkBackup, LinkVPN and LocalCatalogue on the main thread before the first frame; worth one Instruments "App Launch" pass on a device.

**Streaming start.** B8 (probe), the extra `mv_asset_folder_check` round trip (LinkClips.swift:49), and the exact seek to a moment with zero tolerance before `play()` (ClipScreen.swift:95–99): on long-GOP camera clips, a ±0.5 s tolerance (as `seek(to:)` already uses, 231) then a fine seek after the first frame would start sooner.

**Battery.** B11 (full Photos relist per foreground); the willEnterForeground connect race on every app switch (LinkConnection.swift:108–111) even when the link is live (comment says it "keeps a live link", but it still runs the race; skip when last success is recent); `UserDefaults.didChangeNotification` observer (105–107) runs `mv_link_set_upgrade` under the lock for every defaults write, including the 7 writes `publish` does itself (242–252).

**Features worth noting.** Live Photo playback in the viewer and Live Photo save (PhotoLibrarySaver adds only `.photo`/`.video`, PhotoLibrarySaver.swift:92–94); pinch to change grid density; a date scrubber for long libraries.

---

## D) Test gaps

1. **Nothing runs in CI** (B2). Add at least `swift test --package-path src.ios/MobilePackage` for MobileUITests + MobileLinkSupportTests on macOS runners, and a single UITest smoke (SAMPLE code → grid → search → viewer) on a Simulator.
2. **MobileLink has no test target** (Package.swift:51–59 lists MobileCoreTests, MobileLinkSupportTests, MobileUITests only). LinkConnection (publish ordering, generation checks, Keychain save failure), LinkImages (ladder, cache scoping), LinkClips (route decisions, probe), LinkBackup and LinkVPN are untested except via manual UITests.
3. **Viewer zoom resolution** (B1): a test that a zoomed photo ends with an image whose pixel size exceeds the screen's.
4. **Save to Photos / Share**: no functional test of the happy path, Photos-denied path, cancellation on supersede (B9), or file naming (PhotoLibrarySaver `named`/`safeName`).
5. **First-copy Cancel** (B5) and **Keychain atomicity** (B4): no tests.
6. **Packet tunnel extension**: VPNTunnelTests cover the shared types; nothing covers `PacketTunnelProvider` start/stop races, `rejoin` backoff, or path moves (could be unit-tested by extracting the state machine as was done for `VPNMoves`).
7. **iPad layouts**: no UITest on an iPad destination (inspector, sidebar tab view, keyboard).
8. **Offline transitions**: unreachable → reachable while foreground (B10), airplane mode mid-scroll, background/foreground with a live clip.
9. **Pair link from another app**: `PairLinkTests` covers the decision table; no UITest opens a URL in each stage.
10. **People → Person screen**: never opened by a UITest.
11. **Large library**: ScrollHitch uses fixtures; no memory-growth test (scroll 50k items and assert resident memory stays bounded).
12. **Sleeps** in UITests (B23) make the slow suites (Accessibility ~3 min of sleeps) unlikely to be run; wait on element existence instead.
