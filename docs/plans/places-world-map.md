# Places: the world map

The Place section's map ([27](../design/27-places.md)) is 160 pt tall, stops at a 40° span and
shows one folder at most. The owner asked (2026-11-04) for the map **in a window of its own that
can go full screen**: every geotagged item of the **whole library** on it, zoom out to the world
and back in to a town, and a click on a dot opens those photos. This page is how; what lands is
folded into 27 and 25.

It stays read-only and offline: no tiles, no street detail, nothing leaves the machine (rule 6).
Picking a point to *write* coordinates stays the backlogged feature it is ([06](../design/06-metadata.md)).

## Shape

- **The window.** Places contributes a screen `world` (place `window`, "Places", 1100 × 720 pt,
  resizable, and full screen on both hosts). It opens from an **Open map** button under the
  section's map and from the command bar (manifest command `places.world`). One window; opening
  it again brings it forward.
- **What it shows.** Every asset of the Local search index with coordinates
  (`locations_of_index`: index.db's own lat / lon, no file read, streamed in batches). A route
  point row (#606) is not a dot: a clip is one dot at its first fix. Without the AI pack, or
  before anything is indexed, it shows the open folder (`locations_in`, as the section's toggle)
  and says so under the map: "Showing this folder. Index your library in Local search to see
  everything." The file on screen when the window opened is the pin.
- **Dots and clusters** are the folder map's layer (`folder_map.h`), unchanged: a world-anchored
  22 pt grid, overlapping dots merged, a cluster's size by log₂ of its count. At world zoom a
  library is a few hundred clusters.
- **Zoom.** The section's gestures (pinch, wheel, drag, double-click, −/+), from the whole world
  (the world's width fills the window's, about 170° of latitude on a wide window) down to the
  section's town scale (0.2°). The section keeps its 40° limit; the cap is a field of `map_view`
  (`max_span_deg`), not a second renderer. Region borders and towns appear at the spans they
  already do.
- **A click on a dot** shows its items in the viewer as a **result list** titled with the place
  under the dot ("Sintra, Lisbon, Portugal"), the clicked dot's first item on the canvas, in the
  order the index gave them: the same listing Local search results open (Mac `mv_chrome_open_list`,
  Windows `mv_folder_open_list`), so the person arrows through that place's photos and "Back to
  folder" returns. The viewer window comes forward; the map stays open behind it.
- **Idle.** Nothing ticks. The picture is drawn on Places' worker once per open, resize,
  gesture or index read; the chrome shows the posted pixels and moves them itself during a
  gesture. The canvas presents nothing while the window is open and still.

## What the host gains (contribution API 12, host table)

1. **A surface that fills** (`"fill": true` on a `surface` node, API 12). It takes the room its
   parent gives on both axes (`width` / `height` then optional and ignored). The chrome reports
   the laid-out size as a new event, `MV_SLOT_EVENT_RESIZE` (15) `{"width", "height"}` in
   points plus `"scale"`, once at first layout and again when a resize settles (live resize
   ended on the Mac, `SizeChanged` quiet for 150 ms on Windows); never per frame of a drag. The
   add-on draws for that size, at a backing scale it lowers to keep each side within 4096 px.
2. **`open_list`** in the host table (appended after `still_mask`):
   `open_list(host, screen_id, generation, title_utf8, paths_utf8, count, select)`. From an
   open window, pane or sheet of the add-on (never an info section), under its generation: show
   `paths` as a result list titled `title`, item `select` on the canvas, and bring the viewer
   forward. Each path is an absolute file path or a `photos:` key, valid UTF-8, at most 4096
   bytes; 1–10 000 of them. The registry keeps the last request with a growing revision
   (`mv_slots_open_list_json`, ABI 0.38); each chrome acts on a revision once. It opens and
   selects only, never writes; a file that is gone is skipped by the listing as a search
   result's would be.

Both are generic: any window screen can fill with pixels and hand the viewer a list.

## Slices

One PR, both hosts (D9): the core (screen.cpp's `fill` and `RESIZE`, the registry's
`open_list`, the ABI), Places (the screen, `max_span_deg`, the world view, the walk, the click),
the SwiftUI half (`ScreenView` fill + resize, `SlotStore` open list, full screen on the window)
and the WinUI half (`IslandHost.Screens` fill + resize, `IslandHost.Slots` open list, a
maximisable window).

## Verify

On the owner's library with the AI index built, on both platforms: **Open map** shows every
geotagged item of the index; zoom from the whole world to a town and back with pinch, wheel and
drag; full screen works; a click on a dot shows that place's photos in the viewer, and arrowing
walks them. Measured: the window's redraw at 1440 × 900 pt and 2× at the world, country and town
spans (median, p90, max, on the full place blob), and the index walk for the owner's library;
with the window open and still, 0 presents and ~0 % CPU; `--browse-soak` with the window open
against base; both PR 1 present-loop gates hold.

## Not in this slice

Hover thumbnails over a dot, routes on the world map, a default key, a heat map, the Linux half
(after the Linux catch-up order is decided, [linux](linux.md) §10).
