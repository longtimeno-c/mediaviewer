# 23 — Local search inside an editing app (Final Cut Pro search)

How Local search ([17](17-local-ai-search.md)) is reached from inside Final Cut Pro: a
workflow extension panel, a search agent that hosts the AI pack read-only, the wire between
them, and the FCPXML hand-off. Issue #71.

The panel searches the index MediaViewer already built, and its results drag into an FCP
event or timeline. Nothing is re-indexed and nothing leaves the machine. The FCP pieces are
Mac-only; the layer beneath them (the read-only reader, the wire, FCPXML, `mv-nle-export`) is
shared and builds on both platforms.

**Delivery.** The agent and the extension ship inside MediaViewer.app, off until Final Cut Pro
search is turned on in Settings > Local search, which is offered once the Local search Core
pack is installed. Off, the agent is not registered and the extension is elected out of Final
Cut Pro, so nothing of it runs. There is no add-on manifest entry or separate packing: the
pieces are signed and notarised with the app, and a Sparkle update replaces them with it.

## Shape

```
Final Cut Pro (sandboxed) ─ Extensions ▸ "MediaViewer Search"
  MediaViewerSearch.appex  (sandbox + app group; AppKit panel; drag = FCPXML + file URLs)
        │ NSXPCConnection, Mach service "<team>.io.github.longtimeno-c.mediaviewer.fcp.search"
  MediaViewer --search-agent (the app's own executable, started by launchd on demand via
                            SMAppService, registered when Final Cut Pro search is on)
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
| Agent, test client, extension | `src/nle/mac`, `cmake/darwin-fcp.cmake`, `packaging/macos/fcp` | Mac |
| In MediaViewer.app, on / off | `tools/mac/macpack.py`, `tools/mac/lipo_merge.py`, `src/shell/fcp_mac.mm`, `LocalSearchView.swift` | Mac |

### In the app, off until turned on

`macpack.py assemble` puts `MediaViewerSearch.appex` in `Contents/PlugIns` and the agent's
launchd job in `Contents/Library/LaunchAgents`, arm64 only (the AI pack is arm64 only; a
universal app keeps them as the arm64 build signed them). The agent is MediaViewer's own
executable: the job runs `MediaViewer --search-agent`, and `main()` hands over to
`MvSearchAgentMain` before the viewer starts anything. The app already links everything the
agent needs except `agent_mac.mm` and `mv_nle` (~49 KB).

Settings > Local search shows **Final Cut Pro** once Core is installed (`mv_fcp_state`).
Turning it on registers the agent (`SMAppService`) and elects the extension in
(`pluginkit -e use`); turning it off, or removing Core, reverses both. A fresh install elects
the extension out once, in a background block 10 s after launch. While it is on, the same block
re-registers the agent, so an update that changes the launchd job is picked up. An agent
already running when the app updates keeps the old binary until its idle exit; the wire's
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

Repeated queries hit the text-tower cache; a first-time query adds one text-tower run
(~10–30 ms). A 100 k-frame library's scan time is [17](17-local-ai-search.md)'s number.

## Not built

- Crash reporting for the agent (the app's Crashpad and scrubbing are not attached to it).
- People and scenes in the panel, the round trip (`FCPXTimeline`, "Reveal in MediaViewer").
- Panels for other editors (Premiere UXP, Resolve) as a Windows half.
