# 27 — Places: the offline reverse geocoder

How GPS coordinates become "Sintra, Lisbon, Portugal" with no network: the portable `src/geo`
module, its place database, the label rule, place-name matching, and the **Places add-on**
(`src/addons/places`) that delivers it — the pane's Place section and map, the folder and world
maps, search by place and routes. Without the add-on the app shows coordinates only. Plan:
[metadata-search-and-place.md](../plans/metadata-search-and-place.md).

## Rule 6 is why this exists

Every online geocoder and tile map (`CLGeocoder`, MapKit, Bing, OSM, Google) sends coordinates to
a server. So places are computed from shipped data: nothing in `src/geo` opens a socket; only the
package-time data build downloads, from pinned URLs.

## Module

`src/geo`, portable C++ (D9), no platform API.

| File | What |
|---|---|
| `geo.h` / `geo.cpp` | `database`: validate a blob, `lookup(lat, lon)`, `match(text)`, polygon / ring access |
| `blob.h` | On-disk format as POD records with pinned sizes |
| `fold.h` | Case/diacritic fold: `core/fold.h`'s rule (table `src/core/fold_table.inc`, from `tools/fold/gen_fold_table.py`) |
| `open.h` / `open.cpp` | `open_database(path)` via `io/mapped_file`; `set_data_path` / `shared()` (one per process) |

`mv_geo` (`geo.cpp`, `fold.h`) uses only header-only core pieces, is position-independent and
links nothing of the core, so an add-on can link it ([18](18-import.md)). `mv_geo_open` adds
`open.cpp` on `mv_io`. `tools/check-module-graph.ps1`: `geo` → `{geo, io, core}`; `addons` may
include it; **`meta`, `abi` and `shell` may not**. `io/mapped_file`: `mapped_file_posix.cpp`
(`mmap` + `MADV_WILLNEED`), `mapped_file_win.cpp` (`CreateFileMappingW` / `MapViewOfFile`).

```cpp
namespace mv::geo {
struct lookup_options { double town_radius_km = 15; bool native_names = false; };
struct place { std::string town, region, country, country_code, region_code;
               bool at_sea; double town_km; std::string label; };
struct name_match { name_kind kind; bool exact; std::string country_code, region_code, name; };
class database {
  static result<database> from_bytes(std::span<const std::uint8_t>, bool verify_checksum = true);
  place lookup(double lat, double lon, const lookup_options& = {}) const;
  std::vector<name_match> match(std::string_view text, bool prefix = false) const;
  std::vector<std::uint32_t> polygons_in(layer, const box&) const;
  std::size_t ring_count(layer, std::uint32_t) const;
  std::span<const point> ring(layer, std::uint32_t polygon, std::size_t i) const;
  std::string_view polygon_country_code(layer, std::uint32_t) const;
  std::vector<town_ref> towns_in(const box&, std::size_t limit) const; // by population
  std::string_view meta() const;                                       // provenance JSON
};
result<std::unique_ptr<mapped_database>> open_database(std::string_view utf8_path);
void set_data_path(std::string_view);  const database* shared();
}
```

A `database` is immutable once open (concurrent `const` calls safe). Open and lookup run on
workers (rule 1).

## The data

`tools/geo/build_places.py` (stdlib only), inputs pinned by URL and SHA-256, downloads to a
caller-named directory:

| Input | Licence | Pin |
|---|---|---|
| Natural Earth 1:10m admin-0 countries, admin-1 states/provinces (GeoJSON) | Public domain | `nvkelso/natural-earth-vector` v5.1.2, commit `f1890d9f` |
| GeoNames `cities1000`, `countryInfo.txt`, `admin1CodesASCII.txt` | CC-BY 4.0 | Snapshot 2026-10-11 |

GeoNames republishes daily under one URL: a changed file stops the build unless
`--accept-upstream` (which prints the new hash). `META` records every input's hash.

**Polygons.** Topology-preserving Douglas–Peucker: each shared arc is simplified once and reused
by both neighbours (no gaps or overlaps); coast 1 km, borders 250 m. Points are int32
microdegrees, rings closed; each polygon has a bounding box, per-1°-band edge lists and a 1°
candidate grid whose **full** cells need no ray cast.

