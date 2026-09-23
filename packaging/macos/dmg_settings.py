# SPDX-License-Identifier: GPL-2.0-or-later
# dmgbuild settings for the first-install disk image (PR 20, plan/13 "macOS
# first install"). Run by `tools/mac/macpack.py release`, which passes
# -D app=... -D license=... -D background=...
#
# What the image holds, and nothing more: the app, an Applications alias, a
# background with the name and the one-line description, and the GPL shown
# on mount (Agree / Disagree), the Mac counterpart of the Windows wizard's
# required licence page.
import os.path

app = defines["app"]  # noqa: F821 (dmgbuild injects `defines`)
app_name = os.path.basename(app)

format = "ULFO"  # LZFSE: macOS 10.11+, smaller than UDZO; the floor is 14
filesystem = "APFS"
files = [app]
symlinks = {"Applications": "/Applications"}
hide_extensions = [app_name]

background = defines["background"]  # noqa: F821
window_rect = ((200, 120), (660, 400))
icon_size = 128
text_size = 13
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
icon_locations = {
    app_name: (170, 210),
    "Applications": (490, 210),
}

# dmgbuild reads the file and embeds it as the image's software licence
# agreement (hdiutil udifrez), with its default English Agree / Disagree page.
license = {
    "licenses": {"en_US": defines["license"]},  # noqa: F821
}
