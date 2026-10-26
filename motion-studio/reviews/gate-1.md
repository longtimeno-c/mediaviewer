# Gate 1 — assets, reference, what's missing

2026-10-26. Status: decisions made (section 5); **waiting on recordings** (section 4).

## 1. Asset list

| Asset | Path | State | Use |
|---|---|---|---|
| Mac icon master, 1254², charcoal field, pale mountain + play, light frame | `assets/mediaviewer-icon.png` | ✅ ships on Mac (`cmake/darwin-app.cmake`) | Close, poster |
| Windows icon, 256², blue gradient, white mountain + play | `assets/icon/mediaviewer-256.png`, `.ico` | ✅ ships on Windows (`src/shell/mediaviewer.rc`) | see decision 1 |
| Chrome typeface: CozetteVector (MIT, licence alongside) | `assets/fonts/CozetteVector.ttf` | ✅ the default theme's UI font on both hosts (`Theme.swift`) | UI captions, terminal |
| Viewer screenshot, 1480×888 | `docs/img/viewer.png` | ⚠️ old chrome (Sep 24): the lab's Open/View/Settings/About bar, not today's command bar | reference only |
| Gallery screenshot | `docs/img/gallery.png` | ⚠️ same, 8 test files incl. colour-bar test clips | reference only |
| F3 frame-time overlay | `docs/img/frametime-overlay.png` | ✅ the overlay is unchanged; chrome behind it is old | crop the overlay for Film A beat 2 |
| Perf reports | `docs/perf/*.json`, `investigation/video-native-60s*.json`, `av-after-120s.csv` | ✅ | redraw as motion, values per `brief.md` |
| Driftwire CLI, every card and state | `motion-studio/assets/terminal/gallery-90col.ansi`, `gallery-44col.ansi` | ✅ captured today from `mv_driftwire_cli_gallery` (built from this branch's CLI source, 3359cc8a): real render code, made-up data | Film B beat 11, all bearer states |
| Driftwire CLI animations | `motion-studio/assets/terminal/animate-*.ansi` | ⚠️ only the first frames came through `script`; the renderer will animate the dots from the static states at the CLI's ~12 fps | Film B |
| Driftwire colours | `src/addons/driftwire/cli/theme.h` | ✅ | style guide |
| Driftwire GUI (window, tiles, code, path picture) | — | ❌ none in the repo | blocker, section 4 |
| Current app UI (command bar, search, People, Edit, Video Editor, themes, Import) | — | ❌ none | blocker, section 4 |
| Sample media | `tools/testmedia/raw-manifest.json`, `heif-manifest.json` (CC0, raw.pixls.us / libheif) | ✅ manifests; files fetched by `fetch-raw.ps1` | everything on screen |
| Music | — | ❌ | decision 3 |

## 2. Reference analysis (from the three screenshots, the icons and the CLI)

```
Palette     Canvas #21232A charcoal, text #DCDEE4, hairlines a step lighter than the canvas.
            Photos supply all the colour; chrome is near-monochrome. Driftwire adds one accent,
            teal #40C8E0, and a tint per bearer (Bluetooth #0A84FF, internet #5E5CE6,
            DHT #FF9F0A, relay #BF5AF2); success #30D158, error #FF453A, dim #8E8E93.
Typography  CozetteVector, a bitmap-style monospace, at small sizes for every label and
            filename; the window title in the OS face. Data reads like instrument output
            ("p99 16.900  max 16.867"). Big type is rare: only the Driftwire code, drawn in
            box-drawing digits in the CLI.
Composition The photo owns the frame; chrome is a thin top bar and a filmstrip strip.
            Eye goes to the image first, then the selected thumbnail's white ring, then small
            RAW badges. The F3 overlay is a translucent panel top-left with a thin graph.
Pacing      A tool, not a show: things appear on the next frame, no transitions in the UI
            except the preview → full-res cross-fade (10 frames) and the springy pan/zoom
            (ω=18, ζ=1). Driftwire is the one place with continuous motion: travelling dots,
            a shimmer on waits, a filling ring.
Motion      Snap for UI state, spring for the canvas, constant-rate travel for transfers.
Texture     None. Flat, crisp, pixel-exact.
```

What this means for the films: no glossy gradients or glow. Let the real photos carry colour,
set captions in Cozette on charcoal, and make the numbers look like the overlay's readouts.
Speed is shown by *nothing happening in between*: a hard cut to the finished state, timecode
frozen. Driftwire's teal and bearer tints are the only accent colours in either film.

**Do not copy:** anything from the article's examples; stock "AI reel" moves.

## 3. Facts corrected while reading the data

- **README pan claim mixes two soaks.** "60 s of animated panning on a 42 MP Sony RAW … p99
  16.95 ms": 16.95 is `frametime-animated.json` (the lab's animated soak); the 42 MP ARW soak is
  `pan-soak-42mp-arw.json`, p99 **17.25** ms. Both 0 dropped of 3,597. Film A uses 17.25 for the
  RAW. The README line wants fixing separately.
- **First pixel** in the current JSON is 51–89 ms, inside the README's 44–125 ms. Film A uses the
  JSON per camera.
- **Driftwire has no real-network speed measurement** (loopback and emulated paths only), so Film
  B makes no speed claim.

## 4. Recordings I need from you

Common settings for every take:

- **Machine:** Mac for Film A's master (Metal, Retina), unless you prefer Windows (decision 2).
- **Capture:** 60 fps, display at 2560×1440 or a scaled Retina mode, no compression beyond
  ProRes 422 / high-bitrate H.264. OBS or `⇧⌘5`. Cursor visible; key presses visible are a plus
  (KeyCastr), or I'll add keycaps from a list of what you pressed with timestamps.
- **App state:** Default theme unless a take says otherwise, window ~1600×1000, notifications
  off (Focus), menu-bar clock hidden, Dock hidden.
- **Media:** only `tools/testmedia` CC0 files and your own non-personal shots (landscapes,
  objects, no faces, no house numbers or plates). People takes use the face set in section 4c.
- **Names:** set the computer names to "Ann's MacBook" and "Studio PC" (or tell me yours to blur).
- Each take 10–30 s, with 2 s of stillness before and after. Name files `A01-cold-open.mov` etc.

### 4a. Film A — MediaViewer (12 takes)

| # | Take | What to do |
|---|---|---|
| A01 | Cold open | App quit. Double-click a folder of the test RAWs in Finder; leave it 3 s |
| A02 | Pan with F3 | 42 MP ARW (`sony_ilce7rm3.arw`), `F3` on, wheel-zoom to 100 %, drag around 10 s |
| A03 | Arrow run | Right-arrow through RAW, HEIC, a clip, a Live Photo pair: one press every ~0.6 s |
| A04 | Gallery | `G`, scroll, Shift-click a run, `Esc` |
| A05 | Video | A 4K HDR or 1080p HEVC clip: play, scrub, `.` frame-step ×3, speed 2×, A–B loop |
| A06 | Search | `⌘F`, type "dog on a beach" (or a subject your library has), open a clip result so it jumps to the moment, `N` to the next moment |
| A07 | Search filters | Type `beach -people in:2024 rating:4+` and let results settle |
| A08 | People | Settings → Local search → People: name a card, click it to see their photos |
| A09 | Edit | `Enter` on a JPEG: crop with a preset, straighten, exposure, before/after |
| A10 | Video Editor | *Edit video*: Split, Delete a range, Export (instant cuts) |
| A11 | Import | Insert a card (or a disk image), run Import to its finish screen |
| A12 | Themes | Settings → Appearance: Midnight → Retrowave → Forest, then back to Default |

### 4b. Film B — Driftwire (two computers, both screens recorded at once)

Ideally Mac + Windows to show both hosts; two Macs is fine. Start both recordings, then clap or
press a key on both so I can sync them.

| # | Take | What to do |
|---|---|---|
| B01 | Pick | Sender: gallery, Shift-click 24 files, share icon → Driftwire |
| B02 | Nearby | Sender's Driftwire window with both tiles visible; click the receiver's tile |
| B03 | Code | Receiver shows the six digits; sender types them (type slowly, ~0.3 s per digit) |
| B04 | Wrong code | (optional) one wrong code first, to show the refusal |
| B05 | Accept | Receiver: "wants to send you 24 files", Save to, Accept |
| B06 | Path | 10 s of the path picture with dots moving and the stats line, on both screens. A few GB so it runs a while |
| B07 | Wi-Fi drop | Mid-send, turn Wi-Fi off on one machine: "reconnecting…", then Bluetooth. Turn Wi-Fi back on: it moves back. Keep both screens recording throughout |
| B08 | Done | Finish, the done mark, what arrived, OK; open the received folder |
| B09 | Remember | Tick *Remember* on both; send again with one click, no code |
| B10 | Internet code | Over the internet on: Make a code, Copy; paste it on the other side, send |
| B11 | Four words | Public DHT on: the four-word code, the orange DHT badge on the path |
| B12 | Relay | (optional) a send through your own computer or relay server, purple badge |

The terminal is covered by the gallery capture; a real `driftwire send` take is optional (B13:
`driftwire send ~/Pictures/sample` to one of the machines, iTerm or Terminal, 18 pt, 100×30).

### 4c. Faces for A08

No real people's faces without consent. Options: a CC0 portrait set you pick, photos of people
who agree to appear, or skip faces and show the People card UI with the faces blurred. Tell me
which.

## 5. Decisions for you

1. **Which icon is the brand mark in the films?** Mac ships the charcoal one, Windows the blue
   one. My pick: charcoal master (it matches the canvas and the README's look), with the blue
   one shown only on a Windows close-up.
2. **Master platform for Film A:** Mac (Metal, Retina recordings, easier for me to drive) or
   Windows (where the perf numbers were measured). My pick: Mac footage, numbers labelled with
   the Windows machine they came from, as the README does.
3. **Sound:** a licensed or original track you supply, or synthesized cues only (key clicks,
   soft hits on cuts, a pad). My pick: synthesized, so every cue sits on the timeline.

**Decided 2026-10-26 (owner):** all three picks accepted: the charcoal icon is the brand mark
(blue only on a Windows close-up); Film A is Mac footage with the numbers labelled as measured on
the Windows machine; sound is synthesized cues on the timeline, no music track.

## Next

When the recordings and decisions are in, I copy approved files into `motion-studio/assets/`,
write `style-guide.md` and both `shotlist.md` sections, and stop at Gate 2.