**Regions** are GeoNames admin-1; each Natural Earth admin-1 feature maps to a code by its
`gn_a1_code` (if a town confirms it) or a population vote of up to 40 towns inside; no majority
→ no code. A region ≥ 80% another country's carries `k_region_country` (Svalbard, Réunion).

**Towns:** GeoNames populated places minus `PPLX`, `PPLA5` and historical/abandoned/religious
ones. **Names:** countries in English, native and ~25 languages; regions by GeoNames name and
ASCII name.

### Blob format (`places.mvgeo`, version 1)

64-byte header: magic `MVGEO\x1a`, version, file size, checksum. Then a section table, each
section 8-byte aligned, little-endian: strings; countries (16 B); regions (16 B); towns (20 B)
and their grid; META (JSON); per layer: polygons (40 B), ring starts, points, band offsets, band
edges, grid rows (sparse: used column span only), cell offsets and candidates.

Checksum: four-lane 64-bit multiply-xor over everything after the header. `from_bytes`
bounds-checks every offset, count, index, edge and grid run once and requires every point on the
globe; lookups then run unchecked. Damaged / truncated / foreign → `status::corrupt`; newer
format → `unsupported_format`.

Full blob: **17.2 MB** raw, **5.4 MB** xz; 256 countries, 3 931 regions, 161 600 towns, 230 k
admin-0 and 639 k admin-1 points.

## The label rule

1. **Country:** the admin-0 polygon containing the point; else within **1.5 km** of one
   (`k_coast_snap_km`); else within **2 km** of a town (`k_town_land_km`) → that town's country;
   else within **25 km** of land (`k_off_coast_km`) → "At sea, off Portugal"; else "At sea".
2. **Region:** the admin-1 polygon (or nearest within the snap distance), kept only if it belongs
   to the country; `k_region_country` overrides the country; no polygon → the chosen town's region.
3. **Town:** candidates in the same country (and region, when it is a GeoNames region) within
   **15 km** (`k_town_radius_km`, a setting). A town *reaches* `sqrt(population / (π · 5000/km²))`
   (500 k → ~5.6 km). The town the point is deepest inside (smallest distance / reach) wins, else
   the nearest. A same-country-and-region town is never dropped for a nearer one across a border.
4. **Label:** "Town, Region, Country", skipping empty parts and repeats of a neighbour.

Poles are clamped, longitudes wrapped; town and coast searches wrap grid columns at ±180°. Past
the radius the label is "Region, Country".

## Matching place names (slice 4)

`match(text, prefix)` folds with `geo::fold` and returns countries and regions whose folded names
equal (or, with `prefix`, start with) the text; exact first. The fold matches the hosts' name fold
(`FoldName` in `IslandHost.FileSearch.cs`; Swift `.caseInsensitive, .diacriticInsensitive`):
decompose, drop marks, lowercase; ł ø ß unchanged; Latin, Greek, Cyrillic. Country codes are not
indexed as names.

## Performance

`mv_tests "[.perf-bench]"`, M-series, Release, warm cache: open 11–12 ms median (target < 30 ms);
lookup p99 8.4 µs on the 300-row table, 3–4 µs on random points (target < 50 µs). No allocation on
the lookup path beyond returned strings. Section map render at pane width × 160 pt: 1.5–1.7 ms
CPU median at 1×, 4.5–4.7 ms at 2× (max ~20 ms), on the add-on's worker; cache hit ~0.03 µs.
Nothing here runs on the viewer's paths.

## Tests and the fuzzer

- `mv_tests "[geo]"`: fold, label rule, names, rings, damaged-blob sweep, on the committed test
  blob `tests/data/geo/places-test.mvgeo` (`--subset test`).
- **300-row table** (`tests/geo_places_table.inc`, hand-checked) runs when the full blob exists;
  `MV_REQUIRE_GEO=1` makes its absence a failure (CI's `test-media` job builds and caches the
  blob; Windows legs set `MV_GEO_BLOB`).
