# Making an add-on for MediaViewer

Anyone can make an add-on, and anyone can install one from a file or a link. This page is for
the person making one. The design and its reasons are in
[docs/design/25-open-addons.md](design/25-open-addons.md).

**What an add-on can hold today: themes and scripts.** A theme gives colours and a typeface for
MediaViewer's bars, panes and text. A script is Lua 5.4 that MediaViewer runs in a sandbox, with
commands people run from the keyboard ([Scripts](#scripts)). Settings pages, keymaps, whole
screens and chrome slots are planned
([docs/design/25](design/25-open-addons.md#7-the-contribution-model)). No add-on ever gets network
access, and native code (a DLL or a dylib) is never loaded from an add-on.

## In five minutes

You need Python 3.8 or later. Nothing else: the tool is one file with no dependencies.

```bash
python3 tools/addon-sdk/mvaddon.py keygen --out publisher.key
```

```bash
python3 tools/addon-sdk/mvaddon.py init my-addon --id you.my-addon --name "My Add-on" --publisher "You"
```

```bash
python3 tools/addon-sdk/mvaddon.py pack my-addon --key publisher.key --out dist
```

`dist/you.my-addon-1.0.0.mvaddon` is your add-on. For a script add-on instead of a theme, add
`--script` to `init`: it starts a command that says how many files the open folder holds. Drop it on MediaViewer, or use
**Settings → Add-ons → From others → Install from file…**. To let people install it from a link,
put the file on any `https://` address and give them the address.

`tools/addon-sdk/mvaddon.py` is MIT-licensed, so using it puts no licence on your add-on. Working
examples: a theme in [examples/addons/film-tones](../examples/addons/film-tones), a script in
[examples/addons/folder-count](../examples/addons/folder-count).

**Your add-on is yours.** MediaViewer is GPL, but an add-on that reaches it only through this
format and its documented interfaces is not a work based on MediaViewer: you may publish it under
any licence, or none, and need not publish its source
([LICENSE-ADDONS.md](../LICENSE-ADDONS.md)). The manifest asks for an SPDX licence so the person
installing can see what you chose.

## Your key

`keygen` makes an Ed25519 key. It is who you are to the people who install your add-on:

- MediaViewer remembers the key an add-on was first installed with, and **only that key can
  update it**. Nobody can replace your add-on with theirs by copying its id.
- People see its **fingerprint** (`3f9a-02c1-77de-b410`) when they install. Publish the
  fingerprint where you publish the add-on, so a careful person can compare.
- **Keep `publisher.key` private and keep a copy.** Do not commit it. If you lose it, your users
  must remove your add-on and install it again under your new key.

The key does not prove your name. MediaViewer runs no registry and checks no identity; it tells
the person installing exactly that.

## The folder

```
my-addon/
  addon.json          what you write
  themes/
    example.json      a theme
```

`pack` takes every file in the folder except `addon.json` and hidden files. Links are refused.

### addon.json

```json
{
  "id": "you.my-addon",
  "name": "My Add-on",
  "version": "1.0.0",
  "description": "One line people read before they install.",
  "licence": "MIT",
  "publisher": { "name": "You", "url": "https://you.example" },
  "update_url": "https://you.example/my-addon.mvaddon",
  "api": { "min": 1, "max": 1 },
  "contributes": {
    "themes": [ { "id": "example", "name": "My Theme", "path": "themes/example.json" } ]
  }
}
```

| Field | Rule |
|---|---|
| `id` | `publisher.name`: lowercase letters, digits and hyphens, at least one dot, 64 characters at most. It never changes. `mediaviewer.` is reserved |
| `version` | `x.y.z`, numbers only. MediaViewer installs a newer version over an older one, never the reverse |
| `name`, `publisher.name`, `description` | Plain text. Control characters, right-to-left overrides and zero-width characters are refused |
| `licence` | An SPDX identifier. A file with a different licence goes in `"licences": { "path": "SPDX" }` |
| `publisher.url`, `update_url` | `https://` only, or empty |
| `api` | The contribution API you wrote for. `1` is themes, `3` adds scripts. A theme-only add-on keeps `1` so older MediaViewers install it |
| `scripts`, `permissions` | A script add-on's, below ([Scripts](#scripts)) |

`pack` adds the rest: the schema, your public key, and each file's size and SHA-256.

**`update_url`** is where the newest package always is. MediaViewer asks it only when the person
clicks **Check for update**. It never checks in the background, so you cannot count launches,
and should not expect to.

## A theme

```json
{
  "schema": 1,
  "dark": {
    "canvas": "#1c1b1a", "surface": "#262523",
    "title": "#f2efe9", "body": "#b9b4aa", "disabled": "#6f6b64",
    "hairline": "#3a3835", "accent": "#e0793a"
  },
  "light": {
    "canvas": "#f4f1ea", "surface": "#ffffff",
    "title": "#1c1b1a", "body": "#57534c", "disabled": "#9a958c",
    "hairline": "#d6d1c6", "accent": "#b5542d"
  },
  "font": "Avenir Next"
}
```

| Token | What it colours |
|---|---|
| `canvas` | The background of the bars, panes and Settings, and the viewer's own background when it is set to System |
| `surface` | Rows, fields and cards on it |
| `title` | Primary text |
| `body` | Secondary text |
| `disabled` | Text of a control that is off |
| `hairline` | Separators and outlines |
| `accent` | Selection, progress, the focused control |

- Colours are `#rrggbb` or `#rrggbbaa`. `canvas` and `surface` must be opaque.
- **Every palette needs all seven.**
- **Give `dark`, `light`, or both.** With both, MediaViewer follows the system. With one, it
  keeps that appearance while your theme is on.
- **Text must be readable.** MediaViewer refuses a palette where `title` has less than 4.5 : 1
  contrast, or `body` or `accent` less than 3 : 1, against `canvas` or `surface`. `pack` and
  `check` print the ratios.
- `font` is the name of a family installed on the person's computer. If it is not there,
  MediaViewer keeps its own. An add-on cannot ship a font file yet. On Windows a new typeface
  shows the next time MediaViewer starts.
- A theme never changes a photo or a video. It does not reach the histogram's channel colours
  or the overlay a developer opens with F3.
- When the system's high-contrast setting is on, the system's colours are used and your theme
  waits.

## Scripts

A script add-on carries Lua 5.4 source and the commands it adds. MediaViewer runs it on a thread
of its own, in a sandbox, never on the thread that draws the window: a slow or broken script
cannot freeze the viewer.

```
folder-count/
  addon.json
  main.lua
```

```json
{
  "id": "you.folder-count",
  "name": "Folder Count",
  "version": "1.0.0",
  "licence": "MIT",
  "publisher": { "name": "You", "url": "" },
  "api": { "min": 3, "max": 3 },
  "scripts": ["main.lua"],
  "permissions": ["folder.read"],
  "contributes": {
    "commands": [
      { "id": "count-files", "name": "Count files", "key": "Ctrl+Shift+K", "run": "count_files" }
    ]
  }
}
```

```lua
function count_files()
  local n = mv.folder.count()
  mv.notify(n == 1 and "1 file in this folder" or (n .. " files in this folder"))
end
```

**How it runs.** When MediaViewer starts (after the window is up) or your add-on is installed, it
runs each file in `scripts`, in order, in one environment: define your functions there. Each time
a command's key is pressed, MediaViewer calls the global function its `run` names. One call at a
time, in order; calls never overlap.

| Field | Rule |
|---|---|
| `scripts` | `.lua` files of the folder, 16 at most, 1 MB each, 4 MB together. Source text only: compiled Lua is refused |
| `permissions` | Any of `folder.read`, `thumbnails`, `selection`. None is granted unless you list it, and each is a line the person reads before installing |
| `commands[].id` | Lowercase letters, digits and hyphens, unique in your add-on |
| `commands[].name` | What `?`, Settings and the person see |
| `commands[].key` | A key as MediaViewer writes it: `Ctrl+Shift+K`, `Alt+F5`. `Ctrl` is `Cmd` on a Mac. Optional |
| `commands[].run` | The name of a global function in your scripts |
| `commands[].modes` | `viewing` (default: photos, clips and the gallery), `video`, `browse` or `all` |
| `commands[].payload` | What the function is called with: `none` (default), `marks` (the marked files' paths), `marked_or_current`, `screen` (`{path, video}`). Anything but `none` needs `selection` |

**If your key is taken.** A key MediaViewer itself uses, or another add-on took first, is
dropped: the command is still listed, without a key, Settings says why, and the person can give it
one. MediaViewer has room for eight add-on commands at a time.

### What a script can call

Only what is listed here exists. The parts that need a permission are **absent** without it:
`mv.folder` is `nil` unless you asked for `folder.read`.

| Call | Permission | What it does |
|---|---|---|
| `mv.id`, `mv.api` | — | Your add-on's id; the contribution API of this MediaViewer (3) |
| `mv.permissions` | — | A table of what was granted: `mv.permissions["folder.read"] == true` |
| `mv.notify(text)` | — | Shows one short line to the person, with your add-on's name in front (200 bytes at most, four a second at most). Returns whether it was shown |
| `mv.log(...)`, `print(...)` | — | A line for you, the developer. It stays on this computer: in the macOS unified log (private), nowhere on Windows yet |
| `mv.on(event, fn)` | — | Answers an event a later MediaViewer sends (screens and search providers will). None is sent yet |
| `mv.folder.count()` | `folder.read` | How many files the open folder lists, as the viewer lists them |
| `mv.folder.name(i)` | `folder.read` | The name of the `i`th file (from 1), or `nil` |
| `mv.folder.path()` | `folder.read` | The open folder's path, or `nil` (a search result list has none) |
| `mv.selection.current()` | `selection` | The path of the file on screen, or `nil` |

`thumbnails` adds nothing yet: it is for the thumbnail parts of screens, which come next.

The Lua standard library is there in part: `string` (without `string.dump`), `table`, `math`,
`utf8`, `coroutine`, and the basics (`pairs`, `pcall`, `setmetatable`, `tostring` and the rest).
`load` takes text only. `collectgarbage` answers `"count"`, `"collect"`, `"step"` and
`"isrunning"`. A `__gc` metamethod never runs, and `__mode` does nothing: every table keeps
what it holds. There is no `io`, `os`, `package`, `require`, `dofile`, `loadfile` or `debug`, no
socket and no file call, and there never will be a network one.

### The limits

Every call your add-on handles (and your scripts' first run) gets:

| Limit | Value |
|---|---|
| Memory | 64 MB for everything your add-on holds |
| Steps | 100 million Lua instructions per call |
| Time | 1 second per call |

Go over one and **your add-on is stopped**: it does nothing more until the person clicks
**Start again** in Settings → Add-ons → From others, or MediaViewer starts again. Settings says
which limit it hit, for example "One of its tasks (command count-files) ran for more than 100
million steps". A Lua error is not a stop: the call ends, Settings shows the last error, and the
next key press calls you again. Catching errors with `pcall` does not get around a limit.

Scripts are for glue, not pixel loops: a loop over a 10,000-file folder is fine; resizing
images in Lua is not, and MediaViewer will offer that as a call later.

## Check before you publish

```bash
python3 tools/addon-sdk/mvaddon.py check dist/you.my-addon-1.0.0.mvaddon
```

It says what MediaViewer will say. The reasons it can give:

| Reason | What happened |
|---|---|
| `bad_package` | Not the ZIP MediaViewer reads. A package stores its files without compression, with no extra fields or comment. Make it with `pack`, not with a ZIP tool |
| `too_large` | Over 64 MB, or more than 2,048 files |
| `missing_signature`, `bad_signature` | Not signed, or changed after signing |
| `file_missing`, `file_mismatch`, `unexpected_file` | The files are not the ones the manifest lists |
| `unsafe_path` | A file name that leaves the folder (`..`, a drive, a backslash), or a theme whose `path` is not one of the files |
| `malformed` | A field missing, mistyped, or not allowed. With an `api.max` this MediaViewer reaches, a key it does not know is a mistake. A script under an `api.max` below 3, a permission named twice, a `network` permission, a command whose `run` is not a Lua name, or one that sends paths without `selection` |
| `code_not_allowed` | The manifest names `native`, `chrome` or `main` (native code is never loaded from an add-on), or it has `scripts` and this MediaViewer is older than API 3 |
| `needs_update` | `api.min` is newer than this MediaViewer, or the add-on asks for a permission only a newer one grants (`files.write`, `metadata.write`) |
| `invalid_theme` | A theme file that does not pass the rules above; the detail names the theme and the rule |

Two more come only from MediaViewer, because they depend on what is installed: `other_publisher`
(that id is installed under another key) and `downgrade` (a newer version is installed).

## What the person installing sees

```
Install “My Add-on” 1.0.0?
From      You · you.example
Key       3f9a-02c1-77de-b410
Adds      1 theme
Can       Change how MediaViewer's bars, panes and text look
Cannot    Run code, read your files, or use the network
Size      915 bytes · MIT

MediaViewer has not checked this add-on or who made it.
                                   [ Cancel ]  [ Install ]
```

The Adds, Can and Cannot lines are worked out by MediaViewer from your manifest. You cannot
write them. For the Folder Count example they read:

```
Adds      1 command
Can       Run its own script inside MediaViewer, stopped if it uses too much memory or time
Can       Add commands you can run from the keyboard
Can       See the names of the files in the open folder
Cannot    Change your files or use the network
```

## Testing the tool itself

```bash
python3 tools/addon-sdk/test_mvaddon.py
```

With `MV_ADDON_VERIFY` set to a built `mv_addon_verify`, the same tests also ask MediaViewer's
own reader about every package ([DEVELOPMENT.md](DEVELOPMENT.md#test)).
