# 13 — Updates, Crash Reporting, Telemetry

Three systems that all phone home, all touch a user's personal photo library, and are all easy to
get wrong in ways you only discover after shipping.

**The governing rule for this app:** it is pointed at people's private photos. Nothing about a
user's files — paths, filenames, folder structure, image content, thumbnails, EXIF — ever leaves
the machine. That constraint is not negotiable and it shapes every design below.

---

# Part 1 — Auto-update

## The channel decision

You have two distribution channels ([09-build-and-test.md](09-build-and-test.md)), and they update
by completely different mechanisms. Pick a **primary** rather than building both well.

| | **Store MSIX** | **Sideloaded MSIX** (`.appinstaller`) | **Signed installer + own updater** ✅ |
|---|---|---|---|
| Update mechanism | Store, automatic | App Installer polls your update URI | Your updater |
| Delta patches | Yes, block-map based | Yes | Yes (Velopack) |
| Elevation on update | None | None | **None, if installed per-user** |
| Control over timing/rollout | **Little** | Some | **Full** |
| Review latency per release | Days | None | None |
| GPL-compatible | **Possibly not** — see [11-licensing.md](11-licensing.md) | Yes | Yes |
| Reaches non-Store users | No | Awkward | Yes |

**Call: the signed installer with your own updater is the primary channel.** Two reasons — the
Exiv2 licence question may force GPL-2.0, which is a poor fit for Store terms, and a media app
needs to ship codec fixes the day they land, not after a review queue. Add the Store MSIX later as
a *secondary* channel if you want the discoverability; the core must never depend on the Store's
update mechanism existing.

**Use Velopack.** It's the current successor to Squirrel/Clowd.Squirrel, supports .NET (which suits
the C# shell from **D1**) alongside native payloads, and does delta packaging, staging, and
rollback out of the box. Writing your own updater is three weeks and a security surface.

## Per-user install — the decision that makes auto-update tolerable

Install to `%LocalAppData%\MediaViewer`, **not** `Program Files`.

A per-machine install requires elevation for *every* update. That means a UAC prompt each time,
which means users decline it, which means your update mechanism doesn't work and your telemetry
tells you 40 % of installs are three versions behind. Per-user install updates silently.

Ship a **separate per-machine MSI with auto-update disabled** for the IT/enterprise case, where
admins want to control versions anyway.

## Mechanics

### Versioned folders, not in-place overwrite

This sidesteps the locked-DLL problem entirely, which matters here more than for most apps —
`avcodec-*.dll`, `libheif.dll`, `libraw.dll`, and `exiv2.dll` are all loaded and locked whenever
the app is running, and none of them can be overwritten in place.

```
%LocalAppData%\MediaViewer\
  MediaViewer.exe        ← stub launcher; never changes, this is what shortcuts point at
  app-1.2.3\             ← current version, complete with its DLLs
  app-1.2.4\             ← staged update, written while 1.2.3 runs
  packages\              ← downloaded deltas, pruned after apply
```

The stub launches the newest version folder that has started successfully. Applying an update is
therefore an atomic directory swap and a stub pointer change — not a file-by-file replace that can
half-fail.

### Delta patches earn their keep here

Installed size is ~120 MB, and most of it is codec and FFmpeg DLLs that change rarely. A typical
app-only update is a few MB against a 120 MB full payload. Without deltas, every bug fix is a
120 MB download and users disable updates.

### Staging and restart

- Check on launch and every 6 hours; download and stage in the background at low priority.
- **Never interrupt.** No modal, no forced restart, no restart while a video is playing or an
  export/trim job is running.
- Surface a quiet "Update ready — restart" affordance in the command bar. Apply on the next natural
  launch if they ignore it.
- **Preserve state across the restart**: current folder, selected file, view mode, zoom/pan,
  playback position, and any unsaved edit stack. An update that loses the user's place is an update
  they'll remember badly.

### Rollback and the kill switch

- The stub tracks start attempts. If a new version fails to start **twice**, it falls back to the
  previous version folder and reports the failure. Keep exactly one prior version.
- The signed update manifest carries a **minimum-version and a blocklist**, so a bad release can be
  pulled rather than requiring every affected user to act. Assume you will need this — everyone
  does eventually.
- Stage rollouts: 5 % → 25 % → 100 %, gated on the crash-free-session rate from Part 2. This is
  the whole reason to own the channel rather than hand it to the Store.

### Signing

- **Sign the binaries *and* the update manifest.** The updater must verify the manifest signature
  against a pinned public key before trusting anything in it. An unsigned or unverified update
  channel is a malware delivery system you built and shipped yourself.
- Serve updates over HTTPS with certificate validation on. HTTPS is transport security, not
  authenticity — it does not replace signing the manifest.
- Use **Azure Trusted Signing** if available to you; it is far cheaper than a traditional EV
  certificate and builds SmartScreen reputation the same way. Without reputation, every early user
  gets a SmartScreen block on first run.

### Licensing obligation on every release

