# 23 — Local search inside an editing app (Final Cut Pro search)

Issue #71. Search your footage with Local search ([17](17-local-ai-search.md)) from a panel
inside Final Cut Pro, then drag the matching moments into an FCP event or timeline. It uses the
index MediaViewer already built: nothing is re-indexed, and nothing leaves the machine.

**Owner calls (2026-09-28, [12](12-decision-log.md)):** a Mac-only exception to D9 for the FCP
pieces, with the layer beneath shared and built on both platforms. Phases 0 and 1 first.
Delivery, amended the same day: the agent and the extension ship **inside MediaViewer.app**, off
until the owner turns Final Cut Pro on in Settings > Local search, which is offered once the Local
search pack (the bulk) is installed. Off, the agent is not registered and the extension is elected
out of Final Cut Pro, so [18](18-import.md)'s "absent means absent" holds. There is no separate
app to install.

## Shape

```
Final Cut Pro (sandboxed) ─ Extensions ▸ "MediaViewer Search"
  MediaViewerSearch.appex  (sandbox + app group; AppKit panel; drag = FCPXML + file URLs)
        │ NSXPCConnection, Mach service "<team>.io.github.longtimeno-c.mediaviewer.fcp.search"
  MediaViewer --search-agent (the app's own executable, started by launchd on demand via
                            SMAppService, registered when Final Cut Pro is turned on)
        │ loaded_addon::load(store, "ai", …, MV_AI_READER_ENTRY_SYMBOL)
  libmv_ai (the installed Local search pack)  engine_options::read_only
        │ SQLITE_OPEN_READONLY
  ~/Library/Application Support/MediaViewer/Add-ons/AI/data/index.db, faces.db
  ~/Library/Caches/MediaViewer/thumbs/thumbs.sqlite (tiles, read-only)
```

| Piece | Where | Platforms |
|---|---|---|
| Read-only reader (`mv_ai_reader_get`, `engine_options::read_only`, `index_db::open_read_only`, `faces_db::open_read_only`, text-only towers) | `src/addons/ai`, `src/infer` | both |
| `mv.ai.1` `result_duration` (appended) | `mediaviewer_ai.h`, `Ai.cs` | both |
| `loaded_addon::load(…, entry)` | `src/addon` | both |
| Wire format, `search_session`, `thumb_reader`, FCPXML | `src/nle` (`mv_nle`, `cmake/nle.cmake`) | both |
| `mv-nle-export` (search → FCPXML file) | `src/nle/export_main.cpp` | both |
| Agent, client, extension | `src/nle/mac`, `cmake/darwin-fcp.cmake`, `packaging/macos/fcp` | Mac (D9 exception) |
| In MediaViewer.app, on / off | `tools/mac/macpack.py`, `tools/mac/lipo_merge.py`, `src/shell/fcp_mac.mm`, `LocalSearchView.swift` | Mac |

### In the app, off until turned on

`macpack.py assemble` puts `MediaViewerSearch.appex` in `Contents/PlugIns` and the agent's
launchd job in `Contents/Library/LaunchAgents`, arm64 only (a universal app keeps them as the
arm64 build signed them). The agent is MediaViewer's own executable: the job runs
`MediaViewer --search-agent`, and `main()` hands over to `MvSearchAgentMain` before the viewer
starts anything. The app already links everything the agent needs except `agent_mac.mm` and
`mv_nle`, which add 49 KB to it. Settings > Local search shows **Final Cut Pro**
once Core is installed. Turning it on registers the agent (`SMAppService`) and elects the extension
in (`pluginkit -e use`); turning it off, or removing Core, reverses both. A fresh install elects
the extension out once, in a background block 10 s after launch. While it is on, the same block
re-registers the agent, so an update that changes the launchd job is picked up. An agent already
running when the app updates keeps the old binary until its idle exit; the wire's version refuses
a reply either side cannot read.

### The reader

The agent is a second host of the pack, not a second search. `mv_ai_reader_get` builds the same
`engine` with `read_only`:

- `index.db` and `faces.db` open `SQLITE_OPEN_READONLY`; no schema is created or migrated, and
  an index of another schema is refused. WAL gives a consistent snapshot while the app indexes.
- Only the answering tower's text half opens (`clip_model::open_text_only`, CPU), plus CLAP's text
  tower when the Sound piece is installed. Transcripts are rows; People are names and faces.
- No scans, no workers, no settings file. Every call that would change anything returns
  `MV_ERR_UNSUPPORTED_FORMAT`.
