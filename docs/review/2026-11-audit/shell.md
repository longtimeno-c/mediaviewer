# Audit: native hosts (src/shell), ABI (src/abi) — read-only

Scope actually read: `command_table.cpp`, `command_coverage.inc/.h`, `commands.h` (ids 147-240),
`main.cpp` (startup 14580-14833, open/drop paths 1270-1340, 1563-1630, 8319-8330, 9320-9350,
11400-11415, 13985-14000), `main_mac.mm` (185-215, 6550-6590, 9790-9830, 10720-10935,
15270-15300), `settings_store.cpp` (all of 296-526), `telemetry.cpp` (all), `update_guard.cpp`
(193-225), `crash_reporter_mac.mm` (225-260), `single_instance_win.cpp` (grep). Open issues
checked (`gh issue list`); already-filed items are cited and not re-reported.

Not covered for lack of time: file_jobs / file_transfer internals, photos_backup*, shellext,
catalogue, per-signature C# P/Invoke type checks (only name-level ABI check done).

---

## A) Command inventory

Source of truth: `src/shell/command_coverage.inc` (228 rows, one per `command_id` except `count`;
`static_assert` in `command_coverage.h:41` keeps it complete) and bindings in
`src/shell/command_table.cpp:37-370`. Both hosts dispatch by area from the same .inc
(`main_mac.mm:9807`, Mac Settings/`?` filter via `MvCommandSupported` `main_mac.mm:196`).
Ctrl = Cmd on the Mac.

### Bound commands handled by both hosts (abridged by area)

| Area | Commands (key) |
|---|---|
| Walk | prev (←, A, Backspace), next (→, D, Space on still), first (Home), last (End), skip_back/forward (PgUp/PgDn), folder_up (Ctrl+↑), folder_prev/next (Ctrl+←/→), folder_back/forward (Ctrl+[ / ]) |
| View | fit 0, one_to_one 1, zoom_200 2, zoom_400 3, fill 4, reset_view Ctrl+0, zoom_in +/=, zoom_out -, sticky_zoom S, clipping C, overlay F3, info_overlay O, af_points Shift+O, zoom_af_point Shift+Z, loupe Z (hold), hold_previous \ (hold), pan_* arrows/Shift+arrows, loupe_nudge_* (loupe arrows), fullscreen F/F11, toggle_filmstrip T, toggle_gallery G, gallery_* (W/S/↑/↓/Enter/+/-), compare_two_up \|, next/prev_page Ctrl+PgDn/PgUp, toggle_page_sidebar Shift+T, eyedropper Shift+I, reset_stats R |
| Video | play_pause Space, J/K/L, frame_back/forward , ., rate_down/up Shift+Q/E, skim Q/E, mute Shift+M, trim_mode Ctrl+T, trim_in/out [ ], trim_clear, trim_preview P, trim_keyframe Enter, trim_reencode Shift+Enter, keyframe_prev/next Ctrl+←/→, clip_tools Ctrl+S, clip_split Ctrl+B, trim_remove_middle Ctrl+X, search_next/prev_match N / Shift+N |
| Marks / files | toggle_mark Ins/Shift+Space, mark_all Ctrl+A, unmark_all Ctrl+D, copy_to F7 / Shift+F7, move_to F8 / Shift+F8, delete Del, reveal Ctrl+E, copy_clipboard Ctrl+C, copy_path Ctrl+Shift+C, copy_flattened Ctrl+Alt+C, share Ctrl+Shift+S, print Ctrl+P, rename F2, new_folder Ctrl+Shift+N, paste_files Ctrl+V |
| Cull | set_rating_0-5 (Numpad0-5, Ctrl+Shift+0-5), label_red..blue (Numpad6-9, Ctrl+Shift+6-9), clear_label U, reject X, edit_comment Ctrl+I, metadata_pane I |
| Edit | rotate [ ], flip H/V, crop_mode Shift+C, crop_* (arrows, Shift+arrows, , . , A, X, Enter), export_image Ctrl+S, undo_edit Ctrl+Z, reset_edits Ctrl+R, adjust_pane Shift+A, edit_workspace Enter, show_original Y (hold), tool_* ([ ] Ctrl+Z Ctrl+Shift+Z Bksp Enter Space B) |
| Slideshow | slideshow_start F5, pause Space, faster/slower +/-, blackout ., shuffle R |
| Session | open Ctrl+O, open_folder Ctrl+Shift+O, close_window Ctrl+W, new_window Ctrl+N, always_on_top Ctrl+Shift+A, help ?, folder_tree Ctrl+Shift+E, typeahead /, open_settings Ctrl+,, open_import Ctrl+Shift+I, import_now Ctrl+Shift+F7, jobs_pane Ctrl+J, search_open Ctrl+F, search_similar Ctrl+Shift+F, search_this_person Ctrl+Shift+P, back Esc, game_toggle_3d 3 (runner) |

