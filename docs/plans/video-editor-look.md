# The Video Editor's look: turn, crop, colour, sound, speed, save frame

The Video Editor ([21](../design/21-video-editor.md)) cuts one clip. The owner asked (2026-11-03)
for the edits a photo gets in Edit image to reach Edit video too: **rotate, flip and crop**,
**light and colour**, **sound (mute, volume, fades)**, **save the frame on screen as a photo**
and **speed (0.5× / 2×)**. This page is how; what lands is folded into 21 and 08.

It stays the base editor: one clip, one picture track and its sound, every setting for the whole
edit. Grading tools (wheels, curves, qualifiers, LUTs, scopes), an audio engine, keyframed
settings and multi-track stay the Editor add-on's ([22](../design/22-editor-addon.md)). The
decision log records the line.

## Shape

- **One look per edit.** `edit::clip::look` (shared, POD): quarter turns (0–3, clockwise, on top
  of the clip's own display rotation), flip horizontal / vertical, a crop (a ratio: original,
  16:9, 9:16, 1:1, 4:3, 4:5; centred, on the turned picture), PR 11's colour (exposure,
  contrast, saturation, temperature, tint), mute, volume (0–200 %), fade in / fade out
  (0–5 s each, on the program), speed (0.5×, 1×, 2×). `video_timeline` keeps it beside the
  pieces: one history, so `⌘Z` undoes a turn like a cut; a slider drag is one step.
- **Preview** is the canvas and the player, nothing new on the present path:
  - turn / flip / crop in the video blit's uv map (HLSL + MSL twins), and the camera fits the
    cropped, turned size;
  - colour through PR 11's shared kernel in the video blit (the same tokens as the photo blit
    and the export bake);
  - sound as a gain envelope on decoded audio blocks (`player/audio_decode`), keyed on source
    time from the pieces, so fades land on the program's start and end;
  - speed as the player's rate (already pitch-corrected).
- **Export exact** with a look runs in `MediaViewerClipJob` like every encode: `frame_reader` →
  the kept frames, retimed for the pieces and the speed → turn / flip / crop and colour in C++
  (colour through a 33³ table built from the kernel once per job) → `picture_writer` on the
  hardware encoder; the sound through `audio_reader` → kept and retimed → gain, fades, tempo →
  the OS's AAC (else FLAC in Matroska). A look makes the output 8-bit BT.709 SDR (labelled), as
  Enhance's clips are. Without a look, Export exact is unchanged.
- **Export (keyframes)** still copies packets. It carries a turn (display matrix) and mute (no
  sound stream). Anything else in the look needs Export exact: Export is greyed with that reason.
- **Save frame** (`⌘⇧S` in the editor): the frame at the playhead as a JPEG beside the clip
  (`op::frame`), with the turn, flip, crop and colour applied.
- **ABI 0.36:** `mv_clip_request` gains a `look` pointer (`mv_clip_look`, its own
  `struct_size`); the helper wire gains an optional `"look"` object.

## Slices (each on both hosts; both PR 1 gates and launch → first pixel unchanged)

1. **The look, turn / flip / crop, Save frame** — built 2026-11-03 (Mac verified by tests; the
   Windows native half and both apps' clicks owed). `look` in the timeline and its history; the
   video blit's uv map and fit; the helper's render op (picture side; the sound copied when the
   look leaves it alone, else re-encoded through the cut); keyframe Export with a turn; Save
   frame. Editor bar: ⟲ ⟳, Flip, Crop menu, Save frame.
   *Verify:* a 1080p landscape clip cropped 9:16 previews cropped on both hosts and Export exact
   writes 608 × 1080 at the clip's rate (turned once as well, 1080 × 1920), frame N matching the
   preview's frame N within 2 8-bit codes on a flat patch; keyframe Export with only a turn writes
   no re-encode and a display matrix of 90°.
2. **Light and colour** — built 2026-11-03 (Mac verified by tests; the Windows native half, both
   apps' clicks and the 4K playback soak owed). Exposure, contrast, saturation, temperature, tint and Auto (#289's auto
   tone on the frame at the playhead) in a Colour popover; the kernel in the video blit; the
   table in the render op; Save frame applies it.
   *Verify:* the exported frame and the preview agree within 2 codes at five patches for each
   slider at ±50; playback of a 4K HEVC clip with a colour look holds the clip's rate with 0
   dropped presents (`--soak` on video), measured against no look.
3. **Sound: mute, volume, fades** — built 2026-11-03 (Mac verified by tests; the Windows native
   half, both apps' clicks and listening owed). A Sound popover; the envelope in `audio_decode`; the render
   op's gain and fades; keyframe Export with mute only.
   *Verify:* exported RMS of a 1 kHz tone at 50 % is −6 dB ± 0.2; a 1 s fade-out's last 10 ms are
   below −40 dB; the preview's envelope matches (unit test on the envelope, by ear in the app).
4. **Speed.** 0.5× / 1× / 2× in the bar; preview at that rate; the render op retimes frames
   (2×: every frame at half its duration; 0.5×: double) and the sound through a tempo change at
   the same pitch.
   *Verify:* a 10 s edit at 2× exports 5.0 s ± 1 frame with picture and sound in sync at the end
   (a clap) within 40 ms; at 0.5×, 20.0 s.

## Not in this plan

Keyframed settings, crop by dragging a box, straighten, LUTs, curves on video, stabilisation,
speed ramps, reverse, HDR export with a look.
