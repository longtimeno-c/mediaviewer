# 23 — Local search inside an editing app (Final Cut Pro search)

How Local search ([17](17-local-ai-search.md)) is reached from inside Final Cut Pro: a
workflow extension panel, a search agent that hosts the AI pack read-only, the wire between
them, and the FCPXML hand-off. Issue #71.

The panel searches the index MediaViewer already built, and its results drag into an FCP
event or timeline. Nothing is re-indexed and nothing leaves the machine. The FCP pieces are
Mac-only; the layer beneath them (the read-only reader, the wire, FCPXML, `mv-nle-export`) is
shared and builds on both platforms.

**Delivery.** The agent and the extension are the **Final Cut Pro add-on**'s (`fcp`, arm64,
Mac only; since 2026-10-19, issue #485, [12](12-decision-log.md) "Final Cut Pro search is an
add-on"). Settings > Local search offers it in its Final Cut Pro row once the Local search Core
pack is installed ("Install and turn on"). Not installed, nothing of it exists; installed and
off, the agent is not registered and the extension is not registered, so nothing runs and Final
Cut Pro lists nothing. The app keeps `MediaViewer --search-agent` and the agent's launchd job.

## Shape

```
Final Cut Pro (sandboxed) ─ Extensions ▸ "MediaViewer Search"
  MediaViewerSearch.appex  (sandbox + app group; AppKit panel; drag = FCPXML + file URLs)
        │ NSXPCConnection, Mach service "<team>.io.github.longtimeno-c.mediaviewer.fcp.search"
  MediaViewer --search-agent (the app's own executable, started by launchd on demand via
                            SMAppService, registered when Final Cut Pro search is on)
        │ loaded_addon::load(store, "fcp")  → mv.fcp.1 run_agent (the add-on's listener)
        │ loaded_addon::load(store, "ai", …, MV_AI_READER_ENTRY_SYMBOL)  (lent through mv_fcp_services)
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
| The add-on: agent (`mv.fcp.1`), container + extension | `src/addons/fcp`, `src/nle/mac`, `cmake/darwin-fcp.cmake`, `packaging/macos/fcp`, `tools/package/addon-pack.py` | Mac |
| The host: `--search-agent`, the launchd job, on / off, install offer | `src/shell/fcp_host_mac.mm`, `tools/mac/macpack.py`, `src/shell/fcp_mac.mm`, `LocalSearchView.swift`, `SlotItems.swift` (`FinalCutOffer`) | Mac |
| Test client | `src/nle/mac/client_mac.mm` (`mv-search-client`) | Mac |

### The add-on, off until turned on

`build/addons/fcp` (arm64; `addon-pack.py pack --addon fcp`) holds:

```
libmv_fcp.dylib                         the agent: the Mach listener, peer checks, idle exit (mv.fcp.1)
MediaViewer Search.app/                 background-only container (LSBackgroundOnly, an empty main)
  Contents/PlugIns/MediaViewerSearch.appex   the workflow extension
