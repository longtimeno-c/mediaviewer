# 15 — Platforms

**D9.** v1 is a Windows app. The native core is kept hostable from PR 4 so a Mac app is a
second host of the same decode / colour / edit / metadata library, not a rewrite of those.
The Mac app is **Milestone F** ([10-roadmap.md](10-roadmap.md)), after Windows ships.

This document is the rule set. Metal, Swift, and `*_mac.cpp`/`*_mac.mm` work is authorized
to proceed during Windows v1 for PRs 16–20 only (widened 2026-09-17 from the PR-16-only
exception of 2026-09-13 — [12](12-decision-log.md)); VideoToolbox specifically waits for
PR 19, in F's own sequence. The hostable-core rules continue through future Windows
updates (PRs 9–15) regardless; do not call Win32 from `image/`, `player/`, `edit/`, or
`meta/`.

## This is not a UI update

The C ABI exists so chrome can be native per OS without rewriting decode. That is the
true part. The false part is “therefore Mac is SwiftUI on top of the Windows C++.”

| Transfers to Mac unchanged | Does **not** transfer |
|---|---|
| `codec/`, `image/` colour, `edit/` op graph, `meta/`, `canvas/` springs, `core/` jobs, the C ABI | Present path, GPU backend, hardware decode, audio clock, async I/O, directory watch, atomic replace, windowing, chrome, installer |
| libjpeg-turbo / libspng / libwebp / libheif / LibRaw / Exiv2 / LCMS / SQLite | D3D11, DXGI, DComp, D3D11VA, WASAPI, `HWND`, `OVERLAPPED`, `ReplaceFileW`, WinUI, Velopack |
| UTF-8 paths across the ABI | `wchar_t` Win32 paths, `ID3D11*` in public headers |

Chrome is the visible half of Milestone F. The other half is a **Metal present lab**
with its own 60 s gate, VideoToolbox on *your* `MTLDevice`, Core Audio as the master
clock, POSIX I/O, and a notarized Sparkle pipeline. A Windows DXGI pass is not a Mac
pass. Sharing chrome by rewriting it in C++ throws **D1** away; sharing it via
Qt / Flutter / MAUI / Catalyst throws D1 away a different way.

## The call

| | **Windows only, rewrite Mac later** | **Windows v1, hostable core, Mac as Milestone F** ✅ | **Windows and macOS in v1** |
|---|---|---|---|
| Windows v1 date | Unchanged | Unchanged — hostable-core tax per PR from 4 | **Slips** — two present labs, two chromes, two ship pipelines |
| Present path | One, forever Windows-shaped | One per OS; Mac proven in F, not hoped | One per OS, both green before v1 |
| Chrome | WinUI baked into the core | WinUI now; SwiftUI in F; same C ABI | WinUI and SwiftUI in parallel from PR 4 |
| Mac later | A rewrite of decode *and* present | A host + backends, specified now | The remaining Windows PRs become dual-track |
| Reversibility | Hard (wrong direction) | Easy if Mac never happens | Hard — a Metal lab in v1 is sunk cost |

**v1 is Windows. macOS is the next product, specified now, built after PR 8.** PR 1–3
are not retrofitted. Putting both in v1 would pause the Windows viewer for a present
path the Windows user does not need. Treating Mac as “just SwiftUI” would ship a Mac
app with no present-loop gate.

D1, per OS: native chrome + C++ core. Windows remains C# WinUI 3 hosted in the native
window. macOS is SwiftUI hosted in an AppKit window that owns a `CAMetalLayer`. ImGui
is the present lab and the F3 overlay on both, never shipped chrome.

D2, per OS: FFmpeg + hardware decode on *your* device, presented on the same swapchain
as photos. Windows remains D3D11VA; macOS is VideoToolbox. `IMFMediaEngine` stays a
Windows-only escape hatch. **`AVPlayer` is forbidden** — it is the Mac version of
child-HWND mpv.

