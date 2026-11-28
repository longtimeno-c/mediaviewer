# Video / audio path audit (read-only)

Repo state: branch `claude/project-audit-performance-180749` @ a232bda4. Nothing built or run; every
item below cites code I read. "Confirmed" = the behaviour follows directly from the code read;
"suspected" = plausible from the code but needs a measurement or a run to prove.

Open issues checked (`gh issue list --limit 200 --state open`): none of the findings below are filed.
Related-but-different: #572 (trim on MP3/M4A writes .mkv), #587 (chapters), #590 (sync nudge), #155 (test gaps, unrelated area).

---

## 0. What the "22 fps / 636 ms" number actually is (read this first)

* `docs/perf/compare/screen.json` `play[hevc_4k_8bit_bt709.mp4, mv]`: `fps 22.1`, `changes 265`, **`p99_ms 635.8`**. It is a
  **99th-percentile** gap from a screen-grab diff, not a worst gap (`tools/perf/compare-screen.ps1:60-68`; `docs/img/compare-video.svg` caption says "99th percentile gap"). The README calls it "worst gap" (README.md:94). With 264 gaps, p99 = `sorted[261]`, i.e. **at least 4 gaps of >= 636 ms** in a 12 s sample — ~2.5 s of frozen picture, consistent with 360 expected vs 265 seen changes. So it is real stalling, not grab noise (the 1080p clip measures 27.4 fps in both apps with the same grab).
* The run: launch, `Start-Sleep 4`, **`MoveWindow` to 1600x900 (a swapchain resize during playback)**, 1 s, then 12 s sampling of a 20 s clip (`compare-screen.ps1:163-175`; clip is `testsrc2 3840x2160 30 fps 20 s`, `tools/testmedia/generate.sh:122,143`).
* **The 4K clip has never been measured with the in-app harnesses.** Every native/`--av-soak` result in `docs/perf/investigation/` is the 1080p soak clip (`investigation/README.md` table + repro commands); `grep 4K docs/perf` hits only `compare/screen.json`. The overlay counters that would localise the stall (`held_starved`, `ring_backpressure`, `surface_waits`, `decode_errors`, observed decoder) have never been recorded for this file.
* Windows Media Player "stayed on a single frame" on the same file — the file itself is unusual for MF (likely missing HEVC extension), not proof of anything about MV.

**Step 1 of any fix is to measure, and it is cheap:**
```
mediaviewer_lab.exe tools\testmedia\hevc_4k_8bit_bt709.mp4 --soak 15 --static --json 4k.json   # 4k.json + 4k.json.video.json (selected/dropped/starved/error_p99)
mediaviewer_lab.exe tools\testmedia\hevc_4k_8bit_bt709.mp4 --av-soak 15 --csv 4k.csv           # decoder name, ring_backpressure, surface_waits, decode_errors per second
```
Caveat: `--av-soak` returns 3 when the clip ends before `seconds` (`src/shell/av_soak.cpp:72`), so keep it under 20 s or generate a longer 4K clip; and `--av-soak` never presents, so on D3D11 nothing flushes the immediate context except the decoder itself (`av_soak.cpp:56`) — use `--soak` for anything about on-screen behaviour. Then repeat `--soak` with a resize mid-run to reproduce the compare script.

---

## 1. Performance opportunities, ranked

Answers to the specific questions first (all confirmed by reading):

