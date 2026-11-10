#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fails the iOS build on any GPL or LGPL dependency (docs/plans/mobile.md 14).

It looks at what really went in, three ways:

  - every vcpkg port installed for the triplet: its declared SPDX licence must
    be made only of licences on PERMISSIVE, and its copyright text must not
    name the GPL family. A port that declares nothing fails too: unknown is
    not permissive. REVIEWED_PORTS is the way past this for one exact licence.
  - each other licence file named with --licence (ONNX Runtime's, from its
    archive).
  - the linked program, or every Mach-O file in an .app (--binary): nothing but
    system libraries may be loaded, and no symbol of a library we know to be
    (L)GPL may be in it. Give it the app before it is stripped: stripping
    removes the symbols, though not the rest of the check.

This covers third-party code. MediaViewer's own source is GPL-3.0-or-later; the
App Store build ships it under the additional permission the owner is to record
(docs/plans/mobile.md section 3, call 1), which is not something a build can check.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import sys

# Any spelling of the family: GPL-2.0, LGPLv3, AGPL, "GNU Lesser GPL", "GNU LGPL"
# and the licence's name in full, with or without "General".
GPL_TOKEN = re.compile(r"(?<![A-Za-z0-9])[AL]?GPL", re.IGNORECASE)
GPL_NAME = re.compile(
    r"\bGNU\b(?:\W+\w+){0,4}?\W+LICEN[CS]E"
    r"|\b(?:LESSER|LIBRARY|AFFERO)\s+(?:GENERAL\s+)?PUBLIC\s+LICEN[CS]E"
    r"|\bGENERAL\s+PUBLIC\s+LICEN[CS]E",
    re.IGNORECASE,
)
UNKNOWN = {"", "NOASSERTION", "NONE"}
# SPDX ids, lower case. Every id in an expression must be here, the alternatives
# of an OR included.
PERMISSIVE = {
    "0bsd", "apache-2.0", "blessing", "bsd-2-clause", "bsd-3-clause", "bsl-1.0", "cc0-1.0", "isc", "mit",
    "unlicense", "zlib",
}
PERMISSIVE_EXCEPTIONS = {"llvm-exception"}
# Ports cleared by hand, for one exact declared licence (a dual-licensed port
# taken under its permissive side, a copyright text that only mentions the
# GPL): port -> {"expression": ..., "reason": ...}. A changed licence fails again.
REVIEWED_PORTS: dict[str, dict[str, str]] = {
    "blake3": {
        "expression": "(CC0-1.0 OR Apache-2.0)",
        "reason": "Taken under CC0 (docs/design/18). The copyright text names GPLv2 only in the Apache "
        "LLVM exception's compatibility clause.",
    },
}

# Mach-O symbol prefixes of libraries under (L)GPL terms: FFmpeg, libheif,
# libde265, LibRaw, Exiv2, x264, x265, libsmb2, GnuTLS, Nettle, GMP, mpg123.
BANNED_SYMBOLS = re.compile(
    r"^_+(av_|avcodec_|avformat_|avfilter_|avio_|swscale_|sws_|swr_|heif_|de265_|libraw_|x264_|x265_|smb2_"
    r"|gnutls_|nettle_|gmp_|gmp[znqf]_|mpg123_)"
    r"|Exiv2"
)
SYSTEM_LIBRARY = re.compile(r"^(/System/|/usr/lib/)")
MACH_O_MAGIC = {b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf", b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe"}
# Fewer defined symbols than this: the symbol check saw too little to mean much.
FEW_SYMBOLS = 200


def names_gpl(text: str) -> bool:
    return bool(GPL_TOKEN.search(text) or GPL_NAME.search(text))


def not_permissive(expression: str) -> list[str]:
    """The ids of an SPDX expression that are not on the permissive lists."""
    words = re.findall(r"[A-Za-z0-9.+-]+", expression)
    out: list[str] = []
    after_with = False
    for word in words:
        lower = word.lower()
        if lower in {"and", "or"}:
            continue
        if lower == "with":
            after_with = True
            continue
        if lower not in (PERMISSIVE_EXCEPTIONS if after_with else PERMISSIVE):
            out.append(word)
        after_with = False
    return out


def port_licence(share: pathlib.Path) -> str:
    """The port's declared SPDX expression, "" when it has none."""
    spdx = share / "vcpkg.spdx.json"
    if not spdx.is_file():
        return ""
    data = json.loads(spdx.read_text(encoding="utf-8"))
    for package in data.get("packages", []):
        if package.get("name") == share.name:
            return package.get("licenseConcluded") or package.get("licenseDeclared") or ""
    return ""