Forbidden, unchanged: **do not introduce Electron, Tauri, Node, D3D12, Vulkan, or a
second present path on one OS.** Metal is macOS's first present path, not a second one
on Windows. No MoltenVK, no wgpu, no SPIR-V, no third shader language.

## Floors

| | Windows v1 | macOS (Milestone F) |
|---|---|---|
| OS | Windows 10 21H2+ | macOS 14+ |
| CPU | x64 | Apple Silicon |
| Deferred | Windows ARM64 | Intel Macs |

Intel Macs are a second GPU story for a dying install base. Windows ARM64 waits for
the same reason: a second present / decode path, not a compile flag.

## Hostable-core rule — from PR 4, every remaining Windows PR

New native code does not take a Windows-only dependency a Metal / AppKit / SwiftUI
host cannot replace.

- **`HWND`, `ID3D11*`, `IDXGI*`, `wchar_t` paths, `OVERLAPPED`, WASAPI, `ReplaceFileW`**
  stay in `shell/` and the Windows backends under `gfx/`, `io/`, `player/`.
- Headers consumed *above* `gfx/` do not include `d3d11.h`.
- Paths across the C ABI stay **UTF-8**. The Windows host converts at the boundary.
- Canvas input is a **POD snapshot** the host publishes ([02](02-architecture.md)).
  The Mac host publishes the same struct from AppKit / SwiftUI events.
- **Key bindings live in the host** ([16-commands.md](16-commands.md)). The core sees
  command effects through the ABI, never `VK_*` or Win32 accelerators. The Mac host
  writes its own default map; it does not import a XAML keymap.
- Completions stay a queue the host pumps. C++ does not call a WinUI dispatcher
  *or* `DispatchQueue.main`.
- **No empty `*_mac.cpp` in PRs 1–8.** A stub that does not present is not a port
  and will bit-rot. The Windows file is `*_win.cpp` (or lives in `shell/`) when a
  port is required; the Mac file arrives with Milestone F, as a real implementation.
- **No Swift project, no Metal, no Cocoa, no Catalyst in Windows v1**, except the
  already-authorized PR 16 Metal present lab.

`tools/check-module-graph.ps1` already forbids native modules depending on `shell/`.
`tools/check-hostable-core.ps1` (PR 4) fails a direct `#include` of `d3d11.h`,
`<windows.h>`, or `<atlbase.h>` from `core/`, `codec/`, `canvas/`, `image/`,
`meta/`, `player/`, `edit/`, and from `io/*.h`. Windows I/O and watch stay in
`io/*_win.cpp`. A leak that “saves a day” in PR 5a is a rewrite in PR 16.

`image/gpu_image.h` still pulls D3D11 *transitively* through `gfx/device.h`. That
is a PR 2 leftover, not a licence to add more. Do not include `gfx/device.h` from
new headers above `gfx/`. Opaque GPU resources (pimpl, `native.h` accessors) are
the fix when a new type would otherwise leak `ID3D11*` upward.

The narrow port, when F starts, is a header plus a real `*_win.cpp` and `*_mac.cpp`
for: gfx (device, texture, blit, present), io (async read, directory watch, atomic
replace), hwdecode, audio, encode. Not a general RHI.

## Shaders

Hand-written **HLSL and MSL twins** from the first kernel the Mac path needs. The
Windows kernels are HLSL; throughout PRs 4–15, write each new kernel
so a line-for-line MSL twin is possible (no HLSL-only syntax in the *algorithm*,
register binding documented in a comment). Do not introduce FXC/DXC → SPIR-V → MSL.
Do not share bytecode.

## Milestone F — the Mac host (PR 16–20)

F no longer waits for PR 8's verify to start (widened 2026-09-17, below), but **PR 1's
present-loop verify must still hold on Windows**, independently, and F has its own
present-loop gate on Mac; a DXGI JSON report is not evidence on Metal.

