# M1 spikes: numbers from a real iPhone and a real Synology

<!-- Copy to docs/perf/mobile-m1-spikes.md and fill in. M1's verify line asks for: the numbers from
     the oldest and newest supported iPhone, the NAT64 result and the supported Synology list.
     §13 of docs/plans/mobile.md is then rewritten from this report. Delete these comments.
     `python3 tools/spikes/mobile/summarise.py results.jsonl` prints the S1, S3 and S4 tables.
     Keep each results.jsonl next to the report: quote the JSON, not impressions.
     A number from a simulator does not belong here. This goes in the repository: keep the NAS's
     public name, addresses and tokens out of it, in the prose and in any pasted output. -->

Date: · Commit: · Who ran it:

## What ends the project (`mobile.md` §16)

| Question | Line | Result | Verdict |
|---|---|---|---|
| Text tower on the **oldest** device | Encode over 500 ms, or jetsam | | |
| The link through the forward | Thumbnails over about 1 s, or frequent stalls | | |
| The Synology can run the service | Unsupported CPU, or DSM blocks it | | |
| The Synology's building has a public IP | The forward never answers | | |

## Devices

| | Oldest supported | Newest |
|---|---|---|
| Model (`device`) | | |
| iOS | | |
| RAM | | |
| Battery state, Low Power off | | |

**Only one iPhone?** Say so here: M1's verify line wants both, so it is not met.

## S1 text tower

ONNX Runtime 1.30.0 on the CPU, `clip-l14/text.onnx`, 247,784,246 bytes.

Paste `summarise.py`'s S1 table for each device. Then:

| Run | Oldest | Newest |
|---|---|---|
| Cold after a reboot: `did_finish_to_first_encode_ms`, median of 3 | | |
| Cold after a force quit: same, median of 5 | | |
| Runs with `prewarmed` true, of those | | |
| Process start → first encode, runs not prewarmed only (ms) | | |
| Load (ms) | | |
| Encode p50 / p95, performance cores (ms) | | |
| Encode p50 / p95, one thread (ms) | | |
| Footprint after load (MB) | | |
| Footprint peak during load (MB) | | |
| Footprint after encodes (MB) | | |
| `available_mb_after_load`, read right after the session opens | | |
| Killed during load or encode? (`killed` rows) | | |

## S3 scan

Paste `summarise.py`'s S3 table for each device. Then:

| | Oldest | Newest |
|---|---|---|
| Has `FEAT_DotProd` | | |
| 100k, the desktop's loop, one thread, p50 (ms) | | |
| 100k, best kernel, best thread count, p50 (ms) | | |
| 1M mapped, best kernel, first pass / p50 (ms) | | |
| 1M mapped, `mb_after_load` / `mb_after_scan` / peak (MB) | | |
| 1M heap: survived? Footprint (MB) | | |
| After a reboot, 1M mapped, `pass_first_ms` of the row `desktop_copy`, 1 thread (ms): the true cold read, since that row runs first and later rows find the pages cached | | |

Budget in §13: submit → results at 100k frames in 150 ms or less, with encode, a query's two scan
passes, gates and rank all inside it. A pass here is scan only.

## S4 AVPlayer over a resource loader

Clip used (codec, resolution, bitrate, size):

Paste `summarise.py`'s S4 table. Then:

| Path | First frame (ms) | Seek → playing p50 / max (ms) | Stalls | Seeks that timed out | 60 s no-seek: played / wall (s) | Waiting (ms, events) |
|---|---|---|---|---|---|---|
| File in Documents | | | | | | |
| Range server, Wi-Fi at home | | | | | | |
| Range server, through the forward, mobile data | | | | | | |
| Range server, through the forward, other Wi-Fi | | | | | | |

A run with `sustain_limited_by_clip` true did not play for 60 s: use a longer clip. A link that cannot sustain the clip's bitrate (`clip_mbit`) shows as waiting time.

## S2 the link through a real port forward

Forwarded ports: · Router: · Upload of the NAS's line (Mbit/s):

`range_server.py probe`, which is the **path only**, plain http, no Driftwire crypto:

| From | connect ms | 64 KB range p50 / p95 / max (ms) | Bulk Mbit/s | IPv4, IPv6 or NAT64 |
|---|---|---|---|---|
| Other Wi-Fi | | | | |
| Mobile data (Mac on the phone's hotspot) | | | | |

`driftwire` through the relay on the NAS:

| From | Bearer it showed | File size | Time | MB/s |
|---|---|---|---|---|
| Other Wi-Fi | | | | |
| Mobile data | | | | |

If the bearer was not the relay, the router mapped a port by itself or a public IPv6 address
reached the NAS directly, and this did not use the forward. Say so.

| Question | Result |
|---|---|
| Did the receiver register at the relay from inside the network (router hairpin)? | |
| Reconnect through the forward | **Not testable today**: a relayed send does not move (BLOCKERS.md). Owed to M2b. |
| Range reads over the real link | **Not testable today**: `driftwire` has no range read. Owed to M2b. |

## NAT64

| Test | Result |
|---|---|
| `dns64` from the socket probe: is this really a NAT64 network? | |
| Socket probe by **name** (`getaddrinfo`): ok, family answered | |
| Socket probe, IPv4 literal through `getaddrinfo`: ok, family answered | |
| Socket probe, IPv4 literal on a raw `AF_INET` socket: ok, error | |
| S4 range server through AVPlayer, by name | |
| Driftwire's own code connecting by name on NAT64 | Not tested: the socket probe follows its `resolve`, but is not its code |

`getaddrinfo` and URLSession can both turn an IPv4 literal into a NAT64 address, so a literal working
there says little. The raw row is the one a stored IPv4 address would take.

## S5 the Synology

Paste `probe-synology.sh`'s output for each model.

| Model | CPU, `uname -m` | DSM | glibc | Page size | `driftwire` runs? | Relay runs? | Container Manager? | Supported? |
|---|---|---|---|---|---|---|---|---|
| | | | | | | | | |

| Question | Result |
|---|---|
| Which build ran (CI package, `build-linux.sh`, which base image) | |
| Newest glibc symbol the program needs, and what DSM has | The CI x64 `driftwire` at `1facf73a` needs GLIBC_2.35 |
| Starts at boot through Task Scheduler, as the account named? Survives a reboot? | |
| Container Manager: on this model? `driftwire --version` in `ubuntu:22.04`? Relay restarts after a reboot? | |
| A DSM package (`.spk`) | **Not covered by this kit.** Deferred: the §7.2 question stays open for that format |
| Files still there after a DSM update? | |
| Replacing the binary while it runs, then restarting: what happened | |
| How an update should work, from the above | |
| Models not tested, and why | |

## What these numbers do not say

<!-- Keep and extend. -->

- S1 tokens are pre-computed, so tokenizing is not in the encode time.
- S3 is dot products over synthetic rows, with no gates or ranking.
- S2's range numbers are the path through the forward, not the Driftwire link.
- Mac vs phone parity of embeddings is not checked: that is M5.
- A DSM package was not tried, so S5 answers for a bare program and a container only.
