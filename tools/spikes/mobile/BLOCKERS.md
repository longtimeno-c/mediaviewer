# M1: what the spikes worked around, and what the real port needs

Throwaway notes for M3–M5 of `docs/plans/mobile.md`. Checked against `1facf73a`. The spike links
none of `src/`, so everything below is what the spike **avoided**, not something it fixed.

## ONNX Runtime

| Finding | Evidence | What the real port needs |
|---|---|---|
| **Microsoft's Swift package cannot load the shipping text tower.** Its newest tag is 1.24.2, and session creation fails on `clip-l14/text.onnx` (fp16) with optimisation at `ORT_ENABLE_ALL`, the desktop's setting. Lower levels were not tried. | Simulator, 2026-11-10: `graph_utils.cc:30 … InsertedPrecisionFreeCast_/text_model/final_layer_norm/Constant_output_0 for node …/SimplifiedLayerNormFusion/` | Do not take the Swift package. Link the pod archive at the desktop's pin instead. |
| **`pod-archive-onnxruntime-c-1.30.0.zip` loads and runs it.** It is the desktop's version (`cmake/ai.cmake`), has a static `ios-arm64` framework and the C API. | The spike links it through `app/OrtBinary/Package.swift`, SHA-256 pinned. | `cmake/ios.cmake` takes this archive. Keep its version tied to `MV_ORT_VERSION`, or the phone and desktop can differ in numbers. |
| `infer` loads ONNX Runtime with `dlopen` (`src/infer/ort_dylib_posix.cpp`), and `ort.cpp:177` gets `OrtGetApiBase` as a symbol. iOS allows no downloaded code. | Read, not compiled for iOS. | A third implementation of `mv::infer::dylib` (`open`, `symbol`, `close`, `library_name`) for iOS: `open` returns a non-null marker and `symbol` returns `&OrtGetApiBase` for that one name. `ort.cpp` then needs no change. |
| Static ONNX Runtime costs about 37 MB of executable. | Device build, unsigned: `MobileSpikes` 37,483,952 bytes, arm64; the app 36 MB on disk. | Plan's app-size budget. The archive is the full build; a reduced-ops build is the lever, and needs its own check that the tower still loads. |
| The session settings match the desktop's (`ort.cpp:247-252`): intra-op threads, inter-op 1, `ORT_ENABLE_ALL`, no spinning. | `ort_bridge.cpp` | Nothing. |
| `text.onnx` has one input, `input_ids`, n × 77, and `text_embeds` comes out unnormalised (norm 19.07). | Simulator | The desktop normalises afterwards; the phone must too. |

## Memory: read this before the phone numbers

On the simulator the **load peak of the L/14 text tower is about 670 MB, and it settles at about 485 MB**
(`mb_peak_after_load`, `mb_after_load`). The simulator is a Mac and has no jetsam, so this says nothing
about whether a phone survives, but the allocations are the same code. §9 of the plan budgets 248 MB
for the model, and §13 targets 400 MB for the whole app at 100k frames. **If the phone shows numbers
like these, the budget fails before any vectors are loaded.** Levers to try, in order:

1. ONNX Runtime's `.ort` format with `session.use_ort_model_bytes_directly`, so the weights stay in a
   mapped file. Not tried: it needs a conversion step and a parity check.
2. `com.apple.developer.kernel.increased-memory-limit`. Not tried: it needs a paid team's provisioning.
3. Disabling the CPU memory arena (`DisableCpuMemArena`). Not tried. May trade the peak for encode time.

## Other code that will not build for iOS

