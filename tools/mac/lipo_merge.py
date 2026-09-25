#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Join an arm64 MediaViewer.app and an x86_64 one into one universal app.

Each architecture is built natively (its own vcpkg triplet, its own dylibs), so
the two trees have the same layout. This copies the arm64 tree, then for every
Mach-O file `lipo -create`s the x86_64 counterpart into it. Files that already
hold both architectures (Sparkle ships universal) are left alone. Code
signatures are invalid after the merge; `macpack.py release` re-signs inside
out, so run this before it.

  lipo_merge.py --arm64 build-arm64/MediaViewer.app --x86_64 build-x64/MediaViewer.app \\
                --out build/MediaViewer.app

The tree walk is pure so tools/mac/test_macpack.py can run it without a Mac; only
the three tool wrappers at the bottom need `lipo`.
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable

REQUIRED = frozenset({"arm64", "x86_64"})
MACHO_MAGIC = (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def is_macho(path: Path) -> bool:
    if not path.is_file() or path.is_symlink():
        return False
    with path.open("rb") as f:
        return f.read(4) in MACHO_MAGIC


def payload_files(root: Path) -> dict[str, Path]:
    """Regular files by bundle-relative path. Symlinks are structure (framework
    Versions/Current) and copy as-is; _CodeSignature is rebuilt by codesign."""
    out: dict[str, Path] = {}
    for p in sorted(root.rglob("*")):
        rel = p.relative_to(root)
        if "_CodeSignature" in rel.parts or p.is_symlink() or not p.is_file():
            continue
        out[rel.as_posix()] = p
    return out


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def merge_trees(
    arm: Path,
    x64: Path,
    out: Path,
    *,
    archs_of: Callable[[Path], frozenset[str]],
    lipo_create: Callable[[Path, Path, Path], None],
    macho: Callable[[Path], bool] = is_macho,
) -> list[str]:
    """Build `out` from `arm` and merge `x64` into it. Returns warnings.

    Raises SystemExit if the trees do not have the same files, or if a Mach-O
    pair does not end up with both architectures."""
    a, b = payload_files(arm), payload_files(x64)
    if set(a) != set(b):
        only_a, only_b = sorted(set(a) - set(b)), sorted(set(b) - set(a))
        raise SystemExit("lipo_merge: the two apps differ in layout; "
                         f"only in arm64: {only_a[:8]}, only in x86_64: {only_b[:8]}")

    if out.exists():
        shutil.rmtree(out)
    shutil.copytree(arm, out, symlinks=True)

    warnings: list[str] = []
    for rel, arm_file in a.items():
        target = out / rel
        x64_file = b[rel]
        if macho(arm_file) != macho(x64_file):
            raise SystemExit(f"lipo_merge: {rel} is Mach-O in one app only")
        if not macho(arm_file):
            if _sha256(arm_file) != _sha256(x64_file):
                warnings.append(f"{rel}: differs between builds; keeping the arm64 copy")
            continue
        have = archs_of(arm_file) | archs_of(x64_file)
        if REQUIRED <= archs_of(arm_file):
            continue  # already universal
        tmp = target.with_name(target.name + ".lipo")
        lipo_create(arm_file, x64_file, tmp)
        mode = target.stat().st_mode
        tmp.replace(target)
        target.chmod(mode)
        got = archs_of(target)
        if not REQUIRED <= got:
            raise SystemExit(f"lipo_merge: {rel} has {sorted(got)} after merge (inputs had {sorted(have)})")
    return warnings


# ---------------------------------------------------------------------------
# macOS tool wrappers
# ---------------------------------------------------------------------------

def lipo_archs(path: Path) -> frozenset[str]:
    out = subprocess.run(["lipo", "-archs", str(path)], check=True, text=True,
                         capture_output=True).stdout
    return frozenset(out.split())


def lipo_create(arm: Path, x64: Path, out: Path) -> None:
    subprocess.run(["lipo", "-create", str(arm), str(x64), "-output", str(out)], check=True)


def main(argv: list[str]) -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--arm64", required=True, type=Path, help="the arm64 MediaViewer.app")
    p.add_argument("--x86_64", required=True, type=Path, help="the x86_64 MediaViewer.app")
    p.add_argument("--out", required=True, type=Path, help="the universal MediaViewer.app to write")
    args = p.parse_args(argv)
    for label, app in (("arm64", args.arm64), ("x86_64", args.x86_64)):
        if not app.is_dir():
            raise SystemExit(f"lipo_merge: {label} app {app} not found")
    for w in merge_trees(args.arm64, args.x86_64, args.out,
                         archs_of=lipo_archs, lipo_create=lipo_create):
        print("lipo_merge: warning:", w, file=sys.stderr)
    print(f"lipo_merge: universal app at {args.out}")


if __name__ == "__main__":
    main(sys.argv[1:])
