#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build LLVM's OpenMP runtime (libomp) for the app's deployment target.

LibRaw's OpenMP needs libomp on macOS and Apple's toolchain has none.
Homebrew's bottle declares the build machine's macOS as its minimum (26.0 on
a current Mac, 15.0 on the release runners), above the app's 14.0, and it
ships in Contents/Frameworks (issue #158). This builds the same runtime from
LLVM's release sources, hash-pinned, with -mmacosx-version-min taken from
cmake/darwin.cmake.

  build_libomp.py --prefix build/libomp/arm64 --arch arm64
  export MV_LIBOMP_ROOT=$PWD/build/libomp/arm64

The dynamic triplets (tools/mac/triplets/openmp-dynamic.cmake) and
cmake/raw-openmp.cmake read MV_LIBOMP_ROOT. The install name is the absolute
path under the prefix, as Homebrew's is; macpack.py rewrites it to @rpath
when it copies the dylib into the bundle.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LLVM_VERSION = "19.1.7"
URL = "https://github.com/llvm/llvm-project/releases/download/llvmorg-{v}/{name}-{v}.src.tar.xz"
# openmp/ needs LLVM's shared cmake/ modules beside it for a standalone build.
SOURCES = {
    "openmp": "bd7e6901ab086fd268750363017935fd4a717c153dad3c2aab86cb0140d9e3fe",
    "cmake": "11c5a28f90053b0c43d0dec3d0ad579347fc277199c005206b963c19aae514e3",
}


def deployment_target() -> str:
    text = (REPO_ROOT / "cmake" / "darwin.cmake").read_text()
    m = re.search(r'set\(CMAKE_OSX_DEPLOYMENT_TARGET "([0-9.]+)"\)', text)
    if not m:
        raise SystemExit("build_libomp: CMAKE_OSX_DEPLOYMENT_TARGET not found in cmake/darwin.cmake")
    return m.group(1)


def fetch(name: str, dest: Path) -> Path:
    archive = dest / f"{name}-{LLVM_VERSION}.src.tar.xz"
    subprocess.run(["curl", "-sSfL", "-o", str(archive), URL.format(v=LLVM_VERSION, name=name)],
                   check=True)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != SOURCES[name]:
        raise SystemExit(f"build_libomp: {archive.name} sha256 {digest}, expected {SOURCES[name]}")
    return archive


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--prefix", required=True, help="install prefix (lib/libomp.dylib, include/omp.h)")
    ap.add_argument("--arch", required=True, choices=["arm64", "x86_64"])
    ap.add_argument("--cmake", default="cmake")
    args = ap.parse_args()

    prefix = Path(args.prefix).resolve()
    target = deployment_target()
    stamp = prefix / "mv-libomp.txt"
    want = f"llvm {LLVM_VERSION} {args.arch} macos {target}\n"
    if stamp.is_file() and stamp.read_text() == want and (prefix / "lib" / "libomp.dylib").is_file():
        print(f"build_libomp: {prefix} is current ({want.strip()})")
        return

    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        for name in SOURCES:
            with tarfile.open(fetch(name, work)) as tar:
                tar.extractall(work)
        # LLVM_COMMON_CMAKE_UTILS defaults to ../cmake from openmp/.
        (work / f"cmake-{LLVM_VERSION}.src").rename(work / "cmake")
        if prefix.exists():
            shutil.rmtree(prefix)
        build = work / "build"
        subprocess.run([
            args.cmake, "-S", str(work / f"openmp-{LLVM_VERSION}.src"), "-B", str(build),
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_INSTALL_PREFIX={prefix}",
            f"-DCMAKE_INSTALL_NAME_DIR={prefix / 'lib'}",
            f"-DCMAKE_OSX_ARCHITECTURES={args.arch}",
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={target}",
            "-DLIBOMP_INSTALL_ALIASES=OFF",
            "-DOPENMP_ENABLE_LIBOMPTARGET=OFF",
            "-DOPENMP_ENABLE_OMPT_TOOLS=OFF",
            "-DOPENMP_ENABLE_TESTING=OFF",
        ], check=True)
        subprocess.run([args.cmake, "--build", str(build)], check=True)
        subprocess.run([args.cmake, "--install", str(build)], check=True)
    stamp.write_text(want)
    print(f"build_libomp: {prefix / 'lib' / 'libomp.dylib'} (macOS {target}, {args.arch})")


if __name__ == "__main__":
    main()
