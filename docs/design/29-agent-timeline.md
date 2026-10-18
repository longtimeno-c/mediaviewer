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

## Not built

The agent (`MediaViewer --mcp`) and its tools, edits, `analyse`, the editing know-how and its eval
set, the Final Cut Pro add-on, the Script Editor: issue #485 slices 2–6.
