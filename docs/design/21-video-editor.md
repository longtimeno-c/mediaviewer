# The Video Editor

The Video Editor window: one clip cut into pieces on a timeline, played back over the edit and
exported to a new file, on both hosts. It is part of the base app and adds no library, model or
download; it is built on the clip core ([08](08-video-editing.md)).

## What it is

*Edit video* (or `Enter`) on a clip opens a window of its own with a preview and a timeline:

```
┌ Video Editor — GOPR0412.MP4 ─────────────────────────────────────────────────────────────┐
│                                                                                           │
│                     preview: the viewer's own canvas, moved into this window              │
│                                                                                           │
├───────────────────────────────────────────────────────────────────────────────────────────┤
│ ◁| ❚❚ |▷  0:07.57 / 0:10.70 │ Split  Delete  Mark in  Mark out  Trim start  Trim end │ Undo  Redo   Export  Export exact │ Done │
│ 0:00      0:01      0:02      0:03      0:04      0:05      0:06    ▼ 0:07      0:08      0:09   │
│ [▣▣▣▣▣▣ piece 1 (thumbnails) ▣▣▣▣▣▣][▣▣▣▣ piece 2, selected ▣▣▣▣▣▣▣▣▣▣]                        │
│ [∿∿∿∿∿∿∿ waveform ∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿][∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿∿]                        │
│ Space play · ← → frame · I O mark · ⌘B split · ⌫ delete · ⌘Z undo · ⌘E export                │
└───────────────────────────────────────────────────────────────────────────────────────────┘
```

Scope: one clip, one video track and its audio, cuts only.

## Pieces

| Piece | Code |
|---|---|
| The cut list: the kept source ranges of one clip, in source order, played back to back; split, delete, trim start / end, marked ranges, edge drags, undo / redo; source ↔ timeline clocks; where playback jumps | `shell/video_timeline.{h,cpp}` (shared; `test_video_timeline`) |
| `J K L` shuttle | `shell/video_timeline.h` `editor_shuttle` |
| The strip: keyframe thumbnails (sRGB, display-rotated, HDR tone-mapped like the canvas) and an audio peak envelope | `edit/clip_strip.h`, in `edit/clip_encode.cpp` |
| Export: `clip::op::keep_ranges` over the pieces, on keyframes (packets copied) or exact (every piece decoded and re-encoded on the hardware encoder, in `MediaViewerClipJob`) | `edit/clip_run.cpp`, `clip_encode.cpp`; ABI 0.13 `mv_clip_request.ranges_ns` / `range_count` |
| Mac window, canvas hand-off, playback follow | `main_mac.mm` (`setEditorOpen:`, `moveCanvasToEditor:`, `editorFollowPlayback`) |
| Mac timeline UI | `VideoEditorView.swift` |
| Windows window, canvas hand-off, keys, playback follow | `main.cpp` (`set_editor_open`, `editor_window_proc`, `editor_key`, `editor_follow_playback`); `gfx/swapchain` `retarget`, `present_lab.cpp` |
| Windows timeline island and the viewer's "away" card | `IslandHost.VideoEditor.cs` (`BuildEditorAway`), `chrome_host.h` |

## The cut list

`video_timeline` holds the kept ranges (`pieces()`, ascending, never empty once loaded). There are
two clocks: **source** time is the player's timeline (`clip::time_ns`); **timeline** time is the
edited program, `0 .. length()`.

- `split(t)`, `remove(i)`, `set_in(t)` (cut everything before), `set_out(t)` (cut everything
  after). An edit that would change nothing or leave nothing returns false. Pieces shorter than
  `kMinPiece` (40 ms) are not made.
- **Marks:** `mark_in` / `mark_out` mark a range and cut nothing; `remove_marked` cuts it as one
  undo step; any edit clears marks.
- **Edge drags:** `begin_trim` / `trim_to` / `end_trim` move a piece's edge in source time,
  clamped inside its neighbours; the whole drag is one undo step.
