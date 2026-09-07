# 12 — Decision Log

What was decided, when, and what changed someone's mind. This exists so the *reasoning* outlives
the documents it was argued in — two of the calls below reversed an earlier draft of this plan, and
a reversal without its reason gets quietly re-reversed six months later.

Full trade-off tables live in [01-decisions.md](01-decisions.md#contested-decisions).

---

## 2026-09-06 — Plan drafted

Initial plan written: C++20, Win32 + DComp + Dear ImGui, libmpv for video, linear scRGB FP16
end-to-end, full photo editor, broad format coverage, phase-based roadmap with week numbers.

## 2026-09-06 — Reviewed against a competing draft (`grok-plan.md`)

A second independently-written plan for the same app was compared against this one. It has since
been deleted, its substance folded in here. What it changed:

| # | Decision | From | To | Why |
|---|---|---|---|---|
| **D4** | v1 scope | Full editor, curves/HSL/local/healing | **Viewer + light edits** | Same edit stack either way; deferring ops costs nothing architecturally and halves time-to-ship. The original was overreaching. |
| — | Roadmap shape | Phases with week numbers | **PR slices with verify lines** | Week numbers are fiction. A verify line is falsifiable. |
| — | Licensing | *Absent* | [11-licensing.md](11-licensing.md) | A genuine gap. "Static-link everything" is wrong for FFmpeg (LGPL), and **Exiv2 is GPL-2.0**, which silently decides the whole app's licence. |

## 2026-09-06 — Second review

A third pass, reviewing the merged plan. Four more reversals, one counter-proposal.

| # | Decision | From | To | Why |
|---|---|---|---|---|
| **D1** | App shell | Win32 + DComp + Dear ImGui | **C# WinUI 3 chrome + C++ core** | The original priced WinUI's composed frame and never priced the *chrome*. A daily viewer needs a virtualizing 10k filmstrip, accessibility, and IME; ImGui reads as a tool and FastStone users bounce. C++/WinRT XAML is a known velocity tax, so C# for the shell. Cost accepted: no VRR, no `ALLOW_TEARING`. |
| **D6** | Perf gate | 0 dropped frames at 144 Hz + VRR | **0 dropped frames panning a cached image at display refresh** | D1 makes the original unreachable. Setting a gate you've architecturally excluded is how a plan starts lying. |
| **D6** | Colour | Linear scRGB FP16 *including* the swapchain | **Linear FP16 working space, 8-bit sRGB swapchain** | Two separable decisions, conflated in the original. The working space can't be retrofitted; the swapchain format is one runtime branch. FP16 doubles present bandwidth for displays that are almost all SDR. |
| **D7** | Smart cut | v1 differentiator | **v1.1** | Join-artifact minefield. Ship two clearly-labelled paths first; add smart cut once there's a golden-file corpus to catch seams. |
| **D5** | Formats | Everything incl. EXR/PSD/SVG/DDS/VVC | **Camera-dump set** | JPEG/HEIC/RAW/H.264-HEVC MP4 is the job. The rest is collector behaviour delaying the moment a user can point this at a real camera dump. |
| **D2** | Video | libmpv (child HWND → GL interop) | **FFmpeg + D3D11VA on one present path** | See below — the only place the review's conclusion was accepted but its proposed mechanism wasn't. |

### D2 in detail — accepted the diagnosis, changed the prescription

The review argued for **Media Foundation + a dynamically-loaded FFmpeg fallback**, on the grounds
that child-HWND mpv is a second canvas (flicker on resize, your shaders don't apply), that
`WGL_NV_DX_interop2` is fragile on hybrid GPUs, and that subtitle layout / bitstream / motion
interpolation are movie-player features this app doesn't sell.

**All of that is correct, and mpv was dropped.** But MF was not adopted, for two reasons:

1. **It contradicts D3.** MF needs Store codec extensions for HEVC and AV1 — the exact formats an
   iPhone produces. Bundling libheif so the HEIC *photo* opens, then demanding a codec pack for the
   HEVC *video* beside it in the same camera dump, is incoherent.
2. **Once HEVC and AV1 route through FFmpeg anyway, MF is a second pipeline earning its keep on
   H.264 alone** — which is the same "two implementations of the hardest subsystem" objection the
   original plan raised against MF, still standing.

**FFmpeg + D3D11VA is one pipeline, one canvas, native D3D11 textures, no interop, and no codec
packs** — it satisfies every structural objection the review raised while staying consistent with
D3. The honest cost is that you write the A/V clock (audio-master, present against QPC, drop/dupe):
2–4 weeks, and the classic place to get subtly wrong.

Also corrected here: the original plan's claim that writing a player is "18 months." That was about
a *full* player with subtitle layout, HDR passthrough, and exotic containers. For play/pause/seek/
scrub on a folder of clips it is weeks, and overstating it was how mpv got waved through.

**`IMFMediaEngine` is retained as an escape hatch** behind `IVideoSource` if the clock work
overruns its PR 5b budget — the review's proposal, held in reserve rather than adopted, because a
fallback costs nothing and a second permanent pipeline costs forever.

## 2026-09-06 — Third review

A technical pass over the merged plan. Four **outright errors** found, plus five design holes and a
set of scope/estimate corrections. Nearly all accepted.

### Corrections to things the plan had wrong

| Area | Was | Now | Why it mattered |
|---|---|---|---|
| **Tone mapping** ([03](03-rendering.md)) | Tone-map everything for SDR output | **Display-referred sources get ICC → linear → sRGB, no tone map**; only scene-referred/HDR sources are tone-mapped | Tone-mapping a camera JPEG crushes highlights. This would have made every photo look worse than Explorer's preview while fighting golden images forever. Images now carry a `TransferIntent`. |
| **HDR video** ([05](05-video-pipeline.md)) | HLG/PQ → SDR mapping was v1.1 | **v1 correctness requirement** | iPhone dumps are full of HLG HEVC. Without it, PR 5's "4K 10-bit plays" verify passes green while the picture is visibly washed out — a test that certifies a bug. |
| **P010 sampling** ([05](05-video-pipeline.md)) | Bind luma R8 / chroma R8G8 | **NV12 is R8/R8G8; P010 is R16/R16G16** with the data in the high bits | That was NV12-only. The 10-bit verify would have failed looking like a colour bug. |
| **Decoder surfaces** ([05](05-video-pipeline.md)) | "Zero copy — just another texture" | **Copy into a presentation ring you own**; never present a decoder-pool surface | D3D11VA surfaces belong to the DPB. Holding them for present starves the decoder and hitches periodically, looking like a decode perf problem it isn't. |
| **x265** ([04](04-image-pipeline.md)) | Listed for HEIF decode | **libde265, or libheif's FFmpeg plugin** | x265 is a GPL *encoder*, directly contradicting D3. The FFmpeg plugin also avoids two HEVC stacks in one binary. |
| **Installed size** ([09](09-build-and-test.md)) | < 120 MB | **< 250 MB, self-contained .NET, published honestly** | Self-contained .NET alone is ~70 MB. The old target would have been discovered as a failure late, and framework-dependent trades it for a runtime prompt — the same mistake as a codec-pack prompt. |
| **Timeline** ([10](10-roadmap.md)) | "~3 months full-time to v1" | **Sized milestones, no promised calendar** | The original overconfidence surviving the scope cuts. PR 5b, PR 7, and PR 14 are each multi-week alone. |

### D1 amended — the shell is native, the chrome is hosted

The original had PR 3 port the canvas to `SwapChainPanel` and *test* whether it paced well enough,
with a native island as fallback. That was a science experiment with a foregone conclusion:
composition swapchains cannot take `ALLOW_TEARING`, a composed frame is inherited either way, and
`SwapChainPanel` resize/DPI is a known tax. **The fallback was always the destination.**

Inverted: PR 1's Win32 window and swapchain **are** the app; WinUI 3 chrome is hosted inside as
XAML content islands (`DesktopWindowXamlSource`). One present path, owned by C++, never
re-implemented. Cost accepted and written down: the entry point is C++ Win32, so C# is chrome
content rather than app host, and islands are a less-travelled path (popups, backdrop, focus
traversal need care). PR 3 validates islands early; if they fail, the fallback is now the *old*
plan.

### PR 5 split into 5a / 5b / 5c

As written it was FFmpeg + D3D11VA + WASAPI + clock + seek + transport in one PR — with the 2-4
week A/V clock buried inside it. Now: **5a** silent video into the swapchain (the architectural
win), **5b** audio and clock, **5c** transport. The reason is the escape hatch: with 5b isolated,
taking `IMFMediaEngine` is a considered decision with a clean boundary. Inside one large PR, the
same fallback is a panic merge under deadline pressure.

### Design holes now filled

| Hole | Resolution |
|---|---|
| **The ABI was named, not specified** | [14-abi.md](14-abi.md) — opaque handles + `SafeHandle` (an `IntPtr` the GC loses is leaked VRAM), ownership rules, `mv_status` + `mv_guard` so no exception crosses the line, a per-function thread contract, and **C# drains a completion queue** rather than C++ touching the dispatcher. |
| **[02] was still the ImGui app** | Module graph rebuilt post-D1: no C++ `ui/`, chrome is C#, `abi/` is the top of the native graph, nothing may depend on `shell/`. |
| **v1 RAW editing underspecified** | GPU demosaic is v1.1, so v1 must choose. **Chosen: wait for LibRaw's full decode before enabling the adjust pane** (~200-600 ms once), not edit the embedded preview. Editing pixels you will not export erodes trust in everything else the app reports. On PR 10's verify line. |
| **Live Photos / motion photos** | The actual object in a camera dump. **One item, not two** — pair on scan, still is primary, hold-to-play, video extractable, degrade to showing both if detection fails ([04](04-image-pipeline.md)). |
| **Hybrid GPU vanished after being used to reject mpv** | Adapter selection, decode-and-present on one device, and rebuild-on-adapter-change now specified in [03](03-rendering.md). It is now *your* problem on the path you chose. |
| **Explorer shell handlers** | Out-of-process `DllSurrogate`, timeouts, no shared decoder state. In-process, a malformed HEIC in a browsed folder takes down Explorer — a crash users cannot attribute to you and never forgive. A landmine, not polish. |
| **CI cannot see dropped frames** | Hosted runners have no usable GPU, so the D6 gate was unenforceable theatre. Self-hosted GPU runner for frame-time only, rolling baseline rather than an absolute threshold; corpus lives in an object store with a manifest, tiny licence-clean subset in CI. |
| **`IoRing` is Win11-only** | Platform floor stated: Windows 10 21H2, so `OVERLAPPED` is the real path. |

### Licence reframed as a product question

"What do we do about Exiv2" is a checklist item with no owner. The actual fork is **do you need the
Microsoft Store?** — Store matters → buy the Exiv2 licence and stay GPL-free; Store does not matter
→ GPL-2.0-or-later and every LGPL/GPL dependency becomes trivially compliant at once. Decided by a
person, before PR 1 merges.

### Small fixes

PR 2's verify used a 100 MP TIFF (TIFF is PR 7 — now a 60 MP PNG); the v1 vcpkg manifest listed
formats D5 defers (libjxl, openexr, openjpeg, resvg removed until v1.1); `atempo` is 0.5-2.0 per
instance so 0.25x/4x need a chain; and [07]'s "simpler than Lightroom" line now says the *structure*
is simpler while the kernels are not — so it stops arguing you back into writing them.

---

## 2026-09-06 — PR 1 implemented

### The licence decision, settled

The open question was framed as a product question with one input: **do we need the
Microsoft Store?** ([11-licensing.md](11-licensing.md)).

**Answer: no.** Therefore:

| | Decision |
|---|---|
| **App licence** | **GPL-2.0-or-later.** `LICENSE` is the GPL-2.0 text; every source file carries an SPDX identifier. |
| **Exiv2** | **Kept, used under the GPL.** No commercial licence bought, no replacement written. |
| **Consequence** | FFmpeg, libheif, libde265, LibRaw and Exiv2 are all trivially compliant at once, and any future GPL dependency is simply fine. |
| **Cost accepted** | Direct download only. Store MSIX is off the table under some readings of its terms; PR 15's "Store as a secondary channel" line no longer applies. |

The rules that do **not** relax because we went GPL, and are enforced by
`tools/licence-check.ps1` in CI:

- FFmpeg stays **LGPL-only** — no `--enable-gpl`, no `--enable-nonfree`. Our own licence
  does not make x264 and x265 acceptable, because the objection to them is **patent
  exposure**, not copyleft.
- **No software HEVC or AAC encoder** anywhere in the dependency graph.
- **No LibRaw GPL demosaic pack.**
- FFmpeg, libheif, libde265, LibRaw and Exiv2 are **dynamically linked**. LGPL requires a
  user be able to substitute their own build; that obligation is unaffected by our licence.

`THIRD-PARTY.md` lists what is linked today and what each later PR will add, with the
required linkage decided in advance rather than discovered afterwards.

### Two corrections to the plan, found by implementing it

| Doc | Said | Now | Why |
|---|---|---|---|
| [03-rendering.md](03-rendering.md) | Swapchain `Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB` | **Buffer `R8G8B8A8_UNORM`, render-target view `_SRGB`** | DXGI rejects every `_SRGB` format on a flip-model swapchain; the documented line is not creatable. **D6 is unchanged and fully honoured** — the hardware still does the linear-to-sRGB encode on write and the app still presents 8-bit sRGB. This is a mechanism correction, not a decision reversal. |
| [10-roadmap.md](10-roadmap.md) | PR 1 hosts Dear ImGui with the standard Win32 + DX11 backends | **DX11 backend only; the platform layer is fed from the published input snapshot** | ImGui's Win32 backend mutates `ImGuiIO` from inside the window procedure, which puts the UI thread inside the render thread's ImGui context — a data race, and a direct contradiction of [02](02-architecture.md)'s "the UI thread publishes, the render thread consumes, they never share a mutable object." Feeding ImGui from the snapshot costs about fifteen lines. `imgui[win32-binding]` is therefore **not** in the vcpkg manifest. |

### Design details settled while building

| Question | Resolution |
|---|---|
| **What counts as a dropped frame?** | `DXGI_FRAME_STATISTICS`, not QPC intervals. A present the compositor silently held for an extra vblank still looks like a clean interval from inside the app. The QPC path is kept as a labelled fallback, and `meets_pr1_gate()` **requires** the authoritative source — an inferred zero is not the D6 gate. |
| **`PresentCount` not advancing** | Not a fault. It advances when the compositor *displays* a frame, not when `Present()` returns, so a same-value sample is the common case. Treating it as a discontinuity made two thirds of a clean soak look like a measurement failure. The counters are cumulative, so nothing is lost by skipping such a sample. |
| **Warm-up** | The first second is discarded before measuring, and the report states that it was. DWM has not picked the window up and the first frame carries ImGui's font-atlas upload; measuring them reports a stall that is not in the thing being verified. Declared, not quietly trimmed. |
| **MMCSS on the render thread** | Registered as a `"Games"` multimedia task at `AVRT_PRIORITY_HIGH`. Thread priority alone does not stop the scheduler preempting a present loop on a machine doing anything else. |
| **`publish_slot` is a wait-free triple buffer** | The producer and consumer each own a slot and exchange through a third. Two slots permit overwrite during a read; a seqlock requires retries and can starve the render thread. The triple buffer has no retry loop. |

### Verify status after review (2026-09-07)

> **"Presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle."**

The original numbers are not proof of this gate. They used a fallback 60 Hz rate,
`SyncRefreshCount` instead of `PresentRefreshCount`, and did not require complete
measurement coverage. Low elapsed CPU-frame time alone cannot establish the cause of
a missed presentation. The parked-cursor bug also invalidated the general idle claim.
The revised instrument must be rerun; PR 2 remains dependent on a passing GPU gate.

### PR 1 review corrections (2026-09-07)

- Keep wait-before-render and `Present(1, 0)` for the composition path. The waitable
  object bounds queue depth; sync interval 1 controls display duration. The earlier
  plan/02 `Present(0)` example was not evidence of a double-vsync bug.
- Query the host monitor's active display path for the rational refresh rate. Unknown
  or ambiguous rates are zero and cannot pass the gate.
- Track `PresentCount` with `PresentRefreshCount`, retaining the last displayed
  baseline across duplicate polls. Missing statistics invalidate the entire window.
- Require full 60-second windows, refresh-matched cadence, and separate idle CPU and
  presentation counts. The operational tolerances are documented in the root README.
- Input snapshots carry cumulative wheel units and an activity sequence. Consumers
  take differences once; a parked cursor is not ongoing activity. Visible commands
  request redraws. Idle waits still honor the soak deadline.
- Resize failures restore the old RTV where possible and trigger recovery in the lab.
  A same-size request recreates a missing view. Close each frame-latency handle.
- Worker exceptions become error completions. Completion callbacks are called once;
  exceptions from them are logged and contained without retrying their side effects.
- Both soaks must pass before the existing animated report can become the baseline.
  CI uses immutable, uniquely keyed caches and never reruns a soak just to save it.
- Missing GPU-runner configuration produces a failed hosted check. It is not a pass
  or a silent skip. Runner provisioning and required branch checks remain repository
  administration tasks; the workflow does not invent a GPU.

Design-history notes from the implementation live here. The unconditional render-thread
join handles self-termination; testing `running_` before joining had left a joinable
thread at destruction. The sRGB buffer/view distinction and snapshot-fed ImGui platform
input are mechanism corrections, not changes to D1 or D6.

---

## 2026-09-07 — PR 2 started

PR 2 (still decode + pan/zoom) began from `pr1-present-lab` at the owner's request, with the D6 "0 dropped frames over 60 s" clause still **unproven** on the development box.

This is a sequencing call, not a reversal of D6:

- The present-loop verify is still inherited. No frame-time baseline is committed.
- The self-hosted GPU runner specified in [09](09-build-and-test.md) is still what closes the gate.
- Starting the decode/pan work does not lower the bar; it just stops the rest of the app waiting on a quiet machine that does not exist yet.

## 2026-09-07 — PR 1/PR 2 review (development box)

The corrected instrument was re-run. This is status, not a waiver.

| Soak | Result | Why it is not the gate |
|---|---|---|
| Animated 60 s | One run: 3597 frames, 0 drops, DXGI statistics complete, `meets_pr1_gate` true. An earlier run the same night: 4 dropped-frame events, 6 missed refreshes, 2 statistics gaps. | Repeatable on a quiet GPU runner, not a single pass on a noisy desk. |
| Idle 60 s | Process CPU ~0.2–0.5 % of one core when measured. Zero-presents was not established: the lab window received mouse input (53–60 activity events). A truncated 57 s sample with 0 presents was cut off before the window closed. | Idle is invalidated by any cursor in the client area. |
| Combined `frametime --seconds 60` | Fail (idle). | Both soaks must pass. |

`frametime.exe` opens a generated BMP on the animated soak so the blit path is paced. Idle stays empty so a still that lands after warmup cannot fail the zero-present gate. Unit tests cover colour and decode shape, not a 12 MP pan.

PR 2 review found constraints that were implicit and are now written into [02](02-architecture.md), [03](03-rendering.md), [04](04-image-pipeline.md), [09](09-build-and-test.md), and [14](14-abi.md). None reverse D1–D8:

- An idle renderer (rule 4: stop presenting) must be **woken** when a worker publishes a texture the canvas should show. Draining `MV_COMPLETION_IMAGE_OPENED` on the UI thread and logging it is not enough; the 500 ms input tail does not cover a 60 MP decode. Device-loss re-upload has the same requirement.
- CPU mip dimensions must match D3D11: `max(1, floor(prev/2))`. Ceil produces a chain `CreateTexture2D` cannot consume.
- LittleCMS on the decode pool needs a **per-job `cmsContext`**. The default/global context is not thread-safe.
- The ready GPU image is an SPSC/atomic handoff. The render thread must not take a mutex a worker holds.
- Device rebuild bumps the job generation (or equivalent) so in-flight `CreateTexture2D` against the old device cannot be published onto the new one.
- `CreateTexture2D` on a multithread-protected device still serializes with the immediate context. The ~2 ms upload budget in [03](03-rendering.md) applies to that path; "the worker did it" is not a pass around the hitch.
- A broken ICC profile is not a licence to treat tagged bytes as sRGB (**D6**). Fail the transform; do not fail-open.
- `mv_image_open` follows a generation bump. Opening without bumping replaces rather than cancels.

The current tree still has several of these as defects. Recording them here is so they are not re-discovered as taste.

## 2026-09-07 — Default-app prompt (PR 14)

Associations were specified as register + Default Apps deep link, never a silent hijack
([09](09-build-and-test.md), [10](10-roadmap.md)). That left becoming the default as
something the user had to discover in Windows Settings on their own.

**Added: ask once, after the first successful still open.** "Make MediaViewer your
default photo viewer?" Yes opens Default Apps focused on this app. No / dismiss is
remembered; Settings keeps the same action. Skip if already default.

Why this shape:

- Windows 10+ does not let an app write `UserChoice`. A prompt that claimed to "set
  default" without opening Settings would be a lie, and writing the key ourselves is
  the silent hijack the plan already forbids.
- Asking at install, or on an empty first launch, is a codec-pack-shaped nag in front
  of photos the user has not seen work yet. Asking after a successful still open is
  the moment the app has earned the question.
- Stills only. Taking `.mp4` / `.mov` in the same prompt would steal the existing
  video player by surprise. Video stays on "Open with" plus a Settings row.
- Do not stack with the telemetry first-run screen ([13](13-updates-and-telemetry.md)).

This does not reverse D1–D8. It is a PR 14 product call, not a new contested decision.

## 2026-09-07 — D9: macOS is Milestone F, not a UI port and not dual-track v1

The owner asked to add Mac support as a step on the plan, with the working assumption that
it is a UI update because the C++ core would keep working.

That assumption is half right. **Decode, colour, EditStack, metadata, canvas springs, and
the C ABI transfer.** Present, GPU backend, hardware decode, audio clock, async I/O,
directory watch, chrome, and the installer do not. A Windows DXGI soak is not a Mac pass.

Two rejected shapes, both written down so they are not re-proposed:

| Rejected | Why |
|---|---|
| **Mac in v1** (parallel SwiftUI / Metal from PR 4) | Slips every remaining Windows PR for a present lab the Windows user does not need. |
| **Mac as “just SwiftUI” after PR 15** | Ships a Mac app with no present-loop gate, no VideoToolbox path, and `AVPlayer` as the panic fallback — the Mac version of child-HWND mpv (D2). |

**Call: v1 stays Windows. From PR 4 the core is kept hostable. Mac is Milestone F
(PR 16–20)** — Metal present lab, stills on Metal, SwiftUI hosted in AppKit, VideoToolbox +
Core Audio, Finder + notarized Sparkle. Apple Silicon + macOS 14 only. Intel Macs and
Windows ARM64 wait.

This is not a reversal of D1–D8. It writes down D9, which was named in the index and
missing as a document. [15-platforms.md](15-platforms.md) is that document.
[01-decisions.md](01-decisions.md) and [10-roadmap.md](10-roadmap.md) now match it.

Do not implement Metal, Swift, or a `*_mac.cpp` during PRs 1–15. Do not skip a D9 port
in those PRs in order to call Win32 from `image/`, `player/`, `edit/`, or `meta/`.

## 2026-09-07 — PR 3 island shape, and what that forces on PR 4

PR 3 hosted WinUI 3 as a `DesktopWindowXamlSource` **top strip** on the native Win32
HWND (48 DIP command bar). The canvas is still the D3D11 swapchain. Flyouts use
`ShouldConstrainToRootBounds = false` so they are siblings of the swapchain, not
clipped by the strip. Chrome talks to the lab through a blittable command callback
(`chrome_command_fn`); it does not own the `mv_session` and does not drain completions.
`MediaViewer.Interop` exists and is unused by the island. Completions are drained on
the native UI thread in `main.cpp`, for logging.

That is enough chrome to put a filmstrip on, and it closes the "is the island even a
window we can host?" question. It does **not** close PR 3's verify line (zero dropped
frames with chrome on screen, tab traversal, flyout over canvas) nor PR 1's inherited
present-loop gate. Those stay inherited. PR 4 starts on top of this host because the
owner asked to, not because those gates are green.

What PR 4 must not do, given that host:

| Temptation | Why not |
|---|---|
| Grow the command-bar island over the full client | The island would eat canvas mouse-move (plan/02) and cover the swapchain. |
| One rectangular island with a "hole" for the canvas | `DesktopWindowXamlSource` is one rect. |
| `SwapChainPanel` for the photo | D1 amendment; PR 3 already refused this. |
| Marshal decoded frames / RGBA into C# `Image.Source` | plan/14: pixels do not cross the ABI. The *canvas* is never a XAML `Image`. |
| Draw 20 filmstrip thumbs onto the swapchain every frame | Makes filmstrip scroll a present-loop problem. PR 3 put chrome in XAML so scrolling chrome does not fight `Present`. |

**Calls for PR 4:**

1. **Second island, bottom strip**, same parent HWND, same hostfxr host. Top = command
   bar, bottom = filmstrip. `input_snapshot` gains `chrome_bottom_px`; `usable_canvas`
   subtracts both. Do not put the filmstrip in the command-bar island.
2. **`ItemsRepeater` lives in that island.** Virtualizing filmstrip is why D1 picked
   WinUI. Item chrome (selection ring, filename) is XAML. The main photo stays on the
   swapchain.
3. **Thumbnails that the island displays are JPEG files on disk**, spec `jpg512.1`,
   keyed in SQLite by `(path, mtime, size, spec)`. The ABI returns a UTF-8 path, not
   pixels. C# `BitmapImage` loads that file. This is a path, not a decoded frame.
4. **On-disk BC7 is deferred.** The roadmap named BC7 because it is the GPU-resident
   form (1/4 the VRAM, upload without recompression). A BC7 blob cannot be an
   `Image.Source`. Keep BC7 as the cache format if thumbs ever need to sit in VRAM
   (native overlay, Explorer handler in PR 14). Do not pull DirectXTex in PR 4.
5. **C# borrows the session and drains completions.** Pass `mv_session_t` into the
   filmstrip attach payload; C# `retain`s and wraps it in `SafeHandle`. Native stops
   draining when the island is attached — two drainers race. `--no-chrome` keeps the
   native drain. This is the plan/14 shape PR 3 postponed because the command bar
   did not need it.
6. **Warm arrow-key browse (< 40 ms) is a five-slot GPU LRU** of decoded textures
   (current ± 2), not a second `mv_image_open` that replaces the canvas. Prefetch
   jobs use the view generation; thumb jobs use a folder generation so arrow-key
   bumps do not cancel the filmstrip.

`IThumbnailCache` as a first-sight seed is still allowed later. It is not on the
verify line. Skip it in PR 4.

D9 starts here: `io/dir.h` is portable; `ReadDirectoryChangesW` lives in
`io/dir_win.cpp`. `tools/check-hostable-core.ps1` fails `d3d11.h` / `windows.h` /
`atlbase.h` in `core/`, `codec/`, `canvas/`, `image/`, `meta/`, `player/`, `edit/`.
`io/*.h` is in that net too; `*_win.cpp` under `io/` and `gfx/` is not.

## 2026-09-07 — Frame-time CI: skip when no GPU runner, do not fail-closed

PR 1 recorded: missing GPU-runner configuration produces a **failed hosted check**. It
is not a pass or a silent skip.

That fail-closed throw is what turned **Frame-time gate (self-hosted GPU)** red on
every PR, including this one, in ~16 s on `windows-latest`. No soak ran. The D6 gate
was no closer to proven; the rest of CI looked broken.

**Reversed, reporting only.** The job now has an `if:` and runs solely on
`[self-hosted, windows, gpu]` after `MV_GPU_RUNNER_ENABLED=true`. Unset, or a fork PR:
the job is skipped. Hosted Windows is not used as a fail vehicle.

This does not waive D6, does not measure on hosted runners, and does not invent a GPU.
A skip is not a pass. GitHub will treat that skip as success for merge if the check is
required; do not require it until a runner exists. The open item below is unchanged.

Why reverse the reporting:

- The PR 2 sequencing call already said work continues without waiting on a quiet
  machine that does not exist yet. Fail-closed CI contradicted that by blocking every
  later PR.
- "A green check that proves nothing is worse than no check" still holds. Skip is the
  no-check. A hosted failure that never ran `frametime.exe` was a red check that also
  proved nothing.

---

## 2026-09-07 — Keyboard-complete v1; remap is v1.1

The owner asked to make the Windows app fully usable without a mouse, with configurable
keybinds, and to take other speed-safe viewer features while ignoring Mac.

This is not a reversal of D1–D9. D1 already named FastStone keyboard/IME as the reason
chrome is WinUI. The roadmap already had "keyboard-only browse" on PR 6 and listed
keymap customization as v1.1. Those were named, not specified — the same hole [14] filled
for the ABI.

**Call:**

| | |
|---|---|
| **v1** | One command table, one key router on the UI thread, a FastStone-class default map, `?` overlay, `Ctrl+K` palette, focus that crosses islands. Every later PR registers commands into that table. |
| **v1.1** | Remap UI, import/export, alternate layouts (FastStone / IrfanView / vim). Same argument as batch metadata: do not build the editor before the defaults have been used. |
| **Where** | [16-commands.md](16-commands.md). PR 6 is the first cut and the mouse-free verify. |

Also written down, all speed-constrained (no decode on keydown, no second present path):

- **RAW+JPEG pairing** as one filmstrip stop (the DSLR equivalent of Live Photos). PR 7.
- **Companion hiding** (`.xmp`, `.thm`, `.aae`, voice memos, system files). Listing filter.
- **Folder tree** as a third island, left, hidden by default. Slips to PR 8 if PR 6 overruns.
- **Marks** (not Explorer multi-select) + `F7`/`F8` copy-to / move-to on the I/O thread.
- **Sticky zoom**, loupe, hold-previous (existing five-slot LRU), display-referred blinkies,
  pixel grid, checkerboard, always-on-top, on-canvas info, AF-point quads, one-pixel eyedropper.
- **Space** becomes next-image (play/pause on video). Lab sweep does not ship.

Rejected as v1, with reasons in 16: keymap editor, compare workspace, burst-stack heuristic,
print, card ingest, GPS map, PiP, peaking/zebras, catalog/albums, AI, slideshow crossfade.
Hold-previous is the cheap cousin of compare. Burst-stack waits because it can hide files.

Speed is the constraint, not a vibe: key-repeat next stays inside the generation counter;
copy/move never runs on the UI thread; blinkies are off by default because they animate a
present loop; pairing happens at scan.

## 2026-09-07 — Video decode workers may issue copies on the immediate context (PR 5a)

**Decision.** The video decode thread issues `CopySubresourceRegion` from the D3D11VA decoder pool
into our presentation ring **on the immediate context**, holding the FFmpeg
`AVHWDeviceContext` lock (`hwctx->lock(hwctx->lock_ctx)`, which defaults to the device's
`ID3D10Multithread`) for the duration of the submit and nothing else.

**Why this is a decision and not an implementation detail.** [02-architecture.md](02-architecture.md)'s
thread table says the decode pool never touches the immediate context. That rule was written for the
**still** path, where decode workers create immutable textures with `D3D11_SUBRESOURCE_DATA` and need
no context at all. A D3D11VA output surface cannot be copied without one, so the rule cannot be
applied literally to video. Read narrowly — video frame copy-out only — rather than reversed.

**Why not a deferred context, which would have kept the table literal.** An `AVFrame` from
`AV_HWDEVICE_TYPE_D3D11VA` is a *(pool texture, array slice index)* pair: `data[0]` is one texture
array shared by the whole DPB, `data[1]` is the slice. A COM reference on that texture therefore
reserves **nothing**. If a decode thread records the copy into a command list and releases the
`AVFrame` immediately, the decoder is free to reuse the slice and overwrite it before the render
thread calls `ExecuteCommandList`. The result is intermittent wrong-frame corruption that appears
only under DPB pressure — i.e. on exactly the 4K clips in PR 5a's verify line, and never in a short
test. The deferred context does not buy the early release it appears to buy; it hides the hold.

Using the immediate context is what makes the early release **correct**: FFmpeg's d3d11va decode
submits through an `ID3D11VideoContext` QI'd off that same immediate context, so D3D11's submission
ordering guarantees our copy precedes the decoder's next write to that slice. That guarantee exists
*only* while the copy goes to that same context. Moving it to a private or deferred context later to
"reduce contention" silently removes it.

**Constraints that come with the decision.** Copies only on that thread — no `Map`, no `Flush`, no
`ClearState`, no query wait, no GPU sync of any kind. `extra_hw_frames` covers the presentation ring
depth so a scheduling delay cannot starve the pool. Release the `AVFrame` immediately after submit.

**How it gets reversed.** This is measured, not assumed. PR 1's present-loop verify is the gate: if
p99 frame time regresses more than 10 %, or any frame exceeds 2× the refresh interval, the copy moves
to the render thread with `extra_hw_frames` raised to cover the queue instead. Frame times with and
without playback are reported as a comparison, not a pass/fail.

## Still open

| Question | Blocks | Notes |
|---|---|---|
| ~~**Do we need the Microsoft Store?**~~ | ~~PR 1~~ | **Closed 2026-09-06: no.** App is GPL-2.0-or-later, Exiv2 kept under the GPL, direct download only. See the PR 1 entry above. |
| **A quiet machine for the D6 gate** | PR 1 verify (inherited) | Re-run 2026-09-07: one animated pass, one animated fail, idle contaminated by mouse. Still needs the self-hosted GPU runner [09](09-build-and-test.md). |
| **PR 4's verify was never run** | PR 5 (inherited) | Three sessions held PR 4; the first hallucinated, the second committed `5eaa530` without reporting, the third confirmed it never owned the PR. Recorded state as of 2026-09-07: the 2000-JPEG scroll, the warm second-visit thumbnail check and the < 40 ms warm arrow-key number are **not run**; `tests/test_frametime.ps1` is **not run**; the plan edits in that commit to [10](10-roadmap.md) and [16](16-commands.md) are **unreviewed**. PR 5 is being built on top of this knowingly. |
| **Do WinUI 3 XAML islands hold up?** | PR 3 verify (inherited) | Command-bar island is in the tree. Filmstrip is a second island (PR 4). Present-loop + tab + flyout-over-canvas still unproven on a quiet GPU runner. Fallback unchanged: WinUI app with `SwapChainPanel` and an accepted composed frame. |

## How to use this file

Add a row when a decision changes, with the reason — not just the new value. If a decision here is
revisited and *upheld*, add that too; knowing an option was reconsidered and rejected again is worth
as much as the original call.
