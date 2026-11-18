# 25 — Open add-ons: anyone can make one, and what one can change

Open add-ons: `.mvaddon` packages from any publisher, plus the contribution points both hosts
read from one table. Built: PR 55 (packages, themes), 56 (settings pages, keymaps, theme shape,
first-party manifests), 57 (Lua scripts), 58–59 (screens, slots, providers; first-party native
add-ons). Not built: PR 60 (`files.write`, exports, `metadata.write`). The author's guide is
[docs/ADDONS.md](../ADDONS.md).

## 1. What this is

- **Open add-on:** one `.mvaddon` file, signed by whoever made it, installed from a file or a
  link. MediaViewer's own add-ons (release key, Team ID; [18](18-import.md)) keep their channel and
  loader; neither kind loads through the other's path.
- **Contribution model:** named points an add-on fills — themes, settings pages, commands and
  keys, chrome slots, search providers, screens — resolved once into tables both hosts read.
- **An add-on carries its own code and data** in its package, installed and removed together;
  nothing ships dormant inside the app. Places ([27](27-places.md)) is the first: `src/geo` and its
  data ship in the Places package.

## 2. Rules, and how an open add-on holds them

| Rule | How it holds |
|---|---|
| 1 — nothing blocking on UI / render | Contributions are resolved at load into plain tables; nothing is looked up per frame, tile or key. Add-on code runs on its own worker |
| 2 — the canvas is C++'s | A theme colours the chrome only, never pixels, swapchain or colour pipeline |
| The view comes first | No add-on is read before first pixel; the chrome paints with cached theme tokens and verifies afterwards |
| 5 — never modify an original | No point writes a user's file; PR 60 writes new files only, metadata via PR 12's writer |
| 6 — nothing leaves | Data add-ons cannot read files or connect. Scripts have no network at all and file access only by named permission. The app contacts a third party only on a click (install from link, check for update), plain GET |
| D1 / D9 | Every point exists on both hosts; each renders natively. No web view |
| **Absent means absent** | No open add-on installed: nothing listed, nothing read at start, no folder created |
| Signed, verified, then loaded | Publisher's key: signature over the manifest, SHA-256 per file, nothing extra; checked at install and every load |
| Host API versioning | Outside the declared API range: "needs a newer MediaViewer", never a half-load |

## 3. The package

`name.mvaddon` is the narrowest ZIP the reader accepts (`src/addon/package.*`):

- every entry **stored** (no compression, no inflate code);
- no ZIP64, encryption, data descriptors, extra fields, comments or folder entries;
- entries end to end from byte 0 to the central directory, which ends at the 22-byte end record;
  no gaps or trailing bytes;
- every name a safe relative path: `/`-separated, no `..`, drive, control char, `<>"|?*`, part
  ending in dot/space, or Windows device name (`con`, `nul.json`, `com1`); no case-folded duplicates;
- **64 MB and 2,048 entries at most.**

Contents: `manifest.json`, `manifest.json.sig` (64-byte raw Ed25519, detached, over the exact
manifest bytes) and the listed files. **Anything else refuses the package.** The file is read once
into memory; what was inspected is what is installed. `tools/addon-sdk/mvaddon.py` writes it.

## 4. The manifest (schema 2)

```json
{
  "schema": 2,
  "id": "acme.film-tones",
  "name": "Film Tones",
  "version": "1.2.0",
  "description": "Warm and cool chrome themes for grading in a dim room.",
  "licence": "MIT",
  "publisher": { "name": "Acme Pictures", "url": "https://acme.example", "key": "<64 hex>" },
  "update_url": "https://acme.example/film-tones.mvaddon",
  "api": { "min": 1, "max": 1 },
  "installed_size": 1830,
  "files": [ { "path": "themes/dusk.json", "sha256": "<64 hex>", "size": 915, "licence": "MIT" } ],
  "contributes": { "themes": [ { "id": "dusk", "name": "Dusk", "path": "themes/dusk.json" } ] }
}
```

- **`id`**: two or more dotted parts of `[a-z0-9-]`; first-party ids have no dot, so they cannot
  collide. `mediaviewer.` reserved; no part a Windows device name (the id is a folder).
