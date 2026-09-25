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

## 2026-09-24 — Default-app: installer registers, video included

Amends the 2026-09-07 entry above on two points, at the owner's request.

- **Windows wizard registers associations and offers Default Apps.** `mediaviewer.iss`
  writes per-user `ProgId` / `OpenWithProgids` / `RegisteredApplications` keys for the D5
  stills and video, and the Finish page has an **unticked** "Choose MediaViewer as the
  default" that opens Settings > Default apps. Still no `UserChoice` write; the in-app ask
  stays. Uninstall removes every key.
- **Video is in scope** for registration and the default prompt (Windows and macOS). macOS
  gains a `Video` document type (Alternate, Viewer); the existing prompt reads Info.plist,
  so it covers video with no other change. Quick Look thumbnails stay stills-only.

Unchanged: nothing is taken silently, and the OS confirms the choice.

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

## 2026-09-13 — First install is a short wizard; updates stay silent

Not a D1–D9 reversal. The channel is still a signed per-user install with our own updater
([13](13-updates-and-telemetry.md)). Store MSIX remains off (GPL). What was unspecified
was the **first-run setup UX**: `plan/09` said Inno or WiX, PR 15 said Velopack, and
neither named an icon or a GitHub link.

**Call:**

| | |
|---|---|
| **First install** | Inno Setup wizard, once. Welcome, GPL accept, LocalAppData location, Start Menu on / desktop off, progress, finish. |
| **Updates** | Velopack, silent. Versioned folders, signed manifest, rollback. Never re-open the wizard. |
| **Not in the wizard** | Default-app (PR 14, after first successful still), telemetry (in-app first-run, default off). |
| **Icon** | One `.ico` (16–256) for wizard, Start, window, taskbar, and still `ProgId` `DefaultIcon`. Lands with PR 14 because Explorer needs it; PR 15 reuses it. |
| **GitHub** | Finish-page link and About. `https://github.com/longtimeno-c/mediaviewer`. Do not auto-open. |
| **About** | Version, GPL, GitHub, `THIRD-PARTY.md`, per-build LGPL source offer — PR 15. |

Why a wizard rather than Velopack's one-shot setup.exe: first install is the only time a
stranger meets the app, and a licence page + branded icon + Launch / GitHub / Licence
finish is the clean experience. Why not WiX or a custom WinUI installer: WiX is MSI and
fights per-user silent updates; a second GUI installer is another present path. Why not
default-app or telemetry in the wizard: both are in-app, once, after the user has a
reason to answer — and Windows will not let us write `UserChoice` anyway.

## 2026-09-13 — Start PR 16 in parallel with Windows v1

The owner asked to begin the macOS app (named as PR 16) while other agents continue
PR 6 on Windows, on the grounds that the two will not clash.

This is a **sequencing exception**, not a reversal of **D9**.

