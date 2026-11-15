# Clips the phone can't play, converted by the server

Status: built (draft PR). Mac verified in the Simulator; Windows written to compile, not run.

The owner's library lives on their own computer (`docs/plans/self-hosted.md`) and the phone reads it
in file mode. AVFoundation opens MP4/MOV/M4V, and (since `TsStreams`) H.264 transport streams as
HLS. Everything else, AVI, MKV, WebM, MPG/MPEG, MPEG-2 or HEVC in TS, LPCM sound, is "can't play
here" with a download that then does not play either. The server has FFmpeg and a hardware encoder;
the phone has neither for these formats. So the server converts, on its graphics hardware, while
the phone watches.

## Shape

```
phone: AVPlayer ── http://127.0.0.1/<token>/index.m3u8 ─▶ TsStreams (loopback HLS, in the app)
                                                           │ plan, then segment k as asked
                                                           ▼
link:  stream_open {"op":"convert","root","rel","seg":-1|k}  (serve_wire.h, kConvertOp)
                                                           ▼
server (driftwire host / serve):  serve_convert.h  ── cache hit? send the file
                                   one worker, a queue  ── miss: run the helper, then send
                                                           ▼
helper (MediaViewerClipJob --convert-segment, low priority): FFmpeg demux + decode,
        VideoToolbox / NVENC / QSV / AMF / Media Foundation H.264, the OS's AAC, MPEG-TS out
```

### Why HLS of fixed-time segments, not one fragmented MP4

AVPlayer needs a fragmented MP4's length and its index before it can seek in it, which means
converting the whole clip first. HLS gives seeking for free: the playlist is the timeline, and each
segment is converted when AVPlayer asks for it. Segments are a fixed 4 s of the source's timeline
(the last shorter), so the playlist needs only the duration, not a keyframe index (an MPG has none to
read cheaply). Segment k is decoded from the keyframe at or before 4k s, frames before 4k are
dropped, and the encoder starts it with an IDR. Each segment is a self-contained MPEG-TS (PAT, PMT,
SPS/PPS in band), timestamps on the source's timeline, so segments made at different times by
different helper runs play one after another. MPEG-TS rather than fMP4 segments because an fMP4
playlist shares one init segment, and two encoder sessions are not promised to produce the same
SPS/PPS. The loopback HLS server is the one `TsStreams` already has (AVFoundation takes no HLS
segments from a resource loader).

### The wire op

`convert` (op 7, `kConvertOp`). Like `write`, it is not in `kAllOps`: the service adds it to
`serve_caps` only when its converter is there (a helper found and a hardware encoder that opens)
and the owner has not turned it off. A phone that does not list it, or a service that does not know
the name, never sees it (the capability rule of `serve_wire.h`). The phone asks for it only when
`serve_caps` has it; otherwise it keeps today's "can't play here".

- `{"op":"convert","root","rel","seg":-1}`: the plan. `stream_end ok
  {"duration_ms","segment_ms","segments","width","height"}`. A file it cannot convert (no video,
  nothing FFmpeg opens): `unsupported {"why":"not-a-clip"}`.
- `{"op":"convert","root","rel","seg":k,"credit"}`: segment k's MPEG-TS as `stream_data`, then
  `stream_end ok {"sent"}`, exactly as `read_range`. The bytes start when the segment is ready; credit
  and cancel work as for any stream while it waits.

The path is checked as every other request's (root id, `safe_relative`), and the file is opened
beneath the root; the helper gets the resolved path only after that check.

### The cache

`<host data>/serve/convert-cache/<key>/` holds `plan.json` and `seg-<k>.ts`. The key is the BLAKE2b
of the folder id, the relative path, the file's size and mtime and the converter's version, so an
edited or replaced file is converted again and the cache's names say nothing about the library.
Bounded (2 GiB by default), oldest-used segments go first; the off switch empties it. It is never
beside an original (rule 5).

### Bounded cost on the server

One conversion at a time across all sessions. A requested segment goes first; the next two after it
are made ahead while the phone plays, and a seek drops what was queued ahead for that clip. The
helper runs at background priority (POSIX `nice 10` and utility QoS; Windows
`BELOW_NORMAL_PRIORITY_CLASS`), decodes on at most two threads, and encodes on the media engine,
not the 3D pipeline the viewer presents with. A helper crash ends one segment (the phone sees an
error on that segment), never the service.

Output: H.264 High, long side at most 1920 (square pixels, so a 16:9 PAL DVD becomes 1024×576),
about 0.1 bit per pixel per frame (clamped to 1.5–10 Mbit/s), no B-frames (no negative DTS at a
segment's start); AAC-LC 160 kbit/s stereo, 48 or 44.1 kHz, from the OS's encoder (`aac_at`,
`aac_mf`: design 11 keeps FFmpeg's own `aac` out); with neither, the segment is silent. Timestamps
are the source's plus 10 s. A tail under 250 ms joins the last segment. No deinterlacing yet
(MPEG-2 from DVDs and old camcorders shows combing): a follow-up with the LGPL `bwdif` filter.

## Measured (M-series Mac, VideoToolbox, 30–60 s samples)

Over loopback, real helper behind the real service (`"[.convert-bench]"`), cold cache:

| Clip | Plan | First frame (plan + segment 0) | Seek to the middle | From the cache |
|---|---|---|---|---|
| AVI 1080p30, MPEG-4 Part 2 + MP3 | 105 ms | 650 ms | 590–600 ms | 5–8 ms |
| MKV 1080p30, VP9 + Opus | 107 ms | 646 ms | 640–655 ms | 5 ms |
| MPG 720×576, MPEG-2 + MP2 | 102–111 ms | 317–322 ms | 218–222 ms | 2–3 ms |

Conversion runs at 5.6–7.4× realtime for 1080p and about 19× for SD (`clip_convert`'s own runs). A
segment made ahead for one clip delays the next clip's first frame by up to one segment's time
(about 0.6 s at 1080p), since a running job is not interrupted. In the Simulator, all three play
and seek (forward, back, past the end) with posters from their first segment; the first frame came
in under a second, so the "Converting on your server…" line (after a 1 s wait) was not seen.

### Phone

`LinkClips.route` (file mode `fileRoute` and the catalogue route) returns `.stream` through
`TsStreams` for any clip that would have been `notPlayableHere` when the session's caps include
`convert`, and the plan says it converts. Posters use the same route (`LinkFiles.poster`). While the
first segment is on its way the player shows "Converting on your server…".

### Desktop

The Phone access card's server section (`addons/phone/screens.cpp`, one screen for both chromes)
says "Convert clips for the phone" with the encoder found ("Apple VideoToolbox", "NVIDIA NVENC"...)
or why not ("No hardware video encoder on this computer"), and an off switch. `driftwire host convert
on|off` sets it in `host.json`; the running host writes what it found to `state.json`.

### Where the converter code lives

In the app's existing clip helper (`MediaViewerClipJob`, `src/edit`), which already carries FFmpeg,
the hardware encoder port and the AAC encoder, and is the process that is allowed to crash in a GPU
driver. The host is given its path by the card (`host convert on --helper PATH`). Carrying a second
FFmpeg in the Phone access add-on would double ~40 MB for the same code. The owner kept it there
(2026-11-15), an exception to "add-ons carry their own code" (decision log).
