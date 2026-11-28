# Stills path audit: performance and correctness

Read-only audit at `a232bda4` (branch `claude/project-audit-performance-180749`). Nothing was built or run. Every finding cites code I read. **Confirmed** means the behaviour follows directly from the code. **Suspected** means the mechanism is in the code but its size or effect is not measured.

## Context: where the gap is

| File (Windows) | bench `full` (decode only, 4 threads) | app `still_full_s` | app first pixel | bench preview |
|---|---:|---:|---:|---:|
| NEF D7500 | 638 ms | 1980 ms | 134 ms | 41 ms |
| ARW A7R III | 807 ms | 1376 ms | 60 ms | 6.5 ms |
| CR2 7D II | 713 ms | 1169 ms | 65 ms | 26 ms |
| **Mac M5** NEF | 299 ms | 420 ms | 50 ms | 26 ms |

Sources: `docs/perf/bench.json`, `docs/perf/first-pixel/*.json`, `docs/perf/mac/*`.

- On the Mac, the app adds about 120 ms to the decode. On Windows it adds 0.5–1.3 s.
- The bench rows measure decode only: no file read, no mips, no upload, no concurrent work (`tests/test_perf_bench.cpp:155-168`).
- The first-pixel harness opens a file in `tools/testmedia/raw`, which holds five RAWs, so neighbours are prefetched during every measurement.
- The NEF report shows 6 texture creates for one open, all over budget, the largest 215.6 MB at 42 ms.

Most of the Windows gap is explained by items P1, P2 and P6 below.

---

## Performance opportunities, ranked

### P1. Windows prefetches the neighbours alongside the selected open. The Mac waits.

**Status:** confirmed by reading; that this causes most of the gap is suspected.

**Where:**
- Windows submits the prefetch at the same time as the open:
  - `src/abi/abi.cpp:1855` (`apply_folder_list` → `submit_prefetch` right after `submit_decode_to_lru`)
  - `src/abi/abi.cpp:2922`, `:2931` (`mv_folder_select`)
- The Mac parks the prefetch until the selected image is fully up:
  - `src/shell/present_lab_mac.mm:327-353` (`prefetch` parks while `loading_item_` is the current item)
  - `:572` (`release_prefetch` after the full image is published)

**Cost:**
- Each of the four neighbours (`+1,-1,+2,-2`) runs:
  - a full 1-thread LibRaw develop (0.8–1.5 s; serial baseline in `raw.cpp:757-760`)
  - a serial Mitchell pyramid (P2)
  - a 100–215 MB `CreateTexture2D` that holds the `ID3D10Multithread` device lock for 20–42 ms
- All of that runs at the same time as the 6-thread foreground develop, on the same priority (P6).
- This matches the numbers: the Windows app is 1.3 s behind the bench on NEF, the Mac (which parks) about 0.12 s.

**Change:** port the Mac's park/release to `submit_prefetch`.
- Hold the neighbours until `publish_decoded` of the selected full image.
- For formats that have no full stage, release them on the first-pixel publish.

**Expected gain:** Windows `still_full_s` should drop to roughly decode + mips + one create, about 0.8–1.0 s on NEF (a guess).

**Risk:** neighbours are ready about 0.5–1 s later. A user who steps on within a second of landing gets more cold steps. Hand-off (P8) offsets this.

**Measure:**
- `python tools/perf/regenerate.py --only first-pixel,browse`, base and new alternated: `still_full_s`, `texture_creates`, `refine_fade_dropped`.
- Browse with a short dwell (see P8).

### P2. The CPU Mitchell mip pyramid runs on one thread

**Status:** confirmed by reading; cost not measured anywhere in the repo.

**Where:**
- `src/image/upload.cpp:153-226` (`downsample_half`) and `:276-284`.
- The Mac twin: `src/image/upload_mac.mm:116-183` and `:287-294`.
- `core::parallel_bands` exists (`src/core/parallel.h:42-86`) but is not used here.

