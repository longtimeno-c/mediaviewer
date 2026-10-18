#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Slice 1's round-trip check (docs/design/29 "Round trip"): every project in
the set is written back unchanged byte for byte, and its new version
("<Project> (Claude 1)") describes the same timeline with every item's bytes
kept (mv-timeline roundtrip). With Final Cut Pro installed, each new version
is also validated against FCP's own DTD for its FCPXML version, so it is a
document FCP accepts for import.

    python3 tools/timeline/check_roundtrip.py --tool build/bin/mv-timeline [--set DIR] [--keep DIR]
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile
import urllib.parse

DTD_DIR = "/Applications/Final Cut Pro.app/Contents/Frameworks/Interchange.framework/Versions/A/Resources"


def main() -> int:
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tool", required=True, help="the mv-timeline executable")
    ap.add_argument("--set", default=os.path.join(here, "..", "..", "tests", "data", "fcpxml"))
    ap.add_argument("--keep", help="write the new versions here (for an import test in FCP)")
    args = ap.parse_args()
    files = sorted(glob.glob(os.path.join(args.set, "*.fcpxml")))
    if not files:
        print("no FCPXML in", args.set)
        return 2
    out = args.keep or tempfile.mkdtemp(prefix="mv-timeline-")
    os.makedirs(out, exist_ok=True)
    r = subprocess.run([args.tool, "roundtrip", *files, "--write", out])
    ok = r.returncode == 0
    have_dtd = os.path.isdir(DTD_DIR)
    if not have_dtd:
        print("(Final Cut Pro not installed: the DTD check is skipped)")
    for f in files:
        written = os.path.join(out, os.path.basename(f))
        if not have_dtd or not os.path.exists(written):
            continue
        head = open(written, encoding="utf-8").read(400)
        m = re.search(r'<fcpxml version="([0-9.]+)"', head)
        dtd = os.path.join(DTD_DIR, "FCPXMLv%s.dtd" % m.group(1).replace(".", "_")) if m else ""
        if not os.path.exists(dtd):
            print(f"FAIL {os.path.basename(f)}: no DTD for its version")
            ok = False
            continue
        v = subprocess.run(["xmllint", "--noout", "--dtdvalid", "file://" + urllib.parse.quote(dtd), written],
                           capture_output=True, text=True)
        print(("dtd  " if v.returncode == 0 else "FAIL ") + os.path.basename(f) + f" new version, FCPXML {m.group(1)}")
        if v.returncode != 0:
            print(v.stderr)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