- **History:** undo / redo, up to 200 steps. `revision()` bumps on every change; the host compares
  it with the revision last exported to know whether closing would lose work.
- `next_play_start(source_t, lead)` tells the host where to be: inside a piece, the same time; in
  a cut or within `lead` of a piece's end, the next piece's start; past the last piece, stop.
- `export_request(source, exact)` builds the `keep_ranges` request.

## The window and the canvas

The preview is the viewer's own canvas, moved into the editor window while it is open and back
when it closes. There is one canvas, one swapchain / `CAMetalLayer` and one present path per OS.

- **Mac:** `setEditorOpen:` moves the canvas layer into the editor window's preview.
- **Windows:** a top-level Win32 window owned by the viewer (stays above it, minimises and closes
  with it), preview on top and the timeline island below. The one composition swapchain's DComp
  visual moves to a target on the editor window: `input_snapshot::canvas_window` asks for it, the
  render thread retargets between frames and resizes to the preview. The editor window is
  destroyed only after `present_lab::canvas_window()` reports the swapchain has left it.
- **The viewer while the editor has the canvas** (Windows): a card over the canvas area
  ("Editing in the Video Editor window", *Show editor*, *Done*) beside any right-edge pane; the
  filmstrip and transport step aside; the bar's button reads *Done*; any key in the viewer raises
  the editor; another item on the canvas closes the editor.

**Playback over the edit:** a UI-thread tick (60 Hz on Mac; a 15 ms timer on Windows) asks
`next_play_start` and jumps the player over each cut, pausing at the end.

**Closing** with an edit not exported since it last changed asks first ("Discard this edit?").
Closing the viewer window while the editor is open (Mac) closes the editor first, through that
prompt; *Keep editing* leaves both windows open.

## Timeline UI