- Find similar on an indexed still or moment uses its stored vector: nothing is decoded.
- It catches up with the app at most every 5 s (`PRAGMA data_version`), appending frames with a
  higher id and dropping assets that went or changed. A finished migration (a new
  `active_spec`) reloads.

### The wire

`search_wire.h`: a request is 40 bytes of POD plus the query and scope strings. A reply is
`header | rows | moments | blob`, with a version, the correlation id and a status. Every offset
is checked before a byte is read. Only local XPC carries it; the agent accepts a peer only when
it is signed by the agent's own team (`setCodeSigningRequirement`, from the agent's signature).
Tiles cross as JPEG bytes: the extension's sandbox cannot read the viewer's cache, and the agent
serves only files inside that cache.

### The hand-off

FCPXML 1.10 (`fcpxml.h`): one asset per file (`media-rep src="file://…"`, the clip's length
from `result_duration`). A video moment becomes an `asset-clip` over −2 s / +3 s of the match,
clamped to the clip, with a marker at the match and at the clip's other matches inside the range.
A still becomes a 5 s clip. The query becomes a keyword over each clip ("MV: …", so FCP files
the drop into a keyword collection; optional, open question 8). The panel puts the document on
the pasteboard as `com.apple.finalcutpro.xml.v1-10` and `com.apple.finalcutpro.xml`, once per
drag, plus a file URL per item.

## Phases and verify lines

**Phase 0 — Spike.** A signed container app with a workflow extension that drags a hard-coded
FCPXML, and a file-URL-only variant.
> *Verify:* On macOS 26 with the current FCP, the extension appears under the Extensions button.
> Dragging a hard-coded FCPXML that references 3 files under `~/Movies/test` (2 clips with ranges,
> 1 photo) into an event creates 3 playable items, a marker at the stated time, and a keyword
> collection `MV: test`. No sandbox prompt, or one documented prompt. The same drop onto the
> timeline inserts the ranged clips frame-accurately. The file-URL-only variant imports too. The
> report records whether `ProExtensionHost` is embedded and under what licence.

**Phase 1 — Search agent.**
> *Verify:* `mv_tests "[search-agent]"` returns the same top-K (ids, `pts_ms`, scores within 1e-4)
> as in-app search for the plan/17 eval query set on the same index. The agent opens `index.db`
> read-only and the test fails on any write. Query p95 in the agent is at most in-app p95 + 5 ms
> (warm). With MediaViewer indexing and the agent answering 1 query/s, the Mac PR 1 present-loop
> soak still passes. The agent exits within 60 s of its last client.

