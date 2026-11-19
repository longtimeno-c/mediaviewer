# Additional permission for Wintun

MediaViewer is licensed under the GNU General Public License, version 3 or (at your option)
any later version (`LICENSE`). Under section 7 of that licence, the copyright holder grants
the following additional permission.

## The permission

If you modify MediaViewer, or any covered work, by linking or combining it with **Wintun**
(the `wintun.dll` library distributed by WireGuard LLC under its "Prebuilt Binaries License",
or a modified version of it whose licence permits this), containing parts covered by the terms
of that licence, the licensors of MediaViewer grant you additional permission to convey the
resulting work. Corresponding Source for a non-source form of such a combination does not
include the source code for the parts of Wintun used, nor does it need to be released under
the GNU GPL.

## What it does not cover

- It is granted by longtimeno-c for longtimeno-c's own code only. It does not license the
  code of anyone else, such as Exiv2 or FFmpeg, so MediaViewer loads `wintun.dll` only in the
  Windows VPN service, a program holding no third-party GPL code, and never in
  `MediaViewer.exe` (`docs/design/11-licensing.md`).
- Wintun itself, which stays under WireGuard LLC's licence.

## Terms

This permission is granted by longtimeno-c, the copyright holder of MediaViewer, on 2026-11-19.
It applies to every version of MediaViewer that ships this file. It may be withdrawn for later
versions only by the copyright holder. It adds to, and does not take from, the rights the GNU
GPL gives you.
