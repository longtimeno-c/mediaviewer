# 29 — An agent edits the Final Cut Pro timeline

How an AI agent (Claude, or any MCP client) reads the Final Cut Pro project the editor is working
on and edits that footage, using only the timeline's own media and the folders the editor hands
it. Issue #485; the owner's calls of 2026-10-18 are in the issue's "Owner direction" table and in
[12](12-decision-log.md) "Agent timeline editing".

Final Cut Pro has no API to edit an open timeline in place. A workflow extension can read the
active project's name, its playhead and change notifications (`FCPXTimeline`), not its clips; what
it can exchange is FCPXML, both ways, by drag. So the round trip is:

```
FCP browser ── drag the project onto the MediaViewer panel ──▶ FCPXML of the whole timeline
                                                              │ read into the timeline model
agent ◀──────────── reads the model, proposes edits ──────────┘
   │ edits a copy of the model
   ▼
new FCPXML: "<Project> (Claude 1)" in the same event ── imported by FCP beside the original
```

The original project is never changed: each pass is a new project with its own name and no uid,
so FCP makes a new project beside it and the editor compares, keeps or throws away.

## Pieces

| Piece | Code | Platforms |
|---|---|---|
| Lossless XML tree: parse, edit, write back what was not edited byte for byte | `src/nle/xml.{h,cpp}` | both |
| FCPXML time, exact (rational seconds, overflow-checked, frame snap) | `src/nle/rational.h` | both |
| The timeline model and the new-version writer | `src/nle/timeline.{h,cpp}` | both |
| What the panel shares with the agent | `src/nle/bridge_state.{h,cpp}` | both |
| `mv-timeline` (describe, round trip, write a version) | `tools/timeline/main.cpp` | both |
| The round-trip set and its generator; the check | `tests/data/fcpxml`, `tools/timeline/make_fixtures.py`, `check_roundtrip.py` | both (the DTD check needs FCP) |
| The panel's timeline bar | `src/nle/mac/timeline_bar_mac.mm`, in `MediaViewerSearch.appex` | Mac |
| Libraries | `mv_nle_timeline`, `mv-timeline` (`cmake/timeline.cmake`, every architecture) | both |

## Lossless

Everything MediaViewer does not edit must come back exactly as FCP wrote it: effects, colour,
keyframes, retiming, audio components, metadata, and element kinds a newer FCP invents. The tree
keeps the source text and every node remembers its span in it:

- **Write** copies the original bytes of every node that was not changed and holds no changed
  node. A changed start tag is written afresh, but each attribute that did not change keeps its
  original bytes (quotes, spacing, entity spelling); a changed child list writes the unchanged
  children from source. Parse then write with no edit gives the input back byte for byte.