The tests live in `mv_ai_tests` (the AI suite's binary, arm64 on the Mac) under
`[search-agent]`, not `mv_tests`.

**Phase 2 — Panel and drag hand-off** (a grid, scope and kind filters, keyboard-only,
multi-select; built 2026-09-28, see "Phase 2" below), **Phase 3 — People, scenes, the round trip** (`FCPXTimeline`, "Reveal in
MediaViewer") and **Phase 4 — other NLEs** (a Premiere UXP or Resolve panel as the Windows half)
are as issue #71 has them.

## Implementation notes (2026-09-28, branch `fcp-workflow-extension`)

### Phase 1: measured on an Apple M5 (macOS 26.6), Release, a copy of the owner's index (504 assets, 5,830 frames, CLIP L/14)

| Check | Result |
|---|---|
| `[search-agent]` + `[nle]` (reader vs app engine, 15 queries covering subjects, a clip moment, sounds, words said, people, exclusions, kinds, dates and nonsense; index byte-identical after a reader session; catch-up; session over the wire) | pass (8 cases); full `mv_ai_tests` 61 pass, 7 skipped (pack-dependent) |
| Agent vs in-process reader, 10 queries × 20, warm, alternating (`mv-search-client --bench`) | agent p50 0.33 ms / p95 2.69 ms; in-process p50 0.14 ms / p95 2.30 ms; identical rows |
| Cold first query (launchd start, pack verify, text tower) | 4.6 s |
| A real query ("mountain") | 200 rows, all files present, 135 clips with a length, 164 tiles from the viewer's cache |
| Idle exit after the last client | 52 s (`kIdleSeconds` 50) |
| An ad hoc-signed client | refused |
| Present-loop soak (60 s `frametime`, lab with no pack) alternating A (no agent) / B (agent 1 q/s) | animated cadence passed in all four B runs (p99 ≤ 17.1 ms, max ≤ 22.6 ms). Idle CPU was noisy on **both** arms (A 0.39, 0.93, 1.14 %; B 0.79, 2.59, 0.75 %; one A and one B run voided by a display sleep; load average 8.5): **owed on a quiet machine**, and with the app indexing at the same time |

Not measured: the bench's "in-app" is the same reader in-process, not the app's own engine
while it indexes; the index is small (5.8 k frames), so a 100 k-frame library's p95 is plan/17's
`ai-bench` number, not this one. Repeated queries hit the text-tower cache; a first-time query
adds one text-tower run (~10–30 ms).

### Phase 0: in progress in FCP

The extension and the agent build into MediaViewer.app, where they are assembled and signed: the
extension is sandboxed with the app group, and the agent runs as the app's hardened executable.

First run in FCP (owner, 2026-09-28, release 0.1.18):
- The Settings switch registers the agent and elects the extension in (`+`), and FCP finds the
  extension inside MediaViewer.app.
- The extension then trapped on FCP's first connection: FCP's extension point needs
  `ProExtension.framework`'s `ProExtensionRemoteContext`. The fix loads the framework from the
  installed FCP before `NSExtensionMain` (plan/12).
- 0.1.19 loaded and stayed up, but showed an empty black panel ("does not have a panel view
  controller"). `ProExtensionRequestHandling` reads
  `infoDictionary[NSExtension][ProExtensionPrincipalViewControllerClass]`, directly under
  `NSExtension` and not under `NSExtensionAttributes`, then calls
  `initWithNibName:bundle:`. 0.1.20 moves the key.

Still to see in FCP:
- the panel opening;
- what FCP does with the dragged FCPXML (rational-millisecond times, asset durations, sandbox
  access to the `src` files, external volumes);
- the election hiding the extension while it is off.

The panel's first rows are the spike's hard-coded drag (`~/Movies/test/clip1.mov`, `clip2.mov`,
`photo.jpg`).

### Phase 2: the panel (built, owed in FCP)

The panel (`extension_mac.mm`, AppKit; plan/12 says why not SwiftUI):

- **Search field.** Live, with a 0.35 s debounce; Return searches at once. The app's query
  language works as typed (`@Anna`, `-night`, `video`, `in:2024`), and named people appear as
  buttons under the field (Tab takes the first).
- **Kind:** All, Video or Photos.
- **Scope:** the open library's folder and below by default (read from FCP), any indexed folder,
  or all of them. The choice is remembered per library, and the panel follows FCP when another
  project opens. When nothing is indexed there, it says so and points at MediaViewer.
- **Preview:** the selected result plays at its match, with sound (`AVPlayerView`, read-only file
  access). Space plays and pauses, Return plays from just before the match, and N / Shift+N step
  through the clip's other matches.
- **Grid:** the viewer's tiles, with the match time, the clip length and "+N" more matches.
  Hovering scrubs through the clip. Multi-select and drag hand FCPXML plus file URLs to FCP. The
  context menu has Find similar (Cmd+Shift+F), Show in Finder and Open in MediaViewer.
- **Options:** the clip handles a drag uses (1 s, 2/3 s, 5 s, 10 s, or the whole clip), whether
  the search becomes a keyword collection (open question 8), and the Phase 0 test drag.

Owed in FCP: the library scope (the host objects over Apple Events, and the Automation prompt),
the panel's size in FCP's window, and the drag with each handle setting.

### Not in this slice

Crash reporting for the agent (the app's Crashpad and scrubbing
should be attached before it ships). There is no add-on manifest entry or separate packing: the
pieces are signed and notarised with the app, and a Sparkle update replaces them with it.

## Open questions (from issue #71)

1. ~~D9~~ — Mac-only exception for the FCP pieces (owner, 2026-09-28).
2. ~~Delivery~~ — inside MediaViewer.app, off until turned on under Local search (owner,
   2026-09-28; this replaced a separate container app the same day).
3. Model ownership: the agent loads its own text tower (hundreds of MB briefly for L/14's text
   half), rather than asking a running app.
4. Does FCP read FCPXML `src` media from a third-party drag without a prompt, including on
   external volumes? (Phase 0.)
5. ~~`ProExtensionHost` redistribution~~: FCP does not accept an extension without
   `ProExtension.framework` (Phase 0, 2026-09-28). The extension loads the copy inside the
   installed Final Cut Pro at run time and ships none (plan/12, plan/11).
6. Clip range: fixed −2 s / +3 s for now (`fcpxml_options`).
7. Search only; the panel points at MediaViewer to index.
8. ~~Keyword collection from the query~~: on by default, a switch in the panel's options (Phase 2).
