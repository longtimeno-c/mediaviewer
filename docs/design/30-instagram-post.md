# 30 — Instagram Post (an installable add-on)

Build a carousel for Instagram: choose up to 20 photos and clips from the Mac Photos library or the
open folder, set one shape for them all, frame each one, and export files ready to post. Owner
request 2026-11-09. Both platforms share one library (`src/addons/instagram`); the hosts draw its
window from view trees ([25 §10](25-open-addons.md#10-screens)). It is offered in Settings → Add-ons
and shipped with stable releases as `mediaviewer-addon-instagram-*`.

It writes files on this computer. **Nothing is uploaded**: no account, no token, no network.

## The window

```
┌ Instagram Post ───────────────────────────────────────────────────┐
│ [1:1|4:5|3:4|16:9|1.91:1]                         3 of 20 [Export]│
│ ▣ ▣ ▣ ▢ ▢ ▢ ▢   the carousel, at the shape it will be posted  ⇢   │
│ [Move earlier][Move later][Remove]            Saved 3 …  [Show]   │
│───────────────────────────────────────────────────────────────────│
│ [Choose|Adjust|Enhance] [Photos|This folder] [Search ] [Show more]│
│ ▢ ▢ ▢ ▢①▢ ▢ ▢ ▢②▢ ▢ ▢ ▢ ▢ ▢ ▢   the gallery, newest first         │
└───────────────────────────────────────────────────────────────────┘
```

- **Choosing.** A click in the gallery adds to or removes from the post. The number on a tile is its
  place in the post. A 21st is refused with "A post holds 20".
- **Shape.** 1:1, 4:5 (default), 3:4, 16:9, 1.91:1; one for the whole post, as Instagram requires of a
  carousel, and it can change at any time. An item's framing is kept apart from the shape (a centre
  and a zoom, `post.h` `frame`), so a change of shape keeps every item's framing.
- **Framing.** A click on a tile focuses it (the ring). Drag inside it to move the picture, scroll or
  pinch to zoom, double-click to go back to the whole picture. *Move earlier*, *Move later* and
  *Remove* act on the focused tile. The tools row is always laid out, so the gallery never jumps.
- **Adjust and Enhance, in this window.** A switch beside the search (*Choose · Adjust · Enhance*) swaps
  the lower half for a large preview of the focused photo and its tools. *Adjust* has Auto, Reset and
  the Edit workspace's Light, Colour, Black & White and Vignette sliders, run by the same colour chain
  (`adjust_rgb`). They are kept with the post and baked in at export, so trying a look writes nothing.
  *Enhance* is Enhance's own panel drawn here (an `embed` node on its place-`embed` screen `panel`),
  with its options and downloads; what it makes takes the photo's place, in the same position and
  framing, and the panel follows the new file. Remove needs the brush on the Edit canvas, so it stays
  in Edit; the panel offers everything else Enhance has for a photo. Photos only: a clip shows no tools.
- **Sources.** *This folder* is the folder of the file on screen. *Photos* appears once the library
  answers (Mac, added in Settings: [26](26-photos-library.md)); a library item on screen starts there.
- **Search.** Names filter on every keystroke. After a pause Local search is asked too, when
  installed, and what the picture shows comes first. Without it the name filter is the whole search.
- **Gallery.** Pages of 3,000 tiles with *Show N more*: every selection revision carries the tree to
  the chrome, and a page keeps a click cheap. Marked files (the command) and a context-menu selection
  start the post.
- **Closing** cancels an export and keeps the post until the app quits.

## Export

To `Pictures/MediaViewer/Instagram/<date time>/`, numbered in post order (`01.jpg`, `02.mov`, …):

| Shape | Size |
|---|---|
| 1:1 | 1080 × 1080 |
| 4:5 | 1080 × 1350 |
| 3:4 | 1080 × 1440 |
| 16:9 | 1080 × 608 |
| 1.91:1 | 1080 × 566 |

- **Stills:** the full decode (a RAW's own develop), cropped, scaled in linear light, JPEG 92, sRGB
  tagged, **no metadata**: a file made to go online does not carry its GPS. Originals are only read
  (rule 5).
- **Clips:** every frame turned upright, cropped and scaled, on a hardware encoder ([11](11-licensing.md));
  the source's sound is copied. The output keeps MP4 / MOV and is Matroska for anything else, which
  Instagram will not take. Length is not trimmed; Instagram's limits are Instagram's.
- A library item's original is fetched from iCloud if this Mac lacks it, as *Copy To* does: the person
  asked for that item. Choosing never fetches: tiles use the Mac's own cached rendition.

## How it is built

`post` (the rules and crop arithmetic, no I/O), `pixels` (the scaler, built once per size and reused
for each frame), `view` (the trees), `engine` (state and threads, behind `engine_port`), `addon_entry`
(the host table). Listing and pictures run on one lane, Local search on another, the export on its
own thread; events return at once.

Host additions (ABI 0.40, contribution API 15, host table rows appended after `still_extend`):
`library_items`, `item_preview_rgb`, `library_file`, `index_search`, `still_write_into`, `adjust_rgb`,
`adjust_auto`, `embed_open` / `embed_close` and `item_saved` (delivered as `MV_SLOT_EVENT_ITEM_SAVED`);
`mv_addon_clip_out` gained a folder, a stem and a rotation. In the vocabulary: `numbered` and a row
`badge` on `thumbnail_grid`, `fill` on it too, `animate` and `justify` on `stack`, `look: segmented`
on `choice`, `live` on `slider`, `fill` on `split`, and the `embed` node. `mv_slots_thumbnail_path` now answers library keys and clips.

## Design: Stageshow's rules on a real screen

The owner asked for the window to follow Stageshow (benji.org/stageshow). Its kit is React and cannot
ship in native chrome (CLAUDE.md, D1), so what is taken is its rules:

- **One focal object.** The carousel, at the shape it will be posted, on real pictures. Empty places
  hold the shape, so it never reads as a blank band.
- **Restraint.** The real words are the shapes, the count, Search and Export. No masthead (the title
  bar names it), no logo, no sparkle.
- **Every motion has a cause.** The strip is an `animate` stack: a tile arrives where it was chosen,
  neighbours make room, and a new shape eases, pixels following. Nothing moves on its own.
- **Controls change on themselves.** Export becomes Cancel on the same button; the tools row stays laid out.

On Windows a revision rebuilds the content, so `animate` is a fade-in of what is new; the slide and
the shape ease are the Mac's. `Reduce Motion` and Windows' animations setting turn all of it off.

## Not built

Drag to reorder (the buttons do it). Publishing to Instagram, which needs an account and Meta's API.
Trimming a clip to Instagram's length. Keyboard selection of a strip tile: `surface` takes no focus.

## Verify

`mv_import_tests "[instagram]"` on both platforms' CI; the window itself is in `docs/DEVELOPMENT.md`.
