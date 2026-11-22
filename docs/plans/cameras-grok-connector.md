# Plan — Cameras: ask Grok Bot what happened at home (#96)

Written 2026-11-09 from the code at `cb3d8906`. Part of the Cameras add-on (#96), which is not built:
`src/addons/cameras` does not exist. Dual-track (D9). Slice names only; numbers are assigned when
the slices are scheduled.

Status: **plan, not built.** Nothing derived from footage leaves the machine until the
[decision-log row](#decision-log-row) has landed. "Line N" means line N of the output of
`gh issue view 96 --repo longtimeno-c/mediaviewer`. Research ran on 2026-11-09 and read documents and
source, plus one local Vision test on synthetic images: no camera, recorder, Grok Bot or Tailscale call
has been made.

## What it is, and who decided what

From a phone the owner asks Grok Bot what happened, on any camera ("has my package come?", "anything
in the garden last night?") and gets a complete text answer built from the home event store. The
memory lives on the Mac mini and Grok is only its voice: Grok asks per question, with no bulk sync. A
picture of a person, a clip or a car stays at home behind a private link. The Mac mini also runs three
concrete rules and sends a "worth checking" phone notification through a Grok Bot routine. All of it
comes from the owner's own UniFi Protect recorder through an MCP server inside the Cameras add-on.

Owner decisions, 2026-11-09:

| # | Decision |
|---|---|
| 1 | A MediaViewer add-on feature, part of Cameras, not a separate project. |
| 2 | Grok may receive allow-listed text plus one redacted thumbnail with nobody in it (decision 10). The answer is complete as text, because display of MCP images in Grok Bot is unproven. |
| 3 | The clip never goes through Grok. Grok gets a short-lived link that opens only over the owner's Tailscale network. |
| 4 | Accepted: a sent thumbnail cannot be recalled. "An expired event is never shown" narrows on this path to the [promise below](#retention-promise). |
| 5 | UniFi clips: about a minute from a RAM buffer of the live stream, package events only, door camera only, video without audio, deleted when the recorder's footage expires. Open in Protect when no clip was saved. No UniFi private API. Needs TLS in the bundled FFmpeg. Pre-roll is unmeasured; package events may fire after the courier has left. |
| 6 | Defaults the owner may overrule: no public signed image URL (if Grok cannot show inline images, text only); the tunnel is a Tailscale Funnel the owner runs, not bundled. |
| 7 | Built and proven on the owner's setup first; others later, on the [terms below](#before-release-to-others). |
| 8 | Individual Grok Bot plan: no data processing agreement applies, training exclusion is unclear. Check Privacy Mode and prefer a dedicated account before any real thumbnail. The door camera's model and whether it detects packages are unknown. |
| 9 | "Literally like Jarvis": every camera feed is queryable, the Bot can ask what happened and knows the goings-on, and it monitors for concerning behaviour. Concrete rules (decision 13) are how it monitors. |
| 10 | Pictures of people stay at home on every camera, behind private Tailscale-only links. Grok only ever receives a redacted image with nobody in it. Events with people reach Grok as generic text with no identities: no names, faces, plates or transcripts. |
| 11 | The owner has no indoor cameras. Indoor handling is a [release question](#before-release-to-others) only. |
| 12 | Alerts are phone notifications sent through Grok Bot. Reverses line 545. Untested ([slice 0b](#slice-0b-does-a-webhook-reach-the-phone)). |
| 13 | Three first rules on the Mac mini, worded "worth checking", never an AI judgement of "suspicious": a person in the garden overnight; someone lingering at the door without ringing; the same unknown car passing repeatedly. |
| 14 | Saved clips stay door-only, package events only. Other cameras get the add-on's saved frames plus Open in Protect, so this does not become a second recorder. |
| 15 | Alert payloads are fixed: rule name, camera alias, time, private link. No pictures, no free-form AI text. The add-on keeps its own alert log and flags failed webhook calls. |

Tools exposed to Grok are read-only and exclude faces, plates, speech and descriptions.

## What is not in it

Free-text or semantic search, faces, plates, vehicle identities, speech or generated descriptions over
MCP or in an alert. An AI judgement of what is suspicious. A bulk sync, export or history cursor. A live
snapshot on request, a frame at a chosen time. Clips from any other camera or event type. Hikvision
(out of scope here, not ruled out). Another alert channel. Cloud indexing or captioning (that half of
line 548 stands). A tunnel bundled, installed or started by MediaViewer; any MediaViewer server. Audio in
clips. UniFi's private API (line 117), including its event thumbnails, video, ring and plate data. Being
an open add-on: other makers' add-ons have no network permission ([25](../design/25-open-addons.md)
line 50).

`--search-agent` is the wrong host: it exits 50 s after its last client and makes no network request
([23](../design/23-nle-search.md) lines 87-96).

## The shape

The server runs on its own threads inside the kept-alive viewer process (line 415; `keep_alive`,
`mediaviewer_addon.h:822`), with `net::try_lock_instance` (`src/addons/driftwire/net.h:226`) because
slot add-ons load in every window. Two listeners on fixed IPv4 loopback ports, reachable only through
the owner's Tailscale. `net::listen_internet(port, true)` (`net.h:82`) will not do: where IPv6 exists
it binds `::1` only (`net_posix.cpp:348-352`, `net_win.cpp:421-426`; the comment at `net.h:81` is
wrong), so a proxy aimed at `127.0.0.1` is refused, and `net::listen` has no fixed port (`net.h:58-60`).
Foundation adds `net::listen_loopback_v4(port)`, posix and win, and corrects that comment.

```
AT HOME (Mac mini, no window; all memory is here)
 Protect device socket: each opted-in camera's state, detection on/off -> gap rows
 Protect event socket, each opted-in camera:
   allow-listed kinds -> event row (enums and times, nothing else)
   person or vehicle -> one saved frame (snapshot), add-on folder, never sent to Grok
   package at the door camera -> snapshot into RAM -> clip cut from RAM ring -> frame chosen
     -> redaction -> egress JPEG stored
     (any step after the event row may produce nothing; the answer still works)
   rules over event rows (Repeat car also over C6's local plates)
THE QUESTION
 phone -> Grok Bot (Cursor-hosted) -> HTTPS -> Funnel relay (ciphertext) -> tailscaled on the Mac
   -> 127.0.0.1:A /mcp, bearer token checked; every tool rechecks the cut-off and the limits; local log
      1 what_happened      every camera, text only, honest coverage
      2 package_status     door camera, text only
      3 event_thumbnail    gates, one atomic claim with its log row, one JPEG, nobody in it
                           (if slice 0 allows)
      4 picture_link       private link to a saved frame
      5 package_clip_link  private link to a door package clip
THE ALERT
 rule fires -> de-duplicate, rate limit -> bounded wait for the saved frame
   -> alert log row `claimed`, link token minted once -> recheck cut-off, consent, age before every attempt
   -> HTTPS POST + Bearer key -> Grok Bot routine webhook -> 200 means the run started
   -> run finishes -> Bot notifies the phone (or the desktop: slice 0b)
THE PICTURE (never touches Grok)
 phone on the tailnet -> tailscale serve -> 127.0.0.1:B /f/<token> or /c/<token>
   -> Tailscale-User-Login must equal the owner -> JPEG, or MP4 with byte ranges, else 403
```

What crosses to Grok, alerts included, reaches Cursor (Anysphere), SpaceXSI, and third-party model providers on
Cursor's sub-processor list ([security][bot-sec], [FAQ][bot-faq]). Funnel relays do not decrypt; TLS
ends in `tailscaled` on the Mac ([Funnel][funnel]).

## Door 0: what can these cameras trigger?

First, because it is the cheapest way to end part of the project. The owner makes the API key. Scratch
only, no repo code. `GET /v1/cameras` once: for each camera `featureFlags.smartDetectTypes`,
`smartDetectSettings.objectTypes`, `hasPackageCamera` ([spec][protect]). Then watch
`/v1/subscribe/events` while the owner, for about twenty minutes in all: stages one box at the door; walks
through the garden zone; stands at the door for two minutes; presses the bell; drives a car past every
camera that can see the road.

*Verify:* a dated note here with those fields per camera, whether each event came, and, for the person
events, whether `add` arrived with `end` null or already closed; for a `smartDetectLoiterZone` event, how
long after arrival its `start` falls and when its `end` comes against the person actually leaving (the spec
has no dwell or threshold field); which camera sees the road. The spec has no doorbell flag in
`featureFlags`, so the note also says which cameras have a bell. No package event: the package path has no
trigger, so stop that path and tell the owner; the rest does not depend on it. No `ring`: [open call
13](#open-calls). No open person event: Lingering can only use `smartDetectLoiterZone`. No vehicle event on
any camera: Repeat car has no trigger, so tell the owner. No shipped code, so no halves or gates.

## Slice 0: does Grok Bot show an MCP image?

After Door 0, before any other work. Synthetic images only, no camera, no repo code: a throwaway script outside the
tree behind a public tunnel. The owner signs in and connects it.

No xAI page read says a custom MCP image block is shown to the person ([connectors][grok-conn],
[files and results][bot-files]), and the spec obliges no client to show one ([MCP tools][mcp-tools]).
Third-party changelog copies say connector images reach the Bot as images from 0.75.8: the model, not
display ([gradually.ai][gradually], [wiki][wiki]; x.ai/changelog/bot returned 403). Cursor has drawn
nothing for an image its model received ([forum][cursor-bug]). MCP App UI is reported ([#1694][gh1694]).

One tool, `probe(mode, size)`, returns a fresh JPEG with a random six-digit code **only in its
pixels**. Tool list final before connecting ([#1402][gh1402]); `initialize` challenged, or the server
connects with no token ([#1694][gh1694]); a missing `MCP-Protocol-Version` tolerated ([wiki][wiki]).

- **Modes:** image block, also with `audience: ["user"]`; embedded blob; `resource_link`; markdown
  public URL; markdown tailnet URL; MCP App card, including one loading its image from a tailnet
  address so the pixels never pass through Grok (untested idea).
- **Sizes:** 20 KB to 2 MB. **Clients:** current Bot desktop, iPhone, Android. grok.com has a separate
  OAuth client ([#1694][gh1694]); try it, and do not carry results across.
- **Score:** model reads the code the same turn; the next turn; the person sees the picture; any server
  fetch; a file on the Bot's computer or in its Library; whether the Bot can re-send it as an attachment.
  Also the OAuth registration used, the redirect URI, and the timeout seen.

*Verify:* a dated results table in Status, with app versions. No shipped code, so no halves or gates.

| Result | Consequence |
|---|---|
| The person sees an inline image | Thumbnail slice goes ahead with that mode |
| An MCP App card shows it from the tailnet | Prefer it: no pixels reach Grok. Owner confirms |
| Model reads it, person does not see it | No image is sent; text only, by decision 6. The owner may add a tailnet still (open call 8), which would need its own tool field, slice and verify |
| Display needs a public URL | Text only, by decision 6, unless overruled. It would rest only on signature and expiry: Bot computers share egress IPs ([private networks][bot-priv]) |

## Slice 0b: does a webhook reach the phone?

With slice 0, before any alert code. Invented payloads only, no camera, no repo code. The owner creates a
Bot and routine and gives a throwaway script the URL and key. Those are shown only in the routine's Webhook
section on desktop; the phone's routine view has none ([routines][bot-routines], [mobile][bot-mobile]). So the
owner signs in the desktop app once for setup, on a machine that is not signed in to the tailnet. The key this
slice uses is retired afterwards: the real routine gets a fresh key, or is a new routine if a key cannot be
replaced.

Documented ([routines][bot-routines], [mobile][bot-mobile], [notifications][bot-notify]): the POST carries
`Authorization: Bearer <key>` and an optional JSON body, and the Bot receives the body with the routine
instruction. `200` means a run started, any other response means none did, and a run over the limit (200
an hour per routine, 300 across routines) fails with "Automation run rate limit exceeded" and is not retried.
The same error can hit a run below those numbers, because of a limit all Grok Bot routines share, which the
docs call a known issue being fixed; for a webhook they say to send the call again, and they do not say which
status the sender gets. Each run spends usage. A long absence
may pause routines. A Bot with Notifications on notifies when it finishes or is waiting. While the desktop
app counts as in use (open, signed in, screen unlocked, computer used recently, banners allowed), finished
replies from owned Bots go to the desktop and skip the phone, with no setting to change that; when away or
idle the phone is notified. iPhone push is "rolling out gradually".

Send at least 20 payloads, day and night, phone locked, and score:

- **Delivery:** phone notified or not, with the desktop signed in once for setup, then signed out and its
  computer removed (Remove Computer while it is offline; restarting Grok Bot on it adds it back,
  [mobile][bot-mobile]); in use on macOS; open but idle; screen locked; quit. Focus mode and app closed on the
  phone. iPhone and Android.
- **Latency:** POST to `200`, to the run finishing, to the notification. Median and worst.
- **Content:** what the banner shows; whether the Bot repeats the fields unchanged or rewords them; whether
  the link arrives intact; what a tap opens.
- **Failure:** wrong key, paused routine, oversize body, double POST; the status a rate-limited webhook
  returns, the shared-limit error included if it can be provoked, and whether that failure can come after a
  `200`; what happens when the weekly usage limit runs out; how a key is replaced and how soon the old one stops
  working; usage cost per run.
- **Injection:** one inert invented payload, on a Bot with nothing connected because a test run does real work
  ([routines][bot-routines]), with an instruction in the `camera` field, to see what an unvalidated body would
  do. The shipped alias is typed only by the owner and never derived from the recorder, so a visitor or a
  recorder string cannot reach it; its charset limits length and symbols, not meaning.

*Verify:* a dated results table in Status, with app versions. No shipped code, so no halves or gates.

| Result | Consequence |
|---|---|
| Phone notified reliably after setup with the computer removed | If the owner accepts no Bot desktop on that account ([open call 14](#open-calls)), alerts go ahead; replacing the key repeats the setup step |
| Phone notified only when the desktop is away or quit | [Open call 14](#open-calls) |
| Banner rewords the fields or drops the link | Tighten the routine text and retest; if it still rewords, tell the owner, as decision 15 then holds only up to the POST |
| No reliable phone notification | Alerts stop. No other channel without the owner's say-so |

## What it needs from Cameras

Issue 96 wants a spike, a plan doc and a decision-log row before Cameras code (lines 25, 452, 490-510).
None exists: [12](../design/12-decision-log.md) has no Cameras row. Whether Door plus this file replace
the spike and plan doc for what this plan builds is [open call 2](#open-calls). The Foundation slice
builds `mv.cameras.1` (line 445).

| Issue slice | Needed here | Not needed here |
|---|---|---|
| **C1** (line 516) | UniFi adapter, local server only; HTTPS/WSS client with a pinned certificate; secrets; always-on mode and recovery; Settings → Cameras; the network thread role; for Thumbnail, one model file from its publisher, and for Repeat car, the plate detector and OCR model (open call 3) | Hikvision, the rest of the model catalogue and its tiers |
| **C2** (line 519) | Event store and retention end to end: a required cut-off, enforced twice (line 294), sweep and scan (lines 302-303) | Towers, vector index, query terms, results grid |
| **C6** (line 534) | Repeat car only: local plate detection, OCR and multi-frame voting, on the pixel source [open call 16](#open-calls) settles. Line 534's vehicle cards have a name, merge and correct but no known or unknown mark, so that mark is new work in the Repeat car slice | Lookup, `plate:` search |
| **C3** (line 522) | Nothing; descriptions must never reach this path. Its region masks could share the door polygon's editor | All of it |

## Slices

A code slice is done when its verify line and both gates hold on both platforms (CLAUDE.md). A half
may start ahead on a branch; PRs stay draft. Linux stays hostable through `*_none.cpp` stubs until
[linux.md](linux.md) §10 settles the order. Screens are add-on view trees, so no WinUI or SwiftUI code.
"Both gates", on Windows and Mac with the feature on against base and the slice's work running: Windows
PR 1 and Mac PR 1 (`--soak 60 --gate`, [DEVELOPMENT](../DEVELOPMENT.md) line 758), and nothing on
CLAUDE.md's no-regression list worse: launch → first pixel, launch → full resolution, arrow → next photo
(cached, cold, quick, held), pan pacing, idle (`--static --json`), video pacing. Lab JSON from base,
new, base, new, same machine and configuration, warm cache, base in its own build directory. The add-on
starts after the first pixel (line 470). Linux PR 1's gate joins once CI's Linux GPU runner is enabled
(`MV_LINUX_GPU_RUNNER_ENABLED`, `.github/workflows/ci.yml:986`). Each Windows verify is proven as
[open call 1](#open-calls) settles.

Order: Door 0; slices 0 and 0b; Door; Foundation; Answer; Frames; then Clip, Thumbnail and Alerts, each on
its own prerequisites; Lingering after Alerts. Repeat car after Alerts and C6, independent of Lingering; before
C6's Repeat car work, open call 16 is settled and line 503's plate measurement runs on the chosen camera.

**Door** (measurement, after slice 0). Scratch scripts and a scratch FFmpeg outside the repo. Place a
box at least ten times, day and night, logging walk-up, `start`, `end`, socket receive and the
console-clock to ring offset with its skew; and how long after `end` a receipt can come, which bounds
the snapshot.
- **Which lens and quality.** Outputs `ring_quality` and `snapshot_channel` per camera. If
  `hasPackageCamera`, record whether package events coincide with the package lens seeing the box.
  Bitrate, codec and GOP for that quality only; snapshot sizes; burned-in text; whether person and
  vehicle zones cover the polygon.
- **TLS.** The bundled FFmpeg cannot open RTSPS (`--disable-autodetect`, `--disable-openssl`:
  `ports/ffmpeg/portfile.cmake:36,462`), so build the same n9.0.1 source with
  `--enable-securetransport`, outside the repo. Connect with and without `?enableSrtp`, allow-list
  `tls,tcp`, the leaf captured from the RTSPS port as `ca_file` plus `verifyhost`, `tls_verify` on; a
  different certificate must be refused. The Schannel connection has its own check on a Windows machine
  before the Clip Windows half starts.
- **Key privilege.** Whether a view-only local user can mint a key that still reaches events, snapshot
  and `rtsps-stream`. Use the least privilege that works; the spec has no `securitySchemes` describing
  key scope, and the same key can reach sirens, relays and arm profiles.
- **Rule inputs.** A week of person events on the garden and door cameras and vehicle events on the camera
  that sees the road, written to a scratch file only: night-time detection rate, whether events start before or
  after a person has gone, how long a normal caller stands at the door, the loiter `start` offset from arrival,
  how soon a snapshot after `add` still shows the person or the car, and how long a snapshot takes to save.

*Verify:* a dated table here, with pre-roll, ring length, the lens, the key's role, the rule
thresholds chosen from it, and the bound on an alert's wait for its frame.

**Foundation** (after Door; open call 2 decides whether it may start). What no other slice owns:
`src/addons/cameras`, manifest, packing (`mv.cameras.1`); the UniFi adapter, on one event socket for every
opted-in camera, each with an alias typed by the owner (`[A-Za-z0-9 -]{1,24}`, checked on input and on output,
never derived from the recorder) and a "has a doorbell" tick set from Door 0; the event store with the allow-list under [Tools](#tools); a retention
bound required at setup; sweep and scan; Settings → Cameras; secrets; always-on start;
`net::listen_loopback_v4`.
*Mac half:* NSURLSession HTTPS and WebSocket with the pin, on the `fetch_*` pattern
(`src/addons/ai/fetch.h`, GET only today); a sleep hold from a generalised
`src/addons/driftwire/power.h`. *Windows half:* WinHTTP WebSocket and pin; `power_win`; tray and
`--background` start through `keep_alive`.
*Setup:* the Mac mini needs a Developer ID build with the keychain access group, because a dev build or
the lab binary has no secret store and registers no login item
([28](../design/28-driftwire.md) lines 572-574, 1188-1190). From a draft branch that is
`macpack.py release --team-id` with the add-on signed by a key that build accepts; the owner runs it. A
login item does nothing before login, so the Mac must log in unattended after a reboot.

> **Mac and Windows:** the add-on installs from its package and starts after the first pixel. A camera
> cannot be added without a retention bound (line 520). With seeded events past the cut-off, a scan of
> the database, free pages included, finds nothing older. A camera not opted in produces no row, and a
> recorder message with an unknown type or field stores nothing from it. A real `tailscale serve` reaches the loopback
> listener. The API key survives a restart in the secret store of the signed build. Both gates.

**Answer** (needs Foundation). HTTP server (new add-on code on `net::`; none exists), Streamable HTTP,
OAuth, all five tools with their final schemas, the call limits and row budget, local log, Settings rows, setup text. The
tools are registered from the start because the client refreshes its list only on re-authentication
([#1402][gh1402]): `event_thumbnail` answers `none` / `off` and the two link tools answer `none` until
their slices land; if slice 0 rules images out, `event_thumbnail` is not registered. A second WebSocket on
`/v1/subscribe/devices`, with a `GET` of each camera on each connect, because `state` and
`smartDetectSettings` are on the camera object and do not arrive on the event socket ([spec][protect]);
offline and detection-off intervals persist as gaps, per camera.

> **Mac and Windows:** with a real or replayed Protect socket, asking Grok Bot on a phone returns each
> answer form of `package_status` and `what_happened` with exactly the fields in [Tools](#tools), and no
> field outside the schema; all five tools are listed from the first connection. With canaries seeded in
> the recorder's camera name, a face name, a plate, a transcript and a description, none appears in any
> response. `what_happened` returns `nothing_seen` only with full coverage: a default call across a doorbell
> camera and a non-doorbell camera, all complete and with no events, returns `nothing_seen` and names what is not
> watched, and the same call asking for `doorbell_ring` by name returns `cannot_tell`. It caps the rows and accepts
> no time or cursor argument. Each of the five tools answers `rate_limited` past the limits, on one shared counter.
> 24 hourly polls plus a sweep of `24h` and `7d` by camera and kind release no more than the budget of distinct rows
> in a day, `7d` never lists an event, and `counts` stay complete past the budget. With no token, `initialize` gets a 401
> with `resource_metadata`; a revoked token is refused at once. `/authorize` answers 404 unless pairing
> is armed, disarms after one grant, shows the redirect host and refuses any other redirect URI. An
> already-ended `update` for an unknown id makes an event. Killing the socket for ten minutes, unplugging
> the door camera mid-window and switching package detection off each give `cannot_tell`, never
> `no_package_seen`; a gap on one camera does the same for `nothing_seen`. With the sweep disabled, an event past the cut-off is never returned. After a reboot
> the add-on recovers at login with no hand on the machine; after a forced kill it recovers at the next
> launch or login, as nothing relaunches it ([28](../design/28-driftwire.md) lines 565-570), and until
> then Grok gets a connection error, never an answer. A scan of logs, database, crash annotations and
> minidumps finds no canary API key, stream URL or token. Parser fuzz corpora run clean. Both gates.

**Frames** (after Answer). `picture_link` goes live, with `/f/<token>` and listener B. One saved frame per
person or vehicle event on each opted-in camera: `GET /v1/cameras/{id}/snapshot?highQuality=true`
([spec][protect]) on the first message for that event, on the network thread, with a per-camera minimum
interval. JPEG magic bytes and a size cap checked, `.partial` then rename, random name, its own folder, a
disk cap, oldest first. Same expiry and backup exclusion as a clip. The frame is the camera's picture at
fetch time, so an event that arrives late may show an empty scene. The redaction code has no call that
reads this folder. No platform code beyond Foundation's HTTPS client, so the Mac and Windows halves are the
same add-on code built and verified on each.

> **Mac and Windows:** a staged person on each opted-in camera gives one frame that opens on the owner's
> iPhone over Tailscale, with `picture_link` as the only route. A staged vehicle gives one frame. With no frame
> saved on a non-door camera, `picture_link` returns `none`, `fallback` `open_in_protect` and fixed text naming that
> camera's alias. The URL fails from off the tailnet, through
> Funnel, with a wrong login, from a second device signed in as the owner (with open call 11's grant) and
> when expired. Sign-out, feature off and camera opt-out answer 410 at once. A marker in a saved frame's bytes
> never appears in anything sent to Grok, by capture of every outgoing response. A burst of events keeps to the
> interval and the disk cap. With footage past the cut-off, a scan finds no frame or row and an earlier token
> answers 410. A kill during a save leaves no `.partial` after the next sweep. Both gates with frames being saved.

**Clip** (after Door and Frames). `package_clip_link` goes live, with `/c/<token>` on listener B.
Host-table rows appended after `still_extend` (`mediaviewer_addon.h:1029`), with a
[14](../design/14-abi.md) row: `stream_open` (URL, video only, ring seconds, protocol allow-list, TLS
pin), `stream_peer_cert` (a handshake on host and port that returns the leaf and sends nothing),
`stream_status` (oldest, newest, connected), `stream_frame_rgb` and `stream_save_clip` (both by wall-clock
time; stream copy from held AVPackets), `stream_close`; today's rows take a source file (lines 629, 884,
899 of that header). Each held packet is stamped with a wall-clock time: Mac or PC receive time corrected
by Door's offset, or RTCP where present (`rtsp.c:2531`, set once after the first sender report).
The host installs an `av_log` callback (none exists in `src/`) that drops every line from a stream's
contexts rather than masking strings, as messages arrive in fragments and `rtsp.c:2222` prints the URL on
a redirect at INFO.
FFmpeg TLS: the Mac needs an overlay port with `--enable-securetransport` (precedent
`tools/vcpkg/ports/libraw`) or the OpenSSL feature (open call 9). *Mac pin:* the leaf written as a PEM file,
passed as `ca_file` plus `verifyhost` (`tls_securetransport.c:282-291`); a fingerprint cannot be passed.
*Windows half:* the port adds `--enable-schannel` when OpenSSL is off (`ports/ffmpeg/portfile.cmake:462-464`).
Schannel has no `ca_file` and offers only system-store validation or none
(`tls_schannel.c:1155-1161`), so the overlay port carries a small patch: an option holding the leaf's
SHA-256 that selects manual validation and compares after the handshake. `tls_verify=0` is never set, and
a test greps for it. If neither holds on a platform, Clip is blocked there and the answer stays Open in
Protect (open call 10). Rows through `src/abi/addon_abi.cpp`. *Mac half:* wiring in
`src/shell/addons_mac.mm`.

> **Mac and Windows:** ten staged package events give ten MP4s playable on the owner's iPhone over
> Tailscale, about a minute each, one video stream, no audio stream (`ffprobe`), no re-encode. A person
> event alone saves no clip; nor does another camera. Asked before a clip is written, the answer says
> `pending`. When the ring missed the event, `clip` is `none`, `fallback` is `open_in_protect` and the
> text sends the owner to the Protect app. The URL fails from off the tailnet, through Funnel, with a
> wrong login, from a second device signed in as the owner (with open call 11's grant) and when expired.
> Sign-out, feature off and camera opt-out make an issued link answer 410 at once. With footage past the
> cut-off, a scan finds no clip file or row and an earlier token answers 410. A kill during a save leaves
> no `.partial` after the next sweep. The ring holds its budget over 24 h. A changed console certificate
> is refused. A canary in the stream URL, with a forced 302 and verbose logging, reaches no log or
> minidump. Both gates with the ring running and during a save.

**Thumbnail** (after slice 0 allows it, the account checks, and Clip). Cameras carries ONNX Runtime (line
104) and fetches `face_detection_yunet_2023mar.onnx` from `opencv/face_detection_yunet`, at a pinned
revision and SHA-256, with `remote_file` (`src/addons/ai/fetch.h:23,38`), subject to open call 3. The
host's `still_faces` needs Local search and hides the confidence and the 4 % floor, so it is not used.
Rows `encode_jpeg_pixels`, over
`encode_jpeg_rgba` (`src/codec/decode.h:226`), which is given only pixels and so cannot carry EXIF;
and `decode_jpeg_rgb` for the snapshot fallback, as `decode_still_rgb` takes a path. A per-camera
polygon editor, the gates, `event_thumbnail`. `moment_thumbnail` and the JPEG-512 cache are not used:
`camera:` keys fail its `stat` (`src/abi/addon_media.cpp:100-104`), and that cache holds the owner's
photos. *Mac half:* a Vision port for text boxes (word boxes exist at
`src/shell/text_recognition_mac.mm:74`; `text_reader.h` returns strings only), text regions and
barcodes. *Windows half:* the same on `Windows.Media.Ocr`, whose word rectangles `ocr_win.cpp` does not
read today, plus a text-region and a barcode detector still to be chosen; without them Windows sends no
image.

> **Mac and Windows:** on a labelled door set, day and night, per-gate miss rates are recorded and the
> owner signs the target. No image is sent, and the text answer is still complete, with: any event
> carrying person or face, on any camera; any camera with no polygon or no ring; each detector
> removed or forced to time out; no polygon; images off; either account check unticked; an overlapping
> person or vehicle event; person detection off; the socket cut across the frame; a night IR frame; text
> fill over the measured share; the wrong channel, a resized source, a degenerate polygon or a shifted
> camera. A staged person (back to the camera included), face, printed word, QR code, barcode or
> non-Latin label in the zone is withheld or filled. An event with both person and package types saves a
> clip and sends no image. Every pixel outside the polygon equals the fill and the crop equals its bounding
> box. A marker scan of every outgoing JPEG finds only SOI, JFIF APP0, DQT, SOF, DHT, SOS, EOI. The budget
> refuses the next image. An expired or unknown token sends nothing. Concurrent calls send one image; a
> kill between claim and send leaves an `attempted` row and no resend. Every image sent has one log row
> with a matching SHA-256 and no pixel in the log. Turning images off deletes stored egress JPEGs. With
> footage past the cut-off, a scan of the data folder, free pages included, finds no egress JPEG or event
> row; a kill during encoding leaves nothing after the next sweep. Both gates during redaction.

**Alerts** (after Frames and a passing slice 0b). [Alerts](#alerts) delivery, the alert log, Settings rows
for the webhook and each rule, and Person overnight. Rules and the limiter are shared code with an injected
clock. *Mac half:* POST over NSURLSession. *Windows half:* POST over WinHTTP. The add-on's fetcher is GET
only (`src/addons/ai/fetch.h:56`), so both are new; system trust, not the recorder pin.

> **Mac and Windows:** with a replayed socket, a person event inside the window fires exactly once; one
> outside it, or on another camera, does not. In dry run nothing is sent and the log says what would have
> been. Against a local fake webhook: the body is exactly the four fields, within the size cap; a capture of
> it and of every log finds no picture, plate, face name or recorder string; a 500 and a dropped connection
> retry to the limit and end `failed` or `uncertain`; a 401 does not retry and raises the Settings flag; a
> redirect is refused; the cool-down and both caps hold under a burst, and the cap notice goes once per period. A
> frame that lands inside the wait gives the link; a slow or failed snapshot gives `link` null after the wait and
> mints no token. A replayed event past the maximum age ends `stale`, unsent. Before every attempt, retries
> included, an expired event, feature off, camera opted out or rule disarmed ends the row with nothing sent. A kill
> between claim and POST leaves a `claimed` row that becomes `uncertain` at the next start, is flagged and is not
> resent. Every retry's body is byte-identical. After a restart an issued alert link still opens until its expiry,
> sign-out, feature off and opt-out still answer 410, and no raw token is on disk. The flag stays until
> acknowledged, and `what_happened` states how many alerts may not have been sent and how many were held back. The key survives a restart in the
> secret store and appears in no log, database, crash annotation or minidump (canary). Against the real
> routine, a staged walk at night notifies the owner's phone within slice 0b's measured bound, the link
> opens over Tailscale, and fails with the phone off the tailnet. Both gates with rules running and during a send.

**Watching** (after Alerts; owner, 2026-11-09, open call 15). [Watching failed](#alerts): the two home-side
fixed alerts, the `watching_status` tool on the shared call limit but outside the row budget, and the scheduled
routine's setup text. *Verify, Mac and Windows halves, Windows hardware owed as open call 1:* with the event socket
held down past the threshold, exactly one "Watching stopped" alert is sent and, on restore, one "Watching resumed";
a camera taken offline names only its alias; with the add-on stopped or the tunnel cut, the next scheduled run
notifies the phone that it could not reach the Mac mini; `watching_status` returns no event, picture or identity;
slice 0b's quiet-run result is recorded and the schedule set from it; every Mac PR 1, Windows PR 1 and no-regression
gate this plan already applies still holds, and Linux PR 1's once its runner is on.

**Lingering** (after Alerts; needs Door 0's `ring` result or open call 13). The second rule, shared code
only, built and verified on each platform.

> **Mac and Windows:** with a replayed socket, a person open past the dwell with no ring fires once; a ring
> at any point before the grace ends suppresses it; an event that closes before the dwell does not fire; a
> ring message lost to a socket gap suppresses the alert rather than claiming nobody rang; on the loiter path, a
> ring before the loiter `start`, within the measured offset, suppresses it. On the door, ten
> staged long waits and ten staged rings give the recorded hit and false-alarm counts. Both gates.

**Repeat car** (after Alerts and C6; needs open call 16 and line 503's plate measurement on the chosen camera).
The third rule, over C6's voted plates, plus a "mark as known" on C6's vehicle cards, which C6 as written does
not have and which never leaves the Mac mini. Shared code only, built and verified on each platform.

> **Mac and Windows:** with replayed reads, an unknown plate seen the set number of times, spaced past the
> minimum gap, fires once that local day with the right count wording, and not again at a higher count; a second
> unknown car soon after still fires; a known plate, or repeats inside the gap, does not. Marking a car known stops
> it firing that same day and survives a restart.
> A capture of every body, MCP response and log finds no plate, whole or partial. No lookup request is made.
> With sightings past the cut-off, counts drop them and a scan finds none. Staged passes on the owner's
> drive give recorded hit and false-match counts by day and night, on the camera open call 16 chose. Both gates
> with plate reading running, including the source's ring and decode during a vehicle event.

## Tools

Five tools, final before the connector is first added. All `readOnlyHint: true`,
`destructiveHint: false`, `openWorldHint: false`. Each returns a fixed-template text block and
`structuredContent` under an `outputSchema` with `additionalProperties: false`. Bad arguments get one fixed
error that reflects no input.

**The text is built, never written.** Everything Grok reads comes from validated event fields and a closed
vocabulary: enums, times, random tokens and the owner's charset-limited alias. No string from the recorder
is forwarded, and no description-model output reaches this path, even though C3 may exist at home: prose
carries sign text from a frame into an agent with a computer, and can name or describe a person. The cost is
that answers say what kind of thing was detected and when, not what it did.

**Per-question limits, all five tools on one counter.** 30 calls an hour and 200 a day (guesses). Past them every
tool answers `rate_limited` in its own status field, with the one fixed text below and every other field null or
empty.

**A budget on what is released.** `what_happened` releases at most 150 distinct event rows a day (a guess); a row
already released that day does not count again. Past the budget it still answers, with complete `counts`, no
`events[]` and everything in `more`, and `7d` is always counts only, so every feed stays queryable. `package_status`
returns only the newest package event, so the call limits bound it. Windows are enums and there is no start time,
offset or cursor. What the server bounds is the amount per day: a routine or an injected agent can still copy out up
to that budget into a conversation Grok keeps, and it cannot tell them from a question ([Security](#security)).

`what_happened(window?: "1h" | "today" | "last_night" | "24h" | "7d", camera?: alias, kinds?: kind[])`,
default `today`, every opted-in camera unless one alias is given, clamped to the cut-off. `last_night` is the
owner's overnight window. With `kinds` omitted each camera is judged on the kinds it can detect and has on, and the
answer names what is not covered; with `kinds` given, every kind named must be covered.

| Field | Values |
|---|---|
| `answer` | `events`, `nothing_seen`, `cannot_tell`, `rate_limited` |
| `window_from`, `window_to` | RFC 3339, rounded down to the minute |
| `coverage_complete` | Boolean: every camera in scope `complete` and, only when `kinds` was given, no kind asked about in `not_watched` |
| `coverage[]` | One per camera in scope: `{ camera, state, indexed_through, not_watched }` |
| `coverage[].state` | `complete`, `camera_offline`, `events_disconnected`, `gap_in_window`, `detection_off` |
| `coverage[].not_watched` | Kinds this camera cannot detect or had off for the whole window, listed with or without `kinds`. From `featureFlags` and `smartDetectSettings`; the spec has no doorbell flag, so `doorbell_ring` comes from the owner's tick (Foundation). A kind switched off during the window is `detection_off` instead |
| `counts` | Per kind, whole window |
| `events[]` | Newest first, at most 40, none for `7d` or past the row budget: `{ event_token, camera, kinds, lingered, seen_at, duration, picture, thumbnail, clip }` |
| `kinds` | `person`, `vehicle`, `package`, `animal`, `doorbell_ring`, `alarm_sound` |
| `lingered` | Boolean: from `smartDetectLoiterZone` |
| `seen_at` | RFC 3339 of `start`, rounded down to 5 minutes |
| `duration` | `under_1m`, `1_to_5m`, `over_5m`, `ongoing`, `unknown` |
| `picture` | `pending`, `saved`, `none` |
| `thumbnail`, `clip` | As in `package_status`; `none` unless a package event at the door camera |
| `more` | Events in the window not listed |
| `alerts_sent`, `alerts_failed`, `alerts_suppressed` | Counts in the window. Failed includes `uncertain`; suppressed includes `stale` |

The allow-list: `type`, `start`, `end`, `device` and `smartDetectTypes` of a smart-detect or `ring` event.
`face` maps to `person` and `licensePlate` to `vehicle`; neither word is ever returned. `alarm_sound` is
`alrmSmoke`, `alrmCmonx`, `alrmSiren`, `alrmBurglar` and `alrmGlassBreak`. Other audio types (`alrmSpeak`
among them) and plain `motion` are not stored. `nothing_seen` requires `coverage_complete`; with no event and
anything less, the answer is `cannot_tell`, and events with incomplete coverage say so. The only wordings:

- *events:* "Garden: a person around 02:10, for 1 to 5 minutes. Door: a package around 11:25. Detection can
  miss things, and cannot say who was there or why."
- *nothing_seen:* "Nothing detected on Door, Garden or Drive today. All were watched throughout; indexed up to
  14:35. Detection can miss things."
- *cannot_tell:* "I can't tell for all of this window. Garden was not being watched for part of it."
- *added when coverage is incomplete:* "Garden was not being watched for part of this window, so this list
  may be short."
- *added when `more` is above zero:* "There were 12 more, not listed."
- *instead of that, for `7d` or past the row budget:* "Counts only; individual events are not listed."
- *added when a camera does not watch for a kind:* "Garden does not watch for packages." On a camera with no bell
  `doorbell_ring` stays in the field and is left out of this sentence.
- *added when `alerts_failed` is above zero:* "2 alerts may not have been sent in this window."
- *added when `alerts_suppressed` is above zero:* "3 alerts were held back by the limits."
- *rate_limited, on every tool:* "Too many questions in a short time. Ask again later."

`package_status(window?: "today" | "24h" | "7d")`, door camera, default `today`, clamped to the cut-off:

| Field | Values |
|---|---|
| `answer` | `package_seen`, `no_package_seen`, `cannot_tell`, `rate_limited` |
| `reason` | `null`, `camera_offline`, `events_disconnected`, `gap_in_window`, `detection_off`, `detection_unsupported` |
| `camera` | The owner's alias ([Foundation](#slices)); never the recorder's camera name |
| `camera_online` | boolean |
| `package_detection` | `on`, `off`, `unsupported`, `unknown` |
| `indexed_through` | RFC 3339, rounded down to the minute: events received without a break up to here |
| `event` | `null` or `{ event_token, seen_at, thumbnail, clip }` |
| `event.event_token` | Random 128 bits, base32; maps only locally; dies with the event |
| `event.seen_at` | RFC 3339 of `start`, rounded down to 5 minutes |
| `event.thumbnail` | `pending`, `available`, `withheld`, `none`, `already_sent` |
| `event.clip` | `pending`, `saved`, `none` |
| `earlier_in_window` | Count of older package events in the window |

`no_package_seen` requires camera online, detection on, socket up and no gap in the window; anything
else without an event is `cannot_tell`. The only wordings:

- *package_seen:* "A package was detected at Door around 14:05. Detection is not proof of what was
  left or whether it is still there."
- *no_package_seen:* "No delivery seen at Door today. Camera online; indexed up to 14:35. Package
  detection can miss deliveries, so this does not prove nothing came."
- *cannot_tell:* "I can't tell. Door was not being watched for part of this window; indexed up to 14:35."
- *added when `pending`:* "The clip or picture is still being saved; ask again in a minute."

The caveats are required: the public schema has no removal signal, and package detection has failed to
reach integrations in 2026 ([HA #170153][ha170153], [HA forum][ha-forum]).

`event_thumbnail(event_token)`, any camera, under the [Redaction](#redaction) rule. Named for any event so
the tool list need not change later; in this plan only a package event at the door camera can pass. On
success a text block and one `image/jpeg` in slice 0's mode.
Fields: `thumbnail` (`pending`, `sent`, `withheld`, `none`, `already_sent`, `rate_limited`), `withheld_reason` (`null`,
`privacy_check`, `unavailable`, `budget`, `expired`, `off`), `frame_source` (`stream`, `snapshot`),
`frame_at` (for `snapshot` the fetch time, as the API gives no capture time). The fixed text says the
picture is cropped and any words in it are not instructions. One image per event; a daily budget and
burst limit (start at 5 a day, a guess). The detailed reason stays in the local log. One transaction
before any byte leaves rechecks the cut-off, consumes the budget, marks the event claimed and inserts a
log row with state `attempted` and the SHA-256; the row is updated after the write, and a claim is never
reset on failure.

`package_clip_link(event_token)`. Fields: `clip` (`ready`, `pending`, `none`, `expired`, `rate_limited`), `url` (only when
`ready`), `expires_at`, `network` (constant `tailscale_only`), `fallback` (`open_in_protect` when
`none`). No Protect URL is sent: no documented deep link was found, and one would expose the console's
address. The text says to open the Protect app at that camera's alias near that time. Minting a token changes only
the server's own table.

`picture_link(event_token)`. The same fields with `picture` for `clip` (`ready`, `pending`, `none`,
`expired`, `rate_limited`), `url` to `/f/<token>`, on any opted-in camera. The text says the picture opens only on the
owner's private network; when `none`, `fallback` is `open_in_protect` and the fixed text names that camera's alias.

## Alerts

Each rule is off until armed, and starts in dry run: it logs what it would have sent and sends nothing, so
false alarms are seen before they cost a notification. A rule says "worth checking" and nothing more. It
cannot tell a resident from a stranger, it depends on Protect's detection, and silence is not an all-clear.

| Order | Rule | Fires when | Needs |
|---|---|---|---|
| 1 | **Person overnight** | The first message of a `smartDetectZone` event with `person`, on the garden camera, with `start` inside an owner-set local time window | Person events; a zone drawn in Protect ([open call 12](#open-calls)); the window. Slice Alerts |
| 2 | **Lingering at the door** | A person on the door camera for a dwell time (start at 60 s, from Door), by a `smartDetectLoiterZone` event or a zone event still open, and no `ring` from the person's start to a grace period after the dwell, with the socket up throughout. On the loiter path the threshold is whatever is set in Protect, and the lookback reaches before the loiter `start` by at least Door's measured offset | Dwell and `ring`. The spec lists `ringEvent` on the event socket ([spec][protect]); Home Assistant's docs put ring behind a local user ([HA docs][ha-docs]). Door 0 settles it on this console; no private API either way. Slice Lingering |
| 3 | **Same unknown car** | One normalised plate, not marked known, reaching the set count (3 to 5, start at 3) in a local day, each sighting a minimum gap apart. Once per plate per local day | C6, on the camera [open call 16](#open-calls) settles. The public API has a `licensePlate` type but no plate text ([spec][protect], [HA docs][ha-docs]). Exact matches only: a misread loses a sighting instead of joining two cars. Plates stay on the Mac mini and no lookup is made, so line 552 stands. Slice Repeat car |

**Watching failed** (owner, 2026-11-09). A Mac mini that has stopped cannot report that it stopped, so two checks
cover each other, and neither is armed until the owner turns it on.

- *From home.* If the event socket stays down, or a camera covered by an armed rule stays offline, past a threshold
  (start at 5 minutes, a guess), the sender posts the fixed `rule` "Watching stopped, worth checking", with the camera's
  alias or "all cameras" and `link` null, once per outage, then "Watching resumed" when it clears. These bypass the
  rule cool-downs but share the sender's cap.
- *From Grok Bot.* A scheduled routine calls the read-only `watching_status` tool, which returns only `state` (`ok`,
  `degraded`, `rate_limited`) and `indexed_through`. If the call fails, or `state` is not `ok`, the run reports it.
  This is what catches the Mac mini, its power, the internet or the tunnel going down.

Untested: xAI's docs say a Bot notifies when it finishes, so an all-clear run may notify too. Slice 0b measures that;
if quiet runs cannot be silent, the schedule drops to once a day. The scheduled check depends on an LLM following the
routine and on Grok Bot being up, and it does not detect an alert lost after a `200`, which stays a stated limit
(Run history is the only record).

**Payload.** JSON of exactly four fields, in a capped body. The routine text from setup asks the Bot to repeat
them unchanged, open no link and use no tool; that is a request the add-on cannot enforce.

| Field | Values |
|---|---|
| `rule` | One of a fixed table: "Person overnight, worth checking"; "Someone waiting at the door without ringing, worth checking"; "Same unknown car, third time today, worth checking" (the count word is third, fourth or fifth, from the set count); "Alert limit reached, further alerts are only logged" |
| `camera` | The owner's alias |
| `at` | RFC 3339, rounded down to the minute |
| `link` | `/f/<token>` on the tailnet name, or `null` when no frame was saved by the end of the wait |

**Frame first.** The snapshot starts on the same first message as the rule, so a rule waits for that save up to a
bound set from Door (start at 5 s, a guess), then claims the row and mints the link, or sends `link` null. A frame
that lands later is not linked. An event inside the per-camera minimum interval has no frame, so `null` at once.

**Delivery and secrets.** One sender thread. HTTPS only, redirects refused. The URL and key are held together
in `secrets::os_store`, entered in Settings, shown masked, replaced by pasting, erased by `mv_addon_forget`. They
are read together once at claim and used for every attempt of that row, so a replaced key never goes to an old URL.

**Before every attempt,** the first and each retry, the sender rechecks the cut-off, feature on, camera opted in and
rule armed; any failure ends the row with nothing sent. It also checks age: an alert more than 10 minutes (a guess)
past the moment it is about (rule 1's `start`, the end of rule 2's grace, the `start` of rule 3's last sighting) is
`stale` and logged, so a late-delivered event cannot go out as if current.

**Retry.** A dropped connection, timeout, 408, 429 or 5xx retries at 5 s, 30 s and 2 min (guesses), and never
later than 5 minutes after the rule fired. Any other non-200 does not retry, except that the status slice 0b finds
for the shared-limit failure joins this list, as the docs say to send the call again. A timeout may mean a run did
start, so its state is `uncertain` and a retry can notify twice ([open call 17](#open-calls)). The link token is
minted once at claim and every retry sends a byte-identical body, so a duplicate is at least recognisably the same
alert.

**De-duplication and rate.** An event fires a rule once. A cool-down per rule and camera (30 min, a guess) stops a
second alert for the same visit; for rule 3, once per plate per local day takes its place, so one car cannot silence
another. At most 6 per rule per period and 12 in all per 24 hours (guesses), kept low because each run spends usage;
low volume is no protection from the shared limit under [slice 0b](#slice-0b-does-a-webhook-reach-the-phone). The
period is the overnight window for rule 1 and the local day for rules 2 and 3. A person who arrives after a cap or
inside a cool-down is logged, not alerted (open call 17). When a cap is first reached, one fixed "Alert limit
reached" alert goes, once per period, outside the caps, with `link` null, so a night of false detections cannot
blind the rule without the phone being told.

**Alert log.** One local row per decision: time, rule, event token hash, state (`dry_run`, `claimed`, `started`,
`uncertain`, `failed`, `suppressed`, `stale`), attempts, HTTP status, latency. No key, URL, link, alias or pixel.
`claimed` is written before the POST; at the next start a row still `claimed` becomes `uncertain`, is flagged and is
never resent. A `failed` or `uncertain` row raises a flag in Settings and the status item until the owner
acknowledges it, and as the Mac mini has no window `what_happened` says so too. A paused routine or stale key is a
non-200 and so is flagged. A `200` proves only that a run started: the add-on cannot learn whether the phone was
notified, and an alert can be lost after a `200` with nothing flagged. The phone's Run history (Failed, with the
reason) is the only place that shows it ([routines][bot-routines]).

## Redaction

**The rule, for every camera: an image goes to Grok only with nobody in it.** An event with `person` or `face`
never produces an image, and a picture of a person only ever leaves through [`picture_link`](#tools). The
pipeline below applies to any camera; step 1's motion check needs ring frames, and the only ring it reads is the
door camera's, so in this plan only door package events can pass. Another camera needs its own polygon and ring, a
new measurement and verify.

A camera has an owner-drawn polygon wholly inside the owner's boundary, stored with the channel,
quality and source dimensions it was drawn on and a reference frame from that moment. Ring and snapshot
use exactly that channel and quality. No polygon, no image; no full-frame option. A zone narrows what leaves; it does not change the status of what the camera
records ([ICO][ico], [FRA on Ryneš][fra]).

The frame check reads that camera's person and vehicle intervals from the event store. An interval with no `end` counts as overlapping. In
order, on a worker, once per event, into the add-on's own data folder:

1. **Pick a frame** from the ring, at the first of `end` + X or `start` + Y (X and Y are starting
   guesses set from Door, clamped to what the ring holds), with no person or vehicle interval within a
   margin and motion inside the polygon near zero across several seconds around it. Encode only after a
   settle delay past the frame, in which the socket stayed up; past that delay, withheld. Else the
   snapshot: `GET /v1/cameras/{id}/snapshot?channel=<snapshot_channel>`, fetched into RAM on receipt of
   every package event and discarded once a ring frame succeeds. It counts only if the event was still
   open or received within a small bound of its end (from Door), with an unbroken socket. The API gives no
   event thumbnail, past frame or capture time ([spec][protect]), so the capture time is treated as unknown
   across the fetch and the person gate covers that whole span. Its motion check needs ring frames around
   the fetch, so with no ring there is no image; Door shows whether the snapshot earns its place. Else none.
2. **Crop** to the polygon's bounding box; fill everything outside it solid. Withhold on any mismatch of
   channel or aspect ratio, or if the static scene no longer aligns with the reference frame. Before
   encoding, assert that every pixel outside the polygon equals the fill and the crop equals the bounding box.
3. **Detect** on the full-resolution crop: YuNet tiled, because `yunet_tensor` always scales to 640
   (`src/infer/preprocess.cpp:143-148`), with the 4 % floor off and confidence lowered
   (`src/infer/models.h:139-141`). OS text boxes: a plate is text, so this gate needs no plate model. A
   text-region detector independent of language (`VNDetectTextRectanglesRequest`) and a barcode detector
   (`VNDetectBarcodesRequest`). Recognition alone is not enough: with the settings of
   `text_recognition_mac.mm`, a test on 2026-11-09 read nothing from a QR holding an address (only the
   barcode request found it) and nothing from a line of Hebrew (only the region request found it).
4. **Fill** every recognised line box solid with a margin, never evenly split words
   (`text_recognition_mac.mm:87` splits unboxed lines evenly), and every barcode; parcel labels carry
   names and addresses. A text region no recognised word covers withholds the image.
5. **Downscale** to a long edge of 384-512 px and at least a fixed factor below source (starting
   guesses, set from Door).
6. **Encode** from pixels only (JFIF APP0 from `jpeg_set_defaults`, `src/codec/jpeg.cpp:491`; no other
   APP or COM). Vendor bytes are never forwarded.
7. **Detect again** on the decoded outgoing bytes.

**Fail closed.** Text is still answered and no image is produced if: no polygon, images off or the
account checks unticked; a detector or model is missing, errors or times out; any face is found
(withheld, not masked); a person or vehicle event overlaps the frame; person and vehicle are not both in
`smartDetectSettings.objectTypes` (and, if Door shows it matters, in the zone covering the polygon), since
"no person event known" is not "no person present" and no local gate looks for a body; the socket was not
continuously up from a margin before the frame to the settle delay after it; text fill exceeds a measured
share of the crop or text remains on the second pass; the frame is night IR or darker than the measured
range; the event is past the cut-off; the OS has no text, text-region or barcode detector
(`ocr_none.cpp`: no images on Linux).

Solid fill only: recognition through mosaic and blur is shown ([McPherson][mcpherson]). Passing local
detectors is a lower bound on what a cloud model can read, and YuNet's published AP gives no miss rate
on a doorstep ([YuNet][yunet]). The withheld rate is unmeasured and may be high.

## Clip capture, storage and expiry

- **Scope.** Door camera, package events (decision 14). `/f/` frames are [their own slice](#slices).
- **Stream.** `POST /v1/cameras/{id}/rtsps-stream` with `qualities: [<ring_quality>]`, a required field,
  door camera only, plus one RAM-only stream if open call 16 chooses it, from which no file is written; re-`GET` before each connect, as aliases can rotate ([uiprotect][uiprotect-docs]);
  `DELETE` on removal ([spec][protect]). URLs are credentials: memory only, dropped by the host's log
  callback (line 430). Protocol allow-list `tls,tcp`, since rtsps opens through `tls` over TCP only
  (`rtsp.c:1942-1945`) and line 438's list has no `tls`; Door confirms it needs nothing more.
- **Ring.** Undecoded video AVPackets in RAM, each with its wall-clock stamp, audio never demuxed. Start
  at 120 s, sized from Door for `ring_quality`: at an assumed 4 Mbit/s, 60 MB. Its thread never touches UI
  or render. Off when the feature is off.
- **Trigger.** `smartDetectZone`, `smartDetectLine` or `smartDetectLoiterZone` with `package` in a
  non-null `smartDetectTypes`, on `add` or `update`, once per id. An ended `update` for an unknown id
  counts ([uiprotect #999][uiprotect-999]).
- **Window.** From `start` minus the measured pre-roll, about a minute, in wall-clock time through Door's
  offset, clamped to what `stream_status` says the ring holds. If it holds nothing near the event, no clip.
  Until the window has closed and the file is written, `clip` is `pending`. A clip may still begin after
  the courier left.
- **File.** Stream copy to MP4, moov first, `.partial` then rename, random name, in the add-on's data
  folder, excluded from backup whatever line 305's switch says. A disk cap, oldest first.
- **Expiry.** Enforced twice (line 294). The sweep at start and hourly is directory-driven: it deletes the
  clip, any unsent egress JPEG and the row, and any `.partial` or file with no row. Every serve rechecks.
  The egress JPEG is deleted once sent (one image per event, so it is never re-sent), at the cut-off, and
  when images are turned off. The API has no retention field ([spec][protect]), so on UniFi "the recorder's
  cut-off" is the number of days the owner enters (line 296). Line 296's check against reality needs a
  result opened from the recorder, which this path never does, and Protect keeps by capacity or a set time
  (line 299): if it drops footage sooner, MediaViewer cannot tell (open call 4).
- **Serving.** `GET /c/<token>` and `/f/<token>`, 256-bit random, expiry 10 minutes (a guess), an alert's link 24 hours (a guess, so it still opens in the morning), never past the cut-off.
  Reusable until expiry, because iOS fetches in ranges ([Apple][apple-range]). `Cache-Control: no-store`.
  Tokens live in memory with a generation that sign-out, feature off and camera opt-out all bump. An alert link's
  hash, expiry and generation persist in their own table, so it survives a restart and can still be revoked; the raw
  token is never written down and the row goes at expiry.
- **D5 and rule 5.** Line 463 set these aside only because nothing was recorded; decision 5 removes that
  premise. Neither conflict returns. The stream is private input to the add-on, never openable in the
  viewer, and a saved clip is a stream-copied MP4 of a D5 codec (Door confirms which). A codec outside the
  D5 set saves no clip and the answer is Open in Protect. The add-on deletes only its own files; it touches
  no original and no footage on the recorder.

## Sign-in and tunnel

**Grok Bot to the add-on.** Bot connectors are reported to be OAuth only (Cursor staff, relayed by
[wiki][wiki]); xAI says an OAuth or API-key flow completes in Grok ([tunnelling][grok-tunnel]). Slice 0
confirms. The add-on is a small authorization and resource server per [MCP authorization][mcp-auth]:
Protected Resource Metadata; 401 with `resource_metadata` on every unauthenticated call, `initialize`
included; `S256`; exact redirect URIs; tokens only for this resource, never in a query string; short
access tokens, rotating refresh tokens, stored hashed. No account: the authorize page asks for a
one-time code shown in Settings → Cameras, valid five minutes, locked after a few tries. Signing out is
one button. `Origin` is validated ([MCP transports][mcp-transports]). Secrets use
`secrets::os_store(service)` (`src/addons/driftwire/secrets.h:56`), erased by `mv_addon_forget`.

*Registration.* After slice 0 this file records the one client and exact redirect URI, and the server
supports only that: pre-registered, or a `/register` that accepts only that URI. If Client ID Metadata is
required, the add-on fetches only an allow-listed Cursor or xAI host, with no redirects and no private
addresses; the spec warns that this flow fetches a URL from an unknown client. `/authorize` answers 404
unless pairing was armed in Settings; it disarms after one grant, allows one active grant and shows the
redirect host. Lockout applies to the armed code only, so it is not a public denial of service.

**To the recorder.** The local server only, never the cloud connector, with an API key the owner enters,
of the least privilege Door found to work. Each certificate is shown once and pinned, and a change is
refused (line 428): the HTTPS one, and the RTSPS port's own, captured with `stream_peer_cert` rather
than assumed to match.

**Tunnel, owner-run.** xAI rejects private addresses and documents ngrok and Cloudflare, not Funnel
([tunnelling][grok-tunnel]). The add-on never installs, starts or scripts Tailscale; Settings shows what
to run: `tailscale funnel` for port A, `tailscale serve` for port B on another HTTPS port. Funnel needs
MagicDNS, HTTPS and the `funnel` attribute ([Funnel][funnel]). Precedent: a program the app does not
start ([12](../design/12-decision-log.md) lines 2429-2434).

**Tailnet-only pictures.** Serve adds `Tailscale-User-Login` and strips client copies; Funnel traffic has no
identity headers; the backend should listen only on localhost ([Serve][serve]). The picture listener
requires that header to equal the login the owner entered, so a Funnel misconfiguration gets 403. Tagged
devices get no header, so the phone must be user-owned. The hostname stays in the conversation and in
the public certificate ledger ([HTTPS][ts-https]), so the machine name must say nothing about the owner.
The header names a login, not a device: every device on that login passes, including one where a Bot can
run commands, so a clip or saved frame could still pass through Grok (decisions 3 and 10). Owner checks, shown in Settings:
local-computer commands set to Never allow on the Bot account ([computer and apps][bot-comp]), no Bot
desktop on a device signed in to the owner's tailnet, and an owner-run tailnet grant so only the phone can
reach the Mac's port B.

## Retention promise

For the sheet and [13](../design/13-updates-and-telemetry.md), replacing "an expired event is never
shown" on this path only:

> Nothing older than the number of days you entered is sent. If your recorder runs out of space sooner,
> MediaViewer cannot tell. MediaViewer's own index and plate sightings follow your recorder: on
> UniFi, the number of days you entered. Saved pictures and clips go by that number or after 7 days,
> whichever comes first. A log on this computer, with no pictures in it, records what
> left and when. A picture or alert already sent cannot be recalled. Grok Bot keeps conversations until you
> delete the Bot or your account, and MediaViewer has no way to delete one image there.

Basis: conversations stay until the Bot or account is deleted ([Cursor privacy][cursor-priv] §7); no
per-message delete is documented; Bot deletion leaves backend retention to Cursor's terms and computer
files can remain ([Bots][bot-bots], [FAQ][bot-faq]). Never write "not stored" ([approvals][bot-appr]), nor
"not used for training" on an individual plan ([Cursor help][cursor-help]).

The answer log: time sent, kind, recipient, reason ([ICO guidance][ico-gov]), event token hash, byte count,
SHA-256, gate result. Fixed ASCII; rule 6 applies.

## Decision-log row

To land in [12](../design/12-decision-log.md) under a new `## Cameras` heading, before the Answer slice
sends anything, in the form of its lines 2026-2031 and 2142-2151:

> **The Cameras add-on may answer the owner's own agent and alert through it (owner, 2026-11-09, #96;
> narrows rule 6 and "an expired event is never shown" on this path; amends issue 96 lines 21, 29, 35, 140,
> 149, 292, 296, 422, 431, 438, 460, 463, 543, 544, 545, 547, 548, 550 and 559).** Off by default, per
> camera. For each tool call the add-on may return allow-listed text about events on any opted-in camera, and
> at most one redacted thumbnail with nobody in it, to an agent the owner connected; recipients are Cursor,
> SpaceXSI and Cursor's sub-processors. Armed rules may also POST, unasked, a fixed alert of rule name, camera
> alias, time and private link to the owner's Grok Bot routine. Text is built from validated fields and a
> closed vocabulary. Pictures of people, saved frames and clips stay home behind links that open only on the
> owner's Tailscale network. UniFi package events at the door only: about a minute of video without audio
> saved from a RAM buffer, deleted when the recorder's footage expires. Indexing stays local. Faces, plates,
> speech and descriptions are never returned or alerted. Rule 6 is unchanged for logs, telemetry and crash
> reports. Nothing older than the entered retention is sent, and the add-on cannot tell if the recorder ran out of space sooner; the home index and plate sightings follow the recorder; images, saved frames and clips have a hard ceiling (7 days to start); pixel-free local logs record
> what left; a sent image or alert cannot be recalled. No MediaViewer server; the tunnel is the owner's and
> the app never starts it. TLS is added to the bundled FFmpeg. Other makers' add-ons still have no network
> permission. Residual risk: the public HTTP, JSON and OAuth code runs in the viewer's process, so a
> memory-safety bug on that path reaches the recorder key, the webhook key and the photo library.

A base Cameras row (scope, search only, the 2026-09-29 calls; lines 25, 452) must land before or with
this row.

| Text amended | Today | After |
|---|---|---|
| Rule 6, `CLAUDE.md:80-81`; [13](../design/13-updates-and-telemetry.md):7-8; [15](../design/15-platforms.md):178-179 | Nothing about a user's files leaves; 13 names thumbnails | One named, opted-in exception, read broadly as line 403 and [27](../design/27-places.md):25-30 do. Not a D1-D9 reversal |
| Lines 21, 140, 543; 149 | Stores no video; ring never written | UniFi package clips only, per decision 5 |
| Lines 29, 559 | Search only; no recording | Still no NVR replacement; UniFi package clips only are recorded (decision 5), one frame is kept per person or vehicle event (decision 14), and rules run over events |
| Line 545 | No real-time alerts or notifications | Alerts through Grok Bot, by armed rule only (decisions 12, 13) |
| Lines 35, 292 | An expired event is never shown | Holds at home; "never sent" on this path |
| Line 292, first clause | Nothing derived from a moment outlives its footage | Holds for all the add-on keeps except the pixel-free log, which may outlive the event (open call 5) |
| Line 296 | The entered number is checked against reality and lowered | No such check on this path, which opens nothing from the recorder (open call 4) |
| Line 438 | Allow-list `rtsp,rtp,rtcp,tcp,udp` | `tls,tcp` for this path's rtsps stream |
| Line 463 | Rule 5 and D5 conflicts do not arise, as nothing is recorded | Clips are recorded; neither conflict returns, for the reasons under [Clip capture](#clip-capture-storage-and-expiry) |
| Line 544 | No camera or recorder configuration | One exception: creating and removing the door camera's RTSPS stream and, only if open call 16 chooses it, one RAM-only stream on the camera that sees the road |
| Line 427 | No OpenSSL is added | Stands if open call 9 takes the overlay port; amended only if the OpenSSL feature is chosen |
| Lines 422, 547, 550 | No access from outside the LAN; no search from another machine | Grok's per-question tools and the owner's Tailscale links only. Searching the index from another machine stays out: these tools take no query text |
| Line 548 | No cloud inference, incl. a frame | Indexing and captioning still banned; a thumbnail with nobody in it may go to Grok on a tool call |
| Lines 431, 460 | Two third-party requests, both started by the user; plates first to leave | Two more kinds: tool answers, and alerts the owner armed but no click starts. Plates still do not leave on either |

Lines 551-553 still hold: no owner lookup, no watch-list sharing, and no vector, thumbnail, frame, face, caption or plate sighting outlives the cut-off; the pixel-free logs are the exception under open call 5. The plate-lookup exception (line 569) stays open and
excludes images (line 406), so this row does not settle it. The issue text is the owner's to edit.

## Privacy and README changes

**On landing:** a new Cameras doc in `docs/design/`, with a row in CLAUDE.md's doc table; `CLAUDE.md:80-81`
gains the clause "except the opted-in Cameras answer and alert path (12)"; a subsection in 13 beside Driftwire's,
with lines 7-8 saying the same; 15:178-179 likewise; `docs/DEVELOPMENT.md` lines 2215 and 4565 scoped to
match; 14's rows; [11](../design/11-licensing.md)'s FFmpeg paragraph (lines 29-36) and any build check on
the configure line, for the TLS flags. `packaging/macos/Info.plist.in:70` says "Driftwire… Nothing goes
to the internet" and needs wording that covers the recorder: a base-bundle change (open call 6).

**README**, in the change that makes the add-on installable by others, not before. Proposed:

- Lines 9-10: "…and nothing about your files leaves your computer unless you turn on an add-on that
  says it does."
- Line 25: "No account, no upload. Telemetry is off unless you turn it on. Everything is local unless
  you connect an AI agent to your cameras."
- Line 286: "**Nothing about your files leaves the machine,** except what you choose to send: Driftwire
  to a person, Cameras' answers and alerts to your AI agent."

## Security

- **Pixels are untrusted input to an agent with a computer.** A visitor can hold up text. Typographic
  attacks work on other models ([AgentTypo][agenttypo]); no figures exist for Grok on doorstep scenes.
  Controls: zone, solid-filled text, downscale, and an answer built from the event row, never from what
  the model says the picture shows. Inference hooks are Enterprise and see image metadata only
  ([hooks][bot-hooks]).
- **Webhook.** It leaves unasked, so the body is limited as under [Alerts](#alerts). A routine has the Bot's
  connected tools; the docs do not say how to restrict them or what an unattended approval does
  ([routines][bot-routines]). Setup asks for approval on every write action. The add-on cannot see that. The
  limits above bind only what the add-on sends: the key is the routine's only control, the docs describe no
  signature or source restriction, and anyone holding it can start an unattended run with any body ([open call
  18](#open-calls)).
- **Aliases** reach Grok, so setup says to name places, never people. Only the owner can set one; the charset limits
  length and symbols, not meaning, and cannot enforce that.
- **Blast radius.** A separate Bot does not contain an injection: apps, files, browser sessions and
  credentials are account-wide ([computer and apps][bot-comp], [terms][cursor-terms]). Setup asks for a
  dedicated account with no mail or drive, **Ask first** on the thumbnail and link tools, and the checks
  under [Tailnet-only pictures](#sign-in-and-tunnel). The server cannot see or enforce that. `what_happened` and
  `package_status` are left off Ask first: they are the ordinary question, they return capped text with no identities,
  and the row budget bounds them. The owner can turn it on.
- **Server limits.** It cannot tell a question from a routine or always-allow rule ([routines][bot-routines]);
  budget, tokens and allow-list are what it enforces. Never claim "only when you ask".
- **Exposure.** Public URL: unknown paths 404; bodies, headers and connections capped; nothing is parsed
  beyond the request line, a size-capped header block and the bearer check before auth. HTTP, JSON, OAuth
  forms and Protect frames get fuzz harnesses (line 440). D8: the demuxer always, decoders on an event,
  in the app's process, which also holds the recorder key, Keychain access and the photo library: "read-only"
  describes the MCP tools, not a process boundary. A default Bot cloud computer cannot reach the tailnet
  ([private networks][bot-priv]), but the login check does not separate the owner from the owner's own agent.
- **Never reachable or logged.** The MCP and webhook code opens no faces, plates, transcripts, descriptions,
  photo index or path; the Repeat car rule alone reads plates and passes out only a count. No event id, alias, token, URL or pixel in logs or crash reports. Heap is not captured, so
  minidumps omit the ring and the egress JPEG, but `src/shell/minidump_scrub.h` leaves a `://` URL on stacks:
  with Cameras loaded it also masks `rtsps://` and any URL path. Canary API key, stream URL, link token,
  bearer token and webhook key join `tools/mac/crash_canary.py` and `tools/minidump-scan.ps1`.

## Before release to others

1. Legal review including footage going to a cloud AI and automated alerts about passers-by (line 572
   extended): tracking a neighbour's car by plate first of all, the domestic exemption beyond
   the boundary ([ICO][ico], [FRA][fra]), disclosure and possible restricted transfer to a US service
   ([ICO transfers][ico-transfer]; no DPA on individual plans, [Cursor help][cursor-help]), private
   claims (Fairhurst v Woodard, reported at [CRS][crs]), signage. Not legal advice.
2. Off by default; per-camera opt-in, no pre-ticked box; a sheet naming every recipient.
3. Per-install sign-in, reviewed. A tunnel answer that does not break "no second app" (12:1360). Whether
   the public parser needs isolating from the recorder key and the library.
4. The Windows half, both verify lines, both gates. Miss rates on more than one camera.
5. Cursor's sub-processors, DPA and Grok Bot Terms read; both companies' DPF status checked.
6. Indoor cameras. Setup asks of every camera whether it is indoors; an indoor camera is off this path entirely
   (no answers, frames, links or alerts) unless a separate, informed opt-in passes the legal review. Nothing
   in this plan was built or tested for one.
7. Rules off by default, Repeat car hardest to reach, with wording that names the people recorded. Notification
   behaviour restated from what slice 0b found, not assumed.

## Open calls

**Status (owner, 2026-11-09).** Recommendations accepted for calls 1-6, 9, 10, 12, 14 and 16-18, including
accepting in words the leaked-key risk of call 18. The call 4 ceiling starts at 7 days, and the promise and row
above carry its wording. Still open: 7 (numbers, set from measurements), 8 (only if slice 0 shows no inline
display), 11 (the owner's account checks) and 13 (only if `ring` is not on the official socket). Call 15: alert when
watching fails, yes (slice Watching); an alert lost after a `200` is a stated limit. The reasoning below is kept.

1. **D9 against decision 7.** D9: done only when both platforms hold. Compatible if the Mac half runs
   ahead on a draft branch and nothing merges alone; merging Mac-only needs a dated D9 amendment.
   The verify lines also need a phone, a recorder and Tailscale on Windows, and no Windows test machine is
   named; native Windows has been CI-only here. Name a machine, or split each Windows verify into what a
   replayed socket and a synthetic RTSPS server prove in CI and what is owed on real hardware.
   **Recommend: draft, nothing merged alone; split, with the hardware part owed before the slice counts as done.**
2. **Does #96's spike and plan doc gate this connector?** Lines 25 and 492-510 say so, and this plan now goes well
   past the package path. Either the full spike comes first, or Door plus this file replace them, with Foundation
   cut as the C1 and C2 halves above. **Recommend: Door plus this file for the all-camera answers, Frames, the package
   path and rules 1 and 2. Plates are not waived:** line 503's measurement, per camera by day and night, runs before
   Repeat car, and C6 keeps the spike's targets (line 534). The full spike still gates the rest of #96. The owner
   confirms in the decision-log row, and the base Cameras row lands before or with it.
3. **How the models reach the add-on.** Answer, Frames, Clip, Alerts and Lingering need none. Thumbnail needs YuNet;
   Repeat car needs C6's plate detector and OCR model (line 193, licences "to check per model"). Download each from
   its publisher with a hash pin, a minimal C1 catalogue, or ship them in the add-on package, which would amend
   lines 32 and 41. **Recommend: download, with the owner accepting each licence at download**, as the 2026-09-29
   calls have it.
4. **Decision 4's wording against capacity retention.** The promise above stays as decided. On UniFi the
   cut-off is only a typed number, and if Protect runs out of space sooner, footage that has gone can still
   be described, sent or served. Decision 5's "deleted when the recorder's footage expires" has the same
   limit. **Recommend:** the owner rewords it to "Nothing older than the number of days you entered is sent.
   If your recorder runs out of space sooner, MediaViewer cannot tell", mirrors that in the row, and sets a
   hard ceiling on images, frames and clips well below the entered number, for example 7 days.
5. **How long the local log lasts.** It outlives the event by design. **Recommend: until cleared.**
6. **Base-bundle changes with the add-on absent** (lines 458, 574, open since #96): the Info.plist string,
   host-table rows, FFmpeg TLS in every install, windowless or tray start. Accept each or say which not.
   **Recommend: accept**, as the clip path cannot exist without them; the first three add no running code.
7. **Numbers:** thumbnail budget, call limits, the daily row budget, link expiries, disk caps, downscale, ring
   length, X, Y, the settle delay, and every rule threshold, frame wait, maximum alert age, retry, cool-down and cap.
8. **If slice 0 shows no inline display:** text only stands by decision 6. Add a tailnet still, or overrule
   decision 6, only by the owner's say-so.
9. **FFmpeg TLS on the Mac:** SecureTransport overlay port (OS library, line 429) or the OpenSSL
   feature. Either changes the pinned configure line. **Recommend: overlay port.**
10. **The certificate pin.** Line 428 forbids ignoring the certificate, and FFmpeg's Schannel backend cannot
    pin. **Recommend:** a PEM file on the Mac, a small Schannel patch in the overlay port on Windows. The
    alternative on Windows is the port's OpenSSL feature with `ca_file`, which would amend line 427.
11. **Account and plan.** Privacy Mode and a dedicated account are owner checks before any real thumbnail.
    Before any real clip or frame link: local-computer commands set to Never allow, no Bot desktop on a device signed
    in to the tailnet (the webhook setup machine included), and the tailnet grant limiting port B to the phone.

12. **"The garden" is one zone in Protect.** The public event has no zone id or box and the camera object lists
    no zones ([spec][protect]), so the add-on knows only the camera. **Recommend:** the owner keeps person
    detection on that camera to one zone drawn on the garden. The alternative, a polygon in the add-on plus a
    local person detector, adds a model and frames to the rule.
13. **If `ring` does not reach the official socket.** The rule cannot honestly say "without ringing", and the
    private API stays banned. **Recommend:** the owner chooses between dropping the rule and rewording it to
    "someone waiting at the door" with no claim about the bell.
14. **If the desktop diverts alerts.** Decision 12 says phone; the docs say owned Bots' finished replies go to a
    desktop in use. The webhook's URL and key are shown only on desktop, so a desktop must be signed in at least once.
    **Recommend:** the dedicated account of open call 11, with the desktop signed in once for setup on a machine off
    the tailnet, then signed out and the computer removed, so alerts reach the phone (untested); replacing the key
    repeats the step. If the owner wants the desktop app kept on that account, alerts are unreliable on the phone and
    the owner should accept that in words.
15. **Alert when watching fails.** Decision 13 names three rules; none says the cameras have stopped watching,
    and "monitors" (decision 9) arguably implies it. `what_happened` shows gaps only when asked. An alert can also be
    lost after a `200`, the routines' shared rate limit being one documented cause, with nothing flagged at home; only
    the phone's Run history shows it. **Recommend:** the owner decides whether that is acceptable for a monitoring
    feature; nothing is built for it here.
16. **Where Repeat car gets its pixels.** C6 reads several main-stream frames of each vehicle (lines 374-376). The
    public API has no recorded footage, plate text or event image, the one snapshot at event start may show an empty
    road, and this plan's only stream is the door camera's. (a) A RAM-only `rtsps-stream` on the camera that sees the
    road, decoded for vehicle events only, nothing saved, so decision 14 stands; it widens the line 544 exception and
    adds ring and decode cost to Repeat car's gates. (b) C6 on single snapshots with a measured read rate, which
    changes C6's verify and will rarely give the exact matches the rule needs. (c) Only what the door camera sees.
    **Recommend: (a), or (c) if Door 0 shows the door camera sees the road.** Repeat car is blocked until this is
    settled.
17. **Duplicates against loss, and what the limits hide.** Two policies, not numbers: a retry after a timeout can
    notify twice, accepted over losing an alert; and a person who arrives inside a cool-down or after a cap is logged
    but not alerted, with only one cap notice. **Recommend: keep both**, with this plan's fixed-body retry and
    cap notice.
18. **A leaked webhook key.** Anyone holding the key can start an unattended run with any body, and a run has the
    account's connected tools, the MCP connector included. Either the routine lives on a second account with no
    connector and no apps, or the owner accepts in words that a leaked key reaches the connector: text with no
    identities, within the budget, and links that open only on the tailnet. **Recommend: accept in words**, as the
    key lives only in the Mac mini's secret store.

## Unverified

- The door camera: model, package detection, codec, bitrate, GOP, whether it frames inside the boundary.
- How long after a delivery the event fires; no source measures it, and events may arrive already closed
  ([uiprotect #999][uiprotect-999], [HA docs][ha-docs]). The ring may roll past it.
- Which UniFi cameras detect packages (no official list found). That `smartDetectSettings.objectTypes`
  means "enabled" is inference. Whether an `update` item is partial.
- The stream URL's lifetime; whether `?enableSrtp` must be stripped (one anecdote); whether FFmpeg 9.0.1
  connects over RTSPS with SecureTransport or Schannel. Whether a self-signed leaf as `ca_file` validates
  on SecureTransport; the Schannel patch is unwritten. Snapshot freshness and size.
- Whether a view-only key reaches events, snapshot and `rtsps-stream`. Whether a stationary person passes
  the motion gate, and how often the snapshot is usable once it applies.
- Everything in slice 0b: webhook to phone, latency, desktop diversion, whether the Bot repeats a payload
  unchanged, how a routine behaves when an approval is pending, routine body-size limits, any plan
  requirement, and how soon a routine pauses during absence. Which status a rate-limited webhook returns and whether
  the shared-limit failure can come after a `200`; what the weekly usage limit does. How a key is replaced. Whether
  the web app shows the Webhook section (the docs say only desktop), and whether a routine and its webhook keep
  working once the setup computer is removed.
- Whether `ring` and `smartDetectLoiterZone` arrive on this console and camera; the loiter `start` offset and
  when its `end` comes; whether person events arrive
  open; night-time person detection in the garden; how often a snapshot after `add` still shows the person.
- False-alarm rates of all three rules. Which camera sees the road. C6's read rate on the owner's cameras.
- Everything in slice 0. The x.ai changelog and consumer terms were unreadable (403).
- Whether Funnel works as a Grok connector path, and whether Funnel and Serve coexist as drawn.
- How long Cursor's backend keeps a tool-result image, whether it reaches the Bot's disk.
- Cursor's sub-processor list and DPF status (neither loaded); training on individual plans.
- Miss rates of YuNet and OS text detection on this door. `Windows.Media.Ocr` word boxes here. Injection
  rates against Grok.

## Sources

Protect spec v7.3.74 (its `info.version` reads "0.0.0") parsed 2026-11-09; other pages read through a
summarising fetch.

[protect]: https://developer.ui.com/protect/v7.3.74/openapi.json
[uiprotect-999]: https://github.com/uilibs/uiprotect/pull/999
[uiprotect-docs]: https://uiprotect.readthedocs.io/en/latest/usage/
[ha-docs]: https://www.home-assistant.io/integrations/unifiprotect/
[ha170153]: https://github.com/home-assistant/core/issues/170153
[ha-forum]: https://community.home-assistant.io/t/unifi-package-detected-sensors-disappeared-with-2026-7-0/1015785
[mcp-tools]: https://modelcontextprotocol.io/specification/2025-11-25/server/tools
[mcp-auth]: https://modelcontextprotocol.io/specification/2025-11-25/basic/authorization
[mcp-transports]: https://modelcontextprotocol.io/specification/2025-11-25/basic/transports
[grok-conn]: https://docs.x.ai/grok/connectors
[grok-tunnel]: https://docs.x.ai/grok/connectors/custom-mcp-tunneling
[bot-files]: https://docs.x.ai/grok-bot/files-and-results
[bot-sec]: https://docs.x.ai/grok-bot/security
[bot-faq]: https://docs.x.ai/grok-bot/faq
[bot-bots]: https://docs.x.ai/grok-bot/bots
[bot-appr]: https://docs.x.ai/grok-bot/approvals-security-and-privacy
[bot-comp]: https://docs.x.ai/grok-bot/computer-and-apps
[bot-priv]: https://docs.x.ai/grok-bot/private-networks
[bot-hooks]: https://docs.x.ai/grok-bot/inference-hooks
[bot-routines]: https://docs.x.ai/grok-bot/skills-routines-and-automations
[bot-mobile]: https://docs.x.ai/grok-bot/mobile
[bot-notify]: https://docs.x.ai/grok-bot/settings-and-notifications
[cursor-priv]: https://cursor.com/privacy
[cursor-help]: https://cursor.com/help/security-and-privacy/privacy
[cursor-terms]: https://cursor.com/en-US/terms/grok-bot
[cursor-bug]: https://forum.cursor.com/t/regression-mcp-tool-image-content-no-longer-renders-in-agent-chat-model-receives-it-ui-does-not/168824
[gradually]: https://www.gradually.ai/en/changelogs/grok-bot/
[wiki]: https://www.grokbotwiki.com/guides/grok-bot-plugins-mcp
[gh1694]: https://github.com/xai-org/plugin-marketplace/issues/1694
[gh1402]: https://github.com/xai-org/plugin-marketplace/issues/1402
[funnel]: https://tailscale.com/kb/1223/funnel
[serve]: https://tailscale.com/kb/1312/serve
[ts-https]: https://tailscale.com/kb/1153/enabling-https
[apple-range]: https://developer.apple.com/library/archive/documentation/AppleApplications/Reference/SafariWebContent/CreatingVideoforSafarioniPhone/CreatingVideoforSafarioniPhone.html
[yunet]: https://github.com/opencv/opencv_zoo/blob/main/models/face_detection_yunet/README.md
[mcpherson]: https://arxiv.org/abs/1609.00408
[agenttypo]: https://arxiv.org/html/2510.04257v1
[ico]: https://ico.org.uk/for-the-public/domestic-cctv-systems/
[ico-gov]: https://ico.org.uk/for-organisations/uk-gdpr-guidance-and-resources/cctv-and-video-surveillance/guidance-on-video-surveillance-including-cctv/governance-post-deployment/
[ico-transfer]: https://ico.org.uk/for-organisations/uk-gdpr-guidance-and-resources/international-transfers/international-transfers-a-guide/
[fra]: https://fra.europa.eu/en/node/17583
[crs]: https://www.charlesrussellspeechlys.com/en/news-and-insights/insights/commercial/2021/fairhurst-v-woodard-property-audio-and-video-surveillance-system-breached-gdpr/
