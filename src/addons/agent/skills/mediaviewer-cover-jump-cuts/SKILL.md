---
name: mediaviewer-cover-jump-cuts
description: Cover the jump cuts in a tightened interview with B-roll from the folders the editor gave, as connected clips in Final Cut Pro through MediaViewer. Use when asked to add B-roll, cover cuts, hide jump cuts or cut away.
---

# Cover jump cuts with B-roll

## Find the cuts and the shots

1. `get_timeline`: a jump cut is where two storyline pieces of the **same file** meet with a jump in
   file time (`file_out` of one ≠ `file_in` of the next), or any edit the editor points at. Skip
   joins already covered by a connected clip.
2. `scope_files` lists the B-roll you may use (the given folders only). `search` finds shots by
   what is seen or heard; when the words of the line match a shot ("the lake", "the summit"), use
   it there. `thumbnail` (if the editor allowed frames) lets you look.
3. Never use a file the server refuses: the scope is the timeline's media and the given folders.
   If the editor asks for a file outside it, say it was refused and why.

## Place them

- Each B-roll clip: connected (`connect`, lane 1, video only), starting about 0.5-1.0 s **before**
  the jump cut and ending about 0.5-1.0 s after it, so the cut is hidden and the face returns on a
  word, not on the cut. 1.5-3 s per shot is usually right.
- Use a part of the shot that moves or holds (avoid the first and last second of a clip: handles).
  Do not reuse the same range of a shot twice; vary shots across cuts.
- Do not cover the opening line or the close unless asked: the viewer should see who is talking.
- One `edit` batch with every `connect`; the timeline's length does not change.

## Then

`preview_changes`, `commit`. Report a table: the cut, the shot (file and range), the timeline span,
and anything refused.
