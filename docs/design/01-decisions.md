# 01 — Stack Decisions

The technology stack MediaViewer is built on, and the settled decisions D1–D9 that code comments cite, each with the reason it holds.

## The stack

**A C++20 core behind a flat C ABI, native chrome per OS, a native canvas per OS.**

| Layer | Windows | macOS |
|---|---|---|
| Core (decode, colour, edit, metadata, jobs) | C++20, shared | C++20, shared |
| Boundary | Flat C ABI ([14-abi.md](14-abi.md)) | Same header |
| Chrome | C# WinUI 3, hosted as XAML islands in a Win32 window | SwiftUI, hosted in an AppKit window |
| Canvas / present | Direct3D 11, flip-model composition swapchain in a DirectComposition visual | Metal, `CAMetalLayer` paced by `CAMetalDisplayLink` |
| Video decode | FFmpeg + D3D11VA on the app's `ID3D11Device` | FFmpeg + VideoToolbox on the app's `MTLDevice` |
| Audio (master clock) | WASAPI shared mode | Core Audio output AudioUnit |
| Debug overlay / present lab | Dear ImGui (DX11 binding) | Dear ImGui (Metal binding) |

Bundled libraries (vcpkg manifest, pinned baseline; [`vcpkg.json`](../../vcpkg.json)): libjpeg-turbo,
libspng, giflib, libwebp, libtiff, libheif (+ libde265), libavif + dav1d, LibRaw, Little CMS, Exiv2,
SQLite, FFmpeg, Crashpad, BLAKE3, libsodium, Catch2. On the Mac, the dynamically linked set (libheif,
LibRaw, FFmpeg, Exiv2) comes from a separate dynamic triplet
([`tools/mac/dependencies/vcpkg.json`](../../tools/mac/dependencies/vcpkg.json)). The build is CMake +
vcpkg in manifest mode ([09-build-and-test.md](09-build-and-test.md)).

**Why C++ and not a web stack.** Electron/Tauri composite through a browser renderer: no waitable
swapchain, an extra frame or more of latency, a copy per video frame into a canvas, second-class colour
management, and a ~150 MB / ~400 ms baseline before the first pixel. The app's purpose is pacing and
first-pixel time, so the present path has to be its own.

**Why C++ and not C.** D3D11/DXGI/WIC are COM, and the edit stack, format registry and undo want value
semantics and destructors. The core uses a narrow subset: RAII with `std::unique_ptr` / `ComPtr`,
`std::span`, `std::string_view`, designated initializers, `std::expected`-style results instead of
exceptions on the hot path, no RTTI, no `std::shared_ptr` in the render loop. MSVC builds with
`/W4 /WX /permissive- /GR- /utf-8`.

**Why D3D11 and not D3D12/Vulkan.** D3D11 has a free-threaded device (decode workers create textures
directly), flip-model swapchains and DirectComposition interop. A viewer draws about ten quads a frame;
the bottleneck is decode and upload, not draw submission, so D3D12's lower driver overhead buys nothing.
Each OS gets its own native API (D3D11 on Windows, Metal on macOS); there is no portability layer
(Vulkan, MoltenVK, wgpu) under either.

---

## D1 — App shell: native chrome + C++ core

Windows chrome is **C# WinUI 3**; macOS chrome is **SwiftUI hosted in AppKit**. Both talk to the same
core through the same C ABI. ImGui is the present lab and the F3 overlay on both, not shipped chrome.

Native chrome gives a virtualizing filmstrip, folder tree, accessibility, IME and system theming without
hand-writing them; C# is used on Windows because C++/WinRT XAML is slow to develop exactly where the
chrome lives. The core never sees XAML, C# or Swift, which is what makes the second host possible.

**The shell is native, the chrome is hosted.** On Windows the app's entry point is a C++ Win32
top-level window that owns the D3D11 swapchain (in a DirectComposition visual tree). The WinUI chrome
is hosted inside it as XAML content islands (`DesktopWindowXamlSource`), one island per chrome strip
([02-architecture.md](02-architecture.md)). On macOS the AppKit window owns the `CAMetalLayer` and
hosts SwiftUI views (`NSHostingView`) beside the canvas. In both, there is one present path and C++
owns it.

