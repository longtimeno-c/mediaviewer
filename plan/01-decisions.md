# 01 — Stack Decisions

## The recommendation

**C++20 core behind a flat C ABI. Native chrome per OS: C# WinUI 3 on Windows, SwiftUI/AppKit on
macOS. Native canvas per OS: Direct3D 11, Metal.** Not straight C, not Node/npm, not a shared UI
toolkit.

The core — decode, render, edit, metadata — is a UI-free C++ lib behind a flat C ABI. Chrome is
native because a daily-driver viewer needs a virtualizing filmstrip, accessibility, and IME, and
hand-rolling those is a year that isn't the product. See **D1** and **D9** below. The Windows
side replaces an earlier Win32 + Dear ImGui recommendation; the Mac side is the same argument
in SwiftUI, not a port of XAML.

## Why not JavaScript (Electron / Tauri / "nitro" native modules)

You asked about npm native-module routes (Nitro modules, or the more mature `napi-rs` /
`node-gyp` path). They are genuinely fast *for the native part* — the problem is everything
around it:

- **You do not control presentation.** Chromium composites your frames through its own
  renderer/GPU-process pipeline. You inherit an extra 1–3 frames of latency and you cannot use a
  waitable swapchain, `ALLOW_TEARING`, or DirectComposition-level control. "Smooth like butter"
  at 144/240 Hz with a 60 Mpx RAW or an 8K HDR clip is exactly the case that breaks.
- **Zero-copy video is out of reach.** You'd decode into a GPU texture and then hand pixels back
  to JS to get them into a `<canvas>`/WebGL context — a copy per frame, or a fight with
  `VideoFrame`/WebCodecs, which does not expose ProRes, DNxHD, most RAW video, or 10-bit 4:2:2.
- **HDR and color management are second-class**, and per-monitor-v2 DPI + mixed-refresh
  multi-monitor behaves worse.
- **~150 MB baseline RSS and a ~400 ms cold start** before you've drawn a pixel.

Electron is the right call for a *media library/organizer* with modest playback needs. It is the
wrong call for the thing you described.

## Why not straight C

C is viable — libmpv, FFmpeg, libjpeg-turbo, libheif, and Exiv2's C wrapper all expose C APIs.
But:

- D3D11/DXGI/WIC are COM. Usable from C (`lpVtbl->Method(this, ...)`) but every call site
  becomes noise and every early return leaks unless you hand-roll cleanup gotos.
- You will hand-write the RAII, the job system, the vector/hashmap, and the string handling that
  a media app leans on constantly.
- Edit-stack, format-registry, and undo/redo all want value semantics and destructors.

**Use C++20 with a deliberately narrow subset**: RAII + `std::unique_ptr`/`ComPtr`, `std::span`,
`std::string_view`, designated initializers, no exceptions in the hot path (use
`std::expected`-style result types), no RTTI, no `std::shared_ptr` in the render loop, no deep
template metaprogramming. Read it as "C with destructors and containers." Compile with
`/EHsc /GR- /permissive- /W4`.

## The credible alternative: Rust

If you'd rather write Rust: `windows-rs` (full D3D11/DXGI/WIC bindings), `wgpu` or raw D3D11,
`ffmpeg-next` or `libmpv-rs`, `image` + `rawler`/`libheif-rs`, `rexiv2`. Everything in this plan
maps over 1:1 — the architecture, threading model, and algorithms are language-agnostic. Rust
buys you memory safety exactly where you need it most (decoders parsing hostile files) at the
cost of a rougher COM/interop experience and thinner bindings for LibRaw/Exiv2.

**Pick one and do not mix.** If you're faster in C++, use C++.

## Why D3D11 and not D3D12/Vulkan

D3D11 gives you a free-threaded device (create textures from worker threads with no external
sync), flip-model swapchains, DirectComposition interop, and HDR — all the things that actually
determine smoothness here. D3D12 would buy you lower CPU driver overhead on draw-call-heavy
scenes; a media viewer draws roughly ten quads a frame. The bottleneck is decode and upload, not
submission. **Do not spend the D3D12 complexity budget.**

**D9 does not reopen this.** Vulkan / MoltenVK / wgpu as a "portable GPU" is the same complexity
budget spent to make Windows worse. macOS gets Metal as that OS's first present path, not as a
layer under D3D11.

---

# Contested decisions

These decisions are genuinely arguable. Each records the options, the call, and *why* — so the
reasoning survives even though the competing draft it was argued against is gone. **Reversibility
is the deciding column**: a decision that's cheap to revisit doesn't deserve much agonizing; one
that isn't, does.

