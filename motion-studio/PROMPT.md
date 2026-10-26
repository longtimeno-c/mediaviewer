# MediaViewer showcase films — production prompt

A production contract for two code-rendered films, built on the "motion engineering" method
(director's brief → reference → timeline → renderer → critic, with review gates). Every product
fact below comes from `README.md`, `docs/design/28-driftwire.md`, `docs/perf/` and
`src/addons/driftwire/cli/theme.h`. Paste everything under the line into a fresh session run
from the repo root.

---

You are directing and engineering two code-rendered motion films for MediaViewer, a fast,
keyboard-first, private photo / RAW / video viewer for Windows and macOS. You work in
`motion-studio/` and nowhere else in the repo, apart from the README embed in the last gate.

## Ground rules (non-negotiable)

- **Never invent product screens, numbers or features.** Every UI frame is a real screenshot or
  screen recording of MediaViewer or `driftwire`, or a faithful redraw of one you were given.
  Every number is quoted from `README.md` or `docs/perf/*.json`, with its source noted in
  `shotlist.md`. A missing screen is a **blocker**: stop and ask for it, do not draw a convincing
  fake.
- **No "coming soon" features as if shipped.** Linux, voice search, the Final Cut panel and the
  Editor add-on are not built. Leave them out, or show them only on a card labelled "Coming next".
- **Privacy is the brand.** No real person's photos, faces, names or paths on screen. Use CC0
  sample files (raw.pixls.us, libheif samples, as the README screenshots do) and invented machine
  names like "Ann's MacBook" and "Studio PC".
- **Avoid the generic AI reel:** big centred text over a gradient, everything fading in, logo at
  the end. Product footage carries the film; type supports it.

## Inputs

Real assets (inspect them before you plan):

- App icon: `assets/icon/mediaviewer.ico` (extract the largest frame)
- Screenshots: `docs/img/viewer.png`, `docs/img/gallery.png`, `docs/img/frametime-overlay.png`
- Measured charts (redraw them as motion from the JSON; never change a value):
  `docs/perf/*.json` → `docs/img/perf-pacing.svg`, `perf-first-pixel.svg`, `perf-browse.svg`,
  `perf-video.svg`, `perf-av-sync.svg`, `compare-*.svg`, `search-accuracy.svg`
- Driftwire colours (`src/addons/driftwire/cli/theme.h`): Wi-Fi / accent teal `#40C8E0`,
  shine `#A9E6F1`, Bluetooth `#0A84FF`, internet `#5E5CE6`, public DHT `#FF9F0A`, relay
  `#BF5AF2`, success `#30D158`, error `#FF453A`, warning `#FFD60A`, dim grey `#8E8E93`
- Product facts: `README.md` (product page), `docs/design/28-driftwire.md` (Driftwire in full)

**Missing — ask for these at Gate 1 before building anything that needs them:**

1. Screen recordings (Mac, 60 fps, 16:9 at 2560×1440 or larger) of: opening a camera folder cold;
   panning a 42 MP RAW with `F3` on; arrowing through photos; the gallery (`G`); a 4K HEVC clip
   playing; a Local search query ("dog on a beach") jumping into a clip at the matching moment;
   People cards; the Edit pane; the Video Editor timeline; Settings → Appearance cycling three themes.
2. Driftwire, on **two** machines side by side: the Driftwire window with Nearby tiles; the
   receiver's six-digit code; typing it on the sender; Accept with "wants to send you 24 files";
   the path picture with travelling dots and the stats line ("1.2 GB of 4.8 GB · 14 MB/s ·
   about 4 min left"); a Wi-Fi drop and the link moving to Bluetooth then back with no new code;
   *Remember*; Over the internet with a four-word code.
3. The `driftwire` terminal UI: `driftwire send ~/Pictures/trip` to a MediaViewer, recorded with
   asciinema or a screen capture at a large font.
4. A licensed or original music track, or permission to synthesize sound cues.

## Film A — "MediaViewer" (product showcase)

```
FILM
One sentence: Your whole camera dump opens instantly, pans without a dropped frame, and
              you can find anything in it by describing it, without anything leaving your computer.
Audience:     Photographers and people with big camera rolls, on GitHub and social
Duration:     60 s (16:9 master), 30 s cut (9:16)
Formats:      16:9 1920×1080 and 9:16 1080×1920, each its own composition

PROOF POINTS (quote exactly, cite the JSON)
- RAW preview on screen in 44–125 ms; full resolution cross-fades in without moving the view
- 0 dropped frames over 60 s of panning a 42 MP Sony RAW: 3,597 frames, p99 16.95 ms on 16.68 ms
- Next photo ready in 0.2 ms
- 0.08 % CPU and 0 presents on a still: idle means idle
- 0 dropped video frames over 60 s of 1080p HEVC; A/V error p99 7.2 ms
- Measured on a Ryzen 7 5700X3D, RTX 4070, 59.95 Hz display (say so on screen, small)
```

Story states: `DOUBLE-CLICK → FIRST PIXEL → PAN → BROWSE → VIDEO → SEARCH → PEOPLE → EDIT → PRIVATE → DOWNLOAD`

Suggested beats (refine in `shotlist.md`):

```
00–04  HOOK     Double-click a card folder; a RAW is on screen before the click sound ends.
                A timecode counter freezes at its real first-pixel ms.
04–12  SPEED    Pan and zoom a 42 MP RAW with the F3 overlay; the frame-time line stays flat on
                16.68 ms. Overlay "0 dropped frames · 3,597 / 3,597".
12–20  BROWSE   Arrow-key run through RAW, HEIC, video in one filmstrip; RAW / LIVE badges; G
                opens the gallery.
20–28  VIDEO    HDR clip plays on the same canvas; scrub, frame-step, A–B loop.
28–40  SEARCH   Type "dog on a beach"; results; jump straight to the moment inside a clip.
                "Computed on your computer. Nothing is uploaded."
40–46  PEOPLE   Name a face once; every photo and clip of them (sample faces only).
46–52  EDIT     Crop, straighten, Video Editor split. "Originals are never touched."
52–56  MORE     Quick cut: Import verifying a card, Driftwire send, three themes.
56–60  CLOSE    Icon, "MediaViewer", "Windows and macOS · Free and open source", release URL.
                The last frame is the poster.
```

## Film B — "Driftwire" (detailed explainer)

```
FILM
One sentence: Send files to the computer next to you, or anywhere with a code, sealed end to
              end, verified as they land, and it keeps going when the network changes.
Audience:     MediaViewer users and terminal people who would install the CLI on its own
Duration:     90–120 s (16:9 master), 45 s cut (9:16)
Formats:      16:9 and 9:16, separate compositions
Colour:       Driftwire's palette above; teal is the one accent, each bearer keeps its tint
```

Story states: `PICK → NEARBY → CODE → ACCEPT → PATH → DROP → RESUME → VERIFIED → ANYWHERE → REMEMBER → TERMINAL`

Explain each idea with the real UI first, then one diagram, in this order:

1. **Pick, then send.** Shift-click a run in the gallery, the share icon, Driftwire. Or drag out.
2. **Nearby.** Round tiles with a monogram and "Wi-Fi" / "Bluetooth". One click on a tile sends.
3. **The code.** The receiver shows six digits; the sender types them; nothing moves until the
   other person accepts. Three wrong codes end the link. *Diagram:* both sides commit before
   either sees the other's part, the receiver reveals last, so a machine in the middle cannot
   steer both screens to the same digits (protocol 3, `docs/design/28` "The wire", step 5).
4. **Sealed for the long run.** Noise XX with a post-quantum hybrid
   (`Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b`): reading a recording needs both X25519 and
   ML-KEM-768 broken. The handshake is checked against the Cacophony vectors and modelled in
   ProVerif. Keep the cipher name on screen for one readable hold, not as a wall of text.
5. **The path picture.** This computer, the other, the bearer badge between them, dots
   travelling the way the bytes go, the stats line ticking.
6. **It keeps going.** Wi-Fi drops: the dots freeze, "reconnecting…", the badge turns Bluetooth
   blue, the transfer continues from where it was, then moves back to Wi-Fi teal when both are
   on one network. No new code, nothing sent twice. Optional: carry on over the internet.
7. **Verified copies.** Every file checked as it lands and read back before it appears; a
   cancelled send leaves nothing half-written; originals stay where they are.
8. **Anywhere, with a code.** *Send and receive over the internet*: a code that works once, for
   ten minutes, no server in between. Behind a stubborn router, the public DHT makes it four
   words to read out (show the orange DHT badge).
9. **Remember each other.** Tick *Remember* on both; next time it is one click from anywhere. The
   key lives in the Keychain or Windows Credential Manager and it refuses loudly if it changes.
10. **Relays that can't read.** Through another computer of yours or your own relay server
    (purple badge); still sealed end to end.
11. **From a terminal.** `driftwire send ~/Pictures/trip`: the code in big digits, the shimmering
    wait line, the path in characters over an eighth-block bar, the transcript left in
    scrollback. Interactive only: nothing is saved until you answer.
12. **Close.** The three install lines from the README (curl / brew / irm), "No account. No
    cloud." Last frame is the poster.

## Build

- Choose the renderer and say why in `README.md` inside `motion-studio/`. Default to Remotion
  (React, frame-addressed), since both films share components (the path picture, stat counters,
  chart redraws, keycap overlays) and a series is planned. Pick plain Canvas only if you can
  justify it.
- Every frame seekable and deterministic: `frame = render(t, shotlist, assets, tokens, seed)`.
  No hidden clocks, no unseeded randomness. Frame 240 renders without frames 0–239.
- Motion rules by class: keycaps and badges snap with minimal overshoot; cards and panes settle
  (spring ω≈18, ζ≈1, the app's own pan/zoom spring); camera moves slow enough to keep
  orientation; numbers count up then hold long enough to read; Driftwire's dots move at a
  constant rate tied to the shown speed. Not everything springs.
- Type: the app's system faces (SF Pro on Mac, Segoe UI Variable on Windows) and a mono face
  for the terminal and the cipher name.
- Sound on the same timeline: a beat grid in `shotlist.md`; every keypress, click, accept and
  "done" mark lands on a cue. Real key-click and UI sounds you record or synthesize.
- One repeatable render command per film and format (e.g. `npm run render:a:16x9`).

## Files

```
motion-studio/
  brief.md          the two FILM blocks above, plus any facts added at Gate 1
  style-guide.md    palette, type, motion rules, banned defaults, from the reference analysis
  shotlist.md       beats, entry/exit states, purpose, source of every number, beat grid
  assets/           approved icon, screenshots, recordings, audio (and their licences)
  src/              the compositions and shared components
  reviews/          contact sheets and defect notes per gate
  out/              exports
```

## Review gates (stop at each one and show me)

1. **Assets and references.** The asset list, what is missing, and a reference analysis of
   `docs/img/*.png` (palette, type, composition, pacing). Stop for the missing recordings.
2. **Style guide and both shot lists.** Every number with its source. No building before I approve.
3. **Representative stills.** One frame per beat for each film, 16:9 and 9:16.
4. **Rough cut and contact sheet.** One frame per beat plus a strip around each fast transition.
5. **Critic pass.** Judge only the rendered frames, not what was intended. Score hook,
   readability at phone size, continuity, motion, sound sync, product truth (1–5 each). Name
   the three worst defects with timestamp, evidence and a local fix; patch only those and
   re-render those sections. Repeat once.
6. **Formats, audio and delivery.** 16:9 and 9:16 as separate compositions with their own
   contact sheets; listen with the picture off.

## Deliver

- `out/mediaviewer-showcase-16x9.mp4`, `-9x16.mp4`; `out/driftwire-explainer-16x9.mp4`, `-9x16.mp4`
- Poster frames (`.png`) and short looping previews (`.webp` or `.gif`, under 5 MB) for each film
- Contact sheets, source, the assets list with licences, and a note on what still needs a human eye
- **README embed (last, and only after I approve the cut):** GitHub plays an `.mp4` only from an
  uploaded attachment URL, not from a repo path. Prepare: a poster image under `docs/img/` that
  links to the video, placed under the hero screenshot for Film A and at the top of the
  "Driftwire" section for Film B, in the README's product-page voice, no captions about how it
  was made. I will upload the MP4s (release asset or a drag into an issue) and give you the URLs.
  Do not commit video files to git.
