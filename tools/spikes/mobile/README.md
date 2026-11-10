# M1 spikes: will the iPhone companion work at all?

Throwaway code for M1 of `docs/plans/mobile.md`. Nothing here ships, and nothing here links `src/`.
You need an iPhone and the Synology; this kit gets you numbers from them.

**Where to write them:** copy [RESULTS-TEMPLATE.md](RESULTS-TEMPLATE.md) to
`docs/perf/mobile-m1-spikes.md`. [BLOCKERS.md](BLOCKERS.md) lists what the real port has to fix.

| Spike | Question | Runs on | Section |
|---|---|---|---|
| S1 | CLIP text tower: load, encode, memory, cold start | iPhone, oldest and newest | [S1](#s1-the-text-tower) |
| S3 | int8 scan at 100k and 1M rows | iPhone, same app | [S3](#s3-the-scan) |
| S4 | AVPlayer over a resource loader, with seeks | iPhone, same app | [S4](#s4-video-over-a-resource-loader) |
| S5 | Which `driftwire` runs on which Synology | The NAS | [S5](#s5-the-synology) |
| S2 | A link through the real port forward; NAT64 | NAS, Mac, iPhone | [S2](#s2-through-the-port-forward) |

**Do S1 first.** It can end the project on its own (more than 500 ms per encode on the oldest phone, or
the app being killed), and the simulator already shows its memory is a worry (BLOCKERS.md).

## What was run before you got this

| | Built | Run on the iOS Simulator | Measured on real hardware |
|---|---|---|---|
| S1 | Simulator and device | Yes: real `text.onnx` loads and encodes | **No** |
| S3 | Simulator and device | Yes: 100k and 1M, heap and mapped | **No** |
| S4 | Simulator and device | Yes: file and HTTP range source, 7 seeks each | **No** |
| Plain socket probe | Simulator and device | Yes, over loopback, by name and by IPv4 literal | **No**: never on a NAT64 network |
| `range_server.py` | n/a | Yes, on this Mac over loopback | **No**: not through a real forward |
| `probe-synology.sh` | n/a | Syntax checked, and run on a Mac, where the Linux fields are empty | **No** |
| `build-linux.sh` | **Not run**: no Docker on this Mac; syntax checked only | | **No** |
| S2 and S5 steps, Task Scheduler and Container Manager | Written from the code and `28-driftwire.md`; no step has been carried out | | **No** |
| A DSM package (`.spk`) | **Deferred**: this kit does not build or try one | | **No** |

Simulator numbers are a Mac's. They prove the code paths work and nothing about a phone's speed.

## Install the app on the iPhone

1. Turn on Developer Mode: Settings → Privacy & Security → Developer Mode. Plug the phone in and trust the Mac.
2. Open `app/MobileSpikes.xcodeproj` in Xcode. Under the target's Signing & Capabilities, pick your team.
   A personal team may need a different bundle identifier: change `dev.mediaviewer.spikes.m1` to something of yours,
   and use it in the commands below.
3. Pick the phone as the destination and press Run. The project has only a Release configuration, on purpose:
   an unoptimised scan would give meaningless numbers.
4. Get the phone's identifier, for the commands below:

```bash
xcrun devicectl list devices
```

Then put the model on it (replace `DEVICE`):

```bash
xcrun devicectl device copy to --device DEVICE --domain-type appDataContainer --domain-identifier dev.mediaviewer.spikes.m1 --source "$HOME/Library/Application Support/MediaViewer/Add-ons/AI/data/fetched/ai/models/clip-l14/text.onnx" --destination Documents/text.onnx
```

If that path is empty, turn on search in MediaViewer's AI add-on so it downloads the model. Finder works too: phone → Files → M1 Spikes.

**Before measuring:** charge above 50 %, Low Power Mode off, close other apps, let the phone cool. Every result records the thermal state: discard runs that are not `nominal`.

**Getting results off the phone:** after each session:

```bash
xcrun devicectl device copy from --device DEVICE --domain-type appDataContainer --domain-identifier dev.mediaviewer.spikes.m1 --source Documents/results.jsonl --destination results-iphone.jsonl
```

```bash
python3 tools/spikes/mobile/summarise.py results-iphone.jsonl
```

A `killed` row means the previous run ended inside the step it names. That is how jetsam shows up, and it is a result: write it down.

## S1: the text tower

Open the app fresh for each measurement, since the peak-memory figure belongs to the whole process.

| Step | Do | Record |
|---|---|---|
| Warm | Force quit, open, tap **Run (performance cores)**. Three times. | `load_ms`, `encode_p50_ms`, `encode_p95_ms`, the `mb_*` fields |
| One thread | Force quit, open, tap **Run (1 thread)**. Three times. | Same |
| Cold, force quit | Turn on **Run at launch**. Force quit, open, wait for the result. Five times. | `did_finish_to_first_encode_ms`, `prewarmed` |
| Cold, reboot | With **Run at launch** on, reboot, unlock, wait a minute, open. Three times. | Same. `uptime_s` tells these runs apart. This is the case with no file cache |

iOS can start ("prewarm") a process long before you tap, so a clock from the process start can be seconds too long. Each result says `prewarmed`, and `process_age_at_start_ms` shows how old the process already was. Quote `did_finish_to_first_encode_ms`, and use `process_start_to_first_encode_ms` only from runs where `prewarmed` is false.

Turn **Run at launch** off afterwards. Do all of it on the oldest iPhone you intend to support and on the newest.
**Stop line (§16):** over 500 ms encode on the oldest phone, or a `killed` row.

## S3: the scan

Force quit between rows of this table, so one run's memory does not colour the next.

| Tap | What it answers |
|---|---|
| **100k, heap** | Today's reader at the plan's 100k-frame target. Is the desktop's loop good enough? |
| **100k, mapped file** | The same, in clean file-backed pages |
| **1M, mapped file** | M12's design: writes a 768 MB file in Caches the first time |
| **1M, heap (may be killed)** | Today's reader at 1M: expect a `killed` row. Last, because it may take the app with it |

Then reboot, open, and tap **1M, mapped file** again: `pass_first_ms` is the read from flash with no cache, and `file_reused` is `true`. Finish with **Delete S3 files**.

Each run covers the desktop's loop, NEON and `FEAT_DotProd` (where the CPU has it), on one thread and on the performance cores. The first row of a mapped run (`desktop_copy`, one thread) is the only one that reads from flash; later rows find the pages cached. For a mapped file, read `mb_after_scan`: `mb_after_load` is taken before any page is touched.

## S4: video over a resource loader

Put a real camera clip on the phone (the first `.mp4`, `.mov` or `.m4v` by name in Documents is used), through Finder or `devicectl device copy to` as above. Prefer a large HEVC clip from the phone, since that is the real case, and at least 65 seconds long: each run starts with 60 seconds of playing with no seeks, then the seeks.

1. Tap **Play and seek: clip in Documents**. Three times.
2. On the Mac, on the same Wi-Fi:

```bash
python3 tools/spikes/mobile/nas/range_server.py serve /path/to/clip.mov --port 47615
```

3. It prints a URL. Put the Mac's address in it, type it into the app's field, and tap **Play and seek: range server**.
4. Repeat with Network Link Conditioner on the phone (Settings → Developer) at 5, 30 and 100 Mbit.

Record `first_frame_ms`, `seek_to_playing_p50_ms` and `_max_ms`, `stalls`, `bytes_served`, and for the no-seek phase `sustain_played_s`, `sustain_wall_s`, `sustain_waiting_ms` and `clip_mbit`. A link that cannot sustain the clip shows as waiting time, which stall counts alone can miss. A seek marked `timed_out` is a finding.

## S5: the Synology

**This tests a bare program and a container. A DSM package (`.spk`) is deferred:** this kit does not build one, so the plan's question about package formats stays open for that format, and the report has to say so.

**On the NAS:** turn on SSH (Control Panel → Terminal & SNMP). Replace `admin` below, wherever it appears, with an account in the administrators group, and use that same account throughout.

**What needs Docker or a Linux machine:** only the relay, `mv-driftwire-relay`. It is not in the CI packages. S5's look at the NAS, `driftwire` itself, the range server and the NAT64 test need neither. Without the relay you can't do S2 step 4, and the boot test in step 4 uses the range server instead.

**1. Copy what needs no build.** `driftwire` comes from CI. Run 42394906859 is `main` at `1facf73a`, and needs glibc 2.35 or newer (x64, checked):

```bash
gh run download 42394906859 -n driftwire-linux-x64 -D /tmp/driftwire-linux-x64
```

```bash
tar -xzf /tmp/driftwire-linux-x64/driftwire-*-linux-x86_64.tar.gz -C /tmp/driftwire-linux-x64
```

```bash
ssh -t admin@nas 'sudo mkdir -p /volume1/mv-spike && sudo chown admin /volume1/mv-spike'
```

```bash
scp -O /tmp/driftwire-linux-x64/driftwire-*-linux-x86_64/bin/driftwire tools/spikes/mobile/nas/probe-synology.sh tools/spikes/mobile/nas/range_server.py admin@nas:/volume1/mv-spike/
```

Use `driftwire-linux-arm64` for an aarch64 model; the names inside change to match. The artifact expires; if it has, `gh run list --workflow ci.yml --branch main --event push --status success --limit 1` gives a newer run.

**2. Look at the NAS.**

```bash
ssh admin@nas 'cd /volume1/mv-spike && sh probe-synology.sh'
```

Paste the output into the report. `uname -m` says which build: `x86_64` is x64, `aarch64` is arm64, and anything else (`armv7l`, for example) has no build and goes off the supported list.

**3. Build the relay,** on a Mac with Docker, or by following the script's commands on any Linux machine:

```bash
tools/spikes/mobile/nas/build-linux.sh x64
```

It builds `driftwire` too, and prints the newest glibc symbol each program needs. If DSM's glibc is older than that, try `BASE=ubuntu:20.04` before it (untested), or step 5's container. Then copy the result over the top:

```bash
scp -O tools/spikes/mobile/nas/dist/x64/driftwire tools/spikes/mobile/nas/dist/x64/mv-driftwire-relay admin@nas:/volume1/mv-spike/
```

**4. Start at boot.** Make the key first, once, as the account you will run it under:

```bash
ssh admin@nas 'cd /volume1/mv-spike && head -c 24 /dev/urandom | base64 > key.txt && chmod 600 key.txt'
```

Control Panel → Task Scheduler → Create → Triggered Task → User-defined script. Event **Boot-up**. Set **User** to that same account, not `root` and not a different user: it owns `key.txt` and can write to the folder, so a failure here means boot, not permissions. This script:

```
cd /volume1/mv-spike && ./mv-driftwire-relay --key-file key.txt --port 47614 >> relay.log 2>&1
```

**No relay built?** Copy a test clip you would not mind a stranger seeing, and use the range server as the boot job instead. Take the `python3` path from the probe's output (`/usr/local/bin/python3` here is an example), and check `range.log` rather than `relay.log`:

```bash
scp -O /path/to/clip.mp4 admin@nas:/volume1/mv-spike/clip.mp4
```

```
cd /volume1/mv-spike && /usr/local/bin/python3 range_server.py serve /volume1/mv-spike/clip.mp4 --port 47615 >> range.log 2>&1
```

Reboot the NAS and check `relay.log`. Record whether it starts after a reboot, whether `/volume1/mv-spike` survives a DSM update, and what happens when you replace the binary while it runs and restart it. Which account the real service should use is M2b's call.

**5. Container Manager, on models that have it** (the probe's `container_manager` line). It supplies its own glibc, so it is also the way round a DSM that is too old. Install the package from Package Center, then:

```bash
ssh -t admin@nas 'sudo /usr/local/bin/docker run --rm -v /volume1/mv-spike:/s -w /s ubuntu:22.04 ./driftwire --version'
```

With the relay built, disable the Task Scheduler job from step 4 and start it in a container that comes back after a reboot:

```bash
ssh -t admin@nas 'sudo /usr/local/bin/docker run -d --name mv-spike-relay --restart unless-stopped --network host -v /volume1/mv-spike:/s -w /s ubuntu:22.04 ./mv-driftwire-relay --key-file key.txt --port 47614'
```

Reboot, and check `sudo /usr/local/bin/docker logs mv-spike-relay`. Remove it with `sudo /usr/local/bin/docker rm -f mv-spike-relay`.

Repeat on every Synology model you can reach. Models you could not test stay off the list.

## S2: through the port forward

Today's `driftwire` picks its port at random, so a hand-made forward cannot reach it. These steps use the two things that do listen on a fixed port: the relay, and a plain range server for the path. BLOCKERS.md says what this cannot answer.

**Keep the public DHT out of it.** Your Mac's saved Driftwire settings have the DHT **on**, so never run `driftwire` on the Mac without the throwaway `HOME` in step 4; a saved setting would join the DHT as soon as the internet is on. Do not type `/dht` anywhere. Do not run any of the test programs or Catch2's hidden tags.

**1. Forward the ports.** On the router: TCP 47614 and TCP 47615 to the NAS. Allow both in DSM's firewall if it is on. Use a name for the NAS (DDNS) rather than an address, since NAT64 needs one.

**2. Serve a test clip.** Pick one you would not mind a stranger seeing:

```bash
ssh -t admin@nas 'cd /volume1/mv-spike && python3 range_server.py serve /volume1/mv-spike/clip.mp4 --port 47615'
```

DSM needs its Python 3 package for this. Keep the printed URL.

**3. Measure the path.** With the Mac on other Wi-Fi, then on the phone's hotspot, which is mobile data:

```bash
python3 tools/spikes/mobile/nas/range_server.py probe "http://NAME:47615/TOKEN/clip.mp4"
```

Record each line. Then, on the phone, on mobile data with Wi-Fi off, use the same URL with **Play and seek: range server** (S4's fields).

**4. A Driftwire link through the forward.** On the NAS, in a terminal, since it refuses to run without one:

```bash
ssh -t admin@nas 'cd /volume1/mv-spike && mkdir -p in && XDG_STATE_HOME=/volume1/mv-spike/state ./driftwire --save-to /volume1/mv-spike/in'
```

Type `/internet on`, then `/relay-server` and enter `NAME:47614#` followed by the contents of `key.txt`, then `/code`. `/help` lists them if one has moved. A NAS has no secret store, so expect to enter the key each run.

On the Mac, on hotspot, get the sender from the release, and run it:

```bash
gh release download v0.1.41 -p 'driftwire-0.1.41-macos-universal.tar.gz' -D /tmp/driftwire-mac
```

```bash
tar -xzf /tmp/driftwire-mac/driftwire-0.1.41-macos-universal.tar.gz -C /tmp/driftwire-mac
```

Run it with a throwaway home, so none of your saved settings apply:

```bash
HOME="$(mktemp -d)" /tmp/driftwire-mac/driftwire-0.1.41-macos-universal/driftwire
```

Before anything else, type `/settings` and check the DHT is off. If it says on, `/quit` and stop. Then `/internet on`, add a large test file with `/send` and its **full path** (`~` now means the throwaway folder), then `/send-to` and paste the code. Accept on the NAS (`y`).

Record the bearer the transcript shows, the size and the time. **If the bearer is not the relay,** this run did not go through your forward: either the router mapped a port by itself, or a public IPv6 address reached the NAS directly, since a port forward is only for IPv4. Turn off UPnP and NAT-PMP on the router, turn off IPv6 for this test (DSM: Control Panel → Network → Network Interface → Edit → IPv6), and retry.

If the NAS cannot register at the relay, your router probably does not hairpin. Say so in the report; the sender side can still be measured with the receiver on a machine outside your network.

**5. NAT64.** On the Mac: System Settings → General → Sharing, hold Option and open Internet Sharing's details, then turn on **Create NAT64 Network** (Apple moves this between versions). Share over Wi-Fi and join the phone to it. On the phone, with the NAS's **name** in the URL:

1. Tap **Plain socket probe: range server (NAT64)**. It does what the C++ core will do: `getaddrinfo`, then `connect` and one range GET, and logs which family answered. `dns64` must be `true`, or this is not a NAT64 network and the test means nothing.
2. Tap **Play and seek: range server**.

Then put the NAS's IPv4 address in the URL and tap the probe again. That records two rows: through `getaddrinfo`, which may well work, as Apple synthesises an address for an IPv4 literal, and on a raw `AF_INET` socket, which is what a stored IPv4 address would use. Don't treat either as a pass or fail in advance; record what happens. URLSession also handles literals by itself, so AVPlayer's result proves nothing about the raw path.

**6. Clean up.** Remove both forwards from the router, turn UPnP and IPv6 back on if you turned them off, disable the boot task, remove the container if you made one, and delete `/volume1/mv-spike`. Delete the app from the phone, which removes the model, the clip and the S3 files with it.
