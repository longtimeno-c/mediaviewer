# The full Adjust pane, Enhance in Edit, and eyes open

A plan for three asks from the owner (2026-11-02): the still Edit workspace should offer the
whole Photos-style **Adjust** list as standard; the **Enhance** add-on should sit in that
workspace and take a typed request ("brighter, less noisy"); and a face with **closed eyes**
should be fixable from the person's other pictures. Nothing here is built.

What exists today ([07](../design/07-photo-editing.md), [20](../design/20-edit-workspace.md),
[29](../design/29-enhance.md)): geometry (rotate, flip, crop, straighten, resize) and five colour
parameters (exposure, contrast, saturation, temperature, tint) in one per-pixel kernel on the
blit (`gfx/adjust_kernel.h`, HLSL + MSL + C++); a histogram; auto tone in the core with its button
owed; Enhance as a separate sheet whose still model is Real-ESRGAN, which does not see the edit
stack.

## Owner calls before code

1. **Face data for edits.** [17](../design/17-local-ai-search.md) "People" promises *"Face data is
   used for nothing but local search."* Slice 6 reads `faces.db` to pick a donor picture. That is
   a new purpose: it needs the owner's yes, a dated row in
   [12](../design/12-decision-log.md), and the People opt-in text changed to say so. Without the
   yes, slice 6 runs on the burst alone (no identity: same position in neighbouring frames), which
   is weaker but uses no face data.
2. **A generative model in Enhance** (slice 5b): which weights, under which licence. Most
   instruction-editing and identity-preserving face models are non-commercial or OpenRAIL-M with use
   restrictions; [11](../design/11-licensing.md) has no rule for model weights in a first-party
   add-on yet.
3. **Persisting the stack** (XMP sidecar) stays out of this plan; with many more sliders it will
   be asked for. Separate call.

## Slices (each on both hosts, HLSL + MSL twins, GLSL kept hostable)

Every slice keeps the PR 1 present-loop gates on both platforms, launch → first pixel and idle
cost unchanged, and is measured per the rule in CLAUDE.md. A per-pixel term costs nothing while
no edit is active (the identity view skips the kernel); a slice that adds a pass reports the
pass's time at `kWorkingPreviewEdge` (3072 px) on both machines.

### 1. Light and colour, per-pixel (no new pass) — built 2026-11-02

New `adjust_param`s in the existing kernel: **highlights, shadows, whites, blacks, vibrance**,
**black & white** (intensity, filter, tone), **vignette** (strength, radius, softness). Each
section has an **Auto** but the vignette: Light is `auto_tone` (its owed button), Colour is a
grey-world white balance, Black & White picks the filter that spreads the grey most. Each lands
as one `adjust_all` undo step. How they work: [07](../design/07-photo-editing.md).

Pane: sections collapsible like the screenshot, each with reset and Auto, on both hosts. A
section's filmstrip of presets is not in this slice; Colour's Auto sets white balance only (no
vibrance guess).

Verify: the C++ kernel and the shader agree within 1/255 on the golden set for every new
parameter at its ends and middle; a slider drag re-uploads uniforms only (no decode, no working
image rebuild); PR 1 gates hold.

Status (2026-11-02, Apple M-series, Release):

- The shared half's verify holds: `mv_tests` `[adjust]` and `[auto_tone]` (the kernel against a
  written-out double-precision chain within one 8-bit code, every new slider's direction and
  monotone tone curves, each section's Auto, the export bake's vignette); the full suite passes.
- Mac: built and run. `MV_EDIT_SELFTEST` now also sets highlights, shadows, vignette and black &
  white, runs Colour's Auto and resets Black & White, and writes the values to `state.txt`; the
  saved copy carries them. Mac PR 1 gate: animated 60 s, 0 dropped, p99 16.8 ms; static 60 s,
  0 presents, 0.26 % CPU.
- Export bake, 24 MP, PR 11's sliders only (`mv_tests "[adjust-bench]"`, four alternated rounds
  against the base commit): base 312.6–315.2 ms, new 331.1–342.8 ms, about +6 % (the larger
  kernel's branches; forcing it inline and skipping the vignette's position when off took it
  down from +18 %). Every stage on: about 1.2 s. Export is not one of the gated metrics; the
  canvas cost of the new stages is not measured (the harness cannot set an edit during a soak).
- Windows: the C# compiles (warnings are errors); the HLSL is the same tokens through fxc, and
  `main.cpp`'s new handlers are compiled by CI only. The Windows pane, its PR 1 gate and
  Narrator / VoiceOver on the new controls are owed.

### 2. Curves and levels (a LUT, no new pass) — built 2026-11-02

One 1D texture, 256 × 4 (R, G, B, master), sampled in the kernel after contrast. **Levels**
(black, grey, white in and out per channel) and **Curves** (points per channel, a monotone cubic)
both compile to that LUT on the UI thread (microseconds). The C++ export samples the same table.
Curve and levels points are new op kinds; the op block stays POD (fixed max points).

The pane's histogram sits under the curve. Auto levels takes percentiles per channel (as built:
of the colour as it reaches the table, not `auto_tone`'s histogram).

As built: 128 rows of float4 rather than 256 × 4 (one row holds red, green and blue, read with
linear interpolation); the table sits after saturation, before black & white. Curve points cross
the island as parked text (`chrome_cmd_adjust_curve`, 1090); Levels are twelve appended island
commands (197–208). Status: `mv_tests` `[curves]`, `[auto_levels]` and the full suite pass; the
Mac self-test sets an S curve and runs Levels' Auto, and the saved copy carries both; the C#
compiles; Windows native is CI's. Mac PR 1 gate: see the PR.

