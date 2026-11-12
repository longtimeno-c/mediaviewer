# Plan — MediaViewer for iPhone: your NAS library, searched from your phone

A phone app that reaches a person's home NAS, from the sofa or from another country, takes the
search index their Mac already built, and answers queries about the whole library on the phone.
Written 2026-11-09, revised 2026-11-10 after the App Store decision. It is a forward plan, not a
description of the product; when a slice lands, move what it built into the matching
`docs/design/` doc and strike the slice here.

**The iPhone app ships on the App Store (owner, 2026-11-10).** The other calls in §3 are not
decided until answered. Four of them touch D1–D9, the numbered rules or the licence. The plan
recommends an answer to each and does not assume it.

Labels used below: **[code]** read in this repo at the cited line. **[Apple]**, **[vendor]**
platform or third-party knowledge, to be proven by a spike. **[est]** my arithmetic, not a
measurement. No iPhone number exists anywhere in this repo.

## 0. The short version

- **Yes, it is time, but the first work is not on the phone.** Today a NAS is only a path to the
  app. There is no catalogue on the NAS, no publish step, no stable identity for a file between
  machines, and several bugs would corrupt anything built on top. Throwaway iPhone and Synology
  spikes (M1) come first, because they are the cheapest way to find a problem that ends the
  project. Then M0 and M2, which are desktop work.
- **The shape:** the Mac publishes an immutable, signed catalogue to the NAS, and also hands it
  straight to the phone when the two are nearby. The phone answers queries **on the phone**, with
  the same text model and the same search code, so search needs no connection at all. Thumbnails
  and originals come from the NAS on demand.