- **Edits** set or remove an attribute, replace an element's text, remove a node (with the
  indentation in front of it), insert one (copying its neighbour's indentation), and clone a
  subtree (an unchanged clone writes its source bytes).
- **Refused:** anything not well formed, with a byte offset; entities other than the five
  predefined ones and character references (FCP writes none; a declared entity is how a small
  file becomes a large one); more than 1,024 levels; 1 GiB or more. No XML library.

## The model

`timeline::load` reads the document's first project (or one named) and lists its items in timeline
order: each storyline element, then what is anchored to it.

| Item | What it is | Editable |
|---|---|---|
| `clip` (`asset-clip`), `audio`, `video` | a range of one file | yes |
| `gap`, `transition`, `title`, `caption` | as FCP has them; a title's text is read from its text-style runs | yes |
| `compound` (`ref-clip`), `multicam` (`mc-clip`), `sync`, `audition`, `container` (`clip`) | opaque: trimmed and moved as a unit, never opened; their media is still the timeline's media | as a unit |
| `storyline` | a connected storyline (a spine with a lane); its clips are items | its clips |
| `other` | an element the model does not know | no: carried as it is, "Claude did not touch" |

Each item has its kind, the item it is anchored to or sits in, its lane, whether it is in a
storyline (and which), its offset, start and duration as written, **where it starts on the
timeline** (sequence time from the sequence's start, exact), enabled, name, resource, role, the
file it plays and the file time of its first frame, title text, markers (with their timeline
position, chapter and to-do markers), and flags: effects (filters and adjustments other than
volume, pan and conform), retimed (a `timeMap` or a rate conform: file time is not 1:1), split
audio (`audioStart` / `audioDuration`: a J or L cut). An item whose time does not parse is not
editable.

Timing rules: a primary-storyline element's `offset` is sequence time (from `tcStart`); an item
anchored to a clip has its `offset` in that clip's own time (which starts at the clip's `start`);
a connected storyline's elements follow one another from its first element; markers are in their
clip's own time. A transition sits over the edit point and does not move the clips around it.

`media_paths()` is every file the timeline plays, including inside compounds, multicams, sync
clips and auditions (through nested media): the scope the agent may use besides the editor's
folders. FCPXML 1.8 and earlier name the file on the asset itself (`src`); later versions use
`media-rep`. File URLs are decoded to paths (`file:///C:/…` to `C:/…`, `file://server/share` to
`//server/share`).

### A new version

`write_version(name)` is the whole document with:

- only this project left in its event, only its event in its library, nothing else at the top
  (other projects, event clips and collections would otherwise be imported again);
- the project renamed (`next_version_name`: "Interview (Claude 1)", then 2, …; a name that already
  ends in "(Claude N)" counts on from it) and its `uid` and `modDate` dropped, so FCP makes a new
  project and never replaces the original;
- resources nothing in the project references, transitively, removed;
- every other byte the source's: the library location and event (uid included), so FCP imports it
  into the same event of the same library, and the assets by the same uid, so it links the media
  already there.

## The bridge

The panel (a sandboxed FCP extension) and the agent (not sandboxed) share one folder in the
panel's app group container, `~/Library/Group Containers/<team>.io.github.longtimeno-c.mediaviewer.fcp/Timeline/`
(`dev.mediaviewer.fcp` in a development build):

| File | Holds |
|---|---|
| `project.fcpxml` | the FCPXML of the last project dragged onto the panel, its bytes |
| `state.json` | `bridge_state`: the project's name, event, library and FCPXML version; when it was dragged; when FCP said it changed after that; what FCP has open now; the playhead and when it was read; the folders the editor gave; the versions written |
| `versions/*.fcpxml` | each new version |

## The panel

A bar at the top of the MediaViewer panel in Final Cut Pro (`MVTimelineBar`):

- **Drop a project** from FCP's browser on it. FCP puts the whole timeline on the pasteboard as
  FCPXML (`com.apple.finalcutpro.xml.v1-N`); a `.fcpxml` file or `.fcpxmld` bundle dropped from the
  Finder works too. The panel reads it (off the main thread); a document that is not a project is
  refused with the reason, and nothing is kept.
- **It says what is held:** "Timeline: “Interview” · dragged 2 min ago". When FCP reports the
  project changed after the drag (its `FCPXTimelineObserver` `sequenceChanged`, 3 s after the drop
  at the earliest), it adds "changed in Final Cut Pro since: drag it again"; when FCP has another
  project open, it says which. One drag re-syncs.
- **Folders:** the folders the agent may use besides the timeline's own media, added with an open
  panel (never `/`) and removed from the same menu.
- **New Version** writes "<Project> (Claude N)" from the held project and asks Final Cut Pro to
  open it (`NSWorkspace`), which imports it into the library the document names. The last version
  is also a chip that drags into an FCP event.
- The playhead (`playheadTimeChanged`, at most once a second) and the open project's name go into
  `state.json` for the agent.

The extension's sandbox gained user-selected read-only file access (for the folder picker); it
still writes only its container.

## Round trip

The set: 20 projects in the shape FCP exports them (`tools/timeline/make_fixtures.py`, each
validated against Final Cut Pro's own DTD for its FCPXML version): an interview; gaps, chapter and
to-do markers and a disabled clip; connected B-roll above and music below; a connected storyline
with a transition; compound clips with a clip connected to one; a multicam with three angles;
titles and a lower third; colour wheels, curves, a LUT, keyframed transform and crop on HLG 4K;
cross dissolves; audio components with J and L cuts; roles on audio and video elements; retiming;
a synchronised clip; an audition; captions; NTSC drop frame from 01:00:00;00; an event holding two
projects, clips and collections; FCPXML 1.8; names and paths that need escaping; a 4-minute
interview with six B-roll clips and a title.

| Check | Result (Apple M5, macOS 26.6, FCP 12.4 DTDs) |
|---|---|
| Parse then write, no edit, all 20 | byte-identical |
| New version of each: renamed, no uid or modDate, same event and library, one project | 20/20 |
| New version describes the same timeline (every item's kind, name, resource, file, lane, position, offset, start, duration, enabled, role, text, markers, flags) | 20/20 |
| Every item's bytes (effects, colour, keyframes, audio components) and the sequence's, in the new version | identical, 20/20 |
| New version against FCP's DTD for its version (1.8, 1.13, 1.14) | valid, 20/20 |
| Unknown elements and attributes, an unreadable time | carried byte for byte, not editable |

`mv_timeline_tests` (`[timeline]`) and `ctest -R timeline_roundtrip` run these.

**Not yet verified in Final Cut Pro itself:** the set is generated, not exported from FCP, and no
version has been imported into FCP and compared clip by clip; the panel's drop, its labels and the
hand-off to FCP have not been run inside FCP (the extension must be Developer ID-signed for FCP to
load it). The issue's verify line asks for 20 real FCP projects dragged in; that run is owed.

## Edits

`nle/edit.h`: the operations the agent's `edit` tool and the Script Editor apply to a timeline,
each a few attribute and child-list changes on the lossless tree.

| Op | Does |
|---|---|
| `cut` from, to | removes that range of the timeline; ripple closes the gap (default), else a gap is left (a lift) |
| `trim` item, from/to or head/tail | an item keeps part of itself; a negative head or tail lengthens it into its media (checked against the asset's length) |
| `split` item, at | two items; anchored clips and markers from `at` on go with the second |
| `remove` item | a storyline item ripples (or leaves a gap); a connected one goes |
| `move` item, before | reorders the storyline |
| `source` item, source_in[, path] | the same clip plays another range of its file, or another file in the scope (another take) |
| `insert` path, source_in, duration[, before] | a clip from the scope into the storyline |
| `connect` path, source_in, duration, at[, lane, with_audio] | B-roll from the scope, connected at `at` (video only unless asked) |
| `marker` at, text[, todo] / `unmarker` item, n | markers |
| `title` item, text | a title's or caption's text (its runs keep their style) |
| `volume` item, dB / `enable` item, on | levels and on / off |

Rules every op keeps: times snap to the sequence's frame grid; only `editable` items change, units
(compound, multicam, sync, audition) only as units; connected clips follow their content (a cut
removes the part of a connected clip inside it, splitting one that spans it; a music clip, role
`music` or audio only below the storyline, plays on through a cut, shortened by what was cut); a
connected storyline a cut crosses refuses the cut; transitions a cut or move touches go; J and L
cuts keep their lead and tail when an edge moves; every change leaves a "Claude: …" marker where it
happened (`edit_options::mark_changes`). A batch is atomic, and its item ids and times are those of
the timeline before the batch: later times are mapped through the batch's earlier cuts and
inserts, so "cut 0:12-0:13 and 0:40-0:41.5" means what it says. A batch with a `move` holds no
time-addressed op. `parse_ops` reads the JSON form (times as seconds, `m:ss.ff` or FCPXML).

## The agent

`MediaViewer --mcp [--timeline FILE.fcpxml] [--folder DIR]... [--no-handoff]` is an MCP server
over the stdio pipes the client starts it with (JSON-RPC 2.0, one message per line; MCP
2025-06-18, 2025-03-26 and 2024-11-05). No socket is opened by the host or the add-on.

```
MCP client (Claude Code, Claude Desktop, …)
   │ stdin / stdout
MediaViewer --mcp   (shell/mcp_host.cpp: main() hands over before the viewer starts)
   │ loaded_addon::load(store, "agent", host table v3: sound, frames)      mv.agent.1 serve
   │ Local search services (search, find similar, transcript) over the AI pack's read-only
   │ reader, opened at the first call (nle/search_session)
libmv_agent / mv_agent.dll   (src/addons/agent: server, scope, transcript, nle/edit)
   │ reads Timeline/ (the panel's bridge), or --timeline FILE
   │ writes versions/ (or beside FILE) and hands a version to Final Cut Pro (open -b com.apple.FinalCut)
```

| Tool | Does |
|---|---|
| `get_timeline` | the working copy (or `original`): items with ids, kind, timeline `at` / `end`, lane, file and file range, role, text, markers, flags, not-editable reasons; length, frame rate, the playhead, the scope's folders; `stale` when FCP changed the project after the drag (the agent is told to ask for a new drag) |
| `transcript` | per storyline clip (or `item`, `from`/`to`, or a scope `path` in file time): phrases on the timeline, refined against the sound; words with estimated times; silences measured in the sound, with `cut_from`/`cut_to` keeping 80 ms of air; "not transcribed yet" where the index has none |
| `search` / `find_similar` | Local search over the scope only: each given folder (and below), and each folder the timeline's files sit in (that folder only), every answer filtered to the scope; which timeline items already use a result |
| `scope_files` | the media files in the given folders (kind, size, length for a short list, whether the timeline uses it); hidden files and packages skipped |
| `edit` | a batch of `nle/edit.h` ops on the working copy; every path must be in the scope; files are probed through the host table |
| `preview_changes` | the change list, runtime before and after, what `commit` would name the version |
| `commit` | writes "<Project> (Claude N)" (the panel's `versions/`, or beside `--timeline`), lists it in the bridge, asks FCP to import it; never the original's name |
| `reset` | the project as last dragged |
| `thumbnail` | a frame as JPEG, only when the editor turned frames on |

The server's `instructions` carry the editing rules every client gets (read first, propose before
changing, cut on silences and phrase edges never inside a word, keep J/L cuts, roles, effects and
B-roll, units stay units, never lose a take the brief needs, one batch in the timeline's own times,
preview then commit, a stale drag means ask for a new one).

### Scope

The timeline's own media (`timeline::media_paths()`, compounds and multicams included) plus the
folders the editor gave (the panel's Folders, `--folder`, or the AI agents window on Windows). The
server checks every path a tool is given and every result before the client sees it (`scope.h`):
`photos:` keys are refused (even when a timeline names one), relative paths are refused, paths are
compared after resolving `..` and symbolic links (a link inside a given folder that points outside
is outside), case-insensitively on macOS and Windows, and `/` or a drive root is never a folder of
the scope. A search query with `photos:` or `in:photos` is refused. Search never asks the pack for
the whole index (`MV_AI_SCOPE_ALL` is refused by the host's services).

### Transcript

The index keeps Whisper's phrases (`mv.ai.1 transcript_json`, appended for this; file time). The
agent lays each phrase of the range an item plays onto the timeline, moves its edges to where the
sound starts and stops (within 1.5 s), shares the phrase among its words by length and moves each
word boundary to the quietest 10 ms within 120 ms, and finds silences from the loudness itself
(16 kHz mono through the host table's `audio_open`, RMS per 10 ms, the threshold 30 % of the way from
the 10th to the 90th percentile, at least 6 dB over the floor, clamped to -60…-28 dBFS). Silences do
not depend on the transcript: a clip not transcribed yet still has its pauses. Word times are
estimates (Whisper's phrases carry no word timing, and Whisper often leaves fillers such as "um"
out); the tool says so.

## Install

The AI agents add-on (`src/addons/agent`, id `agent`, a slot add-on: no chrome of its own). It is
offered with one click in Settings > Add-ons on both hosts while it is not installed; installed, its
card opens the **AI agents** window, a view tree both hosts draw:

- **What the agent sees:** file paths, timecodes and transcript text of the dragged timeline and the
  given folders; a cloud model receives what it reads; no pictures unless frames are on; no network
  connection; status names in logs.
- **Let the agent see frames** (off by default): the `thumbnail` tool. Kept in the add-on's data
  folder (`agent.json`).
- **Claude Code:** finds `claude` (`~/.local/bin`, Homebrew, a login shell's PATH; `%USERPROFILE%\.local\bin`,
  npm's shim, the PATH on Windows) and runs `claude mcp add --scope user mediaviewer -- <MediaViewer> --mcp`,
  or removes it; it shows whether it is added (`claude mcp get`).
- **Claude Desktop:** asks first, then adds `mcpServers.mediaviewer` to `claude_desktop_config.json`
  (every other key kept, the old file kept as `.bak`), or removes it.
- **Another client:** the command line and the JSON entry, copyable.
- **The timeline:** on the Mac, which project the panel holds and its folders; on Windows (no FCP), the
  FCPXML file to work on (exported from DaVinci Resolve or Premiere Pro) and folders to give.

## Not built

`analyse` (the agent asking the app to index), the editing skills and the eval set, the Final Cut
Pro add-on, the Script Editor: issue #485 slices 3-6.
