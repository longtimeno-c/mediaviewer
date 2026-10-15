# Plan — Local search over standalone audio files (MP3, M4A, M4P)

Local search (the AI pack, [17](../design/17-local-ai-search.md)) indexes a **video's soundtrack**
for what it sounds like (CLAP) and what is said (Whisper). MP3, M4A and M4P now open in the viewer
([audio-and-documents](audio-and-documents.md) §2.2), but "Standalone audio files are not indexed":
`media_kind_of_name` (`src/addons/ai/engine.cpp:127`) knows photo and video extensions only, and
`asset_kind` is `{photo, video}` (`src/addons/ai/index_db.h:76`). This plan makes audio files
searchable with the models the `ai-audio` piece already ships, behind the File types setting.
Issue #355. Written 2026-10-15.

## Status (2026-10-15)

Slice A1 is being built (this branch). A2 is not started.

**Taken from the issue (#355):**

- Audio files are indexed **only while Settings → File types "Show audio files" is on**. It is off
  by default ([04](../design/04-image-pipeline.md) "Leaving kinds out of a listing"). The rules are
  in §1.
- The per-folder Pictures / Sound / Both option applies to audio files as it does to clips.
- No new model. CLAP and Whisper are already in `ai-audio`.
- M4P (FairPlay) is indexed from its tags and cover art only. Its audio is never read.

**Owner calls taken (2026-10-15):** standalone audio is searchable, behind "Show audio files"
(§8, call 1). Whisper runs on music, so lyrics are searchable (§8, call 2).

## 1. The gate: "Show audio files"

The setting belongs to the base app (`settings::show_audio`, `src/shell/settings.h:60`; a view
flag, `kSettingShowAudio`). The pack can't read it. Its host table gives it only its own settings
page (`settings_get`).

**Route: the chrome forwards it, as it forwards "Index videos for".** A new pack setting
`audio_files` (0/1, default 0) goes through `set_setting` and is listed in `settings_json`. Each
chrome sends it when the pack loads and every time the File types toggle changes. Windows: the
base chrome calls `ISearchChrome.OnShowAudioChanged` (a default interface method, so an older
search chrome still loads) on attach and from `ApplySettings`. Mac: `setViewFlags` and
`loadSettings` call `MvAddonsShowAudioChanged`, which sends the optional `-showAudioChanged:` to
the AI chrome. The pack keeps it in its `settings.json` beside `video_index`, which a reader (a
later window, the Final Cut Pro agent, [23](../design/23-nle-search.md)) loads too.

A host-table getter (`hidden_kinds`, appended) was the other option. It is not needed: the chrome
already owns the moment the toggle flips, and a getter would still need a change event to drive
the drop and re-queue below.

| State | Behaviour |
|---|---|
| **Off** (default) | `media_kind_of_name` answers 0 for `mp3` / `m4a` / `m4p`, so the walk drops them before any row, stat or queue entry. Search adds `MV_AI_KIND_AUDIO` to no result, and counts and the Show segment leave audio out. |
| **Turned on** | Every enabled root is re-walked for audio only, and the audio files are queued like a newly added folder. Photos and clips already indexed are not touched (their `(mtime, size)` match). |
| **Turned off** | Search drops audio results at once (the kind mask is applied before the rows go). Then one transaction deletes every `kind = audio` asset and its frames (cover art and sound windows), speech, `asset_text` and cues, as removing Sound from a folder drops its audio rows. Queued audio work is cancelled by bumping the generation. |

The per-folder `root_set_media` choice and `doc_index` (when [document-search](document-search.md)
lands) apply only while the setting is on.

**Verify (gate):** with the setting off, a folder of 200 audio files adds 0 rows and 0 ms of
indexing work (`mv_ai_tests`, a `[audio-gate]` case timing the walk with and without the files).
Turning it on indexes all 200. Turning it off leaves 0 audio rows, and a search right after the
toggle returns no audio result before the delete commits.

## 2. What is indexed per file

| Pass | Source | Needs | Per-folder option |
|---|---|---|---|
| **Tags** | Title, artist, album, album artist, genre, year, comment, through the host's `tag_facts` (`fields_utf8`). The pane already reads them (`meta/clip.cpp`, audio-and-documents §2.2). | Nothing new in the pack. The host fills `fields_utf8` for an audio file. | Always, while the gate is on |
| **Cover art** | The attached picture (`AV_DISPOSITION_ATTACHED_PIC`), as pixels from the host's still entry. The viewer already decodes it as a still for the canvas (audio-and-documents §2.2); the entry learns to answer with it for an audio file. One CLIP embedding at `pts_ms = -1`. No art: no frame row. | Core pack | Pictures, Both |
| **Sounds** | CLAP, 48 kHz, 10 s windows on a 5 s hop, from `audio_open` / `audio_read`: the soundtrack path unchanged | `ai-audio` | Sound, Both |
| **Speech** | Whisper (base or small by the picture tower's quality, as for clips), same guards and re-listen | `ai-audio` | Sound, Both |
| **Cues** | Chapters, through `read_cues`. M4A podcasts and audiobooks carry them; MP3 `CHAP` frames too, where the demuxer exposes them. | Nothing new | Always |

**Default per folder:** the same as clips. Unset means Pictures (tags and art only) without the
audio piece, and Both once it is installed. A music folder whose owner wants it searchable by
"what it sounds like" gets that from the piece being installed, with no extra switch.

**M4P.** The host's container probe already knows FairPlay (`drms` / `sinf` scheme `itun`,
audio-and-documents §1). In A2 `tag_facts` sets a new flag bit, `MV_ADDON_TAG_PROTECTED 0x8` (a
flag, not a struct change, so the table version does not move). The pack never calls `audio_open`
on a protected file: tags and cover art only, whatever the folder option. An unprotected `.m4p`
(rare) and a protected `.m4a` are decided by the flag, not the extension. A1 reads no audio at
all, so it needs no flag: a protected file's tags and cover art are in the header, which is not
encrypted.

**Budgets (recorded in 17 when built).** Music libraries are the long tail here.

- The audio passes run after pictures, as for clips. A folder of photos and songs is searchable by
  picture before any song is listened to.
- Speech on songs transcribes lyrics. That is useful ("the song that says *suited and booted*"), and
  Whisper's no-speech guard already skips instrumental windows. Cost on the Windows dev box: base
  at RTF 0.075 on 2 CPU threads is ~18 s per 4-minute track, ~5 h CPU for 1,000 tracks; on CUDA,
  ~7 s and ~2 h. The status names the pass ("Listening to audio · 120 of 1,000") and it yields
  exactly as the soundtrack pass does.
- One file is capped at 3 hours of audio, as cues cap at two hours. Past that, sounds and speech
  stop and the work is recorded as done, so the rest is never re-queued.

## 3. Index

- `asset_kind::audio = 4`. The value 3 is reserved for `document` by
  [document-search](document-search.md) §3; this plan does not take it. `index_db.cpp:75` maps the
  value explicitly instead of "2 is video, anything else is a photo".
- **Schema 8, no new table.** Audio uses `frames` (cover art, and CLAP windows as clips store
  them), `speech`, `asset_text` (tags) and `cues` as clips do. The bump is there so an older pack refuses the index ("a newer add-on's
  index", `index_db.cpp:304`) rather than reading kind 4 as a photo. The migration only sets the
  version.
- `media_kind_of_name` returns 3 for audio (`0 none, 1 photo, 2 video, 3 audio`), and only when
  `audio_files` is on. The scan's `seen_file` takes `asset_kind::audio`. Audio files never pair:
  `pair_names` leaves them alone, and the Live Photo branch only looks at kinds 1 and 2.
- `progress` keeps one row per (asset, spec), so tags, art, sounds and speech each resume and
  re-queue independently. Installing or removing `ai-audio` queues or drops an audio file's sound
  and speech rows exactly as it does a clip's.

## 4. Search

- `MV_AI_KIND_AUDIO 8u`. Documents take `4u`; whichever plan lands first widens `MV_AI_KIND_ALL`
  (to 11, or to 15 with documents), and the old value 3 keeps meaning photos and videos for an
  older chrome. The read-only reader for Final Cut Pro asks for photos and videos only.
- **Query syntax** (`query.*`, one parser for both chromes, FCP and voice): `is:audio`,
  `is:music`, `is:song`, and a trailing `audio` / `songs` / `tracks`. Bare `music` and `song` stay
  query words, because "piano music" is a CLAP query. `said:` and quoted phrases match an audio
  file's transcript as they match a clip's. A plain word matches tags by the existing tag rule, so
  `beatles` finds the album.
- **Ranking:** unchanged per tower. Sounds by margin over generic prompts, speech and cues by word
  coverage, tags first as `MV_AI_MATCH_TAGS`. Merged per file, best moment first, "N more in this
  file" as for clips.
- **Results:** the tile is the cover art, or the music-note glyph tile the viewer already draws.
  It shows `mm:ss` as a clip moment does, and the snippet is the matching line or tag.
  `mv_ai_result.kind` is `MV_AI_KIND_AUDIO` and `pts_ms` is set. **Enter** opens the file and seeks
  to the moment, the same path a clip moment takes (Windows `SearchWindow.cs:989` keeps `PtsMs` for
  videos only today; it keeps it for audio too). `N` / `Shift+N` step through moments.
- **Opening a result while "Show audio files" is off** can't happen: results are gated by the
  same setting. A result opened from an `.mvindex` search scope still opens, because a file opened
  directly always shows ([04](../design/04-image-pipeline.md)).

## 5. Settings and search panel

### Settings → Local search (both hosts)

```
Index videos for  [Pictures | Sound | Both]
  Audio files     Off — turn on "Show audio files" in File types to search them.   ← gate off
  Audio files     Indexed as videos are: Pictures is tags and cover art, Sound adds  ← gate on
                  what they sound like and what is said.
Indexed folders
  Music ........ Up to date · 1,000 audio files (12 protected)   Videos and audio: Default ▾
```

- No second per-folder flyout: the existing "Videos: X ▾" becomes "Videos and audio: X ▾" while the
  gate is on. One choice, as the issue asks.
- The folder row counts audio files and protected ones. The status pill names the audio pass.
- Windows: `ManagePanel.cs` (the "Index videos for" row, ~216–240, and the root row ~531).
  Mac: `ManagementView.swift` (`rootRow` / `mediaChoices`).

### Search panel (Ctrl+F / ⌘F)

```
Show:     [All | Photos | Videos | Audio]          ← Audio only while the gate is on
Match by: [Picture] [Sound] [Speech]               ← unchanged; Picture also matches cover art
```

Windows: `SearchWindow.cs` kind segments and `AvailableFinds`. Mac: `SearchPanel.swift`,
`SearchModel.swift`.

## 6. Sharing, privacy, other surfaces

- **`.mvindex`:** an export carries audio assets with their rows. An import skips them for now
  (`transfer.cpp` takes kinds 1 and 2 only), and the receiver's own walk indexes them while its
  gate is on. Importing them, and counting the skipped ones, is A2's.
- **Privacy:** transcripts of voice memos and songs stay in `index.db`, like soundtrack
  transcripts. They are kept out of crash reports, minidumps and telemetry, and *Clear index*,
  removing a root or turning the gate off deletes them.
- **Photos library (Mac):** not affected. It holds no standalone audio.
- **Voice query** ([19](../design/19-voice.md)): gets `is:audio` from the shared parser and nothing
  else.

## 7. Slices

Each slice is one PR, dual-track (D9): a shared core and pack change, a WinUI half and a SwiftUI
half. Both present-loop gates hold, and the arrow-key timings on the camera-dump folder do not
change: with the gate off the scan does exactly the work it does today.

| Slice | Delivers | Verify |
|---|---|---|
| **A1 — Gate, tags, cover art** | `audio_files` setting forwarded by both chromes; `asset_kind::audio`, schema 8; scan, re-queue on and drop on off; album, album artist, composer and genre in the search fields (`kSearchFactsVersion` 3); cover-art frame (`edit::clip::cover_art_rgb`, header only); `MV_AI_KIND_AUDIO`, `is:audio`; Audio segment; tiles and open at file; Settings copy and counts | §1's verify. On a folder of 300 photos + 200 MP3/M4A/M4P (incl. 10 M4P, 2 broken, 1 × 3 h): every file is found by its title and by its artist; a cover-art query ("album cover with a red car") finds the right one; M4P never reaches `audio_open`; broken files counted, never a crash; Mac PR 1 and Windows PR 1 gates with indexing running: 0 drops |
| **A2 — Sounds and speech** | CLAP and Whisper passes on audio files under Sound / Both; `MV_ADDON_TAG_PROTECTED`; `.mvindex` import of audio rows; cues; `mm:ss` tiles, open at moment, `N` / `Shift+N`; the per-folder flyout relabelled; the 3-hour cap | On the A1 set with `ai-audio`: a quoted line from a spoken-word file is found at its time (±2 s) and Enter seeks there; "dog barking" finds the field recording; an instrumental track adds no speech rows; indexing time per audio-minute within 10 % of the same audio as a soundtrack; both present-loop gates with indexing running: 0 drops |

A1 needs no model. Neither slice changes the viewer's own paths.

## 8. Owner calls

Both answered on 2026-10-15 (owner):

1. Standalone audio is searchable, behind File types "Show audio files".
2. Whisper runs on music: lyrics are searchable, and the no-speech guard handles instrumentals.
