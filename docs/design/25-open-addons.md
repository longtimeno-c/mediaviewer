# 25 — Open add-ons: anyone can make one, and what one can change

**Status: proposed 2026-09-29, from issue #79 and the owner's call the same day ("add-ons can be
made and installed by anyone, not just our repo… if people have a compatible file / URL they can
install their add-on… add-ons can do a wide range of things like add new screens"). Post-v1,
Windows and macOS together (D9). Milestone L, PRs 55–60. Not a D-decision. PR 55 is built
([Implementation notes](#implementation-notes-pr-55-2026-09-29)); PR 56 is built, both halves
([first](#pr-56-first-half-import-described-by-its-manifest-2026-10-04),
[second](#pr-56-second-half-declarative-contributions-2026-10-11)); the owner's calls on code, network,
the file type and licensing were made on 2026-10-03 ([§17](#17-decisions-owner-2026-10-03)). PRs 58
(screens) and 59 (slots) are built for MediaViewer's own native add-ons, ahead of PR 57's scripts
([Implementation notes](#implementation-notes-prs-58-59-2026-10-11)). Owner, 2026-10-11: an add-on
carries its own code and its own data ([§1](#1-what-this-is)). PR 57, code, is built on both hosts
(2026-10-11, [Implementation notes](#implementation-notes-pr-57-2026-10-11); its Windows half is
compiled by CI and not yet run).**

## 1. What this is

Two things, which today are one closed thing:

1. **Anyone can publish an add-on.** Today an add-on loads only when MediaViewer's own release
   key signed it ([18](18-import.md) "Signed, verified, then loaded"), and on the Mac only when
   MediaViewer's own Team ID signed its code. This plan adds a second kind, the **open add-on**:
   one `.mvaddon` file, signed by whoever made it, installed from a **file** or a **link**.
2. **An add-on can change much more of the app.** Today each add-on gets bespoke hooks written
   twice, once per host (the path-bar search icon, the Settings page it owns). This plan adds a
   **contribution model**: named points an add-on fills in — themes, settings pages, commands and
   keys, chrome slots, search providers, whole screens — that both hosts read from one table.

**An add-on carries its own code and its own data (owner, 2026-10-11).** A feature delivered as an
add-on lives in its package: its library, any chrome, and its data, signed together and installed
and removed together. It is not shipped dormant inside the app with only the bulk downloaded (the
2026-09-28 call made for Final Cut Pro search, which this replaces as the rule; [12](12-decision-log.md)).
There is still never a second *app* to install or open. Places (#344) is the first: `src/geo` and
its place data ship in the Places package, and fill the screens and slots below.

**What does not change:** Import, Local search and Voice ([18](18-import.md),
[17](17-local-ai-search.md), [19](19-voice.md)) keep their channel, their key, their native code
and their folders. Nothing here loosens that path. An open add-on never loads through it, and a
first-party add-on never loads through this one.

## 2. Rules, and how an open add-on holds them

| Rule / decision | How it holds |
|---|---|
| 1 — nothing blocking on UI / render | Contributions are **resolved at load into plain tables** the chrome reads; nothing an add-on supplies is looked up per frame, per tile or per key. Add-on code (PR 57) runs on its own worker and is never called synchronously from the UI or render thread |
| 2 — the canvas is C++'s | A theme colours the chrome. It never touches a photo's pixels, the swapchain, or the colour pipeline (D6) |
| "The view comes first" | No add-on is read before first pixel. The chrome paints with the theme tokens it **cached** at the last run and verifies the add-on behind it afterwards |
| 5 — never modify an original | No contribution point writes to a user's file. When add-on code gets file writes (PR 60) they are new files in a folder the user picked, and metadata goes through PR 12's checked writer |
| 6 — nothing about a user's files leaves the machine | API 1–2 add-ons are data: they cannot read a file or open a connection. Code (PR 57) gets **no network access at all** and **no file access by default**; file access is a named permission shown before install. The app itself contacts a third party's server only when the user clicks (install from link, check for update), with a plain GET: no cookies, no identifier, nothing about files |
| D1 / D9 | Every contribution point exists on both hosts in the PR that adds it. An add-on is written once; WinUI and SwiftUI each render it natively. No HTML, no web view, no Electron |
| Absent means absent | With no open add-on installed nothing is listed, nothing is read at start, no folder is created. The base install is byte-identical whatever is installed (add-ons live in the user's profile) |
| Signed, verified, then loaded | Still true, with the publisher's key in place of ours: signature over the manifest, SHA-256 per file, nothing extra in the folder, checked at install **and at every load** |
| Host API versioning | A manifest declares the contribution API range it was written for. Outside it: "needs a newer MediaViewer", never a crash, never a half-loaded add-on |

## 3. The package

One file, `name.mvaddon`. It is a ZIP, so anyone can open one and look inside, but the app reads
only the narrowest ZIP there is, because it comes from a stranger:

- every entry **stored** (no compression): nothing can inflate past the bytes on disk, and the
  reader carries no inflate code to attack;
- no ZIP64, encryption, data descriptors, extra fields, comments, or folder entries;
- entries laid end to end from byte 0 to the central directory, which ends where the end record
  begins, which is the last 22 bytes. No gap to hide anything in;
- every name a safe relative path (`/`-separated, no `..`, no drive, no control character, none
  of `<>"|?*`, no part ending in a dot or space, no part a Windows device name such as `con`,
  `nul.json` or `com1`, so nothing written on install lands on a device), no two names equal when
  case is folded;
- **64 MB and 2,048 entries at most.** A package is themes, settings and scripts, not models.

It holds `manifest.json`, `manifest.json.sig` (64-byte raw Ed25519, detached, over the exact
manifest bytes) and the files the manifest lists. **Anything else in the package refuses it.**
The app reads the file once into memory; what was inspected is, byte for byte, what is
installed. `tools/addon-sdk/mvaddon.py` writes this format; a hand-made ZIP that is compressed
is refused with a reason that says so.

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

- **`id`** is `publisher.name`: two or more dotted parts of `[a-z0-9-]`. MediaViewer's own ids
  have no dot (`import`, `ai-faces`), so the two kinds cannot collide. `mediaviewer.` is
  reserved. No part is a device name Windows keeps (`con`, `nul`, `com1`…), since the id is a
  folder.
- **`publisher.key`** is the Ed25519 public key whose signature is in `manifest.json.sig`.
  MediaViewer's own release key is refused here: a schema 2 add-on cannot borrow our name.
- **`api`** is the contribution API range ([§7](#7-the-contribution-model)). `min` above the
  app's → "needs a newer MediaViewer".
- **Text a person will read** (`name`, `publisher.name`, `description`, licences) is bounded
  and may not hold control characters, bidirectional overrides or zero-width characters: the
  consent sheet must show what is written.
- **Links** (`publisher.url`, `update_url`) are `https://` only, printable ASCII, no
  credentials.
- **Strictness:** a manifest that claims no API newer than the app's may hold no key the app
  does not know (a typo fails loudly instead of doing nothing). One that reaches past the app's
  API may, so one package can serve an older and a newer MediaViewer.
- **Native code is refused by name.** `native`, `chrome`, `main` at the top level or under
  `contributes` refuse the add-on (`code_not_allowed`), whatever its range: other makers' code
  runs only as a script ([§9](#9-code-how-a-strangers-add-on-runs)).
- **Scripts (API 3, PR 57).** `scripts` lists the package's `.lua` files, run in order in one
  sandbox; `permissions` names what they may reach; `contributes.commands` adds key-bound rows
  that call a script function. A range that ends before API 3 cannot carry them (malformed),
  and a host before PR 57 refuses them by name (`code_not_allowed`), so no API range turns a
  data add-on into one that runs. The shape is in [Implementation notes (PR 57)](#implementation-notes-pr-57-2026-10-11).

## 5. Identity and trust

**What the signature means, and what it does not.** It proves the files are the ones the
publisher packed, and that version 1.3 comes from whoever made 1.2. **It does not prove who the
publisher is.** Anyone can generate a key and type any name. MediaViewer runs no registry, no
review, and no store, and says so where it matters:

```
┌ Install “Film Tones” 1.2.0? ─────────────────────────────────────┐
│ From        Acme Pictures · acme.example                          │
│ Key         3f9a-02c1-77de-b410                                   │
│ Adds        2 themes                                              │
│ Can         change how MediaViewer's bars, panes and text look    │
│ Cannot      run code, read your files, or use the network         │
│ Size        2 KB · MIT                                            │
│                                                                    │
│ MediaViewer has not checked this add-on or who made it.           │
│                                          [ Cancel ]  [ Install ]  │
└────────────────────────────────────────────────────────────────────┘
```

- **Where it shows:** in Settings, at the foot of Add-ons, as a panel in place of the list
  (the pattern Import's Remove confirmation uses). A package handed to the app opens Settings
  on the General page and scrolls the panel into view.
- **Cancel is the default button.** `Enter` does not install.
- **Key fingerprint:** the first 8 bytes of SHA-256(key), four groups of four hex digits. A
  publisher prints it where they publish; a careful person compares.
- **The "Can / Cannot" lines are computed from the manifest**, not written by the publisher.
  Under API 1–2 "Cannot" is always the line above. From PR 57 each permission the add-on asks
  for is a "Can" line in plain words.
- **The key is pinned per add-on at first install** (`publisher.json`, written by the app beside
  the versions). A later package with the same id and another key is **refused**
  (`other_publisher`), with "remove the installed one first" as the way through. A copied id
  cannot replace an installed add-on.
- **Consent is bound to bytes.** The sheet is shown for a package whose SHA-256 the core
  returned; install takes that hash and refuses if the file it is handed differs (`changed`).
- **No downgrades**, as for our own ([18](18-import.md)): an older version than the one
  installed is refused; the same version again is a repair.
- **A folder dropped into place is not an install.** Without the app's own `publisher.json` it
  is listed as "not approved" and not loaded; the way in is the consent sheet.

## 6. Install, update, remove

| | Behaviour |
|---|---|
| **From a file** | Settings → Add-ons → **Install from file…** (the OS file panel), or drop a `.mvaddon` on the window, or open one with MediaViewer. Then the consent sheet |
| **From a link** | Settings → Add-ons → **Install from link…**, paste an `https://` link. A plain GET: no cookies, fixed `User-Agent: MediaViewer`, redirects to `https` only, stopped at 64 MB. Then the same consent sheet. The line under the field says the server will see this computer's network address, as any download does |
| **Update** | Each installed add-on with an `update_url` has **Check for update**. It is the link flow with the link filled in, and it accepts only a newer version from the pinned key. **Never automatic:** the app makes no background request to a third party's server, so a publisher cannot learn when or how often MediaViewer runs |
| **Remove** | One click, no restart for data add-ons. A theme in use falls back to the default at once |
| **Where** | `%LocalAppData%\MediaViewer\open-addons\<id>\<version>` · `~/Library/Application Support/MediaViewer/Open Add-ons/<id>/<version>`. Beside, not inside, the first-party `addons` folder, so neither store ever lists the other's |
| **Not offered** | A `mediaviewer://` link that starts an install from a web page (a drive-by prompt); silent or automatic install; install for all users |

## 7. The contribution model

An add-on's manifest says what it contributes. The host resolves every contribution **once, at
load, off the UI thread**, into tables the chrome reads. Each point is versioned by the API that
introduced it, exists on both hosts, and has a default the base app provides.

| Point | API | PR | What the add-on supplies | Base default | Performance rule |
|---|---|---|---|---|---|
| **Built-in add-ons by manifest** | — | 56 (first half, built 2026-10-04) | A first-party manifest (schema 1) says its one-line description, its commands with their default keys and modes, and the hint it asks for; the app reads them instead of naming the add-on | Import's two built-in rows and its hard-coded Settings text, for a manifest from before it said any | Rows are resolved once at load into eight reserved command slots (`addon_cmd_0..7`), appended to the live table; dispatch is still an array index |
| **Theme** | 1 | 55 | Colour tokens (dark and / or light), a font family | The system-following palette, CozetteVector | Tokens cached by the chrome; read before the add-on is verified, corrected after |
| **Theme, shape** | 2 | 56 (built 2026-10-11) | Corner radius scale, density, type scale (theme file schema 2) | Today's literals | Same table, resolved once when the theme is painted |
| **Settings page** | 2 | 56 (built 2026-10-11) | A schema: sections, toggles, choices, numbers, text, each with a key, default and help; `enabled_by` greys an item while a toggle is off | — | Rendered by the host when Settings opens; values in one file per add-on id the core keeps (not each host's own store: [12](12-decision-log.md)), kept on remove unless asked |
| **Keymap** | 2 | 56 (built 2026-10-11) | Bindings for existing commands (the v1.1 "named layouts": FastStone, IrfanView, vim) | `default_bindings()` | Applied to the live table once; the router stays an array index ([16](16-commands.md#one-table-editable)) |
| **Command** | 3 | 57 (built 2026-10-11) | A name, a default key, and the script function it runs | — | Rows appended to the table at load; a key clash is resolved for the base app, the add-on's key is dropped and Settings says so |
| **Screen** | 4 | 58 (built 2026-10-11, first-party native) | A view tree ([§10](#10-screens)) in a window, a pane or a sheet | — | Built by the host from data; updates are diffs posted from the add-on's worker |
| **Chrome slot** | 5 | 59 (built 2026-10-11, first-party native) | Command-bar and path-bar items, context-menu entries, info-pane sections, status items, gallery tile badges | — | Badges come from a table filled off-thread; **no add-on call per tile**, ever |
| **Search provider** | 5 | 59 (built 2026-10-11, first-party native) | Answers a query with a list of files | File-name search (base app, 2026-09-28) | Runs on the add-on's worker with the query's generation; a stale answer is dropped |
| **Export / files** | 6 | 60 | Writes new files into a folder the user chose; metadata through PR 12's writer | — | Jobs on the job system, generation-tagged |

Local search becomes a search provider in PR 59 instead of owning the path-bar button (#76); it
keeps its native pack and its own channel. As built (2026-10-11): while the pack is attached the
host lists it in the registry as the local-scope provider, a Local search query goes to every
other local-scope provider too, and the result list's path row offers their rows ("+ 3 in
Portugal"); the pack's own rich results stay in its panel. A query with place terms
(`place:portugal`) is sent to no other provider (#441): the pack asks the `place:` provider
itself, through the host table's `place_matches`, and intersects ([17](17-local-ai-search.md)
"Places over the index").

## 8. Themes (API 1)

A theme file, one per theme the add-on lists:

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
| `canvas` | The chrome's background, and the viewer's on every screen: the empty window, and around a photo or a video (issue #408; there is no canvas background of its own) | `Canvas`, `PanelBg`, native `home.canvas` | `canvas`, host `home.canvas` |
| `surface` | Rows, fields, cards | `Surface` | `surface` |
| `title` | Primary text | `Title` | `title` |
| `body` | Secondary text | `Body` | `body` |
| `disabled` | Disabled text | not read yet: a disabled control is dimmed by opacity | `disabled` |
| `hairline` | Separators, outlines | `Hairline` | `hairline` |
| `accent` | Selection, progress, focus, marks | `TrimAccent`, `StarOn`; `Selection`, `TrimKeep`, `TextSelection` derive from it as they do from the system accent | `accent` (new; replaces `Color.accentColor` in the chrome, and tints the system's controls) |

- **All seven are required** in each palette a theme gives. `#rrggbb` or `#rrggbbaa`; `canvas`
  and `surface` must be opaque.
- **One palette or two.** With both, the chrome follows the system's light / dark. With one, the
  chrome keeps that appearance while the theme is on (system-drawn controls are told to match,
  so a toggle is never light on a dark theme).
- **A theme nobody can read is a trap**, because Settings is where it is turned off. The host
  refuses a palette whose text does not stand out, by WCAG contrast against both `canvas` and
  `surface`: `title` 4.5 : 1, `body` and `accent` 3 : 1. The SDK reports the measured ratios.
- **High contrast wins.** With the system's high-contrast / increased-contrast mode on, the
  chrome uses the system's colours and the theme waits.
- **`font`** names a family installed on the machine. Missing → the chrome's own face. A theme
  ships no font file in API 1 (the backlog's user font is API 2's to weigh). On the Mac it
  changes at once; on Windows every control holds the face it was built with, so a new
  typeface shows at the next start and Settings says so.
- **The empty window** (issue #408) is drawn natively by ImGui, so the hosts hand the render
  thread the resolved tokens in the input snapshot (`input_state.h` `home`): `canvas` for the
  clear, `surface` for the welcome card and its hovered row, `title` and `body` for the type,
  `hairline` for the rule and the edges, `accent` for the hover and the glyphs. Default sends
  the system's own colours (AppKit's semantic colours on the Mac, the chrome's system palette
  on Windows) and high contrast the system's contrast colours, so the native side never
  derives a colour. The chrome already holds the tokens, so this reads no file before first
  pixel. Not built yet: a theme's `font` on the card (the card keeps CozetteVector); no
  built-in sets one.
- **Not themed:** photo and video pixels, the canvas's colour pipeline, the F3 overlay (ImGui,
  a lab tool), the histogram's channel colours (they carry meaning), the hover washes (they
  follow the appearance, not a token). The first-party add-ons' own windows read the host's
  tokens since PR 56 (Import and Duplicates on both hosts, Local search on the Mac; the Windows
  AI chrome already did).
- **Where shape reaches** (theme schema 2; issue #402): every padding, spacing and corner radius
  of the host chromes goes through the shape tokens: `MVTheme.space` / `MVTheme.radius` on the
  Mac, `Space` / `Pad` / `Corner` on Windows. That includes the add-on slot surfaces (#348) and
  script screens (PR 57). These stay literals because they are geometry, not spacing: the photo
  grid's tiles, gutter and inset (the keyboard's column math), the filmstrip's cells and gutter
  (on Windows the cells fill the native strip), the video transport bar (Windows sizes its
  native island from `kTransportPadDip`), the editor timeline's tracks, pills and circles
  (radius = half the height; the Mac's `Capsule`), hairlines and highlight outsets, and the
  metadata pane's label column. The add-on windows (Import, Duplicates, Local search on the
  Mac) scale corner radii only.
- **Choosing one:** Settings → Appearance → **Theme**: Default, then nineteen host palettes
  compiled into both chromes — Dark, Light, Midnight (rain), Cyberpunk (a circuit), Retrowave
  (embers), Forest (petals), Starfield, Terminal (fireflies), Organs (slow rain), Ume (petals),
  GPT, Claude, Cute (sparkles), Eclipse (a starfield), Porcelain (dots, light), Arcade (a
  circuit), Blueprint (dots), Monolith (a slow drift) and Yoyo (fireflies) — then every theme
  of every installed add-on. Installing an add-on does not switch. Each is one palette and
  sets no `font`, so the chrome keeps that appearance, system-drawn controls included, and a
  choice applies at once on both hosts. They are not packages: no publisher key and no install
  sheet. A theme someone else ships is still Contribution API 1, and still appears after the
  built-ins. Default clears the override (`mv.theme.*` on the Mac, `theme.json` beside
  `settings.ini` on Windows).
- **Ground effect:** a built-in may draw one effect behind the chrome — falling rain, a
  starfield, fireflies, petals, embers, sparkles, a circuit, a slow drift, or a still dot
  grid. Dark, GPT and Claude draw none. It is not a token and not part of the theme JSON, so
  API 1 is unchanged and an add-on cannot set one. It is drawn by the host in its own panes,
  and since issue #408 on the empty window behind the welcome card (`shell/home_ground.h`, the
  panes' field ported mark for mark, one static present a tick). It stops once a
  photo or a video is up; it is never drawn on a photo, a video or an error card, and it does
  not change the clear. It moves on every display frame (60 a second, 120 on ProMotion) while the
  app is in front and someone has
  moved the pointer or a key in the last five seconds (over the canvas, the bar or a pane), and
  10 times a second while the window is visible, behind another app or quiet, for up to a minute
  after the last input; then it rests. It never moves while the window is hidden, minimised or
  covered (on Windows, covered means DXGI occlusion: the panes cannot tell), and it stops under
  Reduce Motion (Mac) or when Windows animations are off (owner, 2026-10-15). High
  contrast still hides the theme, effect included. Dots do not move, and neither does a theme
  with no ground. Organs' rain is much slower and smaller than Midnight's. A pane that uses
  the system material (the Mac filmstrip, folder tree, help and transport) keeps that
  material, so the picture can show through it.
- **Start-up:** the chrome stores the resolved tokens of the theme in use in its own settings
  and paints with them at once. The add-on is verified on a worker afterwards; if it is gone,
  changed or refused, the chrome returns to Default and says why in Settings. A built-in theme
  is resolved from the palette compiled into the chrome. The cache records which one, and the
  resolved tokens, and is rewritten when they differ. That adds no file read before first pixel.

## 9. Code: how a stranger's add-on runs

Themes, settings and keymaps are data. Commands, screens, providers and exports need code.
Three ways to run it, and the one chosen. **Decided by the owner, 2026-10-03: A** (the
recommendation below); the table stays as the record of why.

| | A. Sandboxed script (recommended) | B. Native, in process (what Import is) | C. Native, in a helper process |
|---|---|---|---|
| One package for both OSes | **Yes** | No: a DLL + WinUI assembly and a dylib + Swift bundle, built twice | No |
| Rule 1 (UI never blocks) | Holds: the script has its own worker and cannot reach the UI thread | **Cannot be held**: their code runs on our threads | Holds |
| Rule 6 (nothing leaves) | Holds **by construction**: no socket or file call exists unless a permission puts one in the table | **Cannot be held**: native code can do anything the user can | Holds only with an OS sandbox per platform |
| A crash in the add-on | The add-on stops; the viewer carries on | Takes the viewer down, in our crash reports | The helper dies |
| Mac hardened runtime | Untouched | Needs **library validation switched off** for the whole app, weakening it for everyone, add-ons or not | Helper signed by us; their code still needs loading into it |
| Licence (GPL-3.0-or-later app) | Scripts are the author's | Code linked into the app must be GPL-compatible | As B for the helper |
| Screens | Described as data, rendered natively by each host ([§10](#10-screens)) | Anything WinUI / SwiftUI can draw | No way to host another process's views in SwiftUI |
| Speed | Interpreted: glue, not pixel loops. Heavy work is a host call | Native | Native, plus IPC |
| Build cost to us | A runtime, its host API, its limits | Little: the loader exists | Two sandboxes, IPC, a view protocol |

**A it is**, with Lua 5.4 as the runtime (MIT, ~250 KB, no JIT so the hardened runtime needs
no exception, the language Lightroom's plug-ins already taught photographers' tool-makers).
WebAssembly (stronger isolation and any language, but a heavier toolchain for the person writing
a first add-on) was the alternative weighed; PR 57's first spike may still swap the two if Lua's
sandbox cannot hold the limits below, and says so in [12](12-decision-log.md) if it does. Under A:

- **One worker per add-on.** Host calls that answer later post to it; it never runs on the UI,
  render or decode threads.
- **Limits:** a memory ceiling through the allocator, an instruction budget per event with a
  watchdog, no standard `io` / `os` / `package` / `debug` libraries. Over a limit the add-on is
  stopped and Settings says which one and why.
- **Permissions** are manifest entries, each a line in the consent sheet, none granted by
  default: `folder.read` (the folder model and metadata of what is open), `thumbnails`,
  `selection`, `files.write` (new files, in a folder the user picks each time or once),
  `metadata.write` (through PR 12's writer). **There is no `network` permission** (owner,
  2026-10-03): no add-on can open a connection, so rule 6 holds for other people's code as it
  does for ours, by construction. An add-on that wants a service does not get one; the app
  itself fetches only what the person clicked (install from a link, check for update).
- **The host API** is the v1 / v2 function table's read side ([18](18-import.md),
  [17](17-local-ai-search.md)) re-exposed per permission, plus the contribution points.

B stays what it is: MediaViewer's own add-ons, under our key and Team ID. C is not planned.

## 10. Screens

"Add new screens" without shipping WinUI and SwiftUI code twice: a screen is a **view tree**, a
small vocabulary both hosts render with their own controls.

- **Places:** a window of its own (like Import's), a docked pane, a sheet over the viewer.
- **Vocabulary (PR 58):** stack, grid, scroll, split, text, image, thumbnail (a file's, from
  the viewer's cache), thumbnail grid (virtualised by the host), list, table, button, toggle,
  choice, slider, number, text field, progress, separator, and `surface` (pixels the add-on
  drew, for what the vocabulary lacks). Since #442 also `tiles`, and looks on stack, progress,
  text field and text (implementation notes below).
- **State** lives in the add-on. It posts a tree, then diffs; the host applies them on the UI
  thread in one pass. Events (click, change, key, selection) post back to the add-on's worker.
- **Keyboard-complete** by construction: every control the host renders is focusable and named;
  the add-on's commands are rows in the command table ([16](16-commands.md)).
- **Theme and accessibility** come from the host's controls, so a screen follows the theme,
  high contrast, text size, VoiceOver and Narrator without the add-on doing anything.
- **Windows control limits apply** (`tools/check-winui-controls.ps1`): the vocabulary maps only
  to controls that load in the islands, with the existing stand-ins for the rest.

## 11. Performance

- **Launch:** zero add-on work before first pixel. No open add-on is listed, verified or parsed
  until the chrome is up, and then on a worker. The launch → first pixel and launch → full
  resolution numbers must not move with add-ons installed; PR 55's verify measures it.
- **Steady state:** contributions are tables. The theme is brushes recoloured once. A keymap is
  the binding table. A command is a row. Nothing is resolved by name per frame, tile or key.
- **Idle:** an add-on with nothing to do costs nothing: no timer, no poll. 0 presents and ~0 %
  CPU on a still hold with add-ons installed.
- **Verification cost:** SHA-256 of a package's files at load, once per process (the existing
  per-folder cache). At 64 MB that is tens of milliseconds, on a worker.

## 12. Licensing

- The manifest names an SPDX licence for the add-on and for each file, as ours do. MediaViewer
  shows it and does not police it.
- A data add-on (API 1–2) is data: its licence is its author's choice.
- The SDK (`tools/addon-sdk`) and the examples are **MIT**, so an add-on author takes on no
  GPL obligation by using them. (The app stays GPL-3.0-or-later.)
- **Add-ons are the author's, under any licence** (decided 2026-10-03, delegated by the owner):
  the app's licence carries an additional permission under GPL-3.0 section 7
  ([LICENSE-ADDONS.md](../LICENSE-ADDONS.md)) for works that reach MediaViewer only through the
  documented add-on interfaces (the `.mvaddon` package, its manifest, the contribution points and
  the script host API). Without it the question of whether a script "links" to a GPL program
  through an interpreter's bindings has no settled answer, and an author should not need one to
  publish a theme. Code that links the core directly (MediaViewer's own add-ons) stays under the
  GPL as before. The exception can be withdrawn for future versions by the owner, not by anyone
  else; add-ons already published keep it.

## 13. For people who make add-ons

- **`tools/addon-sdk/mvaddon.py`**: `keygen` (a publisher key; the private half never leaves
  the author's machine), `init` (a starter folder), `pack` (manifest, signature, the strict
  ZIP), `check` (what the app will say, including each theme's contrast ratios).
- **`docs/ADDONS.md`**: the author's guide — format, manifest, theme tokens, the consent sheet
  their users will see, how to publish a link, how to keep a key.
- **`examples/addons/`**: a theme add-on that packs and installs as-is.
- **Cross-check:** the SDK's output is verified by the C++ reader in CI
  (`tools/addon-verify --open`), so the two cannot drift.

## 14. Roadmap (both platforms each; verify line per platform; viewer gates every PR)

Milestone L. Each slice holds both present-loop gates and the launch numbers with add-ons
installed.

| PR | Slice | Verify (both platforms) |
|---|---|---|
| **55** | **Open packages and themes.** The `.mvaddon` reader, manifest schema 2, publisher keys, the consent sheet, install from file and from link, check for update, remove, Settings → Add-ons "From others", Settings → Appearance → Theme, the token table on both hosts, the SDK, the author's guide | A package made by the SDK with a fresh key installs from a file and from an `https` link, shows the publisher and fingerprint, and themes the chrome; choosing Default restores it exactly. **Refused, each with its reason:** one changed byte in any file or the manifest; an extra entry; a compressed, encrypted or ZIP64 package; a path with `..`; a package naming code; a second publisher's package under an installed id; an older version; a theme under the contrast floor; a package swapped after the sheet was shown. The link request carries no cookie, query or identifier of ours. With an add-on installed and a theme on: launch → first pixel and launch → full resolution within noise of none installed (alternated runs), 0 presents idle, **both present-loop gates**. With none installed: no folder created, no file read at start |
| 56 | **Declarative contributions.** Settings pages from a schema, keymap packs, theme shape (radius, density, type scale), first-party chromes read the host's tokens | A settings page renders the same controls and order on both hosts from one schema; its values survive restart and removal-with-keep; a keymap pack rebinds and Reset restores; `?` lists what is bound |
| 57 | **Code** (owner call 1, made; **built 2026-10-11**). The runtime, one worker per add-on, limits and watchdog, permissions in the consent sheet, commands | A script command runs from its key on both hosts; a runaway loop and an allocation bomb each stop the add-on, not the viewer, within the watchdog's bound; without `folder.read` the folder API is absent; **both gates hold while a script spins** |
| 58 | **Screens** (built 2026-10-11 for first-party native add-ons, [notes](#implementation-notes-prs-58-59-2026-10-11)). The view vocabulary, windows / panes / sheets, diffs, events | One add-on's screen renders natively on both hosts, keyboard-only, VoiceOver and Narrator name every control; a 2,000-tile thumbnail grid scrolls without a hitch |
| 59 | **Slots and search providers** (built 2026-10-11 for first-party native add-ons). Bar items, context menus, info-pane sections, tile badges; file-name search and Local search as providers | Badges for a 10,000-file folder cost no add-on call per tile (counted); a slow provider never delays typing; removing the add-on removes every item |
| 60 | **Files.** `files.write`, exports, `metadata.write`. No `network`, ever | An add-on export never overwrites and never touches an original (rule 5, tested by hash); no add-on can open a connection, shown by the host API's surface |

**Sequencing:** 55 is useful alone and decides nothing about code. 57 → 58 → 59 → 60 are the
spine of "add new screens"; with the owner's calls made (2026-10-03) they can start, 56 before
or beside them.

### PR 56, first half: Import described by its manifest (2026-10-04)

Built ahead of the open-add-on settings pages, because it is what makes Import "an installed
add-on whose manifest says what it contributes" rather than a special case in both chromes:

- **Schema 1 manifests gain `description`, `contributes.commands` and `contributes.hint`**
  (`src/addon/manifest.*`, optional, a mistyped key is malformed). The packer writes Import's
  (`tools/package/addon-pack.py`): "Import…" on `Ctrl+Shift+I` / `Cmd+Shift+I` with the viewer's
  marks, "Import marked now" on `Ctrl+Shift+F7` / `Cmd+Shift+F7` with the marked or current
  files, and the card hint's words.
- **Eight command slots** (`addon_cmd_0..7`, `shell/commands.h`) take the loaded add-ons' rows
  at load (`set_addon_commands`): name, key (parsed from the manifest's label by
  `parse_key_label`, the inverse of `key_label`), modes and payload. They are appended after the
  built-in rows, so remaps of built-in rows keep their indices; a remap of a contributed row is
  lost when the add-on set changes. An add-on that contributes rows **supersedes the rows the
  table has built in for it** (Import's two lose their key while its own are live, and get it
  back when they go), so the router never answers the old row ahead of the new one on the same
  key. The eight placeholder infos keep the wire ids dense.
- **One run path per host.** Windows: `chrome_host::run_addon_command` → `IslandHost.RunAddonCommand`
  → the add-on's `IAddonCommands.RunCommand(id, json)`; the core serves the loaded manifests'
  rows through `mv_addon_commands_json` (ABI, additive) when the chrome reports an add-on
  loaded or unloaded. Mac: `MvAddonsRunContributedCommand` → the bundle's optional
  `runCommand:payload:`; the host reads the rows from the loaded add-on's manifest itself.
  **An Import built before this** (no `contributes`, no generic entry) keeps the built-in rows
  and is driven through its two frozen calls: nothing installed today breaks.
- **Settings' Import line and the card hint** come from the manifest (installed, else the
  channel's), with the built-in words for a manifest from before.
- **Not moved:** the AI pack's four commands (Ctrl+F's file-search fallback is the app's own
  behaviour, not the pack's), and the hard-coded list of first-party channels, which the app must
  know to offer a download at all.

### PR 56, second half: declarative contributions (2026-10-11)

Settings pages from a schema, keymap packs, theme shape, and first-party chromes on the host's
tokens: contribution API 2. Manifest schema stays 2. Calls made are in
[12](12-decision-log.md) ("Declarative contributions").

**The manifest.** `contributes.settings` (one page) and `contributes.keymaps` (up to eight
layouts) are API 2 points; a theme file with `"schema": 2` may carry `shape`. Every §7 point and
its API is one table, `kContributionPoints` (`open_manifest.h`), the same on the branches of
PRs 56–59. Under API 2 a manifest whose `api.max` is 1 may not name them (malformed); an API 1
host leaves them to a later one; `api.min` above the host's is "needs a newer MediaViewer",
never half-loaded. Every PR 55 refusal stands (code keys still refuse by name). A schema 1
(first-party) manifest may carry `contributes.settings` too: Places (#344) is its first user.
A change takes effect at once in an add-on that fills slots: see "A changed setting re-asks"
under PRs 58–59.

| Piece | Code |
|---|---|
| The page schema and its rules (sections, toggle / choice / number / text, `enabled_by`, bounds) | `src/addon/settings_schema.*` |
| The values: one JSON per add-on id, checked in and out, cached, keep or forget; each file names the publisher key it was written for, and values kept for one publisher are never read for another (installing another publisher's add-on under the id deletes them) | `src/addon/settings_values.*`; the folder is `io::addon_settings_dir()` (`src/io/paths*`), never created by a read |
| Keymap pack shape | `src/addon/keymap.*` |
| Applying a pack to the live table; stable command names | `src/shell/keymap.*` (`apply_keymap`, `command_slug`); `set_live_key` and Reset's superseded rows in `src/shell/command_table.cpp` |
| Theme shape | `src/addon/theme.*` (`theme_shape`) |
| One JSON for both hosts | `src/addon/open_json.*` (`addon_settings_page_json`, `addon_setting_set_json`, `addon_settings_forget`, `open_keymap_json`; `settings` / `keymaps` in the list and the sheet's lines) |
| Host table | `settings_get`, the row after `walk_files2` in `mv_host_api` (`host.cpp`), before the v3 rows of PRs 58–59; host API not bumped |
| C ABI 0.24 (Windows), chrome bridge (Mac) | `mv_addon_settings_json` / `_setting_set` / `_settings_forget`, `mv_open_addon_keymap_json`; `mv_addons_settings_*`, `mv_open_addons_keymap`, `mv_chrome_apply_keymap` |
| Mac chrome | `AddonSettingsPage.swift` (page, Key layout row), `OpenAddons.swift` (page under each add-on, Keep its settings), `AddonsView.swift` (Import's page), `Theme.swift` (`MVTheme.radius` / `space` / type scale; `MVChromeTheme` for the add-on bundles); `HostTheme.swift` (Import), `AITheme` (Local search) |
| Windows chrome | `IslandHost.AddonSettings.cs`, `IslandHost.OpenAddons.cs`, `IslandHost.Settings.cs` (Key layout), `IslandHost.Theme.cs` / `ThemePack.cs` (shape), `ImportLook.cs` (Import, Duplicates), `IAddonThemeShape` in `MediaViewer.Interop/Addons.cs`; native `chrome_cmd_apply_keymap` (1050) in `main.cpp` |
| SDK, examples, guide | `tools/addon-sdk/mvaddon.py` (checks pages, layouts, shape; API 2), `examples/addons/settings-demo`, `examples/addons/vim-keys`, `docs/ADDONS.md` |
| Tests | `tests/test_addon_contributions.cpp` (`mv_import_tests`), `tests/test_keymap.cpp` (`mv_tests`), `tools/addon-sdk/test_mvaddon.py` (cross-checked by `mv_addon_verify --open`), `tools/fuzz/fuzz_addon_contributions.cpp`, the Mac rig's PR 56 leg (`MV_ADDON_SELFTEST_PR56`) |

**Same controls, same order.** toggle → a switch (`ToggleSwitch`); choice → a menu of the labels
(`ComboBox`); number → a field with − / + (Windows: `FakeInput` and two buttons; Mac: a field and
a stepper) and the unit; text → a field (`FakeInput` / `TextField`). The page title, then each
section's title and help, then each item with its help beneath, in the manifest's order. Only
controls that pass `tools/check-winui-controls.ps1`.

**Coordination (2026-10-11).** With PRs 58–59 (#348): the point table, API numbers 4–5 for theirs,
and `settings_get` as the host-table row before their rows. With PR 57: API 3; contributed
commands are named `<add-on id>/<command id>` in packs; `mv.settings.get` reads
`settings_values::value_json` with the script's id, publisher and page (wired at integration,
2026-10-12). On merge,
`kContributionApi` / `MV_ADDON_CONTRIBUTION_API` and `MV_ABI_VERSION_MINOR` take the highest
and join their comments.

**Measured on the Mac** (arm64, Release, 2026-10-11/12, the owner's machine while other agents
built beside it: load average 8–49 through the runs). Base is `8ef8679`
(`plan/metadata-search-and-place`) in its own source copy and build directory; "add-ons" is the
new build with `example.settings-demo` and `example.vim-keys` installed in the profile and
Compact Dusk (a schema 2 theme with shape) on. Runs alternated base, new, add-ons; one untimed
warm-up open per file and build; `mediaviewer_lab --soak 6 --static`:

| File | Build | First pixel, s (5 runs) | Full resolution, s (5 runs) | Dropped |
|---|---|---|---|---|
| `sony_ilce7rm3.arw` | base | 0.061 · 0.060 · 0.145 · 0.054 · 0.040 | 1.451 · 1.489 · 1.477 · 1.433 · 1.172 | 1 · 1 · 2 · 1 · 0 |
| | new | 0.056 · 0.055 · 0.058 · 0.040 · 0.044 | 1.538 · 1.642 · 1.445 · 0.955 · 1.201 | 1 · 1 · 1 · 0 · 1 |
| | add-ons | 0.057 · 0.053 · 0.062 · 0.041 · 0.041 | 1.415 · 1.375 · 1.388 · 0.995 · 1.166 | 1 · 2 · 1 · 0 · 1 |
| `canon_eosr6.cr3` | base | 0.045 · 0.103 · 0.050 · 0.082 · 0.044 | 0.544 · 0.823 · 0.706 · 0.786 · 0.684 | 0 |
| | new | 0.033 · 0.060 · 0.044 · 0.096 · 0.046 | 0.474 · 0.838 · 0.704 · 0.837 · 0.678 | 0 |
| | add-ons | 0.099 · 0.048 · 0.048 · 0.049 · 0.040 | 0.796 · 0.737 · 0.752 · 0.610 · 0.509 | 0 |

The ranges overlap in every row. The absolute numbers are 2–3× PR 55's on this machine (full
resolution 1.2–1.6 s against 0.47 s): the machine was loaded, so these runs can show "no
difference between the builds", not the product's speed. The `--static` report's idle presents
were 0–42 on every build alike (base included). Mac PR 1's gate (`frametime --seconds 60`),
alternated:

| Run | Build | Animated: frames · p99 ms · max ms | Idle: presents · CPU of one core |
|---|---|---|---|
| 1 | base | 3600 · 16.85 · 16.94 | 0 · **64 %** |
| 2 | add-ons | 3600 · 16.75 · 16.89 | 0 · **42 %** |
| 3 | base | 3600 · 16.85 · 16.95 | 0 · **80 %** |
| 4 | add-ons | 3568 · 16.85 · 538 | 0 · **83 %** |
| 5 | base | 3537 · 16.90 · 1068 | 0 · **3.1 %** |
| 6 | add-ons | 3600 · 16.85 · 22.2 | 0 · **1.5 %** |

Pacing is the same on both builds; the long frames land on base and add-ons alike, under the
load. **0 presents idle on every run.** The idle CPU clause (≤ 1 %) fails on every run, base
included, as in PR 55, so "both present-loop gates hold" is not shown on this machine for this
PR either. Windows: not measured.

PR 56's verify line, where each part stands:

| Verify | State |
|---|---|
| A settings page renders the same controls and order on both hosts from one schema | **Mac: run in the app** (rig, `p2-page.png` / `p4-page.png`: title, sections, switch, switch, field + stepper + "km" greyed by `enabled_by`, menu, field, in the manifest's order). **Windows: built and compiled only** (same order and mapping in code); owed in the app |
| Its values survive restart and removal-with-keep | **Mac: run in the app** across two launches (40 / false / "local" / "Porto" read back after the restart; an out-of-range 400 refused with its message; removed keeping: file kept, page gone; installed again: the values back; removed without: file gone). Core: Catch2. **Windows: owed** |
| A keymap pack rebinds and Reset restores | **Mac: run in the app** (Vim: Previous A→H, Next D→L, Toggle mark Shift+Space→X; Flip horizontal and Forward 10 s left unbound and reported; Reset: every row back). Core: every built-in command rebound and reset in `tests/test_keymap.cpp`. **Windows: owed** (native apply compiled only in CI) |
| `?` lists what is bound | **Mac**: the rig reads the table `?` and Settings read; Catch2 checks each rebound row's `?` line. **Windows: owed** |
| Launch → first pixel within noise with these add-ons installed | **Mac: measured** (above, loaded machine). **Windows: owed** |
| 0 presents idle | **Mac: 0 on every gate run.** Windows: owed |
| Both present-loop gates | **Not shown**: Mac pacing holds, the idle CPU clause fails on base and new alike; Windows not run |

Owed before this merges: the Windows half run in the app (settings page, keep-on-remove, Key
layout and Reset, Import / Duplicates under a theme, shape at next start), VoiceOver / Narrator
on the page, a layout checked against the Mac's fixed menu chords, and the Windows fuzz build of
`fuzz_addon_contributions` (it ran 2.5 M inputs clean under libFuzzer + ASan + UBSan on the Mac).

### PR 57: code (2026-10-11)

Built on both hosts: Lua 5.4 in a sandbox per add-on, its limits and watchdog, permissions in
the consent sheet, commands from a script. What it is, the API, the limits and where each part
of the verify stands: [Implementation notes (PR 57)](#implementation-notes-pr-57-2026-10-11).

## 15. Not in this plan

A store, a registry, reviews, ratings or a hosted index; payments; automatic updates of
third-party add-ons; `mediaviewer://` install links; third-party native code in the app's
process ([§9](#9-code-how-a-strangers-add-on-runs) B); add-ons that replace a decoder, the
canvas, the colour pipeline or the present path; add-ons that change an original; a web view.

## 16. Risks

| Risk | Mitigation |
|---|---|
| A hostile package attacks the reader | The narrowest ZIP, a strict JSON parser with a depth limit, bounded sizes everywhere, no inflate; truncation and every-byte sweeps in the unit suite under ASan / UBSan; libFuzzer harnesses beside the decoders' (PR 6/7) are owed |
| People trust a name | The sheet says the add-on is unchecked, shows the fingerprint, and lists what it can and cannot do from the manifest, not from the publisher's words |
| An add-on's update is hijacked | Key pinned at first install; another key is refused |
| A theme makes the app unusable | Contrast floor at install; high contrast overrides; Default is one click, and one key (`Reset`, Settings) |
| Add-ons erode launch time one by one | Nothing before first pixel; cached tokens; PR 55's launch verify is inherited by every slice |
| Rule 6 weakened by other people's code | No network access at all, ever; no file access without a named permission in the sheet; none of either under API 1–2 |
| Dual-track doubles every contribution point | The model exists so a point is written twice **once**, not once per add-on |

## 17. Decisions (owner, 2026-10-03)

Asked as open calls on 2026-09-29; answered on the pull request on 2026-10-03.

1. **How a stranger's code runs:** the sandboxed script with screens described as data
   ([§9](#9-code-how-a-strangers-add-on-runs) A), as recommended. Third-party native code in the
   app's process stays out ([§15](#15-not-in-this-plan)).
2. **`network`: no.** There is no such permission; nothing an add-on does can reach the network.
3. **`.mvaddon` registered with the OS: yes.** Done in PR 55: the installer's `MediaViewer.Addon`
   ProgId and `.mvaddon` association on Windows, a document type and an exported UTI on the Mac
   ([Implementation notes](#implementation-notes-pr-55-2026-09-29)).
4. **Licence of add-ons:** delegated; decided as the GPL section 7 additional permission in
   [§12](#12-licensing), with the SDK staying MIT.
5. **"From others"** stays the wording.

## Implementation notes (PRs 58-59, 2026-10-11)

Built for MediaViewer's own native add-ons (§9 option B), ahead of PR 57's scripts, issue #348.
The first consumer is Places (#344): an info-pane section (place, coordinates, a map surface with
a chrome-drawn pin) and a search provider (`portugal`, `place:`, `in:`).

| Piece | Code |
|---|---|
| The view-tree vocabulary (all 20 node types), its checks, diffs and events | `src/addon/screen.*` |
| What a manifest contributes (screens, info sections, providers, items, badges; the symbol set) | `src/addon/contrib.h`; parsed for schema 1 in `src/addon/manifest.cpp` |
| The registry: tables built at attach, one worker per add-on, coalesced generation-tagged requests, trees, surfaces, rows, badge sets, status | `src/addon/slots.*` |
| Host table v3 (`screen_post` … `locations_in`) and the add-on side `mv.slots.1` | `mediaviewer_addon.h`, thunks in `src/addon/host.cpp`; `loaded_addon::load` attaches, its destructor detaches first |
| A file's coordinates, a bounded read | `src/meta/location.*` (becomes `geo_point` when #290 lands) |
| C ABI 0.25 for the Windows chrome | `mv_slots_*` in `src/abi/slots_abi.cpp`, `mv_slots_thumbnail_path` in `abi/addon_abi.cpp`; `SlotNative` in `MediaViewer.Interop/Slots.cs` |
| Mac bridge and chrome | `mv_chrome_slots_*` in `src/shell/slots_mac.mm`; slot add-ons loaded in `addons_mac.mm` (`MvSlotAddonsStart`); `SlotStore.swift`, `ScreenView.swift`, `SlotItems.swift`, hooks in `MetadataView/Store`, `FileSearch`, `CommandBarView`, `GalleryView`, `AddonsView` |
| Windows chrome | `IslandHost.Slots.cs`, `IslandHost.Screens.cs`, hooks in `IslandHost.Panels.cs`, `.FileSearch.cs`, `.Gallery.cs`, `.Addons.cs` |
| Tests | `tests/test_addon_screens.cpp`, `tests/test_addon_slots.cpp` (in `mv_import_tests`, all three builds) over a real native test add-on, `tests/addons/slots_test` (`mv_slots_test_addon`), loaded through a signed store; `tests/test_meta_location.cpp` in `mv_tests` |

How it works:

- **Manifest (schema 1).** `contributes.api {min, max}` names the contribution API range; then
  `screens`, `info_sections`, `search_providers`, `bar_items`, `path_items`, `context_menu`,
  `status_items`, `tile_badges`, each an array of at most 16. A point outside the range is
  malformed; a range starting past this host (5) is "needs a newer MediaViewer". Naming any of
  them needs `host_api.min` 3, and an add-on that only fills slots may name no `chrome`. A
  stranger's schema 2 add-on naming one is `code_not_allowed` until PR 57.
- **One worker per add-on.** Every call into an add-on (`info_section`, `search`, `screen_open`,
  `badges`, `event`) runs there. A newer request of the same kind replaces the queued one; events
  queue in order. Generations are global: a post for a stale one is `MV_ERR_CANCELLED` and
  changes nothing, and `is_stale` lets a provider stop early. An idle add-on's worker sleeps on a
  condition variable.
- **Trees.** The core keeps each screen's tree, checks every tree and diff whole, and hands the
  chromes one canonical JSON. Every control, picture and surface must carry a `label` (its
  VoiceOver / Narrator name), or the tree is refused. Events from the chrome are checked against
  the tree (node exists, is interactive and enabled, value of the right kind and range) and the
  tree keeps the new value; the add-on answers with a diff. Text fields report on Return and on
  losing focus, sliders when let go: never an add-on call per key.
- **Info sections** are asked for when the Metadata pane shows a file, once per file and width,
  and never for a file without coordinates when the section says `"when": "location"`. Both hosts
  draw them at the very bottom of the pane opened with `I`, after all built-in metadata, on every
  tab, never collapsed (owner, 2026-10-12). A `surface` is BGRA the add-on drew at the pane's
  width × backing scale; the chrome draws the pin over it in the accent colour, with a drop-in
  that Reduce Motion turns off, and the surface's `marks` (up to 64 names, priority order, a
  name skipped where it would overlap one drawn or the pin) as native text.
- **A changed setting re-asks (2026-10-12).** When a write changes what an add-on reads from its
  settings page (`mv_addon_setting_set` / the Mac bridge's `mv_addons_setting_set` storing a
  different value, Remove without keep forgetting the file, an install by another publisher
  deleting kept values), `settings_values` tells its listeners with the add-on's id, after the
  write is on disk and outside the store's lock. Every slot registry listens
  (`slot_registry::settings_changed`): that add-on's info sections for the file on screen are
  asked again under a new generation of their own (the file's request keeps its generation; a
  late answer to the old one is dropped; what the section showed stays until the answer
  replaces it), and, if it has badges, its badge sets for the folder shown go and it is asked
  again. Other add-ons are not asked; nothing is asked when nothing is on screen; the same value
  written again asks nobody. Generic, not per add-on. The requests coalesce like any other, so a
  run of changes costs one answer. The chromes refresh through the listener they already have
  (a section whose tree revision moved is redrawn): no new work on the UI thread, and the write
  itself is unchanged. An add-on reads its values through `settings_get` on every request and
  event and caches none across a change; a picture cache keys on anything a setting changes in
  the pixels. Search results already shown are not re-run: the next query reads the new values.
- **Interactive surfaces (owner, 2026-10-12).** A surface with `"interactive": true` (or
  `{"min_scale", "max_scale"}`) takes the pointer: a mouse wheel or a trackpad pinch (on Windows a
  precision touchpad's pinch arrives as Ctrl + wheel) zooms about the pointer, a drag pans, a
  double-click is reported; a trackpad two-finger scroll is left to the pane. During a gesture
  the chrome scales / moves the current pixels itself, within the bounds, and when it settles
  sends one event (`zoom` {scale, x, y}, `pan` {dx, dy}, `double` {x, y}; surface coordinates,
  0..1); the add-on's next pixels replace the provisional transform. Nothing presents the canvas.
- **Search providers.** File search posts the typed text on every debounced change and returns;
  name hits show at once and a provider's rows join when they arrive, with its reason
  ("Portugal") or "Still reading tags — N of M" under the field. A query starting with a
  provider's prefix (`place:`) goes to that provider alone, prefix removed, unless a digit follows
  (Local search's `in:2024` stays a date term). In Local search a query with place terms goes to
  no provider; the AI pack asks the `place:` provider for each term as a request of its own
  (`slot_registry::provider_paths`, #441). `locations_in` walks the folder at background
  priority (it waits while the viewer presents) and caches coordinates by path + size + mtime, so
  a second query reads nothing. Local search with no folder open has `locations_of_index`
  instead (#398): the AI pack's index's coordinates, read from the index, never from the files
  ([17](17-local-ai-search.md) "Places over the index").
- **Badges** are asked once per folder the gallery shows, posted as sets, inverted once into a
  path → badges table; a tile is a dictionary lookup (the verify counts add-on calls).
- **Items.** A bar or path item with `opens` opens that screen with no add-on call; otherwise the
  add-on gets a `command` event with the paths its `target` names (the marks, else the current
  file; the folder). Context-menu entries are on gallery tiles, filmstrip tiles and the canvas
  (issue #402, 2026-10-15): whatever a gallery tile lists for a file, the filmstrip's tile for it
  and a right-click (Control-click on the Mac) on the canvas showing it list too, each invoked
  with the paths the gallery tile would give it (the marks when it is one of them). Nothing
  shows where no add-on contributes an entry, over the gallery or the Video Editor, or on an
  empty window. Mac: `SlotContextMenu` on `FilmstripView`'s cells, and `menuForEvent:` on the
  canvas view asking `MVChromeHost.canvasContextMenu` (`SlotCanvasMenu`). Windows:
  `AttachSlotContextMenu` on the filmstrip's tiles, and `WM_RBUTTONUP` on the canvas calling the
  chrome's optional `ShowCanvasMenu` (a `MenuFlyout` at the pointer). The canvas menu has no
  keyboard way in yet (a tile's is Shift+F10 / the menu key on Windows, VoiceOver's Show Menu on
  the Mac). Status items are text in the command bar.
- **Screens** open as a window, a docked pane or a sheet, each with Done and Esc. The docked pane
  is not an island in the viewer's layout ([12](12-decision-log.md)): it lies **over** the right
  side of the canvas, which does not reflow around it. On the Mac it is a child panel held to the
  right edge of the viewer window's content: it follows the viewer's moves, resizes and full
  screen (in and out), and moves to whichever viewer window becomes main. On Windows it is a
  separate window placed at the viewer's right edge when it opens and **not held after that**:
  moving, resizing or maximising the viewer leaves it where it was, and it is not owned by the
  viewer window (it does not minimise with it). Holding it needs the viewer's move/size events in
  the C# host (a WinEvent hook or the host's window procedure), not done yet. A thumbnail grid is
  virtualised (LazyVGrid / ItemsRepeater) and each tile asks the viewer's JPEG-512 cache as it
  appears, cancelled as it leaves.
- **Removal** (Settings → Add-ons lists every installed slot add-on with Remove). Places is
  listed there even when it is not installed, with Install: it is optional and preferred
  ([27](27-places.md)). The add-on is detached first, which removes every item and waits for a
  call it is in, then shut down and unloaded; off the UI thread on both hosts. No restart.
- **Local search** is listed as the local-scope provider while the AI pack is attached
  ([12](12-decision-log.md)).
- **Launch.** Slot add-ons load on the add-on queue / a worker after Import's and the AI pack's
  checks, never on the UI thread; nothing is asked of one until something is shown.
- **Every window (issue #402, 2026-10-15).** Each window is its own process
  ([18](18-import.md) "One host process"), and every one loads the slot add-ons once, the first
  and later ones alike: a later window shows the same info sections, providers, badges, items,
  status and entries, a window opened later gets them, and closing the first window takes
  nothing from the others. They only read; what stays with the window that hosts the add-ons is
  the store's writes (install, update, Remove; Settings in a later window says where). A later
  window loads them at launch on the add-on queue (Mac, utility QoS) or a worker (Windows,
  `mv_addon_load_guest`, which never sweeps the store), after its chrome. Both hosts find them by
  their signed manifests alone (`store::manifests`, `loads_for_slots`; on Windows
  `mv_addon_slot_addons_json`), hashing no file, so a window never hashes the AI pack to find
  Places; loading one still verifies its own files. A Remove in the first window takes the
  add-on from that window at once; another window keeps it until it closes (Windows defers a
  loaded file's removal to the next start, as for any add-on in use). A changed setting re-asks
  only in the window that wrote it; another reads the new value at its next request.

**Added for Sharepoint (2026-10-15, [28](28-sharepoint.md#host-pieces-this-added)):**

- A `text_field` may carry `"pick": "folder"`: the hosts add **Choose…** beside it, show the
  system folder dialog (`NSOpenPanel`; WinUI's `FolderPicker`, owned by the screen's window) and
  report the folder as the field's `change`. Typing a path still works; the add-on checks it.
- An add-on that fills slots may contribute **commands** (`contributes.commands`): the hosts list
  them with the other add-on rows, and running one is a `command` event to the add-on
  (`node_id` = the command id, value = the files its payload names), through
  `slot_registry::run_command` (`mv_slots_command`, ABI 0.29; the Mac host calls the registry).
  It has no chrome to send the command to.
- **Settings → Add-ons** gives such an add-on with a window screen an **Open …** button, as Import
  has; the window opens through `open_screen` with no file.

**Added for an AirDrop-like Sharepoint (#442, 2026-10-15)**, generic, drawn by both hosts
(`ScreenView.swift`, `IslandHost.Screens.cs`) and checked by the core (`"[screens]"`):

- **`tiles`** (interactive; `label` required): `items` of `{id, text, detail?, initials?,
  progress?, mark?}` (at most 256), `selection`, `size` (32–160 points) and `static`. Each item is a
  large round tile: `initials` in a circle in the accent colour, the name and caption under it, a
  ring round the circle while `progress` is 0–1, and a `"done"` / `"failed"` mark. A click, Space or
  Return on one is `activate` with its id; `select` takes one id. A `static` set is shown, not
  chosen: no focus, and its events are refused.
- **`stack` `"look": "card"`**: drawn on a rounded surface with a hairline and padding, as the
  Settings cards. In a vertical stack with `"align": "center"` the hosts centre text too.
- **`progress` `"look": "ring"`** with `size` (16–256): a ring that fills, its percentage in the
  middle; an unknown value is a still third (nothing ticks while nothing moves).
- **`text_field` `"look": "code"`** with `digits` (1–12): one large centred field that takes
  digits only, groups them as typed and submits by itself when the last one is typed (Return
  submits too), then clears for a retry. On Windows it is `FakeInput` with a larger face, as every
  field there.
- **`text` `"style": "display"`**: a large face for a short code read across a room.

For a script add-on (PR 57): the registry reaches code through `slot_client`; a script runtime
implements it on its own worker and calls the same `post_*` functions the v3 rows wrap.

PRs 58 and 59 are one branch: the registry carries both and the Metadata pane's sections are a
screen in a slot, so the two halves share every file.

## Implementation notes (PR 55, 2026-09-29)

Where it lives:

| Piece | Code |
|---|---|
| The strict package reader | `src/addon/package.*` |
| Manifest schema 2, ids, links, display text, key fingerprint | `src/addon/open_manifest.*` |
| Themes: tokens, parsing, the contrast floor, the JSON the chromes read | `src/addon/theme.*` |
| The store: inspect, install, list, remove, the publisher record | `src/addon/open_store.*`; the folder is `io::open_addons_dir()` (`src/io/paths*`), never created by a read |
| One JSON and one wording for both hosts | `src/addon/open_json.*` |
| C ABI 0.17 (Windows) and the chrome bridge (Mac) | `mv_open_addon_*` in `mediaviewer_addon.h` / `abi/addon_abi.cpp`; `mv_open_addons_*` in `mv_chrome_bridge.h` / `shell/addons_mac.mm` |
| A package handed to the app | `shell/open_request.*` (`open_kind::addon_package`), `shell/main.cpp`, `chrome_host::offer_addon` → `IslandHost.OfferAddon`; `main_mac.mm -openEntryPath:` (launch) and `-openEntryPaths:failed:` (a drop, Open, Finder) → `MVChromeHost.offerAddonPackage` |
| Mac chrome | `Theme.swift` (`MVTheme`, `ThemeStore`, `ThemedRoot`), `OpenAddons.swift` (store, download, section, sheet), the Theme row in `SettingsView.swift` and its picker sheet in `ThemePicker.swift` |
| Windows chrome | `IslandHost.ThemePack.cs`, `IslandHost.ThemePicker.cs` (the Theme row and its picker sheet), `IslandHost.OpenAddons.cs`, the hook in `IslandHost.Theme.cs` `RefreshTheme`, `AddonNative.Open*` in `MediaViewer.Interop/Addons.cs` |
| The author's tool, guide and example | `tools/addon-sdk/mvaddon.py` (MIT), `docs/ADDONS.md`, `examples/addons/film-tones` |
| Tests | `tests/test_open_addons.cpp` (in `mv_import_tests`, all three builds), `tests/test_open_request.cpp`, `tools/addon-sdk/test_mvaddon.py` (ctest `addon_sdk`, cross-checked by `tools/addon-verify --open`), the Mac rig `MV_ADDON_SELFTEST` |

Calls made while building it (none reverses a D-decision; logged in [12](12-decision-log.md)):

- **The sheet is a panel in Settings**, not a window of its own, and Settings scrolls to it.
- **Windows' panes take the canvas colour** under a theme (`PanelBg` = `canvas`): the token
  table is the Mac's too, and the Mac has one chrome background.
- **The theme's appearance is applied to the system's controls**: `NSApp.appearance` on the
  Mac, `RequestedTheme` on every island root on Windows, only while a one-palette theme is on.
- **On the Mac a theme change rebuilds the chrome's SwiftUI roots** (`ThemedRoot`, one `.id`),
  so no view watches the theme; Settings returns to the Theme row afterwards. On Windows the
  shared brushes are recoloured in place, as for a system appearance change.
- **The Theme row opens a sheet of cards** (issue #394), not a menu: Default, the built-ins,
  then add-on themes, each drawn in its own tokens (a bar, a pane, the accent, a six-colour
  strip). An add-on's tokens are read on a worker when the sheet opens, with a blank card until
  they arrive. A click, or Return / Space on the keyboard cursor, closes the sheet and applies
  the theme; Esc or Cancel keeps it. On the Mac the theme is applied after the sheet is
  dismissed, because a theme change rebuilds the chrome.
- **A theme's typeface waits for the next start on Windows** ([§8](#8-themes-api-1)).
- **The Mac reads `mv.theme.*` from the defaults; Windows reads `theme.json`** beside
  `settings.ini`, once, when the chrome starts.
- **Check for update refuses a link that serves another add-on**, and says "is the newest
  version" when the link serves what is installed.
- **A version replaced by an update is deleted at once**: a data add-on holds nothing open.
  (Our own add-ons wait for the next start, because theirs may be running.)
- **`disabled` is parsed and checked but not read on Windows yet**; nothing there draws
  disabled text in a colour of its own.
- **The first-party add-ons' windows do not follow a theme yet** (Import's and Local search's
  chromes carry their own copies of the default colours); PR 56.

Measured on the Mac (arm64, Release, 2026-09-29, the owner's machine while in use). Base is
`b9c65c3` built in its own worktree and build directory; "theme" is the new build with the
example add-on installed and its Dusk theme on. Runs alternated base, new, theme; one untimed
warm-up open per file first; `mediaviewer_lab --soak 6 --static`:

| File | Build | First pixel, s (3 runs) | Full resolution, s (3 runs) | Dropped | Idle presents |
|---|---|---|---|---|---|
| `sony_ilce7rm3.arw` | base | 0.032 · 0.020 · 0.021 | 0.478 · 0.467 · 0.467 | 0 | 0 |
| | new | 0.022 · 0.020 · 0.019 | 0.468 · 0.465 · 0.480 | 0 | 0 |
| | theme | 0.025 · 0.029 · 0.018 | 0.488 · 0.475 · 0.464 | 0 | 0 |
| `canon_eosr6.cr3` | base | 0.016 · 0.024 · 0.033 | 0.247 · 0.254 · 0.263 | 0 | 0 |
| | new | 0.017 · 0.021 · 0.017 | 0.248 · 0.250 · 0.248 | 0 | 0 |
| | theme | 0.017 · 0.028 · 0.033 | 0.240 · 0.257 · 0.263 | 0 | 0 |

The ranges overlap in every row: no change that these runs can see. Mac PR 1's gate
(`frametime --seconds 60`), one run each:

| Build | Animated: frames · dropped · p99 ms · max ms | Idle: presents · CPU of one core |
|---|---|---|
| base | 3599 · 0 · 16.85 · 16.94 | 0 · **104 %** |
| new | 3599 · 0 · 16.85 · 16.91 | 0 · **103 %** |
| theme | 3599 · 0 · 16.85 · 16.96 | 0 · **102 %** |

Pacing holds and is the same on all three. **The idle clause fails on all three, the base
included**, so the gate as a whole did not pass on this machine on this day, with or without
this change. The cause was not found here (the lab loads the installed first-party add-ons,
and the machine was in use); it is filed as its own task. Until the base passes, "both
present-loop gates hold" is not shown for this PR.

Measured on Windows (Windows 11, x64, MSVC Release, 2026-10-03, the owner's machine while in
use). Base is `b9c65c3` built in its own worktree and build directory; "theme" is the new build
with the example add-on installed and Dusk on. Runs alternated; one untimed warm-up open per file
first; `mediaviewer_lab --soak 6 --static`:

| File | Build | First pixel, s | Full resolution, s | Dropped |
|---|---|---|---|---|
| `sony_ilce7rm3.arw` | base | 0.038 · 0.060 · 0.051 · 0.041 · 0.048 · 0.050 | 1.261 · 1.458 · 1.633 · 1.249 · 1.319 · 1.346 | 0 |
| | new | 0.048 · 0.047 · 0.055 | 1.385 · 1.235 · 1.580 | 0 |
| | theme | 0.049 · 0.046 · 0.044 | 1.146 · 1.165 · 1.216 | 0 |
| `canon_eosr6.cr3` | base (18 runs) | 0.038 – 0.072 | 0.607 – 0.804 | 0 |
| | new | 0.052 · 0.053 · 0.044 | 0.759 · 0.762 · 0.744 | 0 |
| | theme (15 runs) | 0.039 – 0.077, and one 0.827 | 0.629 – 0.692, and one 2.068 | 0 |

The ranges overlap in every row. One themed CR3 run in 15 was slow (0.83 s / 2.07 s); 6 further
alternated pairs did not repeat it. PR 1's gate (`frametime --seconds 60`), alternated:

| Run | Build | Animated: frames · dropped · p99 ms · max ms | Idle: presents · CPU of one core |
|---|---|---|---|
| 1 | base | 3597 · 0 · 16.95 · 19.16 | 0 · **4.7 %** |
| 2 | new | 3597 · 0 · 16.90 · 17.14 | 0 · **4.7 %** |
| 3 | base | 3442 · 13 · 50.15 · 80.81 | 0 · **4.8 %** |
| 4 | new | 3588 · 3 · 17.05 · 33.38 | 0 · **4.6 %** |
| 5 | new | 3597 · 0 · 16.95 · 17.22 | 0 · **4.8 %** |
| 6 | base | 3597 · 0 · 16.95 · 17.27 | 0 · **4.6 %** |
| 7 | theme | 3588 · 1 · 16.95 · 83.41 | 0 · **5.1 %** |
| 8 | base | 3593 · 2 · 16.90 · 50.07 | 4 · **4.9 %** |

Pacing is the same on all builds; the drops land on base and new alike, from the machine being in
use. **The idle clause (≤ 1 %) fails on every run, the base included**, as on the Mac, so the
Windows gate as a whole is not shown either. Not caused here, as far as alternated runs can tell.

PR 55's verify line, where each part stands:

| Verify | State |
|---|---|
| A package from the SDK with a fresh key installs from a file, shows publisher and fingerprint, themes the chrome; Default restores it exactly | **Mac: run in the app** (`MV_ADDON_SELFTEST`): sheet, install, Dusk (`home_rgb` 1c1b1a), Paper, Default (back to 1e1e1e, the value before), by log and screenshot. **Windows: run in the app** (2026-10-03, by hand): sheet with publisher and fingerprint, Cancel focused; Install by mouse and by keyboard alone; Dusk (`1c1b1a`), Paper (`22201d`, its dark palette), Default (back to `21232a`, the value before, and `theme.json` deleted), read from the screen |
| …and from an `https` link | **Not run on either host.** The download code is written and compiled; no server was stood up. Owed. Windows: an `http://` link is refused before any connection ("A link to an add-on starts with https://.") |
| Each refusal, with its reason | Tested in C++ under ASan / UBSan and from the SDK against the C++ reader: changed byte (file, manifest, signature), extra entry, missing file or signature, compressed, encrypted, data descriptor, extra field, ZIP64, comment, gap, trailing bytes, truncation at every length, unsafe paths, case-folded duplicates, code keys, another publisher, older version, contrast floor, package swapped after inspection, every single-byte change |
| An installed add-on changed on disk is not used | Tested; and run in the Mac app (the chrome fell back to Default and Settings said why). Windows: run in the app (one colour digit of the installed `dusk.json` changed: Default at next start, Settings said why, the add-on "did not pass verification"). Also run there: a package changed after its sheet was shown, and the same id signed by another key ("already installed from a different maker"), both refused; a second plain launch hands the package to the running viewer |
| The link request carries no cookie, query or identifier of ours | By construction (ephemeral session, cookies off, fixed User-Agent; `HttpClient` with cookies and auto-redirect off); **not captured on the wire** |
| Launch → first pixel and → full resolution within noise, add-on installed, theme on | **Measured on both**, above |
| 0 presents idle | Mac: 0 on all runs. Windows: 0 on every run but one base run (4) |
| Both present-loop gates | **Not shown on either**: pacing holds, the idle CPU clause fails on base and new alike |
| With none installed: no folder created, nothing of an add-on read at start | Tested (the store creates nothing on list, find or inspect); on the Mac the profile was checked after every run; on Windows no `open-addons` folder or `theme.json` after ~20 launches |

Owed before this merges:

- **Windows, seen in its first run** (2026-10-03; it builds with no warnings and ctest passes
  all 897): the Theme picker reads "Not installed" when the chosen add-on is installed but failed
  verification (the line beside it is right); a refused link clears the field, losing what was
  typed. Under a theme the system controls keep the system accent on Windows (by design, §8).
- **Install from a link and Check for update, end to end**, on both hosts, including a redirect
  to `http` and a body over 64 MB.
- **VoiceOver and Narrator** on the sheet; keyboard-only install on the Mac (done on Windows:
  Tab to Install, Enter).
- **libFuzzer harnesses** for the package reader, the manifest and the theme parser, beside the
  decoders' (`tools/fuzz` is a Windows clang-cl build).
- **Opening a `.mvaddon` by double-click** is registered with both OSes (2026-10-03, owner call
  3) but not yet seen working: neither installer nor app bundle was rebuilt and installed here.
  Drop, Open With and the command line were.
- **Light appearance, increased contrast and a two-palette theme following a system change**
  were not looked at by eye; the machine was in Dark Mode throughout.
- **Intel Macs:** built for arm64 only here.

Raised 2026-10-03, for later: **Import's window on the screen vocabulary.** Not now: screens are
PR 58, after code (PR 57), and §1 keeps Import on its own path, so moving its
window reverses that and needs a row in [12](12-decision-log.md). If taken, Import's engine (copy,
BLAKE3 skip, verify, resume) stays native and only its window, today written twice
(`MediaViewer.Import.Chrome.dll`, `Import.bundle`), becomes one view tree both hosts render, which
would make it PR 58's first real screen. Possible now with no owner call: PR 56's "first-party
chromes read the host's tokens", so Import's and Local search's windows follow the theme.

## Implementation notes (PR 57, 2026-10-11)

Other makers' code runs as Lua 5.4 in a sandbox per add-on ([§9](#9-code-how-a-strangers-add-on-runs) A).
The reasons for each call below are in [12](12-decision-log.md) ("Other makers' code runs as Lua 5.4").

| Piece | Code |
|---|---|
| Lua 5.4.8, vendored, static, MIT, without `io` / `os` / `package` / `debug` and the tools; marked changes for the watchdog, and no finalizers or weak tables | `third_party/lua` (`VENDOR.txt`), `cmake/script.cmake` (`mv_lua`, built as C++) |
| The sandbox, its limits, the watchdog, the `mv` bindings, Lua ↔ JSON | `src/addon/script_runtime.*`, `src/addon/script_thread_{posix,win}.cpp` (a worker with a 4 MB stack) |
| The host's set of script add-ons: load from the store (bytes re-hashed as they are read), commands, status, Start again, the spin rig | `src/addon/script_host.*`; `open_store::verified_file` |
| Manifest: `scripts`, `permissions`, `contributes.commands` at API 3; the sheet's lines | `src/addon/open_manifest.*`, `src/addon/open_json.cpp` |
| Key clashes give way to the base app; notes for Settings | `shell/command_table.cpp` (`addon_command_row.script`, `addon_command_notes_json`) |
| Windows | ABI 0.26 `mv_script_*` (`abi/script_abi.cpp`); `shell/main.cpp` (start after the chrome, run rows, `kMsgScripts` notices); `IslandHost.OpenAddons.cs`, `AddonNative.ScriptStatus` / `ScriptRestart` |
| Mac | `shell/addons_mac.mm` (the host, the folder model and item on screen as services, rows), `main_mac.mm` (`MvAppShowNotice`, the `MV_SCRIPT_SELFTEST` rig); `OpenAddons.swift` (`ScriptStatus`, Start again), bridge `mv_open_addons_scripts` / `_restart` / `_reload` |
| SDK, guide, example | `tools/addon-sdk/mvaddon.py` (`init --script`, script rules, compiled Lua refused at pack), `docs/ADDONS.md` "Scripts", `examples/addons/folder-count` |
| Tests | `tests/test_script_runtime.cpp`, `tests/test_script_addons.cpp` (`mv_script_tests`, also in cmake/portable), `tests/test_key_router.cpp` (key clash), `tools/addon-sdk/test_mvaddon.py` (cross-checked by `mv_addon_verify --open`), `tools/fuzz/fuzz_script.cpp` |

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

- **`scripts`**: 1–16 listed `.lua` files, ≤ 1 MB each, ≤ 4 MB together, run in order in one
  environment; loaded as text only (`mode "t"`). Under a range ending before 3: malformed; on a
  host before 3: `code_not_allowed`.
- **`permissions`**: each of `folder.read`, `thumbnails`, `selection` at most once, only with
  scripts. `files.write` / `metadata.write` (PR 60): `needs_update`. `network`: malformed, always.
- **`commands`**: 1–32 (eight slots in the table); `id` `[a-z0-9-]{1,32}`, `name` display text,
  `key` a printable label (`Ctrl` is `Cmd` on the Mac), `run` a Lua name, `modes`
  `viewing|video|browse|all`, `payload` `none|marks|marked_or_current|screen` (anything but
  `none` needs `selection`). Only with scripts.
- **Unchanged:** `native`, `chrome`, `main` refuse the add-on whatever its range, and every PR 55
  refusal holds for a package with code (tested: a changed script byte, an unlisted `.lua`,
  compression, `..`).

### The script API

Only this exists; a part whose permission was not granted is **absent** (`mv.folder == nil`):

| Name | Permission | |
|---|---|---|
| `mv.id`, `mv.api`, `mv.permissions` | — | the add-on's id, 3, the granted set |
| `mv.notify(text)` | — | one line in the host's notice (Windows: the title's; Mac: the command bar's), prefixed with the add-on's name; ≤ 200 bytes, ≤ 4 a second |
| `mv.log(...)`, `print` | — | stays local: the macOS unified log (private); not kept on Windows yet |
| `mv.on(event, fn)` | — | handlers for later points' events (`script_call::call_handlers`); none sent yet |
| `mv.folder.count()`, `.name(i)`, `.path()` | `folder.read` | the viewer's listing: Mac `folder_model`, Windows the session's `mv_folder_*` |
| `mv.selection.current()` | `selection` | the file on screen |

Standard library: `string` without `dump` (its metatable locked: `getmetatable("")` is `false`),
`table`, `math`, `utf8`, `coroutine`, the base functions without `dofile` / `loadfile` /
`require` / `warn`; `load` text-only into the sandbox; `collectgarbage` with `count`, `collect`,
`step`, `isrunning` only. A `__gc` metamethod never runs and `__mode` has no effect (every
table is strong): both could hold a worker where no limit reaches ([12](12-decision-log.md)).
A command's function is called with its payload as a Lua value.
`thumbnails` puts nothing in the environment: it gates PR 58's thumbnail nodes.

**For later points** (agreed with PRs 58–59): `add_script_binding({name, api, permission,
install})` installs more `mv.*` tables per state; `script_addon::post(script_task)` runs work on
the add-on's worker (dropped unrun when its generation is stale); `script_call::call_handlers`,
`with_state`, `lua_to_json` / `push_json` carry screens, diffs, search answers and badges.
`script_host::find(id)` hands the add-on to a slot registry; its stop callback is where items
are detached.

### Limits (as shipped)

| Limit | Value | Enforced by |
|---|---|---|
| Memory | 64 MB per add-on | the `lua_Alloc`: refused growth stops the add-on, and stays refused |
| Instructions | 100 M per task | a count hook every 10,000 instructions |
| Wall clock | 1 s per task | the same hook, and one watchdog thread (asleep unless a task runs) for the loops that run no VM instruction (pattern matching, a plain `string.find`, `table.move`, `table.insert`, `table.remove`, `table.sort`) |
| C stack | 4 MB worker | Lua's own 200-level C limit inside it |
| Queue | 256 tasks | `post` returns false past it |

A task is a command call, a handler call, or the scripts' first run. Once a limit is hit, every
instruction raises, so `pcall` cannot keep the add-on alive; its state is closed on its worker.
Settings → Add-ons → From others shows "Stopped." with the limit in words ("One of its tasks
(command spin) ran for more than 100 million steps.") and **Start again**. A Lua error is not a
stop: Settings shows the last error.

### Verify, where each part stands

| Verify (plan §14) | State |
|---|---|
| A script command runs from its key | **Mac: run in the app.** `MV_SCRIPT_SELFTEST` posts ⌘⇧K through the app's own event queue: the router, the live table and the runtime ran, and the bar showed "Folder Count: 4 files in this folder" (`tools/testmedia/raw`). **Windows: owed** (compiled by CI; the path is `run_command` → `mv_script_run_command` → `notice_show`) |
| A runaway loop and an allocation bomb each stop the add-on, not the viewer, within the watchdog's bound | **Tested on the Mac** (`mv_script_tests`, both hosts' shared code): `while true`, a `pcall` loop, a coroutine loop, a tail-call loop, five allocation bombs and two C-side loops each stop within bound + 400 ms (300 ms test bound); with the shipped limits a spin stops after ~0.75 s. **In the Mac app:** ⌘⇧J on the spinner stopped it ("ran for more than 100 million steps") and the viewer kept answering; Settings showed it. **Windows: owed** (same code) |
| Without `folder.read` the folder API is absent | **Tested** (`mv.folder`, `mv.selection`, `mv.thumbnails` are `nil` without their permission; present with it) |
| Both present-loop gates hold while a script spins | **Mac:** **Not shown, as on the base: pacing holds alike with and without a script spinning, and
the idle clause fails on both.** Measured below; the machine was loaded throughout (other
builds; load average 18–50). **Windows: owed** (`MV_SCRIPT_SPIN` works there too; CI's self-hosted gate does not set it) |
| Launch → first pixel within noise with a script add-on installed | **Mac:** **Measured; the ranges overlap in every row** (below). The machine was loaded, so these
show no change that runs on it can see, and nothing finer. **Windows: owed** |

Measured on the Mac (arm64, Release, 2026-10-12, the owner's machine with other agents'
builds running: load average 18–50). Base is `8ef8679` (this branch's parent) built from its own
source copy and build directory. "spin" is this build with two script add-ons installed in a
scratch store (`MV_SCRIPT_STORE`) and `MV_SCRIPT_SPIN=rig.spinner/burn` keeping one add-on's
worker busy throughout (a 40 M-instruction loop posted back to back; checked on its own: in a
20 s static soak the lab process sat at 97–100 % of one core with the rig and ~0.5 % without, by
`ps` every 2 s). Mac PR 1 gate, `frametime --seconds 60`,
alternated:

| Run | Build | Animated: frames · p99 ms · max ms | Idle: presents · CPU of one core | Notes |
|---|---|---|---|---|
| 1 | base | 3600 · 16.80 · 22.31 | 0 · 92.5 % | |
| 2 | spin | 3598 · 16.85 · 42.30 | 0 · 200.2 % | void as a spin run: the rig's loop crossed its own 100 M budget, so the add-on stopped in its first second of each phase (the stop working as designed); shortened to 40 M after |
| 3 | base | 3598 · 16.90 · 49.98 | 0 · 100.1 % | |
| 4 | spin | 3600 · 16.95 · 19.12 | 2 · 47.9 % | void as a spin run, as 2 |
| 5 | base | 3600 · 16.95 · 22.31 | 19 · 37.1 % | 19 input events: the machine was in use; another agent's lab soak overlapped |
| 6 | spin | 3599 · 16.95 · 28.01 | 0 · 186.5 % | spinning throughout |
| 7 | base | 3553 · 17.10 · 662.7 | 0 · 104.9 % | |
| 8 | spin | 3598 · 16.90 · 22.06 | 0 · 100.1 % | spinning throughout |
| 9 | base | 406 · 23.05 · 46,715 | 0 · 52.4 % | display link stalled 47 s: void |
| 10 | spin | 3599 · 16.95 · 33.28 | 0 · 85.5 % | spinning throughout |

Pacing with a script spinning is within the base's own spread (p99 16.85–16.95 ms against
16.80–17.10 ms; the drops and long frames land on base runs as much as on spin runs). The idle
CPU clause (≤ 1 %) fails on every base run, as it did in PR 55's measurements, so the gate as a
whole is not shown on this machine; with a script spinning the lab's idle CPU is one core higher,
which is the script's own work on its own thread (no present waited on it), and 0 presents idle
hold on the spin runs. Launch → first pixel and → full resolution, `mediaviewer_lab --soak 6
--static`, one untimed warm-up open per file, then base / new (no script add-on, an empty
scratch store) / script (the two add-ons installed, idle) alternated, two sessions (3 and 5
rounds):

| File | Build | First pixel, s | Full resolution, s |
|---|---|---|---|
| `sony_ilce7rm3.arw` | base | 0.021 · 0.055 · 0.118 · 0.124 · 0.074 · 0.083 · 0.121 · 0.084 | 0.519 · 1.231 · 1.264 · 3.357 · 1.739 · 2.631 · 2.171 · 2.175 |
| | new | 0.050 · 0.049 · 0.049 · 0.137 · 0.069 · 0.075 · 0.097 · 0.079 | 1.285 · 1.231 · 1.246 · 4.239 · 2.050 · 2.065 · 1.970 · 2.000 |
| | script | 0.047 · 0.047 · 0.051 · 0.119 · 0.072 · 0.080 · 0.062 · 0.075 | 1.176 · 1.050 · 1.337 · 1.969 · 2.171 · 2.083 · 1.593 · 1.977 |
| `canon_eosr6.cr3` | base | 0.051 · 0.048 · 0.043 · 0.114 · 0.094 · 0.133 · 0.079 · 0.071 | 0.685 · 0.694 · 0.564 · 0.977 · 2.353 · 3.218 · 1.658 · 1.462 |
| | new | 0.048 · 0.045 · 0.043 · 0.069 · 0.097 · 0.126 · 0.081 · 0.065 | 0.774 · 0.784 · 0.653 · 1.178 · 2.715 · 3.471 · 1.905 · 1.877 |
| | script | 0.047 · 0.048 · 0.043 · 0.116 · 0.079 · 0.059 · 0.068 · 0.067 | 0.822 · 0.764 · 0.656 · 2.214 · 4.234 · 1.287 · 1.118 · 1.601 |

The ranges overlap in every row. On a quiet machine the full-resolution numbers are a third of
these (PR 55's table above); a re-run there is owed, and is the run to quote. Nothing of a
script add-on runs before first pixel by construction: the scripts are listed and started on a
utility-QoS queue after the chrome is up (Windows: a background job after the chrome attaches).

**Fuzzing.** `tools/fuzz/fuzz_script.cpp` (arbitrary text as a script, every permission, 8 MB /
2 M instructions / 250 ms, then its `f`): 563,234 inputs, about 15 minutes of fuzzing on the Mac
under ASan + UBSan with Homebrew's libFuzzer (the run's 1,587 s span includes the machine
sleeping; the two "slow units" it saved run in 9 ms), 10,470 coverage edges; no crash, no
sanitizer report, no hang. In CI it joins the non-gating harnesses until it has been seen to
start on the runner. `mv_script_tests` also passes under ASan + UBSan (the runtime, Lua and the
tests instrumented).

Owed before this merges: the Windows key walk, the Windows gate with a script spinning, the
Windows launch timings; VoiceOver / Narrator on the new Settings lines; a `files.write`-era
review of the permission lines. (`mv.settings.get` landed with PR 56's store, 2026-10-12.)
