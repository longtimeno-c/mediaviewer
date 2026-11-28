# Add-on system and add-ons: audit (2026-11-25)

Read-only audit of `src/addon`, `src/addons/{ai,driftwire,enhance,import,phone,places,vpn}`, `src/wire`, `src/infer`, `src/geo`, `src/remote`. Nothing was built or run; every finding is from reading code, marked **confirmed by reading** or **suspected**. Open issues (`gh issue list --limit 200`) and PRs (`gh pr list`) were checked; filed items are cited, not re-reported. The four parts below were written by area; the cross-area summary comes first. A few top findings were re-checked against the source while merging (marked "re-checked").

## Summary of findings (all areas, by severity)

No critical finding. The signature/zip/Lua sandbox, model downloads (HTTPS + SHA-256), Import's verified copy (no-replace rename, uncached read-back after F_FULLFSYNC, temp removed on cancel, no source deleted), Driftwire's path checks, nonces, size limits and relay forward, and phone pairing codes (128-bit, single-use, 5 min) all held up.

### High
| # | Area | Finding | Evidence | Status |
|---|---|---|---|---|
| AI-H1 | Local search | "Index full" latches and never clears when folders are removed (the documented remedy); cleared only by changing the cap or Clear all. Size counts free pages and nothing vacuums except Clear all. | `src/addons/ai/engine.cpp:1213`, `:853`, `:5510`; `index_db.cpp:1871` | confirmed, re-checked |
| AI-H2 | Local search | Every model (image, sound, speech, faces, documents towers) stays resident for the session after indexing goes idle; search only needs the text tower. Not covered by PR #693 (which is about Settings attaching without loading). | `engine.cpp:1444-1660`, `2613-2637` | confirmed |

### Medium
| # | Area | Finding | Evidence | Status |
|---|---|---|---|---|
| DW-M1 | Driftwire | LAN DoS: any failed nearby handshake (even a bare TCP connect) feeds one global counter; 5 in 60 s refuses **all** nearby receives for 30 s, shown only in the log. This is the #497 rate limit, but keyed globally rather than per source. | `src/addons/driftwire/engine.cpp:1294-1303`, `3203-3209`, `1317` | confirmed, re-checked |
| HOST-M1 | Host (Windows) | DLL hashed by path, then `LoadLibraryExW` reopens it; later loads only compare size+mtime; no Authenticode. Same-user attacker can swap the DLL after verify (TOCTOU) — but same-user code execution already exists, so this is defence-in-depth. | `src/addon/manifest.cpp:812-843`, `host.cpp:1726-1756`, `loader_win.cpp:60` | confirmed |
| HOST-M2 | Host | Rollback: an older signed version is refused only over a newer working install; a fresh install or one over a tampered copy accepts any older signed (possibly vulnerable) package. | `src/addon/store.cpp:258-262`, `open_store.cpp:287` | confirmed |
| HOST-M3 | Remote cache (Windows) | `good_rel` allows `:`, so a hostile paired server's name like `C:x.jpg` makes `fs::path /` escape the cache dir. | `src/remote/cache.cpp:68`, `key.cpp:21-35` | suspected (Windows path semantics) |
| HOST-M4 | Build | Dev public key is rejected only when `GITHUB_ACTIONS` is set; a locally built Release could trust the dev signing key. | `cmake/import.cmake:93` | confirmed |
| AI-M1 | infer | ORT CPU arena never shrunk; indexing peak stays reserved. | `src/infer/ort.cpp:248-260`, `315` | suspected |
| AI-M2 | Enhance | Cancel blocks the add-on worker until the job thread exits; cancel not polled during restyle model load / Core ML compile. | `src/addons/enhance/runner.cpp:313-323`, `691-707` | confirmed |
| AI-M3 | Local search | PDFs parsed in-process (PDFium); time limit checked only between pages — a pathological PDF in an indexed folder can hang/crash the viewer. No exploit constructed; relates to #390. | `src/addons/ai/pdf_pdfium.cpp:146-171` | suspected |
| VPN-M1 | VPN (Windows) | DNS leak: relies on interface metric, no NRPT/port-53 rule; kill switch allows all local networks on every port, so DNS still reaches the router. | `src/addons/vpn/vpn_win.cpp:27-28`, `200-221` | code confirmed, leak suspected |
| VPN-M2 | VPN | System proxy is set before its undo record is written; a crash in between leaves a dead PAC URL never restored. | `src/addons/vpn/service.cpp:589-596` | confirmed, re-checked |
| IMP-M1 | Import | Resume hashes whole files on the control thread with no cancel; quit and scans wait. | `src/addons/import/engine.cpp:1708-1724` | confirmed |

### Low / Low–Medium (details in the parts)
- DW: nearby peer identity `"key"` in offer/accept is a claim, not proven; resume records keyed on it can be read/wiped by a past contact claiming another's key (no data splice: whole-file read-back blocks it) — `transfer.cpp:224`, `576`; `engine.cpp:3389-3400`, `3532`, `1096-1109`. Internet code dies at the first hello from anyone (by design) — `engine.cpp:3190`.
- HOST: O(n²) remote cache trim (`cache.cpp:161`); `drain()` may touch a freed worker (`slots.cpp:486`); `catalogue_port` assumes NUL termination and holds a lock across add-on calls.
- AI: pending-work query rescans and sorts all assets per refill (`index_db.cpp:861-888`); redundant `frames_spec` index (`index_db.cpp:190`); failed cloud dehydrate ignored (`engine.cpp:3715`).
- VPN: relay key falls back to a 0644 prefs file (`service.cpp:265-284`, `prefs.cpp:81-97`); macOS tunnel provider trusts whoever listens on its saved port (`mac_tunnel/PacketTunnelProvider.swift:58-75`); PAC fails open to DIRECT (`tunnel.cpp:234`).
- Phone: tab/newline in a password breaks `host users apply` input (`self_host.cpp:613-621`).
- Import: paused import holds device locks and polls every 5 ms (`engine.cpp:1086-1093`).

### Status of filed items touched here
- **#497** (Driftwire code): item 1 partly fixed (protocol 3 server commitment `src/wire/channel.cpp:399-486`, nearby limit `engine.cpp:1294-1303`) but the receiver still accepts protocol 1/2 hellos (`channel.cpp:242`, `315-318`), so the commitment can be skipped; item 2 still open (offer read with no code proof, `engine.cpp:3281-3283`); item 3 fixed (`engine.cpp:920-929`); item 4 protocol no longer lowered (`engine.cpp:777`) but names still unauthenticated.
- **#540** throughput, **#532** updates, **#390** out-of-process inference, **PR #693** held models: not re-reported.

### Top improvements
- Startup: every add-on lookup re-verifies every manifest's signature, and the AI pack's multi-GB files are re-hashed each launch — cache verification by (path, size, mtime, file id) per process, verify once at install + cheap check at load (part_host).
- Local search: unload non-text towers after idle; incremental vacuum / measure live pages for the cap; keep a pending-work cursor instead of rescans (part_ai).
- Import: honour `files_in_flight`, batch fsyncs (part_import).

---

# Audit: add-on host side, src/remote, src/geo

Scope: `src/addon/*` (store, package, manifest, open_store/open_manifest, script runtime and host, slots,
loaders, catalogue_port, ai_resume, index_locations, settings), `src/remote/{cache,key,library}`,
`src/geo`. This was a read-only review: nothing was built or run. Each finding says whether it was
**confirmed by reading** the code or is only **suspected**.

Already-filed items are not repeated here: #400, #616, #525, #356/PR396, #618, #79.

---

## Inventory

### First-party (native) add-ons
These are `store.cpp`, `manifest.cpp`, `host.cpp` and `loader_{mac,win}.cpp`.

- **Manifest signature.** Schema 1. The signature is Ed25519 (libsodium) over the exact
  `manifest.json` bytes. The key is pinned in `manifest.cpp:23` and is the same key as the updater's.
  It is checked before any parsing (`manifest.cpp:538-553`).
- **What the manifest pins.** It lists every file with its size and SHA-256.
  - `verify_files` hashes every file. It also walks the folder and refuses any extra file, link or
    junction (`manifest.cpp:796-876`).
  - Only `.DS_Store`, `thumbs.db` and `desktop.ini` are allowed in addition.
- **Hash cache.** Each version folder is hashed fully once per process. After that, a per-process
  snapshot (size + mtime) stands in for the hash (`manifest.cpp:763-843`).
  `note_verified_move` carries the snapshot across the staging → install rename.
- **Install** (`store.cpp:239-316`):
  - Verifies the staged folder.
  - Refuses a downgrade when the installed copy is newer and works.
  - Has a family size ceiling, which is now always 0 (`family_ceiling`, `manifest.cpp:445-452`, retired).
  - Renames with no-replace.
  - Leaves a prune marker so older versions are deleted at the next start.
- **Load** (`host.cpp:1724-1787`): `store::find` (re-verifies) → `dlopen` / `LoadLibraryExW`.
  - On macOS, library validation (same Team ID) is the second gate.
  - On Windows, the DLL's own folder is on the dependency search path and the current directory is not.
