# Plan — Local search over documents (PDF, DOCX), and the Windows folder-add fix

Local search (the AI pack, [17](../design/17-local-ai-search.md)) indexes photos and video. PDF
and DOCX now open in the viewer ([audio-and-documents](audio-and-documents.md), merged as #123
and #124), but the pack skips them: `media_kind_of_name` (`src/addons/ai/engine.cpp:112`) knows
photo and video extensions only. This plan makes documents searchable, says which models that
needs, and lays out the Settings and search-panel changes on both hosts. Slice 0 is a Windows bug
found while looking, and it comes first. Written 2026-10-05.

## Status (2026-10-05)

**Owner calls taken (2026-10-05):**

1. English-only text model, inside the 3 GB ceiling. No ceiling raise.
2. PDFium in the pack, on both OSes.
3. `doc_index` defaults to Words.
4. Online-only cloud files get an opt-in fetch, as the Mac's iCloud videos do: OneDrive on
   Windows and, to match, evicted iCloud Drive files in Finder folders on the Mac. It lives in
   the AI pack, not the base app ("keep the base app small").

**Slice 0** is built: see §0 and [17](../design/17-local-ai-search.md) "Cloud files in indexed
folders". It was run on Windows: `mv_ai_tests` (all, plus `[cloud]`) and `mv_import_tests`
(`[port]`: links and junctions not followed, past `MAX_PATH`). The WinUI half compiles. The Mac
half (`cloud_mac.mm`, the SwiftUI rows) is written but has not been compiled or run, so it is owed
on a Mac. The LOADING pill item is dropped: `Look.PillVisible` keeps the pill off during a load on
purpose, because appearing over a busy viewer would cost it a frame. Settings already says
"Getting ready…".

## 0. Windows: "it isn't scanning my photos"

**What the machine shows (2026-10-05, pack 0.1.25).** `index.db` has one root, `Downloads`
(id 2; a root 1 was added and later removed), with all 26 of its images indexed and People
scanned. The Pictures folder, `C:\Users\<user>\OneDrive\Pictures` (Known Folder moved to
OneDrive, 414 images, all local, none hidden), **is not a root**. So the engine scans fine. The
problem is that the folder never became a root, or it was added and then removed. Nothing in the
UI says which.

**Ways a folder silently doesn't get indexed on Windows, in code:**

1. **Settings → Add a folder fails without a word.** `ManagePanel.RootCall` (`src.managed/
   MediaViewer.Ai.Chrome/ManagePanel.cs:606`) swallows every `MediaViewerException`. `busy` (an
   import is running) and `invalid_arg` (empty path) both vanish. `FolderPicker` can return a
   `StorageFolder` whose `Path` is empty when the user picks the *Pictures* library node rather
   than the folder, so the add is a no-op.
2. **Nothing indexes automatically.** Opening a folder never adds a root (`note_folder_opened`
   only reorders existing ones). The "Index this folder" offer exists only inside the Ctrl+F panel,
   only when the scope isn't *Everywhere*, and only when coverage is 0
   (`SearchWindow.cs:1151-1189`). A user who never opens the panel there never sees it.
3. **OneDrive placeholders are skipped (not this machine, but real).** `walk_dir`
   (`src/io/file_port_win.cpp:388`) skips every `FILE_ATTRIBUTE_REPARSE_POINT`. It was written for
   camera cards (Import add-on) and is now reused by the indexer. Files On-Demand placeholders and
   cloud-tagged files carry a cloud reparse tag, so a OneDrive folder with them indexes 0 files.
   It then reads "Up to date · 0 KB", and `folder_coverage` answers 1 forever, so the offer never
   returns. The viewer's own listing (`dir_win.cpp`) doesn't skip them, which is why the photos
   show in the viewer.
4. **A walk error is silent.** `scan_root` returns on any walk failure (`engine.cpp:1614`); no
   status and no log line. Paths past `MAX_PATH` fail because the walk has no `\\?\` prefix.
5. **No pill while LOADING** (`Look.PillVisible`, `Look.cs:267`): the first model load looks like
   nothing happening.

**Slice 0 fix (Windows chrome + `io/`, both hosts' status text):**

- `walk_dir`: skip only name-surrogate reparse tags (`IsReparseTagNameSurrogate`: symlinks,
  junctions), and keep cloud files. A cloud-only placeholder is listed but marked
  (`assets.cloud`, schema 3), because reading it would trigger a download. It is counted as
  "N only in OneDrive" and fetched only with the opt-in `cloud_files` (owner call 4). Add a
  `\\?\` prefix for long paths.
- Add a folder: resolve a library or Known Folder pick to its real path
  (`SHGetKnownFolderPath`, else the library's default save location), and show errors in the panel
  ("An import is running, try again when it finishes", "That isn't a folder on this PC").
- A root whose walk failed or found 0 media files says so in its row ("Couldn't read this folder",
  "No photos or videos found here") rather than "Up to date". That needs a `last_error` and a
  `found` count in `roots_json` (additive).
- First run: when the pack finishes installing and no root exists, offer the **Pictures** and
  **Videos** Known Folders as one-click roots in Settings. Offer them; never add them silently.

**Verify:** on a clean profile with Pictures redirected to OneDrive, with a mix of local and
online-only files, *Add a folder → Pictures* adds `…\OneDrive\Pictures`, indexes every local file,
and lists the online-only ones as "only in OneDrive". Picking the library node behaves the same
as picking the folder. An add during an import shows the message. Both present-loop gates hold.
The walk change also affects the Import add-on: its card copy still never follows a junction
(test in `test_file_port`).

**Before slice 0 lands, to confirm on this machine:** open `OneDrive\Pictures` in the viewer,
press Ctrl+F with *This folder* scope, and click **Index this folder**. If it indexes, causes 1
and 2 were the story. If the row reads "Up to date" with nothing searchable, it's 3 or 4.

## 1. Does document search need another model?

Three things are worth finding in a document, and they need different machinery:

| Want | Example query | Needs | New model? |
|---|---|---|---|
| **Words in it** | `"invoice 4471"`, `lease`, `file:contract` | The document's text, and a keyword match | **No.** The speech search path already does stop-worded, prefix-matched word coverage and quoted phrases (`merge_audio`, `engine.cpp:3721`; `query::contains_phrase`) |
| **What it's about** | `tax return from my accountant`, `wedding seating plan` | A text-to-text embedding of passages | **Yes, one small sentence-embedding model.** CLIP's text tower takes 77 tokens and was trained on captions, and CLAP is audio. Neither ranks passages |
| **What it looks like** | `a receipt`, `a floor plan`, `slide with a chart` | The page rendered and run through the CLIP image tower already installed | **No.** It reuses the Core pack's tower and the host's page render |

A fourth case, **scanned PDFs with no text layer**, needs OCR. The OS has OCR in the box
(`Windows.Media.Ocr`, Apple Vision `VNRecognizeTextRequest`), so it needs no model, but it is a
port per host and it is slow. It's a later slice (§4, slice D4).

**Recommendation:** ship keyword and page-picture search first with **no new model**, in the Core
pack. Add semantic text search as a new optional piece, **`ai-docs` ("Documents")**, holding one
small text-embedding model, the same way Sound is an optional piece.

### Model candidates for `ai-docs`

| Model | Licence | Params | fp16 / int8 size | Tokenizer | Languages |
|---|---|---|---|---|---|
| all-MiniLM-L6-v2 | Apache-2.0 | 22 M | ~45 / ~23 MB | WordPiece | English |
| bge-small-en-v1.5 | MIT | 33 M | ~67 / ~34 MB | WordPiece | English |
| multilingual-e5-small | MIT | 118 M | ~235 / ~118 MB | SentencePiece (XLM-R) | ~100 |
| snowflake-arctic-embed-s | Apache-2.0 | 33 M | ~67 / ~34 MB | WordPiece | English |

All have ONNX exports and run on the existing ORT `embedder` (`src/infer`) with a new tokenizer.
WordPiece is ~150 lines; SentencePiece unigram is more. Pick by a spike like PR 20's: a labelled
set of ~500 real-world PDFs and DOCX (receipts, contracts, papers, manuals, slides-as-PDF), 50
natural-language queries, P@5 and R@10, CPU passages/s at 2 threads, cosine to the ORT-Python
reference ≥ 0.999.

**The 3 GB ceiling is the binding constraint.** The worst supported combination is already
~2.93 GB (17, "The AI pack"). That leaves ~70 MB: an int8 English model (~23–34 MB) plus
PDFium (~6 MB, below) fits, and multilingual-e5-small does not. Whisper indexes speech in any
language, so English-only document meaning would be an odd gap. **Owner call 1:** stay English
(int8 bge-small or arctic-embed-s) inside 3 GB, or raise the family ceiling to ~3.2 GB for
multilingual-e5-small int8. Raising it needs a row in `12-decision-log.md`.

## 2. Getting text and pages out of documents

The core renders pages and nothing else (`decode_pdf` / `decode_docx`, `src/codec/decode.h:86,97`).

- **DOCX text, no new dependency.** `src/codec/docx.cpp` already builds a paragraph/run model
  (`item`, `paragraph`, `table`, `document`, `:417-450`) before layout. Factor the zip + styles +
  `body_reader` stage into `codec::docx_text(bytes) → paragraphs`, with headings and table cells
  flattened in reading order. Page numbers come from asking the layout for its page breaks; that's
  optional, and paragraph order alone is enough for search.
- **PDF text: Windows has no API for it.** `Windows.Data.Pdf` only renders. The Mac's PDFKit
  (`PDFPage.string`) does extract text, but two engines would give two indexes for the same file,
  and an `.mvindex` shared between machines would disagree. **Recommend PDFium** (BSD-3 /
  Apache-2.0, ~6 MB) **in the pack, not the base app**: it's dynamically loaded, the base installer
  doesn't grow, the extracted text is the same on both OSes, and the viewer keeps rendering with
  the OS (the audio-and-documents §2.4 call is unchanged). PDFium is hostile-input code: it runs on
  the pack's background workers with a per-document time and memory cap, a password-protected PDF
  is recorded as *locked* and skipped, and `tools/fuzz/fuzz_pdf_text.cpp` covers the wrapper.
  **Owner call 2:** PDFium in the pack, or PDFKit on Mac plus OCR-only on Windows.
- **Pages as pixels.** Host table **v3** (append-only, 17 "ABI"): `document_open(path) →
  page_count`, `document_page_rgb(doc, page, max_long_edge)`, `document_text(doc, cb)` (DOCX via
  `docx_text`; PDF through the pack's PDFium, so the host answers `unsupported` and the pack does it
  itself), `document_close`. Rendering stays in the core's codecs: one renderer per OS, the same
  pixels as the viewer.

## 3. Index, search and results

**Assets.** `asset_kind` gains `document = 3` (`index_db.h:52`); `media_kind_of_name` adds `pdf`,
`docx` (magic-byte probing still decides in the host). D5's "Open With only" is unaffected:
indexing a document doesn't make the app its default.

**Schema (index.db schema 3, migration adds tables only):**

```
passages(id, asset_id, spec, page, ord, text)          -- ~1–3 sentences each, ≤ 256 tokens
passage_vecs(passage_id, spec, scale, generic, emb)    -- ai-docs only; int8 like frames
frames(…)                                               -- page pictures: pts_ms = page index, flags |= PAGE
```

The plan doesn't reuse `speech`. `said:` means spoken, and speech rows' `start_ms` is a time that
snippets and scrub markers read as one. Passages get their own table and the keyword path is
generalised to search either. `progress` gains specs `doctext/1` (text extracted),
`docpage/<clip spec>` (page pictures) and `<text-model spec>` (vectors), so each is resumable and
re-queued independently, exactly like pictures, sound and speech.

**Limits (budgets, recorded in 17):** text from every page up to 500 pages; page pictures for the
first 3 pages plus any page that is mostly image (PDFium's image-object area > 50 %); passages
capped at 20,000 per document. The text phase is cheap and runs before page pictures, so words are
searchable first.

**Search.**
- `MV_AI_KIND_DOCUMENTS 4` (kind bits become 1 | 2 | 4; `ALL` widens to 7, so the old value `3`
  keeps meaning photos + videos for an old chrome). `MV_AI_FIND_TEXT 0x80` is the document-text
  tower.
- Query syntax: `is:document` / `is:pdf` / `is:doc`, a trailing `document`/`documents`/`pdf`; a
  quoted phrase matches speech **and** document text; `said:` stays speech-only; new `text:` /
  `says:` means document text only. `query.*` is one parser for both chromes, FCP and voice.
- Ranking: passages rank in their own units (keyword coverage; with `ai-docs`, the margin over
  generic prompts, calibrated in the spike the way CLAP's margin was) and merge per document, best
  page first, "N more in this document". "Nothing found" keeps its rule per tower.
- **Results:** the tile is the page's thumbnail (stored as `path#page=n` in the JPEG-512 cache via
  the host, the moment-thumbnail path), with "p. 12" where a video shows `mm:ss` and the passage as
  the snippet. **Enter** opens the document on that page (`mv_folder_select_page`, ABI 0.16).
  `N` / `Shift+N` step through matching pages, as they do through a clip's moments.

