# Updates, Crash Reporting, Telemetry

How MediaViewer installs and updates on Windows and macOS, how crashes are captured and scrubbed,
and what telemetry exists. Release mechanics are in [RELEASING.md](../../RELEASING.md); runbooks
in [DEVELOPMENT.md](../DEVELOPMENT.md#package-and-install-pr-8).

All three systems follow one rule: nothing about a user's files (paths, filenames, folder
structure, image content, thumbnails, EXIF) leaves the machine.

---

# Part 1 — Auto-update

## The channel decision

Distribution is a signed installer plus the app's own updater, from GitHub Releases. There is
no Store channel: the app is GPL-3.0-or-later ([licensing](11-licensing.md)), and owning the
channel lets codec fixes ship without a review queue.

| | First install | Every later update |
|---|---|---|
| Windows | Signed **Inno Setup** wizard | **Velopack**, in the background |
| macOS | Developer ID–signed, notarized, stapled **`.dmg`** | **Sparkle 2** |
| What the user sees | A short branded first meeting, once | Nothing until "Update ready — restart" |

There are two channels, chosen in Settings: **stable** (default) and **preview**, which also
follows signed prerelease builds.

## Per-user install

Windows installs to `%LocalAppData%\MediaViewer`, never `Program Files`, so neither install
nor update ever needs UAC. There is no per-machine / enterprise variant.

## First install — the Windows wizard

`tools/package/mediaviewer.iss`. Six pages:

1. **Welcome** — app icon, name, one line.
2. **Licence** — GPL-3.0-or-later, scroll and accept.
3. **Location** — the LocalAppData default, editable; no "all users" option.
4. **Options** — Start Menu shortcut on, Desktop shortcut off.
5. **Progress.**
6. **Finish** — **Launch MediaViewer**; links to GitHub and the licence; a checked *Choose
   MediaViewer as the default for all supported photos and videos* that opens
   `ms-settings:defaultapps` on MediaViewer (Windows owns the confirmation; `UserChoice` is
   never written); a checked *Delete the installer* that removes the downloaded
   `MediaViewer-x.y.z-Setup.exe` after the wizard closes. Silent installs neither open Settings
   nor delete the file.

The wizard registers `ProgId` / `OpenWithProgids` / `Capabilities` for the D5 photo and video
types under HKCU. Telemetry is not asked here; it is a first-run screen in the app.

**Re-running Setup** over an install keeps `addons\`, `thumbs\`, `metadata-snapshots\`,
`Crashes\`, `clipboard\`, `telemetry\`, `settings.ini` and `import-hint.dismissed` (moved to a
sibling `MediaViewer.keepN` across Velopack's `--installto`, then restored). If one cannot be
moved, MediaViewer is still running and Setup stops before deleting anything.

**Uninstall** (Apps & features) removes shortcuts, registrations, the install directory, the
thumbnail cache and add-ons, then asks (default No) whether to delete each add-on's `data\`.
Both Setup and uninstall unregister the chrome's session font from `current\` / `fonts\` first,
because a font left registered from `current\` blocks Velopack's rename of that folder.

### Icon

One mark everywhere the OS shows the app: one `.ico` (16, 20, 24, 32, 40, 48, 64, 256) for the
wizard and installer exe, shortcuts, window and taskbar, and each still `ProgId`'s
`DefaultIcon`; one `.icns` (16–1024, @1x/@2x) on macOS for the app, the disk-image volume, the
Dock and document icons. A PNG of the same mark is in About (`tools/make-icon.py`).

### About

Shows the running version, the GPL, links to the GitHub repo and `THIRD-PARTY.md`, and the
source link for **this build's** release tag ([licensing](11-licensing.md#source-offer)).

## macOS first install — a branded disk image

A drag install: no `.pkg`, no installer app, no privileged helper, no login item.

1. **Window** — app icon, an Applications alias, a background with the name and one line.
2. **Licence** — GPL-3.0-or-later as the image's licence agreement, Agree / Disagree on mount.
3. **Location** — wherever the user drags it (`/Applications` or `~/Applications`).
4. **Finish** — first launch. Notarization means Gatekeeper shows only its standard
   downloaded-from-the-internet prompt.

First launch shows a default-viewer sheet with **Use MediaViewer for all supported photos and
videos** checked (`MvSetDefaultViewer`, `NSWorkspace setDefaultApplicationAtURL`); unticking or
Not Now leaves existing defaults alone, and the app menu keeps the command. When the installer
disk is still mounted, or the `.dmg` the app came from is still present, the sheet adds a
checked **Eject the installer disk and move MediaViewer-x.y.z.dmg to the Trash** (Trash, never
a permanent delete; a disk in use stays mounted with a note). The `hdiutil info` lookup runs off
the main thread, and nothing is offered while the app runs from, or translocated off, the image.

**Uninstall** is dragging the app to the Trash; the Quick Look extension is inside the bundle.
The thumbnail cache (`~/Library/Caches/MediaViewer`) and preferences remain.

## Mechanics

### Versioned folders (Windows)

```
%LocalAppData%\MediaViewer\
  MediaViewer.exe     ← Velopack stub; what shortcuts point at
  Update.exe          ← Velopack
  current\            ← the running version, complete with its DLLs
  packages\           ← downloaded full/delta packages; one full package kept for rollback
  updater\trial.ini   ← start-attempt record (below)
  settings.ini, thumbs\, Crashes\, telemetry\, addons\, …
