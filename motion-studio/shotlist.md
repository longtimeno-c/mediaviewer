# Shot lists

Timings are provisional until the takes arrive (Gate 3 retimes to the footage). Every on-screen
number is in `brief.md` with its source. 30 fps timeline; frame numbers are `s × 30`.
Take ids refer to `reviews/gate-1.md` section 4.

## Film A — MediaViewer (60 s, 16:9)

States: `DOUBLE-CLICK → FIRST PIXEL → PAN → BROWSE → VIDEO → SEARCH → PEOPLE → EDIT → MORE → CLOSE`

| # | Time | Take | Entry state | Exit state | On screen | Purpose |
|---|---|---|---|---|---|---|
| 1 | 0.0–4.0 | A01 | Finder, folder selected | RAW on screen, timer frozen | timer `00.000` runs, freezes at **51 ms** in `accent`; caption "sony_ilce7rm3.arw · 42 MP RAW" | Hook: it's already there |
| 2 | 4.0–6.0 | A01 | preview | full resolution | the refine cross-fade; caption "full resolution 1.23 s later. The view doesn't move." | Preview first is honest, not a trick |
| 3 | 6.0–12.0 | A02 | fit | 100 %, panning | F3 overlay cropped; a flat line on 16.68 ms; counter **0 dropped · 3,597 / 3,597**, "p99 17.25 ms" | Smoothness, measured |
| 4 | 12.0–18.0 | A03 | photo 1 | photo 6 | keycaps `→` per press; badges RAW / LIVE; caption "next photo: 0.2 ms" | Browsing a real dump, mixed formats |
| 5 | 18.0–20.0 | A04 | viewer | gallery | `G` keycap, grid scroll | Scale: the whole card |
| 6 | 20.0–27.0 | A05 | clip paused | A–B loop | keycaps `.` `.` `.`; caption "HEVC · 0 dropped frames · A/V p99 7.2 ms" | Video is first-class, same canvas |
| 7 | 27.0–36.0 | A06 | empty search | clip at the matching moment | typed query in Cozette, results, jump; caption "Computed on your computer. Nothing is uploaded." | The memorable feature |
| 8 | 36.0–39.0 | A07 | query | filtered results | the filter tokens highlight one by one | Precision, keyboard-first |
| 9 | 39.0–44.0 | A08 | People list | a person's photos | name typed on a card (faces per 4c) | Search by who |
| 10 | 44.0–50.0 | A09, A10 | photo | export | crop → straighten → before/after; Video Editor split; caption "Originals are never touched." | Safe editing |
| 11 | 50.0–55.0 | A11, B06, A12 | — | — | three 1.6 s hard cuts: Import verifying, Driftwire path, theme change; one-word labels "Import" "Driftwire" "Themes" | There's more, without explaining it |
| 12 | 55.0–60.0 | — | black → close | poster | charcoal icon, "MediaViewer", "Windows and macOS · Free and open source", `github.com/longtimeno-c/mediaviewer`; small `dim` line "Numbers measured on Windows: Ryzen 7 5700X3D, RTX 4070, 59.95 Hz" | Close; last frame is the poster |

Beat grid (cue → time): click 0.30 · freeze hit on the frame the picture appears in the take
(the timer shows the measured value; the edit never speeds the footage up) · refine tick 4.0 · cut hit 6.0 · counter ticks
6.4–7.0 · cut 12.0 · key clicks 12.4, 13.0, 13.6, 14.2, 14.8, 15.4 · `G` 18.0 · cut 20.0 · key
clicks 22.0, 22.4, 22.8 · cut 27.0 · typing 27.5–29.5 · result tick 30.0 · jump hit 31.0 · cut
36.0 · cut 39.0 · cut 44.0 · cut hits 50.0, 51.6, 53.2 · swell 55.0 → resolve 57.5 · end 60.0.

**9:16 cut (30 s):** beats 1, 3, 4, 7, 10 (crop only), 12. Re-frame: beat 3 crops to the overlay's
readout and the image's centre stacked; beat 7 shows the search field and one result.

## Film B — Driftwire (105 s, 16:9)

States: `PICK → NEARBY → CODE → ACCEPT → PATH → DROP → RESUME → VERIFIED → ANYWHERE → REMEMBER → RELAY → TERMINAL → CLOSE`

Both screens side by side (sender left, receiver right) unless noted; a thin `hairline` between.