**Cost:** level 1 of a 20 MP image is 5.2 M output pixels.
- The horizontal pass is about 167 M multiply-adds plus 125 M LUT gathers.
- The vertical pass is about 84 M multiply-adds.
- My estimate is 150–400 ms on one Zen 3 core for 20–42 MP, plus about a third for the deeper levels (guess).
- It runs for the selected still and for every prefetched neighbour.
- It also justifies the double upload for large JPEGs: top level first, then the full chain (`abi.cpp:1274-1285`).

**Change:**
- Split destination rows into bands with `parallel_bands`. Each band keeps its own four row slots and refills the up-to-3 source rows at its edge.
- The output is byte-identical, so `tests/test_upload.cpp` and the tile tests still hold.
- Once the pyramid is fast, consider removing the `mip_limit=1` staging, which saves a 96–170 MB create per large non-RAW still.
- The longer-term alternative is a compute-shader Mitchell on the GPU. That makes the texture non-IMMUTABLE and runs on the render thread's context, so it is higher risk.

**Expected gain:** 3–3.5× on this stage (`band_threads()` caps at 4), so roughly 100–300 ms off the 20–42 MP full stage (guess).

**Risk:** low.

**Measure:**
- Add a `[.perf-bench]` row "mips 42MP" (call `image::upload` with a null-device test seam, or `downsample_half` directly).
- `first-pixel` `still_full_s`.

### P3. The whole RAW file is read before the first pixel, and again for every thumbnail

**Status:** confirmed by reading. Not filed: #397 is about the tag pass's head reads, not decode reads.

**Where:**
- Windows open: `src/abi/abi.cpp:1194` (folder open), `:2184` (`mv_image_open`), `:1100` (hand-off preview).
- Windows thumbnails: `:893`.
- Mac open: `src/shell/present_lab_mac.mm:496-504`.
- Mac thumbnails: `src/shell/folder_model_mac.cpp:476`.
- The read functions: `src/io/file.cpp:19-77` and `src/io/file_mac.cpp:19-62`. Both zero-fill the buffer and then read the whole file.

**Cost:**
- `decode_raw_preview` only needs the TIFF/BMFF header plus the embedded JPEG.
- ARW: the bench preview is 6.5 ms, but the app's first pixel is 60 ms. The 41 MB read is a large part of that gap (suspected).
- From a camera card at about 90 MB/s, the 41 MB read alone is about 450 ms before any decode.
- The thumbnail sweep reads every RAW in full. A 2,000 × 40 MB dump means about 80 GB read to make 512 px thumbnails.

**Change:**
- Give LibRaw a read-only `LibRaw_abstract_datastream` over `pread`/`ReadFile` at offsets, through `open_datastream`, for the preview and thumbnail paths.
- Keep the whole-buffer read for the full develop. Start it right after the preview's header parse, or in parallel with it.
- The datastream has no write path, so rule 5 still holds.

**Expected gain:**
- Warm cache: about 10–30 ms off first pixel on a 20–40 MB RAW (guess).
- Cold cache or card: hundreds of ms off first pixel, and roughly 10–20× fewer bytes read by the thumbnail sweep.

**Risk:** medium. LibRaw's datastream API and its seeks need care. Errors on removable media must map to `io`.

**Measure:**
- `first-pixel` with a cold cache (Windows: RAMMap "Empty Standby List"; Mac: `sudo purge`).
- The same run from a USB card reader.
- Thumbnail sweep wall time on a folder of 200 or more RAWs.

### P4. At launch, the early open waits ~200 ms for the render device

**Status:** confirmed by reading; gain is a guess.

**Where:**
- `src/shell/main.cpp:14789-14794`: `lab.start` blocks the UI thread until the render thread has a device (`src/shell/present_lab.cpp:158-163`).
- `open_paths_early` only runs after that, at `:14823`.
- Windows boot medians (`docs/perf/boot/windows-arw.json`):

  | Stage | ms |
  |---|---:|
  | `window_created` | 33 |
  | `render_started` | 241 |
  | `first_pixel` | 323 |
  | `full_resolution` | 1192 |

  The file read and decode start about 210 ms after they could.