```

macOS lists a workflow extension only from an app bundle LaunchServices has registered:
`pluginkit -a` on a bare `.appex` succeeds and the extension is never discovered. The container is
that bundle. Nobody opens it, and installing is one add-on in Settings. Its manifest contributes
nothing, so no window loads the add-on.

The agent is MediaViewer's own executable: `macpack.py assemble` puts its launchd job in
`Contents/Library/LaunchAgents` (the job runs `MediaViewer --search-agent`; `SMAppService`
registers only a job inside the bundle). `main()` hands over to `MvSearchAgentMain`
(`shell/fcp_host_mac.mm`) before the viewer starts anything: it loads the verified `fcp` add-on
and calls its `run_agent` with `mv_fcp_services`, which open the AI pack's reader at the first
call and answer searches, roots and completions from it. The add-on links nothing of the core.

Settings > Local search shows **Final Cut Pro** once Core is installed (`mv_fcp_state`):
**Install and turn on** while the add-on is missing (`FinalCutOffer`), then **Turn on** /
**Turn off**. On registers the agent (`SMAppService`), the container (`LSRegisterURL`) and the
extension (`pluginkit -a`, `-e use`); off reverses all three. While it is on, a background block
10 s after launch registers them again, so an update of the app's launchd job or of the add-on (a
new version folder) is picked up; if the add-on was removed, the block unregisters the agent. An
agent already running when either updates keeps the old code until its idle exit; the wire's
version refuses a reply either side cannot read.

The extension loads `ProExtension.framework` from the installed Final Cut Pro before
`NSExtensionMain` (FCP's extension point needs its `ProExtensionRemoteContext`; MediaViewer ships
no copy). `ProExtensionPrincipalViewControllerClass` sits directly under `NSExtension` in the
extension's Info.plist (`Extension-Info.plist.in`), which is where `ProExtensionRequestHandling`
reads it.

### The reader

The agent is a second host of the pack, not a second search. `mv_ai_reader_get` builds the same
`engine` with `read_only`:

- `index.db` and `faces.db` open `SQLITE_OPEN_READONLY`; no schema is created or migrated, and
  an index of another schema is refused. WAL gives a consistent snapshot while the app indexes.
- Only the answering tower's text half opens (`clip_model::open_text_only`, CPU), plus CLAP's
  text tower when the audio piece is installed. Transcripts are rows; People are names and faces.
- No scans, no workers, no settings file. Every call that would change anything returns
  `MV_ERR_UNSUPPORTED_FORMAT`.
- **Find similar** on an indexed still or moment uses its stored vector: nothing is decoded.
- It catches up with the app at most every 5 s (`PRAGMA data_version`), appending frames with a
  higher id and dropping assets that went or changed. A finished migration (a new
  `active_spec`) reloads.

### The wire

`search_wire.h`: a request is 40 bytes of POD (version, correlation id, scope, kinds, max
results, …) plus the query and scope strings. A reply is `header | rows | moments | blob`, with
a version (`kWireVersion` 1), the correlation id and a status. Limits: 2,000 rows, 512 moments a
row, 16 MiB a reply. Every offset is checked before a byte is read. Only local XPC carries it;
the agent accepts a peer only when it is signed by the agent's own team
(`setCodeSigningRequirement`, from the agent's signature). Tiles cross as JPEG bytes: the
extension's sandbox cannot read the viewer's cache, and the agent serves only files inside that
cache (`thumb_reader`). An agent started before the viewer has made `thumbs.sqlite` serves
placeholders and opens the cache as soon as the file appears.

The agent exits `kIdleSeconds` (50 s) after its last client, or after launch if nobody
connects; no process sits resident. It logs status names only, never a query, path or result,
and makes no network request.

### The hand-off

FCPXML 1.10 (`fcpxml.h`): one asset per file (`media-rep src="file://…"`, the clip's length from
`result_duration`). A video moment becomes an `asset-clip` over −2 s / +3 s of the match
(`fcpxml_options::before_ms` / `after_ms`), clamped to the clip, with a marker at the match and
at the clip's other matches inside the range. A still becomes a 5 s clip (`still_ms`). The query
becomes a keyword over each clip ("MV: …"), so FCP files the drop into a keyword collection; a
switch in the panel's options turns this off. The panel puts the document on the pasteboard as
`com.apple.finalcutpro.xml.v1-10` and `com.apple.finalcutpro.xml`, once per drag, plus a file URL
per item.

### Export results as FCPXML

`mv-nle-export` (both platforms) runs one Local search through the same read-only reader and
writes the result as FCPXML that imports into Final Cut Pro (File > Import > XML), DaVinci
Resolve and Premiere Pro:

```
mv-nle-export "birthday cake" [--out results.fcpxml] [--scope-dir DIR]
              [--photos | --videos] [--max N] [--no-keyword] [--json]
              [--similar FILE [--at MS]]