### 3. White balance picker and selective colour

- **White balance:** neutral grey, skin tone and temperature/tint modes; an eyedropper on the
  canvas (click → the host reads one averaged 5 × 5 sample from the preview working image on a
  worker, never a GPU readback on the render thread) that solves temperature and tint.
- **Selective colour:** hue, saturation, luminance for eight hue bands, smooth band weights in the
  kernel (no 3D LUT).

### 4. Detail: definition, brilliance, sharpen, noise reduction (one cached pass)

These are local, so they need neighbours. One **base layer** (an edge-aware blur of luma at a
quarter of the preview size) built on a worker when the detail sliders change, uploaded as an
immutable texture, and read by the kernel: definition and brilliance are a gain on detail = luma −
base; sharpen is an unsharp mask from the same pyramid. **Noise reduction** is a separate
bilateral / non-local means pass on a worker, at preview size while dragging and full size on
export; its result replaces the working image for the kernel. The render thread never runs the
pass. Budget: the pass's time is reported; the drag stays at display rate because the kernel only
samples the last finished layer.

### 5. Enhance inside Edit

**5a. The Enhance tab and the edit stack.** When the add-on is installed, the workspace offers an
**Enhance** tab that hosts the add-on's `sheet` view tree in the pane ([20](../design/20-edit-workspace.md)
"Editor add-on panes docking as extra tabs" is this). The host hands the folded edit stack
(geometry + colour, POD) to the add-on through a new host-table row, closing the gap in
[29](../design/29-enhance.md) "Not built". The enhanced copy then carries the crop and colour.

**5b. Type what you want.** One text field at the top of the Adjust pane, two layers:

- **Words → sliders (first, in the core).** A small deterministic parser in the style of the voice
  query parser ([19](../design/19-voice.md)): "warmer", "brighter", "less noisy", "black and white",
  "more contrast but keep the sky", "like a film photo" → slider deltas, shown as the sliders
  moving, one undo step. No model, no download, instant, offline, every result editable by hand.
  This covers most of what people type.
- **Words → a generated edit (later, in Enhance, opt-in download).** For what sliders cannot do
  ("remove the person behind her", "make the sky blue"): an instruction-following image model.
  Cost, honestly: 1–4 GB of weights; on Apple silicon (Core ML) about 5–20 s for a 512–1024 px
  pass; on Windows the bundled ONNX Runtime is CPU only, so minutes. It writes a new file like
  every Enhance run. Owner call 2 first, and a spike measuring a candidate on both machines before
  it is planned in detail.

### 6. Eyes open from the same moment ("Best take")

**Recommended approach — real pixels from a neighbouring frame, not a generated face.**

1. On an explicit click (Adjust → Faces → **Open eyes**), the host finds faces in the photo on
   screen (YuNet, already in the AI pack; 5 landmarks).
2. An **eye-state score** per face. YuNet's landmarks are eye centres, not lids, so this needs a
   small extra model: a dense-landmark model (eye aspect ratio) or an eye-open classifier on eye
   crops, each a few MB and a few ms a face on CPU. Licence-clean weights are part of the spike.
3. **Donors:** pictures within ±10 s in the same folder (a burst), where the same person
   (`faces.db` person id, owner call 1; else the face nearest the same position) has eyes open and a
   similar head pose (landmark geometry).
4. **Transfer:** align the donor's eye region to the target with a similarity transform from the
   landmarks, refined by a small local alignment on the eye patch; match colour; blend with a
   multiband (Laplacian) blend on a feathered mask. The person picks between donors when there are
   several.
5. The result is an edit (a new op kind holding the donor's content id and the patch transform)
   or, simpler first, a new file like Enhance's.

Compute: one decode of each candidate donor (they are usually in the prefetch window or the thumb
cache already, but a full-size decode is needed for the patch), a few ms of detection and scoring
per face, and well under a second of alignment and blending on CPU. **Not too much compute.**
Faces at 24 px are refused (nothing to transfer).

**Not recommended — generating open eyes from the person's identity** (an inpainting diffusion
model conditioned on their face embeddings): gigabytes of weights, seconds on Core ML and minutes
on Windows CPU, a real risk of eyes that are not theirs, and the identity-adapter models in use are
non-commercially licensed. It also needs owner call 1 *and* stores nothing new but uses face data
far more deeply. If the burst approach fails often in practice, revisit with numbers.

Where it lives: the AI pack has the detector and `faces.db`; the edit core does not depend on
`infer/`. So slice 6 is an AI-pack feature reached through the host table (like People's
`search_this_person`), and the core sees only the op it lands. Red-eye's **auto** mode uses the
same detector; its **manual** brush (click a pupil, desaturate the red disc) is classical and goes
in slice 3 in the core.

## Order

1 → 2 → 3 → 4 are independent of the owner calls and each ships alone. 5a can run beside them.
5b's parser after slice 2 (it needs the sliders it drives). 6 after owner call 1 and the eye-state
spike. 5b's generative layer after owner call 2 and its spike.

## Not in this plan

Masks, brushes, gradients, healing and clone (a local-adjustments plan of its own); presets and
filters; persisting the stack; HDR output; redo and history (owed in [07](../design/07-photo-editing.md)).
