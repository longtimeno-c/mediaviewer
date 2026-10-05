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
enum class origin { exif, iptc, xmp, container, computed };

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
   item's kind is present; an empty one shows "—" so missing reads as missing.
2. **Full tree** — every tag, grouped, searchable, raw value kept.
3. **Streams** — per video/audio/subtitle/data/attachment stream: codec and labelled fields
   (profile/level, resolution, pixel format, bit depth, frame rate, colour, HDR mastering
   metadata, rotation, channels, sample rate, language…), plus chapters.

`summary` also carries numeric clip facts (audio channels, sample rate, every codec) for
non-human consumers such as the Spotlight importer, and the date-taken sort key.

## Writing

[`src/meta/write.h`](../../src/meta/write.h). Single file, one change set at a time; worker
threads only.

**Fields:** rating (1–5, −1 rejected; 0 or remove clears every rating tag), EXIF orientation
(1–8), user comment (UTF-8; `""` removes), and — from PR 29 — **any one EXIF / IPTC / XMP tag**
by its full Exiv2 key (set or remove, in Exiv2's string form), plus the capture date written to
every tag that holds it (EXIF `DateTimeOriginal` always and `DateTimeDigitized`; XMP
`exif:DateTimeOriginal`, `xmp:CreateDate`, `photoshop:DateCreated`).

Ratings are a keyboard cull: numpad `0`–`5` or `Ctrl+Shift+0`–`5`. The number row stays
fit/100 % ([16-commands.md](16-commands.md)).

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
container rows).

**Atomic replace** goes through the `io` replace port
([`src/io/replace.h`](../../src/io/replace.h)): Windows writes a same-directory temp,
`FlushFileBuffers`, then `ReplaceFileW` (preserves ACLs, attributes, and the original on
failure); macOS writes a same-directory temp, `F_FULLFSYNC`, then `rename(2)`. Never an
in-place rewrite.

**Revert:** before the first write to a file in a session, its metadata is snapshotted to a
host-owned local store — the three fields, a JPEG's metadata segments byte for byte, and the
sidecar as it was. "Revert metadata" restores every tag. If a due snapshot cannot be written,
nothing is written to the file. Nothing logs a path.

Errors: `invalid_arg` (value out of range, non-UTF-8 comment, a field this file cannot hold),
`io` (unreadable, not replaceable, changed under us), `corrupt` (a JPEG Exiv2 cannot parse),
`internal` (the post-write check failed; the original is intact).

**Lossless rotate** writes orientation (or a lossless JPEG transform) and then calls
`mv_folder_forget` so the navigation LRU does not republish the old pixels
([14-abi.md](14-abi.md)).

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
- **Sort by date taken** — sort key 4 of `mv_folder_set_sort`. `read_date_taken` reads a bounded
  prefix once per file on a worker, remembered per (path, mtime, size); until a stamp lands the
  file sorts by mtime, and the listing re-sorts when stamps arrive. Camera stamps carry no zone,
  so the key is the stamp read as UTC.

## Not built

- LibRaw-specific camera fields (white-balance coefficients, colour matrices) and libheif
  auxiliary items (depth maps) in the read model.
- Writing back to the Windows Property System.
- Batch metadata: date/time shift across a selection, copy metadata to many, filename
  templating, strip-on-share copies.
- GPS map pin / writing coordinates.
- Colour labels / keywords as a feature (`U` to clear a label is unbound).
