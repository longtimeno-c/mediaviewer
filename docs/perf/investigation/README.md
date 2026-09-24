# RAW and playback investigation — 2026-09-24

Windows: Ryzen 7 5700X3D, RTX 4070, 59.95 Hz display, Release build in
`build-pr9`. These results are local measurements, not a hardware matrix.
The original reports are preserved in the parent directory. macOS source and
build changes are included, but no native Mac build or performance run was
available in this session. Apple Silicon and Intel both still need verification.

## Findings and changes

**Video selection used the wrong lifetime.** `player::choose` discarded a due
frame when it was more than one *display refresh* behind its target. On a 30 fps
clip that is about 16.7 ms, although a frame remains current for about 33.3 ms.
A slightly late wake could discard the only due frame, then hold the older
displayed frame because the following one was still in the future.

The shared presenter now drops a frame only when a newer queued frame is also
due. A bounded, consumer-only ring peek supplies the following PTS. This uses
actual timestamps for VFR and continues to discard superseded frames after a
stall or at high playback rates. It does not enlarge the A/V clock's target or
hide skipped frames. Regression tests include 120 seconds of jittered 30p
polling, variable durations, cadence, rates, starvation and catch-up.

**The old A/V harness did not display anything.** `--av-soak` creates a device,
decodes, selects frames and immediately releases them. Its 60 Hz `sleep_until`
loop is not a DXGI/Metal present loop. The CSV column named `presented` counts
selected frames, and `dropped` counts presenter discards. Thus the old 135/3,435
report did not establish 135 dropped screen presents. Its flat A/V error was
also constrained by the old discard threshold.

The diagnostic now starts at PTS zero regardless of saved resume position,
records its maximum polling gap, decoder errors and queue backpressure, emits
a final sample, and avoids a burst of synthetic ticks after a late wake. Both
native hosts also write `<pacing-report>.video.json`, separating frame selection
from the display-pacing statistics in the main report. Selection counters cover
startup; display pacing discards its usual one-second warmup.

**RAW processing was single-threaded.** The pinned LibRaw port has an `openmp`
feature that the manifests did not request. Both platform manifests now enable
it. The selected image can use four threads; concurrent decodes share at most
three extra workers, while Windows neighbour prefetch explicitly uses one.
PPG, full resolution, white balance, tone curve and brightness are unchanged.
All five real camera samples produced byte-identical serial/parallel pixels.

Windows uses LibRaw's MSVC OpenMP runtime, including when our code is compiled
by clang-cl, and copies the redistributable beside the binaries. Mac dynamic
triplets locate Homebrew libomp; the bundle tool already copies transitive
dylibs, and now carries the OpenMP notices too. The project must use the same
runtime as LibRaw for its thread limits to affect LibRaw's workers.

**RAW refinement uploaded the large image twice on Windows.** After the
embedded preview, it uploaded a full-size texture without mips, then uploaded
the full-size pixels again with mips. The original CR3 report records two
refinements and two fades, one with a missed display frame. Its render CPU time
was below 0.3 ms, so it does not establish blocking CPU decode on the render
thread; it lacks a GPU/ETW trace that would prove the exact cause of that miss.

For a RAW with a published preview, Windows now builds the full mipmapped
texture on the worker and publishes it once, keeping the preview visible.
No-preview files retain the early full-resolution path. A same-size
full-top-to-mip replacement also no longer starts a second fade after the first
has finished. macOS already uploaded the full image once; its host now uses
the same shared cross-fade curve instead of a hard preview/full swap.

## Measurements

The warm-buffer decode comparison (milliseconds, one pass per sample) is in
`raw-before.txt` and `raw-openmp.txt`. These measure decode and colour conversion,
not file read, UI startup, mip generation or GPU upload.