- **Pieces.** `part_of` pieces are reached through `piece_dir`, which re-verifies on every call.
- **Dev key.** `MV_ADDON_DEV_PUBLIC_KEY` replaces the pinned key in developer builds
  (`manifest.cpp:454-481`). CMake refuses it only when `GITHUB_ACTIONS` is set.
- **Host table.** `mediaviewer_addon.h` is 1664 lines; the table is at API v3.
  - Rows are filled only when the matching `host_services` hook exists (`host.cpp:~1600-1650`).
  - There is **no per-add-on grant check** in the thunks. Every first-party add-on gets every row
    the shell provides. That is acceptable because native code is in-process and trusted.

### Open (third-party) add-ons
These are `open_store.cpp`, `open_manifest.cpp` and `package.cpp`.

- **Package reader.** It is a strict reader of stored-only zips:
  - Method 0 only, no extra fields or comments, no data descriptors.
  - Local and central headers must agree, and entries must be laid out end to end.
  - Names are case-folded and must be unique.
  - Caps: 64 MB and 2048 entries (`package.cpp:46-134`, `package.h:32-33`).
- **Paths.** `safe_relative_path` (`manifest.cpp:510-532`) rejects:
  - absolute paths, `\`, `:`, `..`, `.`, empty segments;
  - control characters and Win32 wildcards;
  - a trailing dot or space;
  - device names.
- **Publisher key.** It is self-certifying: the key lives in the manifest.
  - Trust is on first use, pinned per id in `publisher.json`, which the app writes outside the
    version folders (`open_store.cpp:350-366`).
  - MediaViewer's own key is refused for schema 2 (`open_manifest.cpp:226-230`).
- **Install.**
  - The user approves the package's SHA-256, and install checks it again (`open_store.cpp:313`).
  - Files are written with `create_new` and re-verified on disk before the rename.
  - Downgrades and other-publisher installs are refused (`open_store.cpp:277-293`).
- **No native code.** `native`, `chrome` and `main` are refused by name (`open_manifest.cpp:293-310`).
- **Scripts (API 3).** Lua text files only: at most 16, 1 MB each, 4 MB in total.
- **Permissions.** `folder.read`, `thumbnails` and `selection`.
  - `network` is malformed: no network, ever.
  - `files.write` and `metadata.write` are reserved and answer `needs_update`.
- **Contribution points.** Themes, settings, keymaps and commands are allowed.
  - Screens and slots are `code_not_allowed` for open add-ons until the script bindings land (#400).

### Script sandbox
These are `script_runtime.cpp` and `script_host.cpp`.

- **Libraries.** Lua runs with base, string, table, math, utf8 and coroutine only. There is no
  io, os, debug or package (`script_runtime.cpp:519-575`).
  - `dofile`, `loadfile`, `require` and `warn` are removed.
  - `load` is forced to text and takes no reader functions.
  - `string.dump` is removed and the string metatable is locked.
  - `collectgarbage` keeps only its harmless options; `print` goes to the rate-limited log.
- **Limits** (`script_runtime.h:50-56`):
  - 64 MB per state, enforced in the allocator: once over, always over.
  - 100 M VM instructions per task, and 1 s of wall clock per task.
  - A count hook every 10 k instructions. Once stopping, it raises on every instruction, so `pcall`
    cannot swallow the stop.
  - A shared watchdog covers C library loops through `mv_interrupt.h`.
  - Queue of 256 tasks; `mv.notify` at most 4 a second; `mv.log` at most 50 lines a second.
- **Bindings.** Only `core`, `folder`, `selection` and `settings` exist (`script_runtime.cpp:507-516`).
  `add_script_binding` has **no callers** outside the runtime (grep), so no binding reaches the
  network.
  - Bindings an add-on has no permission for are absent, not merely refused.
  - A command whose payload is not `none` needs `selection` (`open_manifest.cpp:496`).
- **Settings.** Values are scoped by add-on id and publisher key, so a new publisher under the same
  id never sees the old values (`settings_values.cpp:89-90, 253-256`).
- **Script log.** On the Mac it goes to `os_log` as `%{private}`. On Windows it is dropped
  (`script_abi.cpp:117-118`).

### Other parts
- **Slots and screens** (`slots.cpp`): one worker thread per add-on. Requests carry generations and
  later ones coalesce by key. Payloads are capped (surfaces, rows, text lengths, paths of at most
  4096 bytes, valid UTF-8).
- **Stubbed or reserved:**
  - `family_ceiling` always returns 0.
  - Later permissions answer `needs_update`.
  - Screens and slots for open add-ons wait for #400.
  - Windows keeps no script log.
- **`src/remote`:**
  - Key grammar is `mv-remote:<hex16>/<hex16>/<rel>`; `good_rel` rejects `..`, `\` and NUL.
  - `disk_cache` is an LRU keyed by BLAKE2b(root, rel, size, mtime).
  - The `library` fetches with resume and lets concurrent fetches of one file share one download.
- **`src/geo`:** a memory-mapped place database.
  - Header, checksum, section table and every index and offset are bounds-checked at open
    (`geo.cpp:140-230`).
  - The database is mapped once per process and every mapping is kept.

---

## Issues by severity

No critical findings. Everything below needs an attacker who can already write to the user's files,
a hostile remote server, or a mis-configured build.

### Medium

**M1. Windows: a DLL swapped after it was verified gets loaded (confirmed by reading).**

- **Where:** `manifest.cpp:812-843`, `host.cpp:1726-1756`, `loader_win.cpp:60-61`,
  `docs/design/18-import.md:157-159`.
- **What happens:**
  - Verification hashes by path and closes the file. `LoadLibraryExW` then opens the path again.
  - After the first verification in a process, later loads only compare size and mtime against the
    snapshot.
  - Windows has no load-time signature check like macOS library validation, and the host does no
    `WinVerifyTrust`.
  - So the doc's "every file … **at every load**" holds only as "once per process, then by size and
    mtime".
- **Attack:**
  1. Malware running as the same user (no admin needed; `%LocalAppData%` is user-writable) waits for
     MediaViewer to verify `addons\import\<v>\mv_import.dll`.
  2. It overwrites the DLL with a same-size payload and restores the mtime with `SetFileTime`.
  3. The next `loaded_addon::load` in that process skips the hash and loads it. Triggers include
     reloading the add-on, `piece_dir`, `tell_removed`, or `forget_outside_data` on Remove.
  4. Result: persistent code in a signed, trusted MediaViewer process, which evades AV allow-listing.
  5. Without the mtime trick there is still a race between the hash and `LoadLibraryExW`.
- **Severity:** medium-low, because it needs a same-user attacker. On macOS, library validation
  blocks it.
- **Fix:** before `LoadLibraryExW`, open the DLL with `FILE_SHARE_READ` only (no write or delete
  sharing), hash from that handle, and keep the handle until the load returns. Or run
  `WinVerifyTrust` and pin the certificate. Also correct the doc.

**M2. Rollback: an older signed version installs when nothing (or a broken copy) is installed
(confirmed by reading).**

- **Where:** `store.cpp:253-262`, `open_store.cpp:283-293`.
- **What happens:**
  - A signed manifest has no expiry and no minimum version.
  - A downgrade is refused only when the installed copy is newer **and** in state `ok`.
  - So a fresh install, or an install over a copy that fails verification, accepts any older
    validly signed version.
- **Attack:**
  1. Someone who can feed the installer an older release's manifest and zip installs a known-vulnerable
     build. Routes: a compromised mirror or proxy on the first-party channel, or social engineering
     for open-add-on sideloads.
  2. Or they flip one byte of the installed copy so its state becomes `invalid`, then offer the old
     package. The downgrade guard steps aside.
- **Severity:** medium-low.
- **Fix:** record a high-water version per id in an app-written file outside the version folders (as
  `publisher.json` already is) and refuse anything older. Or compile a minimum version per
  first-party id into the app.

**M3. Remote cache, Windows: a server-chosen file name can write outside the cache (suspected).**

- **Where:** `src/remote/cache.cpp:68` (`original_path`), `key.cpp:21-35` (`good_rel`),
  `library.cpp:763-778, 850-853`.
- **What happens:**
  - The cached original is stored as `dir/server/hash/<leaf of rel>`, built with
    `std::filesystem::operator/`.
  - `good_rel` rejects `..`, `\` and NUL, but **not `:`**. Device names and a trailing dot or space
    also pass.
  - On Windows, `p / "C:x.jpg"` *replaces* `p`, because the operand has a different root-name.
- **Attack:**
  1. A paired but hostile or compromised server lists a file named `C:evil.jpg`. It passes
     `is_listed_extension`.
  2. Opening it makes `fetch_into` write `C:evil.jpg.part`, then `admit` renames it to `C:evil.jpg`.
     Both are drive-relative to the process's current directory on C:, outside the cache.
  3. `trim_locked` may later delete it.
  4. A name like `CON.jpg` targets a device instead.
- **Why only suspected:** this applies only if `src/remote` is built and used on Windows. I did not
  confirm the Windows desktop client links it. Mac and iOS are unaffected.
- **Fix:**
  - Store the cached original as `<hash>.<ext>`; the leaf name is not needed on disk.
  - Or reject `:` and Windows-unsafe names in `good_rel`; the add-on host's `safe_relative_path`
    already handles these.

**M4. Release builds rely only on CI to keep out the developer key (confirmed by reading).**

- **Where:** `cmake/import.cmake:89-97`, `manifest.cpp:454-481`.
- **What happens:**
  - `MV_ADDON_DEV_PUBLIC_KEY` is refused only when the `GITHUB_ACTIONS` environment variable is set.
  - A local packaging run (`tools/package/build-release.ps1`, or Mac signing on the owner's machine)
    reusing a CMake cache that holds the key produces a signed app that trusts the dev key for
    first-party native add-ons.
  - `pinned_public_key()` also feeds the open-manifest "reserved key" check.
  - No packaging script checks for it (grep found it only in `tools/ai-bench` and the sideload
    scripts).
- **Severity:** low-medium; it needs a mistake, but the result is silent and total.
- **Fix:**
  - Make the release scripts fail when the define is present.
  - Have the binary show it at runtime (About, or a log line).
  - Or add a `static_assert` gated on a release-only define.

### Low

**L1. Remote cache trim is quadratic (confirmed by reading).**

- **Where:** `src/remote/cache.cpp:158-161`.
- **What happens:** `std::distance(lru_.begin(), it)` is O(n) and runs on every loop step, under the
  cache mutex. With tens of thousands of cached originals, every `admit` (and every `find` waiting
  on the lock) stalls.
- **Fix:** walk forward to the `keep_recent_` boundary once, or keep a size counter.

**L2. `slot_registry::drain()` can use a freed worker (confirmed by reading; tests only).**

- **Where:** `slots.cpp:485-494`.
- **What happens:** it copies raw `worker*` pointers, releases `m_`, then waits on each worker. A
  concurrent `detach` joins the worker and destroys its `addon_entry`.
- **Impact:** `drain()` is called only from tests (grep). It is a latent trap if a shell starts
  using it.

**L3. A native add-on that unloads itself from its own slot worker could unmap running code
(suspected).**

- **Where:** `slots.cpp:442-446`, `host.cpp:1702-1717`.
- **What happens:**
  - `detach` from inside the add-on's own worker detaches that thread so it can finish its call.
  - If that happens inside `~loaded_addon`, `lib_.close()` follows at once and unmaps the code the
    detached thread is returning through.
- **Why only suspected:** the comment names scripts as the case; I could not build a concrete path
  for native add-ons.
- **Fix:** have the loader refuse, or defer the `dlclose`, when it is destroyed on the add-on's own
  worker thread.

**L4. `catalogue_port` trusts the add-on's reply and holds a lock across its calls (confirmed by
reading).**

- **Where:** `catalogue_port.cpp`, the `json` lambda and `publish` / `json`.
- **What happens:**
  - The reply is built with `std::string(buf.data())`, which trusts the add-on to NUL-terminate
    inside `buf`.
  - The process-wide `lock_of()` mutex is held across `phone_publish` and `phone_json`, so one slow
    add-on call blocks every other caller.
- **Fix:**
  - Build the string as `std::string(buf.data(), strnlen(buf.data(), buf.size()))`.
  - Copy the provider out under the lock, then call it outside the lock.

**L5. Remove matches on the id in any manifest, signed or not (confirmed by reading; by design).**

- **Where:** `store.cpp:328-339`.
- **What happens:** "Remove X" deletes every top-level folder whose manifest in any version
  (unverified) claims id X. A local tamperer can point another add-on's folder at X so removing X
  deletes it too.
- **Impact:** local only. Noted because it is the one place unsigned bytes drive a destructive action.

**L6. `open_store::theme_json` reads a theme without hashing it again (confirmed by reading).**

- **Where:** `open_store.cpp:405-406`.
- **What happens:** the theme is read by path, and its integrity rests on `find()`'s cached
  size + mtime snapshot. `verified_file` (used for scripts) hashes the bytes it returns, so the two
  paths disagree.
- **Impact:** low, because the theme parser is strict.
- **Fix:** use `verified_file`.

**L7. `script_host::find` returns a raw pointer after unlocking (confirmed by reading).**

- **Where:** `script_host.cpp:243-249`, used by the spin rig at `:119-131`.
- **What happens:** the pointer is only safe because `restart` and `stop_all` call `stop_spin()`
  first. It is a fragile contract for a dev/verify rig.

**L8. Open manifests are parsed before their signature is checked (confirmed by reading; documented).**

- **Where:** `open_manifest.cpp:204`.
- **What happens:** up to 256 KB of attacker JSON is parsed (depth limit 32, `json.h:383`) before the
  signature check, because the key is inside the document.
- **Impact:** acceptable. Listed only because the first-party path's "signature first" rule does not
  hold here.

---

## Improvements and optimisations

1. **Startup verification cost.**
   - `store::find(id)` runs `peek()` on **every** add-on folder, which reads the manifest and checks
     its Ed25519 signature, then calls `best_in`, which verifies files (`store.cpp:201-217`).
   - `list()`, `load()` and each `piece_dir` call repeat this, so a launch does O(add-ons ×
     lookups) signature checks.
   - The first `verify_files` per process still hashes every byte. For the AI pack that is
     gigabytes on every launch; the comment at `manifest.cpp:756-761` notes it was 6 GB before
     deduplication.
   - Suggestions:
     - Memoize `peek` per (path, size, mtime).
     - Persist the verified snapshot across launches in an app-written file. Include the file id or
       inode, and accept it only for non-code files, so model weights are not re-hashed while code
       still is.
     - On macOS, rely on library validation for Mach-O files and hash only the data files lazily,
       when the pack first opens them.
2. **Hold the file open while verifying, then load** (the M1 fix). It also closes the race for
   `tell_removed` and `forget_outside_data`, which load a library just to call one export.
3. **Version high-water mark per id** (the M2 fix). `publisher.json` already sets the pattern of an
   app-written record outside the version folders.
4. **Remote cache file names:** store `<hash>.<ext>` (the M3 fix). The path then no longer depends
   on server-supplied names at all.
5. **Windows script log:** script errors are dropped (`script_abi.cpp:117-118`), so Windows users
   only see the last error. A small per-add-on ring buffer would match the Mac's diagnosability.
6. **`trim_locked`:** O(n) per admit (L1).

---

## Test gaps

- **Windows load integrity.** Nothing tests a DLL swapped after verification:
  - a same-size swap with the mtime restored, in the same process;
  - a swap between `verify_files` and `shared_library::open`.

  (`test_addon_manifest.cpp:194-197` covers the same-second tamper, but not a restored mtime.)
- **Rollback.** Missing cases:
  - an older signed package over a *tampered* newer install (expected to be accepted today; decide
    the policy);
  - a fresh install of a version older than one installed earlier.
- **Remote cache:**
  - no test of `disk_cache` names containing `:`, device names, or a trailing dot or space;
  - no test of `remove_server`;
  - no test of `trim_locked` with `keep_recent_` and many entries;
  - only `test_desktop_client.cpp` touches the cache.
- **`parse_key` / `good_rel`:** `:` is not rejected, and no test pins down the intended rule.
- **Developer key guard:** no check that a release packaging path fails when
  `MV_ADDON_DEV_PUBLIC_KEY` is set.
- **Script sandbox.** The existing tests are good (`test_script_runtime.cpp:182-744`: environment,
  bytecode, metatable, gc, limits, C loops, `__gc`, weak tables). Missing cases:
  - a coroutine created in an earlier task (it keeps its own hook) still stops within the bound in a
    later task after a stop;
  - an `xpcall` message handler that loops after a `"stopped"` error;
  - `load(text, name, mode, env)` with a non-table env.
- **Slots:**
  - a concurrent `detach` with `drain()` (L2);
  - a native add-on destroyed from its own worker (L3).
- **`catalogue_port`:** a provider that returns a buffer with no terminating NUL.
- **Open store:** two `install` calls of the same id at the same time (staging is unique, but the
  `remove_tree(target)` and rename can interleave). Suspected benign; untested.

---

# Driftwire audit (src/wire, src/addons/driftwire)

Read-only review, 2026-11-25, branch `claude/project-audit-performance-180749`. About 31k lines in scope
(`src/wire` 15.6k, `src/addons/driftwire` 15.6k, not counting `cli/`). I read the riskiest paths in full:
the handshake and rejoin (`channel.cpp`), the nearby and internet receive (`engine.cpp` `on_incoming`,
`route_busy`, `run_receive`, `offer_remember`, `on_beacon`), the receive write path (`transfer.cpp`
`parse_offer`, `receive_files`, `verifier`), `resume.cpp`, the path gate `wire.cpp:safe_relative`, and
the relay forward (`splice.cpp` `connect_host`). The rest I spot-checked with grep: serve, SMB hosting,
secrets, DHT, CPace and Noise.

Each item is marked **confirmed by reading** or **suspected**.

## Inventory

**Built**
- **Nearby transfer:** Wi-Fi TCP with UDP beacons and Bonjour (`advert_mac/win`), and Bluetooth (`bt_mac.mm`, `bt_win.cpp`).
- **Handshakes:**
  - Protocol 1: X25519 `crypto_kx` with commit-reveal.
  - Protocol 2: `Noise_XXhfs` X25519+ML-KEM-768.
  - Protocol 3: protocol 2 plus a server commitment (the #497 fix).
- **Sessions:** secretstream sessions with acks and a 16 MiB replay window. Rejoin and bearer moves use `NNhfs+psk0` with spent-tag replay protection.
- **Internet codes:** CPace (ristretto255) then `XXhfs+psk0`, over PCP / NAT-PMP / UPnP mappings, IPv6, or the person's own relay (`splice.cpp`, CPace on the server key).
- **Four-word DHT codes:** BEP 44 items, Argon2id per-hour salt.
- **Other transfer features:** remembered devices (hourly DHT knock, TOFU on the static), a DHT handover during a transfer, resume of a stopped receive (#530), and quarantine marks (`io::mark_from_elsewhere`).
- **`driftwire serve`** (the NAS library service): access-key-gated hello, pairing codes, owner passphrase with a persistent lockout, `*_beneath` file access with no links, change/backup roles, convert.
- **`driftwire host`** (self-hosting): port mapping, ACME, DDNS, LAN SMB shares on Mac and Windows through an elevated step.
- **Relay server and Tunnel forwarding:** ports 80/443, public destinations only.

**Stubbed or not built**
- No-op files: `bt_none.cpp` (Linux Bluetooth), `advert_none.cpp`, `host_smb_none.cpp` (Linux uses Samba), `acme_none.cpp`, and `tls_none.cpp` (builds without BoringSSL).
- Design doc §Remembered/§Handover lists two things as not built: a handover through a relay, and marks on nearby tiles.

## Status of already-filed items (not re-reported)

**#497.** Parts have landed:
- **Item 1 (grinding), partly fixed.** Protocol 3 adds the server commitment, revealed only after the client's nonce (`channel.cpp:399-486`). Handshakes that drop, and links that never offer, are counted: `engine.cpp:1294-1303`, `3203-3209`, `3286`. Five in 60 s refuse nearby links for 30 s.
  - **Residual:** the receiver still answers protocol 1 and 2 hellos from anyone (`channel.cpp:242`, `315-318`; `run_receive` calls `open_tracked(l,false)`). A machine in the middle can therefore say "2" (or "1") to the receiver and avoid the commitment. Only the global counter limits it now: roughly 5 tries per 90 s, so about 10^-5 per code window.
  - **Still to do (#497 item 4):** "refuse protocol 1/2 from an id seen at 3", and a setting to refuse old protocols.
  - **A test assumes the opposite:** `tests/test_driftwire.cpp:2353-2362` asserts that a protocol-2 send to a protocol-3 receiver works.
- **Item 2 (the receiver never checks the code): still open.**
  - `run_receive` goes straight to `read_offer` / `read_offer_after` after the channel opens (`engine.cpp:3281-3283`).
  - Nothing in `src/wire` or `src/addons/driftwire` sends or checks a code proof (grep: `code_proof` has no hits).
  - The receiver still shows `ch->peer_name()`, the self-declared ident (`engine.cpp:3228`, `3248`).
  - Quarantine (#498) is now set on every received temporary (`transfer.cpp:1023`). That softens the "malware drop" half but leaves the unauthenticated-offer half.
- **Item 3 (Remember after an unauthenticated receive): fixed.** The receiver must now type the check shown on the sender's screen before it can remember (`engine.cpp:920-929`, `remember_needs_check`).
- **Item 4 (beacons):**
  - Fixed: a beacon no longer lowers a peer's protocol (`engine.cpp:777`, `830`), and a fresh peer keeps its address (`765-768`).
  - Still open: names on beacons and in ident remain unauthenticated.

**#498, #499, #500, #501, #502 (closed).** The fixes I touched are in place:
- `safe_relative` (`wire.cpp:159-183`) rejects absolute paths, `..`, `\ : < > " | ? *` (so no NTFS alternate data streams), leading dots, trailing dot or space, device names, depth over 32, and bidi or zero-width characters.
- Offers must be one folder or bare files (`transfer.cpp:563-570`).
- Received files never overwrite: `free_name`, then `create_new` (exclusive create), then `rename_no_replace` (`transfer.cpp:1010-1019`, `800-808`).
- Folders are made with `make_directories_beneath`.
- The beacon on-link check (`engine.cpp:754`) and the accept on-link check (`engine.cpp:858`) are present.

