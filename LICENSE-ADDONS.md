# Additional permission for add-ons

MediaViewer is licensed under the GNU General Public License, version 3 or (at your option)
any later version (`LICENSE`). Under section 7 of that licence, the copyright holder grants
the following additional permission.

## The permission

An **add-on** is a work that reaches MediaViewer only through its documented add-on
interfaces: the `.mvaddon` package format and its manifest, the contribution points an add-on
declares (themes, settings, keymaps, commands, screens, slots, search providers), and the
script host API, all as described in `docs/design/25-open-addons.md` and `docs/ADDONS.md`.

Making, distributing and running an add-on does **not** make it a work based on MediaViewer
for the purposes of the GNU GPL, however it is combined with MediaViewer at run time. You may
license an add-on under any terms you choose, or none, and nothing in MediaViewer's licence
requires you to publish its source.

## What it does not cover

- Code that links MediaViewer's own libraries or includes its source (as MediaViewer's own
  add-ons do, under `src/addons`) is a work based on MediaViewer and stays under the GPL.
- Changes to MediaViewer itself.
- The add-on tool and the examples (`tools/addon-sdk`, `examples/addons`), which are under the
  MIT licence and need no permission.

## Terms

This permission is granted by longtimeno-c, the copyright holder of MediaViewer, on 2026-10-03.
It applies to every version of MediaViewer that ships this file. It may be withdrawn for later
versions only by the copyright holder; an add-on already published under it keeps it. It adds
to, and does not take from, the rights the GNU GPL gives you.