Decision log with dates: [12-decision-log.md](12-decision-log.md).

## D1 — App shell: **native chrome + C++ core** ✅ decided

Windows: **C# WinUI 3**. macOS (Milestone F): **SwiftUI hosted in AppKit**. Same C++ core, same
ABI. ImGui is the present lab and the F3 overlay on both, never shipped chrome.

An earlier draft of this plan said Win32 + DComp + Dear ImGui. That was wrong for the shipped app,
and wrong for a specific reason: it was arguing against WinUI's *frame cost* and never priced the
chrome.

| | **Win32 + DComp + ImGui** | **C++/WinRT WinUI 3** | **C# WinUI 3 + C++ core** ✅ |
|---|---|---|---|
| Present latency | 1 frame | +1 composed frame | +1 composed frame |
| `ALLOW_TEARING` / VRR | Yes | Effectively no | Effectively no |
| Virtualizing 10k filmstrip | **You write it** | Free (`ItemsRepeater`) | Free |
| Accessibility, IME, high-contrast | **You write it, or ship without** | Free | Free |
| Explorer-grade panes, theming, settings | Hand-rolled | Free | Free |
| UI dev velocity | Slow | **Slow — IDL per view model, `co_await` in code-behind, weak designer tooling** | **Fast** |
| Languages / toolchains | 1 | 1 | 2, plus a flat C ABI boundary |
| Extra runtime | None | Windows App SDK | Windows App SDK + .NET |
| Feels like a Windows app | **No — ImGui reads as a tool** | Yes | Yes |

**Call: C# WinUI 3 for the shell, C++ static lib for the core, flat C ABI between them.**

Three things drive it. **ImGui chrome loses the users you're building for** — someone coming from
FastStone wants a virtualizing filmstrip, a real folder tree, keyboard/IME behaviour, and
accessibility, and "I'll write my own" is a year of work that isn't the product. **C++/WinRT XAML
is a well-known velocity tax** and you'd pay it on exactly the surfaces that are pure chrome:
filmstrip, folder tree, metadata pane, settings. **The core doesn't care** — it never sees XAML,
never sees C#, and stays language-agnostic behind a flat C ABI, which is also what makes the
**macOS host** (D9) — SwiftUI talking to the same header — possible without rewriting decode.
From PR 4 the core stays hostable; the Mac host itself is Milestone F, not a hope and not a
v1 slip ([15-platforms.md](15-platforms.md)).

Consequences, all accepted deliberately:

- **You give up VRR and `ALLOW_TEARING`.** For an app that idles at 0 % GPU on a still image,
  that is an acceptable trade. **144 Hz and tearing control are no longer v1 gates** — see the
  revised target in D6.
- **Design the C ABI once, in PR 1.** Opaque handles, POD structs, no C++ types across the line,
  no exceptions escaping. Retrofitting a marshalling boundary is miserable.
- **The canvas is a native HWND/DComp swapchain from day one — not `SwapChainPanel`.** See the
  amendment below. **Never silently degrade the canvas to XAML** — that failure mode is invisible
  in code review and obvious to users.
- **The ABI is specified before anything is built on it** ([14-abi.md](14-abi.md)).

Win32 + DComp + ImGui remains the right **present lab** — build it as PR 1 to prove the pacing
rules in [03-rendering.md](03-rendering.md), then keep it as a debug harness.

### D1 amendment — the shell is native, the chrome is hosted

The original D1 had PR 3 port the canvas onto `SwapChainPanel` and *test* whether it paces well
enough, with a native island as the fallback. That was a science experiment with a foregone
conclusion: composition swapchains cannot take `ALLOW_TEARING`, you inherit a composed frame either
way, and `SwapChainPanel` resize/DPI behaviour is a known tax. The fallback was always the
destination.

**So invert it. The Win32 top-level window and the D3D11 swapchain from PR 1 are the app**, kept
exactly as built. WinUI 3 chrome is hosted *inside* it as XAML content islands
(`DesktopWindowXamlSource`, Windows App SDK 1.4+), authored in C#. One present path, owned by C++,
never re-implemented.

What this costs, stated honestly:

- **The app's entry point is C++ Win32**, so the C# side is chrome content rather than the
  application host. You keep XAML velocity for panes, filmstrip, and settings — which is where the
  velocity argument actually applied — but not for app scaffolding.
- **Islands are a less-travelled path** than a stock WinUI app. Popups, flyouts, backdrop material,
  and focus/tab traversal across the island boundary all need deliberate attention.
- If islands prove unworkable, the fallback is now the *old* plan — a WinUI app with
  `SwapChainPanel` and an accepted composed frame. **PR 3 validates islands early**, for the same
  reason the original PR 3 existed: find out before chrome is built on top.