Transport, timecode (frames at the clip's rate), Split / Delete / Mark in / Mark out / Trim start /
Trim end / Undo / Redo, Export / Export exact, Done. A ruler, one thumbnail track and one waveform
per piece, a draggable playhead that snaps to joins and marks, piece selection, draggable piece
edges (the preview shows the edge frame). On Mac the playhead is its own layer, so the thumbnail
canvas does not redraw while playing, and the timeline stops polling when the window closes.

Windows island ABI (`chrome_host.h`): native pushes `chrome_editor_view_args` (104 bytes) on each
edit and the playhead on the tick, and `chrome_editor_strip_args` once per clip. The island sends
`editor_seek` (1023, program milliseconds), `editor_action` (1024, `chrome_editor_action`; 1–6 are
the Mac bridge's `mv_chrome_editor_edit` codes), `editor_trim_grab` (1028) and `editor_trim_to`
(1029). Both sides' checksum tests pin the ids.

## Keys

Every key aimed at the editor window goes to the editor (`editor_key` on Windows,
`VideoEditorView` on Mac), never the browse router. Mac uses `⌘` for `Ctrl`.

| Key | Action |
|---|---|
| `Space` | Play / pause |
| `←` `→` (`Shift`: ten frames) | Step a frame |
| `Home` / `End` | Start / end (Windows) |
| `J` `K` `L` | Shuttle: `L` 1×, 2×, 4×; `K` stop; `J` skims back 1, 2, 4, 8 s on quick presses |
| `I` / `O` | Mark in / out |
| `X` | Clear marks |
| `[` / `]` | Trim start / Trim end (cut before / after the playhead) |
| `Ctrl+B` | Split at the playhead |
| `Delete` / `Backspace` | Remove the marked range, else the selected piece |
| `Ctrl+Z` / `Ctrl+Shift+Z` (`Ctrl+Y` on Windows) | Undo / redo |
| `Ctrl+E` / `Ctrl+Shift+E` | Export (keyframes) / Export exact |
| `Esc`, `Ctrl+W` | Close (Windows) |

`Tab` and `Enter` stay with the island on Windows.

## Exports

- **Export** cuts each piece on its nearest keyframe and copies packets (instant; the file runs a
  little longer than the program, labelled). **Export exact** decodes and re-encodes every piece on
  the hardware encoder (Path 2's encoder rules, [08](08-video-editing.md)); audio is copied
  packet-accurately, so a join can carry up to one audio packet of the cut.
- Output is a new file `<name>_edit.<ext>` beside the source, collisions numbered; never an
  overwrite. Staged as `.mvpart`, renamed on success; cancel, failure or a helper crash removes it
  (`sweep_temporaries`). The source is never written.
- Exports and strip builds are worker jobs; encodes run in the helper process. Nothing that
  decodes, encodes or reads a file runs on the UI or render thread.

## Self-test and soak

`MV_EDIT_SELFTEST` (both hosts) also drives the editor on a clip: open → split at ⅓ and ⅔ →
delete the middle → export both ways → close, recording `canvas_moved` in `state.txt`.
`MV_EDIT_SELFTEST_KEYS=1` (Windows) drives the same by synthesised key messages.
`MV_EDIT_SELFTEST_SOAK` (Mac) measures main-thread CPU while playing and counts playback ticks
that land inside a cut. Tests: `test_video_timeline` (`[timeline]`, `[editor-polish]`) and the
`[pr30]` cases in `test_clip` (pieces with and without B-frames, exact pieces frame for frame, the
helper wire, the strip). See [`../DEVELOPMENT.md`](../DEVELOPMENT.md).

## Spikes

### S1 — encoder probe

`tools/encprobe` (opt-in, not part of the app build; `tools/encprobe/CMakeLists.txt`) prints a
Markdown report of what this machine's FFmpeg build can encode and filter, using synthetic frames
only (no user file, no path):

1. Video encoders: the encode port's candidates plus AV1 and ProRes, each opened at 1080p30 8-bit,
   2160p30 8-bit and 2160p60 10-bit and timed. Software HEVC and x264 / x265 are refused, never
   probed.
2. Audio encoders the export path could use.
3. Filters in the LGPL build (audio clean-up, loudness, waveform, LUT / colour) and the GPL-only
   ones that must be absent.
4. Hardware decode device types.

Results on Apple M5, macOS 26.6, libavcodec 63.1.101 (encoder only, so upper bounds):

| Encoder | 1080p30 8-bit | 2160p30 8-bit | 2160p60 10-bit |
|---|---|---|---|
| `h264_videotoolbox` | 257 fps (8.6×) | 71 fps (2.4×) | n/a (no 10-bit H.264) |
| `hevc_videotoolbox` | 250 fps (8.3×) | 69 fps (2.3×) | 69 fps (1.1×, P010) |
| `prores_videotoolbox` | 1812 fps (60×) | 494 fps (16×) | 437 fps (7.3×, P210) |

- Audio: `aac_at` and `alac_at` (OS encoders) open; `flac` and PCM are available. FFmpeg's native
  `aac` is present but not used; native `opus` is experimental; `libopus` is not in the build.
- Filters present: `volume`, `equalizer`, `highpass`, `acompressor`, `alimiter`, `loudnorm`,
  `ebur128`, `afftdn`, `anlmdn`, `arnndn`, `showwavespic`, `amix`, `lut3d`, `tonemap`,
  `colorspace`. `zscale` is absent (no zimg). The GPL-only `eq`, `hqdn3d` and `delogo` are absent.
- Hardware decode: `videotoolbox` creates a device.

No Windows report has been recorded.

## Not built

- Several clips in one timeline, reordering pieces, and `keep_ranges` across sources.
- Timeline zoom and a keyframe grid at frame zoom; cross-dissolves.
- A saved project file.
- `edit/sequence`, a gapless sequence source in `player/`, a grade stage, scopes, audio DSP, and a
  `render_sequence` export op.
- `prores_videotoolbox` and `av1_*` as export encoders (they exist only in `encprobe`).
- Sequence frame-rate / resolution conforming, mixed-audio resampling, HDR export.
- An editor cache root, cache cap and pre-render free-space check.
- Spikes S2–S5.
- The Editor add-on ([22](22-editor-addon.md)).
