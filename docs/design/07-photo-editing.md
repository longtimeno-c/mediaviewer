# Photo Editing

How still-image editing works: the EditStack, how it is evaluated for preview and export, RAW
handling, the export paths and the op set that exists.

The code is `src/edit/` (the stack, geometry, colour, bake, export, encoders, lossless JPEG) and
`src/shell/edit_session` / `src/shell/adjust_pane` (the host state both chromes drive). The
visible entry point is the Edit workspace ([20](20-edit-workspace.md)).

## Model: non-destructive op stack

The original file is never modified by an edit. An edit is an ordered list of small POD
parameter blocks (`edit/edit_stack.h`):

```cpp
struct edit_stack {
  std::uint64_t source_id;  // path hash + size + mtime of the original
  std::vector<op> ops;      // ordered; each op is a POD block, only its kind's field is meaningful
  int version;
};
```

| `op_kind` | Meaning |
|---|---|
| `rotate_cw`, `rotate_ccw`, `flip_h`, `flip_v` | A D4 turn of what is displayed |
| `crop` | Set the crop rect, normalised 0..1 in the straightened, uncropped frame |
| `straighten` | Set the angle, clockwise, ±45° (`kMaxStraighten`) |
| `resize` | Set the output size: long edge, exact, or percent |
| `adjust` | Set one colour parameter (`adjust_param`); `count` resets every colour parameter |
| `adjust_all` | Set every colour parameter at once (`values`), so an Auto or a section's reset is one undo step |
| `curve` | Set one channel's tone curve (`channel`, `values.curves[channel]`); a point drag is one op |
| `red_eye` | Add a red-eye fix at `spot` (source uv, radius a fraction of the diagonal); radius 0 clears them all |

Undo pops the list; reset clears it and returns the original exactly. `edit_session` keeps one
stack per (path, size, mtime) for the session (up to 64 stacks, oldest evicted), so walking away
and back keeps the edits. Stacks are **not persisted** across sessions.

Because the stack is small and its evaluation is a per-pixel kernel on the existing blit, every
edit previews at display rate and undo costs nothing.

## Evaluation

The stack folds into two canonical states:

- **Geometry** (`fold` → `geometry`): `source → D4 (rotate / flip) → straighten about the centre
  → crop → resize`. `place()` turns it into a `placement` against a source size: the oriented,
  cropped and output sizes, the total D4, and one affine **output uv → source uv** map. The
  canvas blit samples the source through that map, so rotate, flip, straighten and crop cost
  nothing per frame. `constrain_crop` shrinks a crop until no corner falls outside the rotated
  source; `auto_crop` is the largest frame-aspect rect that fits an angle.
