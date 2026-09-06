# 11 — Licensing & Patents

**Settle this in PR 1, not at the end.** Licensing is the one category of mistake that can force a
rewrite of a shipped binary, and it is invisible until someone complains.

## FFmpeg — the one that actually constrains your build

FFmpeg is **LGPL 2.1+** in its default configuration and **GPL** if built with
`--enable-gpl` (which pulls in x264, x265, and a set of filters).

Rules:

- Build FFmpeg **LGPL only**. Do not pass `--enable-gpl` or `--enable-nonfree`.
- **Link FFmpeg dynamically** — ship `avcodec-*.dll`, `avformat-*.dll`, `avutil-*.dll`,
  `swscale-*.dll`, `swresample-*.dll` alongside your exe. LGPL requires that a user be able to
  replace the library with their own version; dynamic linking satisfies this trivially, static
  linking obliges you to ship relinkable object files.
- Publish the exact FFmpeg source and configure line you built from, and link to it from the About
  dialog.
- This **contradicts the "static-link everything" instinct** in
  [01-decisions.md](01-decisions.md#third-party-dependency-policy). FFmpeg is the exception.

**FFmpeg is now the video pipeline itself** (D2), not just a fallback — so this is not a corner of
the build, it is the build. If you ever find yourself wanting an `--enable-gpl` filter, find
another way. (libmpv, had it been chosen, would carry the same LGPL-not-GPL requirement.)

**Consequence for encoding:** since x264/x265 are off the table under LGPL, the re-encode paths in
[08-video-editing.md](08-video-editing.md) should use **hardware encoders** (NVENC via
`h264_nvenc`/`hevc_nvenc`, Quick Sync via `h264_qsv`, AMF) or the **Media Foundation H.264
encoder**, not a bundled software x264. This is fine — smart cut only ever re-encodes about a
second of video, so hardware encoding is both legally simpler and faster.

## Codec patents

| Codec | Decode | Encode |
|---|---|---|
| H.264 / AVC | Patents largely expired or covered downstream; universally shipped | Use the OS/GPU encoder, not your own |
| **HEVC / H.265** | Patent pools (Access Advance, Via LA) are **actively enforced**. Shipping your own decoder is what every media app does, but it is real exposure | **Do not ship a software HEVC encoder.** Use the OS/GPU encoder or offer H.264 output |
| AV1 / VP9 | Royalty-free by design (AOMedia, Google) | Free to ship |
| JPEG, PNG, WebP, JXL | Free | Free |
| AAC | Patent pool exists; the OS decoder covers you | Prefer the MF encoder |

Practical position, and the one most independent viewers take: **bundle decoders, never bundle a
software HEVC or AAC encoder.** Decode is what your users need; encode can always be delegated to
Windows or the GPU.

If you ever sell the app or ship at meaningful volume, get an actual opinion on HEVC. Until then,
this is the same posture as VLC, IrfanView, and every RAW tool on Windows.

## Library licenses to comply with

All permissive; all need attribution in an About dialog and in a bundled `THIRD-PARTY.md`.

| Library | License | Note |
|---|---|---|
| libjpeg-turbo | IJG / BSD-3 | — |
| libspng / libpng | zlib-ish / libpng | — |
| libwebp, libavif, dav1d, libaom | BSD-2/3 | — |
| libjxl | BSD-3 | — |
| libheif | **LGPL-3** | Dynamic-link it, same reasoning as FFmpeg |
| libde265 | **LGPL-3** | Dynamic-link it |
| libtiff | libtiff (BSD-like) | — |
| **LibRaw** | **LGPL-2.1 or CDDL** (dual), plus a commercial option | Choose LGPL and **dynamic-link**; note the `LibRaw-demosaic-pack-GPL2/3` packs are GPL — **do not use them** |
| OpenEXR / Imath | BSD-3 | — |
| **Exiv2** | **GPL-2.0** by default | **Trap.** A commercial licence is available. Either buy it, dynamic-link and accept that the combined work is GPL, or substitute a permissive alternative (`libexif` + `TinyXML2`-backed XMP, or Adobe's XMP Toolkit under BSD). **Decide this in PR 1.** |
| Little CMS (lcms2) | MIT | — |
| SQLite | Public domain | — |
| Dear ImGui | MIT | — |
| resvg | MPL-2.0 | File-level copyleft; dynamic-link or keep unmodified |
| DirectXTex | MIT | — |
| .NET / WinUI 3 / Windows App SDK | MIT | Shell only (D1); no constraint on the core |

### The Exiv2 problem, specifically

Exiv2 is GPL-2.0, and it is the single best metadata library available. Options, in order of
preference:

1. **Buy the commercial licence** if you intend to ship a closed-source app. It is inexpensive and
   removes the question entirely.
2. **Make MediaViewer itself GPL-2.0.** Perfectly reasonable for a tool like this, and it also
   resolves any FFmpeg question. Costs you nothing unless you plan to sell a closed binary.
3. **Replace it** with libexif + the Adobe XMP Toolkit (BSD) + your own maker-note handling. This
   is real work and you lose maker-note coverage for several camera brands.

**Recommendation: pick option 2 unless you have a specific plan to sell a closed-source build.**
GPL-2.0 for the app makes FFmpeg, libheif, LibRaw, and Exiv2 all trivially compliant, and it
is what comparable tools do. Note that GPL is incompatible with **Microsoft Store distribution
terms in some readings** — if the Store is a hard requirement, take option 1 instead.

## Your own licence — decide it as a product question, not a library question

Framing this as "what do we do about Exiv2" produces a stalled checklist item with no owner. The
actual fork is a **product** question with one input:

> **Do you need the Microsoft Store?**

| | **Store matters** | **Store does not matter** ✅ likely |
|---|---|---|
| Exiv2 | **Buy the commercial licence** | Use it under GPL |
| App licence | Closed, or a permissive licence; everything else LGPL and dynamically linked | **GPL-2.0-or-later** |
| Cost | A licence fee, and stricter discipline about GPL creeping in anywhere | None |
| Distribution | Store + direct download | Direct download only |
| Consequence | Must audit every future dependency for GPL | Any future GPL dependency is simply fine |

Some readings of Store distribution terms conflict with GPL-2.0. If the Store is a real
requirement, **buy the Exiv2 licence and stay GPL-free throughout** — that is the whole reason to
spend the money, and it must be decided before the dependency set calcifies.

If the Store is not a requirement — and for a direct-download Windows utility with an auto-updater
([13-updates-and-telemetry.md](13-updates-and-telemetry.md)) it usually isn't — **GPL-2.0-or-later
is the path of least resistance.** It makes FFmpeg, libheif, libde265, LibRaw, and Exiv2 all
trivially compliant at once, and matches what comparable tools do.

**MIT is only achievable** if you drop or replace Exiv2 and stay strictly LGPL-dynamic elsewhere.

**This decision has an owner and a deadline: you, before PR 1 merges.** It is not a checklist tick
— it determines the dependency policy every later PR is reviewed against.

## Checklist for PR 1

- [ ] Decide the app's own licence (drives everything else)
- [ ] Decide the Exiv2 route: buy / go GPL / replace
- [ ] FFmpeg configure line pinned, LGPL-only, no `--enable-gpl`, no `--enable-nonfree`
- [ ] FFmpeg, libheif, libde265, LibRaw all built as DLLs
- [ ] `THIRD-PARTY.md` generated and an About dialog that shows it
- [ ] Source-offer page for the LGPL components, referenced from About
- [ ] No software HEVC or AAC encoder anywhere in the dependency graph
- [ ] CI check that fails the build if a GPL-configured FFmpeg or a LibRaw GPL demosaic pack
      appears in the link line