| Camera | Before | Parallel | Reduction |
|---|---:|---:|---:|
| Canon 7D II CR2 | 1044.8 | 873.8 | 16% |
| Nikon D7500 NEF | 1001.1 | 817.5 | 18% |
| Sony A7R III ARW, 42 MP | 1507.8 | 1090.6 | 28% |
| Canon R6 CR3 | 945.3 | 531.4 | 44% |
| Pentax K-50 DNG | 789.1 | 661.9 | 16% |

`regression-tests.txt` includes stage timings for the parallel path. CR2, NEF
and DNG still spend roughly 300–390 ms in unpacking alone; Sony spends about
750 ms in `dcraw_process` and 190 ms making/packing the output. **The <500 ms
full RAW decode gate remains open.** These changes do not claim that every RAW
opens faster end to end, or replace a full-quality decode with a preview.

| Playback run | Result | Scope |
|---|---|---|
| Original 120 s diagnostic | 135 discarded / 3,435 selected | Parent `av-drift-120s.csv`; last sample was at 119 s |
| Reproduced old 30 s diagnostic | 33 discarded / 836 selected | `av-before-30s.csv`; resumed at the stored position |
| Changed 120 s diagnostic | 2 discarded / 3,597 selected; no starvation, decode errors or surface waits | `av-after-120s.csv`; from zero, includes final sample; max poll gap 38.3 ms |
| Native Windows playback, 60 s | 0 display drops, 0 missed refreshes; 0 skipped video frames, 0 starvation | `video-native-60s.json` and companion; 3,597 measured display frames; p99 17.1 ms; A/V selection error p99 7.24 ms |

The native video report uses `--static` to disable the lab animation; its
`meets_pr1_gate` is false because continuous video intentionally fails the
*idle* gate. Assess its display and companion video counters, not that boolean.
Neither a 60/120 s run nor the end-of-run drift fit closes the 30-minute A/V gate.

Six chrome-on CR3 opens (`cr3-open-1.json` through `cr3-open-6.json`) had one
completed fade each and zero dropped fade frames. They are short loading
diagnostics, not idle or full PR 1 gates. They include application startup and
background prefetch; do not compare their first-pixel numbers with a no-chrome
run. Extra before/after no-chrome reports are retained, including experiments
before the foreground-worker priority adjustment.

## Reproduction and remaining checks

Windows, after building Release:

```powershell
ctest --test-dir build-pr9 -C Release --output-on-failure
./build-pr9/bin/Release/mv_tests.exe 'camera RAW: parallel decode*'
./build-pr9/bin/Release/mediaviewer_lab.exe tools/testmedia/soak_31min_1080p_hevc_aac.mp4 --av-soak 120 --csv av.csv
./build-pr9/bin/Release/mediaviewer_lab.exe tools/testmedia/soak_31min_1080p_hevc_aac.mp4 --soak 60 --static --json video.json
./build-pr9/bin/Release/mediaviewer_lab.exe tools/testmedia/raw/canon_eosr6.cr3 --soak 6 --static --json raw-open.json
./build-pr9/bin/Release/frametime.exe --seconds 60
```

For macOS, follow the updated `docs/DEVELOPMENT.md` recipe with `brew install
libomp` and `--overlay-triplets="$PWD/tools/mac/triplets"`. Run on **both** Apple
Silicon and Intel:

```sh
ctest --test-dir build --output-on-failure
./build/bin/mv_tests 'camera RAW: parallel decode*'
./build/bin/frametime --seconds 60 --lab ./build/bin/mediaviewer_lab
./build/bin/mediaviewer_lab --open tools/testmedia/soak_31min_1080p_hevc_aac.mp4 --soak 60 --static --json video.json
```

Also repeat RAW opens and pan during refinement on each Mac. Check the native
playback companion report, full-size image quality, cancellation while browsing,
steady idle after decoding settles, the bundled libomp load, and a long A/V run.
Actual Mac build/pacing, clean-machine packaging, broader refresh rates/codecs,
and the RAW latency target remain unverified or open.
