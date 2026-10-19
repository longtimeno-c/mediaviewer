---
name: mediaviewer-tighten-interview
description: Tighten an interview or talking-head timeline in Final Cut Pro through MediaViewer — remove filler words (um, uh, er) and shorten pauses over a limit, cutting only on silences and word edges. Use when asked to tighten, clean up, de-um, remove pauses or dead air, or make an interview flow faster.
---

# Tighten an interview

You edit the Final Cut Pro project the editor dragged onto the MediaViewer panel, through the
`mediaviewer` MCP tools. Each commit is a new project beside the original; nothing you do changes
the editor's project or their files.

## 1. Read before cutting

1. `get_timeline`: note the storyline clips (`at`, `end`, `file`, `file_in`), connected clips, titles,
   markers, and anything `not_editable`. If it says `stale`, stop and ask the editor to drag the
   project onto the panel again.
2. `transcript` (all storyline clips, or `from`/`to` for a long one). Read the phrases first, the
   words second. If a clip says it is not transcribed, call `analyse` and work on the silences
   meanwhile; say so.

## 2. Decide what goes

- **Pauses.** Use the `silences` list. A silence longer than the limit is cut from its `cut_from` to
  its `cut_to`: the server already keeps about 80 ms of air at each side, so speech never touches
  speech. Do not cut a silence under the limit. With no limit given, tighten to the tone the editor
  wants (adapted from Cut Video, MIT, see NOTICE.md):

  | Pause | Punchy | Balanced (default) | Gentle | Documentary |
  |---|---|---|---|---|
  | under 0.25 s | keep | keep | keep | keep |
  | 0.25-1 s | leave 0.1 s | leave 0.3 s | leave 0.5 s | keep |
  | 1-3 s | leave 0.2 s | leave 0.8 s | leave 1.5 s | leave 2 s |
  | over 3 s | leave 0.2 s | leave 0.5 s | leave 1 s | leave 3 s |

  To leave `x` seconds of a silence, cut its middle: from `at + x/2` to `end - x/2`.
- **Never cut** a laugh, a reaction, the beat before a punchline, or a pause the editor asked to
  keep. Leave 0.4-0.6 s of air where one speaker hands over to another.
- **Fillers.** By default only the conservative list: um, uh, umm, uhh, er, erm, ah, hmm, mhm, said
  as hesitations (not inside a quote). "like", "so", "you know", "I mean", "actually" only when the
  editor names them. A filler sits between a silence and a word, or
  inside a phrase. Cut it from the end of the silence before it (or the end of the word before it)
  to the start of the next word. Word times inside a phrase are estimates: prefer an edge that is
  also a silence edge, and when a filler has no silence on either side, leave it and list it for
  the editor rather than risk clipping a word.
- **Never** cut inside a word, never cut the last copy of a line, never remove a line the editor
  asked to keep. Connected B-roll and music follow the cut on their own; you do not move them.

## 3. Propose, then cut

Unless the editor said to just do it, list the cuts you plan (time, length, why) and wait. Then
send them all in **one** `edit` call: every time in a batch is a time on the timeline as it is
before the batch, so you can take them straight from `transcript`. Example:

```json
{"ops": [
  {"op": "cut", "from": 6.20, "to": 7.16},
  {"op": "cut", "from": 10.44, "to": 12.96}
]}
```

## 4. Check, then commit

- `preview_changes`: the length before and after must differ by the sum of your cuts.
- `transcript` again (the working copy) around each cut: no word cut in half, no word said twice
  across a join, no sentence that now starts mid-way. A word that seems to last a second or more
  hides a pause or a stumble: look at the silences there. Fix and check again, at most three passes;
  then say what is left. (QC steps adapted from video-use and fcp-mcp-server, MIT, see NOTICE.md.)
- `commit`.
Report what you cut (a short table: original time, length, what), the new length, what you left
on purpose and anything the editor should listen to (filler cuts on estimated word edges).
