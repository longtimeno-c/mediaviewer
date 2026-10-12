# 06 — Metadata

How metadata is read into one model, shown (pane, overlays, sort), and written safely
(single-file edits, XMP sidecars, revert).

## Libraries

- **Exiv2** (`exiv2[bmff,png,xmp]`) — EXIF, IPTC, XMP and maker notes, read and write, for
  JPEG, TIFF, PNG, WebP, HEIF/AVIF and RAW. Statically from vcpkg on Windows; dynamically from
  `tools/mac/dependencies` on macOS.
- **FFmpeg (libavformat)** — container and per-stream facts for clips, read in-process
  ([`src/meta/clip.cpp`](../../src/meta/clip.cpp)).

The code is in [`src/meta/`](../../src/meta/), shared by both hosts.

## Unified property model

One vocabulary for the UI ([`src/meta/meta.h`](../../src/meta/meta.h)). `meta::read(path)`
fills a `metadata`:

```cpp
enum class origin { exif, iptc, xmp, container, computed,   // Exiv2, libavformat, the viewer
                    c2pa, png_text, icc, xmp_raw };         // #290: meta/parse.h

struct geo_point { double lat = 0, lon = 0; std::optional<double> altitude; };
// summary also holds: std::optional<geo_point> where; std::string made_with; bool ai_generated;

struct property {          // one row of the full tree
  origin      space;
  std::string group;       // "Exif.Photo", "Xmp.dc", "Container", "Video #0"
  std::string name;        // "ExposureTime"
  std::string label;       // "Exposure Time"
  std::string value;       // human form "1/250 s"
  std::string raw;         // untranslated "1/250"
  std::string raw_tag;     // full key — the origin is never lost
};

struct metadata {
  summary                  s;           // the card
  std::vector<property>    properties;  // the full tree
  std::vector<stream_info> streams;     // per stream: kind, codec, label/value fields
  std::vector<chapter>     chapters;
  std::vector<af_point>    af_points;   // stored grid, 0..1
  // + orientation, rating, comment, display_orientation, write target …
};
```

Reading is worker-thread work and happens once per opened item. Missing metadata is not an
error: an empty field set comes back OK; an error means the file could not be read. A sidecar
beside a file wins on read for rating and comment.

Views:

1. **Summary** (`summary_rows`) — dimensions, file size, format, camera, lens, exposure,
   aperture, ISO, focal length, date taken, GPS, duration, codec, bitrate. Every row for the
   item's kind is present; an empty one shows "—" so missing reads as missing. The one
   exception is **Made with** (#290), shown only when the file names a generator or says it
   is AI-made ("ChatGPT · AI-generated"): no provenance is not a gap a file is expected to fill.
2. **Full tree** ("All tags") — every tag, grouped by source, searchable, raw key and value
   kept. Both hosts list every row (Windows uses a virtualised `ItemsRepeater`; there is no row
   cap in the pane). The read model caps a file at 4 000 rows so a hostile file cannot build a
   100 MB tree.
3. **Streams** — per video/audio/subtitle/data/attachment stream: codec and labelled fields
   (profile/level, resolution, pixel format, bit depth, frame rate, colour, HDR mastering
   metadata, rotation, channels, sample rate, language…), plus chapters.

`summary` also carries numeric clip facts (audio channels, sample rate, every codec) for
non-human consumers such as the Spotlight importer, and the date-taken sort key.

**Position as numbers** (#290): `summary::where` keeps the GPS fix the `gps` display string is
made from — WGS 84 degrees, north and east positive, altitude in metres (negative below sea
level) when the file gives one. Sources in order: EXIF `GPSLatitude/Longitude` (+Ref,
`GPSAltitude`/`GPSAltitudeRef`); else XMP `exif:GPSLatitude/GPSLongitude` in XMP's
`"DDD,MM.mmmk"` / `"DDD,MM,SSk"` form (drones such as DJI write only these) with
`exif:GPSAltitude`; else a clip's ISO 6709 `com.apple.quicktime.location.ISO6709` /
`location`, altitude included. Place names and the map (#344) read `where`, never the string.

## Beyond Exiv2 (#290)

Exiv2 does not surface everything a file carries. [`src/meta/parse.h`](../../src/meta/parse.h)
holds portable, bounds-checked readers (library `mv_meta_parse`: no Exiv2, no FFmpeg, no platform
header, D9) whose rows join the tree:

| Origin / group | What | From |
|---|---|---|
| `c2pa` "Content Credentials (C2PA)" | Claim generator (`claim_generator_info` name + version, else `claim_generator`), title, actions, digital source type, software agent, signer (leaf certificate subject O / CN), certificate issuer, signing time (RFC 3161 `genTime`), manifest and ingredient counts, and "Validation: not checked" | The JUMBF store: JPEG APP11 (segments reassembled by sequence number, continuation headers dropped, the instance labelled `c2pa` chosen), PNG `caBX`, ISO-BMFF top-level `uuid` (C2PA UUID, purpose `manifest`) or `c2pa` box in HEIF / AVIF / MP4 / MOV |
| `c2pa` "C2PA assertions" | Every assertion of every manifest (active first) and the claim, flattened to `label.path[n].key = value`; byte strings shown as their size | CBOR (bounded decoder) and JSON boxes |
| `png_text` "PNG text" | Every `tEXt`, `zTXt` and `iTXt` chunk, compressed or not, keyed `PNG.<chunk>.<keyword>`, `iTXt` language in the label; text shown up to 2 048 characters | Latin-1 by the spec, UTF-8 kept when valid; XMP and Exiv2's "Raw profile type" chunks stay Exiv2's |
| `icc` "ICC profile" | Description, copyright, device manufacturer / model, version, class, colour space, PCS, date, CMM, creator, rendering intent | The profile Exiv2 already reassembled (`desc` / `text` / `mluc`). The description also replaces the summary's "Embedded ICC profile" placeholder |
| `xmp_raw` "Xmp.<prefix>" | Any leaf of the XMP packet Exiv2 did not list, keyed as the packet spells it in Exiv2's path form (`History[2]/stEvt:action`) | An RDF walk over the packet Exiv2 holds (`codec/xml.h`, DOCTYPE refused); checked against Exiv2's keys by namespace URI. Exiv2 0.28 registers unknown namespaces itself, so in practice this is a safety net for what it cannot key |
| `container` "JFIF", "JPEG", "WebP", "HEIF", "PNG" | JFIF version / density / thumbnail, JFXX, COM comments, Adobe APP14 transform; WebP features, canvas, loop count, background, frames, lossy / lossless, alpha; HEIF brands, primary item, item types, references, `mime` items, auxiliary images (alpha, depth, HDR gain map), `nclx` colour, bits per channel; PNG `pHYs`, `sRGB`, `gAMA`, `acTL` | The container's own structures |

Cost: the readers run over the 8 MB prefix `read` already holds, and the packet and profile
Exiv2 already parsed; only ISO-BMFF opens the file again, for a top-level header walk and the
boxes it keeps (meta ≤ 4 MB, C2PA ≤ 32 MB). Every loop, nesting level, decompression (1 MB per
chunk, 8 MB per file), string and row count is capped. Nothing is validated and nothing goes
online (rule 6): a C2PA signature, hash or certificate chain is shown as the file states it.
`read_date_taken` does not run them. Each reader has a libFuzzer harness (`fuzz_meta_*`,
[09-build-and-test.md](09-build-and-test.md)).

All of these rows are **read-only** (`row_access`, rule 5): only Exiv2's own EXIF / IPTC / XMP
rows go through `access_of`, and PR 12's writer and sidecar rule are unchanged.

## Made with and AI provenance (#290)

`summary::made_with` and `summary::ai_generated` are derived once, in the core, by
`derive_provenance` ([`src/meta/generators.h`](../../src/meta/generators.h)) from what the
readers collected. One table of known generators (ChatGPT / GPT-4o / gpt-image, DALL·E, Sora,
OpenAI, Adobe Firefly, Midjourney, Stable Diffusion and its UIs — AUTOMATIC1111, ComfyUI,
InvokeAI, Fooocus, NovelAI, DreamStudio — Bing Image Creator, Microsoft Designer, Meta AI,
Google Imagen / Gemini, Leonardo.Ai, Ideogram, FLUX, Runway, Luma, Kling, Krea, Playground,
Canva Magic Media, Recraft, NightCafe, Craiyon, Grok), matched as whole words, ignoring case,
the more specific spelling first; tested in `mv_tests "[provenance]"`.

- **made_with** is the first table match in: C2PA claim generator, C2PA action software agents,
  PNG `Software`, other PNG generator text (`Generator`, `Source`, `Model`, the `Version:` tail
  of `parameters`), XMP `CreatorTool`, EXIF `Software` (or a clip's software tag). Else
  "Stable Diffusion" for a PNG `parameters` chunk, or "ComfyUI" for `prompt` / `workflow` JSON
  chunks. Else the C2PA claim generator as written — a signed claim names its tool even when
  it is not a generator ("Adobe Photoshop 25.0"). A camera's or an editor's Software that the
  table does not know is **not** "made with".
- **ai_generated** is true on positive evidence only: a C2PA action's or XMP
  `Iptc4xmpExt:DigitalSourceType` of `trainedAlgorithmicMedia` or
  `compositeWithTrainedAlgorithmicMedia` (IPTC URI or bare term, in any manifest of the store,
  since an ingredient's AI origin is still evidence), a table match, or the PNG generator chunks.
- Never from the file name, never from pixels. A file with no provenance is not "not AI":
  missing metadata proves nothing, and nothing in the UI says otherwise.

The hosts get these as POD next to the tables: `chrome_meta_args` (Windows, 80 bytes) and
`mv_chrome_meta_facts_get` / `mv_chrome_meta_made_with` (Mac bridge); see
[14-abi.md](14-abi.md).

## Search text (#291)

Every tag searchable, without the AI pack ([16](16-commands.md) "File search"). Code:
[`src/meta/search.h`](../../src/meta/search.h), [`meta_cache.h`](../../src/meta/meta_cache.h),
[`tag_index.h`](../../src/meta/tag_index.h); tests `mv_tests "[search]"`.

- **The fields.** `search_facts_of` builds one list per file from the read model, in "why"
  order, no empty values, no repeats (a value already inside an earlier field is dropped, so
  "Canon" does not repeat "Canon EOS R5"): camera (`summary::camera`, plus a make the camera line
  lacks: "NIKON CORPORATION"), lens, made-with, software (EXIF `Software`, XMP `CreatorTool`, a
  clip's software), title (XMP `dc:title`, IPTC object name and headline, a clip's `title`),
  caption (XMP `dc:description`, IPTC caption, EXIF `ImageDescription`, a clip's description or
  comment, the PR 12 user comment), each keyword (XMP `dc:subject`, IPTC keywords, a clip's
  keywords), creator (EXIF `Artist`, XMP `dc:creator`, IPTC by-line, a clip's artist / author).
  Camera placeholders ("OLYMPUS DIGITAL CAMERA", "SONY DSC") and blanks make no field. XMP
  sidecars count, as everywhere.
- **No place names.** The Places add-on names places and answers `portugal` through its search
  provider ([27](27-places.md), [25](25-open-addons.md) §7); the app links no geocoder (owner,
  2026-10-11). The facts keep the coordinates (`summary::where`) for its `locations_in`.
- **The fold** is `core/fold.h`, the hosts' name rule (decompose, drop marks, lowercase; ß, æ, ø,
  ł stay). Header-only so the AI pack folds with it too. The fields are folded and joined by
  U+001F, so a word never spans two fields and a match names its field.
- **Matching:** every typed word must be found somewhere in the file's folded fields; the "why"
  is the field the first word matched in ("Canon EOS R5", "ChatGPT"), cut at 48 characters. One
  folded letter matches nothing (a single key would light up every camera make).
- **The read** for search is bounded: `read_search_facts` parses the file's first 512 KB (where
  JPEG APPn, PNG chunks, WebP chunks and the HEIF meta box live), retries a TIFF / RAW on the pane's
  8 MB when the head held no tags, reads a clip through libavformat, and overlays the sidecar.
  No write-target probe. On a 6 MP camera JPEG: 0.19 ms, against 10.9 ms for the pane's full
  `meta::read` and 0.09 ms for `read_date_taken` (`mv_tests "[.perf-search]"`, M-series Mac,
  warm cache).
- **`meta_cache.db`** (SQLite, WAL) next to the thumbnail cache: one row per file, valid while
  path, size, mtime and `kSearchFactsVersion` match. It holds the fields and their fold, date
  taken, the position and the AI flag. A damaged or foreign file (not a database, another schema,
  or corruption found by a later write) is deleted and made again (it is a cache); a file another
  process has locked (a second window) is busy, never deleted, and an open that fails is retried
  at most every 30 s. Rows of files gone from a folder are dropped when the folder is seen again
  (a file only hidden by a setting keeps its row); a folder never revisited keeps its rows, as the
  thumbnail cache keeps its thumbs (about 0.5–0.9 KB a file, path included). A PR 12 write that lands in a sidecar drops the file's row (`tag_index::invalidate`
  on the Mac, `mv_folder_forget_tags` on Windows), since the file's own stamp cannot tell.
  Nothing in it leaves the machine (rule 6).
- **The folder pass** (`tag_index`) keeps the facts of the folder on screen in memory. A folder
  seen before costs one `SELECT` and no file reads; otherwise each file is read once, ever. It
  runs as background-queue chunks of 32 files, so every view job and the ±2 prefetch go first.
  Between files it steps aside for the view: a view-tied job waiting for a worker gets the pass's
  worker (the chunk re-queues itself where it stopped), and while one runs (the photo's full
  decode, the prefetch; `job_system::foreground_running`) or the viewer is presenting
  (`g_present_busy` / `mv_present_set_busy`) it waits. A relist of the same folder reuses the rows
  already in memory (one `SELECT` per folder). It starts only once the item on screen has its first image (Mac: the
  0.2 s folder timer sees `showing_still`, or a second has passed for a clip; Windows: the
  selected stop's `IMAGE_OPENED`, or the first tag search). Sorting by date taken starts it at
  once, as the date scan did, and the listing re-sorts once, when the pass ends (both hosts).
- **Date taken rides the same pass.** The sort keys come from the facts, so a folder costs one
  read per file, not a date read and a tag read, and a folder seen before sorts by date at once.
  On a folder never seen, each key costs the 0.19 ms search read instead of the 0.09 ms date
  read.
- **Served to add-ons:** the host table's `tag_facts` (the AI pack's schema 4,
  [17](17-local-ai-search.md)) and, through #348, `location_of` / `locations_in` for the Places
  provider read the same cache.

## Writing

[`src/meta/write.h`](../../src/meta/write.h). Single file, one change set at a time; worker
threads only.

**Fields:** rating (1–5, −1 rejected; 0 or remove clears every rating tag), EXIF orientation
(1–8), user comment (UTF-8; `""` removes), and — from PR 29 — **any one EXIF / IPTC / XMP tag**
by its full Exiv2 key (set or remove, in Exiv2's string form), plus the capture date written to
every tag that holds it (EXIF `DateTimeOriginal` always and `DateTimeDigitized`; XMP
`exif:DateTimeOriginal`, `xmp:CreateDate`, `photoshop:DateCreated`).

Ratings are a keyboard cull: numpad `0`–`5` or `Ctrl+Shift+0`–`5`. The number row stays
fit/100 % ([16-commands.md](16-commands.md)). Issue #207 adds the rest of a cull on the same
writer: `X` toggles reject (rating −1) and numpad / `Ctrl+Shift` `6`–`9` set the colour label
(`U` clears it), which is the one tag XMP `xmp:Label` (a PR 29 tag edit, `shell::label_fields`)
in Lightroom's words. The read model carries it as `summary::label` (the sidecar wins) and the
summary card as a "Label" row. View ▸ Show filters the listing on them (Picks, Not rejected, a
colour; [16-commands.md](16-commands.md) "Status, sort, filter, typeahead"), from
`read_cull_marks` below.

**Where a change lands** (`write_target_for`, decided by magic bytes and structure):

- **A plain JPEG is edited in place** through Exiv2's parsed structure, so untouched tags —
  maker notes included — carry over. The new bytes are re-read and **checked against the
  original before anything is replaced**: untouched EXIF / XMP / IPTC values, the thumbnail,
  the ICC profile and every non-metadata JPEG segment must match, or the write fails
  (`internal`) and the original is never touched. Rating goes to XMP; EXIF rating tags are
  updated only where the file already had them, so a rating never grows IFD0.
- **Everything else gets an XMP sidecar** (`IMG_1234.CR2` → `IMG_1234.xmp`; a name with no
  extension gets `.xmp` appended), merged with any sidecar already there: camera RAW, HEIC/AVIF,
  TIFF, PNG, WebP, BMP/GIF/ICO, video, and a JPEG with an MPF table or bytes after its final EOI
  (a motion photo). The original is never opened for writing.

Per-tag access (`access_of`): `editable` (set and remove), `via_sidecar` (set only; the value
goes to the sidecar under its XMP name), `read_only` (structure, maker notes, computed or
container rows). The pane's column uses `row_access`, which is `read_only` for every row that
is not Exiv2's own EXIF / IPTC / XMP (C2PA, PNG text, ICC, raw XMP, container, computed).

**Atomic replace** goes through the `io` replace port
([`src/io/replace.h`](../../src/io/replace.h)): Windows writes a same-directory temp,
`FlushFileBuffers`, then `ReplaceFileW` (preserves ACLs, attributes, and the original on
failure); macOS writes a same-directory temp, `F_FULLFSYNC`, then `rename(2)`. Never an
in-place rewrite.

**Revert:** before the first write to a file in a session, its metadata is snapshotted to a
host-owned local store — the three fields, a JPEG's metadata segments byte for byte, and the
sidecar as it was. "Revert metadata" restores every tag. If a due snapshot cannot be written,
nothing is written to the file. Each write also records the sidecar as it left it (absent, or
the BLAKE3 of its bytes); revert refuses with `io`, replacing nothing, when the sidecar has
changed since — `IMG_1234.HEIC`, `.MOV` and `.CR2` share `IMG_1234.xmp`, and Lightroom or
darktable may have edited it. Nothing logs a path.

Errors: `invalid_arg` (value out of range, non-UTF-8 comment, a field this file cannot hold),
`io` (unreadable, not replaceable, changed under us), `corrupt` (a JPEG Exiv2 cannot parse),
`internal` (the post-write check failed; the original is intact).

**Lossless rotate** writes orientation (or a lossless JPEG transform) and then calls
`mv_folder_forget` so the navigation LRU does not republish the old pixels
([14-abi.md](14-abi.md)).

**The selected file's stamp** (path, size, mtime: the key records and edits are filed under) is
the folder listing's, never a stat on the UI thread. A write moves it before the watcher
relists, so its job stats the file on the worker and the host keys by that stamp until the
listing catches up (Windows `shell/listing_stamp.h`; the Mac patches its own listing).

**Export carry** (`read_carried`): for a re-encoded export of a non-JPEG/PNG still, EXIF is
rebuilt as a TIFF block and XMP as a packet from what Exiv2 read — structural IFD0 tags left
out, orientation written as 1, maker notes kept only while the block fits one APP1 segment.

## Overlays that fall out of the read model

These read the already-parsed `metadata` — no file I/O on toggle
([16-commands.md](16-commands.md)):

- **On-canvas info (`O`)** — filename, index, dimensions, then camera, exposure and date lines
  (`overlay_*_line`; an empty line is not drawn).
- **AF-point quads (`Shift+O`)** — from maker notes ([`src/meta/af.h`](../../src/meta/af.h)):
  Canon `AFInfo2` (in-focus / selected points), Nikon `AFInfo2`, Sony `FocusLocation`, Fujifilm
  `FocusPoint` and EXIF `SubjectArea`. Stored-grid boxes are mapped through exactly the
  orientation the decoder already applied (`displayed_af_points`). Off by default. The Canon
  Y-axis sign has not been checked against a real Canon file.
- **Eyedropper (`Shift+I`)** — a one-pixel readback on demand, sRGB 8-bit + hex; `Ctrl+C` copies
  the colour while it is on.
- **Sort by date taken** — sort key 4 of `mv_folder_set_sort`. The keys come from the folder's
  tag pass ("Search text" above): one bounded read per file, once ever (meta_cache.db); until a
  stamp lands the file sorts by mtime, and the listing re-sorts when stamps arrive. Camera stamps
  carry no zone, so the key is the stamp read as UTC. A Photos library item's key is its
  `creationDate`.
- **Rating and label for the cull filter** (issue #207) — `read_cull_marks` reads the rating and
  `xmp:Label` alone (`io::cull_marks`: −1..5 and Red / Yellow / Green / Blue / Purple, or
  "other" for another app's word), on a worker, once per file: the same 256 KB head as
  `read_date_taken` (8 MB only when the head does not parse), then the sidecar, which wins as in
  `read`. A clip is its sidecar. An in-file rating past the head (another app's PNG with XMP after
  the pixels) is not seen; this app writes those formats' ratings to the sidecar. Cached by
  (path, mtime, size): in the session on Windows, in the Mac host's `meta_store`; a write this app
  makes drops its file's entry, because a sidecar write leaves the file's own stamp alone.

## Not built

- LibRaw-specific camera fields (white-balance coefficients, colour matrices) and libheif
  auxiliary items (depth maps) in the read model.
- Writing back to the Windows Property System.
- Batch metadata: date/time shift across a selection, copy metadata to many, filename
  templating, strip-on-share copies.
- GPS map pin / writing coordinates.
- Keywords as a feature. (Colour labels and a filter on them are built, issue #207.)
