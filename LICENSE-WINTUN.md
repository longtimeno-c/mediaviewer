# Additional permission for Wintun

MediaViewer is licensed under the GNU General Public License, version 3 or (at your option)
any later version (`LICENSE`). Under section 7 of that licence, the copyright holder grants
the following additional permission.

## The permission

If you modify MediaViewer, or any covered work, by linking or combining it with **Wintun**
(the prebuilt `wintun.dll` distributed by WireGuard LLC under its "Prebuilt Binaries
License"), containing parts covered by the terms of that licence, the licensors of MediaViewer
grant you additional permission to convey the resulting work. Corresponding Source for a
non-source form of such a combination need not include the source code for the parts of
Wintun used, and those parts need not be licensed under the GNU GPL.

## What it does not cover

- It is granted by longtimeno-c for longtimeno-c's own code only. It does not license anyone
  else's code, such as Exiv2 (GPL-2.0-or-later). So `wintun.dll` may be combined only with a
  program that holds no third-party GPL code (in MediaViewer, the Windows VPN service), never
  with `MediaViewer.exe`, which loads Exiv2 (`docs/design/11-licensing.md`).
- Wintun itself, which stays under WireGuard LLC's licence.

## Terms

This permission is granted by longtimeno-c, the copyright holder of MediaViewer, on 2026-11-19.
It applies to every version of MediaViewer that ships this file. The copyright holder may
withdraw it for later versions; as section 7 of the GPL allows, anyone conveying a copy may
also remove it from that copy. It adds to, and does not take from, the rights the GNU GPL
gives you.
