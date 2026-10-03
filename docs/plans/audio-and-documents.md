# Plan — audio and documents (MP3, M4A/M4P, PDF, DOCX)

MediaViewer opens photos and video. This plan adds four file types the owner asked for on
2026-10-03: **MP3**, **M4P** (and plain **M4A**, the same container), **PDF** and **DOCX**, on Windows
and macOS together. It is a forward plan, not a description of the product; when a slice lands,
move what it built into the matching `docs/design/` doc and strike the slice here.

## Status (2026-10-04)

All slices are built and on review branches, stacked in order: docs and the plan (#118), audio
(#121), pages (#122), PDF (#123), DOCX (this branch). Owner calls taken: D5 amended (2026-10-03);
the new types are Open With only and never made the default (owner, 2026-10-03); PDF uses the
OS renderers (§2.4, recommended); DOCX is drawn by our own layout (§2.5 option C), because B
(LibreOffice) is a 300 MB download and A breaks the single-canvas rule. Slice 4 (preview only)
is folded into slice 5: the thumbnail is page 1 of the real layout. What is measured and what is
owed is in `docs/DEVELOPMENT.md`, "Where this actually is". When these land, fold §2 into
`docs/design/` (04, 05) and retire this file.

## 0. Before any code: the scope change

Today the format set is fixed by **D5** in `CLAUDE.md` (camera-dump formats only, "not a movie
player"). Two machine checks enforce it:

- `tools/mac/check_plists.py:107-114` fails CI on any document type outside D5.
- The folder scans (`src/io/dir_win.cpp:48-78`, `src/io/dir_mac.cpp:20-37`) drop every other
  extension, so an MP3 or PDF never appears in a folder.

**Slice 0 (docs only):** amend D5 in `CLAUDE.md` with a dated line ("2026-10-03, owner: add audio
MP3/M4A/M4P, PDF, DOCX"), and record the reason in `docs/design/12-decision-log.md`. Everything below assumes that
amendment.

## 1. What each format needs

| Format | What's in it | Decoder we have | Biggest piece of work |
|---|---|---|---|
| MP3 | Audio, optional ID3 cover art | FFmpeg `mp3` demuxer + `mp3float` decoder (LGPL, already in both builds) | An **audio-only** playback mode: today the player is video-anchored |
| M4A | AAC/ALAC in MP4, optional `covr` art | FFmpeg `mov` + `aac`/`alac` (already built) | Same as MP3; it currently mis-probes as video |
| M4P | AAC in MP4, **FairPlay-encrypted** | None can decrypt it, and we will not try | Recognise it, show art and tags, say plainly it's protected |
| PDF | Vector pages | None | A page renderer + **pages** inside one folder stop |
| DOCX | Zipped WordprocessingML; needs a layout engine | None | Fidelity: there is no small, permissive DOCX renderer |

### M4P, honestly

M4P is Apple's FairPlay-protected iTunes audio (sold until 2009). Decrypting it means circumventing
DRM, which the app will not do, and FFmpeg can't. The app lists M4P files, shows cover art, title
and artist (the `moov` metadata is not encrypted), and the canvas card reads "Protected by Apple
FairPlay — MediaViewer can't play it." Detection: a `drms`/`drmi` sample entry or a `sinf` box with
scheme `itun`. Most M4A files people have today (iTunes Plus, Apple Music downloads excluded) are
unprotected and play normally.

## 2. Architecture

### 2.1 A media kind, end to end

There is no media-kind enum today; "clip vs still" is spread across a folder flag, an extension
helper, and per-host booleans. Introduce one:

- Core: `enum class media_kind { still, clip, audio, document }` in `src/io/pairing.h` beside
  `is_video_name`, returned by one `media_kind_of(name)` used by both folder scans, the pairing
  code and the Mac folder model.
- ABI (minor bump from 15): `mv_folder_item.flags` gains `MV_FOLDER_ITEM_AUDIO` and
  `MV_FOLDER_ITEM_DOCUMENT` bits beside the clip bit; a `mv_probe_media_kind()` replaces
  `mv_probe_is_video()` (keep the old call as a wrapper). Mirrors in
  `src.managed/MediaViewer.Interop/Types.cs` and `src/shell/mv_chrome_bridge.h` / `FolderStore.swift`.
- AI add-on: `asset_kind` stays `{photo, video}`; audio and documents are not indexed (say so in
  `docs/design/17-local-ai-search.md`).
- Routing: the Windows core routes by magic bytes (`video_path()` in `src/abi/abi.cpp`); the Mac
  host routes by extension (`present_lab_mac.mm:831`). Both move to `media_kind`, probed by magic
  bytes in the core, extension only as the scan's cheap filter.

### 2.2 Audio-only playback (MP3, M4A)

- `src/player/container_probe` learns MP3 (ID3 tag / frame sync) and treats the `M4A `, `M4B `,
  `M4P ` brands as audio, not `mp4`.
- New `audio_source` beside `video_source` behind the same `media_source` facade
  (`open_media`): demux and seek on the audio stream, audio is the master clock exactly as in a
  clip, so `av_clock`, `audio_decode`, WASAPI and Core Audio ports are reused unchanged.
- An **attached picture** (`AV_DISPOSITION_ATTACHED_PIC`) is cover art, never a one-frame video —
  fix `video_source.cpp:77` and `poster.cpp:33`, which accept it today.
- Canvas: cover art decoded once as a still (through the normal image path, so it gets colour
  management and the cache) centred on the window background, plus a waveform strip from the
  existing Video Editor waveform code (`docs/design/21-video-editor.md`). No art → a neutral
  music-note card. Idle cost stays zero: the canvas presents only when the playhead line moves,
  at a capped rate (e.g. 30 Hz), and not at all when paused.
- Transport and keys: the `kVideo` command mode (Space, J/K/L, `,` `.`, seek) applies to audio
  items; frame step becomes ±5 s. Same transport chrome on both hosts, the video-only buttons
  (snapshot, trim, Video Editor) hidden.
- Thumbnails: cover art via `image::make_thumb_jpeg`, else the music-note glyph (also used for
  M4P). Today an item with no thumbnail is a blank tile; add proper glyph tiles for audio and
  document kinds on both hosts.
- Metadata pane: already reads audio through `meta/clip.cpp` (`stream_kind::audio`); add ID3/iTunes
  tags (title, artist, album, track, year) to the pane's rows. Read only.
- Not in this plan: playlists, continuous play across files, gapless, an equaliser. Arrow keys
  still move to the next item and stop playback.

### 2.3 Pages (needed by PDF and DOCX, and already wanted for TIFF)

The original spec called for `Ctrl+PageUp` / `Ctrl+PageDown` over TIFF pages, ICO sizes and HEIC
sequences, one navigation stop per file; it was never built (`docs/design/16-commands.md`, Not built). Build it once:

- ABI: `mv_image_info.page_count`, `mv_image_open` takes a page index (or a
  `mv_image_set_page(handle, n)`), a page-change completion. `decode_page()` in
  `src/codec/tiff.cpp:676` becomes the first user.
- Commands: `next_page` / `prev_page` rows in `src/shell/command_table.cpp` (`Ctrl+PageDown/Up`),
  a page indicator ("3 / 12") in both hosts' chrome, and `Home`/`End` within a document when it
  has pages.
- Prefetch: the ±2 window applies to pages of the current file the same way it applies to items.

### 2.4 PDF

**Recommended: the OS renderer behind a port**, like `hwdecode`:

- `src/codec/pdf.h` — a portable port: open, page count, page size, render page at scale into an
  RGBA buffer. Windows impl `pdf_win.cpp` on `Windows.Data.Pdf` (in-box since Windows 8.1; not a
  Store pack, so rule 7 holds); Mac impl `pdf_mac.mm` on CoreGraphics `CGPDFDocument`. No new
  bundled library, no licence change, no installer growth.
- The alternative is **PDFium** (BSD/Apache, what Chrome uses) bundled on both platforms: identical
  pixels everywhere and one code path, at ~5–8 MB per platform and our own security patching. Pick
  this only if cross-platform pixel parity matters more than size. MuPDF is AGPL — avoid.
- Probe: `%PDF-` magic in `src/codec/probe.cpp`; `format_family::pdf`.
- Rendering keeps the viewer's rules: first pixel is page 1 rendered at fit size (cheap); zooming
  re-renders the visible area at the new scale through the existing tile path
  (`docs/design/04-image-pipeline.md`), so it stays sharp at 400 % without rendering the whole page
  at that size. Rendering runs on decode workers only; the render thread only presents textures.
- Colour: rasterise to sRGB. Security: PDFs are hostile input; the renderer runs on a worker with
  a time and memory cap per page, a password-protected PDF shows a "Protected PDF" card (no password
  prompt in this plan), and a fuzz harness (`tools/fuzz/fuzz_pdf.cpp`) covers our wrapper.
- Thumbnail: page 1. Not in this plan: text selection, search, links, forms, annotations,
  printing.

### 2.5 DOCX — the hard one

There is no small, permissively licensed DOCX renderer. The options:

| Option | Fidelity | Cost |
|---|---|---|
| **A. OS preview** — Mac Quick Look (`QLPreviewView`/`QLThumbnailGenerator`), Windows the registered preview handler (`IPreviewHandler`, provided by Word if installed) | Good where available | Hosted OS view, not our swapchain — breaks rule 2's single canvas; on Windows nothing without Office |
| **B. Convert to PDF** with LibreOffice headless, then show it as PDF | Very good | ~300 MB dependency; could be an optional download like the AI packs |
| **C. Own text layout** — parse WordprocessingML (paragraphs, runs, headings, lists, tables, inline images) and paginate with DirectWrite / Core Text | "Readable", not Word-exact | Large, ongoing; we own a layout engine |
| **D. Preview only** — `docProps/thumbnail.*` when present, else first-page text rendered as a simple page; Enter/`Ctrl+O` opens the default app | Low | Small |

**Recommendation:** ship **D** with the media-kind slice (DOCX files appear, get a thumbnail and
a first-page preview, and open in the user's word processor), and decide B vs C as a separate
owner call after D is in use. A breaks the native-canvas rule, so it's listed for completeness.

## 3. OS integration

- Windows installer (`tools/package/mediaviewer.iss`): new ProgIds `MediaViewer.Audio` and
  `MediaViewer.Document`, `OpenWithProgids` + `Capabilities\FileAssociations` lines per extension.
  Offered through Default Apps, never taken.
- Mac `Info.plist.in`: `CFBundleDocumentTypes` for `public.mp3`, `com.apple.m4a-audio`,
  `com.apple.protected-mpeg-4-audio`, `com.adobe.pdf`,
  `org.openxmlformats.wordprocessingml.document`, all with `LSHandlerRank` **Alternate**; update
  `tools/mac/check_plists.py`.
- **Do not auto-adopt.** `MvAdoptNewDefaultViewerTypes` (`src/shell/main_mac.mm:1796-1860`) makes
  the app the default for every newly declared type when the user chose "all supported" — that
  would silently take over their PDF and music handler. Limit auto-adopt to photo and video UTIs;
  offer audio and documents as separate, unticked choices in the defaults sheet on both platforms.
- Explorer thumbnail handler / Finder Quick Look extension: unchanged (stills only). The OS already
  thumbnails these types.
- Import add-on (`src/addons/import/model.cpp:47-67`): unchanged — it copies camera media.

## 4. Slices

Each slice is one PR on both platforms, with the usual gates (both present-loop soaks, launch →
first pixel, arrow-key timings unchanged on the camera-dump folder).

| Slice | Delivers | Verify |
|---|---|---|
| **0 — Scope** | D5 amendment, decision-log entry, commands doc | Docs only |
| **1 — Media kind + audio** | `media_kind` through scan/ABI/hosts; container probe fix; `audio_source`; cover art + waveform canvas; transport for audio; M4P protected card; glyph tiles; ID3/iTunes tags in the pane; plist/installer entries without auto-adopt | A folder of 200 photos + 20 MP3/M4A/M4P: all listed; MP3 and M4A play with A/V clock drift < 10 ms over 10 min; M4P shows art and the protected card; arrow through the folder with no regression in the PR 4 timings; 0 presents and ~0 % CPU on a paused track |
| **2 — Pages** | Page ABI, `Ctrl+PageUp/Down`, indicator, TIFF multi-page as first user | A 20-page TIFF pages in < 40 ms warm; pages are not filmstrip items |
| **3 — PDF** | PDF port (Windows.Data.Pdf / CGPDF), probe, tiled re-render on zoom, thumbnail, protected card, fuzz harness | A 300-page PDF: first pixel < 150 ms, page turn warm < 40 ms, 400 % zoom sharp, pan gate holds on a rendered page; broken/encrypted PDFs show a card, never crash |
| **4 — DOCX preview** | Option D: probe (zip with `word/document.xml`), thumbnail from `docProps`, first-page preview, open-in-default-app | DOCX files listed with a thumbnail; Enter opens Word/Pages |
| **5 — DOCX in-app** | Option B or C, after the owner call | Set when chosen |

Tests per slice: probe cases in `tests/test_probe.cpp` / `tests/test_container_probe.cpp`, routing
in `tests/test_pairing.cpp`, broken files in `tests/data/broken` via
`tools/testmedia/make-seeds.py`, audio fixtures generated by `tools/testmedia/generate.sh` (FFmpeg's
native AAC/MP3 encoders are not needed — the MP3 fixture can be committed small, the M4P fixture is
a hand-built `drms` sample entry with no real key).

## 5. Open owner calls

1. Amend D5 as in slice 0 (required before slice 1).
2. PDF engine: OS renderers (recommended) or bundled PDFium.
3. DOCX beyond preview: LibreOffice as an optional download (B), our own layout (C), or stop at D.
4. Whether the arrow key in an audio folder should keep playing into the next track (this plan
   says no).
