# 08 — Video Trimming & Light Editing

Scope: trim/cut length, rotate, extract, convert. **Not** an NLE — no multi-track timeline, no
transitions. Keep that boundary or the project never ships.

## Three trim modes

### 1. Keyframe trim (lossless, instant)
Snap in/out points to the nearest keyframes, then stream-copy:

```
ffmpeg -ss <kf_in> -i in.mp4 -to <kf_out> -c copy -avoid_negative_ts make_zero out.mp4
```

No re-encode, no quality loss, ~1 s for any file length. The UI must **show the keyframe grid on
the timeline** so the snapping is visible and expected rather than surprising.

### 2. Smart cut (frame-accurate, near-lossless) — **v1.1, not v1**

A real differentiator and a join-artifact minefield: the head/tail encode must match the source's
profile, level, GOP structure, and bitrate closely enough that the seam is invisible, and getting
it wrong ships a visible quality step at both ends of every clip. **Ship modes 1 and 3 first**
(**D7** in [01-decisions.md](01-decisions.md)), then add this once the two-path version is boring
and the golden-file corpus exists to catch seams. The algorithm, preserved for then:
Most tools force you to choose between "exact" and "fast". You don't have to. Given a requested
cut `[t_in, t_out]`:

```
kf_a = last keyframe <= t_in
kf_b = first keyframe >= t_in
kf_c = last keyframe <= t_out
```

1. **Head segment**: re-encode `[t_in, kf_b)` only — usually well under a second of video —
   matching the source's codec, profile, level, resolution, pixel format, GOP structure, and
   targeting the source's measured bitrate (+10 %).
2. **Body**: stream-copy `[kf_b, kf_c)`. This is 99 % of the data and is untouched.
3. **Tail**: re-encode `[kf_c, t_out]` the same way.
4. Concatenate with the concat demuxer, remux to the target container, fix timestamps.

Only the ~1 s at each end is ever recompressed. Requires matching encoder parameters carefully —
mismatch shows as a visible quality step at the join. Extract source params from
`AVCodecParameters` and, for H.264/HEVC, reuse the source SPS/PPS-derived settings.

Fall back to mode 1 with a clear explanation if the codec can't be re-encoded to match
(e.g. some hardware-specific profiles).

### 3. Full re-encode
When the user changes codec, resolution, or applies filters. Offer NVENC / Quick Sync / AMF
hardware encoding with an x264/x265 software fallback; show an honest quality-vs-speed choice
rather than a "quality" slider with no units.

## Other operations worth having

- **Lossless rotate** — write the container rotation matrix, don't re-encode.
- **Split at points** / **remove a middle section** (two smart cuts + concat).
- **Container remux** (MKV ↔ MP4) without touching streams.
- **Extract**: audio track to WAV/FLAC/AAC-copy; current frame to PNG/JPEG at full resolution;
  a frame range to an image sequence; a clip to GIF/WebP/animated AVIF with a good palette
  (two-pass `palettegen`/`paletteuse`).
- **Merge** clips with identical parameters via stream-copy concat.
- **Subtitle** extract/embed/burn-in.

## Execution & UX

- Run FFmpeg **in-process via libav\*** for probing and simple remux; **as a child process** for
  encode jobs. A child process means a crash in an encode can't take the viewer down, and
  cancelling is a clean kill.
- All jobs go to a **queue panel**: progress, ETA, speed, cancel, retry, "reveal in Explorer".
  Never a modal progress dialog.
- **Never overwrite the source.** Default output `name_trimmed.ext` next to the original;
  confirm explicitly for any in-place replacement.
- Preview the cut before committing by setting the player's A–B loop to the proposed range — free,
  instant, and exactly what the output will be.
- In/out markers are keys as well as a mouse on the scrub bar: `[` / `]` in trim mode
  ([16-commands.md](16-commands.md)). Arming trim is a command (`Ctrl+T` or the command
  bar); until then those keys rotate stills. The job queue panel is keyboard-reachable
  (`Esc` out, `Delete` cancel focused job) — a modal progress dialog would fail the
  mouse-free bar.
