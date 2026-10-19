---
name: mediaviewer-remove-retakes
description: Remove false starts, fluffed takes and restarts ("sorry, let me say that again") from an interview timeline in Final Cut Pro through MediaViewer, keeping the clean take of each line. Use when asked to remove retakes, flubs, false starts, repeats or mistakes.
---

# Remove retakes

Work on the dragged Final Cut Pro project with the `mediaviewer` tools; every commit is a new
project beside the original.

## Find them

1. `get_timeline`, then `transcript` (with words).
2. Show every group of takes in your plan so the editor can choose (adapted from Cut Video, MIT,
   see NOTICE.md). A retake is a line said more than once in a row, the first attempts broken off or stumbled
   ("it changes in the, in the"), often followed by an apology or a cue ("sorry", "let me start
   again", "one more time", "take two"). The **last complete** attempt is the keeper unless the
   editor says otherwise; when an earlier take is complete and better, say so instead of choosing
   silently.
3. A false start is a sentence begun and abandoned ("Most people book the, the two day,") followed
   by the same sentence said cleanly, usually within about 3 seconds: drop the earlier attempt. Of
   two word-for-word repeats, keep the cleaner one.

## Cut them

- Cut from the start of the first bad attempt (the end of the silence before it) to the start of the
  keeper (the start of the silence before it plus the server's breath: use that silence's
  `cut_from`/`cut_to` edges). The pause in front of the keeper stays as long as the pause in front
  of the bad take was, so the rhythm does not change: if the editor asked to keep pauses as they
  are, cut from the bad take's first word to the keeper's first word.
- Remove the apology and cue with the bad take. Never cut inside a word; never remove the only
  complete take of a line.
- Retakes inside a multicam or compound clip are inside a unit: the server will not open it; tell
  the editor.

## Then

One `edit` batch with every cut (times from before the batch), `preview_changes`, `commit`. Report
each removed take with its text, and which take you kept.
