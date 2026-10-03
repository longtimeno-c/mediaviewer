# Video Trimming & Light Editing

The clip operations on a video: two-path trim, rotate, split, remove a range, remux and extract,
and how they run as jobs. The Video Editor window that builds on them is
[21](21-video-editor.md).

Code: `src/edit/clip.h` (the request, probe and keyframe index), `clip_run.cpp`,
`clip_copy.cpp` (stream copy), `clip_encode.cpp` (paths that decode), `clip_jobs` (the queue),
`clip_wire` / `clip_helper` and `tools/clipjob` (the helper process), `src/shell/trim_state`
(trim mode and the clip tools flyout, shared by both hosts). ABI: `mediaviewer_clip.h`
(ABI 0.10, `keep_ranges` in 0.13).

Every operation is one `clip::request` run by `clip::run()` on a worker. Hosts only add UI: the
op, the keyframe grid, snapping and output naming live in the core, so the same request writes the
same file on Windows and macOS.

## Probe and keyframe index

`clip::probe` opens the file and returns `clip_info`: duration, the presentation times of the
video keyframes on the player's timeline, size, frame rate, display rotation, bit depth, HDR
flag, codec and container family. It uses the demuxer's index when complete (MP4/MOV carry every
sync sample) and otherwise walks packets without decoding. Times are the player's timeline
(`time_ns`, nanoseconds from the container start).

The keyframe list is the grid shown on the scrub bar in trim mode, so snapping is visible.
`keyframe_at_or_before`, `keyframe_at_or_after` and `keyframe_nearest` do the snapping;
`keyframe_range` gives exactly what Path 1 will write for a requested `[in, out)`.

## Trim paths

### Keyframe trim (Path 1)

`trim_keyframe`: stream copy from the keyframe at or before `in` to the keyframe at or after
`out`. No re-encode, no quality loss, seconds for any length. Runs in process (it opens no
codec). Output `<name>_trimmed.<ext>`.

### Re-encode trim (Path 2)

`trim_reencode`: frame-accurate `[in, out)`. Video is decoded (software, on the job's own
thread, never the player's hardware decoder on the render device) from the keyframe before `in`
and re-encoded on a **hardware encoder** from the encode port (`edit/hwencode.h`); audio is
stream-copied. Labelled slower; the job records the encoder that ran.

| Platform | Encoder candidates, in order |
|---|---|
| Windows | `h264_nvenc`, `h264_qsv`, `h264_amf`, `h264_mf` (and `hevc_*` of each first for an HEVC source) |
| macOS | `h264_videotoolbox` (`hevc_videotoolbox` first for an HEVC source) |
| Headless core | none |

The first encoder that opens at the clip's size and pixel format wins, so a machine without that
GPU falls through. Any encoder FFmpeg does not mark hardware or hybrid is refused
(`licence_ok`): no x264 / x265, no software HEVC. Bitrate is the source's video rate (or 90 % of
the file's), +10 %, clamped to 1–200 Mb/s; GOP is 2 s; colour tags and 10-bit (HEVC) are carried
over. With no hardware encoder the job fails with `unsupported_format`.

## Other operations

| `op` | What it writes | Path |
|---|---|---|
| `rotate` | A new display matrix (clockwise delta, multiple of 90), no re-encode. `_rotated` | In process |
| `split` | Two files at the keyframe nearest `in`. `_part` | In process |
| `remove_middle` | One file without `[in, out)`, both edges on keyframes. `_cut` | In process |
| `remux` | MKV ↔ MP4, every stream the target can hold | In process |
| `audio` | The first audio track: stream copy, WAV or FLAC. `_audio` | Copy in process; WAV / FLAC in the helper |
| `frame` | The frame shown at `in`, as PNG or JPEG (quality 92), sRGB. `_frame` | Helper |
| `animation` | `[in, out)` as GIF (two-pass palette) or animated WebP; long edge 16–1920 px (480 default), 1–50 fps (15 default); refused over 60 s | Helper |
| `keep_ranges` | The listed ranges in one file, on keyframes (copy) or exact (re-encoded like Path 2); up to 1000 ranges. `_edit`. Used by the Video Editor ([21](21-video-editor.md)) | Copy in process; exact in the helper |

`split` and `remove_middle` are offered on a clip as `Ctrl+B` and `Ctrl+X` (in trim); the rest
are in the **Clip tools** flyout (`Ctrl+S` on a clip).

## Execution & UX

- **Encode jobs run in a child process.** The queue starts `MediaViewerClipJob`
  (`tools/clipjob`) for every job that decodes or encodes and speaks a line protocol with it
  (`clip_wire.h`): one JSON request line on stdin, then `cancel` or EOF to cancel; on stdout
  `progress`, `encoder`, `output`, `written`, then `done` or `error`. A crash in a driver's
  encoder ends the helper, not the viewer, and cancelling is a kill after a grace period.
  Stream-copy jobs stay in process.
- **One queue, one worker** (`clip_jobs.h`): FIFO, every job cancellable while queued or running.
  One worker because a keyframe trim is disk-bound and a re-encode owns the GPU encoder. The
  decode pool is not used, so an export never delays a photo decode. Submit, cancel, retry and
  snapshot are any-thread and never block; the lock is never held across a job's I/O.
- **The Jobs pane** on both hosts (`Ctrl+J`) is a view of `snapshot()`: title ("Trim (re-encode,
  NVENC)"), progress, elapsed, ETA, error. Keyboard: `↑` `↓` choose, `Delete` cancel, `R` retry,
  `Enter` / `Return` reveal in Explorer / Finder, `Esc` back. There is no modal progress dialog.
- **Never overwrite the source.** The source is opened read-only. Outputs go beside it (or into
  `out_dir`) under a name that is free when published (collisions numbered), written first to a
  hidden `.<name>.mvpart` sibling and renamed. Cancel, failure or a helper crash leaves no partial
  output (`sweep_temporaries`). Nothing here logs a path.
- **Preview the cut.** `P` in trim sets the player's A–B loop to exactly what Path 1 will write
  (`keyframe_range`).
- **Keys** (trim is armed with `Ctrl+T` on a clip, which opens the Edit workspace on its Trim
  tab, [20](20-edit-workspace.md)): `[` / `]` in / out at the playhead, `Backspace` / `Delete`
  clear, `P` preview, `Enter` save (Path 1), `Shift+Enter` save exact (Path 2), `Ctrl+←` /
  `Ctrl+→` previous / next keyframe, `Ctrl+X` remove in–out, `Ctrl+B` split, `Ctrl+S` clip tools.
  Unarmed, `[` `]` rotate stills. See [16](16-commands.md).

## Not built

- Smart cut (re-encode only the head and tail GOPs, copy the body).
- Full re-encode with a codec, resolution or filter change, and quality/speed presets for it.
- Merge of clips by stream-copy concat (beyond `keep_ranges` of one source).
- Frame range to an image sequence; animated AVIF.
- Subtitle extract, embed or burn-in.
