# Driftwire over Tor: the spike (issue #472, option 11)

Status: **evaluated on paper; dropped from #472 for now** ([12](../design/12-decision-log.md#driftwire),
2026-10-18). Nothing is built in the add-on. The measurements below need the live Tor network, so
they were **not run**: the harness is `tools/driftwire-relay/tor_spike.py`, and the owner runs it.

## What Tor would add

The one case the other paths still miss: two computers behind hard NATs, with no IPv6, no router
mapping, no computer of the person's own with a public path and no relay server of their own.
A receiver would publish an ephemeral v3 onion service for one code; the sender connects through
Tor; the internet handshake (CPace, then Noise XX) and the session run end to end over it, as over
any link. Tor would also hide each side's address from the other, which nothing else here does.

## The two ways to run it

| | C tor as a child process | Arti (`arti-client` + `tor-hsservice`) |
|---|---|---|
| Licence | BSD-3-Clause; with OpenSSL 3 (Apache-2.0), libevent (BSD-3), zlib. Separate process: no linkage question for the add-on ([11](../design/11-licensing.md)) | MIT OR Apache-2.0; in process it is linked (fine for the add-on's licence) |
| Toolchain | none new: a prebuilt or vcpkg-built `tor` per platform, signed with the add-on | **Rust** in a C++/CMake/vcpkg build, a stack change that needs its own decision ([01](../design/01-decisions.md)) |
| Onion service hosting | mature (v3, ephemeral `ADD_ONION … Flags=DiscardPK` on the control port: no key on disk) | still marked work in progress in `tor-hsservice`'s docs (DoS resistance, proof of work and vanguards listed missing as of 2024); not checked since |
| Size (expected, not measured) | about 5–10 MB per platform with its libraries | about 10–20 MB as a binary; in process, a few MB more in the add-on |
| Startup to a reachable onion (expected) | bootstrap (consensus, microdescriptors) about 5–30 s cold; then the descriptor is published and reachable in roughly 10–60 s | similar or slower |
| Throughput (expected) | an onion-to-onion path has six hops: about 1–5 MB/s, varying a lot | similar |
| Mac + Windows (D9) | yes, the same child process and control port on both | yes, but only once Rust is in both builds |

Either way the add-on would carry a second executable or a large library, Tor's directory
authorities and relays would see that this machine uses Tor while a code waits, and a 10 GB clip
would take about an hour.

## The harness (owner runs it)

`tools/driftwire-relay/tor_spike.py` measures, on this machine, against the live Tor network:

1. the size of the given `tor` binary and the libraries it loads;
2. the time from start to bootstrap 100 %;
3. the time from `ADD_ONION` to a first successful connection to that onion through the same
   Tor's SOCKS port (a full circuit to its own service);
4. the throughput of `--mb` MB sent through the onion to a local sink.

It refuses to run without `--live-tor`, binds tor's control and SOCKS ports to 127.0.0.1 with
cookie authentication, gives bootstrap `--bootstrap-timeout` seconds (300), drains tor's log to the
end, writes nothing outside a temporary data directory (removed after), and prints one JSON line
per run. It drives C tor only; Arti is left out until its onion services
are no longer experimental, and the plan says so.

```bash
python3 tools/driftwire-relay/tor_spike.py --tor /opt/homebrew/bin/tor --mb 64 --runs 3 --live-tor
```

## Keep or drop

Dropped for #472: the own-device relay and the relay server cover the hard-NAT pair without a
third-party network, with no extra executable, at full speed. Reopen if the owner wants the address
hiding, or if the harness numbers come in much better than expected; the way in would then be C
tor as a child process with ephemeral onions, not Arti in process.
