# 14 — The C ABI

The flat C ABI between the hosts' chrome and the C++ core. The headers in
[`src/abi/include/mediaviewer/`](../../src/abi/include/mediaviewer/) are the source of truth for
every signature, struct and enum; this doc gives the conventions and the semantics the headers do
not spell out.

The Windows host is C# WinUI over `mediaviewer_core.dll`, through P/Invoke
([`src.managed/MediaViewer.Interop`](../../src.managed/MediaViewer.Interop/)). The Mac host links
the core statically and calls it directly; it creates no ABI session but uses the same portable
pieces (`abi/clip_session`, the add-on host table, `player/playback_hold.h`) and reaches most
features through its chrome bridge (`mv_chrome_*` in `mv_chrome_bridge.h`).

| Header | Contents |
|---|---|
| `mediaviewer.h` | Version, status, session, generations, completions, image open, folder model, result listings, video transport |
| `mediaviewer_clip.h` | Clip jobs and the keyframe index |
| `mediaviewer_crash.h` | Crash-report context slots |
| `mediaviewer_addon.h` | Add-on entry point, host function table, add-on management, slots, scripts, phone |
| `mediaviewer_import.h` | The Import add-on's interface (`mv.import.1`) |
| `mediaviewer_ai.h` | The AI pack's interface (`mv.ai.1`) and the read-only reader entry |

## Shape

No C++ types cross: no `std::`, exceptions, RTTI, COM or inheritance. Only opaque handles, POD
structs with fixed-width fields and explicit `reserved` padding, UTF-8 `const char*` with
documented ownership, and function pointers always paired with a `void* user_data` / `ctx`.
Everything is `extern "C"`; `MV_API` is `dllexport`/`dllimport` on Windows (export under
`MV_BUILDING_CORE`), `MV_CALL` is `__cdecl`; both are empty elsewhere.