- **Colour** (`fold_colour` → `colour`): the last value set for each of the sixteen
  `adjust_param`s, in four sections (`adjust_section`), each with its own Auto (but the vignette)
  and its own reset:

  | Section | Parameters |
  |---|---|
  | Light | exposure (±5 EV), highlights, shadows, whites, blacks, contrast |
  | Colour | saturation, vibrance, temperature, tint |
  | Black & White | intensity (0..100; 0 is off), filter (−100 blue .. +100 red), tone |
  | Vignette | strength (+ darkens), radius, softness |
  | Levels | black (0..100), mid-tones (−100..+100), white (−100..0), for the image, red, green and blue |
  | Curves | up to 8 points per channel (image, red, green, blue), no parameter |
  | Selective Colour | hue (±30°), saturation, luminance for red, orange, yellow, green, aqua, blue, purple, magenta |
  | Red-Eye | up to 8 fixes, each a click on a pupil, no parameter |
  | Detail | definition, brilliance (−100..+100), sharpen, noise reduction (0..100) |

  Every other parameter is −100..+100 and 0 is always the identity. Order: white balance
  (temperature, tint) and exposure as one per-channel gain → **tone** (highlights, shadows,
  whites, blacks) → contrast → saturation × vibrance → black & white → vignette → display
  encode. Tone is a curve on p = luma^(1/2.2): highlights p³(1−p) and shadows p(1−p)³ (peaks of
  0.105 at p = ¾ and ¼, monotone at ±100), whites p³ and blacks (1−p)³ move the ends; it is applied
  to RGB as a ratio, so hue is kept, and what a ratio cannot lift at black is added as grey.
  Vibrance scales saturation by 1 + v(1 − s), s the pixel's own saturation, so muted colours
  move most. Black & white mixes towards dot(c, w), w from Rec.709 towards a red (0.60, 0.35,
  0.05) or blue (0.10, 0.35, 0.55) filter (each sums to 1, so a grey keeps its value), with a
  contrast slope on that grey. The vignette is ±1.5 stops at full strength, a smoothstep from
  0.35 to 0.95 of the way to the corner (radius moves the start ±0.3, softness the width ±0.4),
  placed on the output frame (crop and straighten included). **Levels and curves** work in
  p = linear^(1/2.2): per channel, the image's levels then its curve, then the channel's own levels
  then curve. Levels map black (up to p 0.5) and white (down to p 0.5) to the ends with a mid-tone
  gamma of 2^(mid/100). A curve is a monotone cubic (Fritsch–Carlson) through its points, held
  flat past the ends; points are sorted, clamped and at least 1/64 apart (`sanitise`). Both are
  compiled on the CPU (`table_of`, microseconds) into one 128-row table (`gfx::adjust_table`),
  which the kernel reads with linear interpolation after saturation and before black & white
  (above p 1, a RAW's headroom, the end plus the overshoot). The table travels in the edit view;
  D3D11 holds it in its own constant buffer (`b1`) uploaded only when it changes, Metal copies it
  with the draw only while a table is on. **Selective colour** measures the pixel's hue on an
  opponent wheel (a = r − (g+b)/2, b = √3/2 (g − b); red 0, yellow ⅙, green ⅓, aqua ½, blue ⅔,
  magenta ⅚) and reads 64 more table rows, built by interpolating the eight bands between their
  centres: it rotates (a, b) by the hue shift, scales it by the saturation, and multiplies the pixel
  by 2^(luminance × its own saturation), so greys never move. It sits after saturation, before
  levels and curves. **Red-eye** fixes are the table's last 8 rows, placed through the geometry
  (`place_spots`: the canvas from the preview placement, the bake from its own, the histogram on
  the working frame); inside a spot, red above green and blue is pulled to their mean, fading over
  the outer 30 %. It runs first, on the gains. The table is 200 rows, 3.2 KB.

  **Detail** is not in the kernel: it needs neighbours. It is one CPU pass over the linear
  working image before the kernel (`edit/detail.h`, `apply_detail`), on perceptual lightness
  L = luma^(1/2.2), put back as a ratio (hue kept): noise reduction (L toward a range-weighted
  mean, weight 1 / (1 + (ΔL/σ)²), and each channel's ratio to luma toward its wide box mean),
  definition (L − a wide base, 1 % of the long edge, times a mid-tone weight), brilliance (lift what
  sits in a dark neighbourhood, hold back what sits in a bright one), sharpen (an unsharp mask,
  each step capped at 0.08 of L). Radii scale with the picture's long edge against
  `kWorkingPreviewEdge`, so the export (the same pass at full size, before the bake) looks like the
  preview. On the canvas the host runs it on a worker 150 ms after a detail slider settles, from
  the working image as built, and uploads the result as the working texture; the histogram and the
  Autos read that result. Measured (Apple M-series, 3072 × 2048, best of 3): sharpen or
  definition 32 ms, noise 87 ms, all four 96 ms.

  The **white-balance picker** (Colour) and the **red-eye tool** (Red-Eye) make the next click on
  the photo theirs (`adjust_pick`; the canvas does not pan): the click goes through the canvas map
  and the edit's map to the working image. The picker averages 5 × 5 working pixels there
  (`sample_mean`, memory already decoded, on the UI thread) and sets the temperature and tint that
  make them neutral (`neutral_balance`, ±100), then ends; the red-eye tool adds a fix at each click
  (radius 1.2 % of the diagonal times 2^(size/100)) until it is turned off. The chain is one kernel with six
  float4 uniforms (`adjust_uniforms` = `gfx::adjust_vectors`); each stage after saturation sits
  behind an `if` on its own uniforms, so an edit that sets only PR 11's sliders runs PR 11's
  arithmetic and the export bake pays for a stage only when it is set.

The kernel is written once (`gfx/adjust_kernel_tokens.h`) and compiled three ways: HLSL and MSL
in the blit's pixel shader, and C++ for export and the histogram (`gfx::adjust`). It takes the
pixel's offset from the output's centre, corner at distance 1 (`gfx::vignette_map` on the CPU;
`image_px` in the blits), for the vignette. On the canvas it runs over the FP16 working texture;
a slider drag re-uploads 96 bytes of uniforms and never decodes.

**Working image.** Colour edits evaluate on a linear FP16 Rec.709 working image
(`image/linear.h`, `linear_image`). The interactive preview uses a copy downsampled to
`kWorkingPreviewEdge` (3072 px long edge); the full-resolution chain runs once, on export. Until
the working image lands, the blit runs the same kernel over the 8-bit viewer texture, so an
adjusted photo is never shown unadjusted.

**Histogram and clipping** (`edit/histogram.h`): a reduction over the preview working image
*after* the kernel and display encode, so it shows what the canvas shows (the vignette is placed
on the working image's own, uncropped frame: on a cropped photo with a vignette the counts are
near, not exact). It runs on a worker
when the sliders settle, samples at most ~1 M pixels (`kHistogramSampleBudget`), and bins R, G,
B and Rec.709 luma. `pack_histogram` gives both chromes the same 64-bin view, scaled to the
tallest non-end bin. Clipping uses the viewer blinkies' thresholds (`kClipHighLinear` 0.9911,
`kClipLowLinear` 0.0003), so the pane's percentages count the pixels `C` blinks.

**Typed requests** (`edit/prompt.h`, [plan](../plans/edit-adjust.md) slice 5b): a field at the top
of the Adjust pane takes a sentence ("a bit brighter and warmer", "less noisy", "darker sky",
"like an old film", "fix the white balance") and moves the sliders it names, as one op. A fixed
vocabulary, no model: each word names a slider and a step (exposure 0.3 EV, contrast 20,
temperature 15, shadows 30, noise reduction 30…); "less", "too", "reduce", "deeper" turn it
around, "a bit" halves and "much" doubles it; an adjective before a region ("brighter shadows",
"darker corners") moves the region's slider; a few looks move several ("punchy", "dramatic",
"film", "matte", "soft"); "fix the white balance" and "auto" run the matching Autos one after
another. Moves add to where each slider is, land on its steps and clamp. Words it does not know are
listed back under the field ("not understood: …"), never guessed at.