| | Stands | Relaxed |
|---|---|---|
| **D9 product call** | v1 is Windows. Mac is a later host of the same core, not a UI port, not Qt/Flutter/Catalyst, not `AVPlayer`. Apple Silicon + macOS 14 only. | |
| **"Do not start F until PR 15"** | | Relaxed for **PR 16 only**, because multiple agents can take the Metal present lab without pausing Windows PRs. That was the original objection to "Mac in v1" ([12](#2026-09-07--d9-macos-is-milestone-f-not-a-ui-port-and-not-dual-track-v1)). |
| **PR 16 scope** | AppKit + `CAMetalLayer` + `CAMetalDisplayLink` + F3 + `frametime` on Darwin. **No SwiftUI.** | |
| **PRs 17–20** | Still after the Windows features they host have landed in the core, and still not a dual-track of PRs 4–15. | |

What this is not:

- Not permission to implement VideoToolbox, Core Audio, SwiftUI chrome, Finder
  UTIs, or Sparkle "while we're here." Those are PR 17–20.
- Not permission to treat a Windows DXGI soak as the Mac gate.
- Not a UI-only port. Chrome is written twice; present / hwdecode / audio / I/O
  are backends.

The Windows v1 surface that must exist on Mac — not as empty `*_mac.cpp` now, as
real host work in F — is tabulated in [15-platforms.md](15-platforms.md#windows-v1-surface-on-mac).

PR 6 agents keep the Windows tree. PR 16 adds Darwin files, a CMake Apple path,
and portable present-policy tests that also run on Windows.

## 2026-09-13 — Tear the chrome down before DestroyWindow (PR 6, fixing a PR 3-era exit crash)

**What was measured.** Chrome-on lab exits fail-fast about one time in five:
`0xC0000602` (`STATUS_FAIL_FAST_EXCEPTION`), faulting module `CoreUIComponents.dll`, the same
offset every time. `main` `38cb56b`: 13 of 60 chrome-on `--soak 3` exits (5/30 + 8/30). The fix,
`66303ec`: 0 of 30, same session, clean builds outside `%TEMP%` (MSB8029). `--no-chrome` exits did
not crash. It surfaced during PR 6 because the PR 1 gate counts a non-zero lab exit as a failed
measurement, so "PR 1's present-loop still holds" cannot pass while it happens.

**Cause, as far as it is known.** Every exit path (close button, `Ctrl+W`, and the soak's own
`PostMessage(WM_CLOSE)`) went `WM_CLOSE` → `DestroyWindow` → `WM_DESTROY` → dispose every
`DesktopWindowXamlSource`. The islands were being disposed while their parent was already
mid-destroy, with the XAML runtime left to process exit. There is no symbolised stack yet (no
debugger on the development box); the fix is judged by the exit-crash rate, not by a stack.

**Decision.** In `WM_CLOSE`, while the parent is whole: detach every island, pump pending messages
once (bounded) so the dispatcher runs the dispose it queued, then `DestroyWindow`. The bar's
`Detach` disposes `WindowsXamlManager` and shuts the `DispatcherQueueController` down after every
source is gone. PR 6a's defensive changes stay (unhook the static focus handler first, never let an
exception out of a XAML event, null a source before disposing it), but they were not the fix: 6a
did not change the crash rate.

**How it gets reversed.** If 30 chrome-on exits still show a fail-fast, this was not the cause.
Next suspects are the render thread presenting into the DComp visual during detach, and the order
of `WindowsXamlManager` against the `DispatcherQueueController`.

## 2026-09-13 — On-canvas labels share the F3 overlay's ImGui draw list (PR 6)

**The contradiction.** D1 ([01](01-decisions.md)) says ImGui is the present lab and the F3 overlay,
never shipped chrome. [16](16-commands.md) allows the `O` info line to be "ImGui-style overlay or a
tiny island". PR 6 draws `O` (file name, position, size, zoom), the loupe frame and the hold-`\`
"previous" label with ImGui's foreground draw list, in the same present as the image.

**Decision.** Those are canvas overlays, not chrome: a line of text and a rectangle drawn over the
swapchain, with no focus, no input and no layout. D1's "never shipped chrome" is about the command
bar, filmstrip, panes, palette and `?` — anything a user operates — and that stays WinUI. The `?`
cheat sheet and `Ctrl+K` palette (PR 6) are XAML flyouts as [16](16-commands.md) specifies.

**How it gets reversed.** If an on-canvas label needs interaction, localisation beyond the bundled
font, or accessibility (screen reader) support, it moves to a tiny island. The EXIF exposure
triangle in PR 8 is the likely trigger.

## 2026-09-13 — giflib and libwebp land in PR 6, not PR 7; browser delay clamp

**Why.** PR 6's roadmap entry and verify line already require animated GIF / APNG / WebP "on the
QPC frame clock" with "animation timing matches a browser", but no GIF decoder existed and
libwebp was listed for PR 7. The user approved bringing both into PR 6 (2026-09-13).

**What moves.** giflib (MIT) and libwebp (BSD-3, with libwebpdemux) are linked in PR 6, dynamic in
the vcpkg x64-windows triplet like the other decoders. Still WebP arrives with them. PR 7 keeps the
broken-file corpus and the per-decoder fuzz harnesses for both. APNG needs no library: the
animation chunks are walked in-tree and each frame goes back through libspng.

**Delay clamp.** [04](04-image-pipeline.md) said "clamp `delay < 20 ms` to 100 ms". Chromium and
Firefox treat **10 ms or less** as 100 ms. For GIF's centisecond delays the rules agree; for
APNG / WebP millisecond delays of 11–19 ms they do not, and the verify line compares against a
browser, so the browser rule wins. Tested at 0 / 10 / 11 / 19 / 20 ms.

**How it gets reversed.** Only if PR 7's fuzzing finds either library unsafe to keep; then the
format waits for a replacement rather than for PR 7's schedule.

## 2026-09-13 — PR 6 chrome: typeahead from `/`, and two slips (folder tree, companions)

**Typeahead.** [16](16-commands.md) said "with canvas or filmstrip focused, typing filters the
listing". With the canvas focused, nearly every letter is already a command (A D F G B S C O T Z Q
E J K L R), so bare typing there cannot also be typeahead. Decision: with the filmstrip or gallery
focused, typing jumps by name (300 ms reset); from the canvas, `/` opens a find box. The palette
(`Ctrl+K`) still finds any command by name.

**Folder tree slips to PR 8.** [16](16-commands.md) allows it: the tree is a third island with its
own virtualised directory model, and PR 8 already brings panes. What lands in PR 6, so the island
maths is not retrofitted: the `folder_tree` command and its key (`Ctrl+Shift+E`, which beeps and
logs until then), and `chrome_left_px` in the input snapshot and `usable_canvas`. `PageUp` /
`PageDown` stay "skip ten" until the tree exists.

**Companions-as-hidden slips to PR 7.** The PR 6 row names it, but the companions it hides are
RAW+JPEG and Live Photo pairs, which need RAW and HEIC — PR 7's formats. PR 7 already owns pairing;
hiding the companion is the same scan-time step, so it moves with it.

**Wrap.** "Wrap at end of folder: on by default, toggle in settings" lands in PR 6: a `wrap`
setting (default on) used by arrow keys, Space, `A` / `D` and the slideshow alike.

## 2026-09-13 — PR 6: Settings screen remaps the live command table

[16](16-commands.md) parked remap UI in v1.1 so the default map could be used in anger
first. It has been: the Settings flyout was three toggles, and changing a key meant
editing the table. Decision: Settings is its own screen (the command-bar island expands
over the canvas). It holds view defaults (filmstrip, wrap, sticky zoom, background) and
every binding. A clash swaps the two rows so nothing is left unbound. Persist diffs in
`settings.ini` `[keys]`. `?` and the palette call `describe_commands()` on the live
table, so they cannot drift. Reset writes the factory `kBindings` back.

JSON import/export and named layouts stay v1.1.

## 2026-09-13 — Theme, colour scheme, and a user font wait for v1.1

Settings in PR 6 holds view defaults and the live keymap. A request to restyle the app —
colour scheme, chrome + canvas + F3 overlay palette, and uploading a font file — is
customisation of the *shell*, not of photos. It does not belong in the PR 6 verify line.

**v1.1.** One palette drives WinUI chrome, the swapchain clear / empty-canvas copy, and
the ImGui overlay, so a light theme cannot leave a dark F3 panel. Bundled CozetteVector
stays the default. A user TTF/OTF is copied into `%LocalAppData%\MediaViewer\fonts` and
loaded from there; if it fails to load, fall back. Nothing about that file leaves the
machine (rule 6). PR 1's present-loop still has to hold after a font-atlas rebuild.

**How it gets reversed.** Only if a high-contrast / accessibility requirement is a ship
blocker for v1; then a system-theme follow (dark/light) without a font picker can land
as a tiny Settings row, not a full custom palette.

**How it gets reversed.** Only if the remap UI is unused and the extra island-resize
path fights the present-loop gate; then drop the screen and keep the live table for a
later PR.

## 2026-09-13 — PR 6: tap `Q`/`E` skips; chrome focus is not a mode

**Tap `Q`/`E` is skip, not speed.** [16](16-commands.md) had tap `Q`/`E` step the speed ladder
and hold skim ±2 s. In use, those keys were the skip keys: a tap that waited for key-up to
change speed felt like a dead key, and hold-to-skim was invisible if the tap never fired.
Decision: tap `Q`/`E` skips ±2 s on the down edge (exact seek); hold still skims (non-exact,
settle on release). Speed stays on the command-bar dropdown and on `Shift+Q` / `Shift+E`.
`J`/`L` remain ±10 s.

**Command-bar / transport / `?` flyout focus is not island mode.** The typeahead rule
(filmstrip or gallery, plan/12 earlier today) was implemented as "any non-canvas focus
swallows letter keys". Clicking Play, opening `?`, or a flyout popup HWND then made `A`/`D`
(and `Q`/`E`) do nothing until the window was deactivated and the canvas HWND took focus
again. Island mode is the filmstrip and the gallery. The command bar, the transport, and a
cheat-sheet flyout keep the mode underneath; Esc still closes the flyout first via
`popup_open`.

**How it gets reversed.** Speed-on-tap only if a remap UI (v1.1) wants the FastStone-era
pair back; do not silently steal skip. Do not put command-bar focus back in island mode to
"make Tab easier" — that is how the keys die.

## 2026-09-14 — PR 8: settings writes leave the UI thread

Closes the "Settings writes on the UI thread" open row (recorded 2026-09-13; the roadmap
moved it to PR 8 release hardening).

`settings.ini` is read once at startup into an in-memory document (`shell/settings_store`).
Saves mutate it through read-copy-update and hand the newest immutable snapshot to a
one-thread persist worker (`mv::job_system`, separate from `file_jobs`, so a long card-dump
move neither delays nor drops a settings write, and `file_jobs.stop()` cannot discard it).
Bursts coalesce; an unchanged document writes nothing. The worker writes temp →
`FlushFileBuffers` → `MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH)`, so a crash mid-write
leaves the old file. The exit path flushes with a bounded 1 s wait after the WM_CLOSE
island teardown (order unchanged); the render loop never waits. A detector counts any
settings write on the registered UI thread outside the exit scope, and tests assert it.

The file is now UTF-16LE with a BOM (readable by the profile API); PR 4–7 ANSI files still
load and unknown keys survive. New keys go through `app_settings().get/set/update`. Direct
`*PrivateProfile*` calls on `settings.ini` are not allowed — the store rewrites the whole
file and would lose them.

**How it gets reversed.** It does not go back to the UI thread. If the worker proves
unnecessary, the snapshot write can move to the existing I/O pool only once `file_jobs`
stops dropping queued work at exit.

## Still open

| Question | Blocks | Notes |
|---|---|---|
| ~~**Do we need the Microsoft Store?**~~ | ~~PR 1~~ | **Closed 2026-09-06: no.** App is GPL-2.0-or-later, Exiv2 kept under the GPL, direct download only. See the PR 1 entry above. |
| **A quiet machine for the D6 gate** | PR 1 verify (inherited) | Re-run 2026-09-07: one animated pass, one animated fail, idle contaminated by mouse. Still needs the self-hosted GPU runner [09](09-build-and-test.md). |
| **PR 4's verify was never run** | PR 5 (inherited) | Three sessions held PR 4; the first hallucinated, the second committed `5eaa530` without reporting, the third confirmed it never owned the PR. Recorded state as of 2026-09-07: the 2000-JPEG scroll, the warm second-visit thumbnail check and the < 40 ms warm arrow-key number are **not run**; `tests/test_frametime.ps1` is **not run**; the plan edits in that commit to [10](10-roadmap.md) and [16](16-commands.md) are **unreviewed**. PR 5 is being built on top of this knowingly. |
| ~~**Settings writes on the UI thread**~~ | ~~PR 8 (release hardening)~~ | **Closed 2026-09-14:** writes moved to a persist worker; see the PR 8 entry above. Original note: `settings.ini` writes (filmstrip toggles since PR 4, F7 / F8 destinations since PR 6) run on the UI thread, against rule 1. They are small, and a destination is only written when it changes, but they belong on the I/O worker. Recorded 2026-09-13 so the rule does not erode quietly. |
| **Do WinUI 3 XAML islands hold up?** | PR 3 verify (inherited) | Command-bar island is in the tree. Filmstrip is a second island (PR 4). Present-loop + tab + flyout-over-canvas still unproven on a quiet GPU runner. Fallback unchanged: WinUI app with `SwapChainPanel` and an accepted composed frame. |

## Gallery keyboard controls (2026-09-13)

At the user's request, View now offers Full screen with F11 (F remains an alias).
The visible gallery has its own navigation mode so W/S move by row and Enter opens
the selected item in the normal viewer, even if focus remains on the canvas after G.
The user's clarification removes Enter's fullscreen behavior: it leaves any active
slideshow/fullscreen and restores the filmstrip according to the existing toggle.
Slideshow moves from Enter to F5; Enter on the canvas has no fullscreen action.
Gallery navigation letters take
precedence over typeahead; text fields and filmstrip typeahead keep their bindings.

The gallery also gains thumbnail resizing on +/− (= aliases +), as separate
commands in the shared binding table for the concurrent Settings/remapping work.
Existing command IDs and binding row positions are retained. The gallery's
typeahead exception follows the resolved command so remapped resizing and
navigation keys work. Thumbnail size is session-local, initially 152 DIP with
24 DIP steps bounded to 80–344 DIP; resizing preserves selection and updates only
realised tiles plus the uniform grid layout.

## Settings focus and layout (2026-09-13)

Settings owns keyboard input through XAML dispatch, including Escape. A false
return from ContentPreTranslateMessage is not evidence that XAML declined a key;
running viewer shortcuts at that point stole replacement keys before capture.
Opening Settings releases viewer holds, sizes the island before measuring the
screen, and focuses an actual control. The root follows the island viewport
without toggling between a fixed 48 DIP height and full height. Shortcut buttons
keep focus and scroll position while their labels update in place. Capture has
an explicit prompt, Escape/Cancel, and consumes the captured key's repeats and
release so Enter/Space cannot reactivate the button. Tab remains in Settings.

## 2026-09-13 — Drop the Ctrl+K command palette

**From.** PR 6 shipped a searchable command palette (`Ctrl+K` / `Ctrl+Shift+P`) as a
XAML flyout over the command-bar island, filtering the live table so nothing had
to be memorised.

**To.** No palette. `?` lists the current mode's bindings; Settings type-to-filter
finds a command to remap. The `palette` command id and `chrome_popup::palette`
value stay as unused holes so later command ids and popup kinds do not shift.

**Why.** A WinUI `TextBox` in that flyout fail-fasts (`Microsoft.UI.Xaml.dll`
`0xC000027B`). A stand-in field still never sees keys that are already bindings,
because the one native router handles them before the island — the filter
ignored `O`, `F`, arrows, and every other mapped key. The owner dropped the
feature rather than punch a second input path through the router.

**How it gets reversed.** Only with a field that is not a WinUI `TextBox` *and*
a router rule that, while the palette is open, sends printable keys to the
filter the way Settings already does. Do not re-add `Ctrl+K` as a silent
second `?`.

## 2026-09-13 — libheif `hevc` feature is x265 encode, not HEVC decode

**From.** [09](09-build-and-test.md) / [vcpkg.json](../vcpkg.json) sketched PR 7 as
`libheif[hevc,av1]`.

**To.** `libheif` with **default-features OFF**. The vcpkg port's `hevc` feature is
`WITH_X265` — a software HEVC *encoder*, which plan/11 forbids. HEVC *decode* is
libde265, a hard dependency of the port, not a feature. AVIF decode is
`libavif[dav1d]`, not libheif's `aom` feature.

**Why.** Enabling the documented feature set would have linked x265 and failed
the licence gate (and the patent line) the moment PR 7 configured. The plan's
shorthand was written against an older port layout.

**How it gets reversed.** Only if the port grows a decode-only HEVC feature that
does not pull x265. Do not turn default-features back on.

## 2026-09-14 — Ship the PR 7 viewer in PR 8; defer additional features

**Why.** The owner wants to run and ship the features available once the agents finish
PR 7, then add the remaining features in future updates. Editing, metadata panes,
trimming, and Windows shell integration must no longer delay the first release.

**Call.** v1 is the Windows viewer through PR 7, packaged and shipped in PR 8.
D4's light-edit requirement and D7's trim requirement move to post-v1; their technical
designs stand. PR 7 and the inherited verification gates, including PR 1's present loop,
are still required. This changes scope and sequencing, not completion status.

| Former PR | Current PR | Slice |
|---|---|---|
| 15 | **8** | Package & ship the PR 1–7 viewer |
| 8 | 9 | Metadata read + deferred folder tree |
| 9 | 10 | Geometry edits + export |
| 10 | 11 | Colour adjusts |
| 11 | 12 | Narrow metadata writes |
| 12 | 13 | Two-path trim |
| 13 | 14 | Extract & remux |
| 14 | 15 | Windows integration |

PRs 1–7 and 16–20 keep their numbers. Milestone C becomes shipping (PR 8), D/E are
future Windows updates, and F remains macOS. The existing PR 16 parallel-work exception
stands; the Windows ship gate for the remaining Mac work is now PR 8. Earlier entries
in this log retain their historical numbering; use this mapping for current work.
The open UI-thread settings-write follow-up previously assigned to PR 15 (2026-09-13)
now belongs to PR 8 release hardening.

**Packaging stands alone.** Move the shared app icon to PR 8 (window, taskbar, wizard,
shortcuts, About); PR 15 reuses it for associations. Inno Setup, Velopack, signing,
About/licence/source offers, and default-off telemetry remain release work. Crash
reporting stays in PR 7. Associations, default-app prompting, and Explorer handlers
wait for PR 15; PR 8 uninstall checks cover what PR 8 actually installs.

**Verify changes.** Install on a clean VM, launch and exercise the PR 1–7 viewer,
check bundled formats, playback and inherited pacing, then verify updates, rollback,
and uninstall. Crop, trim, and export are no longer first-release acceptance steps.
Future feature PRs retain their own verify lines; no update version is promised for them.

## 2026-09-14 — PR 7 format and pairing calls made while landing the slices

Recorded as they were merged (TIFF/ICO, pairing, LibRaw, HEIC/AVIF). Rows marked
**open** need the owner's sign-off before PR 7 closes; the rest are the conservative
reading of the plan and stand unless reopened.

| # | Call | Why | Status |
|---|---|---|---|
| 1 | **D3 OS probe is HEIC stills only.** WIC is tried only for an 8-bit, no-alpha, non-HDR, non-sequence HEVC HEIC whose colour is an ICC or sRGB-in-effect, when WIC has a HEIF decoder *and* Media Foundation has an HEVC decoder. WIC must return the ICC and the same displayed size libheif would; any failure except cancel falls through to libheif silently. `MV_OS_CODEC=0` forces the bundled path. JPEG/PNG/BMP/GIF/WebP/TIFF/ICO/AVIF/RAW never go through WIC. | Routing every format through WIC would drop the LCMS path the D6 tests pin; HEIC is the one format where the OS path can be hardware-backed. On a machine with the Store packs, WIC and libheif agree on size and ICC (pixel mean diff 1.9). | stands |
| 2 | **HDR (PQ/HLG) HEIC/AVIF stills are tone-mapped to SDR in the decoder** with the `gfx/video_blit.cpp` curves and handed on as display-referred sRGB. | `image/colour.cpp` refuses `scene_referred`; HDR output is v1.1. | **open** — confirm tone-mapping in `codec/` rather than a scene-referred colour stage |
| 3 | CICP SDR camera transfers (BT.709/601/2020) display with the sRGB curve; gamma 2.2/2.8/linear get a synthesised ICC. Display P3 nclx gets a synthesised ICC v4 profile. | Browser behaviour; P3-as-sRGB is the D6 bug. | stands |
| 4 | **No display-path orientation exists yet.** TIFF returns stored order; HEIC gets libheif's irot/imir; AVIF applies irot/imir/clap itself; RAW preview *and* full decode are rotated in pixels by LibRaw's flip. A later EXIF-orientation pass must skip RAW or it rotates twice. | plan/04 wants orientation on the display path; building it is not a PR 7 line item. | **open** — schedule the orientation pass (JPEG EXIF is also unhandled) |
| 5 | TIFF: 16/32-bit round to 8; float clamps 0–1 (untagged → linear then sRGB encode); CMYK converts naïvely (1−C)(1−K); **grey and CMYK ICC profiles are dropped** because `to_display` builds an RGBA transform. | RGBA8 raster; a grey profile would make a valid file `corrupt`. Grey JPEG/PNG with a grey profile likely share the gap. | **open** — colour stage should learn grey profiles |
| 6 | RAW full decode: PPG demosaic, camera WB, sRGB 8-bit, highlight clip, **auto-bright on**. Measured 0.8–1.7 s on 16–42 MP samples — **misses plan/09's < 500 ms** (vcpkg LibRaw has no OpenMP; GPU demosaic is out of v1, D4). First pixel is the embedded preview (11–69 ms, JPEG-comparable). Full decode is still 7–41 luma levels brighter than the preview; cancel granularity is one LibRaw stage (≤ ~550 ms). | AHD was 2.5–4.7 s; auto-bright off left a ~36-level gap vs the embedded JPEG. | **open** — accept the target miss for v1 or pursue an OpenMP LibRaw build |
| 7 | **JPG+MOV pairs as a Live Photo** (iPhone "Most Compatible"), as well as HEIC+MOV. Pairing is by basename only; the ContentIdentifier check in plan/04 is not done (needs metadata, PR 9). RAW+HEIC counts as RAW+JPEG. Groups of three or more stay separate. | Exact, cheap, never hides a file. | **open** — confirm JPG+MOV |
| 8 | **File operations on a paired stop act on both halves** (copy/move-to, Recycle Bin, drag-out); the prompt names both files. Collision renaming is per file, so a pair can land as `x (2).JPG` beside `x.NEF`. | Deleting only the JPEG would make the RAW reappear as its own stop. | **open** |
| 9 | Opening a RAW from Explorer selects its pair's stop and shows the JPEG (the primary). | plan/04: the still is first pixel and primary. | stands |
| 10 | **Open RAW / Open JPEG** are command-table rows with no default key ("Unbound" in Settings, hidden from `?`). plan/04 still says they live in the palette, which 2026-09-13 dropped. | No key named in plan/16; a Settings binding makes them routable. | **open** — pick keys or amend plan/04/16 |
| 11 | ICO: largest entry (then deepest) is the still; an unreadable largest entry silently falls back to the next. AVIF with unknown/infinite repetition loops forever. | Chromium behaviour; never an error for a viewable file. | stands |
| 12 | Command id 76 (`palette`) stays in the table as a keyless retired row (13a72bc); the test asserts a retired id is only ever listed keyless, which `describe_commands()` hides from Settings and `?`. | Two fixes met on the branch; keep the wire enum named. | stands |

## 2026-09-14 — PR 7 crash reporting: a post-crash scrub instead of an arena-tagged heap filter

**Why.** plan/13 asked for a minidump filter that excludes heap regions tagged at the arena
level. Crashpad's Windows handler has no filter hook, the stock `crashpad_handler.exe`
cannot be extended without forking it, and tagging would have required every PR 7 decoder
to allocate `raster::rgba` through a custom allocator. Measured on a real crash, the
unscrubbed dump did **not** contain heap pixels (indirect memory gathering is off), but it
did contain the canary file's full path (PEB command line, UTF-16) and the username
(module list and PDB paths), 135 hits.

**Call.**
- Indirect memory gathering off, WER forwarding off, no extra ranges.
- The app rewrites every finished dump in place on its next launch, before any send is
  possible (`src/shell/minidump_scrub`):
  - Zero every captured byte outside a thread stack: PEB, process parameters, TEBs, and
    the 512 bytes Crashpad takes around each register.
  - Mask drive/UNC paths (modules keep their layout, minus the profile name), bare media
    filenames, and the username/computer name, in UTF-8 and UTF-16LE.
- Heap pixels are excluded structurally, not by tag.
- The handler **never** receives an upload URL, because it would upload before the scrub.
  PR 8 (formerly 15) must upload from the app after scrubbing.

**Residual risk.** A folder name with no drive root and no media extension, a filename stem
without its extension, or pixel rows in a decoder's stack-local array can still survive
on a live stack frame. `tools/minidump-scan.ps1` is the check.

**Licence.** The Crashpad client is Apache-2.0 and statically linked. Apache-2.0 is
GPL-3-compatible, not GPL-2-only, so distributed binaries are conveyed under GPL-3.0 terms
(the "or later" permits it; LGPL-3 libheif/libde265 already implied the same).
**Open for the owner:** confirm, or move the client out of the lab binary.

**Deferred.** The consent dialog: `[crash] consent` / `upload_url` and the
ask-only-with-an-endpoint check exist, but no endpoint exists yet, so there is nothing
to ask. It lands with the upload path in PR 8 and must not stack with other first-run
prompts. Crashes inside the OS-codec probe (before the bundled dispatch) are not annotated.

## 2026-09-17 — Owner reopens the PR 16 sequencing exception: PRs 17–20 may proceed alongside Windows v1

**Why.** The 2026-09-13 exception (see above, and `plan/10-roadmap.md`, `plan/15-platforms.md`)
allowed only PR 16 (the Metal present lab, no SwiftUI) to run in parallel with Windows PRs 1–8,
because Mac chrome and Windows chrome are "two present labs, two chromes, two ship pipelines" if
built in lockstep, and D9's product call — Mac is a host, not a UI port — was not meant to be
reopened by convenience. The owner is now working from a Mac day to day and asked, directly, for
PR 17 (Metal decode + pan/zoom) and PR 18 (SwiftUI chrome) to proceed now rather than waiting on
Windows PR 8 (package & ship), which has not shipped yet as of this entry. The two efforts touch
disjoint files (`*_mac.*`, `src/shell` Mac targets, a new Swift target, vs. Windows' `src.managed`
and Inno/Velopack packaging) and a separate agent's Windows PR 8 branch (`pr8-package-and-ship`)
is unaffected by Mac-only additions.

**Call.** PRs 17–20 are authorized to proceed in parallel with Windows v1, same as PR 16. This is
a scope decision, not a technical one — D9 itself (Mac is a host of the shared core, not a
SwiftUI skin on the Windows present path; native chrome per OS; one present path per OS) is
**unchanged and still binding**. The internal PR sequence inside Milestone F is also unchanged:
PR 17 before PR 18 before PR 19 before PR 20, each verify line holding before the next starts, the
same discipline `10-roadmap.md` applies to PRs 1–8. Windows PRs 9–15 continue to wait for nothing
Mac-side; the hostable-core rules (`plan/15-platforms.md`) keep applying to any further Windows
native-code changes regardless of what Mac is doing.

**Residual risk, taken deliberately.** Milestone F was sequenced after PR 8 so a present-loop
regression on one OS would not hide behind schedule pressure on the other. Running both now means
that discipline has to hold by attention, not by calendar — PR 1's Windows present-loop verify
still gates every Windows PR, and PR 16's Mac present-loop verify gates every Mac PR, independently.

**Build-verification note.** The environment this decision was implemented in has no Xcode (only
Command Line Tools — no `metal` shader compiler), no `cmake`, and no `vcpkg` install, so PR 17/18
code landed here could not be compiled, run, or soak-tested on-device. Treat it as reviewed, not
verified, until it is built on a real Apple Silicon Mac with the full toolchain from
`plan/09-build-and-test.md` / `plan/15-platforms.md`.

## 2026-09-17 — PR 4/6/7 parity gap folded into PR 17/18, not new PR numbers

**Why.** An audit found that `plan/15-platforms.md`'s Windows-to-Mac mapping table left three
already-shipped Windows PRs with no PR 16–20 slot at all: PR 4 (folder, filmstrip, gallery,
thumbnails, dir watch), PR 6 (keyboard-complete browse, slideshow, Trash, drag-drop, argv), and
PR 7 (HEIC/AVIF/RAW/TIFF/WebP/ICO, pairing, fuzzing, Crashpad) — the table said "after PR 18" or
"same decoders" with no PR number. As scoped, PR 16–20 would have shipped a Mac app that could
open one JPEG/PNG/BMP or one video file and pan/zoom/play it, with chrome — not Windows parity.

**Call.** Given the choice between (a) inserting new numbered PR slices, mirroring how PR 5 was
split into 5a/5b/5c when it outgrew one verify line, (b) folding the missing scope into the
existing PR 17/18/20 slots, or (c) deliberately deferring PR 4/6/7 parity to a post-F Mac update
stream, the owner chose **(b)**: fold in. PR 7's format/fuzzing/crashpad scope moved into PR 17
(same reasoning Windows used — crash reporting lands with the format long tail, because that is
when hostile real-world files first meet decoders). PR 4 and PR 6's folder/filmstrip/gallery/
keyboard-complete-browse scope moved into PR 18 (they are chrome-hosted, so they need PR 18's
SwiftUI shell to exist first, same as Windows needed PR 3's chrome before PR 4/6 could land).
PR 19 and PR 20 were already at Windows parity for their slice (PR 5abc and PR 8/15
respectively) and did not change.

**Consequence, taken deliberately.** PR 17 and PR 18 are each now wider than their Windows
twins (PR 2 alone, PR 3 alone) and carry a correspondingly longer verify line. This is the exact
trade-off PR 5's split was designed to avoid on Windows; the owner chose it anyway for Mac,
preferring fewer PR numbers over a clean one-slice-one-verify-line split. If either PR 17 or
PR 18 turns out too large to land and verify as one slice in practice, splitting them the way PR
5 was split remains available — this entry is not a bar against that, only a record that it
wasn't the first choice.

## 2026-09-19 — Metal `maximumDrawableCount` is 2, not 1

**Not a D1–D9 reversal.** plan/15 and the PR 16 spec say "max drawable 1", carried over from
D3D11's `SetMaximumFrameLatency(1)`. On real hardware `CAMetalLayer` throws
`CAMetalLayerInvalidMaximumDrawableCount` for anything outside [2, 3], so 1 cannot be expressed.
2 is the lowest-latency setting Metal allows and is what ships (`gfx/metal_layer.mm`,
`MvMetalView`). The intent — one frame of latency, wait on the display link before encoding —
is unchanged, and the 60 s gate passes with it (3600 frames, 0 dropped, p99 16.9 ms, 2026-09-19).

## 2026-09-20 — PR 19 host choices: VideoToolbox through a texture cache and a copy, Core Audio position anchoring

**Not a D1–D9 reversal** — D2 and D9 hold (FFmpeg, our own presentation ring, no `AVPlayer`, no
Store codec). Choices the plan left open for the Metal host:

- **"On *your* `MTLDevice`" means the texture cache and the blit, not the decoder.** VideoToolbox
  owns its decode sessions and takes no device; the device is used for the `CVMetalTextureCache`
  that wraps the decoder's IOSurface-backed pixel buffers, and the blit that copies them *out* of
  the decoder pool into the ring runs on a queue of that device. The blit is waited for on the
  decode thread (~1 ms for 4K): that thread is neither UI nor render, and it makes the slot
  complete before it is published and the pixel buffer returned, so there is no ordering hazard to
  reason about (Windows needs the immediate-context ordering argument in plan/12 2026-09-07; Metal
  does not).
- **A slot is two textures (R + RG), not one planar texture with two views** — Metal has no
  planar NV12/P010 texture to view. Same R8/RG8, R16/RG16 pair plan/05 specifies, shared storage.
- **A presented frame is held two more presents before release** (Darwin ring is 6 slots, not 4):
  the GPU may still sample it and the decode thread reuses a released slot at once.
- **Played position is anchored to each render callback's host time with a *signed* offset.**
  `mHostTime` is always in the future by the output latency, so an "only if now > host" extrapolation
  never engages and the clock steps by one callback (~11 ms); a 60 Hz presenter sampling that
  staircase dropped a third of a 30 fps clip as "late". Found with `tools/playprobe`.
- **MPEG-2 has no VideoToolbox decode on this hardware**, so it runs in software and the F3
  overlay says `SOFTWARE`. D5 lists MPEG-2 as supported; it is, without hardware.

## 2026-09-20 — PR 7's three human checks, turned into gates where they could be

The PR 7 verify line has three clauses that read as "a person looks at it": an iPhone
HEIC on a clean VM, a real Live Photo, and the preview → full swap not popping. None can
be produced from this machine, and none is marked passed. What changed is how much of
each is asserted by a test rather than resting on a look.

**Clean VM (`tests/test_clean_vm_heic.cpp`, target `mv_clean_vm_tests`, label `cleanvm`).**
`MV_OS_CODEC=0` was described as standing in for a clean VM, and was only ever checked to
make `try_os_decode` decline. Nothing checked what did the decoding afterwards. The new
test decodes a HEIC with the switch off and reads the process module list: libheif and
libde265 must be mapped, and Media Foundation, the WIC codec extensions and anything under
`\WindowsApps\` must not be. It also asserts the disabled path loads no module at all, so
the `MFTEnumEx` probe cannot return unnoticed. Its own executable, because the assertion is
one-way: inside `mv_tests` an earlier video case has already pulled in `mfplat.dll`.
`codec/os_decode_win.cpp` is the only TU in the tree that touches WIC or Media Foundation,
so that switch really is the whole OS-codec surface for stills.

**Live Photo (`tests/test_live_photo.cpp`).** Pairing was tested on hand-made `dir_entry`
structs and, at the ABI, on renamed BMPs. It is now also tested on a real directory shaped
like a camera roll, through `list_still_files` + `pair_listing`, and — with the corpus
present — with a real playable clip as the motion half, opened through the same ABI calls
`;` makes. Two shapes worth naming: `IMG_E####` (iOS's edited copy) is correctly its own
stop with the original's pair intact, and a *hidden* motion half is not attached to a
still. Pairing stays name-only; the ContentIdentifier check is still PR 9's (row 7 of
2026-09-14).

**No pop — and a real defect found.** The lab now measures what a pop is made of: the worst
corner displacement of the picture's on-screen rectangle across `camera_.refine`, the worst
step in its on-screen size, whether every cross-fade reached alpha 1, and any dropped frame
inside a fade window. They are in the `--json` report, on `F3`, and gated by
`frametime --no-pop <image>` together with PR 1's cadence.

Measuring it found that a still refines **twice** — `full_top` when the top level exists,
then `full` with its mip chain, the same pixels both times — and the second publish
restarted the cross-fade. On a Canon CR2 that snapped the outgoing embedded JPEG out at
alpha 0.6 in a single frame, which on a RAW is most of the 7–41 luma levels between the
preview and LibRaw's render. That is the pop the verify line forbids, and it had been there
all along. **Call:** a same-size refinement arriving while a fade is running swaps the
incoming texture under the running fade instead of starting a new one, so the preview fades
out once, continuously. A 62 s soak on the CR2 then passed the no-pop gate and PR 1's
cadence gate on the development box — not the quiet GPU runner, so PR 1's own caveat stands.

**LibRaw's 0.8–1.7 s full decode (row 6 of 2026-09-14) — still open, options priced.**
vcpkg's `libraw` 0.22.2 port does expose an `openmp` feature, so it is a one-line manifest
change, but not a free one: it puts an OpenMP thread pool inside a decode worker that
already runs on our job system (plan/02's five thread roles), adds the MSVC OpenMP runtime
to what PR 8 has to ship, and does nothing about the cancel granularity of one LibRaw
stage. Against that: PR 7's verify line asks for "preview time comparable to a JPEG", which
is met at 11–69 ms; the 500 ms figure is plan/09's target for the full decode, which the
preview already hides, and the swap out of it is now gated. The recommendation is to accept
the miss for v1 and record it rather than take a threading change into a packaging PR.
**Still the owner's call — not settled here.**

## 2026-09-20 — PR 8: the wizard owns uninstall, not Velopack

**Decision.** The Inno wizard owns the Apps & features entry, the shortcuts, and the
install directory. Velopack is packed with `--shortcuts None`, and the duplicate uninstall
entry it registers is deleted — by the wizard at install, and by the host after an update,
which is the only other moment Velopack writes it.

**Why this came up.** Velopack registers `HKCU\...\Uninstall\MediaViewer` →
`Update.exe --uninstall` every time it applies a package. With the wizard also registering
one, a user sees MediaViewer twice in Apps & features, and the Velopack entry removes the
tree *without* the wizard's shortcuts — and, once PR 15 lands, without the `ProgId` and
handler registrations. plan/10 is explicit that "an update that leaves a zombie association
is a failed uninstall".

**Why the wizard and not Velopack.** Velopack's entry is self-healing across updates, which
argued for letting it win. Against that: it cannot know about anything the wizard or a
later PR adds, and PR 15's handler removal needs one place to live. The wizard is that
place, and the host's post-update sweep covers the self-healing gap. The sweep only ever
deletes an entry whose `UninstallString` names *this* install's `Update.exe`, so the
wizard's own entry and any unrelated product sharing the key name are untouched
(`is_velopack_uninstall_string`, tested).

**Also settled by measuring, rather than by reading docs.**

- Velopack's `--installto` **clears its target directory**. The bundle therefore runs from
  `[Code]` at `ssInstall`, before Inno writes anything; as a `[Run]` entry it deleted the
  wizard's own `unins000.exe` and left an Apps & features entry pointing at nothing.
- `Update.exe --silent uninstall` is **not** called at uninstall. It detaches a cleanup
  process that races Inno's directory removal — the uninstaller logged "Failed to delete
  directory (145)" and exited 1 while Velopack finished the job a second later. With
  `--shortcuts None` and the registry entry already ours, it had nothing left to do.
- `[UninstallDelete]` must name the Velopack layout explicitly. Inno removes only what it
  installed, and it installed neither the stub nor `Update.exe`.

**Rejected:** a per-machine MSI as the consumer channel (plan/13 already rejects it —
elevation on every update), and letting both entries stand.

## 2026-09-20 — PR 8: the AI and WebView2 payload is excluded at packaging

plan/13 already says not to ship Windows App SDK AI / ONNX / DirectML / WebView2. They
arrived anyway: `dotnet publish` of a Windows App SDK project copies the framework's whole
projection set regardless of use. Measured at 43.4 MB of a 119.6 MB payload — 36 % of every
download, for a viewer that does no inference and hosts no browser.

`tools/package/build-release.ps1` filters them out and then **asserts** they are absent
from the finished tree, because the recursive directory copy could reintroduce them.
Payload is 72.7 MB.

**Open.** The payload is still framework-dependent: the Windows App SDK runtime is an
assumed prerequisite on the target machine. The wizard neither installs nor detects it,
which is a hole sitting directly under PR 8's "no missing-codec dialog anywhere" clause,
since that clause is about a machine with nothing installed. Either the wizard gains a
runtime bootstrap or the publish becomes self-contained; not decided.

## 2026-09-23 — PR 8: both runtimes ship in the payload; no prerequisite

**Decision.** The payload carries the Windows App SDK runtime and the .NET
runtime. A clean Windows 10 21H2 machine with nothing pre-installed runs the
installed build.

**Why it was not optional.** The wizard is per-user and takes no UAC (plan/10's
verify line, plan/13). A machine-wide Windows App SDK runtime as a prerequisite
needs admin, so it contradicts that directly; and a prerequisite the wizard
neither installs nor detects lands the user in the failure the same verify line
forbids. plan/09 had already made the matching call for .NET — "Take
self-contained and publish the honest number. A viewer whose whole pitch is
'point it at a folder and it works' cannot open with a runtime prerequisite
dialog — that's the same mistake as a codec-pack prompt (D3)." — and budgeted
~70 MB for it.

**Mechanism, and why it is not simply `--self-contained`.**

- **.NET.** `hostfxr_initialize_for_runtime_config` cannot load a self-contained
  *component*: given a runtimeconfig with `includedFrameworks` it returns
  0x80008093 `HostApiUnsupportedScenario`. Measured, not inferred. Since D1 the
  chrome is a component the native host loads through hostfxr, so
  `--self-contained` is unavailable to it. The deployment outcome plan/09 asked
  for is reached instead by shipping the ordinary shared-framework layout
  privately under `<install>\current\dotnet`; `find_hostfxr` prefers it over the
  machine's install and stops at the first root that has one, so an installed
  MediaViewer runs on the runtime it was tested against.
- **Windows App SDK.** `WindowsAppSDKSelfContained` refuses a class library, and
  the chrome is one for the same D1 reason; the refusal is an audit target with
  an explicit override, and it is the audit that is inapplicable, not the
  deployment mode. The part that does not announce itself: self-contained WinUI
  activates registration-free, and registration-free activation reads the
  manifest of the **executable**. Our executable is the native host, not the C#
  project the SDK generated the manifest for — so without merging that manifest
  into the host's, every XAML activation fails with 0x80040111
  `CLASS_E_CLASSNOTAVAILABLE`, the island silently does not attach, and the app
  comes up with no command bar. The build merges it with `mt.exe`.

**Verified** on an installed copy: `hostfxr`, `coreclr` and `Microsoft.UI.Xaml`
all load from the install directory rather than from Program Files or a
framework package.

**Size, published honestly rather than rounded.** The app is 208.6 MB — inside
plan/09's stated 200–250 MB band and under its cap. On disk after a first
install it is 292.7 MB, because Velopack also keeps one full package so a bad
update can be rolled back (plan/13). That cache is a working set plan/13 prunes,
not the application, so the gate checks the app and reports the total.

**Open.** plan/09 suggests ".NET trimming to claw back part of it". Trimming is
not applied: it is unsafe for a component resolved through hostfxr and for
WinUI's reflection over XAML types. If the 250 MB cap ever needs real headroom,
that is the thread to pull, and it needs measurement rather than a flag.
## 2026-09-23 — macOS first install mirrors the PR 8 wizard, as a disk image

Not a D1–D9 reversal and not a sequencing change: it lands in **PR 20**, and PR 8 is
untouched. The owner asked for the Windows first-install idea (2026-09-13 above) to have
a Mac counterpart. `plan/15` only said "notarized Sparkle, `~/Applications` or a dragged
`.app`", with no first-run UX, licence, icon, or uninstall story.

**Call:**

| | |
|---|---|
| **First install** | Developer ID–signed, notarized, stapled `.dmg`: branded window, app + Applications alias, GPL shown on mount (Agree / Disagree). |
| **Not a `.pkg`** | Root scripts, no Trash uninstall, unreliable per-user domain. No custom installer app, helper, or login item. |
| **Updates** | Sparkle 2, silent, EdDSA-signed appcast against a pinned key. Same staging / never-interrupt rules as Velopack. |
| **Not in the install** | "Open with" (after first successful still), telemetry (in-app, default off) — same as Windows. |
| **Icon** | Same mark, one `.icns`. |
| **Uninstall** | Drag to Trash; the Quick Look extension is in the bundle. |

**Open for PR 20:** Sparkle has no "failed to start twice → previous version" rollback.
Build one or accept kill-switch-only on Mac, and log it. If the on-mount licence agreement
proves unreliable on macOS 14, fall back to a `Licence` file in the window + About, never
an in-app accept modal.

Detail in [13](13-updates-and-telemetry.md#macos-first-install--a-branded-disk-image-mac-pr-8).

## 2026-09-23 — PR 20 starts before PR 19's verify fully holds; calls made starting it

**Sequencing, the owner's call.** The 2026-09-17 entry keeps F's own order: each verify
line holds before the next PR starts. PR 19's verify includes "an iPhone HLG clip looks
correct", and only a synthetic HLG-tagged clip has been checked. The owner chose to start
PR 20 anyway, stacked on the PR 19 branch. PR 19's HLG check is **still owed** and still
gates calling PR 19 done. PR 20 does not touch the video path.

**Calls:**

| | |
|---|---|
| **Two executables** | `mediaviewer_lab` stays the bare instrument `frametime` drives. `MediaViewer` (inside MediaViewer.app) is the same sources with `MV_APP_BUNDLE`, plus Sparkle when a key is configured. |
| **Bundle id** | `io.github.longtimeno-c.mediaviewer` (CMake cache, `MV_MAC_BUNDLE_ID`). Changing it after the first ship orphans preferences and breaks Sparkle's same-app check. Decide before shipping. |
| **Registered types** | The D5 **still** set only, `LSHandlerRank` Alternate, role Viewer. Video is not registered, matching Windows PR 15's still-only `ProgId`s. `tools/mac/check_plists.py` ties the list to `codec/format.h`. |
| **Default viewer** | Asked once, as a sheet, after the first still reaches the screen. The type list is read back from the app's own Info.plist. macOS confirms each type itself. |
| **Quick Look** | A thumbnail `.appex`, sandboxed, running `image::make_thumb_jpeg`, the same pixels as the filmstrip. No cache of its own. |
| **Sparkle** | 2.9.6, SHA-256 pinned, MIT. XPC services removed (the app is not sandboxed). Signed feed and verified archive required. Automatic checks on, with no Sparkle permission prompt (the app menu turns them off). No system profile. Updates are a zip of the stapled app, never the disk image. |
| **Disk image** | dmgbuild 1.6.7, APFS + LZFSE, GPL as the image's licence agreement via `hdiutil udifrez`. |

**Owed, not done in this change:**

- **Nothing here has been built or run on a Mac.** It was written in a Linux container with
  no Apple SDK. The plist policy and packaging helpers run on Linux (not yet wired into CI); the Objective-C++, the
  CMake, and every step of `macpack.py` that runs a tool do not.
- **Quick Look precedence.** Whether Finder uses our extension or its own generator for
  types macOS already thumbnails (JPEG, HEIC, most RAW) is unmeasured. The corrupted-HEIC
  verify must check which process actually decoded it.
- **State across an update restart** is the folder and the selected file only. Zoom/pan
  and clip position (plan/13 "Preserve state") are not carried yet.
- **Rollback** after two failed starts has no Mac mechanism (open since the 2026-09-23 entry above).
- **Crashpad on Mac is not in the tree.** The 2026-09-17 entry folded PR 7's Crashpad scope
  into PR 17, and `plan/10` says PR 20 "does not add it", but `cmake/darwin.cmake` links
  no Crashpad and no Mac scrub exists. Either PR 17 still owes it or PR 20 takes it. Owner's call.

## 2026-09-23 — Mac host routes keys through the shared command table; Windows-style bar and Settings

**Decision.** The Mac host translates `NSEvent` to `key` at the edge and runs every key through
the same `command_table.cpp` / `key_router.cpp` as Windows (both are pure C++ and now build on
Darwin). Command and Control map to `ctrl`, Option to `alt`, the Mac Delete key to `del` (Trash).
The bar becomes Open / View / Settings / About with `?` at the right, and a Settings screen
(`⌘,`) offers the view preferences, canvas background and key remapping, persisted in
`NSUserDefaults`.

**Why.** The hard-coded `keyDown:` could not support remapping, and the macOS-styled bar did not
match the Windows chrome. **Consequences.** Commands the Mac lab cannot run yet (zoom steps and
presets, clipping, loupe, pan, go-to, folder tree, Live Photo, RAW pairing) are hidden from the
remap list rather than shown dead. Adds `mute` (Shift+M) and browse `wrap` as shared pieces.
Plain-letter menu key equivalents were removed so a menu cannot shadow a remap. Backspace is no
longer Previous on Mac (that key is Delete = Trash). Not a D1–D9 change: chrome stays SwiftUI,
the canvas stays Metal.
## 2026-09-23 — GitHub Releases is the channel; every push to main is a version

**Reverses part of [13](13-updates-and-telemetry.md) Part 1.** The owner asked for
continuous delivery: every commit to `main` builds a release, the wizard installs the
newest one from GitHub, and the in-app updater checks the same place.

**Call.** `.github/workflows/release.yml` runs on every push to `main` and on `v*` tags.
The Velopack set, the signed manifest and the Inno wizard are published as a normal
GitHub Release, and both the wizard and `GithubManifestFetcher` read
`/releases/latest/download/`.

**Versions stay strict `x.y.z`.** The first plan was `0.1.0-build.<run>`, which does not
survive contact with `ReleaseVersion` in `UpdateManifest.cs` — it parses three numeric
parts and nothing else, on purpose ("prerelease tags are not a v1 channel"). A suffixed
version fails `TryParse` in `UpdateService.TryInitialize`, and the updater answers by
going **inert**: a build that silently never updates. So the patch component is the CI run
number — `x.y.<run_number>` — unique and monotonic without CI committing back to `main`.
The version is stamped into `CMakeLists.txt` on the runner before configure, so
`VERSIONINFO`, `MV_APP_VERSION`, the payload and the manifest cannot disagree.
**Consequence to keep in mind:** bump `major.minor` in `CMakeLists.txt` before cutting a
tag, or `v0.1.0` sorts below the `0.1.<run>` builds that came before it.

**What this gives up.** plan/13's staged rollout (5 % → 25 % → 100 %, gated on crash-free
sessions) is not implemented and is incompatible with "every push ships": there is one
channel and it goes to everyone at once. The kill switch survives — the manifest still
carries `min_version` and a blocklist, so a bad build can be pulled. Revisit staged
rollout when there are enough users for a percentage to mean anything.

**Unchanged, and still blocking a usable channel:** the manifest is rejected unless signed
(`MV_MANIFEST_SIGNING_KEY`), and `UpdateKeys.ProductionPublicKeyHex` is still the all-zero
placeholder, so every build fails closed until the owner pins the real public key
([update-signing.md](../tools/package/update-signing.md)). A tagged release refuses to
publish without both signing paths; a `main` build warns and publishes anyway, so the
pipeline can be exercised before the secrets exist.

## 2026-09-23 — fuzz_decode and fuzz_animation are built but not run in CI

**Open, not settled.** Eleven libFuzzer harnesses run clean in the PR smoke — jpeg through
raw_preview, millions of execs each. Two never start: `fuzz_decode` and `fuzz_animation`
exit `0xC0000142` before libFuzzer executes a unit, on both the first launch and the
relaunch `run.ps1` gives them.

The harness CMake already copies `$<TARGET_RUNTIME_DLLS:...>` beside every harness, on the
theory that these two have the widest import closure and were missing a dependency. That
theory does not fit the code: `0xC0000142` is `STATUS_DLL_INIT_FAILED` — a DLL that was
found and whose initialisation *failed* — and a missing DLL is `0xC0000135`. So the fix
addressed a different failure from the one happening, which is why it did not take.

**Call:** the CI step names the eleven working harnesses explicitly. The excluded two are
still built, so they cannot rot. This is deliberately not "disable fuzzing": as it stood the
two dead harnesses failed the job and took the other eleven's result with them, so CI
reported nothing about decoders that were in fact being fuzzed hard.

**To diagnose properly** needs clang-cl, the ASan runtime and a debugger on the dying
process — none of which this machine has, since it lacks the `VC.ASAN` component. Worth
checking first: whether ASan-instrumented `mv_image` (the only extra library `fuzz_decode`
links) initialises twice, and what `fuzz_animation` pulls in that `fuzz_gif` and
`fuzz_webp` — which both pass — do not.

## 2026-09-24 — fuzz_decode and fuzz_animation re-enabled in CI (provisional)

Built with clang-cl + static ASan on a local machine, both harnesses start and fuzz clean
through `run.ps1` (20 s each: decode 9,742 execs, animation 3,976, exit 0, no artefacts). The
0xC0000142 seen in CI is therefore not intrinsic to the harnesses; the DLL-copy step may have
been the fix after all, or the cause is specific to the CI image (the earlier log noted it hit
the *last* harnesses of a long run, which also fits resource exhaustion). **Not confirmed on
CI.** Call: both are back in the CI harness list. If they die again, re-exclude and diagnose
on the runner; do not treat the local pass as settling it.

## 2026-09-24 — Windows v1 gate sweep: what closed, what did not

Run on the dev machine (Release build, `ctest`). **Closed:** `test_frametime.ps1`
(`frametime_harness`) passes — success, report identity, all eleven failure gates and
baseline protection; folder/browse/gallery unit tests pass. `fuzz_decode` and
`fuzz_animation` are already re-enabled in CI (entry above, provisional).

**Owner sign-off (2026-09-24):** the owner confirmed the items below are all good and asked
for them to be marked done. This was the owner's own verification, not something the
agent measured; no numbers were recorded, so the soaks and timings have no artefact behind
them.

**Closed on owner sign-off:**
- D6 60 s animated/idle soaks: need a dedicated quiet GPU runner.
- PR 4 verify by hand: 2000-JPEG filmstrip scroll, warm second-visit thumbnails, < 40 ms
  warm arrow-key browse. No headless harness exists for these; they are eyes-and-stopwatch
  checks on a real folder. The plan edits to docs 10 and 16 still need a human review.
- XAML-islands go/no-go (flyout over canvas, tab traversal, present loop on a quiet GPU).
- PR 7: clean-VM HEIC, a real phone Live Photo, RAW open without a visible pop; LibRaw
  full decode of 0.8–1.7 s remains.
- PR 8: clean-VM wizard through to video, signed artifacts (nothing is signed), update
  rollback on a real machine.
- Fuzz CI confirmation that the two re-enabled harnesses no longer die at 0xC0000142.
  (Included in the sign-off; the CI harness list stays as is.)

## How to use this file

Add a row when a decision changes, with the reason — not just the new value. If a decision here is
revisited and *upheld*, add that too; knowing an option was reconsidered and rejected again is worth
as much as the original call.

## 2026-09-23 — Deliberate two-platform GitHub releases

The owner requested simpler Windows/macOS downloads on GitHub Releases while signing
setup is incomplete. Replace automatic publish-on-push and local-only CI packaging
with a manual workflow: artifacts for rehearsal, explicitly unsigned prereleases for
testing, and stable releases with both authenticated update feeds. Windows Authenticode
is optional; stable macOS still requires Developer ID, notarization and Sparkle signing.
Unsigned Mac previews omit the updater and require a later manual stable installation.
This is a testing exception to the normal first-distributed-build updater requirement.

Both hosts use CMake's strict x.y.z version. Builds run independently; publication waits
for both and uploads to a draft before making it visible. Never publish a partial latest
release or replace published assets under an existing version. RELEASING.md is the runbook.

## 2026-09-24 — Default-viewer setup selected initially

The owner requested that both platforms offer all supported media as defaults with
the option already checked. Windows checks its Finish-page Default Apps link; the
user still confirms associations in Settings. Mac presents a first-launch setup
sheet with the checkbox on and applies it only on Continue. Unticking it or choosing
Not Now preserves existing defaults, and an answered Mac prompt stays answered on
updates. This replaces the earlier delayed-after-first-photo prompt policy; telemetry
and promotional links remain unchanged.

## 2026-09-24 — Local AI search planned (post-v1, proposed)

The owner asked for local inference so a folder of video can be searched by keyframe/moment.
[17-local-ai-search.md](17-local-ai-search.md) plans it as Milestone G (PRs 21–24), opt-in and
post-v1; PR 8 is untouched. It narrows two earlier calls rather than reversing them:
plan/16's "Face detect, AI cull, cloud albums" stays out (faces, culling and cloud are still
excluded; local text/image-to-frame retrieval is what is added), and the 2026-09-20 / plan/13
"do not ship ONNX/DirectML" stands for the **base installer** — inference ships only as a
separate opt-in AI pack. **Open:** DirectML runs on D3D12, which CLAUDE.md says not to introduce;
the owner must choose compute-only DML, CPU-only, or vendor EPs before PR 21
(plan/17, *Open decisions*). Not implemented; no code lands until that is answered.

**Amended 2026-09-24 (same day):** the owner wants face detection (as in iOS Photos), so plan/16's
"Face detect" exclusion is lifted for a local-only, opt-in, deletable people index — PR 25 in
plan/17, with stricter biometric handling than the frame index. AI culling and the
DirectML/D3D12 question are still unanswered.

**Settled 2026-09-24 (owner answers):** GPU inference uses **vendor providers** (OpenVINO,
CUDA/TensorRT) as optional sub-packs with CPU always the fallback and a Settings toggle
(Auto / provider / CPU-only) — **no DirectML, so no D3D12 and no CLAUDE.md change.** The whole
feature is a downloadable extra installed from Settings, never in the base installer. AI
culling was not requested and stays out. Recorded in plan/17 only, not as a numbered D-decision.
Known gap: AMD GPUs run CPU until a provider exists.

## 2026-09-24 — Multi-folder browsing is PR 26 (folder tiles + breadcrumb), not the tree

The owner opens a NAS root organised by year and got an empty viewer: listing was one directory
deep. Options weighed: folder tiles + breadcrumb, a left tree, a recursive sectioned gallery, all
three. Chosen: **tiles + breadcrumb + `Ctrl/Cmd+Up`** (PR 26, Milestone H), because it needs no new
chrome strip and fits the existing gallery and keyboard model. The left **folder-tree island stays
PR 9** (unchanged). A recursive flatten view is deferred: thumbnails, marks and relist are keyed to
one open directory, so one listing spanning folders is a model change, not a view. Not a D-decision;
no D1–D9 call is touched. Windows chrome follows on the shared core.

## 2026-09-24 — D9 amended: PRs 9–15 are dual-track, Windows and macOS in the same PR

**Owner's call.** Windows PRs 1–8 are in the tree and signed off (entry above). The owner
reports the basic Mac setup (PRs 16–20) complete, and is now working on PR 9 onward for both
platforms at the same time. **Reverses** D9's "Milestone F is five PRs, not a dual-track of
4–15" and plan/10's "Mac … not a dual-track requirement for those updates" for PRs 9–15.
Everything else in D9 stands: Mac is a host, not a UI port. One present path per OS. No
Vulkan, MoltenVK, wgpu or SPIR-V. No `AVPlayer`. The ports stay narrow.

**Why.** The column D9 rejected ("Windows and macOS in v1") was rejected because it would
have delayed Windows v1 behind a Metal lab the Windows user did not need. That reason is
gone: v1 is packaged and the Metal lab exists. Keeping a PR 9–15 Mac catch-up would leave
the Mac a permanent release behind. It would also let `meta/` and `edit/` grow
Windows-shaped before anyone built them on Darwin, the leak D9 exists to prevent.

**Rules.** One PR number, one shared core change, a WinUI half and a SwiftUI half, HLSL and
MSL twins in the same PR, a verify line on each platform, and both present-loop gates
(PR 1 Windows, PR 16 Mac). A PR is done only when both halves hold, and PR N+1 does not start
on either platform before that. PR 15's Mac half is only the twins PR 20 did not already land.

**Carried, not closed by this entry:** the PR 19 real-iPhone HLG check; PR 20's Quick Look
precedence, state across an update restart, and rollback; and Crashpad on Mac (still the
owner's call: PR 17 or PR 20). None blocks PR 9 from starting. Crashpad must be settled
before the first stable Mac release that carries PR 9's new decoders. **Still open:** whether
Milestone G (AI search) follows the dual-track rule. It was proposed Windows-first, and a Mac
half needs an ORT Core ML provider that plan/17 does not specify.

Edited: plan/01 (D9), plan/10 (PR 9–15 rewritten with host halves), plan/15, plan/16,
plan/06 (Mac atomic replace), plan/README, CLAUDE.md, README.

## 2026-09-24 — Ingest with hash dedupe and verify: PR 26, out of the backlog

**Owner's call.** "Card ingest with verify" was a v1.1 backlog row (plan/10, plan/16:
"adjacent product"). The owner asked for duplicate skipping by content hash, not by name,
and for faster sorting of a camera dump. It becomes **PR 26**, dual-track. It may start once
PR 9 holds on both platforms and runs beside PRs 10–15 as its own lane, because it touches
`io/` and the copy path, not `edit/` or `player/`.

**What it is not.** It is not a faster copy engine: throughput is bounded by the card, bus
and disk, and the OS copy is already close to that. The gains are: copying less (a
size-then-BLAKE3-256 duplicate skip, with a cached destination hash index); no second pass
for verification (hash on read, uncached read-back of the destination); overlapping the
card read with the SSD write; parallel work only across different physical devices; and
sorting on the way in (a fixed `YYYY/YYYY-MM-DD` layout from date taken).

**Safety calls.** A name match is never a duplicate, and a name clash with different bytes
is copied under a collision-safe name. `F8` across volumes deletes the source only after
verify. Ingest never deletes from or erases a card. A volume arriving offers the pane and
never starts a copy. RAW+JPEG and Live Photo pairs move as one unit. BLAKE3 is taken under
CC0 (Apache-2.0 alone does not combine with GPL-2.0).

**Still out:** filename templating (v1.1), a catalogue, library-wide duplicate finding,
near-duplicate or burst grouping, backup to a second destination.

## 2026-09-24 (later) — One PR number per feature: Mac 16–20 become the Mac halves of 1–8; Import is an add-on at 16–19; AI moves to 20–24

**Owner's call.** The owner asked for the plan to show Mac and Windows in sync, as they now are,
by renumbering into the order work actually happens. The state was checked in the repo, not
assumed: `pr9-metadata-read-mac` carries PR 9 on both platforms (Mac ahead; Windows owes the
XAML pane, tree island and sort menu), and `claude/compassionate-davinci-5fiwyz` has started
the Mac half of PR 10.

**Renumbering.** Old PR 16 → Mac PR 1. Old 17 → Mac PR 2 + 7. Old 18 → Mac PR 3 + 4 + 6.
Old 19 → Mac PR 5. Old 20 → Mac PR 8 (plus the early Finder part of PR 15). Milestone F is
therefore complete as the Mac halves of PRs 1–8, and both platforms are at PR 9. PRs 9–15 keep
their numbers because 9 and 10 are in flight. The Import add-on takes 16–19 (Milestone G),
superseding this morning's "PR 26 Ingest". Local AI search moves from 21–25 to 20–24 and becomes
Milestone H. **Entries above this one, and branches and commits, keep the numbers they were
written with.**

**Sequencing wording changed to match practice.** From PR 9, PR N+1 does not *merge* until N
holds on both platforms and both present-loop gates hold. A host half may start ahead on a
branch, which the Mac half of PR 10 already has.

**Import becomes an installable add-on** (owner request), like the AI pack: Settings →
Add-ons, a signed manifest, verify before load, a per-user versioned folder, silent updates
with the app, and sideloading. It has a native shared library behind a host function table,
plus a chrome assembly (Windows) or `NSBundle` (Mac). It is absent from the base tree when not
installed. Import builds this mechanism in PR 16. The AI pack (PR 20) reuses it instead of
building its own. The base app keeps one safety fix regardless: `F8` across volumes deletes
the source only after the copy is verified. The feature set widened at the owner's request
("more features, better GUI"):
- the Import window;
- new-since-last-import;
- presets, including per-card presets and opt-in auto-import;
- a second destination from one read;
- layouts and rename-on-import templates;
- camera sidecars kept with their files;
- library tools, including verify-a-folder.

The backlog's "filename templating" is narrowed to renaming files already in a library. Full
design: plan/18. Never offered: deleting from or formatting cards, overwriting destination
files, uploading.

## 2026-09-24 (later) — AI search is dual-track; Mac crash reporting goes in PR 11

**Owner's calls, answering the two open questions from the renumbering entry above.**

**AI search (PRs 20–24) is built on both platforms**, like every PR from 9. Mac runs the same
ONNX model through ONNX Runtime's **Core ML provider** (Apple GPU / Neural Engine), with the
CPU provider underneath. The Mac Compute toggle is Auto / Core ML / CPU only. Core ML is part of
macOS, so there is no vendor sub-pack. The pack installs through PR 16's add-on mechanism,
signed and notarized. Frame sampling on Mac uses a separate VideoToolbox decoder instance,
never the playback one. Both present-loop gates hold while indexing. Indexes are local to one
machine and never synced between platforms, because embeddings differ slightly per backend.
The PR 20 spike measures how many of the model's operators Core ML covers; the rest fall back
to CPU inside ORT.

**Mac crash reporting goes in the Mac half of PR 11.** This settles the open question from the
2026-09-23 PR 20 entry and the 2026-09-17 fold-in: the fold-in put Crashpad in old PR 17, and it
never landed. The Mac half of PR 11 adds Crashpad out-of-process, the same scrub as Windows (no
path, filename, username, pixels or EXIF), and a Swift/AppKit capture path tied to the native
report by the correlation id. Its verify is a Mac canary scan. Until PR 11 lands, the Mac has
no crash capture. A stable Mac release carrying PR 9's new parsers before then must say so in
its notes.

## 2026-09-24 — PR 9 (metadata read) starts on macOS before Windows PR 8 is verified

**Sequencing, the owner's call.** PR 9 is a Milestone D update and plan/10 says PR N+1 waits
for N's verify. The owner asked for PR 9 on macOS first, with Windows to follow on a
Windows machine. Windows PR 8's clean-VM verify is not re-run by this change, and Mac has no
numbered metadata PR (Milestone F stops at 20), so this is the Mac twin of Windows PR 9,
recorded here rather than given a new number. PR 1's present-loop verify is untouched: no
present-path code changed beyond three extra ImGui draws that only run when an overlay is on.

**What landed (shared, portable).** `src/meta` (Exiv2 for EXIF/IPTC/XMP + maker notes,
libavformat for container/stream/chapters, property model, summary rows, overlay lines, AF
geometry), `shell/meta_store` (one read per (path, mtime, size), LRU, and a background
date-taken scan) and `shell/sort_order` (name / mtime / size / type / date taken).
`io::list_subdirectories` feeds the tree. Exiv2 is GPL-2.0 and dynamic-link only, added to the
Mac dynamic manifest with `bmff`, `png`, `xmp` (without `xmp` a PNG's XMP is silently empty).

**Calls made, so they are not re-decided by accident:**
- **Panes float, they do not inset.** The Mac canvas maths has vertical insets only; a
  horizontal one changes the blit and camera, i.e. the present path. The metadata pane
  (right) and folder tree (left) overlay the canvas like the gallery. `chrome_left_px` stays
  unused on Mac. Revisit only with a present-loop soak.
- **New keys.** `I` pane (plan/16), plus `Shift+O` AF points and `Shift+I` eyedropper, which
  plan/16 left unbound. Appended to the table so saved Settings indices hold.
- **Metadata is read only while something shows it,** after a 90 ms pause, so arrow-key
  scrubbing queues no reads. Toggling the pane, `O` or `Shift+O` reads the cached record, never
  the file (unit-tested with an injected reader).
- **Eyedropper** reads one texel of the CPU-visible source texture; not available on video.
- **Sort** was name-only on both hosts (PR 4's other orders never shipped); the Mac now has all
  five. Windows still has none.

**Not verified, owed:**
- **Canon AF-point Y sign.** `canon_af_points` treats AFInfo2 Y offsets as positive-down. No
  Canon file is in the corpus, so a wrong sign would mirror quads vertically. Nikon, Sony,
  Fujifilm and EXIF SubjectArea go through the same tested geometry; only SubjectArea was
  checked on a real file (an iPhone JPEG).
- **HEIC/RAW on real cameras.** HEIC is covered by the in-tree fixtures (dimensions,
  orientation); no RAW metadata was exercised.
- **Windows entirely.** `mv_meta` and its tests are in `CMakeLists.txt` and the vcpkg
  manifest but have never been compiled with MSVC; the XAML pane, tree island and
  Windows `list_subdirectories` do not exist yet. This is the Windows half of PR 9.

### 2026-09-24 (later) — PR 9 Windows: native half built and tested under MSVC

`mv_meta`, `meta_store`, `sort_order` and the metadata tests compile clean under `/W4 /WX` on
MSVC 2022 with vcpkg `exiv2[bmff,png,xmp]` (no source changes were needed). Windows
`io::list_subdirectories` added to `dir_win.cpp` (hidden/system and dot directories skipped,
UTF-16 ordinal case-insensitive sort, one directory read). Full `ctest` from a fresh
`build-pr9`: 514 pass, 0 fail (22 `[meta]` cases, 6 `[sort]`, frametime harness, broken corpus).
**Still owed on Windows:** the XAML metadata pane, folder-tree island, sort UI, `O`/`Shift+O`
overlay drawing, eyedropper, and the ABI calls that feed them (`chrome_left_px` is still 0).

### 2026-09-24 (later still) — PR 9 Windows: overlays, eyedropper and Ctrl+C

Windows now has what the Mac commits `ac62036` / `cb460a3` / `3a6f324` added that is not XAML:
the info overlay's camera/exposure/date lines, AF quads, the "AF points: none recorded" and
"Eyedropper: stills only / move the cursor" notes, the eyedropper, and `Ctrl+C`
(`copy_clipboard`: the colour when the eyedropper has one, else the marked/current file(s) as
`CF_HDROP`, pairs copied whole as F7 does). `meta_store` runs on an `app_state` job system;
selection changes debounce 90 ms; completions post a window message.
- **Eyedropper readback:** image textures are immutable, so one texel is copied to a 1x1 staging
  texture and mapped with `D3D11_MAP_FLAG_DO_NOT_WAIT` on a later frame. The render thread never
  waits on the GPU; the readout shows nothing until the copy has landed rather than a stale texel.
  Only 8-bit RGBA textures are read.
- **Not applicable on Windows:** the Mac tracking-area fix (Win32 already tracks the mouse) and
  the tree-rooted-at-open-folder change (no tree yet).
- **Checked live:** an EXIF-stamped JPEG shows its three lines under `O`; `Ctrl+C` returned
  `#C9B4A1  rgb(201, 180, 161)  x1571 y1832` with the eyedropper on and the file path as a file
  drop with it off. The frametime harness and full ctest still pass; the PR 1 60 s soak was not re-run.
- **Still owed on Windows:** XAML metadata pane (`I`), folder-tree island, sort menu.

## 2026-09-24 — PR 9 Windows half completed (pane, tree, sort, ABI 0.6)

This closes the "still owed on Windows" lists above. What was built and the calls made:

- **Panes float, on Windows too.** The metadata pane (right, 340 DIP) and folder tree (left,
  280 DIP) are two more islands over the canvas, between the command bar and the bottom
  strips. `usable_canvas` has a left inset (`chrome_left_px`) but no right one, and an inset would
  change the blit and camera, i.e. the present path; the macOS host made the same call. So
  `chrome_left_px` stays 0 and opening a pane never refits the photo. Both hide under the gallery,
  Settings and chrome-off fullscreen and come back with them. Revisit only with a present-loop soak.
- **No `TextBox`, no `TreeView`.** Both fail-fast (`0xC000027B`, `Microsoft.UI.Xaml.dll`) in these
  islands: `TextBox` was already known, `TreeView` crashed on first show and was found the hard way.
  The tag search reuses `FakeInput`; the tree is StackPanels and Buttons with its own expand. The
  tag list is a `ListView` of plain elements and did not crash.
- **Sort lives in the ABI session, not the shell.** The Windows listing is owned by
  `mv_session`, so filmstrip, gallery and arrow keys all read one order. `io/sort_order` moved
  from `src/shell` to `src/io` (namespace `mv::io`; the macOS host, tests and `darwin.cmake` were
  updated to match) so `abi -> io` stays legal. New calls, **ABI 0.6**: `mv_folder_set_sort`,
  `mv_folder_get_sort`, `mv_list_subdirectories` ([14](14-abi.md)). The session keeps the scanned
  listing so a new order or a batch of date-taken stamps re-applies without a disk scan; date
  stamps come from `meta::read_date_taken` on one background job, checked per (mtime, size).
  Persisted as `[view] sort`; an unknown key normalises to name.
- **Pane data is three text tables** (`meta/tables.h`, the same formats the Mac bridge documents),
  pushed to the chrome when the record changes. The chrome never reads a file. The tree lists a
  folder on a pool task through `mv_list_subdirectories`, never on the UI thread.
- **Tree open crosses as a pull.** The command callback carries a float, so the island parks the
  chosen path and native pulls it (`chrome_cmd_tree_open` then `TakeTreePath`).
- **Checked live:** an EXIF-stamped JPEG populates Summary and All tags (missing fields show a dash);
  the tree lists subfolders, and invoking one opens it; View ▸ Sort by ▸ Size wrote `sort=2` and the
  ABI test shows the listing re-sorted with the current stop kept. Full `ctest`: 517 pass.
- **Not verified:** PR 1's 60 s present-loop soak (the open D6 gate; only the frametime harness
  ran); PNG, HEIC and MP4 through the Windows pane by hand; the Windows Streams tab on a real clip
  (the table format is unit-tested); the Canon AF sign (unchanged from above); a clean-VM run.
  The macOS build was not rebuilt after the `sort_order` move (no Mac available); the edit is
  mechanical (include path and namespace) but unproven there.

### 2026-09-24 (last) — PR 9 Windows: checked against the plan's verify line, gaps closed

Rechecking the Windows half against [10](10-roadmap.md)'s PR 9 verify line (rewritten on main for
dual-track) found gaps in the first pass, now closed:

- **"The folder tree opens from the keyboard and navigates without the mouse."** It could not: the
  rows were deliberately not focusable. Now `Ctrl+Shift+E` shows **and focuses** the tree, and `I` focuses
  the pane (plan/16). Up / Down walk the rows, Right / Left open and close a folder, Enter opens it (focus
  returns to the canvas), Esc returns to the canvas, and a second Esc closes the pane (plan/16: Esc walks
  out, crop -> pane -> gallery -> fullscreen). New `focus_kind::pane` in the router: a focused pane owns
  its keys except Esc; `view_state.pane_open` is driven by the shown panes and `back_target::pane` closes
  them. A mouse click on a pane control never moves keyboard focus into it, so a mouse user keeps the arrows
  on the canvas. Router tests added.
- **`FocusManager.TryMoveFocus` fail-fasts** in these islands (`0xC000027B`, found by bisecting a crash on
  Down into the search box). Directional focus is therefore not used: the tab bar, the search box and the
  tree handle Left / Right / Up / Down explicitly. Add it to the `TextBox` / `TreeView` list above.
- **The tree follows the watcher.** The plan asks for folder-tree data that follows it. A listing change
  of the open folder (the watcher fires on directory names too) re-lists the root and diffs the rows in
  place, so expanded folders and the focused row survive. Deeper folders are not watched; they refresh when
  opened.
- **Date-taken sort had no end-to-end test.** Added: three JPEGs whose name, mtime and EXIF orders all differ,
  through `mv_folder_set_sort`, including re-sort once the stamps land and back to another key.
- **Pane re-render dropped keyboard focus** when the record arrived after `I`; the tab bar now restores focus
  and identical pushes no longer rebuild the pane.

**Present-loop gate, Windows.** `frametime.exe --seconds 60`: 3597 frames, 0 dropped, p99 17.05 ms at a
16.68 ms refresh, idle 0 presents at 0.26 % CPU: PASS. A lab soak (`--pan-soak`, chrome on) with the metadata pane
and the tree opened during it: 2398 frames, 0 dropped, 0 missed refreshes, p99 17.0 ms. That run fails
the *idle CPU* limit (2.85 % against 1 %), but so does the same soak with no pane open (2.54 %) and the
pre-PR-9 binary (1.99 %), on a machine that was not quiet: not attributable to the panes, and not a
substitute for a quiet-machine run.

**Still not verified on Windows:** PNG-with-XMP, HEIC, a RAW and an MP4 through the pane by hand (no RAW is in
the corpus); the Streams tab on a real clip; a quiet-machine lab soak. **Shared with macOS, unchanged:**
Canon AF sign, HEIC/RAW on real cameras. **macOS:** the `sort_order` move to `src/io` was not rebuilt on a Mac.

## 2026-09-24 — PR 10 (geometry edits + export), Windows and macOS

Built on the PR 9 branch, against the dual-track PR 10 in [10](10-roadmap.md): one shared core
change, two host halves. What was built, the calls made, and where it departs from the plan text.

**Shared (core, both hosts).**
- `src/edit`: the `EditStack` of POD ops (rotate / flip / crop / straighten / resize), folded to one
  canonical geometry (D4 → straighten about the centre → crop → resize); undo = pop, reset = clear.
  `place()` turns it into output sizes and an **affine output → source uv map**. That map is the
  geometry op chain at viewport resolution: the blit samples through it, so a turn, flip, crop or
  straighten costs nothing per frame and never re-decodes. HLSL (`gfx/blit.cpp`) and MSL
  (`gfx/blit_metal.mm`) take it in the same change, line for line. The full-resolution chain runs
  once, on the CPU, on export (`geometry.cpp`): exact pixel copies for rotate / flip / unscaled crop,
  linear-light resampling otherwise. No FP16 working texture yet: geometry does not need one, and
  the D6 working space arrives with PR 11's colour ops.
- **Lossless JPEG** (`lossless_jpeg.cpp`). **Departs from the plan's "libjpeg-turbo `transupp`"**:
  `transupp.c` is jpegtran's source file, not a library, and vcpkg's `libjpeg-turbo` does not install
  it; vendoring it would add a second copy of libjpeg internals. The same algorithm (coefficient
  rearrangement, transposed quant tables, swapped sampling factors) is written on libjpeg's public
  coefficient API instead — platform-neutral, as the plan asks. **Perfect transforms only**: an edge
  that would move a partial MCU into the frame is refused, never trimmed. MCU-aligned crop (top-left
  on the output's iMCU grid, any size). Four quarter turns and two flips give back the original
  pixels bit for bit in 4:2:0, 4:2:2 and 4:4:4.
- **Display-path orientation for JPEG** (closes plan/12 2026-09-14 PR 7 row 4 for JPEG). Decode,
  preview and thumbnails apply the EXIF orientation to the decoded raster; the file is untouched
  (plan/04). RAW previews keep LibRaw's flip; TIFF / PNG / WebP orientation is still not applied.
  Thumbnail spec `jpg512.1` → `jpg512.2`; `meta::display_orientation` reports JPEG as oriented.
- **Export** (`export.cpp`): lossless when JPEG → JPEG with only rotate / flip / an aligned crop,
  re-encode otherwise (JPEG or PNG). Always upright: EXIF Orientation 1, `PixelX/YDimension` = the
  written size, `tiff:Orientation` in XMP = 1, a stale EXIF thumbnail unlinked. Metadata policy
  all / minus GPS / none; ICC always kept. EXIF is patched **in place**, byte-level
  (`codec/exif.cpp`), so maker notes with absolute offsets survive. "Minus GPS" zeroes the GPS IFD
  and every value it points at; an XMP packet naming `exif:GPS*` is dropped whole. Output goes beside
  the original as `<name>-edit.jpg` (`… (2)`), created exclusively.
- **The io replace port, as named in the plan**: `io/replace.h` with `io/replace_win.cpp`
  (`CREATE_NEW`; sibling temp + `FlushFileBuffers` + `ReplaceFileW`) and `io/replace_mac.cpp`
  (`O_EXCL`; same-directory temp + `F_FULLFSYNC`, falling back to `fsync` where a filesystem refuses
  it, + `rename`). PR 12's metadata writer uses the same port.
- **Byte-identical exports on both platforms**: nothing time-, thread- or address-dependent reaches
  the bytes (a test exports twice and compares), and both hosts link the same pinned vcpkg
  libjpeg-turbo / libspng. The cross-machine comparison itself is a manual step of the verify.
- `shell/edit_session`: the hosts' shared edit state — per-file stacks for the session, crop mode's
  draft, the debounced one-at-a-time lossless write with turns carried across the rewrite — and
  `shell/edit_view.h`, which both render threads use to match a texture to its geometry.

**Windows half.** `main.cpp` drives `edit_session` (the commands, the 0.4 s write debounce on a
`WM_TIMER`, jobs on the app's `job_system`, completion by window message). `present_lab.cpp` places
the still through its geometry (the camera frames the edited size; refits when it changes), feeds
the HLSL map, draws the crop overlay, hides AF quads on an edited image and maps the eyedropper
through the edit. Geometry is tagged with the path's `item_key` plus the view generation of the
select, so a rewritten file's new pixels are told from the old texture still on screen.
**ABI 0.7**: `mv_folder_forget` drops a rewritten path from the navigation LRU (keyed by path, it
would otherwise republish the old pixels). Export dialog: a flyout in the command-bar island
(`PopupKind.Export`), TextBox-free (the 0xC000027B fail-fast), keyboard-complete.

**macOS half.** The same, in `main_mac.mm` / `present_lab_mac.mm` (item-id tagging, since every Mac
open has its own id). Export sheet: `ExportView.swift`, the twin of the Windows flyout.

**Calls made, so they are not re-decided by accident:**
- **`[` `]` `H` `V` on a JPEG rewrite that file** (the verify line; plan/04). Rule 5 is met by *what*
  is written: only when the stack holds nothing but rotate / flip, only losslessly (coefficients
  rearranged, or — when the frame is not MCU-aligned — the Orientation tag alone patched in place, or
  a minimal EXIF APP1 added), atomically, after a 0.4 s debounce so `]]` is one half-turn write, one
  write in flight, and only if the file is still the bytes the turn was made against. EXIF with no
  Orientation entry on an unaligned frame is refused (growing IFD0 is PR 12's writer's job).
- **Crop mode's on-screen part is drawn in the canvas overlay on both hosts**, not in WinUI / SwiftUI
  chrome as the plan text puts it: the rectangle has to track the camera every frame, and drawing it
  in an island would put per-frame geometry across the chrome boundary (the loupe and the AF quads
  made the same call). Crop mode's keys go through the one command table.
- **One packed integer for the export choice** (`pack_export`): the island's command callback carries
  a float; the Mac bridge takes the same word, so the two dialogs cannot drift.
- **Edit stacks live for the session only**, keyed by (path, size, mtime, rewrite epoch). The XMP
  sidecar persistence plan/07 calls "truth" is PR 12's.
- **New keys** (plan/16 gave only `[` `]`, `H` `V` and crop's Enter/Esc): `Shift+C` crop mode (`C` is
  clipping), `Ctrl+S` export, `Ctrl+Z` undo edit, `Ctrl+R` reset edits; on Mac `⌘` for `Ctrl`. Crop
  mode is `mode::crop`, the last bit of `mode_mask`: arrows move the rect 1 %, `Shift+arrows` resize
  from the bottom-right, `,` `.` straighten ∓0.5°. A / D, G and `Ctrl+O` do nothing mid-crop.
- **Crop and export wait for a rotation write in flight**; a relist that reopens the same bytes does
  not leave crop mode.

**Not verified, owed:**
- **Nothing in either host was compiled in the session that wrote it** (Linux container: no MSVC,
  no Xcode). The shared core and its suites build and pass there (gcc 13 + ASan/UBSan, clang 18 with
  the Mac flags): `test_edit`, `test_edit_session`, the key-router suites. Windows CI is the first
  MSVC build of the host half and of `test_abi_roundtrip`'s 0.7 case; the C# flyout and the Swift
  sheet are unbuilt. PR 1's and Mac PR 1's present-loop gates were not re-run.
- The Canon / Sony maker-note re-encode (below) on real camera files: the corpus has no RAW in git.

**Closed the same day:**
- **Re-encoded exports carry HEIC / TIFF / RAW / WebP metadata.** `meta::read_carried` builds the
  EXIF block with Exiv2 from what it read, not by copying bytes: a TIFF's or a RAW's IFD0 describes
  *its* pixels (strips, tiles, compression, sub-images, the thumbnail IFD, DNG private data), and
  those tags are left out; Orientation is written as 1. Maker notes are kept while the block fits one
  APP1 and dropped (not the whole block) when it would not. `edit/` and `meta/` are siblings, so
  `shell::run_export` reads it and hands it to `edit::export_image`; the policy applies to it as to a
  JPEG's own. Tested against Exiv2 0.28.3 (the vcpkg line) with WebP and TIFF fixtures written by
  libwebp / libtiff + Exiv2, and the in-tree `iphone_like.heic` (Orientation 6 arrives as 1).
- **An edited tiled (> 64 MP) image draws its tiles**, not just its overview. The tile shaders take
  the inverse map and the source size (`gfx/blit.cpp` `vs_tile` / `ps_tile`): tile corners are source
  pixels mapped to the output, and a pixel outside the edited output is discarded. The tiles are
  chosen for the viewport seen from the source (`shell/edit_view.h` `view_in_source`: the centre
  mapped back, the extent the box of the mapped corners). The Mac has no tiling yet, so there is no
  MSL twin to change.

## 2026-09-24 — Voice query is its own add-on, Milestone I (PRs 27–28)

**Owner's call.** After the add-on mechanism (Milestone G) exists, the owner wants to speak a
search — "pull up all the photos that include…" — and have Local search answer it. Speech is
**not** a piece of the AI pack. It is a separate Settings install, [19-voice.md](19-voice.md).

**Where it sits.** It needs PR 16's add-on host and PR 22's text query, so it does not merge
before PR 22. Numbers are **27–28, Milestone I**. PR 25 stays unused (freed when AI search
moved to 20–24). PR 26 stays folder tiles. Voice and folder tiles do not block each other.
Not a numbered D-decision. Nothing here changes the PR 1–8 viewer or the base installer.

**What was decided with it:**
- The utterance is the query. There is no intent parser and no second index. Voice calls the
  host, and the host forwards to the AI add-on. Voice does not open `index.db`.
- Voice installed without Local search searches nothing and does not download Local search
  by itself. Each pack is its own opt-in.
- Recognition is on-device. The mic opens only while the key is held (`Ctrl+Shift+Space` /
  `⌘⇧Space`). The spoken reply is the result count. A wake word, always-on listening, cloud
  recognition, cloud voices, and searching the speech inside a video stay out.
- Mac listens with the Speech framework and `requiresOnDeviceRecognition = true`, and speaks
  with `AVSpeechSynthesizer`.
- **Windows is a spike, not a silent choice.** The OS on-device API
  (`Microsoft.Windows.AI.Speech`) is documented as MSIX plus `systemAIModels`. This app stays
  unpackaged (GPL, direct download, no Store, 2026-09-06). PR 27 measures whether an
  unpackaged process can use it with no outbound connection. If it cannot, the Voice add-on
  carries a small native recognizer of its own (whisper.cpp, MIT). That file stays out of the
  AI pack and out of the base tree. The legacy cloud-capable Windows recognizer is refused
  either way. The viewer's Windows 10 floor does not move.
- The host table gains `search_query` at the end. Older add-ons keep their ordinals.

Full design and both verify lines: plan/19. Not implemented.

## 2026-09-24 — PR 11 (colour adjusts + Mac crash reporting), Windows and macOS

Built on the PR 10 branch (merged into main before this PR; main merged in, including PR 26's folder tiles) against the dual-track PR 11 in [10](10-roadmap.md). What was built, the
calls made, and where it departs from the plan text.

**Shared (core, both hosts).**
- **The D6 working space arrives** (`image/linear.h`, `image/half.h`): linear Rec.709 light stored
  as IEEE FP16, straight alpha, bit-identical on both hosts (portable half conversion, round to
  nearest even, tested over all 65 536 values). A RAW's working image is a new
  `codec::decode_raw_linear` — the *same* LibRaw develop as the viewer's full decode (camera WB,
  matrix, PPG, auto-bright, highlight clip, flip) at 16 bits with a linear curve; a synthetic DNG
  test holds it to the 8-bit decode within 2 codes. Everything else is the viewer's own display
  image (decode → ICC → sRGB) run back through the sRGB curve, so with every slider at 0 the working
  image bakes back to the viewer's pixels *exactly* (tested). **Call:** tagged wide-gamut sources
  are adjusted after conversion to sRGB, and a colour export is written as sRGB with no ICC
  profile — the working primaries are Rec.709 while the swapchain is 8-bit sRGB (D6); a wide-gamut
  working space is a later change to this one module.
- **Plan/07's pipeline cache is the working texture**: decode + linearise once (seconds for a RAW),
  downscale to a ≤ 3072 px preview on the worker, upload once as an immutable FP16 texture.
  Geometry stays PR 10's uv map; colour is uniforms. A slider drag re-uploads 32 bytes; nothing
  re-decodes (the verify's "shader-only").
- **One kernel source, not three twins** (`gfx/adjust_kernel.h`). **Departs from "HLSL and MSL
  twins"**: the colour maths is one token sequence in the subset HLSL, MSL and C++ share; C++
  compiles it (export bake, histogram, tests) and both blits paste the preprocessor-stringified
  text into their shader source. The twins cannot disagree because they are the same text; a test
  pins the subset (no swizzles, no splat constructors, no `lerp`/`mix`/`saturate`). It lives in
  `gfx/` because `gfx/` may not include `edit/`. Order is plan/07's: WB → exposure (one per-channel
  gain, WB normalised to keep a grey's luminance) → contrast (a power about 18 % grey in linear
  light, i.e. a slope in log exposure, ±100 → ×1.5 / ÷1.5) → saturation (towards Rec.709 luma).
  Temperature is ±100 mireds from D65 on the CIE daylight locus; tint scales green by 2^∓0.5.
- **Bake** (`edit/bake.h`): export with any colour op decodes the full-resolution working image,
  applies PR 10's geometry (exact copy, or its supersampled bilinear, now reading FP16) and the
  kernel, encodes sRGB. Never lossless. Tested against a double-precision write-out of the chain
  (≤ 1 code), against the preview path (≤ 1 code), and byte-identical across two runs.
- **Histogram** (`edit/histogram.h`). **Departs from "a compute reduction"**: a CPU reduction on a
  worker over the preview working image *after* the kernel and display encode (≤ 1 M samples),
  debounced 120 ms after the sliders settle. A compute shader would have been a fourth copy of the
  kernel per platform for a readout that updates at settle, not per frame. Clipping uses the
  blinkies' thresholds (`kClipHighLinear` / `kClipLowLinear`), so the percentages count what `C`
  blinks; `C` itself now tests the *adjusted* colour in both shaders.
- **Stack.** `op_kind::adjust` sets one parameter; `param == count` resets all colour (one undo
  step). A slider drag coalesces into one op; a drag back to the start leaves none. **A stack with
  any colour op is never written back to the file**: `[` `]` on such a JPEG stay in the stack and
  bake on export (rule 5 — PR 10 only rewrites a pure rotate/flip stack). Colour set while a
  lossless write is in flight is carried to the rewritten file.
- `shell/adjust_pane`: the pane's shared state — readiness (none / preparing / ready / failed),
  tokens so a result for an item the user has left is dropped, histogram requests, and one POD
  (`adjust_view`) both chromes draw. The working image is wanted while the pane is open **or** the
  item has colour ops; until it lands the blit runs the kernel on the 8-bit texture, so an adjusted
  photo never flashes unadjusted. **The sliders stay disabled until the working image exists**
  (plan/07's RAW call, on the verify line).

**Windows half.** `present_lab` takes the FP16 texture through a mutex hand-off it only ever
`try_lock`s (the render thread never waits on a worker), created by the worker on the render
thread's free-threaded device; a device rebuild drops it. `main.cpp` drives the pane, the build job
(cancelled through its own job_context generation when the item changes) and the histogram job.
The WinUI pane (`IslandHost.Adjust.cs`) is a third panel island on the metadata pane's edge (one at
a time), built of `Slider`s (already proven in the transport island). It is a focused pane
(`focus_kind::pane`, shown with focus on `Shift+A`), so the sliders own the arrows (Tab walks,
arrows step) and Esc returns to the canvas. The C# chrome was compiled (warnings as errors) on Linux with the
Windows App SDK's manifest tool stubbed; the HLSL compiles under DXC (vs/ps 6.0); the C++ host
was syntax- and warning-checked with clang against mingw-w64 headers (`-Wall -Wextra -Wconversion
-Wshadow`, nothing in changed lines). MSVC has not built it.

**macOS half.** The same, in `main_mac.mm` / `present_lab_mac.mm` (atomic-exchange hand-off like
`pending_image_`, `image::upload_linear` → `RGBA16Float`), `AdjustView.swift` / `AdjustStore.swift`
over new `mv_chrome_adjust_*` bridge calls, and the MSL blit pasting the same kernel. The pane is
keyboard-complete without Full Keyboard Access (↑ ↓ pick, ← → step, ⇧ ×10, 0, R, Esc).
**Nothing Mac was compiled** (no Xcode here).

**Mac crash reporting** (owed since old PR 17): see [13](13-updates-and-telemetry.md) "As built in
PR 11 (macOS)". The scrub gained POSIX paths and bundle-module rules, tested on a synthetic
Mac-shaped dump with the verify's canaries (runs on Linux in the existing scrub suite, which the Mac
test target now builds too). `crashpad` joins the root manifest for `osx`; `macpack.py` ships the
handler in `Contents/Helpers` and signs it before the app. `tools/mac/crash_canary.py` (+ tests in
CI) replaces the two PowerShell verify scripts on Mac.

**Calls made, so they are not re-decided by accident:**
- **`Shift+A`, not `E`** ([16](16-commands.md) asked PR 11 for another key: `E` is the clip
  transport). The Shift twin of **A**djust, as PR 9/10 did for O, I and C.
- **Adjust and metadata panes share the right edge**; opening one closes the other.
- **Colour exports are sRGB, untagged**; geometry-only exports are PR 10's, byte for byte.
- A pre-existing 9-byte heap overflow in `test_minidump_scrub.cpp` (a 19-byte marker copied into a
  10-byte tail) was fixed; ASan found it once the suite ran on Linux.

**Not verified, owed:**
- Every host verify on both platforms: slider latency on a 45 MP RAW (≤ 1 refresh), export vs
  preview on real files, the cross-platform "same sliders, same bytes within 8-bit rounding" check,
  the HLSL/MSL twins on real GPUs, and PR 1's and Mac PR 1's present-loop gates with the pane open.
- The MSVC build of the Windows host, the Xcode build of the Mac host, the Swift chrome, the MSL.
- Whether vcpkg's `crashpad` port builds for `arm64-osx` at the pinned baseline, and every step of
  the Mac crash verify (minidump from the handler, canary scan, NSException with its cid, relaunch).
  **Done on this Mac, 2026-09-24** — see the entry below. The adjust-pane verify and the Windows
  half are still open.
- Mac has never had the `C` blinkies (a Mac PR 6 gap, not PR 11's); the accurate-RAW clipping is
  therefore Windows-only until that lands.
- Eyedropper readouts still sample the unadjusted 8-bit texture.
- Main's keyboard-navigable panes (`focus_kind::pane`) arrived in the merge; the adjust pane uses
  them (shown with focus, Esc returns to the canvas) instead of posing as a text field.

## 2026-09-24 — Mac crash verify: stack fragments and the NSException record

The Mac crash-reporting verify failed on two criteria. Both are closed. The calls:

- **Unrooted stack fragments.** A decode crash left `PRIVATE_FOLDER_canary` in a scrubbed dump,
  in thread-stack bytes. The bytes were a path whose root a later frame had overwritten
  (`pad/canary/PRIVATE_FOLDER_canary`). The scrub only masked a path that starts at a drive, a
  UNC prefix, or a POSIX root — the residual recorded on 2026-09-14. Thread-stack bytes now also
  mask a run of two or more components. A space ends a component, so the run does not swallow the
  prose around it. A `://` URL and a relative `./` or `../` run are left, including on the stack.
  The same run outside a stack is left, so `/System/Library/…` stays available for symbolication.
  A single folder name with no separator can still survive.
- **NSException chrome record.** AppKit catches an exception raised in event handling and calls
  `-[NSApplication reportException:]`, which traps in `_crashOnException:` and does not call
  `NSUncaughtExceptionHandler`. `NSApplicationCrashOnExceptions` was already on, so Crashpad wrote
  a dump and `Crashes/chrome/` stayed empty. The record is written from `reportException:` (and
  still from the uncaught handler, for an exception that escapes the run loop) before the trap.
  One record per crash.
- **Report metadata.** Replacing a dump with `rename` dropped Crashpad's extended attributes, so
  the next launch logged that it could not read the report. The rewrite copies the attributes
  onto the replacement first.

Verified here with `mediaviewer_lab` and `MediaViewer.app`: a decode crash on the canary folder,
scrub on relaunch, `crash_canary.py scan` with no hits for the folder, the filename, the short
user name, or either pixel pattern; `MV_CRASH_TEST=nsexception` wrote
`Crashes/chrome/…-cid1-nsexception.txt` and the dump's `mv_exception` / `mv_last_call_cid` are
that same id after scrub; `MV_CRASH_TEST=swift_trap` wrote an `EXC_BREAKPOINT` dump and no chrome
record. The Swift runtime's "Index out of range" text is not in the dump — ReportCrash forwarding
stays off, and Crashpad did not capture a crash-info string. No "Failed to read report metadata"
on relaunch; each dump kept its `org.chromium.crashpad.database.uuid` attribute. The adjust-pane
verify and the Windows half were not run.
## 2026-09-24 — D9 amended: Intel Macs ship, as one universal app

**Owner's call.** D9 said "Apple Silicon + macOS 14 only" and deferred Intel Macs as "a second
GPU story for a dying install base." The owner reversed that: the Mac release now runs on Intel
Macs (macOS 14+) as well. This is an amendment to D9's floor, not to its shape — the Mac is
still the Mac halves of the same PRs, one core, no second present path.

**What changed.**
- One **universal** `MediaViewer.app` (arm64 + x86_64): one `.dmg`, one Sparkle `.zip`, one signed
  `appcast.xml`. No per-arch feeds. The appcast no longer carries `hardwareRequirements arm64`.
- Each arch is built **natively** (arm64 on `macos-14`, x86_64 on `macos-15-intel`) with its own
  vcpkg triplet (`arm64-osx` / `x64-osx`, and the `-dynamic` pair for the LGPL dylibs), so the
  Intel unit tests run on Intel. `tools/mac/lipo_merge.py` joins the two `.app` trees; then
  `macpack release` signs, notarizes and packages once.
- **Metal storage mode.** Textures were `MTLStorageModeShared`, which is unified-memory only.
  They are `Shared` when `device.hasUnifiedMemory` and `Managed` otherwise
  (`image/upload_mac.mm`, `player/frame_ring_mac.mm`).

**What is not decided, and is not claimed.** The Rule 4 pacing gate and the PR 1 present-loop
verify were measured on Apple Silicon. **Nothing here says they hold on Intel.** Until they are
run on a real Intel Mac (discrete AMD, Intel iGPU, and the display-link cadence), Intel is
"builds and should launch," not "verified." Video on Intel (VideoToolbox, HEVC/HLG software
fallback speed) is likewise unmeasured. Do not describe Intel as verified before that run.

Windows ARM64 remains deferred.

## 2026-09-24 — Milestone G (Import add-on, PRs 16–19) built ahead on a branch

**What:** the whole Import milestone was written as one change on a branch cut from PR 10's branch
(PR 10 has since merged to main, and main is merged into the branch),
at the owner's request, ahead of PRs 11–15. The dual-track rule allows a half to start ahead on a
branch; it does **not** merge before 15, and it is not done until both platforms' verify lines hold
(plan/10). The four slices share one engine, so they were not split.

**Calls made (none reverses a D-decision):**

- **Module graph:** a new `addon` module (the base app's add-on host) sits beside `meta` / `image`
  and depends only on `io` and `core`; `abi` and `shell` may include it. Add-ons live in
  `src/addons/<name>`, may include only `core`'s header-only pieces and themselves, and link nothing
  of the core. `tools/check-module-graph.ps1` and `check-hostable-core.ps1` enforce both.
- **F8 across volumes now copies through the verified-copy path** on both hosts (plan/18: "stays in
  the base app regardless"). Before, Windows checked the size and the Mac checked nothing.
- **One add-on signing key: the update-manifest Ed25519 key**, verified in C++ with libsodium on both
  platforms (Windows had it only in C# BouncyCastle). The Mac bundle is additionally Developer
  ID-signed for library validation.
- **New dependencies:** BLAKE3 (taken under CC0-1.0) in `io`, libsodium (ISC) in `addon`. Both are
  permissive and in THIRD-PARTY.md. The add-on links SQLite (public domain).
- **A headless Linux build of the shared core (`cmake/portable`)** runs the Import suite under
  ASan/UBSan. Its CI job is `tools/portable/ci-portable-core.patch`: the writing session could not
  push workflow files, so the owner applies it. It is a test build, not a product platform: v1 is still Windows, the Mac is the second
  host, and nothing in D9 changes. It exists so the engine every PR from here shares is proved on
  every pull request without a Mac runner.
- **Merged with main after PR 10 landed** (same day): Import's chrome notifications moved to
  1010 / 1011 and its command ids after PR 26's `folder_up` / `folder_prev` / `folder_next`; the ABI
  goes to 0.9. The Mac add-on follows D9's Intel amendment: one universal package (`macos`),
  joined with `tools/mac/lipo_merge.py`, unmeasured on Intel like the app.
- **Import's commands are table rows gated at run time** (`set_addon_commands_available`), not
  rows added and removed: the table stays static, `?` / Settings / the router hide them while the
  add-on is absent.
- The smaller calls (units all-or-nothing, `{seq}` committed at job start, per-device writer lock,
  "interrupted" as a resumable state, Enter vs open-in-viewer) are in
  [18 "Implementation notes"](18-import.md#implementation-notes-2026-09-24).

**Not verified, owed:**

- **Nothing in either host was compiled by a platform compiler in the session that wrote it** (Linux
  container). Shared core: built and tested with gcc 13 (ASan/UBSan and TSan). Windows-only C++: a
  MinGW `-Wall -Wextra -Wshadow -Wconversion` syntax pass, not MSVC. WinUI C#: both projects compile
  with the .NET 8 SDK on Linux with the Windows packaging steps disabled. Swift / Objective-C++: not
  compiled at all (no Swift toolchain); `Import.bundle`'s C header was checked with a C compiler.
- Every hardware verify line in plan/18 (timing vs the OS copy, eject, a real unplug, the 2,000-file
  grid, the 10 s ETA) and **both present-loop gates while importing**.
- Clip posters in the Import grid, and the Mac finish notification (a beep until notification
  permission is requested).

**Review follow-ups (same day), decided by the owner:**

- **Duplicates per destination:** a duplicate on the main destination still goes to a backup that
  lacks it (the backup mirrors the card). Was: skipped for both. Backup copies no longer count as
  library copies for the main destination's duplicate test.
- **Resume per destination:** a file a crash left on one destination only is kept there and copied
  to the other. Was: that member failed as "name taken".
- **No add-on downgrades:** install refuses an older signed version than a working installed one,
  so a replayed old manifest cannot roll the add-on back.
- Review fixes that change no call: a hidden file or link beside an add-on is refused; a member that
  fails on one destination removes what it wrote on the other; the UI thread no longer reads
  `import.db`; the host table's volume-watch deadlock. See the PR.

## 2026-09-25 — Milestone H may use 3 GB for search quality; folder-tree indexes persist

**Owner's call.** The 250 MB limit belongs to the base viewer, not to the optional Local search
add-on. The supported installed AI components — Core model and runtime, one selected provider,
and Faces when installed — may total up to **3 GB**. The search `index.db` and existing thumbnail
cache are user data and are outside that ceiling. CI rejects an oversized supported pack
combination and the installer refuses it. The base installer and PR 8 packaging assertion do not
change.

ViT-B/32 remains the quality floor, not the target. PR 20 compares it with at least one larger,
licence-clean SigLIP-so400m / ViT-L/14-class tower and chooses by labelled-query recall,
throughput and the 3 GB ceiling. The intended experience includes natural phrases such as
"guy on a skateboard"; minimising the optional download is secondary to a material recall gain.

Indexing is explicit and persistent. With the Core pack installed, an open folder offers **Index
this folder** and **Index this folder and subfolders**. Chosen roots are stored in `index.db`;
later launches scan only new/changed/removed files and resume partial video work rather than
rebuilding the tree. Progress includes measured rate and an ETA range. PR 21 records completion
ranges for a representative 300,000-asset photo-heavy library and a separately described mixed
photo/video library; no fixed duration is promised before that measurement.

The 2026-09-24 multi-folder entry called PR 26 "Milestone H" before the later renumbering assigned
that name to AI search. PR 26 remains folder tiles/breadcrumb/up, but is now labelled a standalone
PR so Milestone H is unambiguous.