- **`publisher.key`**: Ed25519 public key for the `.sig`. MediaViewer's release key is refused.
- **`api`**: contribution API range ([§7](#7-the-contribution-model)); `min` above the host's →
  "needs a newer MediaViewer".
- **Display text** (`name`, `publisher.name`, `description`, licences): bounded; no control,
  bidi-override or zero-width characters.
- **Links** (`publisher.url`, `update_url`): `https://` only, printable ASCII, no credentials.
- **Strictness:** a manifest whose API is within the host's may hold no unknown key; one reaching
  past it may (one package serves old and new hosts).
- **Native code refused by name:** `native`, `chrome`, `main` at top level or under `contributes`
  → `code_not_allowed`, whatever the range.
- **Scripts (API 3):** `scripts`, `permissions`, `contributes.commands`; see
  [PR 57 notes](#implementation-notes-pr-57-2026-10-11). A range ending before 3 cannot carry them
  (malformed); an older host refuses them as `code_not_allowed`.

## 5. Identity and trust

The signature proves the files are what the publisher packed and that updates come from the same
key. It does **not** prove who the publisher is; there is no registry, review or store.

The sheet: **Install “Name” version?**, then From (publisher, link host), Key (fingerprint),
Adds, Can, Cannot, Size · licence, and "MediaViewer has not checked this add-on or who made it."
Under data-only APIs Cannot reads "run code, read your files, or use the network".

- **Where:** a panel at the foot of Settings → Add-ons, in place of the list. A package handed to
  the app opens Settings on General and scrolls to it.
- **Cancel is the default**; `Enter` does not install.
- **Fingerprint:** first 8 bytes of SHA-256(key), four groups of four hex digits.
- **Can / Cannot** lines are computed from the manifest; each script permission is a "Can" line.
- **Key pinned per add-on** at first install (`publisher.json`). Same id, other key →
  `other_publisher` ("remove the installed one first").
- **Consent is bound to bytes:** install takes the SHA-256 the sheet was shown for; a different
  file → `changed`.
- **No downgrades;** the same version again is a repair.
- **A folder dropped into place** without the app's `publisher.json` is listed "not approved" and
  not loaded.

## 6. Install, update, remove

| | Behaviour |
|---|---|
| **From a file** | Settings → Add-ons → **Install from file…**, a drop on the window, or Open With; then the sheet |
| **From a link** | **Install from link…**, an `https://` link. Plain GET: no cookies, `User-Agent: MediaViewer`, redirects to `https` only, stopped at 64 MB. The field notes the server sees this computer's address |
| **Update** | **Check for update** (with `update_url`): the link flow, newer version from the pinned key only; a link serving another add-on is refused. Never automatic |
| **Remove** | One click, no restart for data add-ons; a theme in use falls back to Default. A replaced version is deleted at once |
| **Where** | `%LocalAppData%\MediaViewer\open-addons\<id>\<version>` · `~/Library/Application Support/MediaViewer/Open Add-ons/<id>/<version>` (`io::open_addons_dir()`, never created by a read); beside the first-party `addons` folder |
| **Not offered** | `mediaviewer://` install links, silent install, all-users install |

`.mvaddon` is registered with both OSes (Windows ProgId `MediaViewer.Addon`; Mac document type
and exported UTI).

## 7. The contribution model

Every contribution is resolved **once, at load, off the UI thread**, into tables the chrome
reads. Each point is versioned by the API that introduced it, exists on both hosts, and has a base
default. **Contribution API:** `kContributionApi` / `MV_ADDON_CONTRIBUTION_API`; every point and
its API is one table, `kContributionPoints` (`src/addon/open_manifest.h`).

| Point | API | PR | Supplies | Base default | Performance rule |
|---|---|---|---|---|---|
| **Built-in add-ons by manifest** | — | 56 | A first-party (schema 1) manifest's description, commands with default keys and modes, card hint | Import's built-in rows and text | Rows go into eight slots `addon_cmd_0..7`; dispatch stays an array index |
| **Theme** | 1 | 55 | Colour tokens (dark and/or light), a font family | System palette, CozetteVector | Tokens cached by the chrome, verified after |
| **Theme, shape** | 2 | 56 | Corner radius scale, density, type scale (theme schema 2) | Built-in literals | Resolved once when painted |
| **Settings page** | 2 | 56 | Sections, toggles, choices, numbers, text, each with key, default, help; `enabled_by` greys an item while a toggle is off | — | Rendered when Settings opens; values in one file per add-on id the core keeps, kept on remove unless asked |
| **Keymap** | 2 | 56 | Bindings for existing commands (named layouts) | `default_bindings()` | Applied to the live table once ([16](16-commands.md#one-table-editable)) |
| **Command** | 3 | 57 | Name, default key, script function | — | Rows appended at load; a clash with the base app drops the add-on's key, Settings says so |
| **Screen** | 4 | 58 | A view tree ([§10](#10-screens)) in a window, pane, sheet, Settings page or Edit | — | Built from data; diffs posted from the add-on's worker |
| **Chrome slot** | 5 | 59 | Command-bar, path-bar, context-menu, info-pane, status, tile-badge, Video Editor items | — | Badges from a table filled off-thread; **no add-on call per tile** |
| **Search provider** | 5 | 59 | Answers a query with files | File-name search | Add-on's worker, query generation; stale answers dropped |
| **Export / files** | 6 | 60 (not built) | New files in a user-chosen folder | — | Generation-tagged jobs |

Later APIs (6–13) extend the screen vocabulary and places; each addition is listed with its API in
the [PRs 58–59 notes](#implementation-notes-prs-58-59-2026-10-11).

**Local search is a provider.** While the AI pack is attached it is the local-scope provider; a
Local search query also goes to every other local-scope provider, whose rows the result list's
path row offers ("+ 3 in Portugal"). A query with place terms goes to no other provider: the pack
asks the `place:` provider through `place_matches` and intersects
([17](17-local-ai-search.md) "Places over the index").

## 8. Themes (API 1)

**Themes.** A theme file per listed theme:

```json
{ "schema": 1,
  "dark":  { "canvas": "#1c1b1a", "surface": "#262523", "title": "#f2efe9", "body": "#b9b4aa",
             "disabled": "#6f6b64", "hairline": "#3a3835", "accent": "#e0793a" },
  "light": { "canvas": "#f4f1ea", "surface": "#ffffff", "title": "#1c1b1a", "body": "#57534c",
             "disabled": "#9a958c", "hairline": "#d6d1c6", "accent": "#b5542d" },
  "font": "Avenir Next" }
```

| Token | Means | WinUI (`IslandHost.Theme.cs`) | SwiftUI (`MVTheme`) |
|---|---|---|---|
| `canvas` | Chrome background, and the viewer's everywhere (empty window, around a photo/video) | `Canvas`, `PanelBg`, native `home.canvas` | `canvas`, host `home.canvas` |
| `surface` | Rows, fields, cards | `Surface` | `surface` |
| `title` | Primary text | `Title` | `title` |
| `body` | Secondary text | `Body` | `body` |
| `disabled` | Disabled text | parsed, not read (opacity dims) | `disabled` |
| `hairline` | Separators, outlines | `Hairline` | `hairline` |
| `accent` | Selection, progress, focus, marks | `TrimAccent`, `StarOn`; `Selection`, `TrimKeep`, `TextSelection` derive from it | `accent` (tints system controls) |

- **All seven required** per palette; `#rrggbb` or `#rrggbbaa`; `canvas`, `surface` opaque.
- **One palette or two.** Two follow the system appearance; one fixes it while on, and system
  controls are told to match (`NSApp.appearance`; `RequestedTheme` on every island root).
- **Contrast floor** (WCAG, against `canvas` and `surface`): `title` 4.5:1, `body` and `accent`
  3:1. Below it the palette is refused; the SDK reports ratios.
- **High contrast wins:** the system's colours, theme waits.
- **`font`**: an installed family, else the chrome's face. No font files. Mac applies at once;
  Windows at next start (Settings says so).
- **Empty window:** ImGui draws it from the resolved tokens in the input snapshot (`input_state.h`
  `home`); Default and high contrast send the system's colours, so native never derives one.
- **Not themed:** photo/video pixels, the colour pipeline, the F3 overlay, histogram channels,
  hover washes.
- **Shape (schema 2)** reaches every padding, spacing and corner radius via `MVTheme.space` /
  `MVTheme.radius` (Mac) and `Space` / `Pad` / `Corner` (Windows). Layout geometry (grid,
  filmstrip, transport `kTransportPadDip`, timeline, hairlines) stays literal; Import, Duplicates
  and Local search windows scale radii only.
- **Choosing:** Settings → Appearance → **Theme** opens a sheet of cards drawn in their own tokens:
  Default, the built-ins compiled into both chromes (one palette, no `font`), then installed
  add-ons' themes. Installing does not switch. Default clears `mv.theme.*` (Mac) / `theme.json`
  beside `settings.ini` (Windows).
- **Ground effect:** a built-in may draw one animated effect behind the chrome and the empty
  window (`shell/home_ground.h`); not a token, not settable by add-ons, never over media. It runs
  only while visible and recently used, and stops under Reduce Motion / animations off and high
  contrast.
- **Start-up:** the chrome caches the theme in use and its resolved tokens in its settings and
  paints with them at once; the add-on is verified on a worker after. Gone, changed or refused →
  Default, and Settings says why.
- **Theme change:** Mac rebuilds the SwiftUI roots (`ThemedRoot`); Windows recolours brushes in
  place.

## 9. Code: how a stranger's add-on runs

Other makers' code runs only as a **sandboxed script (A): Lua 5.4**, on one worker per add-on,
never on UI, render or decode threads. **B** (native, in process) is MediaViewer's own add-ons,
under its key and Team ID. **C** (a helper process) is not built. Why A: one package for both
OSes, rule 1 and rule 6 hold by construction, a crash stops the add-on not the viewer, and the Mac
hardened runtime needs no library-validation exception.

- **Limits:** allocator memory ceiling, instruction budget and watchdog, no `io` / `os` /
  `package` / `debug`. Over a limit the add-on stops and Settings says which and why.
- **Permissions** (manifest entries, each a sheet line, none by default): `folder.read`,
  `thumbnails`, `selection`; `files.write`, `metadata.write` (PR 60, not built). **There is no
  `network` permission.**
- **Host API:** the native function table's read side, re-exposed per permission, plus the
  contribution points. Names, limits and values: [PR 57 notes](#implementation-notes-pr-57-2026-10-11).

## 10. Screens

A screen is a **view tree**, a vocabulary both hosts render with their own controls.

- **Screens** open in a window, a docked pane, a sheet, `settings` (API 8) or `edit` (API 12).
- **Vocabulary:** stack, grid, scroll, split, text, image, thumbnail, thumbnail grid (virtualised),
  list, table, button, toggle, choice, slider, number, text field, progress, separator, `surface`
  (pixels the add-on drew), and later `tiles`, `link` and looks; full list with APIs in the
  [PRs 58–59 notes](#implementation-notes-prs-58-59-2026-10-11).
- **Secure text fields** (contribution API 15, the Phone access add-on): `"secure": true` draws
  dots; the core never keeps the value in the tree (a posted tree may only give `""`; an event's
  value goes to the add-on alone). The add-on renames the field once it has used or dropped what
  was typed, so the host draws a new, empty one.
- **Glyphs on text** (contribution API 16, Phone access's section headings): a `text` may carry a
  `symbol` from `kSymbols`, drawn before it in its size and colour. API 16 also adds the glyphs
  phone, drive, lock, computer and sync (SF Symbols / Segoe Fluent Icons, mapped in each host).
- **State** lives in the add-on: it posts a tree, then diffs; the host applies them on the UI
  thread in one pass. Events post back to the add-on's worker.
- **Keyboard-complete:** every control focusable and named; commands are table rows
  ([16](16-commands.md)). Theme, high contrast, text size, VoiceOver and Narrator come from the
  host's controls.
- **Windows controls** are only those `tools/check-winui-controls.ps1` allows.
- **Look** (`IslandHost.ScreenLook.cs`, `ScreenView.swift`): the system face (or the theme's
  `font`), ramp 17 semibold / 14 / 13 / 12; toggles right of their label; filled buttons (accent
  `primary`, red `destructive`); a window opens no taller than its content. A new revision keeps
  scroll offsets and focus by node id.

## 11. Performance

- **Launch:** zero add-on work before first pixel; nothing listed, verified or parsed until the
  chrome is up, then on a worker. Scripts start after the chrome (Mac utility QoS; Windows a
  background job).
- **Steady state:** contributions are tables; nothing resolved by name per frame, tile or key.
- **Idle:** no timer, no poll; 0 presents and ~0 % CPU on a still with add-ons installed.
- **Verification:** SHA-256 of a package's files once per process, on a worker.

## 12. Licensing

- The manifest names an SPDX licence for the add-on and each file; shown, not policed.
- The SDK (`tools/addon-sdk`) and examples are **MIT**.
- Add-ons may use any licence: the app's GPL-3.0-or-later carries a section 7 additional
  permission ([LICENSE-ADDONS.md](../LICENSE-ADDONS.md)) for works that reach MediaViewer only
  through the documented add-on interfaces (package, manifest, contribution points, script host
  API). Code linking the core directly stays GPL.

## 13. For people who make add-ons

- **`tools/addon-sdk/mvaddon.py`**: `keygen`, `init` (`--script`), `pack`, `check` (including
  contrast ratios).
- **`docs/ADDONS.md`**: the author's guide. **`examples/addons/`**: `film-tones`, `settings-demo`,
  `vim-keys`, `folder-count`.
- **Cross-check:** CI verifies SDK output with the C++ reader (`tools/addon-verify --open` /
  `mv_addon_verify --open`). The SDK serves the host's `kContributionApi` and point table, and a
  test reads both from `src/addon/open_manifest.h`, so a host that gains an API or point fails the
  SDK suite until the SDK knows it.

## 14. Roadmap

55–59 are built (58–59 for first-party native add-ons; scripts reach them via `slot_client`).
PR 60 (`files.write`, exports, `metadata.write`) is not built.

### PR 56, first half: Import described by its manifest (2026-10-04)

- **Schema 1 manifests** may carry `description`, `contributes.commands`, `contributes.hint`
  (`src/addon/manifest.*`; a mistyped key is malformed). `tools/package/addon-pack.py` writes
  Import's: "Import…" on `Ctrl+Shift+I` / `Cmd+Shift+I`, "Import marked now" on `Ctrl+Shift+F7` /
  `Cmd+Shift+F7`, and the card hint.
- **Eight slots** `addon_cmd_0..7` (`shell/commands.h`) take loaded add-ons' rows via
  `set_addon_commands`: name, key (`parse_key_label`, inverse of `key_label`), modes, payload.
  Appended after built-ins, so built-in remaps keep indices; a remap of a contributed row is lost
  when the add-on set changes. Contributed rows **supersede the built-in rows for that add-on**
  until they go.
- **Run path.** Windows: `chrome_host::run_addon_command` → `IslandHost.RunAddonCommand` →
  `IAddonCommands.RunCommand(id, json)`; rows from `mv_addon_commands_json`. Mac:
  `MvAddonsRunContributedCommand` → the bundle's optional `runCommand:payload:`. An Import without
  `contributes` keeps the built-in rows and its two frozen calls.
- Settings' Import line and the card hint come from the manifest, else built-in words.
- Not moved: the AI pack's four commands; the list of first-party channels.

### PR 56, second half: declarative contributions (2026-10-11)

Contribution API 2: `contributes.settings` (one page), `contributes.keymaps` (up to eight
layouts), and `shape` in a theme file with `"schema": 2`. A manifest whose `api.max` is 1 may not
name them (malformed). A schema 1 manifest may carry `contributes.settings` (Places).

| Piece | Code |
|---|---|
| **Settings page** schema and rules (sections, toggle / choice / number / text, `enabled_by`, bounds) | `src/addon/settings_schema.*` |
| Values: one JSON per add-on id, cached, keep or forget; each names the publisher key it was written for and is never read for another (another publisher's install deletes them) | `src/addon/settings_values.*`; folder `io::addon_settings_dir()`, never created by a read |
| **Keymap** pack shape | `src/addon/keymap.*` |
| Applying a pack; stable command names | `src/shell/keymap.*` (`apply_keymap`, `command_slug`); `set_live_key`, Reset in `src/shell/command_table.cpp` |
| Theme shape | `src/addon/theme.*` (`theme_shape`) |
| One JSON for both hosts | `src/addon/open_json.*` (`addon_settings_page_json`, `addon_setting_set_json`, `addon_settings_forget`, `open_keymap_json`) |
| Host table | `settings_get`, after `walk_files2` in `mv_host_api` (`host.cpp`) |
| ABI (Windows) / bridge (Mac) | `mv_addon_settings_json` / `_setting_set` / `_settings_forget`, `mv_open_addon_keymap_json`; `mv_addons_settings_*`, `mv_open_addons_keymap`, `mv_chrome_apply_keymap` |
| Mac chrome | `AddonSettingsPage.swift`, `OpenAddons.swift`, `AddonsView.swift`, `Theme.swift` (`MVTheme.radius` / `space` / type scale; `MVChromeTheme`), `HostTheme.swift`, `AITheme` |
| Windows chrome | `IslandHost.AddonSettings.cs`, `IslandHost.OpenAddons.cs`, `IslandHost.Settings.cs` (Key layout), `IslandHost.Theme.cs` / `ThemePack.cs`, `ImportLook.cs`, `IAddonThemeShape` (`MediaViewer.Interop/Addons.cs`); native `chrome_cmd_apply_keymap` (1050) in `main.cpp` |
| SDK, examples | `tools/addon-sdk/mvaddon.py`, `examples/addons/settings-demo`, `examples/addons/vim-keys` |
| Tests | `tests/test_addon_contributions.cpp` (`mv_import_tests`), `tests/test_keymap.cpp` (`mv_tests`), `tools/addon-sdk/test_mvaddon.py`, `tools/fuzz/fuzz_addon_contributions.cpp`, Mac rig `MV_ADDON_SELFTEST_PR56` |

**Same controls, same order:** toggle → switch (`ToggleSwitch`); choice → menu (`ComboBox`);
number → field with − / + and unit (Windows `FakeInput` + two buttons; Mac field + stepper);
text → field (`FakeInput` / `TextField`). Page title, then each section's title and help, then
items with help beneath, in manifest order.

Contributed commands are named `<add-on id>/<command id>` in keymap packs. Scripts read values with
`mv.settings.get` (`settings_values::value_json` with the script's id, publisher and page).

## 15. Not in this plan

A store, registry, reviews or hosted index; payments; automatic third-party updates;
`mediaviewer://` install links; third-party native code in process; add-ons that replace a
decoder, the canvas, the colour pipeline or the present path, or change an original; a web view.

## 17. Decisions (owner, 2026-10-03)

1. Stranger's code runs as the sandboxed script with screens as data ([§9](#9-code-how-a-strangers-add-on-runs) A).
2. No `network` permission.
3. `.mvaddon` is registered with the OS ([§6](#6-install-update-remove)).
4. Add-on licence: the GPL section 7 additional permission ([§12](#12-licensing)); SDK MIT.
5. The Settings wording is "From others".

## Implementation notes (PRs 58-59, 2026-10-11)

PR 58 (screens) and PR 59 (slots, search providers), one registry, for MediaViewer's own
native add-ons (§9 B). Scripts reach the
registry through `slot_client`, implemented on their own worker, calling the same `post_*`
functions the v3 rows wrap.

| Piece | Code |
|---|---|
| View-tree vocabulary, checks, diffs, events | `src/addon/screen.*` |
| What a manifest contributes; the symbol set | `src/addon/contrib.h`; schema 1 parse in `src/addon/manifest.cpp` |
| Registry: tables at attach, one worker per add-on, coalesced generation-tagged requests, trees, surfaces, rows, badges, status, notices | `src/addon/slots.*` |
| Host table v3 (`screen_post` … `locations_in`), add-on side `mv.slots.1` | `mediaviewer_addon.h`, thunks in `src/addon/host.cpp`; `loaded_addon::load` attaches, its destructor detaches first |
| A file's coordinates | `src/meta/location.*` |
| Windows ABI | `mv_slots_*` (`src/abi/slots_abi.cpp`), `mv_slots_thumbnail_path` (`abi/addon_abi.cpp`); `SlotNative` (`MediaViewer.Interop/Slots.cs`) |
| Mac bridge and chrome | `mv_chrome_slots_*` (`src/shell/slots_mac.mm`); `MvSlotAddonsStart` (`addons_mac.mm`); `SlotStore.swift`, `ScreenView.swift`, `SlotItems.swift`; hooks in `MetadataView/Store`, `FileSearch`, `CommandBarView`, `GalleryView`, `AddonsView` |
| Windows chrome | `IslandHost.Slots.cs`, `IslandHost.Screens.cs`; hooks in `IslandHost.Panels.cs`, `.FileSearch.cs`, `.Gallery.cs`, `.Addons.cs` |
| Tests | `tests/test_addon_screens.cpp`, `tests/test_addon_slots.cpp` (`mv_import_tests`) over `tests/addons/slots_test` (`mv_slots_test_addon`) via a signed store; `tests/test_meta_location.cpp` |

**Manifest (schema 1).** `contributes.api {min, max}`, then `screens`, `info_sections`,
`search_providers`, `bar_items`, `path_items`, `context_menu`, `status_items`, `tile_badges`,
`commands`, and from API 11 `editor_items` (`{id, label}`: a text button in the Video Editor's bar;
choosing it is `MV_SLOT_EVENT_COMMAND` with `{"path", "ranges_ns"}`, via `invoke_editor_item` /
`mv_slots_editor_item`). Each array at most 16. A point outside the range is malformed; a range
starting past the host is "needs a newer MediaViewer". Any of them needs `host_api.min` 3; a
slot-only add-on names no `chrome`.

**Contribution API by addition:**

| API | Adds |
|---|---|
| 4–5 | Screens; slots and providers |
| 6 | `link` node; `button` `"copy"` |
| 7 | `"opens"` on a `status_items` entry |
| 8 | Screen place `"settings"` (`MV_SLOT_PLACE_SETTINGS`) |
| 9 | `stack` `"reveal"` |
| 10 | `surface` `"glow"`; `progress` `"look": "glow"` |
| 11 | `editor_items` |
| 12 | Screen place `"edit"` (`MV_SLOT_PLACE_EDIT`); `surface` `"fill"` and `open_list`; a `choice` option's `"disabled"` |
| 13 | `progress` `detail` and `timed` |
| 15 | `text_field` `"secure": true`: dots on screen, its value never kept in the tree, a snapshot, a log or a crash report (the Phone access add-on, 2026-11-14) |

An add-on asks for the API of each addition it uses, so an older host says "needs a newer
MediaViewer" instead of refusing the tree or showing a blank window.

How it works:

- **One worker per add-on.** Every call in (`info_section`, `search`, `screen_open`, `badges`,
  `event`) runs there. A newer request of a kind replaces the queued one; events queue in order.
  Generations are global: a post for a stale one is `MV_ERR_CANCELLED`; `is_stale` lets a provider
  stop early. An idle worker sleeps on a condition variable.
- **Trees.** The core keeps each screen's tree, checks every tree and diff whole, and hands the
  chromes one canonical JSON. Every control, picture and surface needs a `label` (its
  VoiceOver/Narrator name). Chrome events are checked against the tree (exists, interactive,
  enabled, value kind and range); the tree keeps the value; the add-on answers with a diff. Text
  fields report on Return and focus loss, sliders on release: never per key. Windows rebuilds a
  screen window when its revision moves, but waits while a pointer is down.
- **Info sections** are asked for when the Metadata pane shows a file, once per file and width,
  never for a file without coordinates when `"when": "location"`. Drawn at the bottom of the `I`
  pane after built-in metadata, every tab, never collapsed. A `surface` is BGRA at pane width ×
  backing scale; the chrome draws the pin (accent, drop-in off under Reduce Motion) and the
  surface's `marks` (≤ 64 names, priority order, skipped on overlap) as native text.
- **A changed setting re-asks.** When a write changes an add-on's stored values
  (`mv_addon_setting_set`, `mv_addons_setting_set`, Remove without keep, another publisher's
  install), `settings_values` notifies listeners after the write, outside its lock.
  `slot_registry::settings_changed` re-asks that add-on's info sections for the file on screen
  under a new generation (what showed stays until replaced) and, with badges, its badge sets for
  the folder shown. Other add-ons, nothing on screen, or an unchanged value ask nobody. Add-ons read
  `settings_get` on every request and cache nothing across a change. Shown search results are not
  re-run. Only the window that wrote re-asks.
- **Interactive surfaces.** `"interactive": true` (or `{"min_scale", "max_scale"}`): wheel/pinch
  zooms, drag pans; the chrome transforms pixels during the gesture, then sends one event: `zoom`
  {scale, x, y}, `pan` {dx, dy}, `double` {x, y}, `tap` {x, y} (`MV_SLOT_EVENT_TAP`), coordinates
  0..1. An info section may show a same-folder file via `open_item` ([27](27-places.md#the-folder-map-601)).
- **Fill and open_list (API 12).** `"fill": true` on a `surface` takes its parent's room; the
  chrome reports the size (points, with backing scale) as `MV_SLOT_EVENT_RESIZE` at first layout
  and when a resize settles; only the newest is queued. A window, pane or sheet may call
  `open_list` (1–10,000 absolute paths or `photos:` keys, a title, the item to show); the registry
  keeps the last with a growing revision (`mv_slots_open_list_json`), each chrome opens it once as
  a result list. A window whose tree fills goes full screen ([27](27-places.md#the-world-map)).
- **Search providers.** File search posts the text on each debounced change; name hits show at
  once, provider rows join with their reason or "Still reading tags — N of M". A query starting with
  a provider's prefix (`place:`) goes to it alone, prefix removed, unless a digit follows
  (`in:2024`). In Local search, place terms go through `slot_registry::provider_paths`.
  `locations_in` walks the folder at background priority and caches coordinates by path + size +
  mtime; with no folder open, `locations_of_index` reads the AI pack's index.
- **Badges:** once per folder, posted as sets, inverted into a path → badges table.
- **Items.** A bar or path item with `opens` opens that screen with no add-on call; otherwise a
  `command` event with the paths its `target` names (marks, else current file; the folder).
  Context-menu entries show on gallery tiles, filmstrip tiles and the canvas, with the paths the
  gallery tile would give (Mac `SlotContextMenu`, `SlotCanvasMenu`; Windows
  `AttachSlotContextMenu`, `ShowCanvasMenu` from `WM_RBUTTONUP`). Bar items show name and icon.
- **Status items** are command-bar text. `status_json` gives one `"opens"` (the named screen, else
  the add-on's only window/pane/sheet); then both hosts draw it as a flat button whose click is
  `open_screen` (`OpenSlotScreen`, `SlotStore.openScreen`).
- **Commands** (`contributes.commands`) from a slot add-on are listed with other add-on rows;
  running one is a `command` event (`node_id` = command id, value = payload files) via
  `slot_registry::run_command` (`mv_slots_command`). Settings → Add-ons gives an add-on with a
  window screen an **Open …** button (`open_screen`, no file).
- **Screen places.** Window, docked pane or sheet, each with Done and Esc. The docked pane lies
  **over** the canvas's right side (no reflow); the Mac holds it to the viewer's edge, Windows
  places it on open only. `settings`: on the add-on's page in Settings → Add-ons, opened and closed
  with that page, root a stack, schema 1 only (`AddonSettingsScreen`,
  `IslandHost.AddonSettingsScreen.cs`). `edit`: in the Edit workspace's Enhance tab, opened only
  via `mv_slots_screen_command` ([29](29-enhance.md) "Docked in Edit"). `settings` and `edit`: at
  most one per add-on; `screen_show`, `"opens"` and status items never open them. Thumbnail grids
  are virtualised, each tile asking the JPEG-512 cache.
- **Notices and keep-alive** (first-party only): `notice_post` (title, body, one button back as
  `MV_SLOT_EVENT_NOTICE`; skipped while that screen is front; a click opens it),
  `notice_authorize`, and `keep_alive` (process stays with a menu-bar / notification-area icon after
  every window closes). Held in `notices_json` ([28](28-driftwire.md#in-the-background-452)).
- **Removal:** Settings → Add-ons lists every slot add-on with Remove (Places with Install when
  absent). Detach first (removes every item, waits for a call in progress), then shut down and
  unload, off the UI thread. No restart.
- **Launch:** slot add-ons load on the add-on queue / a worker after Import's and the AI pack's
  checks; nothing is asked until something is shown.
- **Every window.** Each window is its own process ([18](18-import.md) "One host process") and
  loads the slot add-ons after its chrome (Windows `mv_addon_load_guest`, no store sweep), found
  by signed manifests alone (`store::manifests`, `loads_for_slots`, `mv_addon_slot_addons_json`).
  Store writes stay with the first window.

**Node additions** (drawn in `ScreenView.swift` / `IslandHost.Screens.cs`, checked by `"[screens]"`):

- `text_field` `"pick": "folder"`: a **Choose…** button and the system folder dialog
  (`NSOpenPanel`, `FolderPicker`); the folder arrives as `change`.
- **`tiles`** (`label` required): `items` of `{id, text, detail?, initials?, progress?, mark?}`
  (≤ 256), `selection`, `size` (32–160 pt), `static`. Click/Space/Return → `activate`; `select`
  takes one id; `static` takes no focus or events.
- `stack` `"look": "card"` (rounded surface, hairline, padding); `"align": "center"` also centres
  text.
- `progress` `"look": "ring"` with `size` (16–256): fills, percentage inside; unknown = still third.
- `text_field` `"look": "code"` with `digits` (1–12): digits only, grouped, submits on the last
  digit (or Return), then clears.
- `text` `"style": "display"`: large face for a code read across a room.
- **`link`** (picture; `label` required): `via` (`bluetooth`, `wifi`, `internet`, `dht`, `relay`,
  `server`, `tunnel`) and `state` (`connecting`, `moving`, `paused`, `done`, `failed`) required;
  optional `direction`, `from`, `to`, `through` (≤ 64 chars), `caption`, `value` (−1–1). Animates
  only `moving`/`connecting`, ≤ 30 fps. Full width.
- `button` `"copy"`: the host copies the text on click, then reports the click; label reads
  "Copied" for 1.5 s.
- `stack` `"reveal"` (token ≤ 64 chars): on first show or change, scrolls the stack into view
  with a fading accent outline (`IslandHost.ScreenReveal.cs`, `RevealLook`).
- `surface` `"glow": "moving" | "still"`: a multicolour edge; Mac `GlowEdge`, Windows `WithGlow`;
  colours and timings in `GlowMath.cs`, checked against `ScreenView.swift`.
- `progress` (`"bar"` or `"glow"`, drawn the same): one line, fill never goes back, a sweep at
  −1, a caption `label` first ("Enhancing · 40 % · about 1 min left"). `detail` replaces the
  percentage; `timed: true` has the host estimate time left. `ProgressLine` on both hosts; words in
  `ProgressWords.swift` / `src.managed/Shared/ProgressWords.cs`.
- A `choice` option's `"disabled": true`: listed, never chosen; the core refuses a change to it.

## Implementation notes (PR 55, 2026-09-29)

| Piece | Code |
|---|---|
| **The package** reader | `src/addon/package.*` |
| **The manifest** (schema 2), ids, links, display text, fingerprint | `src/addon/open_manifest.*` |
| Themes: tokens, parsing, contrast floor, chrome JSON | `src/addon/theme.*` |
| Store: inspect, install, list, remove, publisher record | `src/addon/open_store.*`; `io::open_addons_dir()` (`src/io/paths*`) |
| One JSON and wording for both hosts | `src/addon/open_json.*` |
| Windows ABI / Mac bridge | `mv_open_addon_*` (`mediaviewer_addon.h`, `abi/addon_abi.cpp`); `mv_open_addons_*` (`mv_chrome_bridge.h`, `shell/addons_mac.mm`) |
| A package handed to the app | `shell/open_request.*` (`open_kind::addon_package`), `shell/main.cpp`, `chrome_host::offer_addon` → `IslandHost.OfferAddon`; `main_mac.mm` `-openEntryPath:` / `-openEntryPaths:failed:` → `MVChromeHost.offerAddonPackage` |
| Mac chrome | `Theme.swift` (`MVTheme`, `ThemeStore`, `ThemedRoot`), `OpenAddons.swift`, `SettingsView.swift`, `ThemePicker.swift` |
| Windows chrome | `IslandHost.ThemePack.cs`, `IslandHost.ThemePicker.cs`, `IslandHost.OpenAddons.cs`, `IslandHost.Theme.cs` `RefreshTheme`, `AddonNative.Open*` (`MediaViewer.Interop/Addons.cs`) |
| SDK, guide, example | `tools/addon-sdk/mvaddon.py`, `docs/ADDONS.md`, `examples/addons/film-tones` |
| Tests | `tests/test_open_addons.cpp` (`mv_import_tests`), `tests/test_open_request.cpp`, `tools/addon-sdk/test_mvaddon.py` (ctest `addon_sdk`), Mac rig `MV_ADDON_SELFTEST` |

Not built: libFuzzer harnesses for the package reader, manifest and theme parser.

## Implementation notes (PR 57, 2026-10-11)

Lua 5.4 in a sandbox per add-on ([§9](#9-code-how-a-strangers-add-on-runs) A); reasons in
[12](12-decision-log.md) ("Other makers' code runs as Lua 5.4").

| Piece | Code |
|---|---|
| Lua 5.4.8, vendored, static, MIT, without `io` / `os` / `package` / `debug`; marked watchdog changes; no finalizers or weak tables | `third_party/lua` (`VENDOR.txt`), `cmake/script.cmake` (`mv_lua`, built as C++) |
| Sandbox, limits, watchdog, `mv` bindings, Lua ↔ JSON | `src/addon/script_runtime.*`, `src/addon/script_thread_{posix,win}.cpp` (4 MB-stack worker) |
| Host's script add-ons: load from the store (re-hashed as read), commands, status, Start again, spin rig | `src/addon/script_host.*`; `open_store::verified_file` |
| Manifest fields; sheet lines | `src/addon/open_manifest.*`, `src/addon/open_json.cpp` |
| Key clashes give way to the base app | `shell/command_table.cpp` (`addon_command_row.script`, `addon_command_notes_json`) |
| Windows | `mv_script_*` (`abi/script_abi.cpp`); `shell/main.cpp` (start after the chrome, run rows, `kMsgScripts` notices); `IslandHost.OpenAddons.cs`, `AddonNative.ScriptStatus` / `ScriptRestart` |
| Mac | `shell/addons_mac.mm`, `main_mac.mm` (`MvAppShowNotice`, `MV_SCRIPT_SELFTEST`); `OpenAddons.swift` (`ScriptStatus`, Start again); bridge `mv_open_addons_scripts` / `_restart` / `_reload` |
| SDK, guide, example | `tools/addon-sdk/mvaddon.py` (`init --script`; compiled Lua refused at pack), `docs/ADDONS.md` "Scripts", `examples/addons/folder-count` |
| Tests | `tests/test_script_runtime.cpp`, `tests/test_script_addons.cpp` (`mv_script_tests`), `tests/test_key_router.cpp`, `tools/addon-sdk/test_mvaddon.py`, `tools/fuzz/fuzz_script.cpp` |

Test rigs: `MV_SCRIPT_STORE` (scratch store), `MV_SCRIPT_SPIN=<id>/<fn>` (keeps a worker busy).

### The manifest (schema 2, API 3)

```json
{
  "api": { "min": 3, "max": 3 },
  "scripts": ["main.lua"],
  "permissions": ["folder.read"],
  "contributes": {
    "commands": [
      { "id": "count-files", "name": "Count files", "key": "Ctrl+Shift+K", "run": "count_files",
        "modes": "viewing", "payload": "none" }
    ]
  }
}
```

- **`scripts`**: 1–16 `.lua` files, ≤ 1 MB each, ≤ 4 MB total, run in order in one environment,
  text only (`mode "t"`).
- **`permissions`**: `folder.read`, `thumbnails`, `selection`, each once, only with scripts.
  `files.write` / `metadata.write`: `needs_update`. `network`: malformed, always.
- **`commands`**: 1–32; `id` `[a-z0-9-]{1,32}`, `name` display text, `key` a printable label
  (`Ctrl` is `Cmd` on the Mac), `run` a Lua name, `modes` `viewing|video|browse|all`, `payload`
  `none|marks|marked_or_current|screen` (all but `none` need `selection`). Only with scripts.
- `native`, `chrome`, `main` still refuse; every package refusal holds for code.

### The script API

A part whose permission was not granted is **absent** (`mv.folder == nil`).

| Name | Permission | |
|---|---|---|
| `mv.id`, `mv.api`, `mv.permissions` | — | id, 3, granted set |
| `mv.notify(text)` | — | one line in the host's notice, prefixed with the add-on's name; ≤ 200 bytes, ≤ 4 a second |
| `mv.log(...)`, `print` | — | local only (macOS unified log, private; not kept on Windows); ≤ 50 lines a second, the rest counted in one line |
| `mv.on(event, fn)` | — | handlers for later points' events (`script_call::call_handlers`) |
| `mv.settings.get` | — | the add-on's settings-page values |
| `mv.folder.count()`, `.name(i)`, `.path()` | `folder.read` | the viewer's listing (Mac `folder_model`, Windows `mv_folder_*`) |
| `mv.selection.current()` | `selection` | the file on screen |

Standard library: `string` without `dump` (metatable locked: `getmetatable("")` is `false`),
`table`, `math`, `utf8`, `coroutine`, base functions without `dofile` / `loadfile` / `require` /
`warn`; `load` text-only into the sandbox; `collectgarbage` with `count`, `collect`, `step`,
`isrunning` only. `__gc` never runs and `__mode` has no effect. A command's function gets its
payload as a Lua value. `thumbnails` adds nothing to the environment; it gates thumbnail nodes.

Extension hooks: `add_script_binding({name, api, permission, install})` adds `mv.*` tables per
state; `script_addon::post(script_task)` runs work on the worker (dropped when stale);
`script_call::call_handlers`, `with_state`, `lua_to_json` / `push_json`; `script_host::find(id)`
hands the add-on to a slot registry, whose stop callback detaches items.

### Limits (as shipped)

| Limit | Value | Enforced by |
|---|---|---|
| Memory | 64 MB per add-on | `lua_Alloc`: refused growth stops the add-on, and stays refused |
| Instructions | 100 M per task | count hook every 10,000 instructions |
| Wall clock | 1 s per task | the hook, plus one watchdog thread (asleep unless a task runs) for loops with no VM instruction (pattern matching, `string.find`, `table.move` / `insert` / `remove` / `sort`) |
| C stack | 4 MB worker | Lua's 200-level C limit |
| Queue | 256 tasks | `post` returns false past it |

A task is a command call, a handler call, or the scripts' first run. After a limit every
instruction raises, so `pcall` cannot save the add-on; its state is closed on its worker. Settings
→ Add-ons → From others shows "Stopped." with the limit in words and **Start again**. A Lua error
is not a stop: Settings shows the last error.