**Change:**
- Issue `open_paths_early` before `lab.start`.
- In the worker, wait (bounded, cancellable) on a device-attached condition only at the upload step, instead of skipping the upload when `copy_device()` is null (`abi.cpp:1204`, `:1251-1256`).
- Mac equivalent: start the open right after `_jobs.start()` rather than after the chrome and `_lab.start` (`src/shell/main_mac.mm:4930-4988`).

**Expected gain:**
- Windows first pixel at launch about 323 → 250–260 ms, bounded by device creation.
- Windows `full_resolution` about 1192 → 1000 ms (guess).
- Mac: tens of ms.

**Risk:** low to medium. The preview must not be lost if it is ready before the device.

**Measure:** `tools/perf/boot.ps1` / `boot.py`, alternated, 10 runs each.

### P5. RAW is always developed at full sensor resolution, even for a fit-to-window view

**Status:** confirmed by reading. The gain is a guess, and the change needs an owner decision.

**Where:** `src/codec/raw.cpp:574` (`p.half_size = 0`).

**Cost:**
- `dcraw_process` is 290–750 ms (`docs/perf/investigation/regression-tests.txt`).
- After it come the pack, the mips and a 4/3 × W × H × 4 byte texture.
- None of that full resolution is needed until the user zooms past about 50 %.

**Change:**
- Add a half-size develop stage (`half_size=1`: no demosaic, ¼ the pixels) published as the full-quality fit image.
- Run the full develop on zoom ≥ 50 % or when idle.

**Expected gain:** a 42 MP ARW "sharp at fit" in about 0.3–0.4 s instead of about 1.4 s (guess).

**Risk:**
- It adds a refinement stage.
- The colour must match the full develop (same pipeline, so it should).
- The investigation README's rule against replacing the full decode with a preview applies, so this needs the owner's call.

**Measure:** a `[raw-stages]` run with `half_size`, and `first-pixel` `still_full_s`.

### P6. Windows background jobs run at the same priority as the view, on up to N−1 workers

**Status:** confirmed by reading; the effect on the NEF outlier is suspected.

**Where:**
- `src/core/job_system_win.cpp:15-18`: every worker is below normal priority.
- `src/core/job_system_platform.h:27-29`: `enter_lane` does nothing on Windows. The Mac drops background jobs to UTILITY (`job_system_posix.cpp`).
- `src/core/job_system.cpp:118`: the background cap is N−1, which is 13 on a 16-thread machine.
- `abi.cpp:1839-1842` submits a thumbnail job for every item on each listing.

**Cost:** on a fresh folder, up to 13 background thumbnail jobs compete with the selected decode, its OpenMP team and its read, at equal priority:
- each one is a whole-file read (P3), a 2048 px or larger JPEG decode (P7), or a full 1-thread decode for formats with no preview

This is a likely contributor to the 134 ms NEF first-pixel outlier (suspected).

**Change:**
- Windows `enter_lane(true)` should call `SetThreadPriority(THREAD_MODE_BACKGROUND_BEGIN)`, which also lowers I/O and memory priority; `END` on the way back.
- Consider a smaller background cap for I/O-bound sweeps.

**Expected gain:** less first-pixel variance on a never-seen folder (guess).

**Risk:** low. Background mode must be ended before a foreground job runs on that thread.

**Measure:** `first-pixel` with the thumbnail cache cleared versus warm, 3 or more runs each.

### P7. RAW thumbnails decode the embedded JPEG at 2048 px or more

**Status:** confirmed by reading.

**Where:**
- `src/image/thumb_pixels.cpp:96` → `image::decode_preview` → `codec::decode_raw_preview` (`src/codec/raw.cpp:824-826`).
- That uses `default_options().preview_min_long_side = 2048` (`raw.cpp:783`) and `preview_scale` (`:364-372`).

**Cost:** a NEF's 2784 px preview is decoded at 1:1 and then box-filtered to 512 px. A 1/4 or 1/8 DCT decode would give 696 or 348 px directly.

**Change:**
- Add a preview entry that takes `min_long_side` (`kThumbLongEdge`) for thumbnails.
- Keep 2048 for the canvas.