```

`--json` prints the rows instead (the headless check of the agent's top-K). Exit 0 with
results, 1 with none, 2 on a usage or load error.

### The panel

`extension_mac.mm`, AppKit:

- **Search field.** Live, with a 0.35 s debounce; Return searches at once. The app's query
  language works as typed (`@Anna`, `-night`, `video`, `in:2024`), and named people appear as
  buttons under the field (Tab takes the first).
- **Kind:** All, Video or Photos.
- **Scope:** the open FCP library's folder and below by default (read from FCP over Apple
  Events), any indexed folder, or all of them. The choice is remembered per library, and the
  panel follows FCP when another project opens. When nothing is indexed there, it says so and
  points at MediaViewer (the panel searches only; indexing happens in MediaViewer).
- **Preview:** the selected result plays at its match, with sound (`AVPlayerView`, read-only
  file access). Space plays and pauses, Return plays from just before the match, and N /
  Shift+N step through the clip's other matches. The preview takes no space until a result is
  selected, then at most 40 % of the panel (16:9 when there is room; the grid keeps 140 pt), so
  the search controls stay visible at the size FCP opens the panel.
- **Grid:** the viewer's tiles, with the match time, the clip length and "+N" more matches.
  Hovering scrubs through the clip. Multi-select and drag hand FCPXML plus file URLs to FCP. The
  context menu has Find similar (Cmd+Shift+F), Show in Finder and Open in MediaViewer.
- **Timeline bar** (top): the project the timeline agent works on, dropped here from FCP's browser,
  the folders it may use, and **New Version**; [29](29-agent-timeline.md) "The panel".
- **Options:** the clip handles a drag uses (1 s, 2/3 s, 5 s, 10 s, or the whole clip), whether
  the search becomes a keyword collection, and a fixed test drag (`~/Movies/test/clip1.mov`,
  `clip2.mov`, `photo.jpg`).

## Performance

The agent loads the pack's text towers only, at user-initiated QoS, one search at a time. The
model is the agent's own (hundreds of MB briefly for L/14's text half), not a running app's.
Measured on an Apple M5 (macOS 26.6), Release, over a copy of a 504-asset / 5,830-frame
CLIP L/14 index:

| Check | Result |
|---|---|
| Reader vs the app's engine, 15 queries (subjects, a clip moment, sounds, words said, people, exclusions, kinds, dates, nonsense); index byte-identical after a reader session; catch-up; a session over the wire (`mv_ai_tests "[search-agent]"`, `"[nle]"`) | identical results |
| Agent vs in-process reader, 10 queries × 20, warm, alternating (`mv-search-client --bench`) | agent p50 0.33 ms / p95 2.69 ms; in-process p50 0.14 ms / p95 2.30 ms; identical rows |
| Cold first query (launchd start, pack verify, text tower) | 4.6 s |
| Idle exit after the last client | 52 s |
| An ad hoc-signed client | refused |
| Mac PR 1 present-loop soak with the agent answering 1 query/s | animated cadence held (p99 ≤ 17.1 ms, max ≤ 22.6 ms) |

**The add-on against the in-app agent (2026-10-19, #485 slice 5).** Same M5, Release, a 21-asset
index (the edit eval lab's), the same 5 queries; base = the in-app agent at slice 4, new = the add-on's
agent hosted by `--search-agent`; launchd job per run, alternating base, new, base, new:

| Check | base | new |
|---|---|---|
| Cold first query (launchd start, the add-on's verify for new, pack load) | 8.80 s, 8.19 s | 8.27 s, 8.10 s |
| Agent p50 / p95, 5 queries × 60, warm (`--bench 60`) | 17.37 / 23.07 ms, 16.46 / 21.10 ms | 16.35 / 21.25 ms, 17.07 / 21.49 ms |
| In-process reader p50 / p95, same runs | 17.25 / 21.87 ms, 16.20 / 20.82 ms | 16.11 / 20.25 ms, 16.66 / 20.84 ms |
| Idle exit after the last client | 52 s, 52 s | 52 s, 52 s |
| Results identical to the in-process reader | yes | yes |

The runs overlap: the add-on adds one call through a function table and one copy of each reply.
The app's launch path changed only in the background block 10 s after launch, which now also asks
the store for the add-on while Final Cut Pro is on (one verify of ~1 MB, at background priority).

Repeated queries hit the text-tower cache; a first-time query adds one text-tower run
(~10–30 ms). A 100 k-frame library's scan time is [17](17-local-ai-search.md)'s number.

## Not built

- Crash reporting for the agent (the app's Crashpad and scrubbing are not attached to it).
- People and scenes in the panel, the round trip (`FCPXTimeline`, "Reveal in MediaViewer").
- Panels for other editors (Premiere UXP, Resolve) as a Windows half.
