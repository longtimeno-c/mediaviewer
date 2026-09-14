# Seeds and broken files (PR 7)

`seeds/<family>/` — one small, valid file per format family and variant that
matters. They start the libFuzzer corpora (`tools/fuzz/`) and are the inputs the
broken-corpus test (`tests/test_broken_corpus.cpp`) mutates.

`../broken/` — hand-crafted hostile files: decompression bombs, IFD loops,
many-frame GIFs, overlapping ICO entries, patched dimension fields. Minimised
fuzzer reproducers go there too, named `<family>_<what>.<ext>`.

## Provenance and licence

Every file here is **synthesised by `tools/testmedia/make-seeds.py`** from
generated gradients. No camera output, no downloaded sample, no third-party
image. They are covered by the repository licence (GPL-2.0-or-later) and are
safe to redistribute. The real format corpus stays out of git (plan/09).

| Family | Files | Made with |
|---|---|---|
| JPEG | baseline, progressive, ICC (sRGB), greyscale, 4:4:4 | Pillow (libjpeg-turbo) |
| PNG | RGBA8, palette, grey16, RGB16, APNG (3 frames, dispose/blend ops) | Pillow; RGB16 hand-built |
| BMP | 24-bit, 32-bit | Pillow |
| GIF | animated (4 frames), still | Pillow |
| WebP | lossy, lossless, animated | Pillow (libwebp) |
| TIFF | strip RGB8, strip LZW, deflate RGBA, tiled RGB8, strip RGB16, 2-page PackBits | hand-built; LZW/deflate/multipage via Pillow (libtiff) |
| ICO | one BMP (32-bpp DIB) entry + one PNG entry | hand-built |
| HEIC | 8-bit, 10-bit | pillow-heif (libheif + x265, dev venv only — never a product dependency, plan/11) |
| AVIF | still, animated (3 frames) | Pillow AVIF plugin (libavif + aom) |
| RAW | `tiny.dng`: DNG 1.4, 8-bit RGB thumbnail IFD0 + 16-bit RGGB CFA SubIFD | hand-built |

Regenerate (bytes may change with encoder versions; that alone is not a bug):

```
python -m venv .venv-seeds
.venv-seeds/Scripts/pip install pillow pillow-heif
.venv-seeds/Scripts/python tools/testmedia/make-seeds.py
```

The whole set (seeds + broken) is kept under 1 MB.