| # | Time | Take | Entry | Exit | On screen | Purpose |
|---|---|---|---|---|---|---|
| 1 | 0–5 | B06 | — | — | cold open on the path picture mid-transfer, dots moving; title "Driftwire" in Cozette, `accent` | Hook: the thing in motion |
| 2 | 5–12 | B01 | gallery | Driftwire with 24 files | Shift-click range, share icon; caption "Pick, then send." | How it starts |
| 3 | 12–18 | B02 | Nearby | tile clicked | the receiver's tile; caption "Computers nearby, on Wi-Fi or Bluetooth." | Discovery |
| 4 | 18–28 | B03 | code shown | code typed | receiver's six digits large; sender types them, one tick per digit; caption "Nothing moves until the other side accepts." | The core trust step |
| 5 | 28–38 | — | diagram | diagram | **Diagram 1:** two columns; each side drops a sealed commitment, receiver reveals last, digits form from both; caption "Neither side knows the code before the other has finished its part." | Why a machine in the middle can't fake it |
| 6 | 38–46 | — | diagram | diagram | **Diagram 2:** two locks on one link, "X25519" and "ML-KEM-768"; hold `Noise_XXhfs_25519+MLKEM768_ChaChaPoly_BLAKE2b` for 2.5 s; caption "A recording made today stays sealed." `dim`: "Modelled in ProVerif" | Post-quantum, without a lecture |
| 7 | 46–52 | B05 | offer | accepted | "wants to send you 24 files", Accept | Consent |
| 8 | 52–60 | B06 | 0 % | ~40 % | path picture, stats line ticking (UI text, not a claim) | What it looks like working |
| 9 | 60–72 | B07 | Wi-Fi | Bluetooth → Wi-Fi | dots freeze, "reconnecting…" in `warn`; badge turns `bt`; resumes; later back to `accent`; caption "Wi-Fi drops. It keeps going. No new code, nothing sent twice." | The standout behaviour |
| 10 | 72–78 | B08 | 99 % | done | done mark, received folder opens; caption "Every file checked as it lands, and read back before it appears." | Verified copies |
| 11 | 78–87 | B10, B11 | toggle off | sent to a code | Make a code, copy, paste; then the four words and the `dht` badge; caption "Anywhere, with a code. Once, for ten minutes. No server in between." | Beyond the room |
| 12 | 87–92 | B09 | send done | one-click send | Remember ticked on both, then a tile click with no code; caption "Remember each other. It refuses if a key ever changes." | Convenience without weakening trust |
| 13 | 92–95 | B12 or diagram | — | — | `relay` badge on the path; caption "Through a relay that can't read it." | Hard networks |
| 14 | 95–101 | terminal | prompt | done transcript | `gallery-90col.ansi` states: banner → offer → sending → transcript, in a terminal frame; caption "From a terminal, too." | The CLI audience |
| 15 | 101–105 | — | — | poster | the three install lines (curl / brew / irm) in Cozette; "No account. No cloud."; charcoal MediaViewer icon small | Close; poster |

Beat grid: open hit 0.0 · title tick 1.0 · cut 5.0 · clicks 6.0–8.5 · cut 12.0 · tile click 15.0 ·
code appears 18.5 · digit ticks 21.0, 21.3, 21.6, 21.9, 22.2, 22.5 · accept-wait tick 23.0 ·
cut 28.0 · commitment blips 30.0, 31.5 · reveal 33.5 · digits form 35.0 · cut 38.0 · lock clicks
40.0, 41.0 · cut 46.0 · accept chime 49.0 · glide starts 52.0 · dropout 62.0 · reconnect chirp
64.5 · back-to-Wi-Fi tick 69.0 · done chime 75.0 · cut 78.0 · copy tick 80.0 · paste 82.0 ·
words 84.0 · cut 87.0 · one-click 90.0 · cut 92.0 · cut 95.0 · terminal key clicks 95.5–97.0 ·
swell 101.0 → resolve 103.5 · end 105.0.

**9:16 cut (45 s):** beats 1, 4 (receiver stacked over sender), 6 (lock diagram only), 9, 11
(words only), 15. Two screens stack vertically instead of side by side.

## Open for Gate 2 review

- Beat 1 of Film A quotes the ARW's 51 ms; the cold-launch time on the Mac recording will differ
  (it includes launching the app). The timer will show the **in-app** first pixel read from the
  app's own log for that take, or the take is re-framed as "open a RAW" inside a running app. I'll
  pick whichever the footage supports and say so at Gate 3.
- Film B beat 13 has no take unless you record B12; otherwise it is a diagram.
