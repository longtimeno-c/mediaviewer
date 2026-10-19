---
name: mediaviewer-assemble-selects
description: Assemble a selects or radio cut in Final Cut Pro through MediaViewer from a script or a list of lines — find each line in the timeline's interviews and put them in the order given, leaving everything else out. Use when given a script, paper edit, list of sound bites or "put these lines in this order".
---

# Assemble selects from a script

## Find every line

1. `get_timeline` and `transcript` for every storyline clip (and `transcript` with `path` for a
   scope file that is not on the timeline yet; `search` with the line in quotes finds where it was
   said).
2. For each line of the script, find the phrase (or run of phrases) that says it. Prefer the take
   that is complete and clean; if a line was said twice, take the better one and say which. If a
   line is nowhere, say so; do not substitute a different line silently.

## Build the cut

The simplest correct way:
- Keep the clips that hold the selects; cut everything that is not a select out of them with
  ripple `cut`s on silence edges (`cut_from`/`cut_to` of the silences around each select), so each
  select stands alone with a short breath on each side.
- If the order must change, do the cuts first and commit nothing yet; then, in a **new** batch after
  a fresh `get_timeline`, `move` the pieces into the script's order (`move` item before item). Times
  and moves never share a batch.
- A select from a scope file not on the timeline: `insert` it (`source_in` and `duration` from that
  file's transcript, file time) before the piece it should precede.

## Then

`preview_changes`: the pieces in script order, nothing else between them. `commit`, and report each
select with its source, time and text.
