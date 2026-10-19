---
name: mediaviewer-cut-to-length
description: Cut a Final Cut Pro timeline down to a target length (a 60 s version, a 15 s social cut, a trailer) through MediaViewer, keeping whole sentences and the lines the editor must keep. Use when asked to shorten to a duration, make a short version, cutdown, teaser or social edit.
---

# Cut to a length

## Plan the cut on paper first

Ask what kind of cut it is if the brief does not say (a single scene, selects, a script edit, a
story with a beginning and an end); a tweak to your plan is not a yes.

1. `get_timeline` (the current length) and `transcript` (phrases).
2. Number the phrases. Mark the ones the editor named as must-keep. Score the rest by how much
   they carry: the point of the piece, a strong close, a concrete fact or image; repetition,
   throat-clearing ("thanks for having me", "so, basically") and asides go first.
3. Choose whole phrases until the total is at the target (the sum of their kept spans plus the
   pauses you keep). Keep the story in order unless the editor asked for a re-order. A cut version
   should still start cleanly and end on a full stop.
4. Tighten pauses between kept phrases to about 0.3-0.5 s before dropping another line: often the
   pauses alone are 10-20 % of an interview.

## Make it

- Cut whole phrases only, on silence edges: from the end of the kept phrase before (its silence's
  `cut_from`) to the start of the next kept phrase (that silence's `cut_to`). Never end inside a
  sentence; never cut inside a word.
- One `edit` batch with every cut, in the timeline's times before the batch.
- `preview_changes`: within a few percent of the target? If you are long, drop the weakest
  remaining line; if short, put back the best dropped one (`reset` then one new batch is simpler
  than cutting a cut).
- `commit`. Report the new length, the lines kept (in order) and what you dropped.