**Expected gain:** 3–6× less JPEG work per RAW thumbnail (guess). Over a 2,000-RAW sweep that is tens of seconds of CPU.

**Risk:** low. The thumbnail output size is unchanged.

**Measure:** `mv_tests "[.perf-bench]"` with a thumbnail row added, and sweep wall time.

### P8. RAW neighbour work is thrown away on every step, on both hosts

**Status:** confirmed by reading.

**Where:**
- Windows disables hand-off while a neighbour RAW develops: `abi.cpp:1224-1233` together with `claim_decode`'s `handoff_ok` check at `:979`.
- Every navigation bumps the generation, so in-flight RAW neighbours are cancelled through LibRaw's progress callback (`raw.cpp:213-216`) and started again from zero.
- The Mac has no hand-off at all: `present_lab_mac.mm:1093` bumps, and the prefetch jobs at `:362-377` have no in-flight tracking.

**Cost:** at a dwell shorter than a 1-thread develop (about 0.8–1.5 s at 20 MP, more at 42 MP):
- no RAW neighbour ever completes
- every step is a cold decode

The browse harness uses a 3 s dwell (`docs/perf/browse.json` `dwell_s: 3.0`), so it never sees this.

**Change:**
- Allow hand-off once the develop has passed `LIBRAW_PROGRESS_INTERPOLATE`, using the progress stage that the callback already receives.
- Port `inflight_decode` / hand-off to the Mac.

**Expected gain:** at a 1 s dwell over 20 MP RAWs, more steps land on already-decoded neighbours (guess).