**Sharing (`.mvindex` v2):** passages, passage vectors and page frames travel like speech rows,
keyed by the text model's spec. A v1 file still imports.

**Privacy.** Document text is the most sensitive thing the index has held. It stays in `index.db`
like transcripts, and it is excluded from crash reports, minidumps (passage buffers out of the
dump filter, like decoded image heaps) and telemetry. Settings says plainly that an index of
documents contains their text. *Clear index* and removing a root delete it.

## 4. Settings and search panel

Both hosts already split Settings → Local search into an outer layer (piece rows, base app) and
an inner one (the pack's own panel). Documents fit the existing patterns. "Index videos for" is
the model for a pack setting with a per-folder override; Sound is the model for a feature gated on
an optional piece.

### Settings → Local search

```
Local search
  Pieces
    Core ............................ Installed  1.18 GB   [Remove]
    People .......................... Installed     87 MB   [Remove]
    Sound ........................... Not installed  1.0 GB [Install]
    Documents (new) ................. Not installed   40 MB [Install]
      "Search PDF and Word files by what they're about, not just their words."
    NVIDIA acceleration (Windows) ... Installed    205 MB   [Remove]

  [status pill]  Indexing · 1,240 of 3,100 · Documents: 210 pages
  Compute ......... Auto ▾
  Precision ....... Broader ─────●───── Stricter
  Index videos for  [Pictures | Sound | Both]
  Documents (new)   [Off | Words | Words and pages]          ← default: Words
      Words: find text inside PDFs and Word files.
      Words and pages: also find pages by what they look like (receipts, plans, slides).
      With the Documents piece, Words also matches by meaning.
      "An index of documents holds their text on this PC. It never leaves it."
  Indexed folders
    Downloads ....... Up to date · 26 photos · 9 documents   Videos: Default ▾  Documents: Default ▾
    OneDrive\Pictures  Couldn't read this folder  [Retry]    (slice 0 states)
    [Add a folder…]   Suggested: Pictures · Videos           (slice 0, first run)
  Index ........... 412 MB  [Clear]   Cap 8 GB ▾ (Windows gains the cap picker Mac has)
  On battery ...... Pause below 30 % ▾
  Import and export
  People
```

- **Setting:** `doc_index` (0 off, 1 words, 2 words and pages) in `settings_json` / `set_setting`.
  The default is **1 (Words)**, because text extraction is cheap, needs no model and is what people
  expect "search" to do. Turning it off drops the rows (like removing Sound) after a confirm
  showing the size freed.
- **Per folder:** `root_set_documents(id, MV_AI_DOCS_DEFAULT | OFF | WORDS | PAGES)`, shown as a
  "Documents: X ▾" flyout beside the existing "Videos: X ▾" (Windows `ManagePanel.cs:571-603`;
  Mac `rootRow`/`mediaChoices`, `ManagementView.swift:779-800`). The case is a Downloads folder
  that should search photos and not your bank statements.
- **Gating:** "by meaning" needs `docs_ready` in settings_json and `MV_AI_STATUS_DOCS_READY` in the
  status. Without the piece, Words is keyword-only and the hint says the piece adds meaning, the
  pattern of `ShowVideoIndex` / `audio_ready` (`ManagePanel.cs:392-407`).
- **Placement:** Windows: the outer piece row in `IslandHost.Addons.cs:93-97` and
  `IslandHost.LocalSearch.cs:542-552` (and the offer probes at ~400 / ~894), the inner row after
  "Index videos for" (`ManagePanel.cs` ~212). Mac: the `pieces` array (`LocalSearchView.swift:53-61`),
  the inner section after "Videos" (`ManagementView.swift` ~686).
- **Status:** the pill and Settings name the document pass ("Reading documents · 210 pages") like
  the Mac's named passes (#127's stage chips). `assets_locked` is counted ("3 protected PDFs
  skipped").

### Search panel (Ctrl+F / ⌘F)

```
[ tax return 2023                                    ]
Show:     [All | Photos | Videos | Documents]                ← new segment
Match by: [Picture] [Sound] [Speech] [Text]                  ← Text is new; dimmed + tooltip when doc_index = 0
Look in:  [This folder and subfolders | Everywhere]

 ┌──────────┐ ┌──────────┐
 │ page img │ │ page img │
 │   p. 3   │ │   p. 1   │
 └──────────┘ └──────────┘
 "…total tax due for the 2023 tax year…"     ← snippet, as for speech
```

Windows: `SearchWindow.cs:285-297` (kind segments), `:316-330` (Match by), `AvailableFinds`
`:667-729`. Mac: `SearchPanel.swift:447,462`, `SearchModel.swift:160-178`.

## 5. Slices

Each slice is one PR, dual-track (D9: a shared core change, a WinUI half and a SwiftUI half),
with both present-loop gates and the arrow-key timings on the camera-dump folder unchanged.

| Slice | Delivers | Verify |
|---|---|---|
| **0 — Folder add fix** (Windows-led, status text both) | §0 | §0's verify line |
| **D1 — Document words** | `asset_kind::document`, host table v3, `docx_text`, PDFium in the Core pack, `passages`, keyword search over passages, `doc_index` + per-folder setting, Documents kind segment + Text token, page tiles, open at page, `is:document` / `text:` | On a folder of 300 photos + 200 PDFs/DOCX (incl. 2 encrypted, 2 broken, 1 × 500 pages): every text document searchable by a quoted phrase from page 1 and from its last page, Enter opens on the matching page; broken/locked files counted, never a crash; indexing 200 documents ≤ 60 s on the Windows dev box CPU and the M5; PR 1 soak with indexing running: 0 drops |
| **D2 — Pages as pictures** | `docpage` spec, page frames through CLIP, "Words and pages" | 20 labelled visual queries ("a receipt", "a floor plan") P@5 ≥ 0.8 over the D1 set; no new model loaded |
| **D3 — Documents piece** | `ai-docs` piece, chosen text model + tokenizer, `passage_vecs`, meaning ranking, calibration, `.mvindex` v2, ceiling decision | The spike's P@5 / R@10 targets on the 50-query set; embeddings within cosine 0.999 of ORT-Python on CPU on both platforms; `addon-pack.py ceiling` passes |
| **D4 — Scanned PDFs (OCR)** | OS OCR port (`Windows.Media.Ocr`, Vision), only for pages with no text layer, background and yield-gated | 50 scanned pages: ≥ 90 % of a page's words found by quoted search; a 100-page scan doesn't hold up the photo pass |

D1 and D2 add no model, so they can land without owner call 1.

## 6. Owner calls

All four were answered on 2026-10-05: see Status at the top.
