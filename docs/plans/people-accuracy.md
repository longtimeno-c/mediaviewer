# People accuracy: what is left

The 2026-11-02 work on "the wrong person is in another person's group" is built and described in
[docs/design/17 "Corrections that hold"](../design/17-local-ai-search.md#corrections-that-hold),
"A person's own fragment", "Tracks", "Reviewing a person" and "Refine and Merge duplicates in
detail", with its numbers; the decisions are in [12-decision-log](../design/12-decision-log.md).
Tried and left out, with their numbers there: admitting into unconfirmed people by their average
in the settle, and farthest-point (diverse) exemplars.

Not built:

1. **Per-person thresholds** (stricter only). Their condition was that `[.people-heldout]` still
   files named people's faces under another named person; on the owner's copy it files none
   (clear or weak), so there is nothing for them to fix. Revisit if that count rises.
2. **A clip counted once in an unconfirmed person's online average.** Confirmed people are
   already matched by exemplars, one per photo or clip; the average decides only for unnamed,
   unpinned people, and no bench here measures a gain there. Build it with a bench that does.
3. **Linux**: no People chrome, because there is no Qt AI chrome yet; the order of the Linux
   halves is an open call (CLAUDE.md, docs/plans/linux.md §10). The core above is portable C++
   and the ABI is the same, so the Qt half is chrome only when it comes.
4. **Owed checks**: the Mac chrome was driven (2026-11-03) against a dev-signed pack on a copy
   of the owner's index and faces.db: People, Show all, Unknown faces and Merge duplicates
   (look, then apply 31 merges; no pinned face moved). Its accessibility tree showed every
   person whose cover had decoded missing (FaceImage's picture is decorative, so the label had
   nothing to sit on); fixed, and the "Might also be" faces with it. Still owed: a pass with
   VoiceOver itself and with Narrator, and the WinUI run (Windows' native build is CI's).