Consequences of composition: the swapchain is created with `ALLOW_TEARING` when supported, but a
composition swapchain cannot tear, so the shipped app always presents with vsync and VRR is not used.
The performance gate is therefore **zero dropped frames while panning a cached image at the display's
refresh rate** (see D6). The keyboard model — one command table, every command reachable without the
mouse — is [16-commands.md](16-commands.md).

## D2 — Video: FFmpeg + OS hardware decode on the app's own present path

Windows decodes with FFmpeg + **D3D11VA** on the app's `ID3D11Device`; macOS with FFmpeg +
**VideoToolbox** on the app's `MTLDevice`. Video frames are copied into a presentation ring the app owns
and drawn on the same swapchain as photos ([05-video-pipeline.md](05-video-pipeline.md)). There is one
video pipeline per OS, no child player window, and no `AVPlayer` in the viewer.

FFmpeg is the single path because the same camera dump holds HEVC, AV1 and VP9 clips, which Media
Foundation only decodes with Store extensions (see D3). The cost is owning the A/V clock: audio is the
master clock (WASAPI / Core Audio), with QPC / host time for clips without audio. The video source sits
behind an `IVideoSource`-style interface ([`src/player/media_source.h`](../../src/player/media_source.h));
an `IMFMediaEngine` source was the reserved fallback and was never needed.

## D3 — Codecs: bundled, never a Store pack

Every decoder the D5 set needs ships with the app, so an iPhone HEIC photo and its HEVC video open on a
clean install. The OS codec is probed first where it can match the bundled result, and any failure falls
back to the bundled decoder silently ([`src/codec/os_decode.h`](../../src/codec/os_decode.h)):

- **Windows:** only HEIC stills are offered to WIC, and only when WIC has a HEIF decoder *and* an HEVC
  MFT is registered, and the file is a case WIC can reproduce exactly (8-bit, no alpha, not HDR, ICC or
  sRGB nclx). Every other format always uses the bundled decoder. `MV_OS_CODEC=0` forces the bundled
  path.
- **macOS:** the hook exists but always declines (ImageIO is not wired), so HEIC uses libheif + libde265.

The app never ships a software HEVC encoder; re-encode uses hardware encoders only
([08-video-editing.md](08-video-editing.md), [11-licensing.md](11-licensing.md)).

## D4 — Scope: a viewer first

The first release (packaged in PR 8) is the viewer of PRs 1–7: browsing, playback, viewer controls and
the camera-dump formats. Metadata reading and editing, light photo edits and export, video trim, and
Explorer/Finder integration followed as updates (PRs 9–15) on both platforms. The light-edit set is
rotate/flip/crop/straighten/resize plus exposure/contrast/saturation/temperature
([07-photo-editing.md](07-photo-editing.md)). The app is a viewer with light editing — not a develop
module, NLE or movie player; the larger editing surfaces are their own documents
([20-edit-workspace.md](20-edit-workspace.md), [21-video-editor.md](21-video-editor.md)).

## D5 — Formats: the camera-dump set

| Kind | Formats |
|---|---|
| Stills | JPEG, PNG (incl. APNG), BMP, GIF (incl. animated), TIFF, WebP (incl. animated), HEIC/HEIF, AVIF, ICO |
| RAW | CR2/CR3, NEF, ARW, RAF, ORF, RW2, PEF, DNG — embedded preview first, full decode as refinement |
| Containers | MP4/M4V, MOV, MKV, WebM, AVI, TS |
| Video codecs | H.264, HEVC, VP9, AV1, MPEG-2; **on macOS also Apple ProRes** (422 and 4444, in MOV) |

