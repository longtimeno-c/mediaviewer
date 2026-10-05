# Licensing & Patents

MediaViewer's own licence, how each third-party library is licensed and linked, the codec-patent
position, and the build checks that enforce it. The per-library list with versions is
[THIRD-PARTY.md](../../THIRD-PARTY.md).

## The app's licence

MediaViewer is **GPL-3.0-or-later**. `LICENSE` is the GPL-3.0 text, `NOTICE` carries the
copyright line, and every first-party source file carries
`// Copyright (C) 2026 longtimeno-c` and an SPDX identifier. `vcpkg.json` declares the same
licence.

Because the app is GPL, Exiv2 (GPL-2.0-or-later upstream) is used under the GPL, and FFmpeg,
libheif, libde265 and LibRaw are compliant without a commercial licence. GPL-3.0 (rather than
2.0) lets the in-app About serve as the GPLv3 "Appropriate Legal Notices" and makes the
statically linked Apache-2.0 Crashpad client compatible outright.

Distribution is direct download only; there is no Microsoft Store channel.

**Attribution.** The project requires only what the GPL allows: keep copyright notices, ship
`LICENSE` and `NOTICE`, mark modified files, show the Appropriate Legal Notices in the UI. No
further terms (GPL §10); the name and icon are a trademark request in the README, not a licence
clause.

## FFmpeg

FFmpeg is the video pipeline ([video](05-video-pipeline.md)), built **LGPL-2.1+ only**: no
`--enable-gpl`, no `--enable-nonfree`. The pinned vcpkg port configures `--enable-d3d11va
--enable-dxva2 --enable-shared` with `--disable-libx264 --disable-libx265 --disable-libfdk-aac`.

- **Dynamically linked**: `avcodec`, `avformat`, `avfilter`, `avutil`, `swresample`, `swscale`
  ship as DLLs (dylibs in `MediaViewer.app/Contents/Frameworks`), so a user can substitute their
  own build as the LGPL requires.
- Enabled features: `avcodec avformat avfilter swresample swscale dav1d nvcodec qsv amf webp`.
  `nvcodec` / `amf` are header-only (the GPU driver is loaded at run time); `qsv` links oneVPL.
- **Encoding uses hardware encoders only**: NVENC (`h264_nvenc` / `hevc_nvenc`), Quick Sync
  (`h264_qsv`), AMF, the Media Foundation H.264 encoder, and VideoToolbox on macOS
  ([video editing](08-video-editing.md)). libwebp provides clip → WebP. The GPL licence of the
  app does not relax this: the objection to x264/x265 is patent exposure, not copyleft.

## Codec patents

| Codec | Decode | Encode |
|---|---|---|
| H.264 / AVC | Bundled (FFmpeg) | OS / GPU encoder only |
| **HEVC / H.265** | Bundled (FFmpeg, libde265); the OS / GPU decoder is preferred when hardware-backed | **No software encoder.** OS / GPU encoder only |
| AV1 / VP9 | Royalty-free; dav1d is the AV1 software fallback | — |
| JPEG, PNG, WebP | Free | libwebp (clip → WebP) |
| AAC | FFmpeg decoder | No software encoder (no fdk-aac) |

Position: bundle decoders, never bundle a software HEVC or AAC encoder.

## Library licences and linkage

| Library | Licence | Linkage |
|---|---|---|
| libjpeg-turbo | IJG / BSD-3 | DLL (Windows), static (macOS) |
| libspng, zlib | BSD-2, Zlib | DLL / static |
| giflib | MIT | DLL / static |
| libwebp, libsharpyuv | BSD-3 | DLL / static |
| libavif, dav1d, libyuv | BSD-2 / BSD-3 | DLL / static |
| libtiff, liblzma | libtiff (BSD-like), 0BSD | DLL / static |
| **libheif** | **LGPL-3** | **Always dynamic.** vcpkg default features off: the port's `hevc` feature is the x265 encoder |
| **libde265** | **LGPL-3** | **Always dynamic.** HEVC decode for libheif; a hard dependency, not a feature, so HEIC decode does not pull x265 |
| **LibRaw** | **LGPL-2.1** (of the LGPL / CDDL dual licence) | **Always dynamic.** No `LibRaw-demosaic-pack-GPL2/3` |
| **Exiv2** (+ Brotli, Expat) | **GPL-2.0-or-later** (MIT for the transitive pair) | **Always dynamic** |
| Little CMS (lcms2) | MIT | DLL / static |
| SQLite | Public domain | DLL / static |
| Crashpad (+ mini_chromium) | Apache-2.0 (BSD-3) | Client static; `crashpad_handler` a separate program |
| BLAKE3 | CC0-1.0 (dual CC0 / Apache-2.0, taken under CC0) | DLL / static |
| libsodium | ISC | DLL / static |
| Dear ImGui | MIT | Static; present lab and F3 overlay only |
| Catch2 | BSL-1.0 | Tests only |
| Velopack, BouncyCastle.Cryptography | MIT | Managed assemblies (Windows updater) |
| Sparkle | MIT | Framework in `Contents/Frameworks` (macOS updater) |
| .NET 8, Windows App SDK / WinUI 3 | MIT | Shipped self-contained with the Windows chrome |
| Microsoft OpenMP (`vcomp140.dll`), LLVM OpenMP | MS redistributable; Apache-2.0 WITH LLVM-exception | Dynamic, beside LibRaw |
| Cozette font | MIT | Bundled TTF |
| Final Cut Pro's `ProExtension.framework` | Apple, proprietary | **Not shipped.** The Mac FCP extension ([NLE search](23-nle-search.md)) `dlopen`s the copy inside the user's installed Final Cut Pro at run time, because FCP's extension point requires its classes. The copyright holder accepts loading it into the GPL extension |

Everything in the table is attributed in [THIRD-PARTY.md](../../THIRD-PARTY.md), which is
shown in the About dialog and copied into `MediaViewer.app/Contents/Resources` with `LICENSE`.
On macOS `tools/mac/macpack.py` fails the bundle if any dylib reference points outside it.

## Source offer

About reads the version from the host exe's `VERSIONINFO` (filled from CMake's
`project(VERSION)`) and links to `releases/tag/v{version}`, so the offer tracks the running
build. Release notes carry the source commit. Attaching the corresponding source archives for
FFmpeg, libheif, libde265 and LibRaw with their configure lines to each tag is a manual release
step, not yet automated ([RELEASING.md](../../RELEASING.md)).

## Enforcement

`tools/licence-check.ps1` runs in CI on every push (and in `build-release.ps1`) and fails on:

1. A forbidden port in either manifest (`vcpkg.json`, `tools/mac/dependencies/vcpkg.json`):
   `x264`, `x265`, `fdk-aac`, `libbluray`, a `libraw-demosaic-pack*`, or an FFmpeg `gpl` /
   `nonfree` feature.
2. FFmpeg's configure string, read from the built avutil/avcodec binaries, containing
   `--enable-gpl` or `--enable-nonfree`, or enabling a forbidden encoder.
3. FFmpeg, libheif, libde265, LibRaw or Exiv2 present as a static library, or a forbidden
   encoder DLL installed.
4. `LICENSE` missing or not GPL-3.0, `NOTICE` missing, `vcpkg.json` not declaring
   GPL-3.0-or-later, or `THIRD-PARTY.md` missing.
