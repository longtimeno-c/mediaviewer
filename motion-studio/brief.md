# Brief

The two films and the facts they may use. Every number names its source; nothing else may
appear on screen as a claim. Changes after Gate 1 are noted at the bottom with a date.

## Film A — MediaViewer

```
One sentence: Your whole camera dump opens instantly, pans without a dropped frame, and you can
              find anything in it by describing it, without anything leaving your computer.
Audience:     Photographers and people with big camera rolls, on GitHub and social
Duration:     60 s master (16:9 1920×1080), 30 s cut (9:16 1080×1920), separate compositions
```

| Claim on screen | Value | Source |
|---|---|---|
| RAW preview on screen | 51–89 ms across five cameras (ARW 51, CR3 52, DNG 69, CR2 79, NEF 89) | `docs/perf/first-pixel/*.json` `still_first_pixel_s` |
| Full resolution after the preview | 0.65–1.72 s, cross-fades in without moving the view | same files, `still_full_s` |
| README's range | 44–125 ms | `README.md` "At a glance" (wider than the current JSON; use the JSON) |
| Panning a 42 MP Sony RAW | 0 dropped of 3,597 frames over 60 s, p99 **17.25** ms on a 16.68 ms refresh | `docs/perf/pan-soak-42mp-arw.json` |
| Animated soak (not the RAW) | 0 dropped of 3,597, p99 16.95 ms | `docs/perf/frametime-animated.json` |
| Next photo, decoded ahead | 0.03–0.15 ms ready ("0.2 ms") | `docs/perf/browse.json` `kind: warm` |
| Jump to a RAW not decoded ahead | 51–83 ms on screen | `docs/perf/browse.json` `kind: cold` |
| Still image on screen | 0.08 % CPU, 0 presents over 60 s | `docs/perf/frametime-idle.json` |
| 1080p HEVC on screen | 0 dropped of 3,597 presents, p99 17.10 ms; A/V error p99 7.2 ms | `docs/perf/investigation/video-native-60s.json`, `av-after-120s.csv` |
| Search accuracy | from `docs/perf/search.json`, the out-of-box setting at 25,000 photos only | `docs/perf/search.json` |
| Machine | Ryzen 7 5700X3D, RTX 4070, 59.95 Hz | `README.md`, `docs/perf/machine.json` |

Not built, so not shown as shipped: Linux, voice search, the Final Cut Pro panel, the Editor
add-on, several clips on one timeline.

## Film B — Driftwire

```
One sentence: Send files to the computer next to you, or anywhere with a code, sealed end to
              end, verified as they land, and it keeps going when the network changes.
Audience:     MediaViewer users, and terminal people who would install the CLI on its own
Duration:     90–120 s master (16:9), 45 s cut (9:16), separate compositions
```

Facts allowed (all from `docs/design/28-driftwire.md` and the README's Driftwire section):
six-digit code shown only on the receiver; nothing moves until the receiver accepts; three wrong
codes end the link; protocol 3 commit-reveal so neither side knows the code early;
`Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b`, X25519 and ML-KEM-768 both needed to read a
recording; checked against Cacophony vectors where it shares code with XX, modelled in ProVerif;
link moves between Bluetooth and Wi-Fi with no new code and nothing sent twice; every file
checked as it lands and read back before it appears; internet code works once, for ten minutes,
no server in between; four BIP-39 words over the public DHT (off by default); Remember keeps the
other's key in the Keychain / Windows Credential Manager and refuses if it changes; relays (your
own computer or your own relay server) pass sealed bytes they cannot read; the `driftwire` CLI on
Linux, macOS and Windows, interactive only.

**No speed claim.** The only throughput numbers are loopback and emulated-path benches
(`docs/perf/driftwire-lan.jsonl`, `driftwire-stream.jsonl`), not a transfer between two real
machines. On-screen stats lines are the UI's own example text ("14 MB/s"), shown as UI, never as
a claim.

## Changes

- 2026-10-26, Gate 1: first-pixel and pan numbers moved to the JSON values; the README's 16.95 ms
  belongs to the animated soak, not the 42 MP RAW (see `reviews/gate-1.md`).
- 2026-10-26, Gate 2 approved (owner): style-guide.md and shotlist.md as drafted.
- 2026-10-26, Gate 1 decisions (owner): charcoal icon (`assets/mediaviewer-icon.png`) is the mark;
  Film A footage is the Mac app, numbers captioned "measured on Windows: Ryzen 7 5700X3D, RTX 4070";
  sound is synthesized cues only.
