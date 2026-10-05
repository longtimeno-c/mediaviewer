# 19 — Voice query

What exists for spoken search queries: Local search's query language reads a spoken
sentence the same way it reads a typed one. A Voice add-on (microphone, recognizer, spoken
count) is not built.

## What it is

The idea is that a spoken sentence such as "pull up all the photos that include a dog on the
beach" is used as a Local search query, word for word, with no intent parser of its own. That
contract lives in the shared query parser (`src/addons/ai/query.*`,
[17 "Query syntax"](17-local-ai-search.md#query-syntax)), so the search fields on both
platforms, and any later voice front end, mean the same thing by the same words:

- **Lead-ins are trimmed:** "pull up all the …", "pull up …", "show me all the …", "show me
  all …", "show me the …", "show me …", "find me …", "find all …", "find …"
  (`kLeadIns` in `query.cpp`). A kind word inside a lead-in is kept.
- **"photos of …" / "pictures of …"** at the start ask for anything, not a photos-only filter,
  because that is how people ask for a picture search out loud.
- **"videos of …" / "clips of …"** ask for videos.
- A named person narrows, exactly as in a typed query.

`tests/test_ai_query.cpp` covers the spoken forms ("photos of" asks for anything).

## Not hurting the viewer

Nothing runs for voice today, so there is no audio capture, recognition worker or speech
output to schedule. The query parser above is linear in the query and runs where a typed
search runs.

## Not built

- The Voice add-on itself (`mv_voice`, `MediaViewer.Voice.Chrome`, `Voice.bundle`), its
  Settings → Add-ons entry and its download.
- Hold-to-talk key and the mic button on the search box.
- On-device recognition (Speech framework on macOS; an OS recognizer or a bundled small model
  on Windows) and the spoken result count (`AVSpeechSynthesizer` / `SpeechSynthesizer`).
- A host-table `search_query(text)` entry for one add-on to call another's search.
- Spoken follow-ups ("open the first", "next", "previous", "close").
