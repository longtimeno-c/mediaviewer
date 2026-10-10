#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail when a binary we ship needs a newer macOS than the app says it runs on.

Every Mach-O file in the app (and in any extra file or folder given, such as
an add-on's dylibs) must declare a minimum OS (LC_BUILD_VERSION minos, or the
older LC_VERSION_MIN_MACOSX) no newer than the app's LSMinimumSystemVersion.
A dylib built on the runner without a deployment target takes the runner's
macOS as its minimum; dyld on an older Mac may then refuse it (issue #158:
Homebrew's libomp declared macOS 26).

  check_minos.py build/MediaViewer.app [build/addons/import ...]

The parsing is pure; tools/mac/test_check_minos.py runs it without a Mac.
Only `otool` is needed for a real check.
"""
from __future__ import annotations

import argparse
import plistlib
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from macpack import is_macho  # noqa: E402

_MIN_FIELDS = {"LC_BUILD_VERSION": "minos", "LC_VERSION_MIN_MACOSX": "version"}


def parse_minos(otool_l: str) -> list[tuple[str, str]]:
    """(architecture, minimum OS) per slice, from `otool -arch all -l`.

    A thin file has no "(architecture ...)" header; its slice is reported as
    "". A slice with neither load command is reported with minimum "".
    """
    found: list[tuple[str, str]] = []
    arch: str | None = None
    arch_seen = False
    want: str | None = None
    for line in otool_l.splitlines():
        stripped = line.strip()
        if stripped.endswith(":") and "(architecture " in stripped:
            if arch is not None and not arch_seen:
                found.append((arch, ""))
            arch = stripped.rsplit("(architecture ", 1)[1].rstrip("):")
            arch_seen = False
            want = None
            continue
        if arch is None:
            arch = ""
        if stripped.startswith("Load command "):
            want = None
        elif stripped.startswith("cmd "):
            want = _MIN_FIELDS.get(stripped[4:].strip())
        elif want and stripped.startswith(want + " "):
            found.append((arch, stripped.split()[1]))
            arch_seen = True
            want = None
    if arch is not None and not arch_seen:
        found.append((arch, ""))
    return found


def version_key(version: str) -> tuple[int, ...]:
    parts = [int(p) for p in version.split(".")]
    while len(parts) > 1 and parts[-1] == 0:
        parts.pop()
    return tuple(parts)


def problems_for(name: str, slices: list[tuple[str, str]], minimum: str) -> list[str]:
    out: list[str] = []
    for arch, minos in slices:
        where = f"{name} ({arch})" if arch else name
        if not minos:
            out.append(f"{where}: no minimum macOS load command")
        elif version_key(minos) > version_key(minimum):
            out.append(f"{where}: minos {minos} > LSMinimumSystemVersion {minimum}")
    return out


def app_minimum(app: Path) -> str:
    with (app / "Contents" / "Info.plist").open("rb") as f:
        return str(plistlib.load(f)["LSMinimumSystemVersion"])


def macho_files(root: Path) -> list[Path]:
    if root.is_file():
        return [root] if is_macho(root) else []
    return sorted(p for p in root.rglob("*") if is_macho(p))


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("app", help="MediaViewer.app; its Info.plist gives the minimum")
    ap.add_argument("extra", nargs="*", help="more files or folders held to the same minimum")
    args = ap.parse_args(argv)

    app = Path(args.app)
    minimum = app_minimum(app)
    problems: list[str] = []
    checked = 0
    for root in [app] + [Path(e) for e in args.extra]:
        if not root.exists():
            problems.append(f"{root}: not found")
            continue
        for binary in macho_files(root):
            out = subprocess.run(["otool", "-arch", "all", "-l", str(binary)],
                                 check=True, text=True, capture_output=True).stdout
            problems += problems_for(str(binary), parse_minos(out), minimum)
            checked += 1
    if problems:
        print("check_minos: binaries need a newer macOS than the app supports:\n  "
              + "\n  ".join(problems), file=sys.stderr)
        return 1
    print(f"check_minos: {checked} Mach-O files run on macOS {minimum}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