**Sequencing exception (2026-09-13, widened 2026-09-17):** PR 16 was authorized to
proceed in parallel with Windows v1 on 2026-09-13. On 2026-09-17 the owner widened
this to PRs 17–20 as well — Milestone F no longer waits on Windows PR 8. D9 itself
(Mac is a host, not a UI port) is unchanged; F's internal sequencing (16 before 17
before 18 before 19 before 20, each verify line before the next PR starts) is
unchanged too. [12](12-decision-log.md).

### PR 16 — Metal present lab

AppKit window, `CAMetalLayer`, display-link pacing (`CAMetalDisplayLink` on macOS 14,
not `Sleep`, not a display-link that waits *after* encode), max drawable 1, idle →
stop presenting, F3 overlay reading real present-to-present intervals, `frametime`
on Darwin. Job system and ABI already exist. **No SwiftUI yet** — this is the
instrument, and it stays as a debug harness the way the Win32 lab does.

**Verify:** presents at exactly display refresh, 0 dropped frames over 60 s, ~0 %
CPU idle, **on Apple Silicon, measured from the Metal / display-link side**. A
Windows soak copied over is not this verify.

### PR 17 — Still decode + pan/zoom, on Metal

Same ABI, same decoders, same LCMS policy (D6), starting from JPEG/PNG/BMP. Immutable
Metal texture upload from the worker pool, fit / wheel-zoom-toward-cursor / drag-pan, `0` /
`1`. First blit shader gets its MSL twin here.

**Folded in (2026-09-17, [12](12-decision-log.md) — no PR 16–20 slot existed for Windows
PR 7):** TIFF, WebP, ICO, HEIC/HEIF, AVIF, RAW via LibRaw with embedded-preview-as-first-pixel,
tiled pyramid above ~64 MP, broken-file corpus + libFuzzer harnesses, and Crashpad + the Mac
minidump scrub — landing here for the reason PR 7 paired them on Windows: this is where
hostile real-world files first meet Mac decoders.

**Verify:** a 12 MP JPEG pans at refresh with zero decode on mouse move; a tagged
AdobeRGB JPEG renders correctly and an untagged one is treated as sRGB, with **no
tone-map applied to either**. iPhone HEIC opens with no extra codec install; a CR2/NEF/ARW
shows a preview in JPEG-comparable time with the full decode replacing it without a visible
pop; original RAW bytes unchanged; nothing in the broken-file corpus crashes or hangs; a
deliberately corrupted RAW produces a minidump with no path, filename, or pixel data.

### PR 18 — SwiftUI chrome, hosted in the AppKit window

**The canvas is not ported to SwiftUI.** PR 16's AppKit window and `CAMetalLayer`
stay exactly as built; SwiftUI chrome is hosted inside them. Command bar and window
chrome only — panes come with the same features they have on Windows, not earlier.

**Folded in (2026-09-17, [12](12-decision-log.md) — no PR 16–20 slot existed for Windows
PR 4/6):** folder listing + sort, `kqueue`/`FSEvents` dir watch (`io/dir_mac.cpp`), a
SwiftUI filmstrip + gallery over the same folder model and JPEG-512 thumbnail cache spec
(`jpg512.1`) as Windows, and keyboard-complete browse — one key router with a Mac default
map (`⌘` not `Ctrl`, same command ids as [16-commands.md](16-commands.md)), `?` overlay,
marks, copy-to/move-to, Trash (not Recycle Bin) with confirm, drag-and-drop in and out,
argv handling, fullscreen, slideshow as a mode, fit/100%/fill, animated GIF/APNG/WebP on
the display-link frame clock. RAW+JPEG and Live Photo pairs surface as one filmstrip stop
here, using the pairing detection PR 17 lands decode-side.

