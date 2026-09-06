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

## Currently linked (PR 1)

| Component | Version | Licence | Linkage | Notes |
|---|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.92.8 | MIT | Static | Present lab and the F3 debug overlay only. Never shipped chrome (D1). |
| [Catch2](https://github.com/catchorg/Catch2) | 3.16.0 | BSL-1.0 | Static, tests only | Not in a shipped binary. |
| .NET 8 / runtime libraries | 8.0 | MIT | Framework | Interop assembly only. |
| Windows SDK (D3D11, DXGI, DirectComposition, MMCSS, TraceLogging) | 10.0.26100 | Microsoft SDK licence | OS import libraries | — |

## Planned, with the PR that introduces each

Nothing below is linked yet. The table exists so the licence position of a dependency
is settled before it arrives, rather than discovered afterwards.

| Component | Licence | Required linkage | Arrives in |
|---|---|---|---|
| libjpeg-turbo | IJG / BSD-3 | Static | PR 2 |
| libspng | BSD-2 | Static | PR 2 |
| Little CMS (lcms2) | MIT | Static | PR 2 |
| SQLite | Public domain | Static | PR 4 |
| DirectXTex | MIT | Static | PR 4 |
| **FFmpeg** | **LGPL-2.1+** | **Dynamic (DLL)** | PR 5a |
| libtiff | libtiff (BSD-like) | Static | PR 7 |
| libwebp | BSD-3 | Static | PR 7 |
| **libheif** | **LGPL-3** | **Dynamic (DLL)** | PR 7 |
| **libde265** | **LGPL-3** | **Dynamic (DLL)** | PR 7 |
| libavif / dav1d | BSD-2 | Static | PR 7 |
| **LibRaw** | **LGPL-2.1** | **Dynamic (DLL)** | PR 7 |
| **Exiv2** | **GPL-2.0** | Dynamic (DLL) | PR 8 |

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
formality, and PR 15 is where the release pipeline that produces it lands.
