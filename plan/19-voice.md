# 19 — Voice query (an installable add-on)

**Status: proposed 2026-09-24, post-v1. Not in PR 8. Milestone I, PRs 27–28. Windows and macOS together.**
Speak a search — "pull up all the photos that include a dog on the beach" — and get the same
grid Local search would show for that sentence, then a short spoken count. It is an opt-in
add-on of its own. It is not part of the AI pack, and the base viewer never carries it.

It exists only because two earlier slices do. **PR 16** (Milestone G) is the add-on mechanism
it installs through ([18-import.md](18-import.md#add-ons-how-import-is-installed)). **PR 22**
is the text query it calls ([17-local-ai-search.md](17-local-ai-search.md)). It does not merge
before PR 22's verify holds. It does not take PR 26's number (folder tiles). PR 25 stays unused.

## What it is

Hold a key, talk, release. The words become the text query Local search already accepts.
Results are that search's gallery: photos, and video moments when the index has them, in the
current folder unless the typed search was scoped wider. The transcript sits in the search
box, so a mis-hearing can be corrected by typing. A short spoken line reports the count
("12 photos", "nothing matched").

There is no second index, no second embedding model, and no intent parser. The utterance
**is** the query, filler included. "Pull up all the photos that include…" is a fine sentence
for the text tower; stripping it down to keywords would be a different product.

## Why it is its own add-on

Local search is a large download (ONNX Runtime and an embedding model). Voice is a
microphone, a recognizer, and a speaking voice. A person can want one without the other:

| Installed | What you get |
|---|---|
| Neither | The viewer. No search box mic, no voice key |
| Local search only | Type a query, as in PR 22 |
| Voice only | The key and the mic button exist. Holding the key says Local search is required and searches nothing. It does not download Local search on its own |
| Both | Speak a query, see the grid, hear the count |

Installing Voice shows the size and, when Local search is absent, offers that install as a
**second** explicit opt-in. No pre-ticked box. Removing Voice leaves `index.db` where it is.
Removing Local search leaves Voice installed and unable to search.

## Why it is compatible with the rules

| Rule | How it holds |
|---|---|
| 1 — nothing blocking on UI/render | Capture, recognition and synthesis run off the UI and render threads. The transcript arrives on the completion queue ([14](14-abi.md)) |
| 4 — zero dropped frames | Recognition is low priority and backs off when the frame-time overlay's rolling p99 exceeds budget. Both present-loop gates are re-run **while the key is held** and **while the count is spoken** |
| 5 — never modify an original | Voice writes nothing to media. It does not write the AI index either |
| 6 — nothing leaves the machine | Audio and the transcript are user content. On-device recognition only. No cloud recognizer, no online voice, no transcript in telemetry or crash reports |
| 7 — no required codec pack | The analogue: the base viewer never requires Voice or an OS speech download. Missing speech support hides the feature |

## Recognizer and voice

**On-device, per OS. The add-on does not ship ONNX Runtime, and it does not call a server.**

| | macOS | Windows |
|---|---|---|
| Listen | Speech framework. `requiresOnDeviceRecognition = true` for every request. If `supportsOnDeviceRecognition` is false, Voice is unavailable | See the spike below. The legacy Windows SDK recognizer is not the path: its free-form mode is the cloud one |
| Speak | `AVSpeechSynthesizer` | `Windows.Media.SpeechSynthesis.SpeechSynthesizer`, and only an installed voice that synthesizes with no network |
| Language | The UI language, once that dictation language is downloaded in System Settings | The UI language, once the recognizer for it is on the machine |
| Model weights in this add-on | None. The OS already has them | None on the OS-recognizer path. A small native model **only** if the spike below falls back |

A query is one utterance, ended when the key is released, well under a minute. The mic is
closed the rest of the time. There is no wake word and no always-on listener; that would
keep the microphone open in the background, and it is a later decision if the owner asks.

**TTS is the count, not a description.** Voice does not caption a photo and read the caption.
Playback owns the audio clock (WASAPI shared on Windows, Core Audio on Mac). Speech opens a
second **shared** client and never exclusive mode, so it cannot move that clock. While a clip
is playing, results still appear and the spoken count waits.

### The Windows spike (PR 27, before any UI)

Microsoft's on-device recognizer is [Windows AI speech recognition](https://learn.microsoft.com/en-us/windows/ai/apis/speech-recognition)
(`Microsoft.Windows.AI.Speech`): Windows 11 24H2 or later, on an NPU or on the CPU, audio
stays on the machine. The same document requires an **MSIX** package with the
`systemAIModels` capability. This app is unpackaged — GPL-3.0-or-later, direct download, no
Store ([11](11-licensing.md), [12](12-decision-log.md) 2026-09-06). The viewer's floor stays
Windows 10 21H2. Voice does not raise it.

PR 27 measures, on an unpackaged build, before the feature is wired up:

1. Can the process create that recognizer with no MSIX identity?
2. During a recognition, are there zero new outbound connections?
3. On a CPU-only PC the model arrives through Windows Update. Does our consent dialog gate
   `EnsureReadyAsync`, and does declining leave the app otherwise unchanged?

**If all three hold,** Windows Voice uses that OS recognizer. Machines that report
`NotSupportedOnCurrentSystem`, and Windows 10, keep the viewer and show Voice as unavailable.
The docs' suggested fallbacks (the old cloud-capable SDK recognizer, or a cloud service) are
refused.

**If any of the three fails,** the Voice add-on carries its own native recognizer
(whisper.cpp, MIT, CPU, a small ggml model whose weights licence passes the same gate as
[17](17-local-ai-search.md)). That model file lives in **this** add-on, never in the AI pack
and never in the base tree. Mac keeps the Speech framework either way. The spike records the
choice and the download size in this doc; those numbers replace the guesses.

TTS does not wait on the spike. The OS speaking voice above is the Windows path in both outcomes.

## The Voice pack (delivery)

Same mechanism as Import and Local search. Settings → Add-ons lists **Voice** with its size,
version, and Install / Remove. The download is a plain GET of a fixed URL on the update
channel ([13](13-updates-and-telemetry.md)): no identifier, no path, no telemetry. Signed
manifest, hash per file, licence per file. Verify, then load.

- **Location:** `%LocalAppData%\MediaViewer\addons\voice\<version>` on Windows and
  `~/Library/Application Support/MediaViewer/Add-ons/Voice/<version>` on Mac.
- **Native library:** `mv_voice.dll` / `libmv_voice.dylib`, exporting the same
  `mv_addon_get` entry Import defined. It does not link the core, and it does not link the
  AI add-on. It does not open `index.db`.
- **Chrome:** `MediaViewer.Voice.Chrome.dll` in its own `AssemblyLoadContext` on Windows;
  `Voice.bundle` via `NSBundle` on Mac. A mic button on the PR 22 search box, present only
  while this add-on is loaded.
- **Host call, appended.** The host table gains `search_query(text) → job id` at the end.
  Existing ordinals stay put, so an Import build that speaks an older host API still loads.
  The host forwards the string to the AI add-on when that add-on is loaded, and returns
  "not installed" when it is not. Results come back as the search completions PR 22 already
  defined. Voice never learns the index layout.
- **Absent means absent.** With Voice not installed the base tree is byte-identical, and no
  mic button or voice key exists. The packaging assert fails if a speech runtime, a ggml
  model, ONNX Runtime, or Windows App SDK AI lands in the base tree
  ([13](13-updates-and-telemetry.md)).

## How a query runs

1. **Key down** (or the mic button). First use asks for the microphone, and on Mac for speech
   recognition. The OS dialog is the ask. Denied: Voice explains where to enable it, and
   does not ask again until the user opens Settings.
2. **Capture** on a worker. Partial text may show in the search box. Nothing is written to disk.
3. **Key up.** The final transcript is the query. The add-on calls `search_query`. Local
   search embeds the text and scans, exactly as if it had been typed.
4. **Results** fill the gallery, same tiles, same Enter-to-open, same match markers.
5. **Speak** the count, when no clip is playing. The line is also the status text, so sound
   off still tells you what happened.

Scope, kind filter, min-score cutoff and "nothing found" are Local search's. Voice does not
grow a second set.

## Commands

New rows, present only while Voice is installed. Checked against the live table when PR 27
lands ([16-commands.md](16-commands.md)); a taken key loses, and this list is updated.

| Command | Windows | Mac |
|---|---|---|
| Hold to talk | `Ctrl+Shift+Space` | `⌘⇧Space` |
| Mic button | On the search box | On the search box |

`Space` stays next-image / play. Follow-up phrases land in PR 28 and map to command ids that
already exist. They are exact short phrases, and only while a voice result set is up:
"open the first", "next", "previous", "close". Any longer sentence is a new query.

## Privacy

- The microphone buffer lives until the utterance ends, then it is discarded. It is not an
  index row, not a log line, not a crash field.
- The transcript is the search string in the box, the same as a typed query. It is not logged.
- A minidump from a crash during capture contains no audio samples and no transcript. The
  filter excludes that buffer the way it excludes decoded image heaps
  ([13](13-updates-and-telemetry.md)).
- The OS may download a dictation language or, on a Windows CPU PC that took the OS-recognizer
  path, a speech model through its own update channel. That download happens only after the
  user agrees in our dialog, and our process still sends no audio and no identifier.

## Not hurting the viewer

Capture and recognition are a new worker role, lowest priority, at most one recognition at a
time. They are not the decode pool and not an inference worker from the AI pack. The
generation counter cancels a query whose folder the user has already left. Synthesis of one
short line does not start a present loop: a still returns to idle after the gallery has
drawn. Memory for the audio buffer is capped at one utterance (a few seconds). Counted
against the budgets in [02-architecture.md](02-architecture.md).

## Roadmap slices

Dual-track, like every PR from 9. Shared add-on library and host call; a WinUI half and a
SwiftUI half; each verify line on **both** platforms. Both present-loop gates hold while
listening and while speaking.

### PR 27 — Listen, ask, answer

The Windows spike, then the pack: install/remove, hold-to-talk, the mic button, the
transcript in the search box, `search_query` through the host, the gallery results, the
spoken count. Permission prompts. The "Local search is not installed" state.

**Verify (both platforms):**
- Voice absent: the base install tree is byte-identical, and no mic control or voice key
  exists. A tampered file or manifest is refused. The download request carries no identifier.
- With Local search installed and a folder indexed: holding the key and speaking a sentence
  from the PR 22 eval set shows the **same** top results as typing that sentence. The spoken
  count matches the grid. Enter still opens the result.
- With Voice installed and Local search absent: holding the key searches nothing, says Local
  search is required, and starts no download.
- A capture with the network blocked opens no new outbound connection. A forced crash during
  capture dumps no audio samples and no transcript.
- Both present-loop gates hold while the key is held on a cached photo, and while the count
  is spoken. PR 1's idle stop still happens once the line has finished.

### PR 28 — Corrections, follow-ups, missing speech

The transcript stays editable; confirming the edited box re-runs the search. The four
follow-up phrases. The missing-language and missing-model states. Uninstall.

**Verify (both platforms):**
- Editing the heard sentence and confirming returns what typing it returns.
- "Open the first", "next", "previous" and "close" do what the matching keys do. A full
  sentence starts a new query instead.
- With the OS dictation language (or the Windows model, on that path) not installed, Voice
  says what to install and does not contact a recognition server. Declining a Windows model
  download leaves the rest of the app unchanged.
- Removing Voice deletes its folder and leaves the AI index in place. Removing Local search
  leaves Voice unable to search, with the mic still local.

## Open decisions (owner)

1. **Windows recognizer** — settled by the PR 27 spike, not before. OS speech if an
   unpackaged process can use it with no network; otherwise a small native model inside this
   add-on. Mac is the Speech framework in either case.
2. ~~**Is this a D10?**~~ **No.** It is plan/19 plus the decision-log entry. Not a numbered
   D-decision.

## Explicitly not in this feature

Searching the words **spoken inside a video** (a transcript index — still out of
[17](17-local-ai-search.md)), captioning, a wake word, always-on listening, cloud
recognition or cloud voices, voice control of edit / trim / import / delete, and shipping
any of this inside the AI pack or the base installer.