AVIF is in the set because dav1d already ships for AV1 video. ProRes is Mac-only because Apple silicon
decodes it in hardware through VideoToolbox (decoded as 10-bit 4:2:0); on Windows it would be a software
decode not measured to hold 4K pacing. Formats are probed by magic bytes, never extension, and adding
one is one decoder file plus one registry line ([04-image-pipeline.md](04-image-pipeline.md)).

## D6 — Colour: linear working space, SDR swapchain

Two separate choices:

| | Value | Where |
|---|---|---|
| Edit working space | **Linear-light FP16, Rec.709/sRGB primaries** | `image/linear.h`, edit chain, export bake |
| Viewer textures | 8-bit sRGB (`R8G8B8A8_UNORM_SRGB` / `RGBA8Unorm_sRGB`) | `gfx/texture.cpp`, `image/upload_mac.mm` |
| Swapchain | 8-bit, sRGB render target view (`R8G8B8A8_UNORM` + `_SRGB` RTV; `BGRA8Unorm_sRGB` on Metal) | `gfx/swapchain.cpp`, `gfx/metal_layer.mm` |

The working space is the part that cannot be retrofitted; the swapchain format is a runtime branch
(`swapchain_desc::hdr_output`, FP16 scRGB) that is wired but always off, because nearly every display
is SDR and an FP16 swapchain doubles present bandwidth for nothing.

**Colour policy:** an untagged image is sRGB; a tagged image is transformed from its embedded ICC
profile (or CICP/nclx primaries and transfer) through Little CMS. Display-referred sources (JPEG, HEIC,
PNG, …) are never tone-mapped. PQ/HLG sources are tone-mapped to SDR: stills at decode, video in the
YUV→RGB shader
([03-rendering.md](03-rendering.md#color-management--hdr)).

**Performance gate:** zero dropped frames while panning a cached image at the display's refresh rate,
measured by the frame-time harness ([09-build-and-test.md](09-build-and-test.md)).

## D7 — Video trim: two labelled paths

Trim offers **keyframe trim** (stream copy, instant, snapped to the keyframe grid drawn on the scrub
bar) and **full re-encode** (frame-accurate, hardware encoder, labelled slower). Smart cut (re-encode
only the head and tail GOPs) is not built: a mismatched encode makes a visible quality step at each seam
([08-video-editing.md](08-video-editing.md)).

## D8 — Decode sandboxing

Decoders run in-process. Hostile-file safety comes from the broken-file corpus and per-decoder libFuzzer
harnesses ([09-build-and-test.md](09-build-and-test.md)) and from Crashpad crash reports
([13-updates-and-telemetry.md](13-updates-and-telemetry.md)). An AppContainer decode process is not
built. Explorer thumbnails and Quick Look run out of process, so a decoder crash never takes down the
desktop shell.

## D9 — Platforms: one core, two hosts

MediaViewer ships on **Windows** and **macOS**. The macOS app is a second host of the same native core,
not a UI port: it has its own Metal present path and pacing gate, VideoToolbox decode, Core Audio clock,
POSIX I/O, FSEvents directory watch, and its own installer and updater. Every feature from PR 9 on has a
Windows half and a Mac half over one shared core change. The core stays hostable because Windows-only
and Apple-only APIs live only in the hosts and in `*_win` / `*_mac` backends behind narrow ports
([15-platforms.md](15-platforms.md)).

| | Windows | macOS |
|---|---|---|
| OS floor | Windows 10 21H2+ | macOS 14+ |
| CPU | x64 | Apple silicon and Intel (one universal app; AI features arm64 only) |

---

## Third-party linkage

FFmpeg, libheif, libde265, LibRaw and Exiv2 are dynamically linked; everything else is linked
statically. FFmpeg is configured LGPL-only (no `--enable-gpl`, no `--enable-nonfree`, no x264/x265),
which `tools/licence-check.ps1` checks. Exiv2 is GPL, so the app as a whole is **GPL-3.0-or-later**;
this also keeps it out of both app stores. Details: [11-licensing.md](11-licensing.md).

The vcpkg baseline is pinned so the decoder versions match between machines and CI (golden images
depend on it).