Each shipped build contains specific LGPL library versions. The **source offer must track the
release**, not a single frozen snapshot — publish the exact FFmpeg, libheif, libde265, and LibRaw
sources and configure lines per version, and have the About dialog link to the offer for *the
running build* ([11-licensing.md](11-licensing.md)). Automate this in the release pipeline; it is
the sort of obligation that quietly rots.

---

# Part 2 — Crash reporting

## Out-of-process, always

Use **Crashpad** (directly, or via Sentry's native SDK which wraps it). An in-process handler
cannot reliably catch stack overflow or heap corruption — the two failure modes a decoder fed a
malformed file produces most often. Crashpad's separate handler process survives your process
dying and writes the minidump regardless.

Given that decoders parse hostile input ([09-build-and-test.md](09-build-and-test.md)), this is not
a nicety. It is how you find out which camera's RAW variant crashes.

## Two capture paths, because D1 gave you two languages

The C#/C++ split means **native crashes and managed exceptions are different capture mechanisms**,
and this catches people out:

- Native crash in the core (decoder, D3D, FFmpeg) → Crashpad minidump.
- Unhandled managed exception in the shell → `AppDomain.UnhandledException` /
  `Application.UnhandledException`, captured as a .NET stack trace.
- **An exception crossing the C ABI boundary is undefined behaviour**, so the boundary must catch
  everything and translate to error codes ([01-decisions.md](01-decisions.md)) — which also means
  the reporting must correlate a managed error code back to the native failure that caused it.
  Give every core call a correlation id and attach it to both reports.

## Symbols

Upload PDBs to a symbol server at release time; **never ship them**. Without symbols a minidump is
an address you can't act on; shipping them hands out a map of your binary. Strip and upload in the
same pipeline step that signs.

## Scrubbing — this is the part that matters for this app

A crash in a decoder is *about a file*. The natural instinct is to attach the file, or its path, or
its EXIF. **Do none of that.** A minidump from a photo viewer can contain decoded pixel data in
process memory — someone's private photograph — plus the path and the GPS coordinates from the EXIF
block being parsed.

- Use a **minidump filter** that excludes heap regions holding decoded image buffers; tag those
  allocations at the arena level ([02-architecture.md](02-architecture.md)) so the filter can find
  them.
- Attach the **format, codec, dimensions, bit depth, and decoder version** — never the path,
  filename, or bytes.
- Scrub the username out of every path in the stack (`C:\Users\<name>\…` → `%USER%\…`).
- **Ask before the first send**, plainly, once. "MediaViewer crashed. Send a report? It contains
  technical details about the crash, not your photos." — with a link to exactly what's in it.

---

# Part 3 — Telemetry

## Default off. Opt-in, once, honestly.

One screen on first run, a real choice, no dark pattern, no pre-ticked box, and a setting that
turns it off later and actually does. For an app whose whole job is looking at people's private
photos, anything else is a betrayal of the use case — and it will be the thing people write about.

**Be honest that the update check itself is a network call.** It reveals IP, version, and rough
timing even with telemetry off. Say so in the privacy note, and offer a setting to disable
automatic checks for users who want the app fully offline.

## What is genuinely worth collecting

Only things that change what you'd build, and only in aggregate:

| Signal | Why it earns its place |
|---|---|
| **Decode failures by format + camera model** | The single highest-value signal here. You cannot personally assemble a corpus of every camera's RAW variant; your users have one. A spike on a new phone's HEIC tells you what to fix. |
| **Crash-free session rate, by version** | Gates the staged rollout in Part 1. |
| **p99 frame time by GPU vendor + driver version** | The pacing gate from **D6** holds on your machine. This says whether it holds on an old Intel iGPU with a 2019 driver. |
| **Hardware decode availability + fallback rate** | Tells you when silent software-decode fallback is hurting real users ([05-video-pipeline.md](05-video-pipeline.md)). |
| **Feature reach** (which panes/tools are opened at all) | Kills features nobody uses before they accrete maintenance. |

## What never leaves the machine

Absolute, no exceptions, enforced by a CI check on the telemetry payload schema:

- File paths, filenames, folder structure, drive labels
- Image or video content, thumbnails, previews, pixel data of any kind
- **EXIF, XMP, IPTC — all of it.** It contains GPS coordinates, camera serial numbers, and
  frequently the owner's name. Camera *model* is a whitelisted exception, extracted deliberately,
  never a metadata blob forwarded wholesale.
- Anything derived from the above, including hashes of paths — a path hash is still a stable
  identifier for a specific private file.

Use a **random, rotatable, non-reversible install id**. Never a machine id, MAC address, Windows
SID, or anything that survives a reinstall.

---

# Sequencing

The roadmap ([10-roadmap.md](10-roadmap.md)) puts all three in PR 15. That's too late for two of
them:

- **The updater must exist in the first build that reaches anyone else's machine.** If your first
  external testers install a version with no update path, they are stranded there and every later
  fix requires them to manually reinstall. Ship it before the first external build, not at v1.0.
- **Crash reporting should land with the format long tail (PR 7)**, which is when you first feed
  real RAW and HEIC files from other people's cameras into decoders you've never tested against
  them. That's precisely the window where crash reports are worth the most.
- **Telemetry can wait for PR 15.** It informs v1.1 priorities, not v1 correctness.
