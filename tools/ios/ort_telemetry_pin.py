#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pins what the iOS telemetry opt-out rests on (rule 6).

src/infer/ort_static_ios.cpp sets ORT_DISABLE_TELEMETRY so ONNX Runtime never
makes its uploader. That was read from telemetry.cc at the reviewed version and
no test can watch it happen. So the build fails when the library stops matching
what was reviewed: another version, the variable's name gone, or a collector
address that was not there. On a bump, re-read
onnxruntime/core/platform/posix/telemetry.cc and telemetry_environment.h at the
new tag, then update these three values.
"""
from __future__ import annotations

import argparse
import pathlib
import sys

REVIEWED_VERSION = "1.30.0"
OPT_OUT = b"ORT_DISABLE_TELEMETRY"
REVIEWED_ENDPOINTS = {
    b"https://mobile.events.data.microsoft.com/OneCollector/1.0",
    b"https://mobile.events.data.microsoft.com/OneCollector/1.0/",
}
HOST_SUFFIX = b".data.microsoft.com"


def printable(byte: int) -> bool:
    return 0x21 <= byte <= 0x7e


def endpoints(data: bytes) -> set[bytes]:
    """Every printable string in `data` holding the collector's host suffix."""
    found: set[bytes] = set()
    at = data.find(HOST_SUFFIX)
    while at != -1:
        start = at
        while start > 0 and printable(data[start - 1]):
            start -= 1
        end = at + len(HOST_SUFFIX)
        while end < len(data) and printable(data[end]):
            end += 1
        found.add(data[start:end])
        at = data.find(HOST_SUFFIX, end)
    return found


def problems(library: pathlib.Path, version: str) -> list[str]:
    out: list[str] = []
    if version != REVIEWED_VERSION:
        out.append(f"MV_ORT_VERSION is {version}, but telemetry was reviewed for {REVIEWED_VERSION}")
    data = library.read_bytes()
    if OPT_OUT not in data:
        out.append(f"{OPT_OUT.decode()} is not in the library: the opt-out may no longer exist")
    seen = endpoints(data)
    for extra in sorted(seen - REVIEWED_ENDPOINTS):
        out.append(f"unreviewed collector address: {extra.decode(errors='replace')}")
    for missing in sorted(REVIEWED_ENDPOINTS - seen):
        out.append(f"reviewed collector address gone: {missing.decode()}")
    return out


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--library", required=True, type=pathlib.Path)
    parser.add_argument("--version", required=True)
    args = parser.parse_args(argv)
    found = problems(args.library, args.version)
    if found:
        print("ONNX Runtime telemetry pin FAILED; re-review the opt-out (see this script's header):", file=sys.stderr)
        for line in found:
            print(f"  {line}", file=sys.stderr)
        return 1
    print(f"ONNX Runtime {args.version}: telemetry strings as reviewed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
