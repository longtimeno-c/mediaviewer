# tests/data — generated HEIC / AVIF fixtures

Tiny (about 60 KB total) synthetic images for `tests/test_heif_avif.cpp`. Every
file here was generated from flat colours and gradients by the scripts in this
directory; no photograph or third-party media is included. They are
licence-clean: the pixels are trivial test patterns (CC0 / public domain), and
the Display P3 ICC profile embedded in some files was written by
`gen_heif.py` from the published primaries.

Real camera media does **not** go here (plan/09). Real iPhone samples are
fetched by `tools/testmedia/fetch-heif.ps1` into the gitignored
`tools/testmedia/`, and their tests skip visibly when absent.

## How they were made

The generators are dev tools only. Nothing here is a build dependency: the app
decodes HEIC with libheif + libde265 and AVIF with libavif + dav1d, and never
links an encoder.

```
py -3.11 -m venv heifvenv
heifvenv/Scripts/python -m pip install pillow-heif pillow numpy   # pillow-heif 1.7.0, Pillow 12.3.0
heifvenv/Scripts/python gen_heif.py heif          # also writes heif/display_p3.icc (not committed)
heifvenv/Scripts/python gen_avif_pillow.py avif heif/display_p3.icc
sh gen_avif.sh avif                               # ffmpeg 8.1 with libaom-av1
# The encoders drop some CICP codes (ffmpeg's avif muxer always writes nclx
# 2/2; pillow-heif stores transfer 13 when asked for PQ 16). The bitstreams
# carry no conflicting description, so the colr box is patched afterwards:
python patch_nclx.py avif/srgb_8bit.avif 1 13 1 1
python patch_nclx.py avif/p3_nclx.avif 12 13 5 1
python patch_nclx.py heif/pq_10bit.heic 9 16 9 1
```

(pillow-heif's wheel bundles x265 to *encode* the HEIC fixtures on the
developer machine. That is the fixture tool, not the product; plan/11's
no-x265 rule is about what MediaViewer links.)

| File | What it pins |
|---|---|
| `heif/srgb_8bit.heic` | 64x32 pattern (left red, right blue, top-left 8x8 green), nclx sRGB, lossless 4:4:4 |
| `heif/irot.heic` | same pattern, EXIF orientation 6 → `irot`; displays 32x64 |
| `heif/gradient_10bit.heic` | 10-bit red/blue ramp, 50 % green |
| `heif/p3_icc.heic` | saturated red, embedded Display P3 ICC (`colr prof`) |
| `heif/p3_nclx.heic` | saturated red, nclx primaries 12 / transfer 13, no ICC |
| `heif/pq_10bit.heic` | BT.2020 PQ: 203-nit white left, 1000-nit white right |
| `heif/iphone_like.heic` | lossy 4:2:0, Display P3 ICC, `irot` — the shape the OS (WIC) path may take |
| `avif/srgb_8bit.avif` | the pattern, CICP 1/13/1 full range, lossless |
| `avif/gradient_10bit.avif` | the pattern at 10 bits |
| `avif/irot.avif`, `avif/irot_imir.avif` | EXIF orientation 6 and 5 → `irot` / `irot`+`imir` |
| `avif/p3_icc.avif`, `avif/p3_nclx.avif` | Display P3 as ICC and as CICP 12/13 |
| `avif/anim.avif` | `avis`, 4 frames 32x16, durations 40/100/200/0 ms, loop=3 |
