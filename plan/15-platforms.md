# 15 — Platforms

**D9.** v1 is a Windows app. The native core is kept hostable from PR 4 so a Mac app is a
second host of the same decode / colour / edit / metadata library, not a rewrite of those.
The Mac app is **Milestone F** ([10-roadmap.md](10-roadmap.md)), after Windows ships.

This document is the rule set. Do not implement Metal, Swift, VideoToolbox, or a
`*_mac.cpp` during PRs 1–15. Do not skip a D9 port in those PRs in order to call Win32
from `image/`, `player/`, `edit/`, or `meta/`.

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

**v1 is Windows. macOS is the next product, specified now, built after PR 15.** PR 1–3
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
- **No empty `*_mac.cpp` in PRs 1–15.** A stub that does not present is not a port
  and will bit-rot. The Windows file is `*_win.cpp` (or lives in `shell/`) when a
  port is required; the Mac file arrives with Milestone F, as a real implementation.
- **No Swift project, no Metal, no Cocoa, no Catalyst in v1.**

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
Windows kernels in v1 are HLSL only; when a new kernel lands in PRs 4–15, write it
so a line-for-line MSL twin is possible (no HLSL-only syntax in the *algorithm*,
register binding documented in a comment). Do not introduce FXC/DXC → SPIR-V → MSL.
Do not share bytecode.

## Milestone F — the Mac host (PR 16–20)

Do not start F until PR 15's verify holds **and** PR 1's present-loop verify still
holds on Windows. F has its own present-loop gate; a DXGI JSON report is not
evidence on Metal.

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

Same ABI, same decoders, same LCMS policy (D6). Immutable Metal texture upload from
the worker pool, fit / wheel-zoom-toward-cursor / drag-pan, `0` / `1`. First blit
shader gets its MSL twin here.

**Verify:** a 12 MP JPEG pans at refresh with zero decode on mouse move; a tagged
AdobeRGB JPEG renders correctly and an untagged one is treated as sRGB, with **no
tone-map applied to either**.

### PR 18 — SwiftUI chrome, hosted in the AppKit window

**The canvas is not ported to SwiftUI.** PR 16's AppKit window and `CAMetalLayer`
stay exactly as built; SwiftUI chrome is hosted inside them. Command bar and window
chrome only — panes come with the same features they have on Windows, not earlier.

**Verify:** zero dropped frames while panning a cached image at display refresh,
unchanged from PR 17 now that chrome is on screen. Focus and keyboard traversal
cross the SwiftUI / canvas boundary; a popover opens over the canvas without
clipping.

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
Silicon only. Crash reporting already exists from PR 7; the Mac minidump path
scrubs the same (no paths, filenames, pixels, EXIF).

**Verify:** double-clicking a HEIC in Finder opens the app; a deliberately
corrupted HEIC in a browsed folder leaves Finder running; a clean Mac → install
from the notarized image → open a real camera dump, with no Gatekeeper block and
no codec dialog.

## What v1 (PR 4–15) must not do

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

| | Windows (PR 15) | macOS (PR 20) |
|---|---|---|
| Install | Per-user `%LocalAppData%\MediaViewer` | Per-user `~/Applications` or a dragged `.app` |
| Update | Velopack, signed manifest | Sparkle, notarized, stapled |
| Signing | Azure Trusted Signing | Developer ID + notary |

The privacy line does not change: nothing about a user's files leaves the machine
([13](13-updates-and-telemetry.md)).