```

The codec DLLs (`avcodec-*`, `libheif`, `libraw`, `exiv2`) are locked while the app runs, so an
update is staged beside the running version and applied as a folder swap on exit, never a
file-by-file overwrite.

### Size and deltas

The Windows app payload is capped at **250 MB** ([build](09-build-and-test.md#installed-size));
most of it is codec and runtime DLLs that rarely change, so Velopack deltas keep a typical
update small. `build-release.ps1` refuses to package Windows App SDK AI / ONNX / DirectML /
WebView2 files. Add-on packs (Import, Local search, Voice) are separate signed downloads
installed from Settings → Add-ons, never part of the base installer or its updates.

### Staging and restart

- **Windows** (`src.managed/MediaViewer.Updater/UpdateService.cs`): one background thread at
  below-normal priority does all network and disk work. First check 30 s after launch, then
  every **6 h**; Settings changes poke an immediate check. Download and staging happen in the
  background.
- **macOS**: Sparkle with automatic checks and automatic download; the app also checks on every
  start (Sparkle's scheduled check alone would miss a release for up to 6 h). Sparkle installs a
  zip of the stapled app, never the disk image.
- **Never interrupt.** No modal, no forced restart. The command bar shows "Checking for
  updates…" / "Downloading update…" quietly, then **"Update ready — restart"**. Ignored, a staged
  update applies on exit (Windows) or is held for the user (macOS delegate).
- **State across the restart** (Windows, `shell/update_guard.h`): the restart relaunches with
  the open path, `--restore-zoom N`, `--restore-fullscreen` and `--restore-gallery`. Playback
  position is not an argument: the player saves its resume point when a clip closes.
- **Automatic checks can be turned off** in Settings (`[update] auto_check`), for users who
  want the app fully offline. The check is the app's only network call by default and reveals
  IP, version and timing; it is a plain GET of fixed asset URLs with no cookies and nothing
  about the user's files.

### Rollback and the kill switch

- **Start-attempt guard (Windows).** Before handing a staged package to `Update.exe`, the
  managed updater writes `[trial]` (version, prior version, prior package, attempts) to
  `updater\trial.ini`. The native host counts starts of that version before anything that can
  crash, clears the record once the chrome has attached and a few seconds have passed, and on
  the third start after **two failed starts** rolls back to the prior package. The failed version
  is listed under `[failed]` so it is never offered again on that machine, and the user is told
  once. Native, because a version that fails to start usually dies before .NET loads.
- **Manifest policy** (`UpdateManifest.cs`, pure): the signed manifest carries `version`,
  `min_version`, a `blocklist`, channel, release time and packages (file, SHA-256, size, kind).
  A manifest is rejected if it is a downgrade, its own version is blocklisted, or this machine
  already rolled that version back. A running version below `min_version` or on the blocklist
  raises the urgency shown to the user.
- **macOS:** Sparkle keeps no previous version, so there is no start-attempt rollback on Mac.

### Signing

- **Windows:** binaries and the Velopack payload are Authenticode-signed with Azure Trusted
  Signing when credentials are given (optional; without it SmartScreen may warn). The update
  manifest is **Ed25519-signed** (detached signature asset); the updater verifies it with
  BouncyCastle against the public key pinned in `src.managed/MediaViewer.Updater/UpdateKeys.cs`
  **before** parsing anything, then checks each downloaded package's size and SHA-256 against
  the signed entry. HTTPS with default certificate validation is transport only.
- **macOS:** the appcast is EdDSA-signed and checked against `SUPublicEDKey` pinned in the app
  (`SURequireSignedFeed`, `SUVerifyUpdateBeforeExtraction`); the feed URL must be `https://`.
  Sparkle is linked only when the build is given that key; a build without it has no updater.
  No system profile is sent.