| Question | Answer |
|---|---|
| Is 4K HEVC hardware decoded? | Yes when D3D11VA offers the surface: `pick_decoder` prefers an hwaccel-capable decoder (`video_decode.cpp:175-189`), `get_format` picks `AV_PIX_FMT_D3D11` (`:54-66`), NV12/P010 accepted (`hwdecode_win.cpp:95-104`). The *observed* decoder is recorded per frame (`video_decode.cpp:261-263`) and shown in F3 / `--av-soak` CSV — but nobody has recorded it for this clip (section 0). |
| GPU→CPU readback? | Not on the hardware path: GPU-to-GPU `CopySubresourceRegion` into a ring texture (`hwdecode_win.cpp:105-131`). Readback only for 4:2:2/4:4:4 surfaces (`video_decode.cpp:120-132`). No `Map` on video textures anywhere in `src/gfx`/`present_lab.cpp` except the ImGui/constant buffers. |
| Software colour conversion? | No on HW path: YUV→RGB in the pixel shader (`gfx/video_blit.cpp:170-188`). Software path uses swscale to NV12/P010 then **creates a new texture per frame** (see P4). |
| Queue depth | Packet queue 512 (`video_internal.h:116`). Presentation ring 4 slots D3D11 / 6 Metal (`video_internal.h:183-190`) — **effectively 2 frames of look-ahead on both** (P1). |
| Present / clock selection | Present after waiting on the DXGI frame-latency waitable, `SetMaximumFrameLatency(1)` (`swapchain.cpp:81,157-167`; `present_lab.cpp:1390-1401`), clock sampled after the wait; `choose()` shows the oldest due frame, drops only if a newer one is also due (`presenter.cpp:7-49`). Sound. |
| Decoder thread scheduling | `thread_count = 1` on D3D11VA (`video_decode.cpp:320-323`); demux/decode/audio-decode are plain `std::thread`s at default priority (no `SetThreadPriority`/MMCSS anywhere in `src/player` except the WASAPI render thread, `audio_win.cpp:560`). 1 ms timer held while playing (`hwdecode_win.cpp:215-220`, `media_source.cpp:59`). |
| 10-bit / HDR | P010 ring + `R16/R16G16` SRVs (`frame_ring.cpp:113-135`), PQ/HLG tone-mapped in the shader (design doc 05:120-123). No extra pass. |
| Audio clock | `IAudioClock::GetPosition` (`audio_win.cpp:429-475`), anchored per seek on the first written block (`av_clock.cpp:731-739`). See C4 for an offset. |

### P1. Ring look-ahead is only ~2 frames on both platforms — confirmed by reading, gain suspected
* D3D11: 4 slots (`video_internal.h:189`). One is always held by the session as the on-screen frame until the *next* frame is acquired (`abi/video_session.h:107-109`), one is the decode thread's pending reservation (`video_decode.cpp:386`, `frame_ring.cpp:197-208`, reserved *before* `receive_frame`). That leaves **2 queued frames = 66 ms at 30 fps, 33 ms at 60 fps**.
* Metal: 6 slots, but on screen + `kRetiredFrames = 2` held (`present_lab_mac.h:537`, `present_lab_mac.mm:2212-2221`) + 1 pending = **also 2 queued**. The design doc's "6 on Metal ... headroom" (05:84-85) is fully consumed by the retire hold.
* Any decoder hiccup longer than 2 frame times (an `E_PENDING` retry burst, device-lock contention — P2 — a resize, a page fault on a 25 MB surface) becomes `held_starved` + catch-up drops, which is exactly the "long gap then skip" signature in section 0.
* **Change:** D3D11 `frame_ring_slots` 4 → 8 (NV12 4K ≈ 12.5 MB/slot, P010 ≈ 25 MB → +50/+100 MB VRAM), Metal 6 → 9; raise `extra_hw_frames` with it (it is `slots + 4`, `video_decode.cpp:305`). Better still, size it from the clip: slots = ceil(150 ms × fps) + 2, capped.
* **Expected gain (guess):** absorbs stalls up to ~200 ms with no visible effect; turns most of the 636 ms-class gaps into nothing *if* the stalls are short and bursty. Does nothing for a sustained decode-rate deficit (then `ring_backpressure` stays 0 and `held_starved` climbs — the measurement in section 0 tells which).
* **Risk:** low (VRAM; the ring is fixed-size arrays, `index_capacity = slots + 1`). Seek latency unchanged (stale frames are recycled at acquire, `frame_ring.cpp:230-236`). Paused clip pins more VRAM per open clip (only one is open plus a retiring one).
* **Measure:** `--soak 15 --static --json` on the 4K clip before/after: `.video.json` `starved`, `dropped`, `error_p99_ms`; main JSON `max_ms`, `dropped_frames`. Then `compare-screen.ps1` for the README number. On Mac the same with `--open`.

