# Motion studio

Code-rendered showcase films for MediaViewer. Not part of the app build: nothing here is
linked, packaged or run by CMake, and the app's no-Node rule is about the product, not this.
The plan and gates are in `PROMPT.md`; decisions in `brief.md`, `style-guide.md`, `shotlist.md`;
review notes in `reviews/`.

## Renderer: Remotion 4.0.250

Both films share parts (the terminal, caption plates, the Driftwire path, stat counters), and a
series is planned, so React components rendered at known frame numbers fit better than a
one-off Canvas script. Every scene is a pure function of the frame: no clocks, no randomness,
so any frame renders on its own (`remotion still --frame=N`).

Each scene is one composition per format (`<beat>-<name>-16x9`, `-9x16`), never a crop.

## Commands

```bash
npm install
npm run stills                 # every still into reviews/gate-3/
npm run stills -- gate-3 B05-code
npm run studio                 # scrub the scenes in a browser
node scripts/ansi-to-json.mjs assets/terminal/gallery-90col.ansi src/terminal-90.json
```

`assets/terminal/*.ansi` come from `mv_driftwire_cli_gallery` (the CLI's own renderer with
made-up data); re-capture them after a CLI look change and re-run the converter.

Fonts: CozetteVector (the app's chrome face, `public/`) for captions; Menlo for terminal shots,
so this renders as intended on a Mac.

## The films

```bash
node scripts/render.mjs                # both films, 16:9 and 9:16, with sound, into out/
node scripts/sheet.mjs FilmA 16x9      # a contact sheet, one still per beat
```

`src/films/timeline.ts` holds both films' beats and sound cues; `scripts/sound.mjs` synthesizes
the soundtrack from it, and the mux normalizes it to −16 LUFS. The captures in `public/caps/`
(gitignored) were taken from MediaViewer 0.1.37 on a folder of the repo's CC0 samples with
`scripts/cap.sh`. The README's copies live in `docs/video/` (Film A re-encoded at CRF 26).
