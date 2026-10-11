# Plan — every tag shown and searchable, place names, a map pin, an AI badge

The owner asked on 2026-10-11 for four connected things, on Windows and macOS together:

1. **Every tag in the Metadata pane**, including AI provenance (#290).
2. **Every tag searchable**: typing `R5`, `ChatGPT` or `portugal` finds the files (#291).
3. **Place**: GPS shown as "Sintra, Lisbon, Portugal" (town, region, country — whichever apply),
   with a small themed map and pin at the bottom of the pane; and searching a place name finds the
   photos and clips taken there.
4. **An AI-generated badge** in the bottom-right of the canvas when a file says it was AI-made,
   **on by default**, switchable in Settings.

This is a forward plan, not a description of the product. When a slice lands, fold what it built
into `docs/design/06-metadata.md` (pane, place, badge), `16-commands.md` (search, Settings) and
`17-local-ai-search.md` (index, query terms), then strike it here.

## Status (2026-10-11)

Plan only; nothing built. #290 and #291 are open with no PR. Place and the map are #344 (slices
2–3 and the place half of slice 4); the badge is #345 (slice 5).

## 0. Rules this touches

- **Rule 6 (nothing leaves the machine).** Place names and the map are computed from data bundled
  in the app. No `CLGeocoder`, no MapKit tiles, no Bing/OSM/Google tiles, no web geocoder: each of
  those sends the photo's coordinates to a server. This is the reason for the bundled data and the
  self-drawn map, not a preference.
- **Rule 5 (never modify an original).** Everything here is read-only. The existing Location
  "Remove" button and PR 12's writer are unchanged.
- **Rule 1 / performance.** Tag reading, place lookup and map drawing run on workers. The badge and
  the map never cause a present on a still (idle stays 0 presents). The folder-wide tag read for
  search never runs ahead of first pixel or the ±2 prefetch window.
- **D9.** One portable core (`meta/`, a new `geo/` under `meta`'s dependencies), a SwiftUI half and a
  WinUI half per slice, a verify line on each platform, plus both PR 1 present-loop gates.
- **Decision log.** `06-metadata.md` lists "GPS map pin" under *Not built*; slice 3 builds it, so add
  a dated row to `12-decision-log.md` (owner, 2026-10-11, read-only pin, offline map). Writing
  coordinates stays not built.
- **Licensing.** GeoNames data is CC-BY 4.0: an attribution line in the third-party notices and the
  About box. Natural Earth is public domain. Both are data, not linked code.

## 1. What exists today (2026-10-11)

| Area | Today | Where |
|---|---|---|
| GPS, photos | EXIF `GPSLatitude/Longitude` (+Ref) → a display string; the doubles are dropped. XMP `exif:GPS*` is not read | `src/meta/still.cpp:182-201`, `meta.h:56` |
| GPS, clips | `com.apple.quicktime.location.ISO6709` / `location` → display string; altitude ignored | `src/meta/clip.cpp:75-84, 347-350` |
| Pane | Summary row "Location" = `48.85837° N, 2.29448° E` with a Remove button; All tags tab via Exiv2; Windows caps All tags at 400 rows | `format.cpp:145`, `MetadataView.swift:85-112`, `IslandHost.Panels.cs:495-540` |
| Plain search | Name only (`/` typeahead, Ctrl/⌘+F file search), folded case/diacritics, in the hosts over the loaded listing | `FileSearch.swift`, `NameFilter.swift`, `IslandHost.FileSearch.cs` |
| Metadata cache | In-memory `meta_store` (48-record LRU) + per-folder `date_keys` for sort by date taken | `src/shell/meta_store.*` |
| AI index | `assets(id, path, root_id, mtime, size, kind, duration_ms, seen, cloud)`, schema 3, step-wise migrations (#176). No EXIF, GPS, camera or date taken. Date terms use mtime | `src/addons/ai/index_db.cpp:14, 125-184`, `query.cpp:140` |
| Place data / map | None | — |

## 2. Slices

Each slice is one PR, both platforms. Order matters: 1 → 2 → 3 → 4; 5 can follow 1 in parallel.

### Slice 1 — All tags (#290)

As written in #290: C2PA/JUMBF (JPEG APP11, PNG `caBX`, ISO-BMFF `c2pa`/`uuid`), PNG
`tEXt`/`zTXt`/`iTXt`, unregistered XMP namespaces listed raw, ICC description/copyright, JFIF,
WebP/HEIF container items. Bounds-checked portable parsers in `meta/`, each with a libFuzzer
harness. Windows drops the 400-row cap (virtualised list instead).

Added to #290 by this plan:
- **`meta::summary` keeps numbers**: `std::optional<geo_point> where` (lat, lon, optional altitude)
  next to the display string. XMP `exif:GPSLatitude` is read when EXIF has none.
- **`meta::summary::made_with`** (string) and **`ai_generated`** (bool), derived once in the core:
  C2PA `digitalSourceType` of `trainedAlgorithmicMedia` / `compositeWithTrainedAlgorithmicMedia`,
  or a C2PA claim generator / PNG `parameters` / `Software` / XMP `CreatorTool` naming a known
  generator. The generator list is one table in `meta/`, tested. A file is never called AI-made
  from its name or pixels.

**Verify:** the ChatGPT, Firefly, Stable Diffusion PNG and Midjourney fixtures show their generator
in Summary "Made with" and every source tag in All tags on both platforms; a 2 000-tag file lists all
of them on Windows; fuzzers run clean for 10 minutes each; PR 1 gates hold.

### Slice 2 — Place names (offline reverse geocoder)

New portable module `src/geo` (depends on `core`, `io`; used by `meta` and the AI add-on through the
host table). Data built at package time by `tools/geo/build_places.py` from pinned downloads:

- **Countries and regions:** Natural Earth 1:10m admin-0 and admin-1 polygons, simplified to
  ~1 km, packed as quantised int32 rings with a per-polygon bounding box and a coarse grid index.
  Point-in-polygon gives the country and region exactly, borders and enclaves included.
- **Towns:** GeoNames `cities1000` (every place with ≥ 1 000 people, ~150 k rows): name, ASCII
  name, admin-1 code, country code, population, quantised lat/lon, in a flat array sorted on a
  grid for nearest-neighbour lookup.
- **Names people type:** English and native names for every country and region
  (`countryInfo`, `admin1CodesASCII`, plus native names), so `portugal`, `Portugal` and
  `Portugália`-style variants fold to the same place. Full GeoNames alternate names (~600 MB) are
  out of scope.

Target size ~6 MB compressed for all three, memory-mapped, loaded on first use on a worker.

**Label rule ("as applicable"):** country from the polygon; region from admin-1; the town is the
nearest `cities1000` place within 15 km of the point (one tunable constant, measured against the
fixtures), else omitted. Off any coast, the label is "At sea, off <country>" within 25 km of land,
else "At sea". So: "Sintra, Lisbon, Portugal"; "Douro, Portugal" in vineyards with no town near;
"Portugal" as the minimum on land.

**Pane:** the Summary "Location" row becomes the place label, with the coordinates in a smaller
secondary line beneath (both copyable). The Remove button stays where it is. The `O` overlay date
line shows the place instead of the coordinates.

**Verify:** a lookup table of 200 hand-checked coordinates (borders, islands, enclaves, sea, the
antimeridian, the poles) returns the expected label in `mv_tests "[geo]"`; one lookup is under
50 µs after load, with load under 30 ms on a worker (`[.perf-bench]`); no change to first pixel or
arrow → next photo on either platform (alternated runs).

### Slice 3 — The map and pin, bottom of the Metadata pane

A small map (pane width × ~160 pt) at the **bottom** of the Metadata pane when the file has
coordinates. It is drawn from the same Natural Earth polygons as slice 2. No tiles, no network.

- **Drawing:** the core rasterises land, country borders and (at region zoom) admin-1 borders into
  an 8-bit BGRA image on a worker. The colours are passed in from the host theme: water, land,
  border and label tokens for light and dark. One portable implementation, cached per (point,
  size, theme, zoom). The host shows it as an ordinary image.
- **Pin:** drawn by the chrome on top, in the app accent colour, with a soft shadow and a gentle
  drop-in on first show. Reduced Motion turns the animation off. It is styled like the rest of the
  pane.
- **Zoom:** opens framed on the region, with − / + steps from country to region scale. There is no
  street level: the bundled data does not have streets, and the label says so honestly.
- **Interaction:** none beyond zoom; clicking the map does not open a browser or a map app (rule 6).
- **Accessibility:** the map is one element. Its label is the place text, and screen readers read
  the place text, not the image.

**Verify:** the map and pin render for the fixtures in light and dark on both platforms; opening the
pane on a geotagged still causes no present beyond the pane's own (idle 0 presents after);
arrow-held browsing with the pane open shows no regression (`--browse-soak`, alternated).

### Slice 4 — Every tag searchable, place included (#291)

**Plain search (no AI pack), both platforms.**
- **The tag text:** a per-file search string is built once in the core from the slice 1 read: make,
  model, lens, software, made-with, title, caption, keywords, creator, and the place label with its
  country and region names. It is folded the same way as names.
- **Storage:** the strings go in a persistent `meta_cache.db` (SQLite) keyed by path + size +
  mtime, next to the thumbnail cache. A folder is read once, ever.
- **Reading:** the read rides the existing `date_keys` worker pass, so a folder costs one read per
  file, not two. That pass is background priority, behind the view and prefetch.
- **Matching:** Ctrl/⌘+F and `/` match names first, then tag text. A tag match shows *why* it
  matched ("Portugal", "Canon EOS R5"). While the folder's tag read is still running, the results
  say "Still reading tags — 340 of 2 000".

**Local AI search.**
- **Migration:** schema 3 → 4, step-wise and atomic as in #176. It adds `assets.taken_at`, `lat`,
  `lon`, `place` (the label), `country`, `region` and `camera`, and an FTS5 table `asset_text`
  holding the slice 4 search string.
- **Backfill:** filled at scan time, and for existing assets by a background backfill that resumes
  like the other passes.
- **Plain words:** a word that matches place or tag text narrows or boosts the result, alongside
  pixels, speech and people.
- **New terms:** `place:` / `in:` (where `in:` names a place rather than a folder, so the parser
  must decide which), `camera:`, `lens:`, and `ai:` / `made:` (`ai:yes` = any AI-provenance file).
  They are listed in the syntax hint and in doc 17's query table.
- **Photos library items (Mac):** `PHAsset.location` supplies the coordinates when the asset has
  them. It is read locally and looked up through `geo`.

**Verify:**
- Searching `portugal` in a fixture folder (no AI pack) finds exactly the geotagged Portuguese
  stills and clips on both platforms.
- `camera:R5` and `ai:yes` return the expected sets in Local search.
- On a 20 k-file folder, the tag read never delays first pixel, and arrow → next photo stays within
  noise (alternated runs, `--browse-soak --json`).
- The 3 → 4 migration survives a kill mid-step (the #176 test pattern).

### Slice 5 — AI-generated badge on the canvas

- **Badge:** when `summary.ai_generated` is true, a small badge (a sparkle glyph plus "AI", about
  20 pt) sits in the bottom-right of the canvas, above the transport strip on clips. It has a
  translucent material and follows the theme. Hovering it, or focusing it with the keyboard,
  shows "Made with <generator>" and the evidence (for example C2PA `trainedAlgorithmicMedia`).
- **Who draws it:** the chrome layer (SwiftUI / WinUI) draws it, like the transport strip. It is
  not drawn into the swapchain, so it never adds a present.
- **Setting:** *Settings → Viewer → "Show a badge on AI-generated images"*. It is **on by
  default** (owner, 2026-10-11) and is stored like the other viewer settings.
- **Wording and honesty:** the badge appears only on positive evidence. A file with no provenance
  gets no badge, and there is no "not AI" mark, because missing metadata proves nothing. Doc 06
  says this in one line.

**Verify:** the badge appears on the AI fixtures and not on camera files, on both platforms; turning
the setting off hides it with no relaunch; idle stays 0 presents with the badge shown; PR 1 gates
hold.

## 3. Fixtures needed (not in git if large; small ones are fine)

- ChatGPT/DALL·E JPEG and PNG with C2PA.
- An Adobe Firefly JPEG.
- A Stable Diffusion PNG with `parameters`.
- A ComfyUI PNG with `workflow`.
- A Midjourney file with XMP.
- An iPhone HEIC and a MOV with ISO 6709 location.
- A DJI JPEG with XMP GPS.
- Points near Portugal/Spain borders, Lisbon, Madeira and the Azores, and one at sea.

## 4. Out of scope

- Writing or editing coordinates.
- Street-level maps.
- Online geocoding of any kind.
- A batch metadata engine.
- Validating C2PA signatures online.
- Inferring AI generation from pixels.
- Switching the AI index's date terms from file modified time to date taken. `taken_at` makes that
  possible, but it is its own change.

## 5. Open calls for the owner

- **The 15 km town radius.** Tune it on the fixtures and report it.
- **A ~6 MB increase in installer size** on each platform for the place data.
