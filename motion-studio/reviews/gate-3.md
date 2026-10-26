# Gate 3 — representative stills (part 1: beats that need no footage)

2026-10-26. Five beats × two formats, in `reviews/gate-3/`. The footage beats follow once the
takes are in `assets/takes/`.

| Beat | Still (frame) | What it shows |
|---|---|---|
| B5 code | `B05-code-16x9.png`, `-9x16.png` (285) | Four handshake messages as a ladder: both sides seal a nonce, the sender opens, the receiver opens last; then both compute the six digits, only the receiver shows them, the sender's person types them |
| B6 locks | `B06-locks-16x9.png`, `-9x16.png` (200) | One link, two locks (X25519, ML-KEM-768), the Noise pattern name held, "A recording made today stays sealed." |
| B14 terminal | `B14-terminal-16x9.png`, `-9x16.png` (165) | `driftwire send ~/Pictures/trip`, the banner, the Wi-Fi send at 42 %, drawn from the CLI's own render output (90 and 44 columns) |
| A12 close | `A12-close-16x9.png`, `-9x16.png` (140) | Charcoal icon, name, "Windows and macOS. Free and open source.", repo URL, where the numbers were measured |
| B15 close | `B15-close-16x9.png`, `-9x16.png` (110) | "≋ Driftwire", "No account. No cloud.", the three install lines (9:16: brew and the install page), "In MediaViewer, or on its own." |

## Fixed in this pass

1. Terminal frames broke and lines overflowed: Cozette has no rounded box corners or eighth
   blocks, so fallback glyphs had other widths. Terminal shots now use Menlo with one 1ch cell
   per character, so the grid holds whatever font draws a glyph.
2. 9:16 code diagram: "only this screen shows them" ran off the right edge. 9:16 now stacks the
   results down the middle, receiver first.
3. 9:16 locks: "ML-KEM-768" wrapped onto three lines, and the link showed through the shackle.
4. B15: the install-page URL had a `$` prompt as if it were a command; Windows now shows `PS>`.
5. 16:9 code diagram: the result labels crowded the caption; the ladder moved up.

## Choices to check

- **Machine names.** The CLI gallery's made-up name for this computer was the owner's first name;
  it is swapped for "Robin's ThinkPad" (frames re-padded), sending to "Ann's MacBook". The GUI
  takes use "Ann's MacBook" and "Studio PC".
- **Version.** The gallery's banner says 1.0.0 (a placeholder); the still shows 0.1.38, the
  driftwire release that is out.
- **Code digits.** 342 917, the digits the CLI gallery draws.
- **Terminal size in 9:16** is 34 px, the one size under the 39 px phone floor, so 44 columns fit
  inside the safe margin.

## Known, left for the footage pass

- Menlo draws square corners where the CLI draws rounded ones (╭╮╰╯); a terminal font with
  those glyphs can go in `public/` if you want them exact.
- The terminal still shows one send at 42 %; the motion version will move the dots and the bar
  as the CLI does (about 12 fps), and end on its transcript line.
- 9:16 terminal and B15 leave the lower third empty; the motion version fills it with the live
  region as it grows. If the still is used as a poster, I'll re-centre it.

## Passed

2026-10-26 (owner): the five footage-free stills pass. The footage beats follow when the takes are in `assets/takes/`.
