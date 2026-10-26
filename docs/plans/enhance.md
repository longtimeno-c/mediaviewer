# Enhance (#297)

A restored copy of the photo, clip or audio file on screen, from a model downloaded only when the
person asks for it. The issue is the spec; this page says how it is being built and what is left.
Owner calls: issue #297 (2026-10-10). Decision log: "Enhance is its own add-on" (2026-10-26).

## Shape

- **One slot add-on**, `src/addons/enhance`, no chrome of its own. The hosts render its two
  screens from view trees: its page in Settings → Add-ons (place `settings`) and the sheet over
  the viewer (place `sheet`), opened by `Enhance…` (palette, context menu; no default key).
- **Models per kind.** `models/<kind>/model.json` ships in the zip (name, credit, weights,
  `run_bytes`, `cpu`, knobs with the kinds they apply to, an `io` section for the runner). The
  weights are pins under `remote` in the signed manifest, fetched into the add-on's data folder
  by `mv_model_fetch` (#482 T3) on the person's click for that kind.
- **The sheet follows the item**: a still → the photo model's knobs; an audio file → the audio
  model's; a clip → picture, sound or both, each from its own model. A kind with no installed
  model is named, not offered. Values are remembered per kind.
- **A run** is a background job on the add-on's own thread, never the playback decoder, the
  render device or the present loop; Cancel, or closing the sheet (leaving the item), stops
  it. The result is a new file beside the original (`<name>-enhance.<ext>`,
  `io::unique_name`'s numbering), written to a hidden part file and renamed. The original is
  never written (rule 5).

## Slices (each on both hosts; the present-loop gates and launch → first pixel unchanged)

1. **Weights outside the zip** — built 2026-10-26.
2. **The sheet** — built 2026-10-26.
3. **A still** — not started (not asked for in this round).
4. **Audio** — built 2026-10-26: DeepFilterNet3, the host's sound rows and `edit/sound_writer.h`
   ([29](../design/29-enhance.md)).
5. **A clip's picture** — next. The candidate is Real-ESRGAN general x4v3 (BSD-3-Clause, 4.9 MB,
   `jonathanst29/tinier-upscale-models@899dc1e4`, SHA-256 `924ebad6…d8d5`; input float32 NCHW
   RGB in [0, 1] with dynamic size, output 4×), downloaded for measurement, not pinned. Needs every frame in display order (the sampler gives
   keyframes and gaps) and a frame sink into the labelled hardware re-encode path
   (`edit/hwencode.h`), in the app process rather than the clip helper, since the frames come
   from the add-on. The first video model is chosen like the audio one.

## Not in this plan

Everything #297 lists under "Not in this issue": loading Enhance at launch, weights in git or a
release asset, running on the view, a live preview on the playback clock.