### Gaps (confirmed by reading)

1. **`play_motion` (`;`, Live Photo motion) is Windows-only.** Bound at
   `command_table.cpp:162` for Browse/Video; `command_coverage.inc:91` marks the Mac
   `excluded` ("the Mac dispatcher does not run it"); Windows runs `start_motion`
   (`main.cpp:9689`, `11406`). `main_mac.mm` has no motion/Live Photo playback at all
   (grep for `motion_pair|start_motion|live photo` finds nothing). Odd gap given Live Photos
   are an Apple format. Not filed.
2. **`go_to` (Ctrl+G, jump to item N) is Windows-only.** Bound `kViewing`; coverage row
   `command_coverage.inc:81` "the Mac has no go-to command"; no `go_to` in `main_mac.mm`.
   ⌘G on the Mac therefore does nothing (the dispatcher returns NO). Note #593 proposes Ctrl+G
   for "go to a typed time" on a clip; whichever lands should be designed for both hosts.
3. **`open_raw` / `open_jpeg` Windows-only and unbound** (`command_table.cpp:166-167`,
   `command_coverage.inc:92-93`). With the palette dropped (`commands.h:224`) these are
   reachable on Windows only via a user key remap, on the Mac not at all.
4. **No default key on either host:** `kind_filter_next`, `pane_wider`, `pane_narrower`,
   `auto_enhance`, `tool_red_eye`, `tool_extend`, `tool_faces` (`command_table.cpp:300,
   343-348, 367-369`). On the Mac they are menu items (`main_mac.mm:8078-8084`), so reachable
   via the menu bar from the keyboard; on Windows only through the island buttons or a user
   remap. Against the CLAUDE.md "every command reachable from the keyboard" standard on
   Windows. `addon_cmd_0..7` are unbound by design (add-on remaps).
5. Island-argument / slider commands (`zoom_preset`, `select_item`, `gallery_activate`,
   `close_gallery`, `crop_aspect_set`, `crop_straighten_set`, `kind_filter_set`, 27
   `adjust_*` sliders) are Windows-dispatch only, Mac does the same through `mv_chrome_*`
   calls — documented in the .inc, not a functional gap.
6. `palette` (76) is a dead id kept for wire stability (`commands.h:224, 481-485`) — fine.

No command is missing from coverage (the static_assert enforces it).

---

## B) Issues by severity

### Medium

**B1. Dropped / forwarded paths are probed on the UI thread** — confirmed by reading.
`resolve_paths` (`main.cpp:1282-1294`) calls `GetFileAttributesW` once per path, synchronously.
Callers: `WM_DROPFILES` handler (`main.cpp:13985-13997`, up to 256 paths) and the
second-instance forward handler (`main.cpp:1620-1630`). A path on an offline SMB share /
sleeping NAS / unmounted OneDrive placeholder blocks the window for the SMB timeout per path.
Same class as #576 (recent folder / OneDrive probe), but a different entry point — extend #576
rather than file separately.

**B2. Launch probes each requested path twice before the window works** — confirmed.
`open_paths_early` (`main.cpp:1303-1305`, called at `14823`) and `open_paths`
(`main.cpp:1330-1332`, called at `14833`) both run `resolve_paths`, and the comment at 1281 says
`open_path` stats again. On a local disk this is microseconds; on a network path given on the
command line it is 2-3 serial round-trips on the UI thread at startup. Fix: pass the
`open_request` from the early call through to `open_paths`.