def check_ports(installed: pathlib.Path) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    seen: list[str] = []
    share_root = installed / "share"
    if not share_root.is_dir():
        return [f"{installed}: no share folder, so no ports to check (wrong triplet folder?)"], seen
    for share in sorted(p for p in share_root.iterdir() if p.is_dir()):
        copyright_file = share / "copyright"
        spdx = share / "vcpkg.spdx.json"
        # A folder with neither is a CMake-config helper, not a port.
        if not copyright_file.is_file() and not spdx.is_file():
            continue
        licence = port_licence(share).strip()
        seen.append(f"{share.name}: {licence or 'none declared'}")
        reviewed = REVIEWED_PORTS.get(share.name)
        if reviewed and reviewed["expression"] == licence:
            continue
        if GPL_TOKEN.search(licence):
            errors.append(f"{share.name}: declares {licence}")
        elif copyright_file.is_file() and names_gpl(copyright_file.read_text(encoding="utf-8", errors="replace")):
            errors.append(f"{share.name}: its copyright text names the GPL family")
        elif licence.upper() in UNKNOWN:
            errors.append(f"{share.name}: declares no licence, so it cannot be cleared")
        elif not_permissive(licence):
            errors.append(f"{share.name}: {licence} is not on the permissive list, and the port is not in REVIEWED_PORTS")
    return errors, seen


def check_licence_file(name: str, path: pathlib.Path) -> list[str]:
    if not path.is_file():
        return [f"{name}: no licence file at {path}"]
    if names_gpl(path.read_text(encoding="utf-8", errors="replace")):
        return [f"{name}: its licence names the GPL family"]
    return []


def programs_in(path: pathlib.Path) -> list[pathlib.Path]:
    """`path` itself, or every Mach-O file inside a bundle."""
    if not path.is_dir():
        return [path]
    found = []
    for candidate in sorted(path.rglob("*")):
        if candidate.is_file() and not candidate.is_symlink():
            with candidate.open("rb") as handle:
                if handle.read(4) in MACH_O_MAGIC:
                    found.append(candidate)
    return found


def loaded_libraries(otool_output: str) -> list[str]:
    """The libraries in `otool -L` output. Only they are tab-indented: the
    file's own name heads the list, once per architecture in a fat binary."""
    return [
        line.strip().split(" (compatibility", 1)[0] for line in otool_output.splitlines() if line.startswith("\t")
    ]


def check_program(binary: pathlib.Path) -> list[str]:
    errors: list[str] = []
    loaded = subprocess.run(["xcrun", "otool", "-L", str(binary)], check=True, capture_output=True, text=True)
    for library in loaded_libraries(loaded.stdout):
        if not SYSTEM_LIBRARY.match(library):
            errors.append(f"{binary.name}: loads {library}, which is not a system library")
    # -U: defined symbols only, so a stray reference does not count as linked code.
    symbols = subprocess.run(["xcrun", "nm", "-U", "-j", str(binary)], check=True, capture_output=True, text=True)
    names = symbols.stdout.split()
    if len(names) < FEW_SYMBOLS:
        print(f"note: {binary.name} defines only {len(names)} symbols; if it is stripped, check it before that")
    banned = sorted({s for s in names if BANNED_SYMBOLS.search(s)})
    if banned:
        errors.append(f"{binary.name}: contains (L)GPL library symbols, e.g. {', '.join(banned[:5])}")
    return errors


def check_binary(path: pathlib.Path) -> list[str]:
    if not path.exists():
        return [f"{path}: no such program"]
    programs = programs_in(path)
    if not programs:
        return [f"{path}: no Mach-O file in it"]
    errors: list[str] = []
    for program in programs:
        errors += check_program(program)
    return errors


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--installed", required=True, type=pathlib.Path, help="vcpkg_installed/<triplet>")
    parser.add_argument("--licence", action="append", default=[], metavar="NAME=FILE")
    parser.add_argument("--binary", action="append", default=[], type=pathlib.Path)
    args = parser.parse_args(argv)

    errors, seen = check_ports(args.installed)
    for entry in args.licence:
        name, _, path = entry.partition("=")
        errors += check_licence_file(name, pathlib.Path(path))
    for binary in args.binary:
        errors += check_binary(binary)

    if errors:
        print("iOS licence check FAILED:", file=sys.stderr)
        for line in errors:
            print(f"  {line}", file=sys.stderr)
        return 1
    print(f"iOS licence check passed ({len(seen)} ports: {'; '.join(seen)})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
