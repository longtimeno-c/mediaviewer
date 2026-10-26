# Gate 4 — the films, built without the owner's takes

2026-10-26. The owner asked for the videos now, before recording the takes. So the footage is
captures I took myself of the installed MediaViewer 0.1.37 on this Mac, driven in the
background on a folder of the repo's CC0 samples (4 RAWs from `tools/testmedia/raw`, the libheif
HEIC, and an 8 s HEVC clip encoded from that HEIC with ffmpeg). Contact sheets:
`reviews/gate-4/Film{A,B}-{16x9,9x16}-sheet.png`.

## What is real, and what is not

- **Every window in Film A is a real capture** (`public/caps/`, gitignored): viewer, the arrow
  run, the gallery, metadata pane, 100 % pan, the video playing, the Edit pane (crop, colour), the
  Video Editor and the Driftwire window. Nothing was saved or exported in the app; the filmstrip
  was turned back off afterwards.
- **Captures are stills, cut hard** (the style guide's "hard cuts to finished states"). No
  footage was sped up. The pan is the 100 % view and the view after one drag, not a smooth pan:
  smooth motion needs a screen recording.
- **Numbers are the Windows measurements** from `docs/perf`, captioned "Measured on Windows" on
  each beat and on the close; the footage is the Mac.
- **Film B's send is the driftwire CLI's own renderer** with its made-up example data. I made the
  data agree across cards: sender "ThinkPad" (the CLI's own short name), receiver "Ann's MacBook",
  one send of 24 files, 412 MB (the gallery's sending cards said 4.8 GB). Every rename is the same
  length or shorter so no card's frame moves.
- **The Driftwire window** (the one real GUI shot in Film B, and in Film A) shows the owner's
  machine names. They are covered with solid boxes; a blur stayed readable.

## Left out, and why

- **Local search window.** It opened on the owner's own Photos library (faces, personal clips),
  so I closed it without capturing. The search beat shows the measured accuracy from
  `docs/perf/search.json` instead.
- **People, Import, themes, F3 overlay.** No faces in the samples; no card to import; changing the
  theme changes the owner's settings; F3 can't be sent to a background window.
- **Driftwire GUI flow** (Nearby tiles, typing the code, the path picture moving). Needs two
  machines; the CLI carries it.

## Found while capturing

- The metadata pane shows the Sony ARW's **Comment as "??????…"** (MediaViewer 0.1.37, Mac). The
  film crops above it. Worth a look as a bug.
- The README's 42 MP pan p99 (16.95 ms) belongs to the animated soak; the RAW soak is 17.25 ms
  (`reviews/gate-1.md` §3). The film uses 17.25.

## Swapping in the owner's takes later

Each beat reads its capture by name (`src/films/FilmA.tsx`); replace a still with an
`<OffthreadVideo>` of the matching take from `assets/takes/` and re-render. The timeline and
sound cues stay in `src/films/timeline.ts`.