**#540 (throughput)** covers the per-file fsync and read-back, the 256 KiB chunks and single-core sealing.

## Issues by severity

### Medium: anyone on the LAN can switch off nearby receiving, silently, with a few bare TCP connects (confirmed by reading)

**Evidence**
- `engine::note_unconfirmed` (`engine.cpp:1294-1303`) keeps a single global list. Five entries in 60 s set `refuse_nearby_until_ = now + 30 s`.
- `on_incoming` then refuses every nearby, non-remembered link (`engine.cpp:1317-1321`). That includes the person's real sender.
- It counts every failed nearby `open_tracked` (`engine.cpp:3203-3209`). That includes a connection that never sends a hello: `channel::open` → `read_hello` fails → error (`channel.cpp:288-292`).
- A connection that only sends 5 junk bytes counts too.
- The only trace is `host_.log` (`engine.cpp:1302`, `1319`). The window shows nothing.

**Attack:** a host on the same Wi-Fi opens and closes 5 TCP connections to the Driftwire port every 30 s. Every nearby send to that machine is refused, and the person sees only that the other computer "can't reach" it. Network inventory scanners (Fing, IT asset scanners, `nmap -sT`) trip it by accident.

**Fix:**
- Count only failures after this side has done key work, meaning a valid hello that dropped after message 2, or an opened link that never offers. A link that fails before or at the hello gives an attacker nothing and should be limited per source.
- Rate-limit pre-hello failures per address with the existing `receive_limit_`.
- Show "Someone nearby keeps trying to connect" on the receiver, as #497 itself asks.