**Auto** (`edit/auto_adjust.h`): each section's AUTO button runs `compute_auto` on a worker
from the preview working image and lands as one `adjust_all` op. Each returns absolute values for
its own section, so a second click gives what the first gave.

- **Light** is auto tone (below). It sets exposure and contrast and puts highlights, shadows,
  whites and blacks back to 0 (they sit between the gain and the slope, so the fit is exact only
  without them).
- **Colour** is the grey world over near-neutral pixels: of the pixels neither clipped (any
  channel ≥ 0.9) nor near black (luma ≤ 0.01), the least saturated quarter (so a frame of sea or
  leaves reads its light from the clouds, sand or stone in it), their mean linear RGB, and the
  temperature and tint that make it neutral (a 5-unit grid, then 1-unit about the best), taken
  70 % of the way (`kAutoColourDamping`, so a sunset stays warm) and less the more colourful
  those pixels still are (`neutral_confidence`: all of it at a mean saturation of 0.15 or less,
  none at 0.35, so a frame of sea and sky with nothing grey in it is left nearly alone), capped
  at ±60, under 3 snapping to 0. Saturation and vibrance are kept.
- **Black & White** turns it on at full intensity (an intensity already set is kept) with the
  filter, −100..+100 by 25, that gives grey^(1/2.2) the largest variance over the colour as it
  reaches black & white; within 2 % of the best, the filter nearest 0 wins. Tone is kept.
- **Levels** sets, per red, green and blue, the input black and white at 0.1 % from each end
  of that channel as it reaches the table (within ±40), the image's own row and the mid-tones
  at 0 (`kAutoLevelsClip`, `kAutoLevelsReach`).
- The vignette and curves have none.

Each reads at most 65 536 pixels (`kAutoSampleBudget`). `adjust_pane::request_auto_tone(section)`
gives the job a token that a colour change, a new item, a dropped working image or another Auto
makes stale; `adjust_view::auto_section` tells the chromes which Auto is running.

