#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the notices of the libraries linked into the iPhone app, for its Acknowledgements screen:
#   tools/ios/acknowledgements.py build-ios-iphoneos src.ios/App/MediaViewer/Acknowledgements.txt
# One "# Name" line per library, then its notice as paragraphs. Reads vcpkg's copyright files and ONNX Runtime's LICENSE from the iOS core build.
import glob
import os
import re
import sys


def paragraphs(body):
    """The notice's words unchanged, without a C comment frame or hard line breaks, so it reads at
    any text size: one paragraph per line, paragraphs apart by a blank line."""
    lines = []
    for line in body.splitlines():
        line = re.sub(r"^\s*(/\*+|\*+/|\*)\s?", "", line).rstrip()
        lines.append("" if line.strip() in ("", "*/") else line.strip())
    out, current = [], []
    for line in lines + [""]:
        if line:
            current.append(line)
        elif current:
            out.append(" ".join(current))
            current = []
    return "\n\n".join(out)


build, out = sys.argv[1], sys.argv[2]
share = os.path.join(build, "vcpkg_installed", "arm64-ios", "share")
parts = [
    ("libsodium", os.path.join(share, "libsodium", "copyright")),
    ("BLAKE3", os.path.join(share, "blake3", "copyright")),
    ("SQLite", os.path.join(share, "sqlite3", "copyright")),
]
ort = sorted(glob.glob(os.path.join(build, "_ort", "pod-archive-onnxruntime-*", "LICENSE")))
if not ort:
    sys.exit("no ONNX Runtime LICENSE under " + build)
parts.append(("ONNX Runtime", ort[-1]))

text = ["MediaViewer is built with these libraries. Their notices follow.", ""]
for name, path in parts:
    with open(path, encoding="utf-8") as f:
        body = f.read().strip()
    text += ["# " + name, "", paragraphs(body), ""]
with open(out, "w", encoding="utf-8") as f:
    f.write("\n".join(text).rstrip() + "\n")
print("wrote", out)