| Code | Problem | Real fix |
|---|---|---|
| `src/addons/ai/platform_posix.cpp:12-13`, power queries at `:34-47`, the battery guess at `:96` | `IOKit/ps/*.h` is not in the iOS SDK (`xcrun --sdk iphoneos clang` confirms: file not found) | Guard the IOKit half with `TARGET_OS_OSX`. On iOS, take power state from `ProcessInfo` / `UIDevice`, through the host, since a phone has its own rules for when to work. |
| `ocr`, `fetch`, `photos`, `cloud` in `addons/ai` | Have `_none` variants on other platforms | Select the `_none` files in `cmake/ios.cmake`. Not tried here. |
| `addons/driftwire`: `net_posix` gateway lookup, `machine_mac`, `secrets_mac`, `power_mac` | Plan §8.1 | iOS versions or stubs. Not tried here. |

## The scan

`src/addons/ai/vectors.cpp:18` has no NEON path; it relies on the compiler vectorising four int32
accumulators. `app/MobileSpikes/scan_kernel.c` carries that loop verbatim beside two written with
intrinsics. On this Mac's simulator (Apple silicon, 1M rows, one thread, p50 of 7 passes):

| Kernel | ms per pass |
|---|---|
| The desktop's loop | 188 |
| NEON widening (`vmull_s8`, `vpadalq_s16`) | 64 |
| `vdotq_s32` | 33 |

**This is a Mac, so only the ratio is evidence, and only until S3 runs on a phone.** If the phone
agrees, the fix is small and helps the Mac too: a NEON `dot_i8` with `FEAT_DotProd` chosen at run
time. Changing the desktop's scan is a performance change under the repo's rules and needs its own
before-and-after.

## Driftwire on the NAS

- **A manual port forward cannot reach today's `driftwire`.** `internet.cpp:249` listens on a port
  the OS picks and asks the router to map it, so there is no port to forward by hand. M2b's
  `driftwire serve` needs a fixed, configured port. Until then the only fixed-port Driftwire
  program is the relay (47614).
- **No range read exists.** `driftwire` moves whole files. `nas/range_server.py` measures the path
  only: it has none of the link's handshake, sealing or framing.
- **A relayed transfer does not move or resume** (`28-driftwire.md`, Not built), so reconnect through
  the forward cannot be tested today.
- **The relay needs the router to hairpin** if the receiver is inside the network, since a code
  carries the relay's resolved address (`invite.h`, `relay_place`).
- **A NAS has no secret store** (`secrets_linux.cpp:6-7`): the identity lasts one run and nothing is
  remembered. M2b needs a key file of its own.
- **`driftwire` refuses to run without a terminal**, and nothing may auto-accept. A service is new code.
- The Linux package targets glibc 2.35 or newer. **Whether DSM meets that is unknown** until
  `nas/probe-synology.sh` has run on the real NAS. `mv-driftwire-relay` is not in the package and is
  linked with `-static-libstdc++` only by `nas/build-linux.sh`.

## AVPlayer over a resource loader

- AVFoundation asks for the **whole file** in one request (`requestsAllDataToEndOfResource`) and
  cancels when it has enough. The real loader must stream in chunks and honour cancellation, or
  every open reads the file. The spike does, and it still over-reads: on the simulator a 68 MB clip, played
  for 60 s and seeked 7 times, cost 164 MB from a loopback HTTP source and 385 MB from a file, since each
  seek starts a new read to the end and a fast source gets far before it is cancelled. On a real link the
  speed of the link bounds this; `bytes_served` will show by how much.
- `getaddrinfo("ipv4only.arpa", AF_INET6)` succeeded on this Mac's plain network, so DNS64 detection must
  check the synthesised address (RFC 7050: 192.0.0.170 or .171 in the last 32 bits), as `net_probe.c` now does.
- A seek may go unanswered. A simulator run hung at its last seek, issued after playback had run off
  the end (the seek's completion looks like the part that never came; not proven). The spike now
  bounds each seek at 20 s and reports `timed_out`, sets `actionAtItemEnd = .none` and calls `play()`
  after each seek, and has not hung since. Keep a bound in the real player.
- An `AVPlayerLayer` that is not in a window never reports `isReadyForDisplay`, and a reused layer
  reports it once. A first-frame clock must allow for both.