**Verify:** zero dropped frames while panning a cached image at display refresh,
unchanged from PR 17 now that chrome is on screen. Focus and keyboard traversal
cross the SwiftUI / canvas boundary; a popover opens over the canvas without
clipping. 2000 mixed JPEGs — filmstrip scrolls without a hitch, second folder visit has
near-instant thumbnails; keyboard-only browse — open, next/prev, zoom, mark, copy-to,
delete to Trash, fullscreen, slideshow — without the mouse, `?` listing those bindings; a
RAW+JPEG pair and a Live Photo are each one filmstrip stop.

### PR 19 — VideoToolbox + Core Audio

FFmpeg + VideoToolbox on *your* `MTLDevice`, decoded surfaces copied into a
presentation ring you own (same DPB rule as D3D11VA — do not present decoder-pool
memory), NV12 and 10-bit sample paths, YUV→RGB plus HDR→SDR in the MSL twin.
Core Audio render, audio-master clock, silent-clip QPC fallback. **No `AVPlayer`.**
No second `AVSampleBufferDisplayLayer`.

**Verify:** 4K 10-bit HEVC plays at full rate with VideoToolbox active, on a clean
Mac with no extra codec packs; an iPhone HLG clip looks correct; A/V drift flat
over 30 minutes; photo → video → photo leaks no textures. `AVPlayer` does not
appear in the process.

### PR 20 — Finder + notarized ship