- **The connection (changed 2026-11-10 for the owner's setup, §2.1):** no VPN and no SMB on the
  phone. A small always-on MediaViewer service on the Synology, built from Driftwire's command line,
  is reached through one forwarded port from anywhere, and the same way on its own network.
  The Mac can be off, asleep or travelling.
- **The App Store changes who this is for.** The design started from one person's setup (§2.1),
  and the store means strangers will install it. That adds a licence permission, a way in for App
  Review, IPv6-only networks, an encryption filing, and a service people can install and keep
  updated (§2.2, §3, §7.2). Version 1.0 is search and viewing; phone backup follows in 1.1.

## 1. What the audit found (2026-11-09, at `1facf73a`)

Code was read, nothing was run. Findings marked *suspected* depend on OS or server behaviour.

### 1.1 NAS management: there is no such subsystem

A share is a path plus one `network` bit from `MNT_LOCAL` / `DRIVE_REMOTE`
(`io/volume_mac.cpp:161,184`, `io/volume_win.cpp:128-142`). That bit drives copy depth
(`io/verified_copy.cpp:506-507`), a "network (slower)" label (`addons/import/engine.cpp:394-400`)
and a 15-minute walk for AI roots (`addons/ai/engine.cpp:2728`). There is no mount handling, no
reconnect and no server identity. `index.db`, `faces.db` and thumbnails are always local
(`addons/ai/engine.cpp:195-200`).

What does work: a share that is wholly unreachable keeps its index rows, Photos backup stores
destination-relative rows that survive a remount (`shell/photos_backup.cpp:61-75`), and
`verified_copy` is sound.

| # | Severity | Finding | Where |
|---|---|---|---|
| N1 | **Critical, fixed 2026-11-09** | **A share dropping mid-walk deletes index rows.** A subfolder that fails to open returns success, its recursion result is ignored, and a `readdir` error looks like end of directory. If the root listing succeeded, `end_root_scan` deletes every unseen asset, with its embeddings and face rows. The comment at `engine.cpp:2943` promises the opposite and holds only when the root itself fails. Confirmed by reading the walk through `end_scan`. | `io/file_port_mac.cpp:587,619`, `file_port_win.cpp:661,697`, `ai/engine.cpp:2942-2962,3270`, `ai/index_db.cpp:642-645` |
| N2 | **High, fixed 2026-11-09** | **`.mvindex` import commits truncated data as success.** `step_row` turns `SQLITE_IOERR` / `CORRUPT` into "no more rows", read loops end, `COMMIT` runs. `progress` is written before `frames`, so assets are marked done with vectors missing, and a rescan will not repair them because `(mtime, size)` still match. | `ai/transfer.cpp:54,514,553,646-663,744` |
| N3 | Medium, fixed 2026-11-09 | `.mvindex` export ignores `CREATE INDEX` and close results, with no journal or sync, then renames into place. A damaged file gets a valid name and feeds N2. | `ai/transfer.cpp:266,433-436` |
| N4 | Medium | The AI index cannot follow a remount. Paths are absolute with no relocate API, so `/Volumes/photos-1` is a new, empty root. | `ai/index_db.h:16`, `ai/transfer.cpp:562,606` |
| N5 | Medium | Photos backup ignores `manifest::record` errors, so a failure after the copy produces `name (2)` next run. | `shell/photos_backup.cpp:136-152,358` |
| N6 | Medium | #576 is still open. Windows probes on the UI thread, and its watcher dies silently on a failed re-arm (not in #576). | `shell/main.cpp:10147-10177`, `io/dir_win.cpp:351,366` |
| N7 | High for a phone, *suspected* | No NFC/NFD handling in `ai`, `io` or `core`. Accented names can differ between macOS SMB, Windows and iOS, and matching is exact. | absence |
| N8 | Medium, *suspected* | A NAS-side empty share walks successfully and deletes all rows. A hung share can stall the AI control thread, because import holds the write transaction while reading from the NAS. | `ai/engine.cpp`, `ai/transfer.cpp` |

### 1.2 Import: verification is solid, the ledger is not

**Solid:** unverified bytes never get the final name (`io/verified_copy.cpp:401-491`), pair and
multi-destination rollback, recovery from a crash between rename and journal, collision safety,
no database work on the UI thread.

| # | Severity | Finding | Where |
|---|---|---|---|
| I1 | **High, fixed 2026-11-09** | **An offline NAS erases the duplicate ledger.** Any failed `stat`, including a disconnected share, drops the row. Opening a previously imported card while the NAS is away forgets every match, and the hash is later re-learnt from whatever is on disk, so rot is accepted. Confirmed. | `addons/import/planner.cpp:55-56,62-64,342` |
| I2 | **High** | **Ledger and journal key on absolute root strings**, though `18-import.md:149` says volume-relative. A remount, or drive letter vs UNC, misses destination dedupe, and interrupted jobs can never resume. | `import/library_index.cpp:147-149`, `import/engine.cpp:764,1076` |
| I3 | Medium | An interrupted job reads as success in the window ("N copied · 0 failed"). `pending_files` is never read by either chrome. | `ImportModel.swift:588`, `ImportWindow.cs:1176` |
| I4 | Medium | A short NAS blip or a full disk marks the file failed and carries on with every remaining file. No free-space preflight. The reason is a generic "could not be written, twice". | `import/engine.cpp:44,1075-1085` |
| I5 | Medium, *suspected* | A dead mount can pin a job: no I/O timeouts, cancel only between buffers, quit joins it. | `import/engine.cpp:298` |
| I6 | Low | Resume reports done despite earlier failures, so a card can auto-eject. Former backups count as library. NAS→disk and disk→NAS jobs can deadlock on device locks. Scan and plan cannot be cancelled. `journal`, `jobs` and `reports/` are never pruned. | `import/engine.cpp:289,945,976,1018,1171,1479`, `planner.cpp:92` |

### 1.3 What this means for the phone

- **No completion hook.** `MV_ADDON_EVENT_JOB_DONE` carries a job id and state, and only Import's
  own chrome listens. New media reaches search only if the destination happens to sit under a
  search root, and then only on the share's 15-minute walk.
- **`.mvindex` is the wrong artifact.** It omits dates, GPS, ratings, tags, cues and OCR
  (`ai/transfer.h:11-25`), so most filters would not work. It is manual, full and unverified.
- **Paths do not travel.** `assets.path` is absolute; the only relative form is `.mvindex`'s
  `root + rel`.
- **Live SQLite on a share is not an option.** `index.db` is WAL, and WAL needs shared memory on
  one host.

## 2. The product

**One sentence:** type what you remember, on your phone, anywhere, and the photo or the moment in
the clip comes back from your own NAS.

**In 1.0:** connect to a NAS · copy its catalogue · search by meaning, words on screen, speech,
file name, person, place, date, camera and kind · browse by date and folder · view photos ·
play clips from the matching moment · save a copy to the phone · use it offline against the
catalogue (results and small thumbnails, not originals).

**In 1.1:** back up the iPhone's photos and videos to a chosen NAS folder while the app is open
(§10b). It moves after the first release because it is the only feature that writes to the NAS,
and a read-only first version is easier to get through review and to trust.

**Not in 1.0:** indexing on the phone (the Mac did it) · any write to the NAS · editing ·
backup with the app closed · NAS administration · Android.

**Who it is for:** someone who already runs the desktop app against a NAS. Setup is one path: scan
the code shown by MediaViewer on a Mac or Windows PC (owner, 2026-11-09). There is no search for a
NAS and no typing a server in by hand. **Changed 2026-11-12 (owner):** one more code, `SAMPLE`, opens
a sample library with no Mac and no NAS, in every build, for App Review and for anyone trying the
app (§11.1). The Welcome screen still offers only the scan.

### 2.1 The first setup (owner, 2026-11-10)

| Machine | Where | Its job |
|---|---|---|
| **Synology** | A different building from home; ports can be forwarded | The library. Does the heavy lifting. Runs the MediaViewer service the phone connects to. |
| **TrueNAS CORE** (FreeBSD) | Home | A redundant backup of the Synology. Runs no MediaViewer software (§7.6). |
| **Mac** | Usually in the Synology's building, but carried around | Imports, indexes and publishes the catalogue. Syncs it to the phone when nearby. |
| **iPhone** | Anywhere | Searches on its own. Connects to the Synology without the Mac. |

Neither NAS runs a VPN, and none is wanted.

| Situation | Search | Originals |
|---|---|---|
| Mac nearby, Synology reachable | Newest catalogue, from the Mac | From the Synology |
| Mac nearby, no internet | Newest catalogue, from the Mac | Not available |
| Mac elsewhere, Synology reachable | Last catalogue the Mac published | From the Synology |
| Nothing reachable | What is already on the phone | Not available |

### 2.2 What a store customer needs

The product page and the setup screens state these plainly, because the app does nothing without
them:

- MediaViewer on a Mac or Windows PC, with Local search and Driftwire installed.
- A Synology on the list S5 produces. Other NAS brands are in §17.
- A way in from the internet: a public IPv4 address with one forwarded port, or a public IPv6
  address with that port opened. Without either, the phone works only on the NAS's own network
  (§7.3).

Setup ends with a real test. On a device with mobile data, the app connects over it and says whether
the Synology can be reached from outside. An iPad without mobile data skips the test and says so.
Anyone who fails finds out on the setup screen, not after buying into the app.

## 3. Owner calls

Each changes what gets built. CLAUDE.md says to stop rather than contradict D1–D9; this is that
stop. A reversal needs a dated row in `docs/design/12-decision-log.md`.

| # | Call | Why it needs you | Recommendation |
|---|---|---|---|
| 1 | **Licence and store. Store decided 2026-11-10: the App Store.** | GPL-3.0 and the App Store's terms are widely held to conflict, and `15-platforms.md:168` rules stores out. | Still to do in M0: amend `15-platforms.md:168` and `11-licensing.md`, add the dated row to the decision log, and write a section 7 additional permission for the iOS app. `LICENSE-ADDONS.md` will not do, because it leaves `src/addons` under the GPL and the iOS app links `addons/ai` and Driftwire. GitHub lists only you and Claude as contributors (checked 2026-11-10), but **check with a lawyer, including how AI-written code affects the permission.** Add contributor terms before accepting any outside code. The iOS build contains no GPL and no LGPL code; none is needed now that SMB is gone. **Two more points for the lawyer (2026-11-10):** three commits (`d383730b`, `fc75792b`, `990a5d73`) carry a Grok co-author trailer, so "only you and Claude" is not the whole list. GPLv3 section 6 asks for installation information on consumer devices, which App Store signing prevents, and the permission must answer that explicitly. |
| 2 | **D9: a fourth platform** | D9 names three hosts and makes every PR dual-track. | Amend: iOS is a **companion client, not a host of the viewer core**. It shares `addons/ai`, `infer` and the catalogue code, and M-slices run on their own track. Desktop PRs do not need an iOS half. |
| 3 | **Rule 2: the canvas on a phone** | "The canvas is a native swapchain C++ owns." | UIKit + ImageIO + AVPlayer for 1.0, stated openly and gated on the hitch numbers in §13. If it fails the gate, port the Metal canvas. Same logic as the XAML-islands question: answered by a measurement. |
| 4 | **Rule 6 and "after the person accepted"** | A catalogue of names and places lands on a phone with standing access. Today's wording says bytes go only after the person there accepted (`12-decision-log.md:2007`). | Amend: a person's own paired devices, on their own NAS, are not "leaving the machine". Still no MediaViewer server, no telemetry, no upload. |
| 5 | **Writing to the NAS** | `18-import.md:186` says data stays on the machine and `:483` excludes a catalogue. Photos backup already sets a precedent. | Allow a `.mediaviewer/` folder, created only when the person turns on phone access, and phone backups into the one folder they choose. |
| 6 | **A service on the NAS, open to the internet** | "No second app" (`28-driftwire.md:518`, `12-decision-log.md:2052`) and D9 name no NAS host. A forwarded port is a public attack surface. | Allow it as a mode of the `driftwire` command line (already a headless product), on the person's own hardware, paired devices only, with an install and unattended update path (§7.2). Still no server of ours and no account. Never forward SMB. |
| 7 | **Face data on the phone** | `faces.db` holds biometric embeddings. | Publish names and which assets they appear in. **Never publish embeddings.** |
| 8 | **Which text model** | The phone must run exactly the indexed tower (`active_spec`). L/14's text half is 248 MB, B/32's 127 MB. H/14's text size is unlisted. | The phone copies the tower from the NAS or a nearby Mac, so the app stays small. Support B/32 and L/14 in 1.0 and refuse H/14 with a clear message until measured. |
| 9 | **iPhone only?** | Android doubles the host work. | iPhone and iPad first. Keep the C++ free of Apple dependencies so Android stays possible. |
| 10 | **Encryption filing and France** | The app encrypts with libsodium rather than iOS, so it is not exempt. Apple asks for a French declaration when an app uses standard algorithms the OS does not provide and is sold in France [Apple]. Whether Noise and CPace count as standard needs a lawyer's reading. | Leave France out of the first release so the filing cannot hold up launch, and add it once the declaration is done. |

**The way in for App Review** is not a call but is required work. Apple expects a working demo
account or demo mode [Apple], and a single-use, short-lived code cannot be that. **Decided
2026-11-12 (owner):** a fixed code, `SAMPLE`, opens a sample library in every build; this reverses
the no-sample-library call and puts the sample before a review server (`12-decision-log.md`, "A
sample library, opened by a code"). See §11.1 and M10.

## 4. Architecture

```
  Mac (carried; usually beside the Synology)
  ──────────────────────────────────────────
  Import ─verified copy over SMB─────────────▶ Synology: /volume1/Photos/2025/…
  AI indexer (index.db, faces.db)                        ▲
     │ quiet + interval                                  │ read_at, backup writes
     ▼                                                   │
  Publisher: builds generation N locally                 │
     ├─ on the Synology's network ─copy over SMB───▶ .mediaviewer/gen/N, models/, thumbs/
     │                                                   │
     └─ phone nearby ─Driftwire link─┐          MediaViewer service (driftwire serve)
                                     │                   │ one forwarded port, sealed, paired
                                     ▼                   ▼
                                   iPhone  ◀─────────────┘ from anywhere, or its own network
                                     │
           query ▶ tokenize ▶ encode ▶ scan ▶ rank   (all on the phone; newest generation wins)

  TrueNAS CORE (home) ─pulls with rsync over SSH, then snapshots─▶ a backup of the Synology (§7.6)
```

### 4.1 Where should the query run?

| Option | Mac must be on | NAS software | Works offline | Verdict |
|---|---|---|---|---|
| **A. On the phone, against a copied catalogue** | No | A small file service | Yes | **Chosen.** Fits "no server of ours", keeps queries private, and latency does not depend on the tunnel. |
| B. On the Mac, over a link | **Yes** | None | No | Fails the brief: the library would vanish whenever the Mac sleeps. The read-only host (`mv_ai_reader_get`) makes it possible as a later fallback. |
| C. Search on the NAS | No | Search engine and models | No | Weak CPUs, and heavy to package. The service in §7.2 only moves bytes. |
| D. SQLite over SMB, live | No | None | No | Unsafe with WAL, and slow over a tunnel. Rejected. |

The costs of A: the phone stores the catalogue and a model, and needs a sync design. §5 and §9
carry those.

## 5. The catalogue format (`.mediaviewer/` on the NAS)

Written only by the publisher. Read-only to every phone.

```
<share>/.mediaviewer/
  library.json                 library_id (UUID), format, publisher key history (rule 5)
  current.json                 library_id, epoch, generation, hash of its manifest, Ed25519 signature
  publisher.lease              machine name + heartbeat; one publisher at a time
  gen/000123/
    manifest.json              every file: size, SHA-256 (the AI pack has no BLAKE3); schema, active_spec, counts, created
    catalog.db                 SQLite, journal_mode=OFF, opened immutable=1
    tiny.pack                  64 px thumbnail for every asset, inside the phone's copy
    thumbs.idx                 asset → pack, offset, length
  models/<spec>/               content-addressed text tower; shared across generations
  thumbs/t-000.pack …          JPEG-512, content-addressed, shared across generations
  previews/p-000.pack …        optional 1600 px stills (setting, off by default)
```

**Rules:**

1. **The commit point is one rename.** The generation is built on the Mac first (§6). Copy it to
   `gen/N.part/`, flush every file, read it back and
   hash it (use the machinery behind `verified_copy`), rename to `gen/N/`, then write
   `current.json.part` and rename it. A reader sees the old catalogue or the new one, never a mix.
   `current.json` is the one file that replaces another, and `verified_copy` only ever renames
   without replacing (`io/verified_copy.cpp:483`), so an atomic replace over SMB is new code and
   is tested on the real share.
2. **Immutable once named.** That is what makes `immutable=1` safe. The manifest hash is checked
   before the file is opened.
3. **Signed.** Anyone with write access to the share could otherwise feed a phone a crafted
   database. The phone pins the publisher's key at pairing, verifies `current.json`, and opens
   `catalog.db` with `SQLITE_DBCONFIG_DEFENSIVE`.
   The manifest also lists the hash of every thumbnail pack, preview pack and model file it uses, so nothing outside the signature reaches the phone: an ONNX model is parsed by a native runtime. The private key lives in the OS store on each desktop (Keychain, DPAPI), which needs a keychain access group the app does not have yet. A stolen publisher can sign until the key is moved (rule 5).
4. **Retention (changed 2026-11-10).** Keep the current generation and the previous one. Anything
   older goes once its successor has been out for 2 hours: a phone copying it has had that long,
   and hourly publishing would otherwise keep about 170 generations. Phones never write to
   `.mediaviewer/`, so there are no leases to clean up. A thumbnail pack is deleted only when no
   retained generation names it and it has been unused for 30 days; a pack under 16 MiB, which
   the next publish folds into a new one, goes after 2 hours. A phone that still misses one falls
   back to `tiny.pack` until its next sync.
5. **One publisher, and a key that can move.** The lease stops two desktops overwriting each other.
   A second one says "Published from Studio Mac" and offers to take over. Taking over needs the
   current desktop to sign a record naming the new key, which raises the epoch, and phones follow
   that chain from the key they pinned. If the old desktop is gone there is nothing to sign with,
   so each phone scans a new code from the new desktop. The phone never trusts a new key because
   a file on the share says so.
6. **Dot folder.** Walkers already skip leading-dot names (`io/file_port_mac.cpp:591`), so the
   catalogue will not index itself.
7. **Newest wins, from any source.** The signature covers the library id, the epoch and the
   generation, and `(epoch, generation)` only goes up. The phone takes a generation from the
   Synology or a nearby Mac only if it is newer than the one it has, so an old copy can never
   replace a new one. The publisher keeps its own high-water mark and always publishes above both
   that and what the NAS holds, so a Synology restored from the TrueNAS cannot make it reuse a
   number. A desktop that has lost both needs the new code from rule 5.
8. **The phone can be ahead of the NAS.** A nearby Mac can hand over generation N while the Synology
   still holds an older one and lacks some thumbnails or files. That is a normal state: the phone
   compares its generation with the Synology's and shows those items from `tiny.pack`, with
   the "not there yet" wording in §11.3 rather than saying the file is gone.

**`catalog.db`, what is in it:** the same table names as `index.db` schema 11, so the reader code
runs unchanged: `meta`, `roots`, `assets`, `asset_text`, `progress`, `frames`, `speech`, `cues`,
`tracks`, `passages`, `passage_text`, `passage_vecs`. Then:

- **Identity.** Every root gets a stable UUID and a `(server, share, sub-path)`, which the NAS
  service maps to its own folder (`Photos` to `/volume1/Photos`). `assets.path`
  becomes a synthetic `/<root-uuid>/<rel>`, NFC-normalised, with the on-disk bytes in a new
  column. `path_key`, `key_parent` and the `in:` filter keep working untouched.
- **Places.** Each asset's place label is resolved at publish time and stored, because a phone
  with no Places provider would otherwise match nothing for `place:` (`engine.cpp:6935`).
- **People.** Names and asset links only (call 7).
- **Labels.** `labels.f32` travels too, and the read-only reader now loads it (2026-11-10). Without the file every row passes, as before.
- **What the reader needs beyond the tables (audit, 2026-11-10; confirmed in the code):**
  - The file must be named `index.db` in the reader's data folder (`engine.cpp:195`), so the phone copies `catalog.db` to that name.
  - `settings.json` must say which kinds are on, or audio and documents silently vanish from results (`load_settings` defaults them off). The publisher writes it.
  - The text tower is found through `tiers.json` and each folder's `model.json`, not `models/<spec>/`. M5 must lay the phone's folder out that way, with the tier set to match `active_spec`.
  - People needs a `faces.db`-shaped file with no embeddings and `faces: true`, or `@name` queries silently return nothing (`query.cpp:834`). Not built yet.
  - Speech and passage search need the `ai-audio` and `ai-docs` spec files on the phone, or those searches are off.
  - `place:` goes through `host_.place_matches` (`engine.cpp:6983`), so the phone's host must answer it from the stored labels. Writing those labels needs a new host call from GPS to place name.
  - A new generation means a new engine instance, not a reload. Memory roughly doubles for the asset maps and speech rows during the swap, and §9's budget must count that.
  - Copies keep only the answering tower's vectors, because a migration leaves two in the index (done).
- **Reachability.** A root the phone can never reach (a Mac-only disk, the Photos library) can be
  left out, or kept as "On your Mac" rows with thumbnails. Default: left out.

**Sizes [est]:** ~100 MB of `catalog.db` per 100k L/14 frames, ~150–200 MB of `tiny.pack` per 100k assets at 1.5–2 KB each (a 64 px JPEG cannot be 500 bytes; the add-on also has no JPEG encoder yet, so a host call is needed), ~5 GB of `thumbs/` per 100k assets (on the NAS, cached on demand).
Your current `index.db` is 174 MB.

**Changes between generations (1.0):** the whole `catalog.db` is copied again. A 161 MB catalogue
takes about 45 seconds through a 30 Mbit upload and about 13 through 100 Mbit [est], and a 1M-frame
library's ~1 GB takes about 4.5 minutes at 30 Mbit [est]. A nearby Mac is faster. Copies run on
Wi-Fi unless the person asks otherwise (§8.4), because hourly publishing could otherwise cost
hundreds of megabytes of mobile data a day. Delta segments are slice M13, because there is no
change feed today: no tombstones, and `frames.id` is not AUTOINCREMENT, so ids can be reused.

## 6. The publisher (desktop, both platforms)

Lives in `addons/ai` because add-ons carry their own code and there is no second app. Windows
gets the same thing: "indexed on a Mac" is today's case, not a limit. Chrome is a new card in
Local search settings on both hosts.

- **Build locally, then deliver.** A `VACUUM INTO`-style copy from a read snapshot goes to a local
  cache, so indexing never pauses and no database read stays open across a network write. The
  finished generation is then delivered: copied to the NAS whenever its share is mounted, and handed
  to a phone when one is nearby (§7.5). While the Mac is away from the Synology, generations wait
  locally and the newest is delivered on return.
- **Trigger:** after a scan goes quiet, at most once an hour, and "Publish now". Import's new
  completion hook feeds the indexer the list of files, which then triggers a publish.
- **Priority:** background only. Yields to the view and to presents, as Import does. The idle gate
  still holds: nothing runs while nothing changed.
- **Missing thumbnails** are generated on a low-priority pool. Hours of that work must survive a
  cancel, unlike `.mvindex` export, which discards everything on any failure
  (`ai/transfer.cpp:258-264`).
- **Failure states are shown once and precisely:** "Waiting for the Synology, will publish when it's back", "Not
  enough space on the share (needs 2.1 GB)".
- **Nothing here is written unless the person turned on phone access.**

## 7. Connecting

Rewritten 2026-11-10 for §2.1. The earlier design (SMB from the phone, a VPN, embedded Tailscale)
is in §17 for NASes that cannot run the service.

### 7.1 One service, one protocol

`remote_file` is a small C++ port: `connect`, `list`, `stat`, `read_at`, `write`, `cancel`. Its one
implementation is the Driftwire link to the service on the NAS, on every network. That removes
`libsmb2`, its LGPL framework and any NAS password from the phone.

### 7.2 The NAS service (`driftwire serve`)

A new headless mode of the `driftwire` command line, on the Synology. It moves bytes and nothing
else: no search, no models, no decoding.

| Need | Today | To build |
|---|---|---|
| Run without a terminal | The CLI needs one (`cli/main.cpp:8`) | `serve` mode, started at boot |
| A fixed port | Mapped only while a code waits (`mapping.h:12-13`) | Listen on a configured port; no mapping or hole punching needed |
| Remember devices | Needs a desktop keyring (`28-driftwire.md:1210-1215`) | Use the file store (`secrets::file_store`, today test-only), owner-only permissions |
| Serve parts of files | Whole-file push only (`wire.h:103-138`); `file_reader::read_at` exists | Stream multiplexing, per-stream credit, cancel, and `read_range`, in new message ids 25+ behind a capability check (old peers end the link on unknown ones, `wire.h:121-122`) |
| Responsive seeking | 256 KiB chunks and 16 MiB windows (`wire.h:51,79`) | Chunks of 64 KiB or less for interactive streams |
| Standing access | Accept on every transfer | A read-only library grant on a paired device; from 1.1, writes only to the backup folder |
| Ignore strangers | `XXhfs` does the ML-KEM work and reveals the NAS's identity before it knows who is connecting (`noise.h`) | Use the existing `XXhfs+psk0` with a 32-byte library access key from the QR. A stranger fails at the first read, so the NAS does no key work and says nothing. Device keys are still checked after that, and `source_limit` (`admission.h`) caps attempts. |
| Install | None | A package a person can install on their Synology, chosen by S5 |
| Stay updated | `driftwire update` only notifies and re-runs the desktop and Linux install channels (`cli/update.h`) | Signed, unattended updates for the NAS package. An internet-facing service cannot wait for someone to notice a notice. |
| Old and new together | Unknown messages end the link (`wire.h:121-122`) | Both directions negotiate capabilities, because App Store versions and the NAS service update on different days. The app says when the service is too old. |

**It serves only named folders**, by opaque root id plus a checked relative path (`safe_rel`), from
a config the desktop writes at setup. It runs as its own unprivileged user: read on the library
and, from 1.1, write on the backup folder under a storage limit the person sets. Revoking a device
is done from the desktop card.

**Setting it up (proposal, S5 confirms).** The person installs the package on the Synology. The
desktop then claims the service by writing a one-time token into `.mediaviewer/` over SMB, then
writing back a nonce the service sends, which proves it can read and write the share (built
2026-11-10, `docs/design/28` "Claiming and pairing"). The service accepts a claim only while it has no owner and only
from its own network. The claiming desktop can then add and revoke devices and change the named
folders.

**Open until spike S5:** which build and package format run on which Synology models. DSM is Linux,
but the model's CPU decides whether the x86-64 build applies, and many models are ARM [vendor]. S5
tries a DSM package, a Task Scheduler boot job, and Container Manager on models that have it, and
produces the supported list in §2.2.

### 7.3 Reaching it

The QR gives the phone two addresses for the Synology: its local address and a public name.
DSM's free DDNS gives a stable name if the building's IP changes [vendor]. Each time the app comes
to the foreground it tries both and uses the first that completes the sealed handshake with the
paired identity, target under one second. The library access key is mixed into that handshake and
never sent, so a wrong server learns nothing and can never be mistaken for the right one.

**IPv6-only networks must work.** App Review has rejected apps that fail on them [Apple]. The public
route is always a name, resolved with `getaddrinfo` (`net::resolve`, `net_posix.cpp:817`), so iOS
can translate it for NAT64. Driftwire's own literal parser (`net_address.cpp`) is never used for
the public route. The service listens on IPv4 and IPv6.

**Requires** a public IPv4 address with a forward, or a public IPv6 address with the port open.
IPv6 can avoid a relay behind carrier-grade NAT, but only for phones that have IPv6 at that moment.
With neither, the phone works only on the NAS's own network, or through a relay (§16). The desktop
warns if the name resolves to a private or carrier-grade address, and the mobile-data test in §2.2
is the final answer.

**Forward one port, to the service only.** Never SMB, never DSM's admin page.

### 7.4 Pairing

The desktop card shows a QR code carrying the library id, publisher public key, the Synology's
addresses and identity, the library access key, and a single-use pairing code for the service.
**One scan pairs the phone
with both the Mac and the Synology, and there is no NAS password to type or store.** The code is
short-lived, and the Mac shows which phone was added. The Mac provisions the service once
beforehand. **Scanning is the only way to add a library** (owner, 2026-11-09): the code comes from
MediaViewer on a Mac or a Windows PC.

**The phone shows the library's name and a short key fingerprint, and the desktop shows the same**, and the person confirms they match before pairing finishes, so a fake QR code cannot quietly pair the phone with someone else's server.

### 7.5 Nearby sync from the Mac

When a paired phone and Mac are on the same network, the app is open, and the Mac has a newer
generation, the Mac sends it directly. No NAS and no internet are involved. It sends the catalogue,
`tiny.pack`, `thumbs.idx` and, once, the text model. JPEG-512 thumbnails and originals still come
from the NAS, which may not have them yet (§5, rule 8).

This is what Driftwire already does best: sealed, resumable whole-file transfer to a known device.
It needs paired devices to skip Accept for catalogue files, and discovery through Bonjour
(`_mediaviewer._tcp`, not built, `28-driftwire.md:1679`), because Driftwire's broadcast beacon would
need Apple's restricted multicast entitlement [Apple]. `_mediaviewer._tcp` is declared in
`NSBonjourServices`.

**The two add-ons have to meet by M3, not here.** The QR needs the publisher key from the AI add-on
and pairing from the Driftwire add-on, and add-ons reach each other only through the host function
table. M2 adds that host-table route, so the design is settled before the QR is built. Phone access
needs both add-ons on the desktop, and the card offers to install whichever is missing. The iOS
app links both statically, so this is desktop-only.

### 7.6 The two NASes

Set up on the NASes themselves; MediaViewer builds nothing here for now.

- **The TrueNAS pulls from the Synology** on a schedule with rsync over SSH. Only the Synology needs
  to be reachable, so home needs no port forward.
- **SSH uses key-only login** from a dedicated read-only Synology user, on its own forwarded port.
  This is a second internet-facing port, separate from the phone's, with its own risks: use DSM's auto-block, and remember that this user can read every photo.
- **The copy includes `.mediaviewer/`**, so a restored Synology keeps its catalogue.
- **One direction, never a two-way sync.** The Synology is the source and the TrueNAS only receives.
- **ZFS snapshots on the TrueNAS after each pull**, kept for weeks, so a file deleted or corrupted on
  the Synology can still be recovered after the next pull. Regular scrubs check the copy.
- **Test a restore** before relying on it.

A Driftwire backup mode would need scheduling and changed-files-only transfer, and a FreeBSD build
for this TrueNAS. It is in §17.

### 7.7 Why Driftwire now, when this plan first rejected it

The first version's objections were that it needed an awake Mac, could not get through two hard NATs, and
only pushed files. The service on the Synology replaces the Mac, a forwarded port removes hole
punching, and the third is the build in §7.2. Its crypto (Noise with post-quantum hybrid, CPace
pairing) and resumable channel are reused unchanged. Its internet path has still only been tested
against emulated networks, which S2 settles.

## 8. The iOS app

### 8.1 Code layout

```
src/mobile/        C++ companion core: catalogue reader, sync, remote_file over the Driftwire link, thumb store, mv_mobile_* ABI
src.ios/           SwiftUI + UIKit chrome, iPhone and iPad
cmake/ios.cmake    arm64-ios triplet; static mv_ai_engine, mv_infer, mv_driftwire_engine, ONNX Runtime
```

Dependencies point down as elsewhere. Reuse, in order of preference: `addons/ai` query, vectors,
tokenizer and index reader unchanged · the `search_session` host-table shape · `core`, BLAKE3,
libsodium. The add-on loader is unusable on iOS (no downloaded code [Apple]), so it is linked
statically behind a small host table. Known iOS build work: `ocr`, `fetch`, `photos` and `cloud`
stubs; `addons/ai/platform_posix.cpp` includes IOKit power headers the iOS SDK lacks; `infer` loads
ONNX Runtime by `dlopen` (`ort_dylib_posix.cpp`) and needs a static path. Driftwire's `net_posix`
gateway lookup, `machine_mac`, `secrets_mac` and `power_mac` need iOS versions or stubs.

### 8.2 Threads and priorities

Same discipline as the desktop: **nothing that can block touches the main thread.**

| Role | Does | Never |
|---|---|---|
| Main | SwiftUI/UIKit, gestures | Network, decode, SQLite |
| Search | Tokenize, encode, scan, rank; one query at a time, newest wins | Network |
| Decode pool | ImageIO to the display size | Allocate past budget |
| Net lane 1 | What is on screen | |
| Net lane 2 | Prefetch: ±2 in the viewer, one screen ahead in the grid | Starve lane 1 |
| Net lane 3 | Catalogue sync, phone backup | Run while lanes 1 or 2 have work |

Each job carries a generation, as on the desktop. Navigation bumps it. Reads in flight for
something the new view still wants are handed over, not restarted.

### 8.3 Local storage

| What | Where | Protection |
|---|---|---|
| Catalogue, tiny pack, text tower | Application Support | Excluded from iCloud backup, `NSFileProtectionCompleteUntilFirstUserAuthentication` so a background sync can run |
| Thumbnail LRU | Caches | Default 2 GB, a setting, system may purge |
| Device identity, pairing secrets, pinned key | Keychain | `ThisDeviceOnly` |
| Backup ledger | Application Support | Excluded from iCloud backup; a copy lives on the NAS (§10b) |

**Excluding from backup is a privacy requirement, not a size choice:** otherwise a catalogue of the
whole library would go to iCloud.

### 8.4 Lifecycle

Foreground work only, plus one `BGProcessingTask` for catalogue sync on Wi-Fi and power [Apple].
Sockets die on suspend, and Driftwire ends a session after 20 s without a link (`wire.h:73-74`),
so resume re-races the addresses and shows the old catalogue meanwhile. Search
never waits for the network. Launch is core-first, as on the desktop: the catalogue opens before
SwiftUI finishes attaching.

**Mobile data.** Checking `current.json` is always allowed, because it is tiny. Copying a catalogue
or a model waits for Wi-Fi unless the person taps to copy it now, and the app shows the size first.

**The first copy** can be hundreds of megabytes, including the text model, and starts from an action
the person just took. On iOS 26 and later it runs as a `BGContinuedProcessingTask`, so leaving the
app does not stop it [Apple]. Older versions need the app kept open. That API is only for work the
person started, so automatic sync does not use it.

## 9. Query on the phone

The path is the desktop's (`engine.cpp:6886`): parse → resolve people and places → scope → filters
→ phrases in speech and cues → encode → int8 dot-product scan → gates → rank.

- **Parity is the test.** Same query set, Mac vs phone, same catalogue: the same rows in the same
  order. D23's parity test used 504 assets; this one needs a real library.
  Ties need a defined order, because the scan sorts on score alone (`vectors.cpp:250-254`) and identical vectors tie. Rank by score, then asset id, then time, on both the desktop and the phone.
- **Encoder.** Static ONNX Runtime on CPU first, as desktop does, because it is the same model and
  the same numbers. Core ML only if S1 says CPU is too slow, and only with a parity check. A
  smaller or quantised text model would be a different tower and is out of scope.
- **Memory.** The reader loads vectors fully: 80 MB at 100k L/14 frames, 797 MB at 1M
  (`vectors.h:131-140`). 1.0 is fine to a few hundred thousand frames. M12 moves vectors to an
  `mmap`ped flat file, since clean file-backed pages do not count against the app the way dirty
  ones do [Apple]. The budget is tight: a 248 MB L/14 text model plus 80 MB of vectors leaves about
  70 MB of the 400 MB target for ONNX Runtime, SQLite and images [est]. S1 measures the real
  footprint. If it fails, the first levers are loading the weights from a mapped file and Apple's
  increased-memory-limit entitlement [Apple].
- **Speed.** A desktop core scans 100k L/14 rows in 31 ms per pass (`17-local-ai-search.md:1741`),
  and a query can take two passes. 1M would be ~310 ms per pass [est], before NEON or threads.
- **Readiness.** The model loads in the background once the first pixel is up, so it never costs
  launch time. Until it is ready, file name, date, people and place still work, and the screen
  says what is still warming up only if someone actually waits. iOS will often have ended the app
  between uses, so this cold path is the usual one and S1 measures it.

## 10. Media pipeline

Rule 3 on a phone: **first pixel is never the full decode.**

| Step | Source | Latency |
|---|---|---|
| 1 | Memory cache | One frame |
| 2 | `tiny.pack`, always local, so results paint even offline | One frame |
| 3 | JPEG-512 from the disk cache, else range-read from `thumbs/*.pack` | Network; adjacent reads are merged |
| 4 | Published preview, if on; else embedded RAW preview by range read | Network |
| 5 | Original, downscaled to the display by ImageIO | Network; progress shown past 1 s |

**Formats.** ImageIO covers JPEG, HEIC, PNG, GIF, TIFF, WebP, AVIF and many RAWs [Apple]. Anything
it cannot decode shows its thumbnail and says so. No bundled decoders in 1.0, which also keeps
libheif and LibRaw out.

**Video.** `AVAssetResourceLoaderDelegate` over a custom scheme, turning AVPlayer's byte-range
requests into `read_range` calls on the link [Apple]. Plays what the iPhone plays: H.264, HEVC, and AV1 on recent
chips, in MP4 and MOV. MKV, WebM, AVI, TS and MPEG-2 show their moments as stills and offer the
file. FFmpeg comes later if people ask.

**The honest limit:** a 100 Mbit 4K clip will not stream through a 30 Mbit upload at the Synology's
building. No NAS
transcoding in 1.0. The app measures throughput, says so, and offers moments or "Download first".
Mac-encoded phone proxies are in §17.

**A speech or scene hit opens at that moment**, which is the feature nothing else on a NAS does.

## 10b. Phone backup (1.1)

The iPhone's photos and videos copied to a NAS folder the person picks. **It runs while the app is
open** (owner, 2026-11-09), which keeps it simple: no background extension, and the app always
knows what has been copied. Apple's background upload extension needs an HTTP upload server, which
the service is not [Apple].

**What the person chooses** (Backup screen): the folder on the NAS, and how files are filed.

| Folders | A file lands in |
|---|---|
| Year | `<folder>/2026/IMG_0412.HEIC` |
| Year and month (default) | `<folder>/2026/09/IMG_0412.HEIC` |
| Year, month and day | `<folder>/2026/09/21/IMG_0412.HEIC` |
| One folder | `<folder>/IMG_0412.HEIC` |

The date is the photo's own capture date in local time. These are the desktop Photos backup's
layouts (`shell/photos_backup.h`, `backup::layout`), including year-and-month, added for this. Changing the layout moves nothing that is already backed up.
Which zone counts is the capture offset stored with the photo, so a photo does not change folders because the phone travelled. The desktop backup's default is Year and day and has no shared default with the phone.

**Knowing what is done.** A ledger on the phone holds one row per file: PhotoKit's asset id, which
resource (photo, Live Photo's video, RAW), size, BLAKE3 hash, where it went, when. A photo counts as
backed up only when it has a row **and** the file is still on the NAS, checked with one folder
listing per folder, so a file someone deleted from the NAS is copied again. A copy of the ledger is
written to `<folder>/.mediaviewer-phone-backup/<device id>/ledger.json` by temp file and rename,
after every 50 files and at the end. A reinstall or a new phone restores from it.
The ledger keys on `PHCloudIdentifier` where iCloud Photos provides one, because a `localIdentifier` is per-device and can change after a transfer to a new phone. Everything else falls back to name, size and hash. The NAS copy is found by library id, which the QR carries, because a new phone has a new device id. It is unsigned, so restoring it only saves re-copying and never counts a file as safe without the folder check.

**Copying,** the desktop's rule through the service: the phone sends the file and its hash, and
the service writes `<name>.part`, flushes it, hashes what is on disk, and renames without
replacing. Only its confirmation adds the ledger row. A file interrupted at any
point is simply not done, and the next run does it again.

| Rule | |
|---|---|
| Starts | On its own whenever the app is open and the phone is on Wi-Fi with the Synology reachable, wherever that Wi-Fi is; a switch turns that off. Never over mobile data unless asked. |
| Order | Newest first, so recent photos are safe soonest |
| What | Originals: the photo or video as shot, a Live Photo's video, a RAW. Not Photos' edited versions. Same as the desktop backup. |
| iCloud-only originals | Downloaded one at a time, copied, then dropped, so the phone never fills up |
| The screen | Stays awake during a backup, with a switch to allow it to sleep |
| Leaving the app | Stops after the current file. Picks up where it left off next time. |
| Same name, different photo | ` (2)`, never overwrites |
| Same name and size already there | Hash the NAS file. If identical, record it and skip. This only helps in the same folder: the desktop's Photos backup does not do the same check and uses a different default layout, so running both into one tree can still make duplicates. |
| Deleting | Never deletes from Photos. Never mirrors a deletion to the NAS. |
| Storage limit | The service refuses writes past the limit set for the backup folder, and the screen says so |
| Foreground priority | Viewing and search come first (§8.2): backup uses its own lane and pauses while a photo is loading |

**The screen says one thing:** "1,204 of 18,320 backed up · 17,116 to go", then the one progress
line. A **Not backed up** view shows exactly which photos remain, and failures are listed with a
reason. With limited Photos access, it says only the chosen photos are covered.

**Getting them into search.** If the folder sits under a Local search root, the desktop indexes it
on its next walk and the next published catalogue includes them. The app says so if the folder
is not indexed.

### 10b.1 Open calls for 1.1 (written 2026-11-12, before M11 is built)

What §10b and §7.2 leave open. Each has a recommendation; none is built until answered.

| # | Call | Recommendation |
|---|---|---|
| B1 | **Who names the backup folder.** | The owner, on the desktop card: "Add a backup folder" is the existing "Add folder" with a storage limit, and the service marks that named folder writable. The phone picks among the writable folders the service lists (usually one) and never names a NAS path. |
| B2 | **Who may write.** Today every paired phone holds a read-only grant. | Every paired phone may create files in every backup folder. A scoped or temporary grant (being built separately for `driftwire serve`) can take that away; a read-only grant never writes. |
| B3 | **The ledger copy on the NAS replaces itself** (§10b says temp file and rename), which breaks "never replaces a file". | Allow exactly one exception: the service replaces `<folder>/.mediaviewer-phone-backup/<device id>/ledger.json`, and only for the device whose id it is. Every other write creates a new file or fails. |
| B4 | **An interrupted large file.** A 4 GB clip killed at 3.9 GB starts over. | The service may append to, or remove, a `.part` it made itself for that same device, never anything else. A run resumes a `.part` from its size; the service still hashes the whole file on disk before the rename. |
| B5 | **What the storage limit counts.** | The bytes in that backup folder as the service counts them (walked at start, then kept up to date by its own writes, including `.part` files). Separately, "NAS full" when the volume would keep less than 1 GB free after the file. The card asks for a limit, suggesting the smaller of 1 TB and half the free space. |
| B6 | **Backup in the sample library** (owner: SAMPLE must demo every feature, and must never upload anyone's photos). | The real backup code with the person's own Photos as the source (so the Photos prompt and limited access are seen), into a clearly labelled **"Sample NAS on this iPhone"** folder in the app's own storage, through the same C++ writer the service uses. Its storage limit is 250 MB, so "Storage limit reached" can be seen. Nothing leaves the phone. Forget removes the folder and its ledger. No change to the `sample-library` release, so no republish. |
| B8 | **Bursts and hidden photos.** | **Settled (owner, 2026-11-12): not backed up.** Hidden photos are left out, and of a burst only the photo Photos shows for it is copied, not the other frames. The Backup screen and the listing say so. |
| B7 | **The desktop indexes what arrives.** | The backup folder is chosen from the desktop's own search roots on that share (or added as one when chosen), so its next walk indexes it and the next publish carries it to the phone. The card says when it is not indexed. |

**Answers (owner, 2026-11-12).** B6 as recommended. B3 and B4 both allowed. B1, B5, B7 as
recommended, and the storage limit and the free-space reserve are both set per backup folder on the
card and can be changed later. B2: worked out with the owner-passphrase and grants plan
(`docs/plans/nas-owner-and-guests.md`, agreed with that session 2026-11-12): a grant gains `backup`
(may create files in backup folders). A device is on the paired list (`known.h` `devices`) only when it
has no expiry, no folder scope and backup allowed; any other device, a phone with backup off included,
lives only in `library_grants`, which an older NAS never reads, so it fails closed. Guest codes default
to backup off. Turning backup on is additive (needs the owner's unlock), off is reducing, and the 2 s
watch cuts a session whose backup flag changed. `may_write` reads the session's grant: none (unlimited)
may write; otherwise `grant.backup` and the grant's folders. Until the grants branch
(`claude/nas-owner-passphrase-grants`) merges, every paired phone may write.

**#46** (phone to desktop through Import) is a different route and stays open. Its findings carry
over: native PhotoKit, and streaming rather than staging originals.

## 11. The interface

### 11.1 Screens

| Screen | Job | Key states |
|---|---|---|
| Welcome | One action: scan the code from MediaViewer on your Mac or Windows PC | No library yet |
| Connect | Nothing to type: the scan pairs the phone with the Mac and the Synology, then tests the outside route over mobile data | Code expired; code not readable; Synology not reachable (names which address failed); reachable only at home |
| First copy | One progress line | "Copying library · 48 of 161 MB · about 25 seconds left" |
| Library | Date-ordered grid, folder switch | Route chip only when it matters |
| Search | Field, recent searches, then results | Typing, results, nothing found |
| Viewer | One photo, pinch and swipe | Refining; original unreachable |
| Clip | Player and moment ticks | Too big to stream |
| Details | Date, place, camera, file | |
| People | Named people | Only when the Mac has them |
| Backup (1.1) | Folder, folder layout, how much is done, what is left | Running; paused off Wi-Fi; folder not allowed by the service; storage limit reached; NAS full |
| Connection | Synology status, where the catalogue came from and when, cache size, forget | Each failure names its cause |

**App Review, and anyone without a NAS (built 2026-11-12, owner):** the code `SAMPLE`, typed on
the Scan screen ("Enter a code"), or `mediaviewer://sample` as a QR code or link, opens the **sample
library**: 44 photos and 3 short clips from Wikimedia Commons, CC0 or public domain, with no
identifiable face (`tools/ios/sample-library/SOURCES.md`). It is in every build, Release and
TestFlight included, and nothing about it is hidden:

- The Enter-a-code prompt says "type SAMPLE to try the sample library". The next page is "Open the
  sample library?" with what it is and where it comes from, and its own check code.
- Only the transport is simulated. The catalogue is a real generation, made by the desktop's own
  indexer and `phone_publish` with the B/32 tower, signed by a sample-library publisher key (not the
  owner's) and read from the GitHub release `sample-library` through a static `remote_file`
  (`src/mobile/sample.*`, the host's HTTPS in `SampleFetch.swift`). The same sync checks its signature,
  generation and every hash and installs it; search, the grid, the viewer and clips are the real code.
  The pictures and clips themselves, and the JPEG-512 pack, are listed with their hashes in
  `files.json`, signed by the same key, and each is checked whole before it is shown.
- Downloaded rather than bundled, because the text tower is 127 MB: the phone copies about 131 MB
  once (the tower, `catalog.db`, `labels.f32`, `tiny.pack`), then the JPEG-512 pack (3 MB) and
  originals (50 MB in all) as they are viewed. The app itself grows by code only.
- Labelled while open: a "Sample library" chip on the grid, which opens Settings; Settings names it,
  what it is and that it comes from GitHub; Forget says "Forget the sample library" and removes
  everything, as for any library. Pairing a real library replaces it; entering SAMPLE with a real
  library paired asks first ("Replace <name> with the sample library?") and changes nothing on Keep.
- `tools/ios/sample-library.sh` makes it again from `sources.json` (licences rechecked each time) and
  uploads it, current.json last. The review notes are `src.ios/AppStore/review-notes.md`.

The review server and review grant are no longer needed for 1.0. A grant may still come later for
showing a real NAS to someone.

### 11.2 Design rules

The [mock-ups](https://claude.ai/artifact/U3rGGf1DyR2iEmSHqwMPLi) (private to the owner until
shared) are built with Stageshow, and the real screens follow the same **Craft rules**
(`20-edit-workspace.md:570`), which already came from it. For this app that means:

- **One surface per thing.** The photo is the interface. No cards around thumbnails.
- **Say it once.** The connection appears when it changes something. "Connected" is not a state
  worth a chip.
- **One progress language:** a 3 pt track, an accent fill that never goes back, one line under it.
- **Icons show what the tool does:** SF Symbols, and no sparkle for search.
- **Dark by default**, as the viewer is. Depth from a lighter surface, not shadow.
- **Touch feedback starts on the input.** Only marketing motion is slow.
- **A photo opens as one object:** the thumbnail grows into the viewer and sharpens in place.

### 11.3 Failure copy, written now

| Situation | The app says |
|---|---|
| Synology unreachable | "Can't reach your Synology. You can keep searching what's on this phone." |
| Catalogue older than 30 days | "Last updated 34 days ago. Open MediaViewer on your Mac near your Synology to refresh it." (once) |
| Original gone | "This file isn't on the NAS any more." |
| Phone is ahead of the NAS | "This one hasn't reached your Synology yet." |
| Reachable only on its own network | "Your Synology can't be reached from the internet. Your provider may not give that building a public address." |
| Service too old | "The MediaViewer service on your Synology needs updating." |
| Publisher changed without approval | "This library was published from a different computer. Scan a new code from that computer to keep using it." |
| Unsupported tower | "This library uses the largest search model, which this phone can't run yet." |

## 12. Security and privacy

| Threat | Answer |
|---|---|
| A fake pairing QR code | The person compares the library name and key fingerprint on both screens first (§7.4) |
| SSH forwarded for the TrueNAS backup | Key-only login, DSM auto-block, a read-only user; a separate port with separate risks (§7.6) |
| Someone with share access swaps the catalogue | Ed25519 signature, pinned key, defensive SQLite |
| Someone with share access swaps the publisher key | Phones accept a new key only through a signature from the key they pinned, or a new scan (§5, rule 5) |
| The forwarded port is open to the whole internet | Without the library access key a connection fails at the first read, before any key work or reply. Attempts are limited per address. Device keys are checked next, so a revoked phone gets no further. The first message is still parsed, so it is fuzzed. The service is unprivileged, sees only its named folders, and updates itself (§7.2). |
| SMB or the admin page exposed to the internet | Never forwarded; the setup guide says so |
| Hostile Wi-Fi impersonating the NAS | The sealed handshake proves the paired identity, and no password exists to capture |
| A photographed QR code | The pairing code is single-use and short-lived, and the Mac shows each added phone |
| Replay of an old signed catalogue | `(epoch, generation)` only goes up (§5, rule 7) |
| A swapped JPEG-512 tile | Not checked per tile: a pack's hash is in the signed manifest, but the phone reads tiles by range and cannot hash the pack. A tile reaches only ImageIO, from a service the phone proved is the paired one, so the threat is someone who can write the share and swaps a pack in place. A per-tile hash in `thumbs.idx` would close it (open). |
| An unclaimed service on the NAS | Claimed only once, only from its own network, and only by a desktop that can write to the share (§7.2) |
| The sample library's release is replaced | The phone pins the sample publisher's key (compiled in). The catalogue is checked as a NAS's is: `current.json`'s signature, then every file against the signed manifest. Every other file (the root list, the JPEG-512 pack, each original and clip) must be in `files.json`, which the same key signs with its size and SHA-256; it is fetched whole and hashed before any byte reaches ImageIO or AVFoundation, and an unlisted file is never read. So write access to the release alone cannot put a picture on a phone. The secret key is not in the repository. |
| The sample library sends something about the person | Only HTTPS reads of the release's files: no cookies, no cache, a fixed `User-Agent: MediaViewer` (not the app's build, the OS or the device) and `Accept-Language: *`, besides the range and the transport's own headers (checked in `sample-library-sim.sh`). GitHub sees the address, as for any download. Not tracking, so nothing for `NSPrivacyTrackingDomains`; the App Privacy answers stay "no data collected". |
| Lost phone | iOS data protection, Keychain device-only, optional Face ID, "Forget this library" wipes everything; revoking the phone from the desktop removes it from both the NAS service and the Mac's paired devices |
| Cloud backup copies the catalogue | Excluded from backup (§8.3) |
| The catalogue on the NAS reveals names, places, text | No worse than the share it sits on, which already holds the files. Encrypting it with a key from the QR is a later option. |
| Biometrics | Never published (call 7) |
| The phone can write to the NAS (1.1) | Only through the service, only into the backup folder, under a storage limit; it creates files and never replaces or deletes one |
| Telemetry | None. Crash reports follow the existing privacy line: default off, ask first, no paths, names, pixels or EXIF. |

Also required for the store [Apple]: a privacy manifest that declares the required-reason APIs the
C++ core calls (file timestamps, disk space, boot time), the App Privacy answers, the export
compliance answers with `ITSAppUsesNonExemptEncryption` set (call 10), and plain-language prompts
for the camera (the QR) and the local network, plus Photos for saving a copy and, in 1.1, backup.

## 13. Performance

**Budgets are targets until S1–S3 give numbers.** They then get the same treatment as the desktop's.

| Measure | Target |
|---|---|
| Launch → first pixel of grid | ≤ 400 ms, from `tiny.pack` |
| Submit → results at 100k frames | ≤ 150 ms |
| Tap → first pixel in viewer | One frame |
| Original on the Synology's network, 12 MP HEIC | ≤ 500 ms |
| Scroll, cached | No hitches at 120 Hz, by XCTest hitch metrics |
| Route race on foreground | ≤ 1 s |
| Idle | No network, ~0 % CPU |
| Memory at 100k frames | Under 400 MB, measured with the app's footprint |
| Cold start → first semantic search answered | Set by S1, with the model not yet loaded |

**Harness:** `tools/mobilebench`, XCTest plus `os_signpost`, emitting the same JSON shape as the lab.
A fixed test library on the Synology, runs alternated, Network Link Conditioner for 5, 30 and 100
Mbit. Devices: the oldest supported iPhone and the newest.

**Desktop gates still apply.** The publisher must not move any published number or the idle cost,
measured before and after on both platforms.

## 14. Build, test, ship

- **Toolchain:** Xcode, CMake with an `arm64-ios` vcpkg triplet, everything static. Minimum iOS to
  be set by S1, likely 18.
- **CI:** build and unit tests on simulator; catalogue and service-protocol fuzzing (one is parsed
  from a share, the other faces the internet), including the first handshake message;
  a licence check that fails on any GPL or LGPL dependency in the iOS build.
- **Tests that matter most:** the publisher killed at each step leaves the old catalogue valid ·
  a truncated file is refused · parity Mac vs phone · a share pulled mid-sync · NFC/NFD names ·
  a remounted share · a NAT64 network · a stranger without the access key gets no reply · a publisher
  key change with and without the old key's signature · a phone ahead of the NAS · a new app against
  an old service and the reverse · in 1.1, backup killed at every step leaves no `.part` counted as
  done · a file removed from the NAS is copied again · a ledger restored after reinstall re-copies
  nothing.
- **Ship:** TestFlight first, then the App Store with the sample library from §11.1 (its release
  uploaded first) and the filings from §12. Builds on TestFlight expire after 90 days, so it is a stage and not a channel.

## 15. The slices

Numbered M to stay clear of the desktop's PR numbers. Each has one verify line. **M1 runs first**,
then M0, then the rest in order. M0–M10 are the 1.0 release, M11 is 1.1, and M12–M13 follow when
real libraries need them.

### M1 — Spikes (throwaway code)

S1 text tower on a real iPhone: load time, latency, memory, including a cold start after iOS has
ended the app. S2 a Driftwire link through a real port forward to the Synology, from mobile data
and other Wi-Fi: throughput, range reads, reconnect, and connecting by name on a NAT64 network.
S3 scan at 100k and 1M. S4 AVPlayer over a resource loader, with seek. S5 which `driftwire` build
and package format run on which Synology models, how it starts at boot, and how it updates.
**§16 says what ends the project.**

> **Verify:** A report in `docs/perf/` with the numbers from the oldest and newest supported iPhone, the NAT64 result and the supported Synology list; §13 is rewritten from them.

### M0 — Decisions and the repairs everything sits on (desktop, both platforms)

Owner calls 2–10, and the rest of call 1: the iOS licence permission, contributor terms, the
decision-log row, and the `15-platforms.md` and `11-licensing.md` changes. N1, N2, N3 and I1 are fixed (2026-11-09, walks and imports tested, a real share dropping not). Still to fix: **I2**, which needs a stable identity for a share on both platforms (Windows has none today: `io/volume_win.cpp:185` keys a network volume by its path). Add NFC normalisation at
the catalogue boundary (N7).

> **Verify:** Unmount a share mid-walk in the harness: 0 index rows lost. Damage a `.mvindex`: import is refused and nothing commits; damage only its thumbnails: the index imports and the result says the thumbnails were incomplete. Open a card with the NAS offline: 0 ledger rows lost. Both present-loop gates hold.

### M2 — Publisher (desktop, both platforms)

**Built (2026-11-10, Mac; the Windows halves share this code and compile only in CI):** everything the phone needs from the catalogue, in `docs/design/17-local-ai-search.md` "The phone's catalogue": thumbnail packs, `thumbs.idx` and `tiny.pack`, names-only `faces.db`, stored place labels, hashes of every shared file in the signed manifest, the text tower on the share, the trigger after a quiet scan, generations that wait for the share, NFC names, the ranking tie-break, retention for shared files. **Built since (2026-11-10, Mac):** the host-table route, claiming and device admin, the QR, nearby sync, moving the key, the address warning (`docs/design/28` "Claiming and pairing", `docs/design/17` "Setting a phone up"). **Still to do:** the kill-point clause of the verify line below, the Windows card, a settings control for `phone_auto`, and previews (the manifest and retention carry them; nothing makes them yet).

§5 and §6, including the label-bar fix, place labels, the root UUID map, the settings card, the
epoch and key history, and the host-table route between the AI and Driftwire add-ons (§7.5).

> **Verify:** Kill the publisher at each of 20 injected points: `current.json` always names a complete, hash-valid generation. A read-only reader on another machine returns the same rows as the source for the query set. A takeover signed by the old key is accepted by a test reader and an unsigned one is refused; a publisher restarted against a restored share never reuses `(epoch, generation)`. Idle cost unchanged, present gates hold on both platforms.

### M2b — NAS service (`driftwire serve`, Linux; provisioning from the desktop)

§7.2: serve mode, fixed port, file-backed identity, access-key handshake, multiplexed streams,
`read_range`, library grant, named folders, the Synology package, claiming from the desktop,
unattended signed updates, capability negotiation, device revocation from the desktop.

> **Verify:** From outside the building, a paired client lists, range-reads and seeks in a 4 GB file; a client without the access key gets no reply and costs the NAS no ML-KEM work; a revoked device is refused; a path outside the named folders is refused; a seek under load lands within 500 ms on a 30 Mbit link; the installed package updates itself unattended and keeps its pairings.

### M3 — Connect (iOS)

**Built (2026-11-10, in `src/mobile/link*`, `sync*` and `src.ios/.../MobileLink`):** `remote_file` over the library channel, the route race by name, the Keychain, `mv_mobile_link.h`, pairing and the QR parser. Proven in the Simulator against `driftwire serve` on loopback (`tools/ios/link-e2e.sh`). Not proven yet: the camera (no camera in the Simulator), a real NAT64 network, mobile data, a device.

Project, CI, Driftwire engine on iOS, `remote_file` over the link, QR pairing as the only setup,
the mobile-data reachability test, Keychain.

> **Verify:** From a clean install, one scan pairs with the Mac and the Synology; the phone then connects from mobile data with the Mac powered off, and connects by name on an IPv6-only NAT64 network; a wrong-signature `current.json` is refused.

### M4 — Catalogue sync (iOS)

**Built (2026-11-10):** copy, signature and manifest checks, swap, resume, the never-lower rule, Wi-Fi by default, `BGContinuedProcessingTask` for the first copy. Not proven: leaving the app during a copy (needs a device), and `BGProcessingTask` for automatic sync is not built.

Copy, verify, swap, resume, background task, first copy as a continued task on iOS 26, model copy,
Wi-Fi by default.

> **Verify:** Airplane mode at any point during sync leaves the previous catalogue searchable and the next run resumes; every installed file matches its manifest hash; no catalogue or model bytes move over mobile data unless the person asks.

### M4b — Nearby sync from the Mac (Mac, Windows, iOS)

§7.5: Bonjour discovery, catalogue transfer without Accept for paired devices, newest generation wins.

> **Verify:** With no internet and the Synology off, a paired phone on the Mac's Wi-Fi receives a newer generation and answers from it; an older generation is refused; interrupted transfers resume; with the Synology back but still on an older generation, new items show from `tiny.pack` with the "not there yet" wording.

### M5 — Search (iOS)

Static engine, host shim, query screen.

> **Verify:** The 200-query parity set returns identical rows in identical order to the Mac on the same generation; submit → results meets §13 on the oldest device.

### M6 — Grid and thumbnails (iOS)

`tiny.pack`, merged range reads, LRU, priority lanes.

> **Verify:** Library grid paints every cell from local data in airplane mode; XCTest hitch metrics show no hitches while scrolling cached content at 120 Hz.

### M7 — Viewer (iOS)

Ladder from §10, card-to-viewer transition, ±2 prefetch, pinch, save to Photos.

> **Verify:** Tap → first pixel in one frame on a cached grid; original refines in place with no layout shift; call 3's hitch gate passes or the Metal port is opened.

### M8 — Video (iOS)

Resource loader, moment jump, throughput check.

> **Verify:** A speech hit opens at its time within ±1 s on the Synology's network and at 30 Mbit; a clip above the link's rate says so instead of stalling.

### M9 — Import reaches the phone (desktop, both platforms)

Completion hook with the file list, indexer trigger, publish. I3–I5 fixed here.

> **Verify:** Import a card to the NAS; with no other action, those files are searchable on the phone after the next sync.

### M10 — Ship 1.0 (iOS)

People, details, failure states, accessibility, privacy manifest, encryption filing (call 10), the
sample library and its review notes (§11.1, built 2026-11-12), and App Store material that states the
requirements in §2.2.

> **Verify:** Every row of §11.3 reproduced on a device; from a clean install on a NAT64 network, the review notes alone get a person from the code SAMPLE to a search result in the sample library; the App Store build contains no GPL or LGPL code and ships under the recorded licence permission.

### M11 — Phone backup, 1.1 (iOS and the service; the year-and-month layout is on the Mac)

**Built (2026-11-12, branch `claude/phone-backup-m11`):** the service's write grant (`docs/design/28`, "Phone
backup: the write grant"), the phone's ledger and run (`src/mobile/backup.*`, `mv_backup_*`), the Backup
screen and its states, PhotoKit (`MobileLink/LinkBackup.swift`), the sample library's on-phone target, the
Mac card's backup folder with its limit and reserve and whether this Mac indexes it, and the 1.1 store copy.
The year-and-month layout already existed on the Mac (`backup::layout::year_month`, #414). Shown on loopback
and in the Simulator only; the verify line below needs a device, a real NAS and 1,000 real files. Not built:
pausing backup while a photo loads (§8.2's lane rule; backup runs on its own queue but does not yet yield),
the Windows card's backup rows (the engine JSON is shared), the write permission inside the grants plan.

§10b: Backup screen, folder picker from the service's named folders, PhotoKit source, verified write
through the service, storage limit, ledger on the phone and the NAS.

> **Verify:** Back up 1,000 mixed photos, Live Photos, RAWs, videos and iCloud-only originals, leaving the app at random points: every file on the NAS matches its hash, none is duplicated, and the count on screen equals the files on the NAS. Delete 10 from the NAS: exactly those 10 are copied again. Reinstall: nothing is re-copied. Peak extra phone storage stays under the largest single file plus 100 MB. A write past the storage limit is refused.

### M12 — Scale

`mmap`ped vector file, threaded NEON scan.

> **Verify:** A 1M-frame catalogue meets §13's memory line and a scan under a figure set by S3.

### M13 — Deltas

Tombstones, a change feed in `index.db`, segment files.

> **Verify:** Adding 100 photos to a 100k-asset library syncs in under 5 MB, and the result equals a full copy.

## 16. Risks, and what would stop this

| Risk | Signal | Response |
|---|---|---|
| **The licence permission does not hold up** | The lawyer's answer in M0 | **Stop before building the app.** The App Store is the decided channel, and TestFlight or EU-only is a hobby distribution. |
| The text tower is too slow or too big on a phone | S1: > 500 ms encode on the oldest device, or jetsam | Raise the minimum device, or fall back to option B and accept that the Mac must be on. Do not ship a different tower silently. |
| The link is too slow or unreliable through the forward | S2: thumbnails over ~1 s, or frequent stalls | Fix the link first; SMB over a VPN (§17) is the fallback |
| The Synology cannot run the service | S5: unsupported CPU or DSM blocks it | Container if available, else the §17 fallback. Models that fail stay off the supported list. |
| The Synology's building has no public IP | The forward never answers | Public IPv6 where it exists, then a relay the owner runs (Driftwire already has `relay.h`), or the §17 fallback |
| The forwarded service is compromised | Any | It sees only its named folders and cannot delete; signed updates close the hole; the TrueNAS's snapshots are the recovery |
| App Review cannot get in | The sample library is not found, or Apple asks for a real NAS | The review notes name the code and the steps; settle it with a TestFlight external review before the first submission. A review server is the fallback. |
| Too few people can set it up | Failed reachability tests, poor reviews | Say the requirements up front (§2.2); a relay or other NAS brands next |
| The sample library's release goes | An update is rejected because SAMPLE finds nothing | Keep the `sample-library` release published; check it before each submission |
| People have libraries the phone cannot hold | Real sizes | M12 sooner |
| A catalogue bug corrupts a desktop index | Any | The publisher only reads. Keep it that way. |

**Where it needs you:** an iPhone and the real Synology for S1–S5, a lawyer for calls 1 and 10, and
the sample library's release uploaded before M10's submission. Nothing in this environment can measure or settle them.

## 17. Later, not now

- Mac-encoded phone proxies for video · a map of places · ratings and favourites written back
  through the desktop's checked writer · voice query (the parser exists, `19-voice.md`) ·
  catalogue encryption · backup with the app closed (needs an HTTP upload receiver on the NAS) ·
  a fallback for NASes that cannot run the service: SMB over the person's own VPN or embedded
  Tailscale, then WebDAV or SFTP · other NAS brands for the service · a Driftwire backup mode for
  NAS-to-NAS, with a FreeBSD build · reading originals from the TrueNAS at home · pushing a
  catalogue to the Synology through the service while the Mac is away · Mac-as-server fallback
  (option B) · Android.