**Risk:** medium. The hand-off logic is subtle (#435).

**Measure:** browse soak with a 1.0 s dwell; count `cached` in the nav samples.

### P9. The viewer LRU evicts the selected image and +1 for stills over ~25 MP

**Status:** confirmed by reading.

**Where:**
- Windows: `src/abi/abi.cpp:730-765`. The budget is 512 MB, eviction takes the oldest first, and only stops at 3 entries.
- The prefetch order is `+1, -1, +2, -2` (`:1317`).

**Cost:** a 42 MP entry is about 226 MB. After the selected image (S) and four neighbours land in order, the LRU goes:

| After | LRU contents | Evicted |
|---|---|---|
| S, +1, −1, +2 | 4 entries, about 904 MB | S |
| then −2 | 4 entries again | +1 |
| final | −1, +2, −2 | — |

So the most likely next step (+1) and the step back (S) are cold decodes, and the work spent decoding +1 is wasted.

The Mac has the same shape on an 8 GB machine: `still_cache_budget()` is 512 MB there (`src/shell/present_lab_mac.mm:206-211`, eviction at `:315-319`).

**Change:**
- Evict the entry farthest from the selection first, and never S or ±1.
- Optionally size the budget from `device::video_memory_budget()`, which `docs/design/02` notes is read but unused.

**Expected gain:** arrow → next on a 45 MP dump becomes a cache hit (about 0.2 ms) instead of 1–2 s.

**Risk:** low.

**Measure:** browse soak on a folder of 5 or more copies of the 42 MP ARW. Add a unit test for the eviction order.

### P10. Previews build a full Mitchell pyramid before the first pixel (Mac, and Windows `mv_image_open`)

**Status:** confirmed by reading.

**Where:**
- Mac: `src/shell/present_lab_mac.mm:520` → `upload_mac.mm:282-294`. It always builds the full chain; there is no `mip_limit`.
- Windows: `src/abi/abi.cpp:2195` uses the default `mip_limit` 0.
- The Windows folder path already uses `mip_limit 1` for previews (`abi.cpp:1205`).

**Cost:** about 1.3 M output pixels of Mitchell for a 5 MP NEF preview, before the first pixel. On the Mac, NEF first pixel is 50 ms against a bench preview of 26 ms.

**Change:** previews go up with `mip_limit 1`, or a cheap 2×2 box chain. P2 also shrinks this.

**Expected gain:** 5–20 ms off first pixel (guess).

**Risk:** low. A preview sits at fit for less than a second.

**Measure:** Mac `first-pixel`.

### P11. Portrait RAW previews are rotated by a per-pixel scalar loop on the first-pixel path

**Status:** confirmed by reading.

**Where:** `src/codec/raw.cpp:334-362` (`apply_flip`, a 4-byte `memcpy` per pixel with strided reads), called at `:524`.

**Cost:**
- A 5 MP gather copy on every vertical shot.
- All the bench samples are landscape, so the published numbers do not include it.

**Change:** a blocked transpose (64×64 tiles), or carry the orientation into the blit's UV transform.

**Expected gain:** 10–20 ms off portrait first pixel (guess).

**Risk:** low. `tests/test_raw.cpp` covers flips.

**Measure:** add a portrait RAW to `MV_BENCH_DIR`.

### P12. RAW RGB→RGBA expansion is serial, and large buffers are zero-filled first

**Status:** confirmed by reading; cost is a guess.

**Where:**
- `src/codec/raw.cpp:642-665`: a `resize` (zero-fill of 170 MB at 42 MP), then a serial loop.
- `src/image/upload.cpp:160` and `src/io/file.cpp:56` / `file_mac.cpp:46`: also zero-fill with `resize`/`assign`.

**Cost:** an extra write pass over every large buffer, plus the serial pack. The committed `regression-tests.txt` predates the `mem` / `pack` split, so the pack's own cost is not on record.

**Change:** use `parallel_bands` for the pack, and default-initialised buffers.

**Expected gain:** 20–60 ms at 42 MP (guess).

**Risk:** low.

**Measure:** the `[raw-stages]` line, which already prints `mem` and `pack` separately (`tests/test_raw.cpp:808`).

### P13. A camera JPEG's first pixel costs almost a full decode; its embedded MPF preview is ignored

**Status:** confirmed by reading; gain is a guess.

**Where:**
- `src/image/pipeline.cpp:50-51` (`decode_jpeg_display(.., 4)`).
- Bench (synthetic, high entropy): "preview 24MP" 166 ms against "full 24MP" 178 ms on Windows. Huffman decoding is not reduced by DCT scaling.
- The codec has no MPF parser. `src/meta/write.cpp:161` only detects that MPF is present.

**Change:** when an APP2 MPF secondary image is present (most Canon, Nikon and Sony JPEGs carry a ~2 MP one), decode that as the first pixel, the same way as a RAW's embedded JPEG.

**Expected gain:** camera JPEG first pixel of about 10–20 ms (guess).

**Risk:** low to medium. The MPF offsets must be bounds-checked like `raw.cpp` does.

**Measure:** real camera JPEGs in `MV_BENCH_DIR`.

### P14. A neighbour's large texture create may land during the preview→full cross-fade

**Status:** suspected cause of the README's NEF dropped frame.

**Where:**
- `src/shell/present_lab.cpp:1335-1340`: `set_canvas_moving(wants_frame && camera_.moving())`. The comment says fades do not hold the gate.
- The gate itself: `src/abi/abi.cpp:697-710`.
- The NEF report shows `texture_max_create_ms 42.5`, more than two refresh intervals at 60 Hz.

**Change:** also hold the gate while a refine fade is active. A fade lasts 80–250 ms, so the cost is small. P1 removes most of the overlap anyway.

**Measure:** 20 alternated NEF opens; `refine_fade_dropped`.

### Smaller notes

- **Shared Metal textures.** Mac stills use `MTLStorageModeShared` on Apple Silicon (`upload_mac.mm:305`). Shared textures cannot use lossless compression, so the sampling cost during pan or zoom of large stills may be higher. Suspected and unmeasured; check with `--pan-soak` on the Mac if one is added.
- **Mac reads cancelled loads.** The Mac open reads the whole file before its first cancellation check (`present_lab_mac.mm:496-504`), so a load cancelled by fast stepping still pays for the read.

---

## Issues by severity

### High

**C1. A stale preview or full image can overwrite a newer LRU-hit navigation, and the canvas then stays on the wrong photo.**

Status: confirmed by reading; not reproduced.

- **Where:**
  - In `submit_decode_to_lru`, the selection check happens outside `image_mutex`, then the publish runs with no generation check. This happens at three places:
    - `src/abi/abi.cpp:1209-1211` (preview)
    - `:1263-1264` (full)
    - `:1173-1174` (remote tile)
  - `publish_ready` (`:525-530`) and `publish_decoded` (`:1017-1023`) do not compare `ctx.gen()` with the current generation.
  - `publish_locked` stamps `view_generation = current_generation()` (`:508`), which is the *new* generation.
- **Sequence:**
  1. A worker passes `path_is_selected(A)`.
  2. The user steps to B, which is an LRU hit: `mv_folder_select` bumps the generation and `lru_publish(B)` publishes synchronously (`:2908`).
  3. The worker publishes A, stamped with B's generation.
  4. The render thread classifies it as navigation and shows A.
  5. A's job is now cancelled and B is never republished, so the canvas shows A while the selection is B.
- **Fix:** do the check under `image_mutex` with `ctx.gen() == current_generation()`, the way `publish_view` (`:516-523`) and the hand-off preview (`:1112-1117`) already do. `ctx.gen()` follows hand-offs, so a neighbour the user lands on still publishes.
- **Test:** a seam between the check and the publish (or a direct call to `publish_*` with a stale context), asserting that `ready` is unchanged.

### Medium

**C2. Windows LRU entries are keyed by path only, so an externally edited file shows stale pixels on revisit.**

Status: confirmed by reading.

- **Where:**
  - `lru_slot` has no mtime or size (`abi.cpp:241-245`), and `lru_publish` matches on path (`:767-793`).
  - The only invalidation is `mv_folder_forget`, which runs only after the app's own edit jobs (`src/shell/main.cpp:2744`).
  - The watcher's relist (`apply_folder_list`) does not touch the LRU.
  - The Mac checks the stamp (`present_lab_mac.mm:283`).
- **Fix:** store `(mtime, size)` in the slot and compare against `folder_items`, or prune on relist when the stamp changed.
- **Test:** an ABI test that rewrites a file in the folder, relists, selects it, and expects a decode rather than an LRU hit.

**C3. SQLite thumbnail-store open runs on the UI thread (rule 1).**

Status: confirmed by reading.

- **Where:**
  - `mv_folder_open` and `mv_folder_open_list` call `session->thumbs.open(...)` inline (`abi.cpp:2307-2309`, `:2395-2397`).
  - Inline means on the host's UI thread (`main.cpp:1224` → `open_folder`).
  - `thumb_store::open` (`src/image/thumb_store.cpp:162-210`) opens the database, sets WAL and creates the schema while holding the mutex that pool lookups use. On the first open after a crash, WAL recovery happens here.
- **Fix:** open the store inside the listing job, which already runs on a worker.

**C4. Background thumbnail decodes cannot be cancelled mid-decode.**

Status: confirmed by reading.

- **Where:**
  - `job_context::cancelled()` always returns false for generation 0 (`src/core/job_system.h:50-54`).
  - `make_thumb_jpeg(bytes, &ctx)` (`abi.cpp:898`) and the full 1-thread decode fallback (`thumb_pixels.cpp:105`) therefore run to completion.
  - `folder_generation` is only checked between steps (`abi.cpp:825`, `:895`).
- **Effect:** leaving a folder of large PNG, TIFF or AVIF files keeps up to N−1 workers decoding dead work for seconds. Meanwhile the next folder's selected decode and prefetch share the one reserved worker.
- **Fix:** pass a context whose `cancelled()` also watches `folder_generation`, via something like `handed_off_via` or a second atomic.

**C5. Peak memory during RAW open with concurrent prefetch.**

Status: suspected magnitude.

- **Per 42 MP job:**

  | Buffer | Size |
  |---|---:|
  | File bytes | 41 MB |
  | LibRaw 16-bit 4-channel working image | ~340 MB |
  | RGBA | 170 MB |
  | Mips | 57 MB |

- Five jobs at once (P1) means a transient peak of about 2–3 GB.
- The file bytes are also kept alive until the end of the job, through mips and upload:
  - `abi.cpp:1194` → `:1286`
  - `present_lab_mac.mm:502` → `:584`
- **Fix:** P1, plus release the bytes after `decode_bytes` unless the format family can be animated.

### Low

**C6. Windows OpenMP team threads probably run above the worker's priority.**

Status: suspected.

- Workers are below normal (`job_system_win.cpp:17`). LibRaw's vcomp team threads are created by the runtime, and Windows threads do not inherit priority, so the team probably runs at normal.
- That contradicts "a decode must never outrank the frame it feeds" for the UI thread. The render thread is above normal plus MMCSS, so it is not affected.
- **Verify** with an ETW CPU-sample trace during a RAW open.

**C7. Mac `read_prefix` treats `EINTR` as an I/O error.**

Status: confirmed by reading. `src/io/file_mac.cpp:51-55`: `n <= 0` is treated as `io` with no retry.

### Missing tests

- The viewer LRU's eviction order and byte budget, on both hosts. No test references `lru_byte_budget` or `still_cache_budget`.
- Publish after navigation (C1).
- An LRU stamp check (C2).
- Performance coverage:
  - `[.perf-bench]` has no row for the mip pyramid, upload or file read, which is why the app-versus-bench gap is invisible in `bench.json`.
  - Browse is only measured at a 3 s dwell, which hides P8.

---

## Doc statements that are wrong against the code

1. **`docs/design/02-architecture.md:198-205`.**
   - The doc says that when the user lands on a neighbour whose prefetch is running, *"the ABI hands that job to the new generation … instead of cancelling and restarting it."*
   - Not for a RAW neighbour mid-develop: hand-off is disabled during the develop and a fresh decode starts (`abi.cpp:1224-1233`).
   - Not on the Mac at all: `present_lab_mac.mm:1093` bumps the generation and abandons the prefetch, and there is no in-flight tracking.
2. **`docs/design/04-image-pipeline.md:124-126`, step 4.** The doc says the top level is published first, then the mip chain, for stills of 2048 or more without a preview.
   - This is Windows-only: Mac `upload` has no `mip_limit` and always publishes the full chain (`upload_mac.mm:267-318`, `present_lab_mac.mm:548`).
   - Windows `mv_image_open` also gives the *preview* a full chain (`abi.cpp:2195`).
3. **`docs/design/04-image-pipeline.md:183-189`, Prefetch.** It describes one behaviour for both hosts.
   - Windows starts the prefetch at the same time as the open (`abi.cpp:1855`, `:2922`, `:2931`).
   - The Mac parks it until the full image is up (`present_lab_mac.mm:327-353`, `:572`).
   - The difference is what P1 is about and should be documented.
4. **`docs/design/02-architecture.md:91`.** The doc says the UI thread *"never block[s] on I/O … or a lock held by a worker."*
   - `thumb_store::open` runs on the UI thread under the store's worker-shared mutex (C3).
   - The launch wait for the device is documented separately at `:115-117`, so it is not a contradiction.
5. **`docs/design/02-architecture.md:211` and `04:199-201`.** They give the LRU as "512 MB … 3–12 entries".
   - With large stills it holds 3 entries whatever their size: 3 × 226 MB = 678 MB at 42 MP (`abi.cpp:760-761`).
   - So the byte figure is a soft target, not a cap.
6. **`src/image/pipeline_mac.h:4-7`.** The comment says this is *not* `decode_bytes` and is JPEG/PNG/BMP only. `pipeline_mac.cpp:16-18` calls `decode_bytes` with the full format set. This is code-comment drift of the kind #621 tracks.

## Already filed (skipped)

- #574: prefetch with wrap-around and shuffle.
- #583: disk thumbnail as first pixel for PNG, TIFF and AVIF.
- #397: 20k-folder first pixel and head reads.
- #576: UI-thread path probe on recent and OneDrive opens.
- #584: Mac stills above 16384 px.
- #573: eyedropper on tiled stills.
