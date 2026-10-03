# 04 — Image Pipeline

How stills are identified, decoded, colour-managed, shown progressively, tiled, prefetched,
paired, listed, thumbnailed and animated.

## Format coverage and which library owns each

Formats are identified by **file signature (magic bytes)**, never by extension
(`codec::probe`, [`src/codec/format.h`](../../src/codec/format.h)). The directory listing
filters by extension only to decide what is a candidate; decode always probes.

| Family | Formats | Library |
|---|---|---|
| JPEG | baseline, progressive, 12-bit, arithmetic | **libjpeg-turbo** (also DCT-domain scaling) |
| PNG / APNG | 8/16-bit, all colour types, animation | **libspng** (APNG frames: `codec/apng.cpp`) |
| BMP | | own reader (`codec/bmp.cpp`, `codec/dib.cpp`) |
| GIF | incl. animation | **giflib** |
| WebP | still, animated, lossless | **libwebp** (+ demux) |
| TIFF | tiled, striped, multi-page (pages with `Ctrl+PageUp/PageDown`) | **libtiff** |
| ICO | largest entry, ranked by the payload's own dimensions | own reader (`codec/ico.cpp`) |
| HEIF / HEIC | HEVC-coded stills, grids, sequences | **libheif** + **libde265** (Windows OS codec first for eligible stills, below) |
| AVIF | still + animated | **libavif** + **dav1d** |
| RAW | CR2/CR3, NEF, ARW, ORF, RAF, RW2, DNG, … | **LibRaw** (OpenMP) |

That is the shipped set: JPEG, PNG, BMP, GIF, TIFF, WebP, HEIC/HEIF, AVIF, ICO and RAW.
TIFF-container RAWs probe as `tiff` and are reclassified by `looks_like_raw`.

The registry is `codec::decode` in [`src/codec/decode.cpp`](../../src/codec/decode.cpp): one
`switch` on `format_family`, one TU per family. Adding a format is one file plus one registry
line. Animated sources have a parallel `open_animation` dispatch (GIF, WebP, APNG, HEIC
sequences, animated AVIF).

HEIC decode uses libde265 (a hard dependency of libheif); libheif is built with default
features off so no x265 encoder is linked.

### OS codec (HEIC only)

`codec::try_os_decode` runs before the bundled decoder; the policy is in
[`src/codec/os_decode.h`](../../src/codec/os_decode.h):

- Only HEIC stills are offered to the OS codec. Every other family always uses the bundled
  decoder.
- On Windows a HEIC goes to WIC only when WIC can match the bundled result: one HEVC-coded
  still (`hvc1` or `grid`), 8-bit, no alpha, not PQ/HLG, colour either an embedded ICC or
  sRGB-in-effect nclx. It also requires WIC's HEIF decoder **and** a registered HEVC decoder
  MFT. Anything else (Display P3 nclx, 10-bit, sequences) is bundled.
- Any OS failure other than cancellation falls through silently to libheif, which then judges
  the file.
- On macOS `try_os_decode` always declines (ImageIO is not wired), so HEIC is always libheif.
- `MV_OS_CODEC=0` forces the bundled path (tests use it to stand in for a clean machine).

## Progressive display — the perceived-speed trick

First pixel is never the full decode. For the item on the canvas
(`image::decode_first_pixel`, [`src/image/pipeline.h`](../../src/image/pipeline.h)):

1. **Navigation LRU hit** — the decoded texture is republished immediately (see Prefetch).
2. **Embedded preview.** A RAW's embedded JPEG (`decode_raw_preview`); a HEIC's largest
   thumbnail item (`decode_heic_thumbnail`, only when smaller than and the same shape as the
   primary).
3. **DCT-scaled JPEG.** libjpeg-turbo at 1/4 scale (`decode_jpeg(..., scale_denom = 4)`).
   Formats without a cheap preview, or previews under 16 px on a side, skip to the full decode.
4. **Full decode.** For a still of 2048×2048 or more without an embedded preview, the top level
   is uploaded and published first (`mip_limit = 1`), then the mip chain.
