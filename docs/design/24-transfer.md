# 24 — Fast network transfer

How MediaViewer's verified copies reach network-share speed: positional I/O in the file
port, the deep (several-requests-in-flight) path in `io::verified_copy`, the per-file and
per-batch copy profiles, and the `copybench` harness. Label: PR 49.

## Why a share needs its own shape

Every Import, and every F8 move across volumes, goes through `io::verified_copy`. On a local
disk or a card that path is bandwidth-bound and sequential: one 4 MiB read, then one write per
destination, overlapped through 2–4 buffers. On a share the same shape is latency-bound:

- **One request in flight per file** leaves a 10 GbE link mostly idle, because each request is
  a network round trip. Windows runs file-extending writes one at a time whatever the handle;
  on the Mac the writer's `F_NOCACHE` makes every write go to the server before it returns.
- **One file at a time** pays create, flush, read-back and rename as round trips in series for
  every file; with small and medium files they dominate.
- The uncached read-back sends the bytes over the wire a second time. It is kept, and gets the
  same deep queue.

Outside the app's control: SMB signing (on by default in Windows 11 24H2 and on macOS) costs
CPU per byte, and a single channel and the MTU cap the link.

## Network-speed copy engine

One change in the shared core (`src/io`), used by both hosts.

- **Positional I/O in the file port** (`io/file_port.h`): `file_reader::read_at`,
  `file_writer::write_at` and `set_size`. Several threads can each have a request in flight on
  one handle: an overlapped handle and a per-request event on Windows, `pread`/`pwrite` on
  POSIX.
- **The deep path in `verified_copy`** (`copy_options::read_depth` / `write_depth` > 1, clamped
  to `kMaxIoDepth` 16):
  - Reader threads claim chunks in order and read them at their offsets. The calling thread
    hashes them in order, and per destination the writer threads write them at their offsets.
  - Chunk *k* lives in slot *k* mod *n*. A slot frees once its chunk is hashed and written
    everywhere, so the hash never falls more than *n* chunks behind. The caller's `yield`
    between chunks (Background priority, Pause) throttles the whole pipeline.
  - Destinations are sized up front, so no write extends the file.
  - The uncached read-back uses the same deep reader.
  - Fault injection, retry-once, keep-both and cancel-leaves-nothing behave as on the
    sequential path.
- **Profiles** (`io/verified_copy.h`):
  - `copy_profile_for(src, dest)` is per file and cheap (`is_network_path` on each end, no
    device query). It turns on deep reads when the source is on a share and deep writes when a
    destination is.
  - `batch_copy_profile(src_dir, dest_dir)` is called once per batch. It also runs
    `kNetworkFilesInFlight` files at once (`io::for_each_in_flight`, `io/in_flight.h`) when
    either end is a share, unless the source is a removable card.
  - Network numbers: `kNetworkIoDepth` 8, `kNetworkChunkBytes` 2 MiB, `kNetworkFilesInFlight` 4.
  - Budget: (depth + 2) chunks per file × 4 files = 80 MiB, under `kCopyBufferBudget`
    (128 MiB).
  - **A card is never read deep or by two files at once** ([18](18-import.md) "Throughput").
    Local disks and cards keep the sequential path byte for byte.
- **Where it is used.**
  - F8 copy and move, both hosts (`src/shell/file_jobs.cpp`, `src/shell/main_mac.mm`): files in
    flight from the batch profile. A move's verified copy is deep.
  - F8 *copy* keeps the OS copier (`CopyFileExW` / `copyItemAtURL`) and is unverified. Those
    copiers pipeline SMB themselves and use server-side copy within one share.
  - Import: the host table's `copy` takes the per-file profile (`src/addon/host.cpp`), so a
    card → NAS import writes deep and still reads the card one request at a time. Import copies
    one unit at a time.
- **Harness.** `copybench` (`tools/copybench`) copies a folder with `--mode auto|seq|deep`
  (`auto` is what the app would pick via `batch_copy_profile`), `--depth`, `--files`,
  `--chunk-kib`, verify on or off (`--no-verify`), and prints JSON (MB/s, files/s).
  - `--rtt-us N` makes every file request wait first (`io::detail::simulated_round_trip`):
    a stand-in for a share on a local disk, not a substitute for one.
  - `--make-fixture <dir> --count N --size-kib N` writes incompressible test sets.
- **Tests:** the deep cases live in the `[io]` tests of the Import suite (`mv_import_tests`),
  including under ThreadSanitizer.

## Not built

- **Transfer (PR 50)**, the general copier: an `io/transfer_job` over this engine (files and
  folders → a folder, copy or move, keep-both / skip-if-identical, journal and resume), its
  `mv_transfer_*` ABI, and the WinUI / SwiftUI Transfer window.
- **The link check**: reading a share's SMB dialect, signing, encryption and channel count
  (`MSFT_SmbConnection` / `smbutil statshares`) and saying which one caps the link.
- Several Import units copied at once to a share.