- `mv_tests "[places]"` (`tests/test_places.cpp`): map, folder map, routes, search by place.
- `mv_import_tests "[places]"` (`tests/test_places_addon.cpp`): the add-on in a signed store
  through host table v3.
- Fuzzers `tools/fuzz/fuzz_geo.cpp` and `fuzz_places_map.cpp`; `tools/package/test_addon_pack.py`
  `PlacesPackTest`.

## Delivery: the Places add-on

Places carries its own code and data. A phone has no Places: the publisher asks
`mv.place_labels.1` (host table `place_labels`) and stores labels in the catalogue
([17](17-local-ai-search.md) "The phone's catalogue").

| File | What |
|---|---|
| `src/addons/places/addon_entry.cpp` | `mv_addon_get`, `mv.slots.1`: info section, search provider, map events |
| `src/addons/places/places.h/.cpp` | ABI-free engine: section tree, zoom/pan state, `place_query` |
| `src/addons/places/map.h/.cpp` | Rasteriser, gesture maths, picture cache |
| `src/addons/places/folder_map.h/.cpp` | Folder-map dot layer: clustering, drawing, hit test |
| `src/addons/places/world_map.h/.cpp` | World-map view |
| `src/addons/places/self_dir_*.cpp` | Own install folder (`dladdr` / `GetModuleHandleExW`) |
| `src/addons/places/manifest.json` | Description and contributions |
| `cmake/places.cmake` | `mv_places_engine` (tests) and the `mv_places` module in `build/addons/places` |

`mv_places` links `mv_geo` and nothing of the core; `src/geo/open.cpp` and
`io/mapped_file_<os>.cpp` are compiled in (`addons` rule `{addons, geo, core}`). The blob opens
on first need, on the add-on's worker, from `<install folder>/data/places.mvgeo`; missing or
damaged → logged once (no path), and the section says "Place names are unavailable".

Manifest (nothing else: no bar, path, status item, badge):

```json
"host_api": {"min": 3, "max": 3}, "chrome": "",
"contributes": {
  "api": {"min": 5, "max": 5},
  "info_sections": [{"id": "place", "title": "Place", "when": "location", "order": 0}],
  "search_providers": [{"id": "place", "name": "Places", "prefixes": ["place:", "in:"], "scope": ["files", "local"]}],
  "settings": {"title": "Places", "sections": [{"title": "Place", "items": [...]}]}
}
```

| | How the blob is made |
|---|---|
| Local dev / tests | `cmake --build build --target mv_geo_data` (or `-DMV_GEO_DATA=ON`) → `build/geo/places.mvgeo`; downloads cached in `build/geo-downloads` (`MV_GEO_DOWNLOADS`) |
| CI | `test-media` builds, caches, restores into `tools/testmedia/geo/` |
| Packaging | `addon-pack.py stage-places --src build/addons/places --downloads <cache> [--blob <built>]` → `data/places.mvgeo` (magic checked, nothing else in `data/`), then `addon-pack.py pack --addon places`. Preferred, not required: `publish` uploads a complete pack, publishes without one, refuses a partial or mismatched one. `MV_RELEASE_ADDONS=1` requires Import only |

Data licence in the signed list: `CC-BY-4.0 AND LicenseRef-PublicDomain`; `LICENSES/NOTICE.txt`
carries the GeoNames attribution and Natural Earth line. Zip ~10 MB, 17 MB installed.

### The pane section

The pane's last item, on every tab, when the file has coordinates. The host asks `info_section`
on the add-on's worker with the location, its coordinate text, theme tokens, content width and
backing scale. Places answers with one tree and one surface:

1. title: the place label, copyable;
2. secondary: the host's coordinates, copyable;
3. a 160 pt `surface` "map" with a `pin`, `marks` (town names) and `interactive` bounds; one
   accessibility element "Map of <place>";
4. caption "No street detail — offline map" with Zoom out / Zoom in (`minus` / `plus`);
5. for a file with a folder, the **This folder** toggle; and **Open map** (the world map).

