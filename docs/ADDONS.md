# Making an add-on for MediaViewer

Anyone can make an add-on, and anyone can install one from a file or a link. This page is for
the person making one. The design and its reasons are in
[docs/design/25-open-addons.md](design/25-open-addons.md).

**What an add-on can hold today:** themes (colours, a typeface, and how round, roomy and large
the chrome is), a settings page, and key layouts. Commands and whole screens are planned
([docs/design/25](design/25-open-addons.md#7-the-contribution-model)): code will run as a sandboxed
script with screens described as data, and no add-on will ever get network access. An add-on
that names code is refused until that runtime exists.

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

`dist/you.my-addon-1.0.0.mvaddon` is your add-on. Drop it on MediaViewer, or use
**Settings → Add-ons → From others → Install from file…**. To let people install it from a link,
put the file on any `https://` address and give them the address.

`tools/addon-sdk/mvaddon.py` is MIT-licensed, so using it puts no licence on your add-on. A
working example is in [examples/addons/film-tones](../examples/addons/film-tones).

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
| `api` | The contribution API you wrote for. `1` is themes; `2` adds a settings page, key layouts and a theme's shape. Use `"min": 2` if you use any of them; with `"max": 2` an older MediaViewer says your add-on needs a newer one instead of installing half of it |

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

### A theme's shape (API 2)

A theme file with `"schema": 2` may also say how the chrome is drawn (your manifest's `api.max` must then be at least 2). Each is a scale on
MediaViewer's own sizes, so `1` is what it looks like today:

```json
"shape": { "radius": 0.5, "density": 0.85, "type_scale": 0.95 }
```

| Key | Range | Scales |
|---|---|---|
| `radius` | 0 – 2 | Corner radii. `0` is square |
| `density` | 0.75 – 1.25 | Padding and the space between rows |
| `type_scale` | 0.85 – 1.25 | Text sizes |

Any of the three may be left out. The photo grid, the filmstrip, the video transport and the
editor's timeline keep their sizes. On Windows a new shape shows the next time MediaViewer starts.
`examples/addons/settings-demo/themes/compact.json` is one.

## A settings page (API 2)

Your add-on can have a page in Settings. You write what is on it; MediaViewer draws it with its
own controls on both Windows and the Mac, in the order you wrote:

```json
"contributes": {
  "settings": {
    "title": "Places",
    "sections": [
      { "title": "Place", "help": "How a photo's place is named.",
        "items": [
          { "type": "toggle", "key": "show_town", "label": "Name the town", "default": true },
          { "type": "number", "key": "town_radius_km", "label": "Town radius",
            "min": 1, "max": 50, "step": 1, "unit": "km", "default": 15,
            "help": "Name the nearest town within this distance.", "enabled_by": "show_town" },
          { "type": "choice", "key": "label_names", "label": "Place names",
            "options": [ { "value": "english", "label": "English" },
                         { "value": "local", "label": "Local" } ],
            "default": "english" },
          { "type": "text", "key": "home", "label": "Home town", "max_length": 64,
            "placeholder": "Lisbon", "default": "" } ] } ] } }
```

| `type` | Drawn as | Also takes |
|---|---|---|
| `toggle` | A switch | `default`: `true` or `false` |
| `choice` | A menu of the labels | `options`: 2 to 32 `{ "value", "label" }`; `default` is one of the values |
| `number` | A field with − / + (a stepper on the Mac) and the unit | `min`, `max`, `step` (default 1, at least 0.001), `unit` (16 bytes); `default` in range; every number to at most three decimals |
| `text` | A field | `max_length` (1 – 1024 bytes, default 256), `placeholder` |

- Every item has a `key` (`a`–`z` first, then lowercase letters, digits and `_`, 40 at most,
  unique on the page), a `label` and a `default`; `help` is optional.
- `enabled_by` names a toggle above the item: the item is greyed out while that toggle is off.
- Up to 16 sections and 64 items. A key MediaViewer does not know, or a default outside its own
  rule, refuses the add-on, and `pack` tells you which.
- What people choose is kept per add-on, survives a restart and an update, and is kept when your
  add-on is removed unless they untick **Keep its settings**. A value your new version's page no
  longer allows (a narrower range, a removed option) reads as your default.
- **Who reads the values:** an add-on that runs code (planned, PR 57) reads its own; today a data
  add-on's page is for the person and for MediaViewer's own add-ons, which read theirs natively.
  `examples/addons/settings-demo` has a page with every kind of item.

## Key layouts (API 2)

A key layout gives MediaViewer's own commands other keys: the FastStone, IrfanView or Vim habits
someone already has. The person picks it under Settings → Keyboard shortcuts → **Key layout**
and clicks **Apply**:

```json
"contributes": {
  "keymaps": [
    { "id": "vim", "name": "Vim",
      "bindings": [
        { "command": "prev", "keys": ["Left", "H"] },
        { "command": "next", "keys": ["Right", "L"] },
        { "command": "toggle_mark", "keys": ["Insert", "X"] } ] } ] }
```

- **`command`** is the command's name in MediaViewer's source: `next`, `prev`, `first`, `last`,
  `zoom_in`, `fit`, `toggle_mark`, `toggle_gallery`, `fullscreen`… (the `command_id` names in
  `src/shell/commands.h`; they never change).
- **`keys` go, in turn, to that command's rows** in MediaViewer's default table. Previous's rows
  are Left, A, Backspace…, so `["Left", "H"]` keeps Left and puts H where A was. `""` leaves a
  row without a key; rows after the last key keep theirs. A layout re-keys rows; it cannot add
  one. Settings lists the rows in order.
- Keys are written as Settings and `?` show them: `Ctrl+Shift+O`, `Page Down`, `F11`, `?`.
  `Ctrl` is `⌘` on a Mac.
- **A key your layout uses is taken from whatever held it** in the same modes (H no longer flips
  the photo in the example); Settings shows those commands as unbound. Two of your own bindings
  on one key: the first wins.
- Applying is once. After that the keys are the person's: they can change any of them, they stay
  if your add-on is removed, and **Reset to default** puts MediaViewer's defaults back.
- A command or key this MediaViewer does not know is skipped and listed when the layout is
  applied, so one layout can serve an older and a newer version.
- Up to 8 layouts of 256 commands each, up to 8 keys a command, and each layout at most 64 KB
  as compact JSON (`pack` and MediaViewer both check it; a real layout is a few KB).
  `examples/addons/vim-keys` is one.

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
| `malformed` | A field missing, mistyped, or not allowed. With `api.max` of 1, a key MediaViewer does not know is a mistake |
| `code_not_allowed` | The manifest names `native`, `chrome`, `scripts` or `main` |
| `needs_update` | `api.min` is newer than this MediaViewer |
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
write them.

## Testing the tool itself

```bash
python3 tools/addon-sdk/test_mvaddon.py
```

With `MV_ADDON_VERIFY` set to a built `mv_addon_verify`, the same tests also ask MediaViewer's
own reader about every package ([DEVELOPMENT.md](DEVELOPMENT.md#test)).
