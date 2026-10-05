# Making an add-on for MediaViewer

Anyone can make an add-on, and anyone can install one from a file or a link. This page is for
the person making one. The design and its reasons are in
[docs/design/25-open-addons.md](design/25-open-addons.md).

**What an add-on can hold today: themes.** Colours and a typeface for MediaViewer's bars, panes
and text. Settings pages, keymaps, commands and whole screens are planned
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
| `api` | The contribution API you wrote for. `1` is themes |

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
