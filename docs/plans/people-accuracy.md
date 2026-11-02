# People accuracy: what is left

The 2026-11-02 review of "the wrong person is in another person's group" found four causes, all
in how corrections and clips were handled, not in the face model (AdaFace IR-50 separates the
owner's named people: the closest two centroids are 0.255 apart, against a 0.30 threshold). What
it built is in [docs/design/17 "Corrections that hold"](../design/17-local-ai-search.md#corrections-that-hold)
and "Reviewing a person", with the numbers; the decision is in
[12-decision-log](../design/12-decision-log.md) "Corrections hold, and a frame is evidence".

Not built yet, each measured the same way (a copy of the owner's faces.db with `[.people-settle]`,
`[.people-heldout]` and `[.people-api]`, base and new alternated; `[.people-bench]` on LFW must
not drop):

1. **Tracks.** Faces of one clip at neighbouring moments that agree (cosine ≥ join, or a box
   overlap) as one track: a track counts once in a person's online average, and its weak frames
   take the decision of its clear ones. Today a clip counts once only in the refinement's
   exemplars and medoid, and "not this person" spreads to the same face in the clip.
2. **The ~60 of Tristan's clip faces** that the new settle files in two unnamed groups (0.30 and
   0.37 to his agreeing pins). "Might also be" offers them; find why the refinement moves them
   out before changing anything.
3. **Admission into unconfirmed people in the settle** by exemplars: an A/B of the current rule
   against the core mean (online matching already uses the mean for them since #434).
4. **Diverse exemplars**: farthest-point choice over pins and core, once "Yes" adds many pins.
5. **Per-person thresholds**, stricter only, and only if `[.people-heldout]` still shows wrong
   named people (it shows none today).
6. **An order for the unknown faces** that settles the most faces per answer, measured with a
   simulated user on LFW (answers needed to reach a BCubed F, against clearest-first).
7. **Refine and Merge duplicates in detail**: what moved, to whom, with a look before it lands.
8. **"This person"** (`search_this_person`) from the viewer: wired in neither chrome; it needs a
   command row (docs/design/16).
9. **Linux**: no People chrome yet; the core above is portable and the Qt half follows the AI
   chrome's order on Linux (docs/plans/linux.md).