### P2. Decoder and render thread share one immediate context under the D3D10 multithread lock — suspected cause of long stalls
* The device is `SetMultithreadProtected(TRUE)` (`gfx/device.cpp:133`); FFmpeg's D3D11VA decode (`DecoderBeginFrame…EndFrame`) and our copy-out (`hwdecode_win.cpp:119-126`) both go through the **render thread's immediate context** under that lock. Every render-thread D3D11 call (ImGui, blits, `Present`, `ResizeBuffers` in `swapchain.cpp:175-199`) contends with it, and the decode thread's copies are not flushed to the GPU until the render thread's next Present/Flush.
* FFmpeg's own E_PENDING loop sleeps 2 ms up to 50 times per frame while re-taking this lock; with `thread_count = 1` (`video_decode.cpp:320-323`) bitstream parsing, hwaccel submit, those sleeps and the copy are all serial on one thread.
* A swapchain resize (`swapchain.cpp:181-191`: `Flush` + `ResizeBuffers`) or a device-wide stall on the render thread therefore stalls decode for its duration — the compare script resizes the window during playback (section 0).
* **Change options:** (a) cheap: measure first with a trace (`core/trace.h` spans around `avcodec_send_packet`/`receive_frame`/`copy_hw_surface` and around the render loop's `Present`/`ResizeBuffers`), and add a "decode wall time per frame" max to `clock_stats`; (b) structural: decode on a second `ID3D11Device` (VIDEO_SUPPORT) with ring textures created `SHARED_NTHANDLE | SHARED_KEYEDMUTEX` or synchronised with `ID3D11Fence` (`ID3D11Device5`), so decode never takes the render device's lock. The comment block at `video_internal.h:289-305` explains why the copy must stay on the decoder's context — with a separate device it does, and the render side waits on a fence instead.
* **Expected gain:** unknown until (a); (b) removes the whole class of render↔decode coupling. **Risk:** (a) none; (b) medium-high (cross-device sync, device-lost now has two devices, docs/design/12 history).
* **Measure:** trace spans + `--soak` with a mid-run resize; `ring_backpressure` should stay > 0 (decoder ahead) throughout.

### P3. Video worker threads run at default priority; the only real-time thread is WASAPI — suspected, small/medium
* Demux/decode/audio-decode threads are created with plain `std::thread` (`video_source.cpp:246-249`) and never re-prioritised (no hits for `SetThreadPriority`/`AvSetMm`/QoS in `src/player` besides `audio_win.cpp:560`). Background jobs are `BELOW_NORMAL` (`core/job_system_win.cpp:16`) so they do not outrank decode inside the app, but any normal-priority process (the PowerShell grab loop in the compare script included) competes on equal terms.
* **Change:** on Windows `AvSetMmThreadCharacteristicsW(L"Playback")` (or `THREAD_PRIORITY_ABOVE_NORMAL`) for the video decode and demux threads while playing; on macOS `QOS_CLASS_USER_INTERACTIVE` for the decode thread. **Gain (guess):** removes scheduler-induced tail stalls under load; nothing on an idle box. **Risk:** low. **Measure:** `--soak` with a CPU-load generator running.

### P4. D3D11 software fallback allocates a new 4K texture + 2 SRVs and repacks 12–25 MB per frame — confirmed by reading
* `create_texture_from_planes` (`hwdecode_win.cpp:134-209`): `new uint8_t[...]` + row-by-row repack + `CreateTexture2D` with initial data + 2 `CreateShaderResourceView`, then **replaces** the ring slot's pre-made texture (`out->texture = std::move(texture)`), so the ring's own slot textures (`frame_ring.cpp:107-141`) are dead VRAM on this path. `sws_scale` is single-threaded (`video_decode.cpp:95-96`).
* Matters for AV1 via dav1d, unsupported profiles, and the 4:2:2/4:4:4 readback path.
* **Change:** keep the slot textures and fill them with `UpdateSubresource` (or a `D3D11_USAGE_DYNAMIC` slot + `Map(WRITE_DISCARD)`) on the immediate context under the same `hwctx->lock` the HW path already takes — the header comment (`video_internal.h:329-333`) only avoids it on principle; the HW path already holds that exception. Have swscale write straight into one contiguous NV12 buffer (one pitch) to skip the repack; enable sws threads (`av_opt_set_int(sws, "threads", n)`, FFmpeg ≥ 5). **Gain (guess):** large on the software path (allocator + driver churn per frame gone; ~2× less memcpy). **Risk:** low-medium. **Measure:** `--av-soak 15` on an AV1 4K clip (decoder column = `software`), `held_starved`/`decoded` per second.

### P5. Metal copy-out waits for GPU completion per frame and flushes the texture cache per frame — confirmed by reading, gain small
* `copy_hw_surface` (mac): blit on a private queue, `[cb waitUntilCompleted]` on the decode thread, then `CVMetalTextureCacheFlush(cache, 0)` every frame (`hwdecode_mac.mm:139-176`). Doc says ~1 ms at 4K (05:92-93). It serialises decode behind a GPU round-trip and adds a second command queue competing with the render queue.
* **Change (zero-copy):** keep the `CVMetalTextureRef` pair + retained `CVPixelBuffer` in the ring slot and release it when the slot returns to the free list (the retire-after-2 logic already exists for exactly this hazard). Removes a 12–25 MB blit per frame and the wait. **Gain:** ~1 ms/frame decode thread, some GPU bandwidth; mostly matters at 4K60+/8K. **Risk:** medium — VideoToolbox's pool must hold `slots + DPB` buffers; the retire hold must become GPU-completion-based (see C6).
* **Measure:** Mac `--soak 60 --static --json` on 4K60 HEVC; decode-thread trace span.

### P6. Keyframe probe for trim reads every packet of the file — confirmed by reading
* `probe` walks the whole file with `av_read_frame` to build the keyframe grid (`edit/clip_common.cpp:292-314`); every Path 1 op re-probes (`clip_run.cpp:101,155,177,197`). A 2-hour 4K camera file is tens of GB of I/O before the copy starts, and the trim UI shows "reading keyframes…" meanwhile (`trim_state.cpp:105-107`).
* Doc 08:21-22 says it "uses the demuxer's index when complete (MP4/MOV carry every sync sample)" — no index code exists (`grep index_get|AVINDEX` → nothing). See D3.
* **Change:** for MOV/MP4, read `avformat_index_get_entry` + `AVINDEX_KEYFRAME`; the index timestamps are DTS, so convert with the sample's composition offset (the reason the code comment at `:292-296` gave up on NONKEY), or fall back to the walk only when B-frames are present. Cache the grid per (path,size,mtime) so the trim-mode grid and the job share one probe. **Gain:** seconds → milliseconds on large MP4s. **Risk:** medium (edit lists, ctts). **Measure:** `mv_tests "[clip]"` B-frame cases must still pass; time `probe` on a multi-GB file.

### P7. Minor
* `video_session::tick` uses `try_lock` and skips the frame if the UI thread is inside `snapshot()`/`command()` (`abi/video_session.h:67-69,121-124`) — a missed acquisition on that vblank. Low frequency; the fix is to publish `info/stats/position` through a `publish_slot` like `av_clock::published()` so the UI never takes `mutex_`. Suspected small.
* `poll_video` runs `tick` twice per presented frame (`present_lab.cpp:908` with vblank −1, then `:1401`), each copying `media_info`+`clock_stats`. Negligible.

---

## 2. Issues by severity

### High

**C1. Exact seek at or past the last frame's PTS discards every frame; the picture stays wherever it was — confirmed by reading.**
* `seek()` clamps to `duration_ns - 1` (`media_source.cpp:77`) and stores `exact_target_ns` (`:80`). The decode thread drops every frame with `pts < target` (`video_decode.cpp:409-411`). The last frame's PTS is `duration − frame_duration < duration − 1`, so **nothing is published**; the decoder reaches EOF, `video_done` is set (`:393`), and the preview path gives up (`media_source.cpp:173-176`). `shown_pts_` = target, so position reads "end" while the canvas shows the pre-seek frame.
* Reachable today: Video Editor **End** key → `editor_seek(length)` → `to_source(length)` = last piece's `out_ns` (`shell/main.cpp:7474,7001-7006`; `video_timeline.cpp:47-52`) → `mv_video_seek(..., exact=1)` (`main.cpp:6998`). Also a scrubber release at the far right, a typed time ≥ last frame, and any A-B loop/resume target in the last frame interval.
* Related off-by-one: an exact seek to T between two frames shows the *next* frame (pts ≥ T), not the frame on screen at T.
* **Fix:** in `run_video_decode_thread`, keep the most recent frame with `pts < target` (one `AVFrame` ref, within `extra_hw_frames`) and publish it when the next frame's pts > target arrives *or* at EOF; i.e. target means "frame covering T". **Test (missing):** `tests/test_video_transport.cpp` — `seek(duration-1, true)` and `seek(last_pts + 1, true)` must yield a frame with `pts == last frame pts`.

**C2. Seek request and generation are not published atomically; a frame from the previous seek can be shown as the new seek's result — suspected (narrow window, real interleaving).**
* `seek()` bumps `generation` first (`media_source.cpp:79`), then stores `seek_request_ns` last (`:89`). The demux thread does `exchange(seek_request)` then `generation.load()` (`demux.cpp:41-44`) and tags all following packets with that generation.
* Two quick seeks (a scrub): seek2 bumps gen to N+1, demux then consumes seek1's still-pending target T1 and reads gen N+1, seeks to T1, flushes, and queues T1 packets **tagged N+1**. The decoder decodes them as current-generation frames (`video_decode.cpp:377`), they are not stale at `acquire`, and the preview takes the first one (`media_source.cpp:165-186`). When seek2's T2 store lands, demux seeks again but the generation does not change, so the decoder is **not flushed** between T1 and T2 packets (`video_decode.cpp:360-363`). For a keyframe (scrub) seek `exact_target_ns = -1`, so nothing filters T1's frames.
* Effect: after a scrub release while paused, preview can land on T1's frame and leave preview (`preview_ = false`), so T2's frame is never shown until the next action. Visible as "scrub landed on the wrong frame".
* **Fix:** make the request carry its generation: store `seek_target_ns` first, then publish `seek_request_gen` (atomic); demux exchanges the gen, then reads the target (a newer target with an older gen is harmless — the newer gen's request is still pending and its packets supersede). Tag packets with the *request's* gen, not `generation.load()`. **Test (missing):** a deterministic unit test needs a hook; at least an integration test that issues 50 back-to-back `seek(t_i,false)` then `pause` and asserts the shown pts is within one GOP of the last target.

### Medium

**C3. After a D3D11 device loss the playing clip is retired and not reopened — suspected (needs a run).**
* `rebuild_device` → `detach_device` bumps the job generation (`present_lab.cpp:332-347`; `abi.cpp:3473-3482`). `video_session::tick` then retires `current_` because `current_gen_ != generation` (`video_session.h:71-80`). `attach_device` re-uploads the still and re-opens an animation (`abi.cpp:3420-3466`) but has no video path. The canvas clears the stale frame after 0.35 s (`present_lab.cpp:892-903`). Unless the chrome reopens on the `VIDEO_STATE=stopped` completion (not checked), the user is left with an empty canvas and no error.
* **Fix:** in `attach_device`, if the selected item is a clip, resubmit the video open at the current generation with the last `position_ns` (resume store already has it). **Test:** `mv_tests` device-lost integration (there is a rebuild path for stills to copy).

**C4. Audio-master anchor ignores the audio already queued in the endpoint; video leads audio by up to one endpoint buffer after every seek/start — suspected.**
* The pump anchors when it hands the first block of a generation to the sink: `anchor_played = played_ns()` *now* (`av_clock.cpp:731-739`). But the WASAPI render thread has been filling the endpoint with silence while the FIFO was empty (`audio_win.cpp:599-607`, whole `buffer_frames_ - padding` each wake), so the block's first sample becomes audible only after that queued silence plays out. `master = pts + (played − anchor_played)` therefore advances from the moment of the write, ahead of the audible sample by the endpoint padding (shared-mode buffer, typically 10–30 ms), varying per seek.
* **Fix:** anchor in the sink at the moment the first sample of a generation is copied into the endpoint buffer (`render_loop` knows `frames_written_` and the padding at that instant): anchor_played = position of that sample = `frames_written_` at `GetBuffer` time converted to ns. Mac `audio_mac.cpp` needs the same check. **Measure:** a loopback/clapper clip (`av_transport.mp4`) recorded with an external mic, or compare `played_ns` vs `frames_written - padding` cross-check already computed at `audio_win.cpp:493-498`.

**C5. `avcodec_send_packet` returning `EAGAIN` drops the packet — suspected, low frequency.**
* `video_decode.cpp:379-383` only `continue`s on real errors; on `EAGAIN` it falls through to the receive loop and the packet is then discarded with the iteration. `EAGAIN` happens when the receive loop was left early with output pending — after a receive error (`:397-399` breaks without draining). Losing a packet corrupts decode to the next keyframe. **Fix:** keep the packet and resend after draining. Low risk.

**C6. Metal ring slot reuse is gated on "two newer frames acquired", not on GPU completion — suspected.**
* A replaced frame is released after two more *acquisitions* (`present_lab_mac.mm:2209-2221`), not two presents or a completed command buffer; the copy-out blits on a **different command queue** (`hwdecode_mac.mm:84,139`). At 30 fps on 60 Hz that is ~4 presents (safe in practice); at 1:1 cadence (60 fps on 60 Hz, 120 on 120 Hz) with three drawables in flight it is two presents, and cross-queue ordering is not guaranteed. Symptom would be an occasional torn/next frame. **Fix:** release on the render command buffer's `addCompletedHandler` (push to an SPSC ring the render thread drains), or `MTLSharedEvent` between the render queue and the copy queue. Design doc 05:84-85 already describes it as "two more presents".

### Low

**C7. Exact seek target never expires across non-exact seeks inside the same generation** — not a bug today because every seek rewrites `exact_target_ns` (`media_source.cpp:80`), but `step(+1)` relies on the ring rather than a seek (`:101-112`), so a forward step right after an exact seek works only because pts ≥ target. Fine; noted for whoever touches C1.

**C8. Demux `EAGAIN` busy-loops** (`demux.cpp:66` `continue` with no wait) — only for network/pipe inputs; local files never return it. Low.

**C9. Lossless trim: a picked track that has no packets after `end` keeps the loop reading to EOF** (`clip_copy.cpp:149-160` breaks only when every picked stream is done). Correct output, wasted I/O on long files with a short audio track. Low.

**C10. Lossless trim on a stream whose packets never carry `AV_PKT_FLAG_KEY`** (some TS/raw elementary streams): the probe grid is empty, `keyframe_nearest` returns `t` unchanged (`clip_common.cpp:336-337`), and `copy_segments` waits for a key packet that never comes (`clip_copy.cpp:136-138`) → output with audio and no video, reported as success. Suspected. **Fix:** fail `unsupported_format` when `keyframes_ns` is empty and the file has video. **Test:** a fixture with keyframe flags stripped.

**C11. `take_ready_video_frame` hard-codes a 60 Hz vblank** (`abi.cpp:3681-3686`). Unused today (no callers in `src/`), so dead code that would mis-pace on 120/144 Hz if anyone used it. Delete.

**C12. Teardown joins on the destroying thread in `~video_session`** (`video_session.h:19-24`: `close_media(current_)` joins demux/decode/audio). Fine at shutdown; would block the render thread if a session were destroyed there.

### Leaks / blocking checked and found fine
* Ring and decoder teardown order (`video_source.cpp:315-329`), retire on the cleaner thread (`video_session.h:130-137`, Mac `present_lab_mac.mm:877-900`), render-thread notify without locks (`wake_signal.h`), no render-thread waits on worker locks in `media_source` — all fine.
* EOF: end-of-stream null packets are queued for both decoders (`demux.cpp:68-73`), `video_done` is generation-checked (`video_decode.cpp:393`), and "ended" waits for the clock to reach duration (`media_source.cpp:198-201`). Fine. (A container that over-states duration plays a frozen last frame until that duration — by design.)

### Missing tests (would fail without the fixes above)
1. Exact seek to `duration − 1` and to `last_pts + 1` returns the last frame (C1).
2. Back-to-back keyframe seeks then pause land on the last target (C2).
3. Device-lost while a clip plays brings the clip back (C3).
4. Ring look-ahead: a decoder stalled for N ms (inject via a test `video_source`) produces 0 `held_starved` when N < look-ahead (P1, guards the ring size).
5. Trim on a no-keyframe-flag stream fails cleanly (C10).
6. No 4K clip in any perf harness run (section 0) — add `hevc_4k_8bit_bt709.mp4` to `tools/perf/regenerate.py`'s native `--soak` set, not only the screen compare.

---

## 3. Doc statements wrong vs code

| # | Doc | Says | Code |
|---|---|---|---|
| D1 | README.md:94 | "a worst gap of 636 ms" | `screen.json` `p99_ms 635.8`; the chart caption says 99th percentile. It is p99 (and implies ≥ 4 such gaps). |
| D2 | docs/design/05-video-pipeline.md:135-136 | sink position = "the render client's position minus queued frames" | `audio_win.cpp:446-454` uses `IAudioClock::GetPosition` alone and explicitly does **not** subtract padding. |
| D3 | docs/design/08-video-editing.md:21-22 | probe "uses the demuxer's index when complete (MP4/MOV…) and otherwise walks packets" | Always walks every packet (`clip_common.cpp:292-314`); no index code exists. |
| D4 | docs/design/05:137-138 | "present the frame whose PTS is closest to master + one vblank" | `choose()` shows the **oldest due** frame (pts ≤ target) and never shows a frame early; a frame 1 ms after target is held (`presenter.cpp:26-35`). |
| D5 | docs/design/05:84-85 | Metal "holds a presented frame for two more presents" | It holds it for two more *acquired frames* (`present_lab_mac.mm:2212-2221`), i.e. ~4 presents at 30p/60 Hz, 2 at 1:1 cadence; and the "6 slots" headroom leaves the same 2-frame look-ahead as D3D11. |
| D6 | docs/design/05:167-168 | scrubber drag seeks the "nearest keyframe" | `AVSEEK_FLAG_BACKWARD` = keyframe at or before (`demux.cpp:47`). |
| D7 | docs/design/05:84 / video_internal.h:181 | "3-4 is plenty" | Of the 4 D3D11 slots only 2 are look-ahead (one on screen, one reserved by the decoder); see P1. |
