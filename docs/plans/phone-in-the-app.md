# Plan — Phone access in the app, not in Local search

Owner calls, 2026-11-12: Phone access is a base feature of MediaViewer, with its own Settings section,
built into the app on Mac and Windows. It must pair, claim and serve with Local search not installed.
Local search only provides the search catalogue. A phone paired with a library that has no catalogue
still opens it and searches as the desktop does without Local search. This plan is piece 0 of the
phone/NAS work; the Windows card (piece 1) is built against it. Parent plan: [mobile.md](mobile.md).

## 1. Where things are today

| Piece | Lives in | Note |
|---|---|---|
| The card | `PhoneAccessView.swift`, `PhoneLinkView.swift` in AIChrome; Windows WIP in Ai.Chrome | Under Settings → Local search |
| The phone-link state machine | `src/addons/ai/engine_phone_link.cpp` (862 lines), `mv_ai_api.phone_link*` | Touches no index; only `phone_` (library id, epoch, generation, destination) |
| Claim, codes, devices, revoke, nearby serve | `src/addons/driftwire/desk.*`, reached by `addon_request("driftwire", …)` | `route_request` takes only the caller `ai` |
| The protocol | `src/addons/driftwire/{wire,link,net*,noise,cpace,stages,channel,serve*,admission,secrets*,known,pair_check,advert*}` | One static library, `mv_driftwire_engine`, also linked by the CLI and iOS |
| The publisher key and the share's layout | `catalog.cpp` (generic half), `catalog_chain.*` | Key in the OS store, service "MediaViewer Local search", name `catalogue-signing-key` |
| Building a catalogue | `catalog.cpp` (`build`, thumb packs, tiny.pack), `engine.cpp` `run_phone_publish` | Needs the index |

Pairing needs a publish today: `pair` fails `not_published` until a first generation wrote
`library.json`. So pairing needs an index.

## 2. Where they go

**Not an add-on (owner).** The 2026-10-11 rule is that optional features ship as add-ons with their
own code. Phone access is the exception: it is how a phone reaches the library at all, and the phone
app is its own product that must work without Local search. Recorded in `12-decision-log.md`.

1. **`src/wire/` — the Driftwire protocol, portable, at the `io` level.** `git mv` of the protocol
   files listed above, plus `key_hex` / `parse_key_hex` out of `transfer.h` (into `wire/keys.h`).
   Two fixes on the way: `known` loses `presence_keys` (the DHT half stays in Driftwire), and the
   `known → desk` back edge goes (Driftwire's `forget` calls both). Target `mv_wire` (static, hidden
   visibility). Driftwire, the `driftwire` CLI, `mv_mobile_link` and the core all link it.
   Ports stay ports (D9): `net_{posix,win}`, `secrets_{mac,win,linux,ios,file}`,
   `advert_{mac,win,none}`. Git follows the renames, so the concurrent `serve.*` work (owner
   passphrase, scoped access) merges into the moved files.