### Low–Medium: a nearby peer's identity key is a claim, not a proof, and resume records and Remember are keyed on it (confirmed by reading)

**Evidence**
- Nearby, the Noise static is a key for that link only (#501, `engine.cpp:1504-1512`). The long-term identity travels as a plain field inside the sealed offer or accept: `"key"`, written at `transfer.cpp:224` and `639`, parsed at `576` and `313`.
- Nothing binds that field to a secret key. There is no signature over the handshake hash and no MAC derived from the identity's private key.
- `run_receive` uses it as the resume-record key: `sender = key_hex(*offer->key)` (`engine.cpp:3389-3400`).
- After any accepted receive that does not fail, `drop_unfinished(sender)` runs (`engine.cpp:3532`). That deletes the record and its partial temporaries (`engine.cpp:1096-1109`, `resume.cpp:142-149`).
- A failed receive instead replaces the record and discards the old one's partials (`keep_unfinished`, `engine.cpp:1068-1091`).

**Attack:** Mallory has met Alice before, so Mallory has Alice's identity public key: Alice's offers and accepts carry it. Mallory offers Bob any harmless file while claiming Alice's key, and Bob accepts.
- Bob's accept tells Mallory which of Alice's files Bob already has and how far the partial got (`"have"` / `"at"`, `transfer.cpp:630-650`). This is an information leak.
- When Mallory's transfer ends, Alice's unfinished transfer is deleted.

**What it cannot do:** splice data into Alice's file. The read-back hash covers the whole file (`transfer.cpp:793-794`), and Mallory does not know Alice's prefix bytes.

**Remember side:** `offer_remember` stores the claimed key with Mallory's pairing secret (`engine.cpp:902-918`). The internet TOFU check (`engine.cpp:3162`) stops it being used to impersonate Alice. It can still rename an existing entry.

**Fix:** prove the identity inside the channel. Either:
- sign `handshake_hash()` with an Ed25519 identity, or
- send `MAC(X25519(identity_sk, peer_link_static), h)`, checked against the claimed key.

Ignore `"key"` until it verifies.

### Low: an internet code dies at the first hello from anyone who reaches its port (confirmed by reading; by design)

**Evidence:** `run_receive` calls `in->spend()` for any well-formed internet hello on the code's way in (`engine.cpp:3190-3200`, `internet.cpp:72-87`). `route_busy` spends it too while a transfer runs (`engine.cpp:1468-1470`).

**Attack:** someone who knows the protocol and learns the mapped port can burn every code shown, for example by watching traffic or by scanning IPv6 neighbours. Only a DoS: no secret is learned, because CPace gives one online guess.

**Note:** this is the stated trade-off, and junk that is not a hello does not spend the code (`engine.cpp:3143`). Recorded so the denial is known.

## Checked and found sound (no finding)

- **Nonces and AEAD.** Every link and every rejoin starts fresh secretstreams with random headers (`channel.cpp:157-167`; rejoins via `NNhfs+psk0` with fresh ephemerals, `1347-1446`).
  - Noise `cipher_state` refuses n = 2^64-1 (`noise.cpp:135`, `153`).
  - Sequence numbers are inside the sealed header and gaps are refused (`channel.cpp:1194`).
  - No reuse of a key and nonce pair found.
- **Domain separation.** Each BLAKE2b use has its own context string (`channel.cpp:36-49`), and the prologue binds the protocol version and the hello kind (`channel.cpp:83-90`, `401`).
- **Constant-time compares.** `sodium_memcmp` is used for commitments, rejoin tags, typed codes, remember checks, pair codes and claim proofs.
- **Size limits on peer-supplied lengths:**
  - Frames: `kMaxMessageBytes` (`channel.cpp:1059`, `1149`).
  - Noise messages are at most 64 KiB (16-bit length).
  - Offers: up to 100k files, 128 parts, sizes summed without wrap (`transfer.cpp:530-560`).
  - A file's data cannot pass its declared size (`transfer.cpp:1031`).
  - Free space is checked before Accept (`engine.cpp:3401-3406`).
  - Resume records are capped at 32 MiB.
- **Resume.** It reopens only a temporary in the same directory, cut to the bytes known to be durable, and the read-back hashes the whole file (`transfer.cpp:994-1008`, `786-794`).
- **Relay forward.** It resolves once and connects to the resolved addresses that passed the public, not-on-link check, so there is no DNS rebinding (`splice.cpp:1051-1083`).
  - `is_public` excludes RFC 1918, CGNAT, link-local, 6to4, Teredo and non-2000::/3 addresses. NAT64 and IPv4-mapped addresses are not 2000::/3, so they are refused too.
- **SMB elevated step:**
  - It refuses system folders (`host_smb.cpp:66-101`).
  - It refuses paths reached through a link: `realpath` must equal the path given (`host_smb_mac.cpp:573`).
  - It requires the folder to be owned by the invoking user or to be on a separate volume (`host_smb_mac.cpp:570-585`).
  - Passwords are set through Open Directory, never on a command line.
- **secrets_file.** Writes use 0600, exclusive create with no-follow, fsync then rename, and the directory is created 0700 (`secrets_file.cpp:46-71`, `143-147`).
- **serve.**
  - All file access goes through `*_beneath`, with no symlinks followed.
  - A wrong owner passphrase is counted before it is checked and saved durably, and answers are spaced out (`serve.cpp:2376-2421`).
  - Pairing codes have 32 hex characters (128 bits), compared with `sodium_memcmp` (`serve.cpp:250`, `4079`).
- **DHT words.** 44 bits, but Argon2id at moderate cost (256 MiB) with an hourly salt and a 10-minute life (`dht_item.h:7-13`, `dht_item.cpp:101-123`). An offline search of the published item key is not feasible within the code's life.

## Improvements

- **Merge the duplicated single-frame readers.** "Read one sealed frame of exactly N bytes and check its type" is written twice: the protocol 3 `code_nonce` read (`channel.cpp:468-479`) and `finish_rejoin` (`channel.cpp:1309-1321`). One helper would leave one place to get the length check and the wipe right.
- **Show the protocol on the receiver.** `v_.in_protocol` is already set (`engine.cpp:3253`). Showing "older, less protected connection" when it is below 3 would turn the residual #497 downgrade into something the person can see before a setting to refuse it exists.
- **Free the transfer slot sooner.** On a nearby link that sends nothing, `run_receive` holds `transfer_running_` for up to `kConnectMs` (8 s, `wire.h:57`). Meanwhile a real sender is pushed to the busy path, which does a full ML-KEM handshake just to say "busy" (`engine.cpp:1487-1491`). Reading the hello with a short deadline before marking the transfer as running would cut both the DoS window and the CPU per junk connection.

## Test gaps

- **Pre-hello connects:** no test that bare TCP connects or junk bytes do not lock out nearby receives. The existing #497 test (`tests/test_driftwire.cpp:2317`) covers links that never offer, and with today's behaviour a pre-hello-connect test would fail.
- **Identity claims:** no test of an offer whose `"key"` is someone else's. It should neither take up nor drop that sender's resume record (`tests/test_driftwire_resume.cpp` covers matching only).
- **#497 item 2:** no test for a receiver-side code proof. That is expected until it is built; the issue already lists the cases.
- **Downgrade:** `tests/test_driftwire.cpp:2353-2362` pins the old "a protocol 2 sender works with a protocol 3 receiver" behaviour. It has to change when the downgrade is refused or flagged, and nothing yet tests that the receiver flags protocol 2.

---

# Audit: src/addons/ai, src/infer, src/addons/enhance

Read-only audit, 2026-11-25, on branch `claude/project-audit-performance-180749`. I prioritised with grep, so I read
only the hot spots, not all ~48k lines. Every line reference below is code I read. I mark each finding
**confirmed** (the code path was read end to end) or **suspected** (the code was read, but the impact was not
measured or not traced all the way).

Out of scope, cited instead: #390 (inference out of process), #85, #355, #608, #602, #603, #600, #298/#297,
PR #693 (Settings attaching without loading models, Clear draining workers).

## Inventory

### src/addons/ai (Local search)
| Area | State |
|---|---|
| Index DB (SQLite WAL, schema 12, migrations 1→12, FTS5 or a plain fallback) `index_db.cpp` | Built |
| Picture embedding (CLIP towers, tiers high/fast, Core ML/CUDA/OpenVINO with CPU fallback, self-test, upgrading tower) `pack.cpp` | Built |
| Brute-force int8 vector scan, tombstoned removals, "nothing found" gate `vectors.cpp` | Built (no ANN, as designed) |
| Query parsing (filters, number forms, tags, places) `query.cpp` | Built |
| Faces/People (YuNet+SFace, clustering, refine, consolidate, settle) `faces.cpp`, `face_refine.cpp` | Built |
| Audio (CLAP sounds, Whisper speech), clip cues/subtitles | Built (audio files are #355) |
| OCR: Vision on Mac (`ocr_mac.mm`), Windows.Media.Ocr (`ocr_win.cpp`) | Built; Linux/iOS are stubs (`ocr_none.cpp`) |
| Documents: PDF via a PDFium dylib (`pdf_pdfium.cpp`), DOCX via the host, passages, passage vectors, scanned pages | Built; iOS `pdf_none.cpp` |
| Model downloads (`fetch.cpp`, WinHTTP/NSURLSession transports) | Built Win/Mac; Linux/iOS `fetch_none.cpp` (stub) |
| Photos library source (`photos_mac.mm`) | Mac only; others `photos_none.cpp` |
| Cloud placeholders (OneDrive `cloud_win.cpp`, iCloud Drive `cloud_mac.mm`) | Win/Mac; others `cloud_none.cpp` |
| Index sharing `.mvindex` export/import (`transfer.cpp`) | Built, with validated input |
| Phone reader (immutable read-only open, catch-up) | Built; iOS indexes nothing (`cmake/ai.cmake:203-207`) |

### src/infer
ORT dynamic load on Win/posix, static on iOS (`ort_dylib_*.cpp`, `ort_static_ios.cpp`). Built: sessions with
CPU/CUDA/OpenVINO/Core ML (`ort.cpp:235-347`), CLIP/YuNet/SFace preprocessing (`preprocess.cpp`), CLIP BPE and
WordPiece tokenizers, text models and audio features/models. No stubs found.

### src/addons/enhance
Built: model cards, downloads (verified, resumable), job runner with sound, picture/video tiles, still, face restore,
remove/extend (inpaint), restyle (SD-style diffusion: text encoder, VAE, UNet per size), preview, ETA, prefs,
memory probes (Win/Mac/Linux). Restyle is refused where the UNet would run on the CPU: Windows ships the CPU ORT
build (`runner.cpp:704-710`). Outputs go only through host writers (`host_port.cpp:90-276`: `still_write`,
`sound_write_*`, `clip_write_*`). The add-on writes no files of its own except `enhance.json` (`prefs.cpp:136-145`)
and downloads. Discard recycles only the result, never the source (`engine.cpp:1247-1263`), and a test covers it
(`test_enhance.cpp:1699-1703`).

## Issues by severity

### High

**H1. INDEX_FULL is sticky: removing folders never resumes indexing, and the size check counts free pages.** *Confirmed.*
- `engine.cpp:1213`: `if (s.index_cap != 0 && bytes > s.index_cap && !index_full_) index_full_ = true;`. Nothing in
  `refresh_counts` ever sets it back to false.
- Only two places clear it: a change to `index_cap_bytes` (`engine.cpp:853`) and Clear all (`engine.cpp:5510`).
  Removing a folder (`engine.cpp:5360-5409`, through `db_->remove_root`) does not clear it.
- `index_db::bytes()` (`index_db.cpp:1871-1878`) is `page_count * page_size`. That includes freelist pages, and the
  database has no `auto_vacuum` and runs no `VACUUM` except in `clear()` (`index_db.cpp:1671-1673`). So even if the
  flag were re-checked, deleting a folder's rows would not lower `bytes`.
- Docs (`17-local-ai-search.md`, "Disk") give per-folder delete as the remedy for the cap. Today it does not work:
  the user must raise the cap or clear the whole index. `faces.db` is added by file size (`engine.cpp:1205`) and
  has the same problem.
- Fix: clear `index_full_` whenever `bytes <= cap`. Measure with `(page_count - freelist_count) * page_size`, or run
  `VACUUM` / `incremental_vacuum` after `remove_root`. Test gap: `test_ai_engine.cpp:2282-2285` only checks that
  the flag turns on.

**H2. Every AI model stays resident for the whole session after indexing finishes.** *Confirmed by reading; size impact not measured.*
- `load_models` (`engine.cpp:1444-1660`) installs `build_`, `answer_`, `faces_model_`, and later sound, speech
  and docs. The control loop's idle branch (`engine.cpp:2613-2637`) does nothing with them except People
  consolidate/settle. No code anywhere unloads a model when `queue_exhausted_ && busy_workers_ == 0`.
- Once the library is indexed, a search needs only the answering tower's text encoder (`query_vector`,
  `engine.cpp:8249-8269`). Find-similar needs the image tower only when the item is not indexed. Yet the image
  tower (Core ML/CUDA session plus the ORT CPU arena), CLAP, Whisper, YuNet/SFace and the docs text model all stay
  loaded until quit.
- During a migration (`engine.cpp:1563-1605`) two towers are resident, which is by design.
- This is a different problem from PR #693, which is about Settings and Clear.
- Suggested: after N minutes idle with the queue exhausted, `retire()` every model except the answer tower's text
  side, and reload on the next work (the `reload_models_` path already exists). Each model should have a "loaded"
  test in `test_ai_engine.cpp`.

### Medium

**M1. The ORT CPU arena is never shrunk, so peak tensor memory stays reserved.** *Suspected (impact not measured).*
`ort.cpp:248-260` builds session options with the default CPU memory arena and `CreateCpuMemoryInfo(OrtArenaAllocator…)`
(`ort.cpp:315`). Nothing sets `memory.enable_memory_arena_shrinkage` in run options, and nothing disables the
arena for dynamic-shape models. The grep for `arena|shrink|RunOptions` in `src/infer` found nothing else. For
Whisper, CLAP and the text tower (variable lengths), and for Enhance tiles and diffusion, each session keeps its
high-water mark. Combined with H2, the indexing peak becomes the idle footprint.

**M2. Enhance Cancel blocks the add-on's slot worker until the job thread exits, and some phases cannot be interrupted.** *Confirmed path; duration not measured.*
- `job_runner::cancel` (`runner.cpp:313-323`) sets the flag and then `join()`s the job thread. It is called from
  `engine::cancel` (`engine.cpp:1237-1245`) on slot events (`engine.cpp:1352-1359`, `1400-1401`) and when one-click
  moves to another photo (`engine.cpp:822-831`). Those slot calls run on the host's per-add-on worker, one at a
  time (`mediaviewer_addon.h:555-558`).
- `run_restyle` checks `cancel_` only before `load_restyle` (`runner.cpp:691`), not during it. It then calls
  `prepare()`, which may compile a UNet on Core ML (`diffusion.cpp:172-191`, `runner.cpp:694-707`). Sessions for
  remove and face are opened the same way.
- During those minutes every Enhance slot event queues behind the join: the sheet does not update, and Cancel
  looks hung. The UI thread is not blocked.
- Fix: signal cancel and return. Let `ev.done` report the cancellation, and join on the next `start()` (which
  already joins, `runner.cpp:308`) or on shutdown.

**M3. Untrusted PDFs are parsed in process, and the time cap is checked only between pages.** *Confirmed by reading; I could not construct a concrete exploit.*
- `pdf_pdfium.cpp:146-171` (and `look`, `184-189`) loads each page and its text under a process-wide
  `pdfium_mutex()`. `max_seconds` is checked only before each page.
- Attack scenario: a PDF in an indexed folder (Downloads is a common root) triggers a PDFium parser bug. That
  either crashes the viewer, which holds Photos and folder access, or hangs one page forever. A hang holds
  `pdfium_mutex`, so every other worker's PDF read stalls too, and `stop()` waits on that worker.
- Limits exist: file size, `max_pages`, and 256 K characters per page (`pdf_pdfium.cpp:159`, `230`). JavaScript
  does not run during text extraction.
- This is related to #390 (out-of-process inference); a document parser in that helper would cover it. Cite #390
  rather than opening a separate issue unless #390 is limited to ORT.

### Low

**L1. `pending()` rescans every asset on each queue refill.** *Suspected (not measured).*
`index_db.cpp:861-888`: the query joins `assets × roots`, left-joins `progress` by primary key, filters with
`CASE r.media …` expressions, and sorts by `COALESCE(p.state,0)=1 DESC, a.kind, a.id`. No index can satisfy that
sort. It runs under `work_m_` (`engine.cpp:4126-4144`) for each of about nine tracks per refill of 256, so a
200 k-asset library does a full scan plus a sort about every 256 items while other workers wait to claim. A
partial index on undone work, or a cursor on `a.id` (`a.id > last`), would make a refill O(batch). Similarly,
the People refill (`engine.cpp:4200-4218`) walks all of `assets_` under `models_m_` + `assets_m_`, and `models_m_`
is also what search takes to read `answer_`.

**L2. `frames_spec` is a redundant index.** *Confirmed.*
`index_db.cpp:190` `frames_spec ON frames(spec)` is a prefix of `frames_spec_asset ON frames(spec, asset_id)`
(`index_db.cpp:193`). Each frame insert and delete (the largest table) maintains one extra B-tree for no read
benefit. Drop it in the next schema step.

**L3. A failed dehydrate leaves a cloud file local while it is marked online-only.** *Suspected.*
`engine.cpp:3714-3718`: `(void)cloud_->dehydrate(c.file); (void)db_->set_cloud(c.id, true);`. If
`CfDehydratePlaceholder` fails (`cloud_win.cpp:80-87`: the file is in use, pinned by the user while it was
hydrated, or not yet synced), the index still marks the file cloud. It will not be read again until the next
scan, and the bytes stay on disk. This never touches the user's content. The risk is quiet disk use and a stale
mark. Only an id is persisted for crash give-back (`engine.cpp:3750`), which follows the privacy rule.

**L4. A one-second wake while idle.** *Confirmed.* The control loop (`engine.cpp:2649-2658`) waits at most 1000 ms
and calls `phone_tick` and `reap_retired` each time, even when nothing is indexing. This is cheap per wake, but
it is a permanent wakeup on battery. Use a longer timeout when idle and not on a phone.

### Checked, no issue found
- **Model downloads** (`fetch.cpp:95-198`): the file is written to `.part`, capped at the pinned size
  (`fetch.cpp:140`), SHA-256 verified before the rename (`fetch.cpp:172-181`), and a mismatch is deleted (`182`).
  The `.ok` stamp records hash, size and mtime.
- **Download URLs** must be `https://huggingface.co/…/resolve/…` with no `/../` (`src/addon/manifest.cpp:57-68`).
  Remote `path`s pass `safe_relative_path` (`manifest.cpp:510-524`, `647`), and the manifest is signed. Enhance
  rechecks the hex sha and size cap (`downloads.cpp:31-66`), and checks free space first (`downloads.cpp:133-147`).
- **Mac transport**: ephemeral session with no cookies or credentials, back-pressure at 64 chunks
  (`fetch_mac.mm:80-82`), and a 60 s timeout.
- **`.mvindex` import**: read-only and `SQLITE_DBCONFIG_DEFENSIVE` (`transfer.cpp:216-220`). `safe_rel` rejects
  `..`, absolute paths, `\`, `:` and NUL (`transfer.cpp:148-160`), and embedding sizes are checked
  (`transfer.cpp:764`, `868`).
- **Orphan rows**: assets that are gone cascade to frames, progress, speech, cues, passages and passage_vecs
  (FKs + `foreign_keys=ON`, `index_db.cpp:166-197`). FTS rows go through triggers (`267`, `371`), and faces through
  `forget_asset` (`engine.cpp:3451`, `3540`, `3563`, `5406`).
- **Enhance models** are loaded per job and freed when it ends (`runner.cpp:350`, `413`, `694`: locals). Only the
  ORT runtime holder persists (`host_port.cpp:110`). The restyle UNet-per-size cache lives only for the job
  (`diffusion.cpp:167`).
- **Decoding** is done once per photo for picture + text + faces (`engine.cpp:4629-4676`). The text-query cache
  holds 64 entries (`engine.cpp:8257-8267`).
- **Original files**: neither AI nor Enhance writes user files. AI writes only its index, caches and models.
  Hydration/eviction changes only a placeholder's local state.

## Improvements (not bugs)
1. Unload models on idle (H2), and enable arena shrinkage or turn off the CPU arena for dynamic-shape sessions (M1).
   Measure RSS before and after on the owner's library.
2. Make the "disk" number report live data: `(page_count - freelist_count) * page_size`, plus `PRAGMA
   incremental_vacuum` after folder removal (would need `auto_vacuum=INCREMENTAL` set at the next schema step, with
   one `VACUUM` to apply it).
3. Make Enhance cancel asynchronous (M2), and check `cancel_` between restyle's session opens.
4. Use cursor-based `pending()` (L1) and drop `frames_spec` (L2).
5. Move the PDFium read into the #390 helper process when it lands (M3).

## Test gaps
- No test that removing a folder (or lowering usage below the cap) clears `MV_AI_STATUS_INDEX_FULL`
  (`test_ai_engine.cpp:2282` only turns it on).
- No test that models are released after indexing goes idle (needs a fake `deps_` counting live models).
- No test that Enhance `cancel()` returns promptly while a job is in model load or `prepare()`. Existing cancel
  tests (`test_enhance.cpp:832`, `3340`, `4212`) cancel between tiles or chunks.
- No test of a crafted `.mvindex` whose `rel` holds `../` or `\` (only "damaged file", `test_ai_engine.cpp:6177`).
  `safe_rel` is right today, but nothing pins it.
- No unit test for `index_db::bytes()` after deletes.
- No test of a PDF page that exceeds the time cap (the cap is not enforced inside a page).

---

# Audit: Import, Phone access, Places, VPN add-ons

Read-only audit of `src/addons/{import,phone,places,vpn}` at worktree `relaxed-mclean-5d04bd`
(main @ a232bda4). Where the add-on hands work to shared code (`src/io/verified_copy.cpp`,
`src/wire/*`), I read the parts that decide the add-on's safety and say so.
"Confirmed" means I read the code path end to end. "Suspected" means I read the code but the
outcome depends on OS or peer behaviour I could not check without running it.

Already filed, so not repeated here: #686 (phone add-on / SMB / RAID follow-ups), #615 (MTP/PTP),
#194 (macOS Import perf/a11y), #611, #599. #497 (Driftwire six-digit code) is a different
code path: Phone pairing uses 128-bit single-use codes (`src/wire/serve_wire.h:345`,
`src/wire/serve.h:236`).

---

## Inventory (built vs stubbed)

### Import (`src/addons/import`, about 6.8k lines)
| Feature | State |
|---|---|
| Card/folder scan, "new since last import" from card memory | Built (`engine.cpp:583-640`) |
| Planner: dated/card/flat layouts, rename templates, `{seq}`, clash names | Built (`naming.cpp`, `planner.cpp`) |
| Verified copy: hash-on-read, `.mvtmp` temp, `F_FULLFSYNC`/fsync, uncached read-back, exclusive rename, one retry | Built (`src/io/verified_copy.cpp:401-606`) |
| Units (RAW+JPEG+sidecar) all-or-nothing, with rollback | Built (`engine.cpp:1218-1259`) |
| Backup destination written from the same read | Built |
| Journal plus resume after crash or card pull, remounted volumes | Built (`engine.cpp:1599-1762`) |
| Retry failed, verify a folder, find duplicates, trash a duplicate (bin only, re-hash of the kept copy) | Built |
| Eject after import (never format or erase); auto-import per card | Built |
| Move (delete the source after copying) | **Not present, on purpose.** Nothing in the add-on deletes a source file. The only deletion is a duplicate the user picks, which goes to the bin (`engine.cpp:2303-2433`). |
| MTP/PTP sources | Not built (#615) |

### Phone access (`src/addons/phone`, about 9.7k lines)
| Feature | State |
|---|---|
| NAS claim, owner passphrase, unlock/lock, timed or guest phones, revoke queue with backoff | Built (`link.cpp`, `desk.cpp`) |
| Pairing code and QR. The code is minted by the host/NAS (128-bit hex, single-use, hashed at rest, 300 s TTL). | Built (`link.cpp:1309-1360`, `qr.cpp`) |
| Nearby catalogue service on the LAN (Bonjour, LAN-only, at most 4 sessions) | Built (`desk.cpp:964-1011`) |
| Hand the library to another desktop (takeover) | Built |
| This computer as the server: bundled `driftwire` program, share folders, DuckDNS, start at login, users, SMB share, drives (combine, replace, copy) | Built as a driver of the `driftwire host …` CLI (`self_host.cpp`, `link.cpp:1700+`). The RAID/SMB follow-ups are #686. |
| Phone backup folders (limit, reserve) | Built (request surface, `link.h:104`) |
| Host tray / menu presence | Built (`host_tray.cpp`) |

### Places (`src/addons/places`, about 3.4k lines)
| Feature | State |
|---|---|
| Metadata-pane place section, offline map, zoom steps | Built (`places.cpp`, `map.cpp`) |
| Search by place, route of a clip (#606), folder map (#601), world map window | Built |
| Geocoding | Built in `src/geo` (out of scope). The add-on makes no network calls. |

### VPN (`src/addons/vpn`, about 5.8k lines)
| Feature | State |
|---|---|
| Tunnel: local SOCKS5/HTTP proxy on 127.0.0.1 to the relay (CPace plus Noise in `wire/splice`) | Built (`tunnel.cpp`) |
| "Use for all apps": the system PAC URL on macOS (SystemConfiguration with admin rights) and on Windows (WinINet per-user LAN setting) | Built (`sysproxy_mac.cpp`, `sysproxy_win.cpp`). Linux has none (`sysproxy_none.cpp`). |
| All traffic (VPN), using a userspace TCP/UDP stack (`netstack.cpp`) | Built. macOS needs the Network Extension entitlement, which is still owed (`vpn.h:13`). Windows needs the app to run elevated and needs Wintun (licence call pending) (`vpn_win.cpp:15-18`). Linux has none (`vpn_none.cpp`). |
| Kill switch ("Block when the tunnel is down") | Windows only, through WFP (`vpn_win.cpp:174-236`). **Not on macOS** (`vpn_mac.mm:213`, `includeAllNetworks = NO`). |
| Relay key in the OS secret store; prefs file keeps the address only | Built (`service.cpp:245-285`). Falls back to the file (see V3). |

---

## Issues by severity

### High
None found. The core import data-safety design holds up well:
- A name that is already taken is never overwritten: there is a stat check up front (`verified_copy.cpp:560-570`) and an exclusive rename, `renamex_np(RENAME_EXCL)` or `renameat2(RENAME_NOREPLACE)` (`file_port_mac.cpp:414-424`).
- Read-back is uncached and runs after `F_FULLFSYNC`.
- A cancel or error removes the temporary file (`verified_copy.cpp:450-491`).
- Units roll back as a whole (`engine.cpp:1218-1259`).
- No source is ever deleted.

### Medium

**V1. Windows "All traffic" leaks DNS, even with Block on.** Confirmed by reading; leak behaviour suspected (Windows).
- `vpn_win.cpp:27-28` relies on "the lowest interface metric" to make the tunnel's resolver win. I found no NRPT rule, no disabling of Smart Multi-Homed Name Resolution, and no WFP rule for port 53 (no `nrpt`, `dns` or `53` handling beyond the comment).
- Windows 8 and later send queries to every interface's DNS server in parallel, so names go to the LAN router or the ISP's resolver.
- The kill switch does not close this. `vpn_win.cpp:200-221` permits every address in `plan.exclude4/6`, and that list includes the local networks, on every port. The usual DNS server, the home router at 192.168.x.1:53, therefore stays reachable outside the tunnel.
- The tunnel's own design goal is that "the network's DNS sees none of them" (`tunnel.h:15-16`).
- Attack scenario: a person on a filtering network (the stated use case) turns on All traffic plus Block. The network's resolver still sees every hostname they visit, and can block them or log them.
- Fix: add WFP block filters for remote port 53 (and 853) on non-tunnel interfaces, placed above the local-network permit. Optionally add an NRPT "." rule pointing at the tunnel resolver.

**V2. The system proxy is changed before its undo record is written. A crash in between, or a failed write, leaves the PAC URL behind with no record.** Confirmed by reading.
- `sysproxy.h:18-20` says the record is kept "before anything else".
- In fact `service.cpp:589` calls `sysproxy_->set(url)` first, and only then `write_proxy_record()` at `service.cpp:595-596`.
- `save_record` removes the old file before creating the new one (`sysproxy_record.cpp:44-51`), and a failure is only logged (`service.cpp:518`).
- If the process dies between those steps, or the record write fails, every enabled macOS network service (or the WinINet LAN setting) keeps `http://127.0.0.1:<port>/…pac` and no later run restores it. The startup reconcile only acts when a record exists (`service.cpp:72-77`).
- Effect: every app that follows system proxy settings tries a dead PAC URL on each network change. Most fall back to DIRECT after a timeout, which means slow first loads and a confused user. The user has to find "Automatic proxy configuration" by hand.
- Fix: write a provisional record (`pac_url` with `saved` = "pending") before `set()`, then replace it. Or make `set()` return what it read first and save that before writing. A test with a `fake_port` whose `set` "crashes" would catch it (see Test gaps).

**I1. Resuming an import hashes whole files on the control thread with no cancel. App quit, scans and plans all wait for it.** Confirmed by reading.
- `resume_on_control` runs on the control thread (`engine.cpp:1595`).
- For every pending row whose final name already exists at the expected size, it hashes the source and every target with an empty `copy_callbacks none` (`engine.cpp:1708-1724`). Through `host.cpp:13-16` an empty callback means "never cancelled".
- `shutdown()` sets `cancel_control_` and then calls `control_.join()` (`engine.cpp:410-421`). That flag is not consulted here, so quitting during a resume of a job with large video files waits for two full reads per file.
- On a card read at about 90 MB/s, a 2-minute resume of a few 4K clips blocks quit for that long. Card arrivals, scans and plan rebuilds queue behind it on the same thread.
- Fix: pass a `copy_callbacks` whose `cancelled` reads `cancel_control_` (and `yield`). Better, move the reconcile onto the job's own thread at the start of `run_job`, which already holds the device lock.

### Low

**V3. The relay key can fall back to a world-readable prefs file.** Confirmed by reading.
- With no store, or when `secrets_->put` fails (for example a locked keychain or a denied ACL prompt), `save_now` keeps `host#key` in the prefs file (`service.cpp:265-284`). The failure is silent.
- `write_whole` creates that file with `O_CREAT|O_EXCL, 0644` (`prefs.cpp:81-97`, `file_port_mac.cpp:612`).
- On Linux homes (often 0755), and on shared Windows profiles with loose ACLs, another local user can read the relay key and use the person's relay. On macOS the home folder's 0750 mostly protects it.
- Fix: create it 0600, and show a note on the page when the key could not go into the store.

**V4. The macOS packet-tunnel provider trusts whatever is listening on the saved loopback port.** Suspected: needs a run to confirm System Settings behaviour.
- The add-on authenticates the provider with a 128-bit token compared using `sodium_memcmp` (`vpn_mac.mm:166-168, 260-264`). The provider does not authenticate the add-on: it connects to `127.0.0.1:<port>` from `providerConfiguration` and starts streaming every packet (`PacketTunnelProvider.swift:58-75`).
- After an app crash the configuration stays saved and enabled. `stop_locked` only runs on a clean exit (`vpn_mac.mm:224-247`).
- Attack scenario: on a multi-user Mac, another local account binds the stale port. The owner later clicks Connect on "MediaViewer VPN" in System Settings, and the attacker receives the token and all of the owner's IP traffic in cleartext.
- Requires a local attacker plus a user action, hence Low.
- Fix: have the provider check a token from the add-on as well (a challenge in both directions), or have the add-on remove the configuration (not just disable it) at its next start.

**V5. The PAC script fails open, and DNS may leak in some browsers.** By design, but worth a note in the UI.
- `pac_script` returns `"SOCKS5 127.0.0.1:port; DIRECT"` (`tunnel.cpp:234`). When the relay is down or blocked, browsers silently go direct, so "Use for all apps" gives no indication the protection is off.
- Suspected: some clients resolve names locally for a SOCKS entry in a PAC. Safari through CFNetwork, and Firefox with `network.proxy.socks_remote_dns` at its default, may do this, which leaks names to the network resolver despite `tunnel.h:15`.
- I could not verify per-browser behaviour without running them.

**P1. The `host users apply` stdin protocol does not escape tab or newline.** Confirmed by reading this layer.
- `apply_lines` writes `name + "\t" + password` per line (`self_host.cpp:613-621`). `link.cpp:1087` passes the typed password through unchecked.
- A password containing a newline is split. The user ends up with a different password than the one typed, and the remainder is parsed as another "name\tpassword" line by an elevated step.
- The owner is the only one typing here, so I could not build an attack across a trust boundary. It is a correctness and robustness issue.
- Fix: reject control characters in the name and password before the step (or have the program do so; I did not check its side).

**P2. Secrets linger in request strings.** Confirmed by reading.
- Passphrases, the DuckDNS token and user passwords arrive inside the request JSON, which sits in `requests_` (a `std::deque<std::string>`, `link.h:264`) and in `run(request)`. Neither is wiped.
- Only `relay_line` and the parsed password vectors are zeroed (`link.cpp:50`, `1089`). This contradicts the header's "never kept here" (`link.h:133`) only in spirit, because nothing is persisted.
- Minor: a core dump would contain them, and crash reports must not.

**P3. A disabled-then-re-enabled Phone add-on can run two link threads against the same `link.json`.** Suspected.
- `stop_app` detaches the old link's thread (`link.cpp:234-235`, `service.cpp:143-163`) and the next `resume_app` makes a new link.
- The old thread may still be in `quit_tidy` → `save_flags()` (`link.cpp:257`) while the new one runs `load_flags()` (`link.cpp:268-290`), which fails closed on an unreadable file and then stops serving nearby.
- Fails safe; only a hiccup.

**P4. Nearby service identity is random per start and not pinned** (`desk.cpp:974-975`).
- Integrity rests on the catalogue signature, so a LAN impersonator cannot forge content.
- Whether such an impersonator can complete a session depends on whether the wire handshake mixes in `access_key`, which every paired phone, including revoked ones, has learned. That code is in `src/wire` (out of scope).
- Worth checking whether revoking a phone rotates `access_key`. If it does not, a revoked phone on the LAN can impersonate the nearby service, or serve an older but still validly signed generation (rollback). Suspected; I could not construct the full attack within scope.

**I2. A paused import holds the source and destination device locks indefinitely.** Confirmed by reading.
- `cb.yield` sleeps in 5 ms steps while paused (`engine.cpp:1086-1093`), inside `host_.copy`, while `source_lock` (`engine.cpp:1072`) and `dest_locks` (`engine.cpp:1147-1154`) are held.
- A second job reading the same card, or writing to the same backup disk, blocks until the first is resumed.
- The 5 ms poll also means about 200 wake-ups per second per paused job.
- Fix: wait on a condition variable, and release the per-file destination locks between files while paused.

**I3. `engine.h:9-10` says every public method returns at once unless marked `[worker]`. That is not true of three methods.**
- `eject()` calls the OS eject (`engine.cpp:1764-1782`).
- `unfinished_json()` reads every unfinished job's journal from SQLite (`engine.cpp:1559-1585`).
- `history_json()` does the same kind of read.
- The Swift chrome calls them off the main thread (`ImportModel.swift:628-631, 656, 687-692`). Any other caller trusting the header would block the UI on import.db or the eject.
- Fix: mark them `[worker]` in the header.

**Places.** No issues worth filing.
- All host-supplied coordinates and sizes are checked for finite values and clamped (`places.cpp:79-95, 126-160`; `world_map.cpp:127-160, 171, 208, 222`). The route is capped at `k_max_route_points` (`places.cpp:196`). Surfaces are capped at 4096 px.
- The add-on parses no untrusted files; geocoding lives in `src/geo`.

---

## Improvements (throughput, robustness)

1. **Import copy path: per-file sync cost.** Each member costs:
   - `F_FULLFSYNC` on the file and again on the parent directory, per target (`file_port_mac.cpp:40-47, 66-73, 414-416`);
   - a full uncached read-back;
   - a `synchronous=FULL` WAL commit for the journal row (`library_index.cpp:140-142`, `engine.cpp:1185`).

   With main plus backup, that is 4 drive-cache flushes and 1 SQLite fsync per file, strictly one file at a time (`engine.cpp:1111-1216`). For cards of many small JPEGs the flushes, not bandwidth, will dominate. Options: sync the parent directory once per folder or unit rather than per rename; overlap the next file's read with the current file's read-back; batch journal rows per unit in one transaction (the unit is already the atomic step). Measure before and after, per CLAUDE.md. This complements #194 (which is UI-side).
2. **Import to a network destination ignores `files_in_flight`.** `batch_copy_profile` sets 4 files in flight for shares (`verified_copy.cpp:512-523`). Only `file_jobs.cpp`/`main_mac.mm` use it; Import copies one file at a time, so small-file imports to a NAS are latency-bound.
3. **Netstack memory ceiling.** 1024 connections × (256 KiB send + 64 KiB receive), held in `std::deque<uint8_t>` (`netstack.h:61-66`, `netstack.cpp:198-199`), allows about 320 MB in the worst case, and `std::deque<uint8_t>` per-byte operations are slow. Use a ring of fixed chunks, and consider a global byte budget.
4. **The `save_record` and `write_whole` pattern of remove then create then rename** (`sysproxy_record.cpp:44-51`, `prefs.cpp:81-97`) has a window where neither the old nor the new file exists. Write the new file, then rename over the old (POSIX `rename` replaces atomically; on Windows use `MoveFileEx(REPLACE_EXISTING)`).
5. **Resume reconcile should honour `yield`**, so it does not compete with the viewer (see I1).
6. **The VPN page should say plainly when Block is off and the PAC falls back to DIRECT** (V5), and the macOS card should say the kill switch is unavailable (`vpn_mac.mm:213`).

---

## Test gaps

- **Proxy record ordering (V2).** No test where the port's `set()` succeeds and the process "dies" before the record is written (`tests/test_driftwire_relay.cpp:1648` covers quit and a record-file crash only). Add a `fake_port` that throws or aborts after `set`, plus a record path that cannot be written, and assert the next `service` restores it.
- **Resume reconcile responsiveness (I1).** No test that `engine::shutdown()` returns promptly while `resume_on_control` is hashing large present targets. The `[import][engine]` resume tests (`test_import_engine.cpp:485, 624, 782, 839`) cover correctness only.
- **Windows kill switch and DNS (V1).** There are route-plan tests (`test_vpn.cpp:916, 931`), but none asserting that DNS (port 53) to a local-network address is blocked or allowed under `block()`. That needs a unit-level check of the filter set the plan produces.
- **The `users apply` stdin framing (P1).** `test_phone_self_host.cpp` has no case with a tab or newline in the name or password.
- **Prefs file mode (V3).** No assertion that the VPN prefs, or a prefs file that ends up holding the key, is 0600.
- **Sequential copy short read.** `run_sequential` stops at the first short read (`verified_copy.cpp:387`). `file_reader::read` loops to fill (`file_port_mac.cpp:524-537`), so this is safe today. No test pins that contract; a refactor of `read` would silently truncate verified copies. Add a test with a reader that returns partial chunks.
- **Card ejected during read-back.** Covered for the copy phase ("a pulled card interrupts", `test_import_engine.cpp:485`). Not covered for a destination that disappears between the rename and the journal write, which is handled by reconcile but untested.
- **Places.** Well covered (27 + 2 cases). Nothing material is missing.