Hosts have no Places-specific code. A location change is a new info request; only pictures are
cached, by point.

### The map

Drawn on the add-on's worker into premultiplied 8-bit BGRA at width × 160 pt × backing scale
(`map.cpp`), at most 4096 px a side (the scale is lowered, never the map dropped).

- **Projection:** Web Mercator, `span_deg` = picture height in latitude degrees; rings unwrapped
  and drawn at every whole-world shift that reaches the picture.
- **Fill:** signed-area accumulation (font-rs), even-odd, anti-aliased; photo's country
  `land_home`, others `land`.
- **Borders:** edges shared by two polygons (coasts never stroked); country 0.6 pt, admin-1
  0.35 pt at span ≤ 9° (≤ 48 regions) or ≤ 4.5°.
- **Towns:** from span 9°, population over `1000 · (span / 0.2°)^1.5`, ≤ 48, 14 pt apart; first
  16 names as `marks`.
- **Frame and colours:** 8 pt rounded corners; colours from the host's theme tokens.
- **Cache:** last 12 pictures by (centre, span, size, scale, palette, country).

**Zooming the map.** Pinch or wheel zooms about the cursor, drag pans, double-click zooms in one
step, −/+ step about the centre; a two-finger scroll scrolls the pane. The chrome moves the last
picture live and sends one event on settle (`MV_SLOT_EVENT_ZOOM {scale, x, y}`, `PAN {dx, dy}`,
`DOUBLE {x, y}`). `apply_gesture` maps p → `anchor + (p − anchor) · scale + pan` at the exact
pixel scale shown, then clamps span to [0.2°, 40°] (`interactive.min_scale` / `max_scale`) and
keeps the pin inside an 8 % margin. Places redraws, posts the surface, then a diff for pin, marks
and buttons.

### The folder map (#601)

The **This folder** toggle (`toggle` node `folder`; not for Photos library items) draws every
geotagged item of the open folder on the map. Read-only.

- **Session state**, not a setting: stays on across files and folders; off in a new process.
- **Points** come from host table `locations_in` under the section's generation (a stale walk
  stops with `MV_ERR_CANCELLED`), kept per folder (`folder_points`, ≤ 50 000).
- **Dots and clusters** (`folder_map.h/.cpp`), on a copy of the cached base: 22 pt cells anchored
  at 0°/0° (pans keep clusters), one dot per cell, overlapping neighbours merged; 3.5–8.5 pt by
  log₂ count. **The current item is the pin**, never a dot.
- **A click opens a dot's file.** Hosts send `MV_SLOT_EVENT_TAP {x, y}`; Places hits the nearest
  dot within radius + 5 pt and calls host table `open_item`. The registry accepts only a file of
  the current folder and generation, carried in `info_json`'s `"open": {path, revision}`.
- **Idle:** drawn once per section, toggle, gesture or folder read (~0.3 ms for 600 items at 2×).

### The world map

Places' own window `world` (contribution API 12; [plan](../plans/places-world-map.md)): the same
map in a resizable, full-screen-capable window, from the whole world to town scale, with every
geotagged item of the **library**. Opened from **Open map** under the section's map and the
command bar (**World Map**). Read-only, offline.

- **Points:** every Local search index asset with coordinates (`locations_of_index`; a clip is
  its first fix, `ROUTE_POINT` rows skipped). If no index is attached it asks the host to start the
  pack (slots change bit 128, or `mv_slots_index_unavailable`), waiting ≤ 60 s. Without the AI pack
  or any indexed place it shows the current folder (`locations_in`) and says so. ≤ 400 000 items
  (`world_points`). The file on screen is the pin.
- **View** (`world_map.h/.cpp`): the section's renderer, `map_view::max_span_deg` =
  `360 · H · cos(lat) / W`; first view frames the library's box; backing scale capped at 4096 px a
  side and 8 MP. Surface `"fill": true`, resized via `MV_SLOT_EVENT_RESIZE` on settle.
- **Dots** are the folder map's (`cluster_points`). A click shows the dot's items (≤ 10 000) as a
  viewer result list titled "<place> (<count>)" via host table `open_list`.