**Auto tone** (`edit/auto_tone.h`, #289, Preview's Auto Levels): exposure and contrast from the
histogram, a levels stretch fitted to the two tone controls the kernel has, a gain and a power
about 18 % grey:

- It reads a histogram computed with exposure and contrast at 0 (`auto_tone_basis`; temperature,
  tint and saturation as set) and returns absolute values, so a second click gives the same
  values as the first.
- Percentiles, interpolated inside the bin: black and white 0.1 % from each end
  (`kAutoToneClip`), the darkest and brightest of R, G and B, so a saturated highlight is not
  pushed into clipping; the luma median.
- Target: white to sRGB 250 (`kAutoToneWhiteCode`, under the blinkies' 254), black to 8
  (`kAutoToneBlackCode`), stretched in linear light, where an underexposure is a gain (pure
  exposure) and a haze is an added veil. A white already above 250 is never lowered (a clipped
  sky stays white) and a black already below 8 is left where it is, so a full-range photo is its
  own target and nothing changes.
- Exposure and contrast put the white percentile on its target and the median where the stretch
  puts it (two points, two unknowns, in log exposure about the pivot), with the black percentile
  never ending up above where it was.
- Caps, so a camera JPEG's own curve is only nudged: contrast 0 to +40 (slope ×1.18; auto tone
  never flattens), exposure ±2 EV. Results within 0.05 EV / 2 contrast of 0 snap to 0. Exposure is
  rounded to 0.01 EV, contrast to 1.
- The limit of the controls: a power about 18 % grey cannot subtract an offset. When a lifted
  (hazy) black needs more than +40 to come down, the black is held and white falls short of 250.
  The Blacks slider can now take it down by hand; auto tone does not use it yet.

`compute_auto_tone` is the histogram reduction plus the fit (a worker job, rule 1; the fit itself
is a few microseconds). Light's AUTO runs it through `compute_auto` above.

**Viewer clipping (`C`)** is a shader on the display blit, not an edit op; with colour edits on,
it blinks what the edit clips.

## RAW: wait for LibRaw's full decode

The Adjust pane's sliders stay disabled ("Preparing the full-resolution image…") until the FP16
working image exists (`adjust_pane`: `none` / `preparing` / `ready` / `failed`). For a RAW that
is LibRaw's full 16-bit linear develop (`codec::decode_raw_linear` → `linear_from_raster16`),
never the embedded preview. The embedded preview still shows instantly for viewing
([04](04-image-pipeline.md)). Editing on the real linear data means the preview is the export and
highlight headroom is present. Every working-image request carries a token; a result for an item
the user has left is dropped.

Demosaic, the camera matrix and as-shot white balance are LibRaw's (CPU).

## Export

`edit/export.h` bakes the stack into a **new** file; the original is never the target. The
host reads the source bytes and writes the result through `io::write_new`, with the name from
`export_file_name` (`IMG_0001.HEIC` → `IMG_0001-edit.jpg`) run through `io::unique_name`, so an
existing file is never overwritten either. Worker thread only.

Three paths, reported back so the chrome can say which ran:

| Path | When | How |
|---|---|---|
| Lossless | JPEG in, JPEG out, only rotate / flip / an MCU-aligned crop (`prefer_lossless`, on by default) | DCT coefficients rearranged (`lossless_jpeg.h`) |
| Re-encode | Geometry that resamples (straighten, resize, a non-aligned crop), or a non-JPEG source | Decode, CPU geometry (`edit/geometry.h`), encode |
| Bake | Any colour adjust | FP16 working image at full resolution (for a RAW, LibRaw's linear develop), geometry, the C++ kernel (`edit/bake.h`), sRGB-encoded RGBA8. The bake and the preview differ by 8-bit rounding only |

Output is always upright: EXIF Orientation 1, EXIF pixel dimensions patched, `tiff:Orientation`
1 in XMP.

**Options** (`export_options`, the export sheet on both hosts):

- Format: JPEG (libjpeg-turbo; quality 1–100, default 92; 4:2:0, 4:2:2 or 4:4:4) or PNG
  (libspng; RGB when opaque, else RGBA). The sheet offers quality 100/95/92/85/75/60.
- Size: the long edge (full, 3840, 2560, 2048, 1600, 1080 px), applied on top of the stack.
- Metadata policy (`metadata_policy.h`): `all` (EXIF incl. maker notes, XMP, IPTC, comments),
  `minus_gps` (GPS IFD zeroed, XMP packets naming `exif:GPS*` dropped), `none`. The ICC profile
  is kept under every policy. JPEG writes EXIF/XMP as APP1 (a block too large for one segment is
  left out); PNG writes `eXIf`, iTXt `XML:com.adobe.xmp` and `iCCP`. Metadata of HEIC, TIFF,
  RAW and WebP sources is read by `meta::read_carried` in the host and passed in.

`Ctrl+Alt+C` / `Cmd+Opt+C` (`copy_flattened`) copies the current still with its edits baked, as
a PNG.

**Print** (`Ctrl+P` / `⌘P`, issue #614, [16 "Print"](16-commands.md#print)) takes the same
pixels before the encode: `edit::render_picture` is the export's re-encode or bake path (the
bake's sRGB, or the decoded source through the geometry with its ICC kept), resampled down to a
box for the paper (`shrink_to_box`, never up). The lossless JPEG path is not taken: a print needs
pixels, and for a crop or turn the decode-and-place is the same picture. Nothing is written.

### Lossless JPEG

`lossless_jpeg.h` is jpegtran's transform on the plain libjpeg coefficient API. Only perfect
transforms run: an edge that would move a partial MCU into the image refuses
(`unsupported_format`) and the caller re-encodes or writes the Orientation tag. Camera frames are
MCU-aligned, so the fallback is for odd-sized files. The file's own EXIF orientation is baked in
and the tag written as 1; pixel dimensions are patched and a stale EXIF thumbnail is unlinked.

**From the viewer** (`[` `]` `H` `V` on a JPEG whose stack holds only rotate/flip):
`rotate_in_viewer` rewrites the file losslessly **in place** (`io::replace_atomic`). The preview
turns at once; the write follows after the host's debounce, one at a time, and turns pressed
while a write is in flight are carried to the rewritten file. When the frame is not MCU-aligned,
only the Orientation tag is rewritten. Pixels are not touched either way.

## Editing scope line

What exists is the first editing set:

| Built | |
|---|---|
| Geometry | Rotate, flip, crop (aspect presets, [20](20-edit-workspace.md)), straighten ±45°, resize |
| Colour | Light (exposure, highlights, shadows, whites, blacks, contrast), Colour (saturation, vibrance, temperature, tint), Black & White (intensity, filter, tone), Vignette (strength, radius, softness), Levels (per channel), Curves (per channel, a point editor), Selective Colour (eight bands), Red-Eye (clicks), Detail (definition, brilliance, sharpen, noise reduction); a white-balance picker; an Auto and a reset per section |
| Readouts | Histogram (R, G, B, luma) and clipping percentages |
| Lossless | JPEG rotate / flip / MCU-aligned crop, from the viewer (`[` `]` `H` `V`) or on export |
| Export | JPEG / PNG, long-edge resize, metadata policy |

Further ops slot into the same structure: one more parameter block in `op`, one more term in the
kernel or one more pass, evaluated in the order above.

## Not built

- Persisting a stack across sessions (XMP sidecar, SQLite catalog index).
- Redo and a named history list for stills (`edit_stack::undo` only pops).
- Perspective / keystone, lens correction; colour grading; automatic red-eye (it needs the
  faces of the AI pack; [plan](../plans/edit-adjust.md) slice 6 reads them).
- Dehaze, grain, split-tone; detail on the GPU (the pass is on the CPU, so the canvas follows a
  detail slider when it settles, not during the drag).
- Local adjustments: gradients, brush masks, healing / clone.
- Auto-straighten, auto-crop. An Auto key outside the pane (the pane's `A` runs the focused
  slider's section).
- GPU demosaic, highlight recovery, lens profiles, dual-illuminant white balance.
- A compute-shader ping-pong chain and a cache of pipeline state after early stages (the single
  kernel on the blit does not need them).
- Batch export with a filename template; export to other formats, bit depths, ICC conversion,
  output sharpening or watermark.
- Focus peaking, zebras, channel isolation.
