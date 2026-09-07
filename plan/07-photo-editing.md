# 07 — Photo Editing

## Model: non-destructive op stack, evaluated on the GPU

The original file is never modified. An edit is an ordered list of parameterized ops:

```cpp
struct EditStack {
  Guid source_id;            // content hash of the original
  std::vector<Op> ops;       // ordered; each is a small POD parameter block
  int  version;
};
```

Persist to an **XMP sidecar** (interoperable) or a local SQLite catalog (fast, survives file
moves) — do both: sidecar is truth, catalog is index.

Because the stack is small and evaluation is a shader chain, **every edit previews at display rate
with zero latency** and undo is just popping the list. This is the architecture Lightroom and
Capture One use, and the *structure* is simpler than a destructive pixel-buffer + history-stack
design.

**The structure is simpler. The kernels are not.** Lightroom's value is in the several hundred
engineer-years inside its ops, not in its op list. D4 already cut those from v1 — do not let this
paragraph talk you back into writing them.

## Evaluation

Fixed pipeline order (order matters for correctness — matching Lightroom's is a reasonable
default):

```
decode → linearize (ICC → linear scRGB fp16)
  → [geometry] crop, straighten, rotate, flip, perspective/keystone, lens correction
  → [white balance] temperature, tint  (in camera-native space for RAW)
  → [exposure] exposure, contrast, highlights, shadows, whites, blacks
  → [tone curve] RGB + per-channel, spline
  → [color] vibrance, saturation, HSL per-band (8 bands), color grading (shadows/mid/highlight)
  → [detail] sharpen (unsharp / Richardson–Lucy deconv), noise reduction (luma + chroma, NLM or guided)
  → [local] radial/linear gradients, brush masks, healing/clone
  → [effects] vignette, grain, dehaze, split-tone
  → tone-map + encode to display space
```

Implementation notes:

- Each op is one compute shader; chain them ping-pong through two FP16 render targets. At
  1:1 on a 45 MP image that's ~15 passes — under 8 ms on a mid GPU. For the *interactive* preview,
  evaluate at viewport resolution only (a 3000×2000 preview), and run the full-res chain once on
  export. This is the single biggest perf decision in the editor.
- Keep a **cache of the pipeline state after the expensive stages** (geometry, WB, exposure) so
  dragging a late slider like vignette doesn't re-run demosaic.
- Histogram + clipping warnings from a compute reduction each frame — cheap, and users expect it.

## RAW in v1 — pick one, and say so

The interactive-edit promise ("slider drag → updated preview within one refresh interval on a
45 MP RAW") is only true **once a full linear decode exists in VRAM**. GPU demosaic is v1.1, so v1
has to choose:

| | **Edit the embedded preview** | **Wait for LibRaw's CPU decode** ✅ |
|---|---|---|
| Time to first *adjustable* frame | ~50 ms | **200–600 ms** |
| Pixels you're editing | The camera's baked JPEG — **not** what exports | The real linear data |
| Export match | **Preview and export disagree** | Preview is the export |
| Highlight/shadow headroom | Gone, clipped in the JPEG | Present |

**Call: wait for the full LibRaw decode before enabling the adjust pane.** The embedded preview
still shows instantly for *viewing* ([04-image-pipeline.md](04-image-pipeline.md)) — that path is
unchanged and is what makes browsing feel fast. But the sliders stay disabled, with a brief
"preparing" state, until the real data is there.

Editing pixels that aren't the ones you export is the kind of wrong that erodes trust in every
other number the app shows. Half a second, once, on entering the adjust pane, is a fair price.

This is on PR 10's verify line.

## RAW specifics

LibRaw gives you the Bayer/X-Trans mosaic. Demosaic on the GPU (AHD or Menon for Bayer; X-Trans
needs Markesteijn — port from LibRaw's CPU version or use LibRaw's and accept the cost, cached).
Apply the camera color matrix and the as-shot WB coefficients before anything else. Highlight
recovery from the per-channel saturation points.

## Tools that need CPU or special handling

- **Healing / clone / content-aware fill** — patch-match is iterative; run it on a worker at
  reduced res for preview, full res on commit. Store as a mask + offset op in the stack.
- **Red-eye** — face/eye detect (bundle a small ONNX model via DirectML, or a classic Haar
  cascade) then a desaturate-and-darken op.
- **Straighten by auto-detected horizon**, **auto-crop**, **auto-tone** — all optional, all
  computed once and written into the stack as normal parameter values so they stay editable.

## Export

- Target format, quality, chroma subsampling, bit depth, ICC profile to embed, resize
  (long-edge / exact / percentage), sharpening-for-output, metadata policy (all / minus GPS /
  none), watermark.
- Batch export a selection with a filename template, running on the decode pool with a progress UI
  that is cancellable.
- **Lossless JPEG operations** for the common cases: rotate, flip, and crop-to-MCU-boundary via
  `jpegtran`-style transforms — no re-encode, no generation loss. Offer this whenever the requested
  edit stack contains only those ops.

---

## v1 scope line (per D4 in [01-decisions.md](01-decisions.md))

The architecture above is what you build. The **op set** ships in two waves:

| **v1 (PR 9–10)** | **v1.1 — same stack, more kernels** |
|---|---|
| Rotate, flip, crop, straighten, resize | Perspective/keystone, lens correction |
| Exposure, contrast, saturation, temperature/tint | Highlights/shadows/whites/blacks, vibrance |
| Histogram + clipping warnings | Tone curve (RGB + per-channel), per-band HSL, colour grading |
| Lossless JPEG rotate / MCU-aligned crop, invokable from the viewer (`[` `]`) without opening the adjust pane | Sharpen, noise reduction, dehaze, vignette, grain |
| Export with resize + metadata policy | Local adjustments: gradients, brush masks, healing/clone, red-eye |
| | Full RAW develop: highlight recovery, lens profiles, dual-illuminant WB |

**Viewer clipping (`C`)** is a shader on the display blit, not an edit op. PR 6 may blink
display-referred luminance; **accurate RAW clip waits for the full LibRaw decode**, same
rule as the adjust pane. Focus peaking, zebras, and channel isolation are v1.1.

Nothing in the v1.1 column requires a design change — each is one more compute shader appended to
the fixed pipeline order and one more parameter block in the stack. **What must be right in v1 is
the foundation**: a linear FP16 *working space* (the swapchain stays 8-bit sRGB until HDR lands —
see D6), ICC handling, viewport-resolution preview with full-res only on export, and the cache of
pipeline state after the expensive early stages.
Retrofitting any of those is a rewrite; adding sliders is an afternoon.