5. **Cross-fade** preview → full over 80 ms (`canvas::crossfade`,
   [`src/canvas/refinement.h`](../../src/canvas/refinement.h)), smoothstepped and driven by
   elapsed time. When the mean luma of the two differs (a RAW's embedded JPEG versus LibRaw's
   render measured 7–41 levels apart), the fade stretches linearly from 80 ms at ≤6 levels to
   250 ms at ≥40.

A clip opened on a search moment shows its cached JPEG-512 (the moment's row, else the poster)
as a placeholder while the player seeks.

Prefetched neighbours never take the preview step; they decode straight to full resolution.
RAW decodes on the selected image use `raw_foreground_threads()` (leaving processors for the
present loop, capped by `raw_thread_ceiling()`); prefetch uses one thread.

**Orientation is applied on the display path.** EXIF/container orientation is honoured; untagged
is identity. RAW and JPEG are decoded already rotated; TIFF is not
(`metadata::display_orientation` records which was applied). The original's pixels are never
rotated on disk by viewing; the lossless rotate command *writes* orientation (see
[06-metadata.md](06-metadata.md)).

## Tiled pyramid for large images

[`src/image/tiles.h`](../../src/image/tiles.h). Above 64 MP — or wider/taller than the 16384
texture limit — a still is not one texture:

- **CPU pyramid:** floor-half Mitchell levels built once on the decode worker; level 0 is the
  decoded image itself.
- **Overview:** the first level whose long edge is ≤ 2048, with its full mip chain, is
  uploaded with the publish and stays resident, so a fast zoom-out is blurry, never blank.
- **Tiles:** every finer level is split into **256×256** tiles, each a 260×260 texture
  (2-texel clamped border for bilinear and Catmull-Rom across seams) with one extra mip.
- The render thread marks the tiles visible at the current LOD plus a **one-tile ring**
  through lock-free per-tile states; the session's `tile_service` thread builds and creates
  them — 4 per 16 ms tick, at most 24 in flight — and the render thread draws ready tiles coarse
  to fine over the overview. It never waits on the service and never creates a texture.
- Resident tile VRAM is capped at **256 MB** (~750 tiles), LRU-evicted, separate from the
  viewer LRU.
- A tiled image is not kept in the navigation LRU; a revisit decodes again. Its CPU pyramid
  (≈ one third over the decoded RGBA; 533 MB for 100 MP) is held only while it is current.

Native entry points: `tiles_frame` / `tiles_stats` in [`src/abi/native.h`](../../src/abi/native.h).

## Prefetch

On `mv_folder_select` the core bumps the view generation, publishes an LRU hit or starts the
open, then prefetches the neighbours **at full resolution** in the order `+1, −1, +2, −2`
(`submit_prefetch`, [`src/abi/abi.cpp`](../../src/abi/abi.cpp)), skipping any already in the
LRU. Thumbnails are generated for every item separately (see Thumbnails). Navigation bumps the
generation, so queued work for an old view is abandoned at its next check.

The navigation LRU is sized in bytes: **512 MB** budget, at least 3 and at most 12 entries,
RGBA8 plus a third for mips. `mv_folder_forget` drops one path (used after a lossless rotate
rewrites a file).

## Live Photos and motion photos

A camera dump is full of paired items. Pairing is done at scan time
([`src/io/pairing.h`](../../src/io/pairing.h)) and collapses them into **one navigation
stop**:

| Source | On disk | Detection |
|---|---|---|
| **iPhone Live Photo** | `IMG_1234.HEIC` + `IMG_1234.MOV` | Same stem (ASCII case-insensitive) |
| **iPhone "Most Compatible"** | `IMG_1234.JPG` + `IMG_1234.MOV` | Same stem |

- The still is the primary: it fills the canvas, is the thumbnail, is what is edited and
  exported. The filmstrip shows one entry. `MP4` never pairs.
- **`;`** plays the motion once and returns to the still (`play_motion`); the host opens the
  pair path with `mv_video_open`.
- Ambiguous groups (three files, mixed stems) stay separate: a file is never hidden.
- Two non-ASCII stems that differ only in case stay two stops (the safe failure).
- A pair sits where the first of its two files sat; pairing never reorders the listing.
  Cost is O(n log n) and reruns on every watcher relist.
- Copy, move, drag-out and Recycle Bin act on both halves (host side).

## RAW + JPEG — the other pairing

| On disk | Detection |
|---|---|
| `DSC_0123.NEF` + `DSC_0123.JPG` (any RAW extension + JPEG/HEIC, same stem) | Stem match, ignoring case and the known RAW/JPEG extension sets |

One stop. The JPEG (or HEIC) is first pixel and the thumbnail; the RAW is the secondary,
reachable as `mv_folder_item_pair_path`. A RAW with no JPEG beside it is its own stop, and
`mv_folder_item.flags` bit 1 (primary is a RAW) drives its RAW badge. "Open RAW of pair" / "Open JPEG of pair" are palette commands
([16-commands.md](16-commands.md)). Opening either half selects the pair's stop.

Ambiguous groups degrade to separate stops.

## Companion files — hide, do not navigate

The listing ([`src/io/dir_win.cpp`](../../src/io/dir_win.cpp),
[`src/io/dir_mac.cpp`](../../src/io/dir_mac.cpp)) keeps only files with a decodable still or a
video extension, so companions are never navigation stops:

| Companion | How it is dropped |
|---|---|
| `.xmp`, `.thm`, `.aae`, `.wav` | Not a media extension |
| `._*`, `Thumbs.db`, `desktop.ini`, `.DS_Store` | Named explicitly |
| Hidden / system attribute (Windows), dot-files (macOS) | Dropped |

This is a listing filter; nothing is deleted. Child folders for gallery tiles also drop NAS/OS
housekeeping folders (`@eaDir`, `#recycle`, `$RECYCLE.BIN`, …; `io::list_subfolders`).

## Sticky zoom

`S` toggles sticky zoom: zoom and pan centre are kept as a fraction of the image when advancing,
instead of refitting. Default off. Camera state only — prefetch and generations are unchanged
([16-commands.md](16-commands.md)).

## Thumbnails / filmstrip

[`src/image/thumb.h`](../../src/image/thumb.h) (Windows), `thumb_mac.cpp` (macOS, same spec
and schema).

- **Spec `jpg512.2`:** a JPEG, long edge 512, never enlarged, ICC → sRGB, EXIF orientation
  applied. Produced by the same first-pixel ladder (DCT scaling, RAW embedded preview, else a
  decode).
- **Store:** SQLite (WAL) table `thumbs(path, mtime, size, spec, file)`, primary key
  `(path, mtime, size, spec)`, with the JPEG files beside the database in the per-user cache
  folder (`%LocalAppData%\MediaViewer\thumbs` on Windows). A row whose file has gone is a miss
  and is dropped. Bumping the spec regenerates old rows.
- mtime + size is the identity: a rewritten file of equal length at the same timestamp keeps
  its thumb.
- **Video posters:** `player::poster` software-decodes one frame near the head of the clip
  (single-threaded, no D3D11VA, sample aspect ratio applied) and stores it under the same spec.
- **Clip moments** (AI search results) are their own rows keyed by the path with `#t=<ms>`
  appended.
- The ABI returns a UTF-8 **path** to the JPEG; the filmstrip loads it as a bitmap. Pixels do not
  cross the ABI ([14-abi.md](14-abi.md)).
- Visible-first: after `FOLDER_READY` the host reports the on-screen range with
  `mv_folder_thumbs_visible`; those are generated first, then the rest, at the **folder**
  generation so arrow-key navigation does not cancel the filmstrip.
- The Explorer thumbnail handler uses `make_thumb_rgba` pixels directly.

## Animation (GIF / APNG / WebP / animated AVIF / HEIC sequences)

[`src/codec/anim.h`](../../src/codec/anim.h). Treated as a mini video: frames are decoded ahead
into a small ring on the session and pulled by the render thread
(`take_animation_frame`, native only), presented against elapsed time with per-frame delays.

- A delay of **10 ms or less is played as 100 ms**, as Chromium and Firefox do. APNG
  `delay_den = 0` means 1/100 s.
- Loop counts are honoured; when exhausted the last frame holds and presenting stops.
- Frame 0 of the still decode (`decode_gif` / `decode_webp`) is the animation's first pixel;
  later frames are never decoded before the first is shown. Prefetched neighbours never open an
  animation.
- Canvases over 256 MP are refused.
- Each frame is colour-converted with a cached `display_transform` built once per profile.
- When the current item is animated, Space is play/pause and `,` `.` step frames
  ([16-commands.md](16-commands.md)).

## Color

[`src/image/colour.h`](../../src/image/colour.h). The ICC profile (`APP2` / `iCCP` / `colr`) is
read and transformed with **LittleCMS** at decode time. Untagged JPEG/PNG/BMP is sRGB.
Display-referred sources are sRGB-encoded with **no tone map** ([03-rendering.md](03-rendering.md)).

- The **display** path is 8-bit in, 8-bit sRGB out: one LCMS 8→8 transform with its
  precomputed LUT (`cmsFLAGS_HIGHRESPRECALC`, `cmsFLAGS_NOCACHE`), colourimetrically
  ICC → linear Rec.709 → sRGB encode.
- An RGB matrix/shaper profile whose colorants and curves match sRGB within an 8-bit step is a
  **copy-through**. Wide-gamut (Display P3, Adobe RGB) and LUT profiles convert.
- A grey ICC on a greyscale JPEG uses a 256-entry grey → sRGB table.
- Each `display_transform` owns its own `cmsContext` and is built without LCMS's one-pixel
  cache, so one instance is safe from several threads. A small process-wide MRU cache keyed by
  the profile bytes shares them across workers.
- A **broken profile is `corrupt`**, never treated as untagged sRGB.
- PNG `gAMA`/`cHRM` without `iCCP` is not turned into a profile.
- The viewer LRU holds 8-bit sRGB textures. The float linear working space is the edit path
  ([07-photo-editing.md](07-photo-editing.md)), not the viewer blit.

## Mips

CPU Mitchell (B = C = 1/3), separable, filtered in **linear light** then sRGB-encoded,
holding four filtered rows at a time ([`src/image/upload.h`](../../src/image/upload.h)). Each
level is D3D11's size rule `max(1, floor(prev / 2))`. The same decimation builds the tiled
pyramid. Uploads are `CreateTexture2D(IMMUTABLE)` with `D3D11_SUBRESOURCE_DATA` on a decode
worker (free-threaded device).

## Pages

A multi-page file is one navigation stop; its pages are `Ctrl+PageUp` / `Ctrl+PageDown` inside it
(`next_page` / `prev_page`). `codec::decode(bytes, ctx, threads, page)` decodes one page and the
raster (then `display_image`, `gpu_image_mac`, `mv_image_info.page_count`) carries `page` and
`page_count`. TIFF pages are the full-resolution IFDs in file order; reduced-resolution subfiles (a
scanner's thumbnail) are skipped. Page 0 is the file as always — cached, prefetched, thumbnailed. A
page past 0 is one decode at the view generation the turn set: no cache, no prefetch, no first-pixel
preview, published under its own key so the canvas fits it like a new stop. Windows turns pages
through `mv_folder_select_page` (ABI 0.16); the Mac host calls `present_lab_mac::open_item(…, page)`.
The host remembers the count the core last reported and shows "Page n of m" in its notice line.

## Not built

- Android / Samsung motion photos (JPEG with an appended MP4) are not detected or played; such
  a JPEG is a plain still (metadata writes route it to a sidecar because of bytes after EOI).
- Live Photo pairing does not check the HEIC/MOV `ContentIdentifier`; it is stem-only.
- Live Photo hover-to-play and "Extract video".
- Voice-memo `.wav` companion playback; the "show hidden" toggle.
- Direction-aware prefetch weighting and a thumbnail-resolution ring beyond ±2.
- Progressive refinement of progressive JPEG / interlaced PNG passes.
- Disk thumbnail as first pixel for a still (used only for clip placeholders).
- An "ignore orientation" toggle.
- ICO size / HEIC sequence-frame navigation with `Ctrl+PageUp/PageDown` (TIFF, PDF and DOCX pages are built)
  (`codec::decode_page` takes a page index; no command uses it).
- Populating from the OS thumbnail cache; BC7 GPU-resident thumbnails; a content-hash key.
- macOS ImageIO path for HEIC.
