# The Editor add-on

The optional advanced video editor (colour grading, multi-track editing, audio, delivery, model
packs) that would dock into the Video Editor window. **None of it is built**; this page records
what exists toward it and what does not.

## What exists

- **The base Video Editor** it would extend: one clip, cuts, keyframe and exact export
  ([21](21-video-editor.md)).
- **The encoder probe** (`tools/encprobe`, spike S1): an opt-in tool that reports which hardware
  video encoders (including AV1 and ProRes), OS audio encoders and LGPL FFmpeg filters a machine's
  build has, and how fast the encoders run. Its Mac results are in
  [21](21-video-editor.md) "S1 — encoder probe".
- **The add-on mechanism** it would ship through: signed add-ons under `src/addons/<name>`
  reaching the core only through the host function table (`mediaviewer_addon.h`)
  ([18](18-import.md)). The table's current version adds pixel access for the AI add-on; it has no
  GPU port.

## Not built

- `src/addons/editor` (`mv_editor`), its chrome (`MediaViewer.Editor.Chrome`, `Editor.bundle`) and
  the `MediaViewerRender` / `MediaViewerModel` helper processes.
- A GPU port in the host function table (opaque textures, buffers, kernels, command lists, fences,
  `preview_present`), and decode, audio, text, file/cache and job services for add-ons.
- A render graph, frame cache, render cache and preview-resolution scheduler.
- The `.mvproj` project format, multi-track timeline (ripple / roll / slip / slide, compound
  clips, keyframes, speed ramps, transitions, titles, captions, stabilisation, proxies).
- Colour management (input / output transforms, log and scene-linear working spaces, OCIO), the
  grading tools (wheels, curves, qualifiers, power windows, tracking, node graph, LUTs, shot match)
  and scopes (waveform, parade, vectorscope, CIE).
- HDR grading and HDR export.
- The audio engine (busses, EQ, dynamics, noise reduction, loudness, ducking) and voice isolation.
- Delivery presets, AV1 / ProRes export, the render queue.
- Model packs (voice isolation, captions, masks, stems).
- Spikes S2–S8.
