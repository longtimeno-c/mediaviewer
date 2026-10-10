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
- **Colour** (`fold_colour` → `colour`): the last value set for each of exposure (±5 EV),
  contrast, saturation, temperature and tint (−100..+100). Order is white balance (temperature,
  tint) → exposure → contrast → saturation → display encode. White balance and exposure are one
  per-channel gain, so the chain is one small kernel with eight uniforms
  (`adjust_uniforms`: RGB gains, contrast slope, saturation, pivot 0.18).

The kernel is written once (`gfx/adjust_kernel.h`) and compiled three ways: HLSL and MSL in the
blit's pixel shader, and C++ for export. On the canvas it runs over the FP16 working texture; a
slider drag re-uploads 32 bytes of uniforms and never decodes.

**Working image.** Colour edits evaluate on a linear FP16 Rec.709 working image
(`image/linear.h`, `linear_image`). The interactive preview uses a copy downsampled to
`kWorkingPreviewEdge` (3072 px long edge); the full-resolution chain runs once, on export. Until
the working image lands, the blit runs the same kernel over the 8-bit viewer texture, so an
adjusted photo is never shown unadjusted.

**Histogram and clipping** (`edit/histogram.h`): a reduction over the preview working image
*after* the kernel and display encode, so it shows what the canvas shows. It runs on a worker
when the sliders settle, samples at most ~1 M pixels (`kHistogramSampleBudget`), and bins R, G,
B and Rec.709 luma. `pack_histogram` gives both chromes the same 64-bin view, scaled to the
tallest non-end bin. Clipping uses the viewer blinkies' thresholds (`kClipHighLinear` 0.9911,
`kClipLowLinear` 0.0003), so the pane's percentages count the pixels `C` blinks.

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
existing file is never overwritten either. Worker thread only. An unsaved item from New from
Clipboard ([16](16-commands.md)) has no folder: Save Copy asks for one and writes
`Untitled.jpg` there (`shell::run_export_to`).

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
| Colour | Exposure, contrast, saturation, temperature, tint |
| Readouts | Histogram (R, G, B, luma) and clipping percentages |
| Lossless | JPEG rotate / flip / MCU-aligned crop, from the viewer (`[` `]` `H` `V`) or on export |
| Export | JPEG / PNG, long-edge resize, metadata policy |

Further ops slot into the same structure: one more parameter block in `op`, one more term in the
kernel or one more pass, evaluated in the order above.

## Not built

- Persisting a stack across sessions (XMP sidecar, SQLite catalog index).
- Redo and a named history list for stills (`edit_stack::undo` only pops).
- Perspective / keystone, lens correction; highlights / shadows / whites / blacks, vibrance; tone
  curve; per-band HSL; colour grading.
- Sharpen, noise reduction, dehaze, vignette, grain, split-tone.
- Local adjustments: gradients, brush masks, healing / clone, red-eye.
- Auto-straighten, auto-crop, auto-tone.
- GPU demosaic, highlight recovery, lens profiles, dual-illuminant white balance.
- A compute-shader ping-pong chain and a cache of pipeline state after early stages (the single
  kernel on the blit does not need them).
- Batch export with a filename template; export to other formats, bit depths, ICC conversion,
  output sharpening or watermark.
- Focus peaking, zebras, channel isolation.
