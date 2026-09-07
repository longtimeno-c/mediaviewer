# 04 — Image Pipeline

## Format coverage and which library owns each

Decide by **file signature (magic bytes), never by extension.**

| Family | Formats | Library |
|---|---|---|
| JPEG | baseline, progressive, 12-bit, arithmetic | **libjpeg-turbo** (SIMD; also gives you DCT-domain scaling) |
| PNG / APNG | 8/16-bit, all color types, animation | **libspng** (fast, small) or libpng |
| WebP | still + animated + lossless | **libwebp** |
| GIF | incl. animation | **libnsgif** or giflib |
| HEIF / HEIC | HEVC- and AV1-coded, image sequences, Live Photos | **libheif** + **libde265** (or libheif's FFmpeg decoder plugin) + **dav1d** |
| AVIF | still + animated | **libavif** + dav1d |
| JPEG XL | lossy/lossless, progressive | **libjxl** |
| TIFF | tiled, striped, 32-bit float, LZW/ZIP/JPEG, multi-page | **libtiff** |
| RAW | CR2/CR3, NEF, ARW, ORF, RAF, RW2, DNG, PEF, SRW, IIQ, 3FR… (~800 cameras) | **LibRaw** |
| OpenEXR / HDR | EXR half/float, Radiance .hdr | **OpenEXR** + custom .hdr reader |
| PSD | flattened composite + layer extraction | **libpsd** or your own reader (the composite is easy) |
| SVG | vector | **resvg** or lunasvg |
| DDS / KTX2 | BCn / ASTC | **DirectXTex** |
| Misc | BMP, ICO, TGA, PNM, QOI, PCX, JPEG2000 (OpenJPEG) | **stb_image** + OpenJPEG for the tail |
| Fallback | anything above missed | **WIC**, last resort only |

Register each as `{ probe(span<byte> header) -> confidence, open(stream) -> Decoder }` in a table.
Adding a format must be one file plus one registry line.

x265 does **not** appear here: it is a GPL *encoder*, and D3 already forbids shipping a software
HEVC encoder. HEIC decoding needs libde265 or FFmpeg's `hevc` decoder — and since FFmpeg is
already the video pipeline (**D2**), libheif's FFmpeg decoder plugin avoids carrying two HEVC
stacks in one binary.

**v1 ships only the camera-dump set** (**D5** in [01-decisions.md](01-decisions.md)): JPEG, PNG,
BMP, GIF, TIFF, WebP, HEIC/HEIF, AVIF, ICO, and RAW. AVIF makes v1 only because dav1d already
ships for AV1 video. JPEG XL, OpenEXR, Radiance, PSD, SVG, DDS/KTX2, JPEG 2000, and QOI are
**v1.1** — the registry above is precisely what makes deferring them cost nothing.

## Progressive display — the perceived-speed trick

Users judge speed by **time to first pixel**, not time to full decode. Always show something
within ~30 ms:

1. **Embedded preview first.** Every RAW file carries a full-size embedded JPEG; HEIC has a
   thumbnail item; JPEG has an EXIF thumbnail. Decode that immediately and display it.
2. **DCT-scaled JPEG.** libjpeg-turbo decodes at 1/2, 1/4, 1/8 scale for ~1/8 the cost. Show the
   1/4-scale image, then swap in full res.
3. **Progressive refinement** for progressive JPEG, JXL, and interlaced PNG — decode passes
   incrementally and re-upload.
4. Cross-fade preview → full over 80 ms so the swap isn't a visible pop.

Full-quality decode of a 45 MP RAW takes 200–600 ms no matter what. The user must never *see*
that.

## Tiled pyramid for large images

Above ~64 MP (gigapixel panoramas, scanned film, PSDs), do not hold one giant texture:

- Split into **256×256 tiles** with a mip pyramid down to a single tile.
- Decode and upload only tiles intersecting the viewport at the current LOD, plus a one-tile ring.
- Keep the coarsest 2–3 levels resident always, so a fast zoom-out is instant and blurry rather
  than blank.
- Tile cache is the VRAM LRU from [02](02-architecture.md).

## Prefetch

When viewing index `i` in a folder, speculatively decode `i+1, i+2, i-1` at full res and
`i±3..i±8` as thumbnails, at low thread priority. Direction-aware: if the user is pressing Right
repeatedly, weight forward 4:1. Cancel and re-target on every navigation (generation counter).

This is what makes arrow-key browsing feel instantaneous.

## Live Photos and motion photos — the actual object in a camera dump

A phone dump is not a folder of stills and a folder of clips. It is full of **paired items**, and
how you present them is a product decision the plan must make rather than discover:

| Source | On disk | Detection |
|---|---|---|
| **iPhone Live Photo** | `IMG_1234.HEIC` + `IMG_1234.MOV`, same basename | Matching basename + the `ContentIdentifier` in the HEIC metadata and the MOV's `com.apple.quicktime.content.identifier` |
| **Android motion photo** | **One JPEG** with an MP4 appended | XMP `MotionPhoto` / `MicroVideo` tags carrying a byte offset into the same file |
| **Samsung** | One JPEG, trailing MP4 with a marker | Trailer scan |

**v1 behaviour: one item, not two.** Pair on scan, show a single filmstrip entry with a small
Live-badge, and the still is the primary — it is what fills the canvas, what gets edited, and what
exports. **Hover or hold plays the motion clip** in place; the pair never becomes two navigation
stops, because arrow-keying through a holiday dump and hitting the same photo twice is exactly the
papercut people switch viewers to escape.

The video half must be **reachable and extractable** (right-click → "Extract video"), because the
other half of the complaint is a viewer that hides it entirely.

If pairing detection fails, degrade to showing both — never hide a file.

## Thumbnails / filmstrip

- Persistent thumbnail cache: a single append-only file plus an index (or SQLite with WAL), keyed
  by `(path, size, mtime, content-hash-of-first-64 KB)`.
- Store as **BC7-compressed** 512 px squares — 1/4 the VRAM, negligible quality loss at that size,
  and uploadable without recompression.
- Populate from the OS thumbnail cache (`IThumbnailCache`) on first sight for instant results,
  then replace with your own higher-quality render in the background.

## Animation (GIF/APNG/WebP/animated AVIF/HEIC sequences)

Treat as a mini video: decode frames ahead into a small ring, present on the render thread against
QPC time with per-frame delays honored (clamp `delay < 20 ms` to 100 ms, matching browser behavior
for legacy GIFs). Loop counts respected. Scrubbable.

## Color

Read the ICC profile (`APP2`/`iCCP`/`colr` box) and transform via **LittleCMS** to linear Rec.709
at decode time. Untagged JPEG → assume sRGB. Untagged RAW → camera matrix from LibRaw.
Display-referred sources then **sRGB-encode with no tone map** (D6, [03](03-rendering.md)).

The **viewer LRU** is 8-bit sRGB (or RGB10A2), not FP16 — [02](02-architecture.md). FP16 is the
edit working space, promoted when an edit stack is active. The linear hop still happens at
decode (ICC → linear → sRGB OETF); skipping it and treating a tagged file as sRGB is a bug.

- LittleCMS calls on the decode pool use a **per-job `cmsContext`**. The default/global context
  is not thread-safe; two tagged files at once is a data race.
- A **broken profile is not untagged sRGB.** Fail the transform (`corrupt` / `unsupported`).
  Copying the encoded bytes through with `icc_tagged = false` still treats tagged pixels as
  sRGB, which is the D6 bug. The overlay must not claim ICC correctness it did not deliver.
- PNG `gAMA`/`cHRM` without `iCCP` is not a substitute for a profile; do not invent one.

## Mips (until the compute pass in [03](03-rendering.md))

CPU Mitchell is acceptable for PR 2. **Each level's size is D3D11's rule: `max(1, floor(prev/2))`.**
Ceil (`(w+1)/2`) uploads the wrong pitch into `CreateTexture2D` on any odd dimension. Filter in
linear, then encode; do not box-filter 8-bit sRGB.
