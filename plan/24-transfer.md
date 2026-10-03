# 24 — Fast network transfer and Transfer (PRs 49–50, both platforms)

Owner, 2026-10-01: a NAS on 10 GbE copies at about 300 MB/s through the OS and through
MediaViewer. Make MediaViewer's own copies to and from a share fast, keep every file verified,
and give the app a general copier for any files, not only media
([12](12-decision-log.md) 2026-10-01).

## Why our copies were slow

Every Import, and every F8 move across volumes, goes through `io::verified_copy`. On a share
that path is latency-bound, not bandwidth-bound:

- **One request in flight per file.** One blocking 4 MiB read, then one blocking write per
  destination. Each request is a network round trip. Windows runs file-extending writes one at
  a time whatever the handle. On the Mac the writer's `F_NOCACHE` makes every write go to the
  server before it returns.
- **One file at a time.** Create, flush, read-back and rename are each a round trip, done in
  series for every file. With small and medium files they dominate.
- The read-back sends the bytes over the wire a second time. It stays (owner, 2026-10-01);
  it gets the same deep queue.

What a user's machine adds, which the app cannot change: SMB signing (on by default in
Windows 11 24H2 and on macOS) costs CPU per byte, a single channel, and MTU. PR 50 says which
of these is on.

## PR 49 — Network-speed copy engine (shared core)

The shared core makes one change; both hosts use it. Nothing new leaks out of `io/` (D9).

- **Positional I/O in the file port.** `file_reader::read_at`, `file_writer::write_at` and
  `set_size`. Several threads can each have a request in flight on one handle: an overlapped
  handle and a per-request event on Windows, `pread`/`pwrite` on POSIX.
- **The deep path in `verified_copy`** (`copy_options::read_depth` / `write_depth` > 1):
  - Reader threads claim chunks in order and read them at their offsets. The calling thread
    hashes them in order, and per destination the writer threads write them at their offsets.
  - Chunk *k* lives in slot *k* mod *n*. A slot frees once its chunk is hashed and written
    everywhere, so the hash never falls more than *n* chunks behind. The caller's `yield`
    between chunks (Background priority, Pause) throttles the whole pipeline.
  - Destinations are sized up front, so no write extends the file.
  - The uncached read-back uses the same reader.
  - Fault injection, retry-once, keep-both and cancel-leaves-nothing behave as on the
    sequential path.
- **Profiles.**
  - `copy_profile_for(src, dest)` is per file and cheap (`is_network_path` on each end). It
    turns on deep reads when the source is on a share and deep writes when a destination is.
  - `batch_copy_profile(src_dir, dest_dir)` is once per batch. It also runs
    `kNetworkFilesInFlight` files at once (`io/in_flight.h`), unless the source is a removable
    card.
  - Network numbers: depth 8, 2 MiB chunks, 4 files.
  - Budget: (depth + 2) chunks per file × 4 files = 80 MiB, under `kCopyBufferBudget` (128 MiB).
  - **A card is never read deep or by two files at once** ([18](18-import.md) "Throughput").
    Local disks and cards keep the sequential path byte for byte.
- **Where it is used.**
  - F8 copy and move, both hosts: files in flight from the batch profile. A move's verified
    copy is deep.
  - F8 *copy* keeps the OS copier (`CopyFileExW` / `copyItemAtURL`), so it stays unverified as
    before. Those copiers pipeline SMB themselves and use server-side copy within one share.
  - Import: the host table's `copy` takes the per-file profile, so a card → NAS import writes
    deep and still reads the card one request at a time. Import still copies one unit at a
    time; several units at once is a follow-up, measured on a share first.
- **Harness.** `copybench` (`tools/copybench`) copies a folder with `--mode seq|deep|auto`,
  verify on or off, and prints JSON (MB/s, files/s).
  - `--rtt-us` makes every request wait first (`file_port.h detail::simulated_round_trip`).
    This is a stand-in for a share on a local disk, not a substitute for one.
  - `--make-fixture` writes incompressible test sets.

**Verify (both platforms):**
- On a 10 GbE SMB share, `copybench --mode auto` with verify (big and RAW-sized sets) reaches
  ≥ 2× `--mode seq` on the same files, runs alternated.
- Unverified auto is ≥ 90 % of `robocopy /MT:16 /J` (Windows) or `ditto` (Mac) on the same
  files.
- A local → local `seq` run is within noise of the base build.
- `mv_import_tests "[io]"` passes, the deep cases under ThreadSanitizer included.
- Both present-loop gates hold while an F8 move to the share runs.

## PR 50 — Transfer (general copier) and the link check

Small code in the app itself: no add-on download and no separate helper app.

- **Core.** `io/transfer_job` over PR 49's engine.
  - Files and folders → a folder, copy or move, keeping the tree.
  - Conflicts: keep both (safe name) or skip-if-identical (size, then BLAKE3). **Never
    overwrite.**
  - Full verify by default; hash-on-read is labelled as the faster, weaker option.
  - A move deletes a source only after it verifies.
  - Pause, resume and cancel. A small SQLite journal lets a crash or a dropped share resume
    with verified files done.
  - Background priority by default (yields to the present loop), or Fast.
- **ABI.** `mv_transfer_*`: an opaque job handle, a POD progress snapshot (bytes, files, MB/s,
  ETA) and completion on the pumped queue ([14](14-abi.md)). The host never waits on it
  (rule 1).
- **Chrome, twice.**
  - A Transfer window in WinUI and SwiftUI: sources, destination, copy/move, verify, conflicts.
  - Progress as Import shows it. Closing the window keeps the job running, with a command-bar
    indicator.
  - A summary of copied / skipped / failed, each with its reason, and **Retry failed**.
  - Keyboard-complete: rows in [16](16-commands.md) before any key is bound.
- **The link check (read-only).** On a share, read the connection's dialect, signing,
  encryption and channel count (`MSFT_SmbConnection` on Windows, `smbutil statshares` on Mac).
  If one of them caps the link, say so in one line with a "how to change it" link. The app
  never changes an OS or NAS setting.
- **Privacy (rule 6).** Paths, names and hashes stay local. Telemetry, if on, gets counts
  and MB/s only.

**Verify (both platforms):**
- A 50 GB mixed tree local → share → local with full verify comes back byte-identical
  (external SHA-256 manifest).
- MB/s is within 10 % of PR 49's copybench auto on the same share.
- A kill mid-job resumes with no duplicates and no temporaries.
- A move never removes an unverified source.
- Both present-loop gates hold during the job.