**Versioning.** `mv_abi_version()` returns `(major << 16) | minor`; the per-minor changelog is the
comment on `MV_ABI_VERSION_MINOR` in `mediaviewer.h`. `MediaViewerSession.Create` throws unless the
DLL's major equals `ExpectedAbiMajor`. Minor bumps are additive: layouts never change; new meaning
goes into a former `reserved` field, a new flag bit, or fields appended behind a `struct_size`
(older callers' sizes stay accepted, e.g. `mv_clip_request`).

## Error model

Every fallible call returns `mv_status` (`core/status.h` mirrors the values).
`mv_status_name` returns a static literal. `mv_last_error_message` (never NULL, `""` when none)
and `mv_last_error_correlation_id` are per-thread, core-owned and valid until the next call on that
thread; C# copies them at once.

No exception crosses. Every export is wrapped in `mv::abi::guard("name", lambda)`
([`src/abi/guard.h`](../../src/abi/guard.h)), which takes a fresh process-wide **correlation id**,
stamps the thread's last-error slot, records the id as the last in-flight call for crash reports,
traces the call, and maps `std::bad_alloc` → `MV_ERR_OUT_OF_MEMORY` and anything else →
`MV_ERR_INTERNAL` with the message kept. The id ties a managed `MediaViewerException` to the native
minidump ([13](13-updates-and-telemetry.md)); every completion carries one too.

## Ownership — the side that allocates, frees

| Direction | Rule |
|---|---|
| Core → managed, strings | Copied into a caller buffer (the common case), or a core-owned per-thread pointer (last error), or a static literal |
| Core → managed, data | POD filled by value; scalar completion payloads. Never a pointer the caller frees |
| Managed → core, strings and buffers | Caller owns; read during the call only, never retained |

**Caller-buffer strings** (every `…_name` / `…_path` / text call): UTF-8; `out_bytes` (optional)
gets the required size including the NUL; `cap == 0` returns the size and `MV_ERR_INVALID_ARG`; a
short buffer is truncated with a NUL and still reports the full size. **JSON calls** use `cap` /
`needed`: short → `MV_ERR_INVALID_ARG` with `needed` set; retry. `mv_folder_entries` (whole listing,
one call, one lock) writes nothing when short and reports both sizes.

## Handles and `SafeHandle`

The only opaque handle is the **session**, reference-counted: `mv_session_create` returns one
reference; `retain` / `release` adjust it. C# wraps it in `MvSessionHandle :
SafeHandleZeroOrMinusOneIsInvalid`, whose `ReleaseHandle` calls `mv_session_release` exactly once;
the wrapper never calls `retain`. The completion wait handle is session-owned; C# wraps it in a
`SafeWaitHandle` with `ownsHandle: false` (never `CloseHandle` it). Images, frames and textures
have no ABI handle.

`mv_session_config.worker_count` 0 → `max(1, cores − 2)`; `enable_etw` registers the ETW provider.

## Thread contract

Each function is annotated in the header:

| Annotation | Meaning |
|---|---|
| `[any-thread]` | Safe from any thread, concurrently |
| `[no-block]` | No I/O, decode or lock contention — safe on the UI thread |
| `[ui-thread]` | Chrome UI thread only (add-on `shutdown`, `mv_addon_unload`, `mv_addon_quit`) |
| `[worker-thread]` | May read files, hash or run a model |

Almost every session call is `[any-thread][no-block]`: it returns a job id and the answer arrives
as a completion. Worker-only calls touch the disk directly: `mv_list_subdirectories`,
`mv_storage_report_json`, `mv_thumb_cache_clear`, add-on management and open-add-on calls,
`mv_script_reload` / `_restart`, `mv_slots_thumbnail_path`, `mv_eyes_json`, `mv_adjust_faces*`.

## Generations

Every job carries the generation current at submission; `mv_session_bump_generation` abandons
everything queued at the old one at its next check ([02](02-architecture.md)). `mv_image_open`
submits at the **current** generation (bump first to replace the view; without a bump it replaces
rather than cancels). `mv_folder_select` and `mv_video_open` bump the view generation themselves.
Thumbnail jobs ride a separate **folder** generation so arrow keys do not cancel the filmstrip.

## Completions — the core never touches the dispatcher

The core never calls a host dispatcher or managed callback from a worker. It owns a completion
queue (`mv_completion`, 40 bytes, payload a kind-specific scalar, never a pointer); the host waits
on `mv_completion_wait_handle` (manual-reset) and calls `mv_completion_drain`.

| Kind | Payload |
|---|---|
| `ECHO` 1 | byte length of the echoed text |
| `IMAGE_OPENED` 2 | `(width << 32) \| height` |
| `FOLDER_READY` 3 / `FOLDER_CHANGED` 4 | stop count (`CHANGED`: watcher relist or re-sort) |
| `THUMB_READY` 5 | item index |
| `FOLDER_SELECTED` 6 | selected index |
| `VIDEO_OPENED` 7 / `VIDEO_ENDED` 8 | duration ns / 0 |
| `VIDEO_STATE` 9 | `mv_play_state`, on every transition including host-requested ones |
| `FOLDER_SUMMARY` 10 | subfolder index |
| `CLIP_INDEX` 11 / `CLIP_JOB` 12 | keyframe count / `mv_clip_job_state` |
| `PAGE_THUMB_READY` 13 | page; `generation` = the serial `mv_folder_page_thumbs` returned |
| `ADDON` 100 | event payload; `job_id` = event id, `generation` = `mv_addon_event_kind` |

- Draining is non-blocking and batched: a folder's 400 thumbnails are one drain, not 400.
- `IMAGE_OPENED` means pixels may be on the native canvas; draining does not wake an idle render
  thread — the shell wakes the presenter ([03](03-rendering.md)).
- The queue caps at 65,536: a host that stops draining loses the oldest quarter, counted and
  logged, never a terminate.
- `mv_session_echo` is the minimal round trip; `mv_session_job_stats` feeds F3 and tests.
- Some features use a **listener** instead: one documented any-thread callback the host marshals
  (`mv_slots_set_listener`, `mv_script_set_listener`; the Windows shell `PostMessageW`s to its
  window). These are the shell's events, not the chrome's queue.

## What crosses, and what does not

**Crosses:** open/close, navigation intent, folder item records, sort and filter state, transport
commands, clip-job requests and progress, UTF-8 paths (including on-disk JPEG thumbnail paths),
add-on JSON and POD status.

**Does not cross:** pixels, textures, decoded frames, `ID3D11*` anything. The swapchain lives in
C++; managed code sends its size and input (through the published input snapshot — there is no
input callback). Edits stay in `shell/edit_session` (shared with the Mac host); geometry reaches
the render thread through the input snapshot. Key bindings and `command_id` stay in the shell
([16](16-commands.md)). The one pixel handover is the add-on host table, in-process, to an
add-on (below).

**Native-only side door** ([`src/abi/native.h`](../../src/abi/native.h)): exported for the C++
present lab, never P/Invoked — device attach/detach, the wait-free ready-image and video-frame
handoffs, tile and animation-ring access, `poll_video`. The navigation GPU LRU is native-only.

## Image open

`mv_image_open` → `IMAGE_OPENED`; `mv_session_image_info` describes the last opened image
(`page_count` 1 for a single-page still). `mv_folder_select_page` opens page N of the selected
stop (page 0 is what `mv_folder_select` showed).

## Folder model

- **Stops, not files.** RAW+JPEG/HEIC and Live Photos (HEIC/JPG+MOV) pair at scan time;
  ambiguous groups stay separate. Name, path, size, mtime, thumbs, decode and prefetch use the
  **primary** (the still); `mv_folder_item_pair_path` returns the secondary (empty, `out_bytes` 1,
  when unpaired). A Live Photo's motion opens with `mv_video_open` on the pair path. Counts are
  stops; copy/move/drag/recycle expand to both halves host-side.
- `mv_folder_item.flags`: bit 0 selected, bit 1 primary is RAW, bit 2 primary is a video **by
  extension** (`io::is_video_name`; the play badge; never on a Live Photo; nothing probed).
- `mv_folder_open`: `utf8_select_path` may be NULL (index 0); either half of a pair selects its
  stop. The folder is watched; a change relists → `FOLDER_CHANGED`.
- `mv_folder_select` bumps the view generation, publishes an LRU hit or opens, prefetches ±2, and
  pushes `FOLDER_SELECTED` ([04](04-image-pipeline.md)).
- Thumb path is empty until that index's `THUMB_READY`; `mv_folder_thumbs_visible` prioritises
  on-screen thumbs (all are generated regardless).
- **Page sidebar:** `mv_folder_page_thumbs` replaces the requested window; one background job
  renders from one open (`image/page_thumbs.h`), cached as `path#page=N`; nothing is asked twice.
  A completion with an older serial is stale; `mv_folder_page_thumb_path` is `MV_ERR_NOT_FOUND`
  until ready.
- **Sort and filters** are session-owned so filmstrip, gallery and arrows read one list. Setting
  re-sorts on a worker, keeps the current stop, pushes `FOLDER_CHANGED`. Unknown sort-key bits mean
  name. `mv_folder_set_kind_filter` (View ▸ Show) reads ratings/labels on a worker before its relist
  and moves off a stop that fails; `mv_folder_note_metadata_written` drops its cache for one file.
  The opened file and current stop survive `mv_folder_set_hidden_kinds`.
- `mv_folder_renamed` is called **before** an F2 rename moves the file; the relist keeps the stop
  selected under the new name with no decode.
- `mv_folder_forget` drops a rewritten file's pixels from the path-keyed LRU (`MV_OK` if absent);
  `mv_folder_forget_tags` re-reads its tags. `mv_folder_tag_search` answers from the tag pass,
  never a file read.
- **Child folders** ride the same relist (`io::list_subfolders`, natural order, housekeeping
  folders dropped; unreadable subfolders still show the files). Summaries are lazy, a bounded
  walk on a pool thread; `mv_folder_request_summary` is a no-op when loaded or out of range.
- `mv_list_subdirectories` takes no session, worker only: `"name\tpath\n"` per subfolder (hidden,
  system and dot dirs skipped, case-insensitive; tabs/newlines in names become spaces); `MV_ERR_IO`
  if unreadable.
- **Storage:** `mv_thumb_cache_set_cap` is process-wide (0 = none) and applies to the next sweep;
  the session sweeps once after its first `IMAGE_OPENED`.
- `mv_meta_parse_location` is pure (no session): the Location editor's text → point, blank or
  invalid (with why).

## Result listings

`mv_folder_open_list` shows a non-directory listing (search results) through the same gallery,
filmstrip, selection and thumbnail cache. Items keep the given order (sort does not apply), are
never paired and not watched. An item with moment ≥ 0 is a clip that opens **paused** on that
frame (exact seek); `moments_ms` may be NULL. While a list is open `mv_folder_directory` is `""`;
`mv_folder_open` of a directory ends it. The Mac host has a direct twin.

## Video

Times are `int64` nanoseconds ([05](05-video-pipeline.md)). `mv_video_open` bumps the view
generation → `VIDEO_OPENED`; `close` is idempotent. `seek(exact=0)` lands on the nearest keyframe
(scrub), `1` decodes to the frame. `step` works paused only. `set_rate` clamps to 0.25–4.0
(pitch-corrected). `set_loop` with `b < 0` clears. `mv_probe_is_video` uses magic bytes.
`mv_video_set_hold` (gallery covers the canvas) pauses and adopts new clips paused on their poster;
release resumes only the clip that was playing, if still on screen and `resume` is 1
(`player/playback_hold.h`). `mv_video_set_sound_look` is the Video Editor's gain envelope,
separate from the listener's volume and mute.

## Clip jobs

`mediaviewer_clip.h`, all `[any-thread][no-block]`; the logic is the portable `abi/clip_session`,
one queue on both hosts ([08](08-video-editing.md)).

- The keyframe index is read from packets (never decoded) on a pool worker; only the newest
  request is read, a superseded one answers `MV_ERR_CANCELLED`. The session keeps the last few.
- Jobs run on the clip queue's single worker. Jobs that need a decoder or encoder run in the
  `MediaViewerClipJob` helper (`mv_clip_set_helper`; `MV_ERR_IO` if it cannot start);
  stream-copy runs in process. Cancel leaves nothing behind; retry makes a new job; split has two
  outputs.
- `mv_clip_request.look`: keyframe `KEEP_RANGES` carries only a turn and mute
  (`MV_ERR_INVALID_ARG` for more); exact renders the look in the helper; `FRAME` applies turn,
  mirror and crop.
- Outputs are new files beside the source under a free name (`IMG_0001_trimmed.mp4`, then
  `" (2)"`), written to a hidden temporary first. Never the source; nothing logs a path; no pixel
  crosses.

## Crash context

`mv_crash_context` returns stable addresses: fixed-size NUL-padded ASCII slots (one per in-flight
decode, claimed by `begin_decode`) and a pointer to the last ABI call's correlation id. The host
registers them with its out-of-process reporter; the core links none. Slots hold format family,
decoder name/version, dimensions, bit depth and correlation id only — never a path, filename,
bytes or EXIF ([13](13-updates-and-telemetry.md)).

## Add-ons

`mediaviewer_addon.h`; the mechanism is in [18](18-import.md), open add-ons in
[25](25-open-addons.md). An add-on is one shared library exporting `mv_addon_get(host_api,
const mv_host_api*, mv_addon_api* out)`; it links nothing of the core.

- **Host API version.** An add-on loads when its manifest range meets
  `[MV_ADDON_HOST_API_OLDEST, MV_ADDON_HOST_API]` and receives `min(HOST_API, its max)`; otherwise
  `MV_ERR_UNSUPPORTED_FORMAT` ("needs an update"). The table only appends; most rows are appended
  **without** a version bump and are read only when `struct_size` covers them, so callers on both
  sides check `struct_size` per row. The AI pack copies what the host's `struct_size` covers.
- **`mv_host_api`** gives files (walk, stat, hash — BLAKE3-256, verified multi-target copy,
  write-new-file, `recycle_file` to the Recycle Bin/Trash only), volumes, the viewer's pairing and
  thumbnails, scheduling (`should_yield` while the present loop is busy — set by
  `mv_present_set_busy`; `post_event` to the completion queue), `log` (never a path, name or hash),
  and media for the AI pack and Enhance: decoded stills, frame samplers (HDR tone-mapped to SDR),
  sound and frame read/write, still read/write. Pixels are 8-bit sRGB RGB, stride `width * 3`,
  worker threads only. New files are published under a free name only when finished.
- **Edit state for add-ons.** `still_open` answers the photo being edited with its edit applied;
  `still_mask` (Remove brush dabs), `still_extend` (Extend margins, all 0 when unset or another
  photo is asked) and `still_faces` describe it. Windows sets these through `mv_edit_still_set` /
  `_mask` / `_extend` (an opaque blob: host and core are built together); the Mac host sets them
  directly (`addon_media::set_*`). `still_faces` / `mv_eyes_json`: `MV_ERR_NOT_FOUND` without the
  pack, refused while People is off.
- **`mv_addon_api`**: `id`, `version`, `shutdown` (`[ui-thread]`), and `query(interface_id)` →
  `"mv.import.1"`, `"mv.ai.1"` ([17](17-local-ai-search.md)) or `"mv.slots.1"`. The AI pack also
  exports `mv_ai_reader_get`, a read-only instance for the Final Cut Pro agent. Optional
  `mv_addon_forget(data_dir)`: on Remove without keeping data, after shutdown, the host loads the
  verified library alone and calls it so it deletes what it keeps outside its data folder; a
  failure never blocks removal.
- **Events** post as `ADDON` completions (`generation` = event kind): 1–9 Import, 20–24 AI pack.
- **Host-side management** (Windows DLL exports for C#; the Mac host calls `src/addon` directly),
  worker-thread: install/check (Ed25519 against the pinned key)/remove/load. `mv_addon_load`
  re-verifies before loading; `MV_ERR_IO` when the OS refuses a library that verified (reason in
  `mv_last_error_message`). `mv_addon_load_reader` (AI pack's read-only door; `MV_ERR_NOT_FOUND`
  when nothing is indexed) and `mv_addon_load_guest` serve windows that do not host the add-ons
  and never sweep the store. `mv_addon_quit_wait` → `MV_ERR_TIMEOUT` means the host terminates
  without static destructors.

## PR 55 — open add-ons (ABI 0.17)

`mv_open_addon_*` ([25](25-open-addons.md)): all `[worker-thread]`, JSON out with the `cap` /
`needed` rule, thin wrappers over `src/addon/open_json.h` (the one JSON both chromes read; the Mac
reaches it through `mv_open_addons_*`). `inspect` returns `MV_OK` whenever it wrote JSON — a
refused package is an answer, not an error. `install` takes the SHA-256 `inspect` returned, so what
is installed is what the person was shown. Install/remove reload scripts themselves.

Related calls in the same header: `mv_addon_commands_json` (manifest command rows, made live by the
Windows shell), `mv_addon_settings_*` / `mv_open_addon_keymap_json` (settings pages, checked on
set; keymap packs native applies).

## Slots, scripts and phone

- **`mv_slots_*`** (screens and slots, [25](25-open-addons.md#implementation-notes-prs-58-59-2026-10-11)):
  `[any-thread][no-block]` — each call copies a registry table as JSON or posts a request to an
  add-on's worker and returns its generation; changes arrive through `mv_slots_set_listener` as
  change bits. `mv_slots_event` refuses a value the screen's tree could not produce
  (`MV_ERR_INVALID_ARG`). `mv_slots_thumbnail_path` is `[worker-thread]`. Mac: `mv_chrome_slots_*`.
- **`mv_script_*`** ([25 §9](25-open-addons.md#9-code-how-a-strangers-add-on-runs)): `reload` /
  `restart` are `[worker-thread]`; the rest `[any-thread][no-block]` so the key handler can post a
  command from the UI thread. Listener codes: 1 notice waiting, 2 an add-on started or stopped.
  `MV_SCRIPT_STORE` / `MV_SCRIPT_SPIN` are verify rigs, inert unless set. The Mac host drives
  `src/addon/script_host.h` directly.
- **Phone access** is an add-on since 0.43 ([plan](../plans/phone-access-addon.md)): `mv_phone_*` and
  `MV_PHONE_SECONDARY` are gone (0.41 had them); its card is the `phone-access` add-on's settings
  screen through `mv_slots_*`. Host table `catalogue_publish` / `_cancel` / `_json` (after
  `thumbnail_make`) reach Local search's catalogue, `MV_ERR_NOT_FOUND` without the pack; the optional
  export `mv_addon_removed` runs on every Remove but a reinstall (`mv_addon_remove` keep_data 2).
  Contribution API 15: a `secure` text field, never a value in the tree.
- **`mv_remote_*`** (0.44, [plan](../plans/desktop-client.md), #686): another computer's folders,
  a host or a Synology paired from its code, as a pinned remote folder; read-only until the server
  grants "change". Listings and reads run on workers; the viewer opens a remote file as a list item.
- **`mv_host_tray_start` / `_stop`**: the server's menu while `driftwire host` runs (the base app's
  shell keep), started by the first window's process; `_stop` at quit never waits on a `host` verb.

## Chrome bridges (outside `mediaviewer.h`)

The metadata pane's record never crosses `mediaviewer.h`: each host hands its chrome text tables
(`meta/tables.h`, written by `meta::editable_properties_table` on both) plus POD facts (position,
altitude, "made with", flag bits) so the map and AI badge read numbers the core decided rather than
parsing rows. Windows: `chrome_meta_args` (80 bytes, `src/shell/chrome_host.h`), matched by managed
`ChromeMetaArgs`; the attach probe refuses a size mismatch (`-11`). Mac: `mv_chrome_meta_facts`
(`struct_size`-versioned). The AI-generated flag is positive evidence only; clear means "not
stated" ([06](06-metadata.md#made-with-and-ai-provenance-290)). The AI badge's text comes from
`shell/ai_badge.h`, pushed only on change (Windows optional entry `ShowAiBadge`; an older chrome
has no badge).

Not built: per-object image handles (`mv_image_t`); a direct input callback (`mv_set_callback`).
