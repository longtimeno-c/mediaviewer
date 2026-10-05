# 25 — Open add-ons: anyone can make one, and what one can change

**Status: proposed 2026-09-29, from issue #79 and the owner's call the same day ("add-ons can be
made and installed by anyone, not just our repo… if people have a compatible file / URL they can
install their add-on… add-ons can do a wide range of things like add new screens"). Post-v1,
Windows and macOS together (D9). Milestone L, PRs 55–60. Not a D-decision. PR 55 is built
([Implementation notes](#implementation-notes-pr-55-2026-09-29)); the owner's calls on code, network,
the file type and licensing were made on 2026-10-03 ([§17](#17-decisions-owner-2026-10-03)).**

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
- every name a safe relative path (`/`-separated, no `..`, no drive, no control character), no
  two names equal when case is folded;
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
- **Code is refused by name.** `native`, `chrome`, `scripts`, `main` at the top level or under
  `contributes` refuse the add-on (`code_not_allowed`) until a PR that runs third-party code
  lands ([§9](#9-code-how-a-strangers-add-on-runs)). No API range turns a data add-on into one
  that runs.

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
| **Theme, shape** | 2 | 56 | Corner radius scale, density, type scale | Today's literals | Same table |
| **Settings page** | 2 | 56 | A schema: sections, toggles, choices, numbers, text, each with a key, default and help | — | Rendered by the host when Settings opens; values live in the host's store, namespaced by add-on id |
| **Keymap** | 2 | 56 | Bindings for existing commands (the v1.1 "named layouts": FastStone, IrfanView, vim) | `default_bindings()` | Applied to the live table once; the router stays an array index ([16](16-commands.md)) |
| **Command** | 3 | 57 | A name, a default key, and the script function it runs | — | Rows appended to the table at load; a key clash is resolved for the base app, the add-on's key is dropped and Settings says so |
| **Screen** | 4 | 58 | A view tree ([§10](#10-screens)) in a window, a pane or a sheet | — | Built by the host from data; updates are diffs posted from the add-on's worker |
| **Chrome slot** | 5 | 59 | Command-bar and path-bar items, context-menu entries, info-pane sections, status items, gallery tile badges | — | Badges come from a table filled off-thread; **no add-on call per tile**, ever |
| **Search provider** | 5 | 59 | Answers a query with a list of files | File-name search (base app, 2026-09-28) | Runs on the add-on's worker with the query's generation; a stale answer is dropped |
| **Export / files** | 6 | 60 | Writes new files into a folder the user chose; metadata through PR 12's writer | — | Jobs on the job system, generation-tagged |

Local search becomes a search provider in PR 59 instead of owning the path-bar button (#76); it
keeps its native pack and its own channel.

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
| `canvas` | The chrome's background; the canvas's "System" background follows it | `Canvas`, `PanelBg`, native home colour | `canvas`, host `home_background_rgb` |
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
- **Not themed:** photo and video pixels, the canvas's colour pipeline, the F3 overlay (ImGui,
  a lab tool), the histogram's channel colours (they carry meaning), the hover washes (they
  follow the appearance, not a token), and the first-party add-ons' own windows until they read
  the host's tokens (PR 56).
- **Choosing one:** Settings → Appearance → **Theme**: Default, then every theme of every
  installed add-on. Installing does not switch.
- **Start-up:** the chrome stores the resolved tokens of the theme in use in its own settings
  and paints with them at once. The add-on is verified on a worker afterwards; if it is gone,
  changed or refused, the chrome returns to Default and says why in Settings.

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
  drew, for what the vocabulary lacks).
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
| 57 | **Code** (owner call 1, made). The runtime, one worker per add-on, limits and watchdog, permissions in the consent sheet, commands | A script command runs from its key on both hosts; a runaway loop and an allocation bomb each stop the add-on, not the viewer, within the watchdog's bound; without `folder.read` the folder API is absent; **both gates hold while a script spins** |
| 58 | **Screens.** The view vocabulary, windows / panes / sheets, diffs, events | One add-on's screen renders natively on both hosts, keyboard-only, VoiceOver and Narrator name every control; a 2,000-tile thumbnail grid scrolls without a hitch |
| 59 | **Slots and search providers.** Bar items, context menus, info-pane sections, tile badges; file-name search and Local search as providers | Badges for a 10,000-file folder cost no add-on call per tile (counted); a slow provider never delays typing; removing the add-on removes every item |
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
| A package handed to the app | `shell/open_request.*` (`open_kind::addon_package`), `shell/main.cpp`, `chrome_host::offer_addon` → `IslandHost.OfferAddon`; `main_mac.mm -openEntryPath:` → `MVChromeHost.offerAddonPackage` |
| Mac chrome | `Theme.swift` (`MVTheme`, `ThemeStore`, `ThemedRoot`), `OpenAddons.swift` (store, download, section, sheet), the Theme row in `SettingsView.swift` |
| Windows chrome | `IslandHost.ThemePack.cs`, `IslandHost.OpenAddons.cs`, the hook in `IslandHost.Theme.cs` `RefreshTheme`, `AddonNative.Open*` in `MediaViewer.Interop/Addons.cs` |
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