Everything else in D1 stands. The core is still UI-free C++ behind a flat C ABI; the shell is still
the thin part.

## D2 — Video: **FFmpeg + OS hwdecode on your own present path** ✅ decided

Windows: FFmpeg + **D3D11VA** on *your* `ID3D11Device`. macOS: FFmpeg + **VideoToolbox** on
*your* `MTLDevice`. One pipeline per OS, one canvas, no codec packs, no second player HWND /
`AVPlayer`.

An earlier draft said libmpv. The structural objection to it is correct: **child-HWND mpv is a
second canvas.** That's the flicker-on-resize, your-shaders-don't-apply problem, and the escape
route — mpv's render API through `WGL_NV_DX_interop2` — is genuinely fragile on hybrid GPUs, which
is most laptops. mpv is the better *movie player*; subtitle positioning, bitstream passthrough,
motion interpolation, ProRes, and DNxHD are movie-player features. **This app is not a player.**

That leaves two candidates:

| | **MF (`IMFMediaEngine`) + FFmpeg fallback** | **FFmpeg + D3D11VA, single path** ✅ |
|---|---|---|
| Pipelines to maintain | **2** | 1 |
| Native D3D11 texture output | Yes (`TransferVideoFrame`) | Yes (`ID3D11VideoDecoder` → NV12 texture) |
| A/V sync + audio output | **Free** — MF owns the clock | **You write it** (~2–4 weeks: WASAPI out, audio master clock, present against QPC, drop/dupe) |
| iPhone HEVC video | **Needs the Store HEVC extension**, or falls to the FFmpeg path anyway | Works out of the box |
| AV1, VP9, MKV | Store extension / unsupported → FFmpeg path | Works |
| Consistency with D3 | **Contradicts it** | Consistent |
| Lines of code to first playback | Few hundred | More |

**Call: FFmpeg + D3D11VA, one pipeline, one present path, behind `IVideoSource`.**

The decider is the interaction with **D3**. You are already bundling libheif so an iPhone HEIC
*photo* opens on a clean install. It would be incoherent to then demand a Store codec pack for the
HEVC *video* sitting next to it in the same folder — from the same phone, in the same camera dump,
which is the exact scenario this app exists for. And once HEVC and AV1 route through FFmpeg
regardless, MF is no longer the primary path; it's a second implementation earning its keep on
H.264 alone.

**The honest cost is the A/V clock.** Audio-as-master, present video frames against QPC, drop or
duplicate on drift — a known and tractable job for a viewer, but also the classic place to get
subtly wrong. An earlier draft of this plan said writing a player is "18 months"; that was about a
*full* player with subtitle layout, HDR passthrough, and exotic containers. For play/pause/seek/
scrub on a folder of clips, it is weeks.

**Escape hatch, deliberately cheap:** if the clock work overruns its budget in PR 5, drop in
`IMFMediaEngine` behind the same `IVideoSource` for H.264/H.265-with-pack and keep FFmpeg for
everything else. That is the review's proposal, available as a fallback rather than as the plan —
which is the right way round, because it costs nothing to hold in reserve and a second permanent
pipeline to adopt up front.

## D3 — Codecs: **bundle, never require a Store pack** ✅ decided

| | **Bundle** ✅ | **Store codec packs** |
|---|---|---|
| iPhone HEIC photo + HEVC video on a clean install | Yes | **No — a dialog instead of the content** |
| Installed size | +~40 MB | Smaller |
| Patent exposure, HEVC **decode** | Real but standard — what every media app carries | Microsoft's problem |
| Patent exposure, HEVC **encode** | **Never ship a software HEVC encoder** | N/A |
| Reversibility | Easy — probe order is one function | Easy |

"Detect the pack and show a clear install message" is the correct *fallback* and the wrong
*default*. A viewer that greets a folder of holiday photos with an install prompt has failed at its
only job, and the user does not come back to try again.

Probe the OS codec first — it may be hardware-backed — and fall back to the bundled decoder
silently. Keep the install-prompt path for HEVC **encode** only, and for any distribution channel
where a bundled decoder must be disabled for licensing reasons
([11-licensing.md](11-licensing.md)). On Windows the probe is MF / DXVA; on macOS it is
VideoToolbox / ImageIO. The bundled fallback is what makes both honest (D3, D9) and is what
keeps golden images matching across OS.

## D4 — v1 scope: **viewer with light edits** ✅ decided

