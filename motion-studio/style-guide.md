# Style guide

Drawn from the reference analysis in `reviews/gate-1.md`. Applies to both films and every format.

## Principle

MediaViewer is an instrument, not a show. Speed is shown by **nothing happening in between**: hard
cuts to finished states, timers that freeze. Real footage carries the film; graphics annotate it.

## Colour (tokens)

| Token | Hex | Use |
|---|---|---|
| `canvas` | `#21232A` | every background; the app's own canvas |
| `surface` | `#2A2D35` | cards, caption plates |
| `hairline` | `#3A3E48` | rules, frames |
| `text` | `#DCDEE4` | captions, numbers |
| `dim` | `#8E8E93` | units, sources, secondary lines |
| `accent` | `#40C8E0` | Driftwire, and Film A's single highlight (a frozen timer, the flat pacing line) |
| `shine` | `#A9E6F1` | the shimmer band on waits |
| `bt` | `#0A84FF` | Bluetooth |
| `net` | `#5E5CE6` | internet |
| `dht` | `#FF9F0A` | public DHT |
| `relay` | `#BF5AF2` | relay |
| `ok` / `err` / `warn` | `#30D158` / `#FF453A` / `#FFD60A` | done, failed, reconnecting |

No gradients except the CLI's own name wordmark (teal → indigo, as `theme.h` draws it). No glow,
no lens flares, no grain. Photos are never colour-graded or tinted; footage plays as captured.

## Type

- **CozetteVector** (`assets/fonts`, MIT) for every caption, number and label, at integer
  multiples of its design size so it stays crisp: 26 / 39 / 52 px at 1080p; 78 px for hero numbers.
- No second display face. The Driftwire code uses the CLI's box-drawing digits, scaled.
- Units and sources in `dim`, one size down: `17.25 ms  p99` / `pan-soak-42mp-arw.json`.
- Phone floor (9:16): nothing under 39 px.

## Layout

- 16:9: footage in a window-sized frame at 82 % width, centred on `canvas`, 1 px `hairline` edge,
  no device mockup, no drop shadow. Captions bottom-left, inside a 64 px safe margin.
- 9:16: footage cropped to the region that matters (re-framed per shot, not centre-cut); captions
  above the footage; at most one caption on screen.
- Numbers look like the F3 overlay's readouts: left-aligned, monospace, label then value.

## Motion rules

| Class | Rule |
|---|---|
| UI state, keycaps, badges | snap: 0–2 frames, no overshoot |
| Cards, caption plates | settle: spring ω=18, ζ=1 (the app's pan/zoom spring), ≤ 12 frames |
| Camera (punch-in on footage) | slow ease, ≥ 1.2 s, ≤ 1.6× zoom, never while the UI itself is moving |
| Numbers | count up over 10 frames, then hold ≥ 1.5 s (readable at phone size) |
| Driftwire dots | constant rate tied to the shown speed; freeze on "reconnecting…"; move with Reduce Motion as the app does |
| Shimmer | the CLI's band, one pass per 1.2 s |
| Cuts | hard cuts. One cross-fade allowed in Film A: the preview → full-res refine, because the app does it |

## Sound (synthesized)

- Key clicks: short filtered noise burst, two variants alternated, on every keycap.
- UI ticks: soft sine blip (880 Hz, 30 ms) on state changes.
- Cut hits: low thump (60 Hz, 120 ms) on section changes only.
- Bed: a quiet two-chord pad (−24 LUFS under cues), one swell at the close.
- Driftwire: a rising glide while bytes move, a dropout + reconnect chirp on the bearer change,
  a two-note chime on done. Every cue sits on the beat grid in `shotlist.md`.
- Mix to −16 LUFS integrated; the film must make sense with sound off (captions carry the claims).

## Banned defaults

Big centred headline over a gradient · everything fading in · logo reveal with particles ·
device mockups and floating 3D tilts · stock whooshes · fake UI · rounded numbers not in
`brief.md` · "blazing fast", "revolutionary", "seamless" · showing features listed as not built.
