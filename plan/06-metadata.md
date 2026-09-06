# 06 — Metadata

## Libraries

- **Exiv2** — EXIF, IPTC, XMP; read *and write*; maker notes for Canon/Nikon/Sony/Fuji/Olympus/
  Panasonic/Pentax; handles JPEG, TIFF, PNG, WebP, HEIF/AVIF, JXL, PSD, and most RAW. This is the
  workhorse.
- **LibRaw** for RAW-specific camera fields Exiv2 doesn't surface (per-shot white balance
  coefficients, color matrices, lens corrections).
- **FFmpeg (libavformat)** for container/stream metadata — it is already the video pipeline
  ([05-video-pipeline.md](05-video-pipeline.md)), so read it in-process rather than shelling out to
  `ffprobe`.
- **libheif** for HEIF-specific items (depth maps, auxiliary images, Live Photo pairing).
- Windows Property System (`IPropertyStore`) only for *writing back* to the shell so Explorer
  agrees with you.

## Unified property model

Don't expose three different metadata vocabularies to the UI. Normalize into one:

```cpp
struct Property {
  PropertyKey key;        // enum: Camera_Make, Exposure_Time, GPS_Latitude, Video_Codec, ...
  Namespace   ns;         // Exif | IPTC | XMP | Container | Computed
  Value       value;      // variant<i64, double, Rational, string, DateTime, GeoPoint, blob>
  std::string raw_tag;    // "Exif.Photo.ExposureTime" — always keep the origin
  bool        writable;
};
```

Views the UI needs:

1. **Summary** — dimensions, file size, camera, lens, exposure triangle, date, GPS, duration,
   codec, bitrate, color space. The 90 % case, rendered as a clean card.
2. **Full tree** — every tag, grouped by namespace, searchable, with raw values shown on hover.
3. **Video streams** — per stream: codec, profile/level, resolution, pixel format, bit depth,
   chroma subsampling, frame rate (and whether it's VFR), color primaries/transfer/matrix,
   HDR mastering display + MaxCLL/MaxFALL, rotation, language, disposition. Same for audio
   (channels, layout, sample rate, bit depth) and subtitle tracks. Plus chapters and attachments.

## Writing

**v1 writes three fields: rating, orientation, and user comment.** Everything below is the correct
design for all of them, but do not build a batch engine before the read pane has been used in
anger — batch date-shift, copy-metadata, strip-on-share, and filename templating are v1.1.

Editing metadata is where you can lose someone's photos. Rules:

- **Atomic writes only**: write to `name.ext.tmp` in the same directory, `FlushFileBuffers`, then
  `ReplaceFileW` (preserves ACLs, attributes, and the original on failure).
- **Preserve everything you didn't touch** — including maker notes and unknown tags. Exiv2 does
  this if you modify its parsed structure rather than rebuilding it.
- **Never rewrite RAW files in place.** Write an **XMP sidecar** (`IMG_1234.xmp`) instead. This is
  the industry convention and it means a corrupt write can't destroy a negative.
- **Undo**: before the first write to a file in a session, snapshot the original metadata block to
  a local store so "revert metadata" is always available.
- Batch editing across a selection, with per-file success/failure reporting — never a silent
  partial success.

## Useful features that fall out of this

- Date/time shift (fix a camera set to the wrong timezone) across a selection.
- Copy metadata from one file to many.
- GPS: show a map pin; write coordinates onto files that lack them.
- Strip-all-metadata-on-copy ("share safely") — one click, produces a sanitized copy, never
  mutating the original.
- Filename templating from metadata for batch rename: `{date:yyyy-MM-dd}_{camera}_{seq:0000}`.