| | **Full editor** | **Viewer + light edits** ✅ |
|---|---|---|
| Photo edits | Curves, HSL, local adjustments, healing, full RAW develop | Rotate/flip/crop/resize + exposure/contrast/saturation/temperature |
| Time to shippable | ~6 months full-time | **~3 months** |
| Risk of never shipping | **Real** | Low |
| Architecture impact | **None** — same edit stack either way | None |

Rotate / crop / exposure / trim is a viewer people will *switch to*. Curves, HSL, local
adjustments, healing, and GPU demosaic are a develop module on the same `EditStack`, later. Every
op deferred is a shader you don't write, a slider you don't lay out, and a golden-image test you
don't maintain — at zero architectural cost, because the stack was designed to take more ops.

**Do not start them.** The v1/v1.1 op split is in
[07-photo-editing.md](07-photo-editing.md#v1-scope-line).

## D5 — Format coverage: **the camera-dump set** ✅ decided

The job is JPEG, HEIC, RAW, and H.264/HEVC MP4. Everything past that delays the moment a user can
point this at a real camera dump.

**v1:** JPEG, PNG, BMP, GIF (incl. animated), TIFF, WebP (incl. animated), **HEIC/HEIF**, **AVIF**,
ICO, and RAW (CR2/CR3, NEF, ARW, RAF, ORF, RW2, PEF, DNG) preview-first. Video: MP4/M4V, MOV, MKV,
WebM, AVI, TS — H.264, HEVC, VP9, AV1, MPEG-2.

AVIF stays in v1 only because dav1d already ships for AV1 video, so it is nearly free. Everything
else waits.

**v1.1+:** JPEG XL, OpenEXR, Radiance HDR, PSD, SVG, DDS/KTX2, JPEG 2000, QOI, VVC, BRAW. These
are collector behaviour. Adding one is a file and a registry line
([04-image-pipeline.md](04-image-pipeline.md)) — that's the point of the decoder registry, and it
is exactly why they don't need to be in v1.

## D6 — Colour: **linear working space, SDR swapchain** ✅ decided

An earlier draft said "linear scRGB FP16 from day one, swapchain included." Half right. These are
**two separable decisions** and conflating them was the error:

| | Working space (edit chain, intermediates) | Swapchain format |
|---|---|---|
| **v1** | **Linear, FP16 — non-negotiable** | **8-bit sRGB (`R8G8B8A8_UNORM_SRGB`)** |
| **v1.1** | unchanged | FP16 scRGB when an HDR output is detected |
| Cost of getting it wrong | **A rewrite** — retrofitting colour management touches every op | One function; the format is already a runtime choice |

Nearly every display this app meets is SDR. An FP16 swapchain doubles present bandwidth for
nothing, and at 45 MP an FP16 intermediate is ~360 MB — real pressure on the upload budget in
[03-rendering.md](03-rendering.md). Keep the linear FP16 *working* space, blit to an 8-bit sRGB
swapchain, and make the format a runtime branch so HDR is a later switch rather than a later
rewrite.

**The v1 colour policy, in one line: untagged JPEG is assumed sRGB; treating a *tagged* image as
sRGB is a bug.** Read the embedded ICC profile (`APP2`/`iCCP`/`colr`) and transform through LCMS.
"We'll add colour management later" is how viewers end up quietly wrong on every wide-gamut phone
photo.

**Revised v1 performance gate**, replacing the old 144 Hz / VRR target which D1 makes unreachable:

> **Zero dropped frames while panning a cached image at the display's refresh rate** — measured,
> in CI, on the frame-time harness.

That is honest, achievable under WinUI, and still the thing that makes the app feel right.

## D7 — Video trim: **two-path first, smart cut in v1.1** ✅ decided

Smart cut (re-encode only the sub-second head and tail, stream-copy the middle) is a real
differentiator and a **join-artifact minefield** — encoder settings must match the source's
profile, level, GOP structure, and bitrate closely enough that the seam is invisible, and getting
it wrong ships a visible quality step at both ends of every clip.

**v1: two clearly-labelled paths.** Keyframe trim (stream copy, instant, snaps to the keyframe grid
drawn on the scrub bar) and full re-encode (frame-accurate, labelled as slower). **v1.1: smart cut**,
once the two-path version is boring and the golden-file corpus exists to catch seams. Algorithm is
preserved in [08-video-editing.md](08-video-editing.md).

## D8 — Decode sandboxing: **defer to post-v1** ✅ decided

An AppContainer decode process is the right security endgame — it converts an RCE in a decoder
into a crashed helper. It is also a pure win with no product-visible change, which is exactly why
it can never justify delaying v1. Fuzz the decoders from PR 6 in the meantime
([09-build-and-test.md](09-build-and-test.md)).

## D9 — Platforms: **Windows v1, hostable core, macOS as Milestone F** ✅ decided

v1 is Windows. From PR 4 the native core is kept hostable so a Mac app is a second host of
the same decode / colour / edit / metadata library, not a rewrite of those. The Mac app is
**Milestone F** ([10-roadmap.md](10-roadmap.md)), after Windows ships.

A draft of this section put both platforms in v1. That is the right way to *not forget* a
port and the wrong way to *schedule* one: every remaining Windows PR would wait on a Metal
present lab the Windows user does not need. Treating Mac as “just SwiftUI on the existing
C++” is the other failure mode — chrome is half the work; present, VideoToolbox, Core Audio,
I/O, and notarization are the other half. Recorded in [12](12-decision-log.md).

| | **Windows only, rewrite Mac later** | **Windows v1, hostable core, Mac as Milestone F** ✅ | **Windows and macOS in v1** |
|---|---|---|---|
| Windows v1 date | Unchanged | Unchanged — hostable-core tax per PR from 4 | **Slips** — two hosts, two present labs, two chromes, two ship pipelines |
| Present path | One, forever Windows-shaped | One per OS; Mac proven in F | One per OS, both green before v1 |
| Chrome | WinUI baked into the core | WinUI now; SwiftUI in F; same C ABI | WinUI and SwiftUI in parallel from PR 4 |
| Mac later | A rewrite of decode *and* present | A host + backends, specified now | The remaining Windows PRs become dual-track |
| Reversibility | Hard (wrong direction) | Easy if Mac never happens | Hard — a Metal lab in v1 is sunk cost |

**Call: v1 is a Windows app. macOS is the next product, specified now, built after PR 15.**
D1–D8 stand, with the per-OS reading below. PR 1–3 are not retrofitted. Do not implement
Metal, Swift, or a `*_mac.cpp` during PRs 1–15.

What this costs, stated honestly:

- **Chrome is written twice.** Filmstrip, folder tree, metadata pane, adjust pane, settings:
  WinUI on Windows, SwiftUI on Mac. Sharing them by rewriting in C++ throws D1 away. Sharing
  them via Qt/Flutter/MAUI throws D1 away a different way.
- **A Metal present lab in PR 16**, the Mac equivalent of PR 1, with its own 60 s animated
  and idle soaks. A Windows DXGI pass is not a Mac pass.
- **Hand-written HLSL and MSL twins** from the first kernel the Mac path needs. No SPIR-V, no
  shader compiler, no third language.
- **Two shipping pipelines:** Velopack + Authenticode, and notarized Sparkle. Neither Store
  (GPL).
- A **narrow** gfx/io/audio/hwdecode/encode port — a header plus a real `*_win.cpp` and
  `*_mac.cpp`. Not a general RHI, not empty stubs, not Vulkan. The Mac files arrive with
  Milestone F, as implementations, not as v1 placeholders.
- **Apple Silicon + macOS 14 only.** Intel Macs are a second GPU story for a dying install
  base. Windows floor stays 10 21H2 x64; Windows ARM64 waits.

D1, per OS: native chrome + C++ core. Windows remains C# WinUI 3 hosted in the native window.
macOS is SwiftUI hosted in an AppKit window that owns a `CAMetalLayer`. ImGui is the present
lab and the F3 overlay on both, never shipped chrome.

D2, per OS: FFmpeg + hardware decode on *your* device, presented on the same swapchain as
photos. Windows remains D3D11VA; macOS is VideoToolbox. `IMFMediaEngine` stays a Windows-only
escape hatch. **`AVPlayer` is forbidden** — it is the Mac version of child-HWND mpv.

Forbidden, unchanged: **do not introduce Electron, Tauri, Node, D3D12, Vulkan, or a second
present path on one OS.** Metal is macOS's first present path, not a second one on Windows.

Full rule set, PR-by-PR: [15-platforms.md](15-platforms.md).

---

## Third-party dependency policy

Bundle and statically link everything **except FFmpeg, libheif, libde265, LibRaw, and (if kept)
Exiv2** — these are LGPL/GPL and must be dynamically linked. See
[11-licensing.md](11-licensing.md); the Exiv2 licence question decides the whole app's licence and
must be settled in PR 1.

Never *require* a Store codec extension. Probe the OS codec, prefer it when present and
hardware-backed, then fall back to the bundled decoder silently.

Pin the vcpkg baseline. Reproducible builds matter more than fresh dependencies, and a decoder
that silently changes version between your machine and CI will cost you a week of golden-image
confusion.
