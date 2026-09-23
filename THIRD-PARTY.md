# Third-party components

MediaViewer is licensed **GPL-2.0-or-later** ([LICENSE](LICENSE)). That decision was
made in PR 1 and is recorded, with its reasoning, in
[plan/12-decision-log.md](plan/12-decision-log.md) and
[plan/11-licensing.md](plan/11-licensing.md).

The short version: the Microsoft Store is not a distribution requirement, so Exiv2 is
used under the GPL rather than bought commercially, and FFmpeg, libheif, libde265 and
LibRaw all become trivially compliant at the same time.

This file is generated as part of the build and shown in the About dialog. It lists
**everything currently linked**, not everything the plan intends to link — entries are
added by the PR that adds the dependency.

## Currently linked (PR 8 — the v1 release set)

| Component | Version | Licence | Linkage | Notes |
|---|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.8 | MIT | Static | Present lab and the F3 debug overlay only. Never shipped chrome (D1). |
| [Catch2](https://github.com/catchorg/Catch2) | 3.16.0 | BSL-1.0 | Static, tests only | Not in a shipped binary. |
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo) | 3.2.0 | IJG / BSD-3 | Dynamic (vcpkg x64-windows) | JPEG decode. |
| [libspng](https://github.com/randy408/libspng) | 0.7.4 | BSD-2 | Dynamic (vcpkg x64-windows) | PNG decode. APNG frames are walked by MediaViewer and decoded by libspng. |
| [giflib](https://sourceforge.net/projects/giflib/) | 6.1.3 | MIT | Dynamic (vcpkg x64-windows) | GIF decode, still and animated (PR 6). |
| [libwebp](https://chromium.googlesource.com/webm/libwebp) | 1.6.0 | BSD-3 | Dynamic (vcpkg x64-windows) | WebP decode, still and animated, via libwebpdemux (PR 6, brought forward from PR 7). libwebpmux is linked into the test binary only. |
| [Little CMS (lcms2)](https://github.com/mm2/Little-CMS) | 2.19.1 | MIT | Dynamic (vcpkg x64-windows) | ICC → sRGB for the v1 8-bit display path (precomputed LUT; sRGB-in-effect profiles copy through). No tone-map on display-referred sources (D6). |
| libsharpyuv | 1.6.0 | BSD-3 | Dynamic (vcpkg x64-windows) | Transitive, via libwebp. |
| zlib | 1.3.2 | Zlib | Dynamic | Transitive, via libspng. |
| [SQLite](https://sqlite.org) | 3.53.4 | Public domain | Dynamic (vcpkg x64-windows) | Thumbnail cache index (PR 4). Public domain, so linkage is not a licence question. |
| **[FFmpeg](https://ffmpeg.org)** (avcodec, avformat, avfilter, avutil, swresample, swscale) | 9.0.1 | **LGPL-2.1+** | **Dynamic (DLL)** | Video demux and decode on D3D11VA (PR 5). Configured without `--enable-gpl` / `--enable-nonfree`; `tools/licence-check.ps1` reads the configure string from the built DLLs. |
| [dav1d](https://code.videolan.org/videolan/dav1d) | 1.5.4 | BSD-2 | Dynamic (vcpkg x64-windows) | Transitive, via FFmpeg: AV1 software fallback. |
| [Crashpad](https://chromium.googlesource.com/crashpad/crashpad) (client, util, mini_chromium base) | vcpkg 2026-07-02 | **Apache-2.0** (mini_chromium: BSD-3) | Client **static** in `mediaviewer_lab`; `crashpad_handler.exe` shipped as a **separate program** beside it | Out-of-process crash capture (PR 7, plan/13 Part 2). No upload URL; dumps are scrubbed locally before any send could be offered. **Licence note:** Apache-2.0 is compatible with GPL-3.0 but not GPL-2.0-only. MediaViewer is GPL-2.0-or-later, so a distributed binary that statically links the client is conveyed under GPL-3.0 terms — the same position libheif/libde265 (LGPL-3) already put us in. Recorded in plan/12 (2026-09-14). |
| .NET 8 / runtime libraries | 8.0 | MIT | Framework-dependent | Hosts the WinUI chrome island via hostfxr. Interop assembly too. Self-contained ships in PR 8. |
| [Windows App SDK / WinUI 3](https://github.com/microsoft/WindowsAppSDK) | 2.4.0 | MIT | Framework package | Command bar chrome in a `DesktopWindowXamlSource` island. Not the canvas. Runtime must be installed on the machine (unpackaged). |
| Windows SDK (D3D11, DXGI, DirectComposition, MMCSS, TraceLogging, D3DCompile) | 10.0.26100 | Microsoft SDK licence | OS import libraries | — |
| [Cozette](https://github.com/the-moonwitch/Cozette) | 1.30.0 | MIT | Bundled TTF | Empty canvas and chrome labels. Bitmap terminal face (Proggy/Dina lineage); `CozetteVector.ttf` for WinUI. Licence: `assets/fonts/LICENSE-Cozette.txt`. |
| [libtiff](https://libtiff.gitlab.io/libtiff/) | 4.7.2 | libtiff (BSD-like) | Dynamic (vcpkg x64-windows) | TIFF and ICO decode (PR 7). |
| **[libheif](https://github.com/strukturag/libheif)** | 1.23.2 | **LGPL-3** | **Dynamic (DLL)** | HEIC/HEIF decode (PR 7). vcpkg `default-features` OFF: the port's `hevc` feature is x265 **encode**, which plan/11 forbids. |
| **[libde265](https://github.com/strukturag/libde265)** | 1.1.1 | **LGPL-3** | **Dynamic (DLL)** | HEVC **decode** for libheif. A hard dependency of the port, not a feature, so HEIC decode does not pull x265. |
| [libavif](https://github.com/AOMediaCodec/libavif) | 1.4.2 | BSD-2 | Dynamic (vcpkg x64-windows) | AVIF still and animated (PR 7), on dav1d. |
| **[LibRaw](https://www.libraw.org/)** | 0.22.2 | **LGPL-2.1** | **Dynamic (DLL)** | Camera RAW (PR 7): embedded preview first, then the full decode. No GPL demosaic pack. |
| [libyuv](https://chromium.googlesource.com/libyuv/libyuv/) | 1916 | BSD-3 | Dynamic (vcpkg x64-windows) | Transitive, via libheif/libavif. |
| [liblzma (xz)](https://tukaani.org/xz/) | 5.8.3 | 0BSD | Dynamic (vcpkg x64-windows) | Transitive, via libtiff. |
| [Velopack](https://github.com/velopack/velopack) | 1.2.0 | MIT | Managed assembly + `Update.exe` beside the app | PR 8 updater: versioned folders, delta packages, staging, rollback (plan/13 Part 1). |
| [BouncyCastle.Cryptography](https://github.com/bcgit/bc-csharp) | 2.7.0 | MIT (Bouncy Castle) | Managed assembly | Ed25519 verification of the signed update manifest (PR 8). The signature check runs before anything from the channel is trusted. |

## Planned, with the PR that introduces each

Nothing below is linked yet. The table exists so the licence position of a dependency
is settled before it arrives, rather than discovered afterwards.

| Component | Licence | Required linkage | Arrives in |
|---|---|---|---|
| DirectXTex | MIT | Static | Deferred from PR 4 until a thumbnail must be GPU-resident (plan/12 2026-09-07) |
| **Exiv2** | **GPL-2.0** | Dynamic (DLL) | PR 12 (metadata write) |

### Rules the build enforces

`tools/licence-check.ps1` runs in CI and fails the build on any of these:

1. **FFmpeg configured with `--enable-gpl` or `--enable-nonfree`.** LGPL only. If a filter
   requires GPL FFmpeg, find another way.
2. **A software HEVC or AAC encoder anywhere in the dependency graph** — x264, x265, fdk-aac.
   Decode is what users need; encode is delegated to the GPU or to Media Foundation
   ([plan/11](plan/11-licensing.md)).
3. **A LibRaw GPL demosaic pack** (`LibRaw-demosaic-pack-GPL2`, `-GPL3`).
4. **FFmpeg, libheif, libde265, LibRaw or Exiv2 linked statically.** LGPL requires that a
   user be able to replace the library with their own build; dynamic linking satisfies
   that trivially, static linking obliges us to ship relinkable object files.

## Source offer

For the LGPL components we ship as DLLs, and for the GPL components, the corresponding
source is offered per release. The About dialog links to it. The exact FFmpeg version and
configure line are published alongside each release — this is a release obligation, not a
formality, and PR 8 is where the release pipeline that produces it lands.

### How it tracks the running build (PR 8)

plan/13 is explicit that the offer must track the **running build**, not a frozen v1.0
snapshot: "have the About dialog link to the offer for *the running build*".

- About reads the version from the host exe's `VERSIONINFO`, which CMake fills from
  `project(VERSION)` — there is no second version number anywhere — and links to
  `releases/tag/v{version}`. A build three versions old links to its own tag, not to
  whatever happens to be newest.
- `tools/package/build-release.ps1` cuts every release, so every release has a tag, and
  the per-release source archive belongs on that tag.

**Still a human step.** Attaching the corresponding source per release — the exact FFmpeg,
libheif, libde265 and LibRaw sources with the configure lines from the pinned vcpkg
baseline in `vcpkg.json` — is not automated yet. Until it is, a published release whose tag
carries no source archive is out of compliance, and About links to a tag that does not
answer the offer. This is exactly the obligation plan/13 warns "quietly rots"; it belongs
in the same pipeline step that signs.