UTIs for the D5 still set, one “Open with” registration, never a silent default-app
hijack. Quick Look / thumbnail generation in a **separate process** — loading
libheif / LibRaw / FFmpeg into Finder is how you crash the desktop, same landmine
as in-process Explorer handlers. Notarized, stapled, Sparkle updates, Apple
Silicon only. Crash reporting already exists from PR 17 (folded in 2026-09-17, mirroring
Windows PR 7); the Mac minidump path scrubs the same (no paths, filenames, pixels, EXIF). First install is the Mac
twin of the PR 8 wizard: a branded drag-install disk image with the GPL shown on
mount, not a `.pkg` ([13](13-updates-and-telemetry.md#macos-first-install--a-branded-disk-image-pr-20)).

**Verify:** double-clicking a HEIC in Finder opens the app; a deliberately
corrupted HEIC in a browsed folder leaves Finder running; a clean Mac → mount the
notarized image (GPL shown) → drag to Applications → open a real camera dump, with
no Gatekeeper block and no codec dialog; Sparkle takes N → N+1 silently and refuses
an appcast signed with any other key; dragging the app to the Trash removes the
Quick Look extension.

## Windows v1 surface on Mac

Milestone F is five PRs, not a dual-track of 4–15. Feature parity is still the
goal: every Windows v1 behaviour has a Mac home. Decode / colour / EditStack /
metadata / canvas springs / the C ABI **transfer**. Chrome, present, hwdecode,
audio, I/O, and ship **do not**. This table is the checklist so a feature is not
forgotten because it was "not a Mac PR."

**2026-09-17:** PR 4, PR 6, and PR 7 originally had no PR 16–20 slot at all (see the
parity note in [10-roadmap.md](10-roadmap.md)). The owner folded them into PR 17/18
rather than adding new PR numbers; the rows below now point at those, not "after PR 18."

| Windows v1 | Transfers? | Mac home |
|---|---|---|
| PR 1 present lab, F3, idle-stop, 60 s gate | No | **PR 16** — AppKit + `CAMetalLayer` + `CAMetalDisplayLink`, `frametime` on Darwin |
| PR 2 JPEG/PNG/BMP, LCMS, pan/zoom springs | Decoders, colour, `canvas/` | **PR 17** — immutable Metal upload, first MSL blit twin |
| PR 3 command-bar chrome | ABI only | **PR 18** — SwiftUI hosted in the AppKit window; canvas stays Metal |
| PR 4 folder, filmstrip, gallery, JPEG-512 thumbs, dir watch | folder ABI, SQLite thumbs | **PR 18** — SwiftUI filmstrip + gallery; `io/dir_mac.cpp` (`kqueue` / `FSEvents`). Same listing, same cache spec `jpg512.1` |
| PR 5a/b/c video, WASAPI clock, transport | `IVideoSource`, clock *policy* | **PR 19** — FFmpeg + VideoToolbox on *your* `MTLDevice`, Core Audio master clock. **No `AVPlayer`.** Bindings from [16](16-commands.md) with a Mac default map |
| PR 6 keyboard-complete browse, slideshow, Recycle, DnD, argv | command *effects* via ABI | **PR 18** — Mac host default map (`⌘` not `Ctrl`). `?` overlay, `⌘K` palette, Trash not Recycle Bin. Same command ids. Remap UI still v1.1 |
| PR 7 HEIC/AVIF/RAW/TIFF/WebP/ICO, pairing, fuzz, crashpad | `codec/` | **PR 17** — same decoders, same fuzz corpus, Crashpad + Mac minidump scrub, same privacy line ([13](13-updates-and-telemetry.md)) |
| PR 9 metadata read, info overlay, AF points, eyedropper | `meta/` | SwiftUI metadata pane; `I` focuses it |
| PR 10 geometry + lossless JPEG rotate | `edit/` | SwiftUI crop mode; `[` `]` from the viewer. MSL twins of the geometry kernels |
| PR 11 exposure/contrast/sat/temp | `edit/` | SwiftUI adjust pane; sliders still wait for full RAW decode |
| PR 12 rating / orientation / comment, XMP sidecar | writers | Same sidecar rule. Atomic replace is `io/replace_mac.cpp` (`rename` + `FSEVENTS`), never rewrite a RAW original |
| PR 13 two-path trim | remux/encode policy | Same two paths, labelled. Hardware encode is VideoToolbox, not NVENC/QSV/AMF |
| PR 14 extract & remux | same | Same operations, SwiftUI job panel |
| PR 15 Explorer associations, OOP thumbnails (reuses PR 8 identity) | No | **PR 20** — UTIs for the D5 still set, never a silent default hijack. Quick Look in a **separate process** |
| PR 8 Inno + Velopack, app identity | No | **PR 20** — branded drag-install `.dmg` (GPL on mount), notarized Sparkle, same mark as `.icns` |

Keyboard: the Mac host writes its own default map. It does not import a XAML
keymap and it does not put `VK_*` or Carbon key codes into `image/`, `player/`,
`edit/`, or `meta/` ([16](16-commands.md)).

Do not ship a Mac build that is "SwiftUI on the Windows present path," and do
not ship one that is "Metal present lab plus `AVPlayer` for video."

## What Windows work (PRs 4–15) must not do

These are the leaks that turn Milestone F into a rewrite:

- `d3d11.h` or `windows.h` in `canvas/`, `image/`, `edit/`, `meta/`, `codec/`, `core/`.
- Presenting a D3D11VA surface that a Metal host cannot own. The presentation ring
  is already required on Windows ([05](05-video-pipeline.md)); keep it abstract.
- Win32-shaped I/O in `io/` headers (`HANDLE`, `OVERLAPPED` as API).
- Completions that assume a WinUI dispatcher.
- Shaders that can only be expressed in HLSL.
- “We'll `#ifdef _WIN32` in `image/` just this once.”

## Distribution, per OS

Neither Store (GPL — [11-licensing.md](11-licensing.md)).

| | Windows (PR 8) | macOS (PR 20) |
|---|---|---|
| First install | Inno Setup wizard, once | Branded `.dmg`, drag to `/Applications` or `~/Applications`, GPL on mount |
| Install | Per-user `%LocalAppData%\MediaViewer` | The dragged `.app`; no `.pkg`, no root, no helper |
| Update | Velopack, signed manifest | Sparkle 2, EdDSA-signed appcast, notarized, stapled |
| Uninstall | Inno uninstaller | Drag to Trash |
| Signing | Azure Trusted Signing | Developer ID + notary |

The privacy line does not change: nothing about a user's files leaves the machine
([13](13-updates-and-telemetry.md)).
