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
| **`publish_slot` is a seqlock, not a double buffer** | Double buffering looks sufficient and is not: with one producer and two slots, a consumer still copying the slot that was live two publishes ago gets overwritten mid-copy and reads a torn snapshot. |

### The verify line, as measured

> **"Presents at exactly display refresh, 0 dropped frames over 60 s, ~0 % CPU idle."**

| Clause | Result |
|---|---|
| Presents at exactly display refresh | **Holds.** p50 = 16.700 ms against a 16.667 ms (60.00 Hz) panel, across every run. |
| ~0 % CPU idle | **Holds.** 16 ms of CPU over 12 s wall with the animation off — 0.008 % of the machine — and the swapchain stops presenting entirely. |
| 0 dropped frames over 60 s | **Not yet demonstrated.** 4-17 dropped frames per 60 s run on the development machine. |

The third clause is **unproven, not failed**, and the instrument says which:
**CPU frame time never exceeds 0.51 ms against a 16.67 ms budget** — the app is not late,
the scheduler is. The drop count also varies by a factor of four between identical
consecutive runs, which is the signature of a noisy machine rather than a systematic
defect.

This is precisely the situation [09-build-and-test.md](09-build-and-test.md) anticipated:
the D6 gate needs a machine with a real GPU, a pinned power profile and nothing else
scheduled on it. **No baseline is committed**, because a baseline captured here would
bake in that noise and quietly lower the bar for every later PR. The gate is wired into
CI behind a `[self-hosted, windows, gpu]` label and is skipped, rather than faked, when
no such runner exists.

**PR 2 must not start until this clause has been demonstrated on a quiet machine.**

---

## Still open

| Question | Blocks | Notes |
|---|---|---|
| ~~**Do we need the Microsoft Store?**~~ | ~~PR 1~~ | **Closed 2026-09-06: no.** App is GPL-2.0-or-later, Exiv2 kept under the GPL, direct download only. See the PR 1 entry above. |
| **A quiet machine for the D6 gate** | PR 2 | PR 1's "0 dropped frames over 60 s" is unproven on the development box, which is noisy. Needs the self-hosted GPU runner [09](09-build-and-test.md) already specifies. |
| **Do WinUI 3 XAML islands hold up?** | PR 3 | Validated early by design. Fallback is a WinUI app with `SwapChainPanel` and an accepted composed frame. |

## How to use this file

Add a row when a decision changes, with the reason — not just the new value. If a decision here is
revisited and *upheld*, add that too; knowing an option was reconsidered and rejected again is worth
as much as the original call.
