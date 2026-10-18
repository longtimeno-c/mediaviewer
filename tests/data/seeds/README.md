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
image. They are covered by the repository licence (GPL-3.0-or-later) and are
safe to redistribute. The real format corpus stays out of git (docs/design/09).

| Family | Files | Made with |
|---|---|---|
| JPEG | baseline, progressive, ICC (sRGB), greyscale, 4:4:4 | Pillow (libjpeg-turbo) |
| PNG | RGBA8, palette, grey16, RGB16, APNG (3 frames, dispose/blend ops) | Pillow; RGB16 hand-built |
| BMP | 24-bit, 32-bit | Pillow |
| GIF | animated (4 frames), still | Pillow |
| WebP | lossy, lossless, animated | Pillow (libwebp) |
| TIFF | strip RGB8, strip LZW, deflate RGBA, tiled RGB8, strip RGB16, 2-page PackBits | hand-built; LZW/deflate/multipage via Pillow (libtiff) |
| ICO | one BMP (32-bpp DIB) entry + one PNG entry | hand-built |
| HEIC | 8-bit, 10-bit | pillow-heif (libheif + x265, dev venv only — never a product dependency, docs/design/11) |
| AVIF | still, animated (3 frames) | Pillow AVIF plugin (libavif + aom) |
| RAW | `tiny.dng`: DNG 1.4, 8-bit RGB thumbnail IFD0 + 16-bit RGGB CFA SubIFD | hand-built |
| PDF | two pages, the second `/Rotate 90` | `pdf_two_pages()`, hand-written objects and xref |
| Add-on data (`addon/`) | a settings page, a keymap pack and a schema 2 theme (docs/design/25 PR 56), the corpus of `fuzz_addon_contributions`; not read by the broken-corpus test | copied by hand from `examples/addons/settings-demo` and `vim-keys` |
| DOCX | heading, bold run, list item, two-cell table, a small page size | `docx_small()`, Python `zipfile` |
| PPTX | two slides over a master and layout: title and body placeholders, bullets, a numbered level, the slide number, a filled shape, a merged table cell, theme colours | `pptx_small()`, Python `zipfile` |
| C2PA (#290) | a JUMBF store; it in JPEG APP11 segments, a PNG `caBX` (with SD / ComfyUI text chunks) and an MP4 `uuid` box | `mv_tests "[.write-seeds]"` (`tests/test_meta_provenance.cpp`) |
| XMP (#290) | a Midjourney-like packet: unregistered namespace, struct, lang-alt, history | the same |
| ICC (#290) | a v4 display profile with `desc` (mluc) and `cprt` | the same |
| Location (#352) | what a person types in the Location editor: decimal, DMS with `°′″`, spaces, `º` and `''`, lettered and swapped, blank | hand-written text |
| Query (#291) | Local search queries (`fuzz_ai_query`): people, `or`, `-`, quotes, tag, date, place, rating and exposure terms, a name with a diaeresis | hand-written text |

The #290 families are written by the test builders instead:
`MV_SEED_DIR=tests/data/seeds mv_tests "[.write-seeds]"`.

Regenerate (bytes may change with encoder versions; that alone is not a bug):

```
python -m venv .venv-seeds
.venv-seeds/Scripts/pip install pillow pillow-heif
.venv-seeds/Scripts/python tools/testmedia/make-seeds.py
```

The whole set (seeds + broken) is kept under 1 MB.