2. **`src/phone/` — the feature, a peer of `addon` (`abi → {…, addon, phone} → {io, wire} → core`).**
   - `desk.*` (claim, codes, devices, revoke, roots, addresses, the nearby server and advert).
   - The state machine from `engine_phone_link.cpp`, on its own thread and its own data folder
     (`<app data>/phone/`), behaviour ported exactly: `link.json` and its fail-closed flags, the
     revoke queue and its backoff, `stopping_` exemptions, the takeover states.
   - The generic half of the catalogue: keys, `current.json` signing and checking, `library.json`
     and its chain, takeover, `deliver`, retention, `pair_url` (`catalog.cpp` splits into
     `phone/catalog_publish.cpp` and the AI's `catalog_build.cpp`; `catalog_chain.*` moves whole).
   - **`library.json` is made when the person chooses the NAS folder**, not by a first build, so a
     library with no index can be claimed and paired. Nothing is written to the NAS until then
     (mobile.md §3 call 5 still holds).
3. **The publisher key is the app's.** The core keeps it, service "MediaViewer Phone access", name
   `catalogue-signing-key`. On first use it reads the old "MediaViewer Local search" item, writes it
   under the new name, reads it back, then removes the old one: a library published before this keeps
   its key, and phones keep trusting it.
4. **Local search becomes a provider.** New `mv_ai_api` entry `catalogue_build(out_dir, request_json,
   answer…)`: it writes the generation's files (catalog.db, thumb packs, tiny.pack, models) into
   `out_dir` and answers the manifest and shared-file list. The core signs, delivers and retains.
   Started by the core after a quiet scan (`phone_auto`) or Publish now. The AI's `phone_publish`,
   `phone_cancel`, `phone_json`, `phone_link`, `phone_link_json` answer `MV_ERR_UNSUPPORTED` from
   now on (kept as slots so the table's layout holds). The way Places provides place names to the
   app is the model; the AI pack is not a slot add-on, so this is an `mv_ai_api` entry, not
   `mv.request.1`.
5. **Driftwire** keeps transfers, the DHT, mapping, relays and Bluetooth. Its `desk` and
   `route_request("ai", …)` go. The CLI's `driftwire serve` links `mv_wire` and is unchanged.
6. **The ABI.** Windows: `MV_API mv_phone_request(const char* json)` and
   `mv_phone_state_json(char*, uint32_t, uint32_t*)` in `mediaviewer.h` (minor bump); Mac:
   `mv_chrome_phone_*` in `mv_chrome_bridge.h`. Both are [no-block]: requests queue onto the phone
   thread, as today.

## 3. Settings → Phone access (Mac and Windows)

Its own section in Settings, not under Local search, the same rows on both hosts, following
[20](../design/20-edit-workspace.md#craft-rules):

- **Folder on your NAS** (Choose… / Change…): writes `library.json` there once.
- **Service on your NAS**: claim, the two addresses, the address warning.
- **Add a phone**: the QR and its check code, five minutes, Cancel.
- **Phones**: each with Remove; "Removed here. The NAS will be told when it can be reached."
- **Shared folders**: the named folders, Add, Stop sharing.
- **Search on the phone**: with Local search indexing this library, "Smart search is on. Published 2
  hours ago · 12,345 files" and Publish now; without it, "Your phone searches file names and tags.
  Install Local search and index this library for words, faces, places and sounds." with the
  install button (Settings → Add-ons).
- **Another computer**: hand over / start over, as today.

Local search's own page loses its Phone access section and gains one line pointing to it.
Windows: the review's fixes of the WIP card carry over (rows updated in place so focus stays; the
poll runs only while a code or a request is under way; QR modules snapped to whole pixels; the advert
teardown never waits under the desk's lock).

## 4. The phone with no catalogue

A paired library whose share has `library.json` and no `current.json` (or a signed one naming no
catalogue) opens in **file mode**:

- It lists the named folders through `driftwire serve` (`roots`, `list`), as folders and files.
- **Search is the desktop's File search** ([16](../design/16-commands.md), "File search"): over the
  folder on screen, by name, case- and accent-insensitive, and by tags (camera, lens, keywords,
  title, caption, creator, #291), with "N of M", no walk of the library. Tags come from each file's
  head, read with `read_range` (the first 256 KiB) as the folder is shown and kept in the phone's
  cache by path, size and mtime; ImageIO parses them on iOS.
- A line above the results: "Smart search (words, faces, places and sounds) needs Local search.
  Install it on your Mac or PC and index this library."
- When a signed catalogue arrives, the library switches to full search on the next sync, as today.
- **Not demoable in SAMPLE** (owner, 2026-11-12): no second sample code, and the sample release is
  not republished for it.

## 5. Order (each step Opus-reviewed, both hosts)

1. `src/wire` split: moves and target only, no behaviour change; every existing test passes; iOS
   builds (`project.pbxproj` lists `libmv_wire.a`).
2. `src/phone`: desk and the state machine moved, key migration, `library.json` at folder choice;
   `[phone-pairing]`, `[phone-link]`, `[phone-chain]` ported to the new seam (a desk interface
   instead of `fake_driftwire`); a new test pairs with no AI engine at all.
3. The AI as provider: `catalogue_build`; `[phone]` publisher tests through the core.
4. Settings → Phone access on Mac (SwiftUI) and Windows (WinUI), the AI chrome's sections removed.
5. The phone's file mode.

**Verify:** with no AI pack installed, a desktop claims a `driftwire serve` on loopback, shows a QR,
a test client pairs with it, lists the named folders, and the desktop revokes it; with the AI pack,
a publish lands a signed generation the phone verifies, under the same publisher key as before the
move. Import and Driftwire transfers pass their tests unchanged. Both present-loop gates hold.

## 6. Owner calls (answered 2026-11-12)

1. **File mode searches the folder on screen**, by name and by tags read as the folder is shown, as
   the desktop's File search does. No walk of the library.
2. **No SAMPLE demo of file mode.** No `SAMPLE-FILES` code; the sample release is not republished.
3. **Both ways to publish:** the Publish now button and the automatic publish after a quiet scan.