- An artefact built without signing credentials is marked unsigned and not publishable.

### Licensing on every release

Each release tag is the source offer for that build; About links to the running version's tag
([licensing](11-licensing.md#source-offer)).

---

# Part 2 — Crash reporting

## Out-of-process, always

**Crashpad** on both platforms. Its separate handler process survives the app dying, so stack
overflow and heap corruption (what a decoder fed a malformed file produces) still yield a dump.

## Two capture paths

- Native crash in the core (decoder, GPU, FFmpeg) → Crashpad minidump.
- **Windows chrome:** unhandled managed exceptions (`CrashCapture.cs`) write a scrubbed text
  report to `Crashes\managed\`.
- **macOS chrome:** an uncaught `NSException` or a Swift runtime trap (below).
- Nothing throws across the C ABI; every core call has a **correlation id**, and the id of the
  last call (`mv_last_call_cid`) is in both the native and the chrome report.

## Symbols

PDBs (and `.ilk`, `.exp`, `.lib`) are excluded from the Windows payload by
`build-release.ps1`.

## Scrubbing

A minidump from a photo viewer can hold decoded pixels, a path and EXIF GPS. Scrubbing happens
on the next launch, before any send could be offered:

- every captured byte outside a thread stack is zeroed (this replaces an arena-tagged heap
  filter: it removes decoded image buffers without needing to find them);
- paths, media filenames and the username / computer name are masked, same length;
- annotations carry only format, decoder / version, correlation id and optional geometry, built
  from literals and integers inside the core.

### Windows

- **Capture.** `crashpad_handler.exe` beside the exe, started asynchronously (~3 ms on the UI
  thread). Database `%LocalAppData%\MediaViewer\Crashes`. No URL, indirect memory off, WER
  forwarding off.
- **Annotations.** Sixteen `mv_decode_N` slots (`core/crash_context`, 128 bytes each; each
  decode claims a free one for its duration, so concurrent decodes never share) plus
  `mv_last_call_cid`.
- **Scrub.** In-app on start, or `tools/minidump-scrub`.
- **Verify.** `tools/make-crash-raw.ps1` → `MV_CRASH_TEST=decode` → `tools/minidump-scan.ps1`
  ([DEVELOPMENT.md](../DEVELOPMENT.md#crash-reports-pr-7)).

### macOS

- **Capture.** `crashpad_handler` in `MediaViewer.app/Contents/Helpers` (signed inside-out by
  `tools/mac/macpack.py`), or beside `mediaviewer_lab`. Started synchronously at the top of
  `main()` (Crashpad has no asynchronous start on macOS), restartable. Database
  `~/Library/Application Support/MediaViewer/Crashes`. No URL, uploads disabled, indirect memory
  off, forwarding to ReportCrash off, so Apple keeps no unscrubbed copy.
- **Annotations.** The same sixteen slots and `mv_last_call_cid`, read from `core/crash_context`.
  Every routed command and every mutating chrome bridge call stamps a fresh correlation id; a
  still's decode job carries the id of the call that opened it.
- **Scrub.** The Windows scrub, plus POSIX paths under `/Users`, `/Volumes`, `/private`, `/var`,
  `/tmp`, `/home`, …, keeping module layout (`.dylib`, `.app/Contents/…`, `.framework/…`) with
  only the user component masked. Thread-stack bytes also lose an unrooted run of two or more
  path components, while `://` URLs and relative `./` / `../` runs stay. Identities: short and
  full user name, the Sharing computer name, the host name. The rewrite copies the dump's
  extended attributes onto the replacement, because the Mac Crashpad database stores report
  metadata there.
- **Chrome path.** `NSApplicationCrashOnExceptions` is on. AppKit catches an exception raised in
  event handling and calls `-[NSApplication reportException:]`, which traps without calling
  `NSUncaughtExceptionHandler`; that method and the uncaught handler both write one scrubbed text
  report to `Crashes/chrome/<ms>-cid<id>-nsexception.txt` and set an `mv_exception` annotation
  before the trap. A Swift trap is a Mach exception (`EXC_BREAKPOINT`) that Crashpad records.
- **Verify.** `tools/mac/crash_canary.py make` / `scan`; `MV_CRASH_TEST=decode | nsexception |
  swift_trap`.

---

# Part 3 — Telemetry

## Default off. Opt-in, once.

Implemented on Windows (`src/shell/telemetry.*`, `IslandHost.Telemetry.cs`). One first-run
screen in the app (never in the installer, never stacked with the default-app step), two
buttons, neither preselected; dismissing it leaves telemetry off. `settings.ini [telemetry]`
holds the choice and whether it was asked. Settings has the same switch; turning it off drops the
install id and deletes the unsent spool.

While consent is off, `record()` returns before doing anything: no id, no file, no buffer.

## What is collected

A fixed event table (`telemetry::event`); adding a row is a deliberate schema change.

| Event | Signal |
|---|---|
| `decode_failed` | Decode failures by format, plus camera model (whitelisted, extracted deliberately) |
| `session_started`, `session_ended_clean` | Crash-free session rate by version |
| `frame_pacing` | p99 frame time by GPU vendor + driver version |
| `video_decode_mode` | Hardware decode availability and silent-fallback rate |
| `feature_used` | Which panes and tools are opened at all |

## What never leaves the machine

An event is an id, at most one **tag** from a fixed vocabulary (format, decoder, GPU vendor,
camera model; ≤ 32 chars) and up to four named integers. There is no free-text field.
`looks_like_user_data()` rejects any tag containing a path separator, drive prefix, `%`, `~`,
`@`, a dot that could be an extension, NUL, or non-printable ASCII, so filenames, folders, URLs,
usernames and path hashes cannot pass. `tools/telemetry-schema-check.ps1` fails CI if the payload
schema gains a field that could carry them (the word "path" is banned from schema names).

The install id is 128 random bits, generated lazily on the first consented event and rotatable;
never a machine id, MAC, SID or anything that survives a reinstall.

Consented events are appended to a local spool (`telemetry\`). There is no upload endpoint.

---

## Not built

- Telemetry upload, and crash-report upload with a first-send prompt (dumps and spool stay local).
- Telemetry and its first-run screen on macOS.
- Staged rollout (5 % → 25 % → 100 %) gated on crash-free rate.
- Symbol-server upload of PDBs / dSYMs.
- Start-attempt rollback on macOS.
- Automated attachment of LGPL source archives to each release.
