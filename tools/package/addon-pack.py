#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack and sign an add-on (plan/18 "Add-ons: how Import is installed").

    addon-pack.py pack --platform win-x64|macos-arm64 --src build/addons/import \
        --version 1.0.0 --key <ed25519-private-key-hex-file> --out dist/

writes, for the release to publish beside the app's own assets:

    mediaviewer-addon-import-<platform>.zip        the files, and nothing else
    mediaviewer-addon-import-<platform>.json       the manifest (schema 1)
    mediaviewer-addon-import-<platform>.json.sig   64-byte raw Ed25519, detached

The manifest lists every file with its SHA-256, size and licence, the host API
range the add-on supports, and the archive's own name, size and SHA-256. It
is signed with the update-manifest key (tools/package/update-signing.md), and
the app refuses anything else: a changed byte, a missing file, an extra file,
an unsigned or wrongly signed manifest, the wrong platform, or a host API
outside the range. The private key never touches the repository or a log.

On macOS the bundle and dylib must already be Developer ID-signed (same Team
ID as the app, for library validation) before packing; the archive is made
with `ditto -c -k` so the code signature survives, and notarization of the
archive follows the app's own procedure (RELEASING.md).
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

ADDON_ID = "import"
ADDON_NAME = "Import"
HOST_API = {"min": 1, "max": 1}  # MV_ADDON_HOST_API range this build supports

# What ships, per platform. Anything else in the build folder stays out.
FILES = {
    "win-x64": {
        "native": "mv_import.dll",
        "chrome": "MediaViewer.Import.Chrome.dll",
        "include": ["mv_import.dll", "MediaViewer.Import.Chrome.dll",
                    "MediaViewer.Import.Chrome.deps.json"],
    },
    "macos-arm64": {
        "native": "libmv_import.dylib",
        "chrome": "Import.bundle",
        "include": ["libmv_import.dylib", "Import.bundle"],
    },
}

LICENCE = "GPL-2.0-or-later"
NOTICE = ("Import add-on for MediaViewer. GPL-2.0-or-later. Uses SQLite (public domain).\n"
          "Content hashes use BLAKE3 (CC0-1.0) and signatures libsodium (ISC), both in the app.\n")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def load_private_key(path: Path):
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    raw = bytes.fromhex(path.read_text().strip())
    if len(raw) == 64:  # libsodium secret key: seed || public key
        raw = raw[:32]
    if len(raw) != 32:
        raise ValueError("expected a 32-byte Ed25519 seed (or 64-byte libsodium key), hex")
    return Ed25519PrivateKey.from_private_bytes(raw)


def collect(src: Path, platform: str, stage: Path) -> list:
    spec = FILES[platform]
    stage.mkdir(parents=True, exist_ok=True)
    for name in spec["include"]:
        item = src / name
        if not item.exists():
            raise FileNotFoundError(f"{name} is missing from {src}")
        if item.is_dir():
            shutil.copytree(item, stage / name, symlinks=True)
        else:
            shutil.copy2(item, stage / name)
    (stage / "LICENSES").mkdir(exist_ok=True)
    (stage / "LICENSES" / "NOTICE.txt").write_text(NOTICE)
    files = []
    for p in sorted(stage.rglob("*")):
        if p.is_symlink():
            raise ValueError(f"{p.relative_to(stage)} is a symlink; the app never follows links")
        if p.is_file():
            rel = p.relative_to(stage).as_posix()
            files.append({"path": rel, "sha256": sha256_file(p), "size": p.stat().st_size,
                          "licence": LICENCE})
    return files


def make_archive(stage: Path, archive: Path, platform: str):
    if archive.exists():
        archive.unlink()
    if platform == "macos-arm64" and shutil.which("ditto"):
        subprocess.run(["ditto", "-c", "-k", str(stage), str(archive)], check=True)
        return
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(stage.rglob("*")):
            if p.is_file():
                z.write(p, p.relative_to(stage).as_posix())


def build_manifest(platform: str, version: str, files: list, archive: Path) -> bytes:
    spec = FILES[platform]
    manifest = {
        "schema": 1,
        "id": ADDON_ID,
        "name": ADDON_NAME,
        "version": version,
        "platform": platform,
        "host_api": HOST_API,
        "installed_size": sum(f["size"] for f in files),
        "native": spec["native"],
        "chrome": spec["chrome"],
        "archive": {"path": archive.name, "sha256": sha256_file(archive),
                    "size": archive.stat().st_size},
        "files": files,
    }
    return (json.dumps(manifest, indent=1, sort_keys=False) + "\n").encode()


def pack(args) -> Path:
    import re
    if not re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)", args.version):
        raise ValueError("version must be x.y.z")
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    base = f"mediaviewer-addon-{ADDON_ID}-{args.platform}"
    stage = out / (base + ".stage")
    if stage.exists():
        shutil.rmtree(stage)
    files = collect(Path(args.src), args.platform, stage)
    archive = out / (base + ".zip")
    make_archive(stage, archive, args.platform)
    manifest = build_manifest(args.platform, args.version, files, archive)
    key = load_private_key(Path(args.key))
    (out / (base + ".json")).write_bytes(manifest)
    (out / (base + ".json.sig")).write_bytes(key.sign(manifest))
    shutil.rmtree(stage)
    print(f"packed {base}: {len(files)} files, {archive.stat().st_size} bytes")
    return out / (base + ".json")


def verify(args) -> int:
    """Signature and archive checks, the same rules the app applies first."""
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey
    from cryptography.exceptions import InvalidSignature
    manifest_path = Path(args.manifest)
    data = manifest_path.read_bytes()
    sig = Path(str(manifest_path) + ".sig").read_bytes()
    pub = Ed25519PublicKey.from_public_bytes(bytes.fromhex(args.public_key))
    try:
        pub.verify(sig, data)
    except InvalidSignature:
        print("bad_signature")
        return 1
    m = json.loads(data)
    archive = manifest_path.parent / m["archive"]["path"]
    if sha256_file(archive) != m["archive"]["sha256"] or archive.stat().st_size != m["archive"]["size"]:
        print("archive_mismatch")
        return 1
    print("ok")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pack")
    p.add_argument("--platform", choices=sorted(FILES), required=True)
    p.add_argument("--src", required=True)
    p.add_argument("--version", required=True)
    p.add_argument("--key", required=True, help="file holding the Ed25519 private key, hex")
    p.add_argument("--out", required=True)
    v = sub.add_parser("verify")
    v.add_argument("manifest")
    v.add_argument("--public-key", required=True, help="32-byte Ed25519 public key, hex")
    args = ap.parse_args(argv)
    if args.cmd == "pack":
        pack(args)
        return 0
    return verify(args)


if __name__ == "__main__":
    sys.exit(main())