**B3. A hung first instance makes every later launch hang ~10 s with no window** — suspected
(read the timeout, not the full flow). `forward_to_running_instance` waits
`WaitNamedPipeW` up to `kClientWaitMs = 10000` (`single_instance_win.cpp:18, 116-123`) before
the second process has any UI (`main.cpp:14646-14652`). If the owner is stuck (e.g. blocked on
B1), double-clicking a file appears to do nothing for 10 s, then the second process runs alone.
A shorter connect timeout plus a fallback window would bound it.

**B4. Policy contradiction: in-place writes to originals** — confirmed by reading, may be
intentional. CLAUDE.md hard rule: "Never modify a user's original file". But
`startRotationWrite` (`main_mac.mm:10887-10905`) rewrites the JPEG via `run_rotation_write`, as
documented in `docs/design/07-photo-editing.md:325-328` ("rewrites the file losslessly in place"),
and metadata writes edit plain JPEGs in place (`docs/design/06-metadata.md:358`;
`main_mac.mm:15287-15295` refreshes the stat after the write). The writes are atomic and
verified, but the CLAUDE.md rule and the design docs disagree; one of them should change so
agents stop "fixing" either side. Owner call.

### Low

**B5. Telemetry consent toggle does file I/O on the UI thread** — confirmed.
`set_enabled` (`telemetry.cpp`, called from the chrome handler at `main.cpp:8327`) calls
`spool_path()` → `update::locate_install()` (`update_guard.cpp:193-207`: `file_exists`,
`read_small_file(sq.version)`) and `DeleteFileW` synchronously. Local and tiny, but it is a rule 1
violation and `locate_install` is already cached in `g_install` (`main.cpp:747, 14597`).

**B6. `telemetry::record` has no callers** — confirmed (grep across `src/`). The spool format,
vocabulary and consent machinery exist but no event is ever recorded, so the consent UI promises
nothing. Either wire events or say so in docs/design/13; not a leak risk.

**B7. Settings write failure is marked settled** — confirmed, by design. `persist_pending`
(`settings_store.cpp:422-446`) sets `settled_seq_` even when the write fails; recovery relies on
the next change or `flush(1000)` at exit (`main.cpp:14948`). A failure followed by a crash loses
the change silently (only a log line). Acceptable; noting because #575 work will touch this.

Checked and clean: crash annotations on the Mac are scrubbed before they are set
(`crash_reporter_mac.mm:237-249`); telemetry rejects any tag with `/ \ : . @ %` etc.
(`telemetry.cpp` `looks_like_user_data`); uploads are off with no endpoint
(`crash_reporter_mac.mm:201`). Mac copy collision checks and compare-source probing run off the
main queue (`main_mac.mm:6550-6575`, `10733-10737`). `settings_store` write is atomic
(tmp + `FlushFileBuffers` + `MoveFileEx WRITE_THROUGH`, `settings_store.cpp:256-287`).

ABI: name-level check found every C# `DllImport` entry point declared in
`src/abi/include/mediaviewer/*.h`; Swift's unmatched `mv_*` names are struct types, not
functions. Per-argument type matching was not done.

Already filed, not repeated: #575 (two windows overwrite settings.ini), #576 (recent folder /
OneDrive UI-thread probe), #574 (prefetch on wrap/shuffle), #356 (key layout size limit),
#151 (untested ABI exports).

---

## C) Improvements

1. **Startup (Windows)**: remove the duplicate `resolve_paths` (B2). The pre-window path is
   otherwise lean: chrome preload is already threaded (`main.cpp:14671`), settings are read once.
   `run_start_guard`/`locate_install` (`main.cpp:14597`) do two file reads before the window;
   fine on local disk.
2. **Use `g_install` in `telemetry::spool_path`** instead of re-probing (B5).
3. **Shared core candidates**: the post-write "re-stat and patch the listing entry's
   size/mtime, bump thumb generation" block exists twice on the Mac alone
   (`main_mac.mm:10923-10930` and `15291-15299`) and presumably once per write path on Windows;
   one core helper (`refresh_entry_stamp(items, path)`) would serve both hosts.
4. **Parity work worth an issue**: Live Photo motion on the Mac (A1) and go-to on the Mac (A2).
5. **Keyboard reachability on Windows** (A4): give the seven `key::none` commands default
   bindings or a keyboard-reachable menu.