- **Idle:** nothing ticks; closing drops the points and cache. A cache miss at 2880 × 1800 px is
  25–40 ms on the worker.

### Search by place

The provider handles `place:` and `in:` (`in:` plus a digit is Local search's date term). For a
folder it walks the host's cached `locations_in`, looks each file up, and posts rows in batches
of 128 with the place label as `why`. Rule (`place_query`): the folded query equals a country or
region name in any language, or starts one at ≥ 4 letters; or equals (or, at 4 letters, starts)
the file's town. Sea points are in no country; a query starting with a digit is never a place. A
Local search request (folder "") walks `locations_of_index` under the same rule; a host without
it gets `done` with no rows.

In Local search the AI pack asks Places: per place term (`place:portugal`, `in:lisbon`,
`-place:spain`) the engine calls host table `place_matches` and intersects the returned paths with
the rest of the query ([17](17-local-ai-search.md) "Places over the index"). **Without Places a
place term matches nothing** and the panel says to install it. File search routes `place:` to
Places directly.

### Routes (#606)

A GoPro or DJI clip's position is the first fix of its telemetry GPS route
([06](06-metadata.md) "Telemetry routes"), so it gets a Place section like a photo.

- **`route_of(path, out, capacity, &count)`** (host table): the whole route thinned to `capacity`
  evenly spaced points, first and last kept (`meta::read_route`, installed via `abi/addon_media`).
  No fix → 0 points. The host caches the last 32 answers by path, size, mtime, capacity.
- **`mv_addon_location.has` is a bit set** (`MV_ADDON_LOCATION_*`): `HAS` on every row with
  coordinates; `ROUTE` on a clip whose position is a route's first fix (`location_of`,
  `locations_in`, from meta_cache.db's `has_route`); `ROUTE_POINT` on a further point of the
  previous row's route (`locations_of_index`, from the AI pack's `tracks`).

**On the map:** the section asks `route_of` (≤ 4 096 points); `draw_route` draws a 2 pt accent
line on a 1 pt surface-colour casing over a copy of the cached picture, unwrapped across 180°,
skipping sub-half-pixel moves. The last routed picture is kept beside the base. `frame_route`
widens the first view until the route fits with a quarter margin.

**In search:** by folder, a non-matching `ROUTE` row has its route asked at 128 points and looked
up point by point (`route_why`); the first hit is the `why`. Over the index, `ROUTE_POINT` rows
are looked up until one matches. So `place:spain` finds a Lisbon-to-Badajoz drive.

### Settings (PR 56 schema)

Settings → Add-ons lists Places even when not installed (optional, preferred). Install, update
and remove use the same channel as Import ([18](18-import.md)); a fresh install loads at once.

One page, "Place" (`manifest.json`):
- `show_map`: toggle, default on.
- `show_town`: toggle, default on.
- `town_radius_km`: 1–50 km, default 15, `enabled_by: show_town`.
- `label_names`: `english` | `local`.

Read through host table `settings_get` on every info request, search and map event (cached),
mapped onto `lookup_options` (`town_radius_km`, 0 when `show_town` is off; `native_names`) and the
map's presence. A change re-asks the section for the file on screen under a new generation
([25](25-open-addons.md) "A changed setting re-asks"); a pending gesture is dropped. No setting
changes map pixels. Listed search results are not re-run.

## Known limits

- Natural Earth 1:10m omits micro-enclaves (Baarle-Hertog, Campione, Büsingen); borders good to
  ~1 km.
- Names are GeoNames' English names ("Sevilla", "State of Berlin"); countries English or native.
- Towns are ≥ 1 000 people; smaller places read as the nearest town or "Region, Country".
- The map has no streets, rivers, roads, terrain or labels beyond towns.
- Routes are searched at 128 evenly spaced points, so a brief crossing can be missed; a route
  wider than 40° shows only its part around the pin.

Not built: a mirrored GeoNames snapshot (the release passes `--accept-upstream`, so the two
platforms' packages can differ by a day of town data).
